#include <Windows.h>

#include <array>
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
        std::fwprintf(stderr,
                      L"Install the current Microsoft Visual C++ Redistributable for x64 before continuing.\n");
        return k_missingRuntime;
    }
    if (a_argumentCount == 2 && (std::wstring_view(a_arguments[1]) == L"--install-probe" ||
                                 std::wstring_view(a_arguments[1]) == L"--diagnose"))
    {
        return 0;
    }
    if (a_argumentCount < 2)
    {
        std::fwprintf(stderr,
                      L"Usage: CueEngineBootstrap <install|update> --bundle-root <absolute> "
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
    if (installerHandle == INVALID_HANDLE_VALUE || !is_x64_installer(installerHandle))
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
