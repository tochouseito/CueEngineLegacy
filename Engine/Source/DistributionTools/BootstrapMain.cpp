#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr int k_usageFailure = 2;
constexpr int k_unsupportedHost = 20;
constexpr int k_missingRuntime = 21;
constexpr int k_invalidBootstrapPath = 22;
constexpr int k_invalidInstaller = 23;
constexpr int k_installerLaunchFailure = 24;
constexpr std::uint64_t k_maximumManifestBytes = 16U * 1024U * 1024U;
constexpr std::string_view k_manifestPrefix = "{\"schemaVersion\":1,\"distributionKind\":\"DeveloperSourceSdk\",";
constexpr std::string_view k_installerEntryPrefix =
    "{\"role\":\"installer\",\"path\":\"Bin/CueEngineInstallerTool.exe\",\"sizeBytes\":";

struct InstallerInventoryEntry final
{
    std::uint64_t byteSize = 0U;
    std::array<unsigned char, 32U> sha256{};
};

/// @brief Absolute PathをExtended-length Win32 Pathへ変換する
[[nodiscard]] std::wstring extended_path(const std::filesystem::path &a_path)
{
    std::filesystem::path preferred = a_path;
    preferred.make_preferred();
    const std::wstring path = preferred.native();
    if (path.starts_with(L"\\\\?\\"))
    {
        return path;
    }
    if (path.starts_with(L"\\\\"))
    {
        return L"\\\\?\\UNC\\" + path.substr(2U);
    }
    return L"\\\\?\\" + path;
}

/// @brief 現在ProcessのExecutable Pathを長さ上限へ依存せず取得する
[[nodiscard]] std::optional<std::filesystem::path> executable_path()
{
    std::vector<wchar_t> buffer(512U);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U)
        {
            return std::nullopt;
        }
        if (length < buffer.size() - 1U)
        {
            return std::filesystem::path(std::wstring_view(buffer.data(), length));
        }
        if (buffer.size() >= 32768U)
        {
            return std::nullopt;
        }
        buffer.resize(buffer.size() * 2U);
    }
}

/// @brief Native HostがM18対象のWindows x64か判定する
[[nodiscard]] bool is_windows_x64() noexcept
{
    SYSTEM_INFO information{};
    GetNativeSystemInfo(&information);
    return information.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
}

/// @brief 通常Toolが必要とするSystem32のVC++ Runtimeを現在ArchitectureでLoad検証する
[[nodiscard]] bool has_visual_cpp_runtime() noexcept
{
    constexpr std::array<std::wstring_view, 3U> libraries = {L"vcruntime140.dll", L"vcruntime140_1.dll",
                                                             L"msvcp140.dll"};
    for (const std::wstring_view library : libraries)
    {
        const std::wstring name(library);
        HMODULE module = LoadLibraryExW(name.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr)
        {
            std::fwprintf(stderr, L"CueEngine prerequisite missing: %ls (Win32 error %lu).\n", name.c_str(),
                          GetLastError());
            return false;
        }
        static_cast<void>(FreeLibrary(module));
    }
    return true;
}

/// @brief Open済みHandleがDirectoryやReparse Pointではない通常Fileか判定する
[[nodiscard]] bool is_plain_file(HANDLE a_file) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO information{};
    return GetFileInformationByHandleEx(a_file, FileAttributeTagInfo, &information, sizeof(information)) != FALSE &&
           (information.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U;
}

/// @brief 小容量Manifestを固定Handleから上限付きで読み取る
[[nodiscard]] std::optional<std::string> read_manifest(const std::filesystem::path &a_path) noexcept
{
    try
    {
        const std::wstring nativePath = extended_path(a_path);
        HANDLE file = CreateFileW(nativePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE || !is_plain_file(file))
        {
            if (file != INVALID_HANDLE_VALUE)
            {
                static_cast<void>(CloseHandle(file));
            }
            return std::nullopt;
        }
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0 ||
            static_cast<std::uint64_t>(size.QuadPart) > k_maximumManifestBytes)
        {
            static_cast<void>(CloseHandle(file));
            return std::nullopt;
        }
        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t offset = 0U;
        while (offset < bytes.size())
        {
            DWORD transferred = 0U;
            const DWORD requested = static_cast<DWORD>(bytes.size() - offset);
            if (ReadFile(file, bytes.data() + offset, requested, &transferred, nullptr) == FALSE || transferred == 0U)
            {
                static_cast<void>(CloseHandle(file));
                return std::nullopt;
            }
            offset += transferred;
        }
        static_cast<void>(CloseHandle(file));
        return bytes;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

/// @brief Lowercase SHA-256文字列を固定長Byte列へ変換する
[[nodiscard]] bool parse_sha256(std::string_view a_text, std::array<unsigned char, 32U> &a_output) noexcept
{
    if (a_text.size() != a_output.size() * 2U)
    {
        return false;
    }
    /// @brief Lowercase Hex一文字を値へ変換する
    const auto nibble = [](char a_value) noexcept -> std::optional<unsigned char>
    {
        if (a_value >= '0' && a_value <= '9')
        {
            return static_cast<unsigned char>(a_value - '0');
        }
        if (a_value >= 'a' && a_value <= 'f')
        {
            return static_cast<unsigned char>(a_value - 'a' + 10);
        }
        return std::nullopt;
    };
    for (std::size_t index = 0U; index < a_output.size(); ++index)
    {
        const auto high = nibble(a_text[index * 2U]);
        const auto low = nibble(a_text[index * 2U + 1U]);
        if (!high || !low)
        {
            return false;
        }
        a_output[index] = static_cast<unsigned char>((*high << 4U) | *low);
    }
    return true;
}

/// @brief Canonical Bundle Manifestから唯一のInstaller Inventory Entryを抽出する
[[nodiscard]] std::optional<InstallerInventoryEntry> parse_installer_entry(std::string_view a_manifest) noexcept
{
    if (!a_manifest.starts_with(k_manifestPrefix) || !a_manifest.ends_with("]}\n"))
    {
        return std::nullopt;
    }
    const std::size_t files = a_manifest.find("\"files\":[");
    const std::size_t entry =
        files == std::string_view::npos
            ? std::string_view::npos
            : a_manifest.find(k_installerEntryPrefix, files + std::string_view("\"files\":[").size());
    if (entry == std::string_view::npos ||
        a_manifest.find(k_installerEntryPrefix, entry + k_installerEntryPrefix.size()) != std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::size_t sizeBegin = entry + k_installerEntryPrefix.size();
    const std::size_t sizeEnd = a_manifest.find(",\"sha256\":\"", sizeBegin);
    if (sizeEnd == std::string_view::npos)
    {
        return std::nullopt;
    }
    InstallerInventoryEntry result;
    const auto parsed = std::from_chars(a_manifest.data() + sizeBegin, a_manifest.data() + sizeEnd, result.byteSize);
    if (parsed.ec != std::errc{} || parsed.ptr != a_manifest.data() + sizeEnd || result.byteSize == 0U)
    {
        return std::nullopt;
    }
    constexpr std::string_view hashPrefix = ",\"sha256\":\"";
    const std::size_t hashBegin = sizeEnd + hashPrefix.size();
    const std::size_t hashEnd = hashBegin + result.sha256.size() * 2U;
    if (hashEnd + 2U > a_manifest.size() || a_manifest.substr(hashEnd, 2U) != "\"}" ||
        !parse_sha256(a_manifest.substr(hashBegin, result.sha256.size() * 2U), result.sha256))
    {
        return std::nullopt;
    }
    return result;
}

/// @brief KERNEL32以外を静的Importせず固定HandleのSHA-256を計算する
[[nodiscard]] std::optional<std::array<unsigned char, 32U>> hash_file(HANDLE a_file) noexcept
{
    HMODULE library = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library == nullptr)
    {
        return std::nullopt;
    }
    const auto openAlgorithm = reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(
        GetProcAddress(library, "BCryptOpenAlgorithmProvider"));
    const auto getProperty =
        reinterpret_cast<decltype(&BCryptGetProperty)>(GetProcAddress(library, "BCryptGetProperty"));
    const auto createHash = reinterpret_cast<decltype(&BCryptCreateHash)>(GetProcAddress(library, "BCryptCreateHash"));
    const auto updateHash = reinterpret_cast<decltype(&BCryptHashData)>(GetProcAddress(library, "BCryptHashData"));
    const auto finishHash = reinterpret_cast<decltype(&BCryptFinishHash)>(GetProcAddress(library, "BCryptFinishHash"));
    const auto destroyHash =
        reinterpret_cast<decltype(&BCryptDestroyHash)>(GetProcAddress(library, "BCryptDestroyHash"));
    const auto closeAlgorithm = reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(
        GetProcAddress(library, "BCryptCloseAlgorithmProvider"));
    if (openAlgorithm == nullptr || getProperty == nullptr || createHash == nullptr || updateHash == nullptr ||
        finishHash == nullptr || destroyHash == nullptr || closeAlgorithm == nullptr)
    {
        static_cast<void>(FreeLibrary(library));
        return std::nullopt;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::optional<std::array<unsigned char, 32U>> result;
    do
    {
        if (!BCRYPT_SUCCESS(openAlgorithm(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0U)))
        {
            break;
        }
        DWORD objectBytes = 0U;
        DWORD hashBytes = 0U;
        DWORD transferred = 0U;
        if (!BCRYPT_SUCCESS(getProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                                        sizeof(objectBytes), &transferred, 0U)) ||
            transferred != sizeof(objectBytes) || objectBytes == 0U ||
            !BCRYPT_SUCCESS(getProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashBytes),
                                        sizeof(hashBytes), &transferred, 0U)) ||
            transferred != sizeof(hashBytes) || hashBytes != 32U)
        {
            break;
        }
        std::vector<unsigned char> object(objectBytes);
        if (!BCRYPT_SUCCESS(createHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0U, 0U)))
        {
            break;
        }
        LARGE_INTEGER origin{};
        if (SetFilePointerEx(a_file, origin, nullptr, FILE_BEGIN) == FALSE)
        {
            break;
        }
        std::array<unsigned char, 64U * 1024U> buffer{};
        for (;;)
        {
            transferred = 0U;
            if (ReadFile(a_file, buffer.data(), static_cast<DWORD>(buffer.size()), &transferred, nullptr) == FALSE)
            {
                break;
            }
            if (transferred == 0U)
            {
                std::array<unsigned char, 32U> digest{};
                if (BCRYPT_SUCCESS(finishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0U)))
                {
                    result = digest;
                }
                break;
            }
            if (!BCRYPT_SUCCESS(updateHash(hash, buffer.data(), transferred, 0U)))
            {
                break;
            }
        }
    } while (false);
    if (hash != nullptr)
    {
        static_cast<void>(destroyHash(hash));
    }
    if (algorithm != nullptr)
    {
        static_cast<void>(closeAlgorithm(algorithm, 0U));
    }
    static_cast<void>(FreeLibrary(library));
    return result;
}

/// @brief InstallerのSizeとSHA-256をBundle ManifestのRoleおよび固定Pathへ照合する
[[nodiscard]] bool matches_installer_inventory(const std::filesystem::path &a_bundleRoot, HANDLE a_installer) noexcept
{
    const auto manifest = read_manifest(a_bundleRoot / L"CueEngineDistribution.json");
    const auto entry = manifest ? parse_installer_entry(*manifest) : std::nullopt;
    LARGE_INTEGER size{};
    const auto digest = entry ? hash_file(a_installer) : std::nullopt;
    return entry && GetFileSizeEx(a_installer, &size) != FALSE && size.QuadPart >= 0 &&
           static_cast<std::uint64_t>(size.QuadPart) == entry->byteSize && digest && *digest == entry->sha256;
}

/// @brief Installer PE Headerを固定Handleから読込みx64 Imageだけを許可する
[[nodiscard]] bool is_x64_installer(HANDLE a_file) noexcept
{
    LARGE_INTEGER origin{};
    if (SetFilePointerEx(a_file, origin, nullptr, FILE_BEGIN) == FALSE)
    {
        return false;
    }
    IMAGE_DOS_HEADER dosHeader{};
    DWORD transferred = 0U;
    if (ReadFile(a_file, &dosHeader, sizeof(dosHeader), &transferred, nullptr) == FALSE ||
        transferred != sizeof(dosHeader) || dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew < 0)
    {
        return false;
    }
    LARGE_INTEGER peOffset{};
    peOffset.QuadPart = dosHeader.e_lfanew;
    if (SetFilePointerEx(a_file, peOffset, nullptr, FILE_BEGIN) == FALSE)
    {
        return false;
    }
    DWORD signature = 0U;
    IMAGE_FILE_HEADER header{};
    if (ReadFile(a_file, &signature, sizeof(signature), &transferred, nullptr) == FALSE ||
        transferred != sizeof(signature) || signature != IMAGE_NT_SIGNATURE ||
        ReadFile(a_file, &header, sizeof(header), &transferred, nullptr) == FALSE || transferred != sizeof(header))
    {
        return false;
    }
    WORD optionalHeaderMagic = 0U;
    if (header.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        ReadFile(a_file, &optionalHeaderMagic, sizeof(optionalHeaderMagic), &transferred, nullptr) == FALSE ||
        transferred != sizeof(optionalHeaderMagic))
    {
        return false;
    }
    return header.Machine == IMAGE_FILE_MACHINE_AMD64 && optionalHeaderMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
}

/// @brief Windows Command Lineの一ArgumentをCreateProcess規則でQuoteする
[[nodiscard]] std::wstring quote_argument(std::wstring_view a_argument)
{
    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t slashCount = 0U;
    for (const wchar_t character : a_argument)
    {
        if (character == L'\\')
        {
            ++slashCount;
            continue;
        }
        if (character == L'"')
        {
            quoted.append(slashCount * 2U + 1U, L'\\');
            quoted.push_back(L'"');
            slashCount = 0U;
            continue;
        }
        quoted.append(slashCount, L'\\');
        slashCount = 0U;
        quoted.push_back(character);
    }
    quoted.append(slashCount * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

/// @brief 検証済みSibling Installerを引数境界を保って起動し終了Codeを返す
[[nodiscard]] int launch_installer(const std::filesystem::path &a_installer, int a_argumentCount,
                                   wchar_t **a_arguments) noexcept
{
    try
    {
        const std::wstring nativeInstaller = extended_path(a_installer);
        std::wstring commandLine = quote_argument(nativeInstaller);
        for (int index = 1; index < a_argumentCount; ++index)
        {
            commandLine.push_back(L' ');
            commandLine.append(quote_argument(a_arguments[index]));
        }
        commandLine.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const std::wstring workingDirectory = extended_path(a_installer.parent_path());
        if (CreateProcessW(nativeInstaller.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0U, nullptr,
                           workingDirectory.c_str(), &startup, &process) == FALSE)
        {
            std::fwprintf(stderr, L"CueEngine Installer could not be started (Win32 error %lu).\n", GetLastError());
            return k_installerLaunchFailure;
        }
        static_cast<void>(CloseHandle(process.hThread));
        if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0)
        {
            static_cast<void>(CloseHandle(process.hProcess));
            return k_installerLaunchFailure;
        }
        DWORD exitCode = 0U;
        const BOOL readExitCode = GetExitCodeProcess(process.hProcess, &exitCode);
        static_cast<void>(CloseHandle(process.hProcess));
        return readExitCode != FALSE ? static_cast<int>(exitCode) : k_installerLaunchFailure;
    }
    catch (...)
    {
        return k_installerLaunchFailure;
    }
}
} // namespace

/// @brief Windows x64とVC++ Runtimeを自己完結診断し、検証済みSibling Installerへ処理を渡す
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    if (!is_windows_x64())
    {
        std::fwprintf(stderr, L"CueEngine Developer Distribution requires Windows x64.\n");
        return k_unsupportedHost;
    }
    if (!has_visual_cpp_runtime())
    {
        std::fwprintf(stderr, L"Install the current Microsoft Visual C++ Redistributable for x64 before continuing.\n");
        return k_missingRuntime;
    }
    if (a_argumentCount == 2 &&
        (std::wstring_view(a_arguments[1]) == L"--install-probe" || std::wstring_view(a_arguments[1]) == L"--diagnose"))
    {
        return 0;
    }
    if (a_argumentCount < 2)
    {
        std::fwprintf(stderr, L"Usage: CueEngineBootstrap <install|update> --bundle-root <absolute> "
                              L"--install-root <absolute> --allow-unsigned-local\n");
        return k_usageFailure;
    }

    const auto bootstrap = executable_path();
    if (!bootstrap)
    {
        return k_invalidBootstrapPath;
    }
    const std::filesystem::path installer = bootstrap->parent_path() / L"CueEngineInstallerTool.exe";
    const std::wstring nativeInstaller = extended_path(installer);
    const DWORD attributes = GetFileAttributesW(nativeInstaller.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        std::fwprintf(stderr, L"CueEngine Installer inventory entry is missing or unsupported.\n");
        return k_invalidInstaller;
    }
    HANDLE installerHandle = CreateFileW(nativeInstaller.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (installerHandle == INVALID_HANDLE_VALUE || !is_plain_file(installerHandle) ||
        !matches_installer_inventory(bootstrap->parent_path().parent_path(), installerHandle) ||
        !is_x64_installer(installerHandle))
    {
        if (installerHandle != INVALID_HANDLE_VALUE)
        {
            static_cast<void>(CloseHandle(installerHandle));
        }
        std::fwprintf(stderr, L"CueEngine Installer inventory entry is not a valid Windows x64 image.\n");
        return k_invalidInstaller;
    }
    const int result = launch_installer(installer, a_argumentCount, a_arguments);
    static_cast<void>(CloseHandle(installerHandle));
    return result;
}
