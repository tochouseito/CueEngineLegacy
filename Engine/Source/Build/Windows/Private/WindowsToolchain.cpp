#include <Cue/Build/Windows/WindowsToolchain.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Windows/UtfConversion.h>

#include <EngineBuildMetadata.h>

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
/// @brief Windows Toolchain検出中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_discovery_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Windows build environment discovery failed unexpectedly");
    std::abort();
}

/// @brief UTF-8 Build Metadata PathをWin32 API用UTF-16へ変換する
[[nodiscard]] std::optional<std::wstring> to_windows_path(std::string_view a_path,
                                                          const cue::AssertContext &a_assertContext) noexcept
{
    std::wstring converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_utf8_to_windows_utf16(a_path, converted, a_assertContext.fatal_handler());
    if (result.status != cue::WindowsUtfConversionStatus::Success || converted.empty())
    {
        return std::nullopt;
    }
    return converted;
}

/// @brief UTF-16 Native PathをPublic Report用UTF-8へ変換する
[[nodiscard]] std::optional<std::string> to_utf8_path(std::wstring_view a_path,
                                                      const cue::AssertContext &a_assertContext) noexcept
{
    std::string converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_windows_utf16_to_utf8(a_path, converted, a_assertContext.fatal_handler());
    if (result.status != cue::WindowsUtfConversionStatus::Success || converted.empty())
    {
        return std::nullopt;
    }
    return converted;
}

/// @brief Absolute Windows Pathを長いPath対応のWin32 API形式へ正規化する
[[nodiscard]] std::wstring to_extended_windows_path(std::wstring a_path)
{
    std::replace(a_path.begin(), a_path.end(), L'/', L'\\');
    a_path = std::filesystem::path(a_path).lexically_normal().native();
    if (a_path.starts_with(L"\\\\?\\"))
    {
        return a_path;
    }
    if (a_path.starts_with(L"\\\\"))
    {
        return L"\\\\?\\UNC\\" + a_path.substr(2U);
    }
    if (a_path.size() >= 3U && a_path[1U] == L':' && a_path[2U] == L'\\')
    {
        return L"\\\\?\\" + a_path;
    }
    return a_path;
}

/// @brief Native Windows ArchitectureをPortable列挙値へ変換する
[[nodiscard]] cue::BuildArchitecture native_host_architecture() noexcept
{
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    switch (systemInfo.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return cue::BuildArchitecture::X64;
    case PROCESSOR_ARCHITECTURE_INTEL:
        return cue::BuildArchitecture::X86;
    case PROCESSOR_ARCHITECTURE_ARM64:
        return cue::BuildArchitecture::Arm64;
    default:
        return cue::BuildArchitecture::Unknown;
    }
}

/// @brief File Version ResourceをTool実行なしで4要素Versionへ変換する
[[nodiscard]] std::optional<cue::BuildToolVersion> read_file_version(const std::wstring &a_path,
                                                                     const cue::AssertContext &a_assertContext) noexcept
{
    DWORD ignored = 0U;
    const DWORD size = GetFileVersionInfoSizeW(a_path.c_str(), &ignored);
    if (size == 0U)
    {
        return std::nullopt;
    }
    try
    {
        std::vector<std::byte> data(size);
        if (GetFileVersionInfoW(a_path.c_str(), 0U, size, data.data()) == FALSE)
        {
            return std::nullopt;
        }
        VS_FIXEDFILEINFO *info = nullptr;
        UINT infoSize = 0U;
        if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &infoSize) == FALSE ||
            info == nullptr || infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE)
        {
            return std::nullopt;
        }
        return cue::BuildToolVersion{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                                     HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
    }
    catch (...)
    {
        terminate_discovery_exception(a_assertContext);
    }
}

/// @brief ExecutableのPE Machineを実行せずPortable Architectureへ変換する
[[nodiscard]] cue::BuildArchitecture read_binary_architecture(const std::wstring &a_path)
{
    std::ifstream stream(std::filesystem::path(a_path), std::ios::binary);
    IMAGE_DOS_HEADER dosHeader{};
    stream.read(reinterpret_cast<char *>(&dosHeader), sizeof(dosHeader));
    if (!stream || dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew < 0)
    {
        return cue::BuildArchitecture::Unknown;
    }
    stream.seekg(dosHeader.e_lfanew, std::ios::beg);
    DWORD signature = 0U;
    IMAGE_FILE_HEADER fileHeader{};
    stream.read(reinterpret_cast<char *>(&signature), sizeof(signature));
    stream.read(reinterpret_cast<char *>(&fileHeader), sizeof(fileHeader));
    if (!stream || signature != IMAGE_NT_SIGNATURE)
    {
        return cue::BuildArchitecture::Unknown;
    }
    switch (fileHeader.Machine)
    {
    case IMAGE_FILE_MACHINE_AMD64:
        return cue::BuildArchitecture::X64;
    case IMAGE_FILE_MACHINE_I386:
        return cue::BuildArchitecture::X86;
    case IMAGE_FILE_MACHINE_ARM64:
        return cue::BuildArchitecture::Arm64;
    default:
        return cue::BuildArchitecture::Unknown;
    }
}

/// @brief Engine Build Metadataで固定したExecutableを一候補として検査する
[[nodiscard]] cue::BuildToolCandidate probe_executable(cue::BuildToolKind a_kind, std::string_view a_path,
                                                       std::string_view a_installationRoot,
                                                       const cue::AssertContext &a_assertContext)
{
    cue::BuildToolCandidate candidate;
    candidate.kind = a_kind;
    candidate.nativePath = a_path;
    candidate.installationRoot = a_installationRoot;
    const auto path = to_windows_path(a_path, a_assertContext);
    if (!path)
    {
        return candidate;
    }
    const std::wstring extendedPath = to_extended_windows_path(*path);
    const DWORD attributes = GetFileAttributesW(extendedPath.c_str());
    candidate.available = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
                          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
    if (candidate.available)
    {
        candidate.version = read_file_version(extendedPath, a_assertContext);
        candidate.architecture = read_binary_architecture(extendedPath);
    }
    return candidate;
}

/// @brief Dot区切りVersionを最大4要素の整数へ厳密変換する
[[nodiscard]] std::optional<cue::BuildToolVersion> parse_version(std::string_view a_text) noexcept
{
    cue::BuildToolVersion version;
    std::uint32_t *parts[] = {&version.major, &version.minor, &version.patch, &version.build};
    std::size_t partIndex = 0U;
    std::uint64_t value = 0U;
    bool hasDigit = false;
    for (std::size_t index = 0U; index <= a_text.size(); ++index)
    {
        if (index < a_text.size() && a_text[index] >= '0' && a_text[index] <= '9')
        {
            hasDigit = true;
            value = value * 10U + static_cast<std::uint64_t>(a_text[index] - '0');
            if (value > UINT32_MAX)
            {
                return std::nullopt;
            }
            continue;
        }
        if (!hasDigit || partIndex >= 4U || (index < a_text.size() && a_text[index] != '.'))
        {
            return std::nullopt;
        }
        *parts[partIndex++] = static_cast<std::uint32_t>(value);
        value = 0U;
        hasDigit = false;
    }
    return partIndex >= 2U ? std::optional<cue::BuildToolVersion>(version) : std::nullopt;
}

/// @brief Engineが記録したWindows SDK VersionのHeaderとx64 Libraryを検査する
[[nodiscard]] cue::BuildToolCandidate probe_windows_sdk(const cue::AssertContext &a_assertContext)
{
    cue::BuildToolCandidate candidate;
    candidate.kind = cue::BuildToolKind::WindowsSdk;
    const auto root = to_windows_path(cue::build_metadata::k_windowsSdkRoot, a_assertContext);
    const auto version = parse_version(cue::build_metadata::k_windowsSdkVersion);
    if (!root || !version)
    {
        return candidate;
    }
    const auto windowsVersion = to_windows_path(cue::build_metadata::k_windowsSdkVersion, a_assertContext);
    if (!windowsVersion)
    {
        return candidate;
    }
    const std::filesystem::path sdkRoot(*root);
    const std::filesystem::path include =
        to_extended_windows_path((sdkRoot / L"Include" / *windowsVersion / L"um" / L"Windows.h").native());
    const std::filesystem::path library =
        to_extended_windows_path((sdkRoot / L"Lib" / *windowsVersion / L"um" / L"x64" / L"kernel32.lib").native());
    std::error_code error;
    candidate.available = std::filesystem::is_regular_file(include, error) && !error;
    error.clear();
    candidate.available = candidate.available && std::filesystem::is_regular_file(library, error) && !error;
    candidate.version = version;
    candidate.architecture = candidate.available ? cue::BuildArchitecture::X64 : cue::BuildArchitecture::Unknown;
    if (auto utf8 = to_utf8_path((sdkRoot / L"Include" / *windowsVersion).native(), a_assertContext))
    {
        candidate.nativePath = std::move(*utf8);
    }
    if (auto utf8 = to_utf8_path(sdkRoot.native(), a_assertContext))
    {
        candidate.installationRoot = std::move(*utf8);
    }
    return candidate;
}

/// @brief Path配下のMarkerがRegular Fileとして存在するか判定する
[[nodiscard]] bool has_marker(std::string_view a_root, std::wstring_view a_marker,
                              const cue::AssertContext &a_assertContext)
{
    const auto root = to_windows_path(a_root, a_assertContext);
    if (!root)
    {
        return false;
    }
    std::error_code error;
    const std::filesystem::path marker = std::filesystem::path(*root) / a_marker;
    return std::filesystem::is_regular_file(to_extended_windows_path(marker.native()), error) && !error;
}
} // namespace

namespace cue
{
BuildEnvironmentRequirements current_windows_build_requirements(const AssertContext &a_assertContext) noexcept
{
    try
    {
        BuildEnvironmentRequirements requirements;
        requirements.hostArchitecture = BuildArchitecture::X64;
        requirements.supportedConfigurations = {BuildConfiguration::Debug, BuildConfiguration::Development,
                                                BuildConfiguration::Release};
        requirements.tools.reserve(5U);
        const auto cmakeMinimum = parse_version(build_metadata::k_cmakeMinimumVersion);
        if (!cmakeMinimum || cmakeMinimum->major == UINT32_MAX)
        {
            terminate_discovery_exception(a_assertContext);
        }
        BuildToolVersion cmakeMaximum = *cmakeMinimum;
        ++cmakeMaximum.major;
        cmakeMaximum.minor = 0U;
        cmakeMaximum.patch = 0U;
        cmakeMaximum.build = 0U;
        requirements.tools.push_back({BuildToolKind::CMake, *cmakeMinimum, cmakeMaximum, BuildArchitecture::X64});
        requirements.tools.push_back({BuildToolKind::VisualStudio,
                                      {build_metadata::k_visualStudioMajor, 0U, 0U, 0U},
                                      {build_metadata::k_visualStudioMajor + 1U, 0U, 0U, 0U},
                                      BuildArchitecture::X64});
        constexpr std::uint32_t compilerMajor = _MSC_VER / 100U;
        constexpr std::uint32_t compilerMinor = _MSC_VER % 100U;
        requirements.tools.push_back({BuildToolKind::MsvcCompiler,
                                      {compilerMajor, compilerMinor, 0U, 0U},
                                      {compilerMajor, compilerMinor + 1U, 0U, 0U},
                                      BuildArchitecture::X64});
        const auto sdkVersion = parse_version(build_metadata::k_windowsSdkVersion);
        if (!sdkVersion || sdkVersion->build == UINT32_MAX)
        {
            terminate_discovery_exception(a_assertContext);
        }
        BuildToolVersion sdkMaximum = *sdkVersion;
        ++sdkMaximum.build;
        requirements.tools.push_back({BuildToolKind::WindowsSdk, *sdkVersion, sdkMaximum, BuildArchitecture::X64});
        requirements.tools.push_back(
            {BuildToolKind::Git, {2U, 44U, 0U, 0U}, {3U, 0U, 0U, 0U}, BuildArchitecture::X64});
        return requirements;
    }
    catch (...)
    {
        terminate_discovery_exception(a_assertContext);
    }
}

BuildEnvironmentInventory discover_current_windows_build_environment(const AssertContext &a_assertContext) noexcept
{
    try
    {
        BuildEnvironmentInventory inventory;
        inventory.hostArchitecture = native_host_architecture();
        inventory.engineSourceRoot = build_metadata::k_engineSourceRoot;
        inventory.engineBinaryRoot = build_metadata::k_engineBinaryRoot;
        inventory.engineSourceAvailable =
            has_marker(inventory.engineSourceRoot, L"Engine\\Source\\GameModule\\CMakeLists.txt", a_assertContext);
        inventory.engineBinaryAvailable = has_marker(inventory.engineBinaryRoot, L"CMakeCache.txt", a_assertContext);
        inventory.candidates.reserve(5U);

        const auto cmakeWindowsPath = to_windows_path(build_metadata::k_cmakeCommand, a_assertContext);
        std::optional<std::string> cmakeRoot;
        if (cmakeWindowsPath)
        {
            const std::filesystem::path cmakePath(*cmakeWindowsPath);
            cmakeRoot = to_utf8_path(cmakePath.parent_path().parent_path().native(), a_assertContext);
        }
        inventory.candidates.push_back(probe_executable(BuildToolKind::CMake, build_metadata::k_cmakeCommand,
                                                        cmakeRoot ? *cmakeRoot : std::string_view{}, a_assertContext));

        inventory.candidates.push_back(probe_executable(BuildToolKind::VisualStudio,
                                                        build_metadata::k_visualStudioMsbuild,
                                                        build_metadata::k_visualStudioRoot, a_assertContext));

        const auto compilerWindowsPath = to_windows_path(build_metadata::k_msvcCompiler, a_assertContext);
        std::optional<std::string> compilerRootUtf8;
        if (compilerWindowsPath)
        {
            const std::filesystem::path compilerPath(*compilerWindowsPath);
            const std::filesystem::path compilerRoot =
                compilerPath.parent_path().parent_path().parent_path().parent_path();
            compilerRootUtf8 = to_utf8_path(compilerRoot.native(), a_assertContext);
        }
        inventory.candidates.push_back(probe_executable(BuildToolKind::MsvcCompiler, build_metadata::k_msvcCompiler,
                                                        compilerRootUtf8 ? *compilerRootUtf8 : std::string_view{},
                                                        a_assertContext));
        inventory.candidates.push_back(probe_windows_sdk(a_assertContext));
        const auto gitWindowsPath = to_windows_path(build_metadata::k_gitCommand, a_assertContext);
        std::optional<std::string> gitRoot;
        if (gitWindowsPath)
        {
            const std::filesystem::path gitPath(*gitWindowsPath);
            gitRoot = to_utf8_path(gitPath.parent_path().parent_path().native(), a_assertContext);
        }
        inventory.candidates.push_back(probe_executable(BuildToolKind::Git, build_metadata::k_gitCommand,
                                                        gitRoot ? *gitRoot : std::string_view{}, a_assertContext));
        return inventory;
    }
    catch (...)
    {
        terminate_discovery_exception(a_assertContext);
    }
}

BuildEnvironmentReport validate_current_windows_build_environment(const AssertContext &a_assertContext) noexcept
{
    BuildEnvironmentInventory inventory = discover_current_windows_build_environment(a_assertContext);
    BuildEnvironmentRequirements requirements = current_windows_build_requirements(a_assertContext);
    return validate_build_environment(inventory, requirements, a_assertContext);
}
} // namespace cue
