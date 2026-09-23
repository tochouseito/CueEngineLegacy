#include "WindowsDeveloperPrerequisites.h"

#include <Cue/Distribution/Error.h>
#include <Cue/Distribution/Manifest.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Platform/Process.h>
#include <Cue/Platform/Windows/WindowsProcess.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::size_t k_maximumToolOutputBytes = 16U * 1024U;
constexpr std::chrono::seconds k_toolTimeout{10};

struct ToolVersion final
{
    std::array<std::uint32_t, 4U> values{};

    [[nodiscard]] bool operator<(const ToolVersion &a_other) const noexcept
    {
        return values < a_other.values;
    }
};

/// @brief Distribution Prerequisite失敗を安定したErrorへ変換する
[[nodiscard]] cue::Error prerequisite_error(const cue::AssertContext &a_assertContext,
                                            std::string_view a_summary) noexcept
{
    return cue::distribution::make_distribution_error(
        a_assertContext, cue::distribution::DistributionError::BundleValidationFailed, a_summary);
}

/// @brief 2から4要素のVersion文字列を比較可能な値へ変換する
[[nodiscard]] std::optional<ToolVersion> parse_version(std::string_view a_text) noexcept
{
    ToolVersion version;
    std::size_t offset = 0U;
    std::size_t componentCount = 0U;
    while (offset < a_text.size() && componentCount < version.values.size())
    {
        const std::size_t separator = a_text.find('.', offset);
        const std::size_t end = separator == std::string_view::npos ? a_text.size() : separator;
        if (end == offset)
        {
            return std::nullopt;
        }
        std::uint32_t component = 0U;
        const auto [parsedEnd, error] =
            std::from_chars(a_text.data() + offset, a_text.data() + end, component);
        if (error != std::errc{} || parsedEnd != a_text.data() + end)
        {
            return std::nullopt;
        }
        version.values[componentCount++] = component;
        if (separator == std::string_view::npos)
        {
            offset = a_text.size();
            break;
        }
        offset = separator + 1U;
    }
    if (offset != a_text.size() || componentCount < 2U)
    {
        return std::nullopt;
    }
    return version;
}

/// @brief UTF-16 PathをChild Process境界のUTF-8へ変換する
[[nodiscard]] std::optional<std::string> to_utf8(std::wstring_view a_text) noexcept
{
    if (a_text.empty())
    {
        return std::string{};
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(),
                                             static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(), static_cast<int>(a_text.size()),
                            result.data(), required, nullptr, nullptr) != required)
    {
        return std::nullopt;
    }
    return result;
}

/// @brief Environment値を長さ上限へ依存せず取得する
[[nodiscard]] std::optional<std::wstring> environment_value(const wchar_t *a_name) noexcept
{
    const DWORD required = GetEnvironmentVariableW(a_name, nullptr, 0U);
    if (required == 0U)
    {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(a_name, value.data(), required);
    if (written == 0U || written >= required)
    {
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

/// @brief ReparseやDirectoryではない実Fileか判定する
[[nodiscard]] bool is_plain_file(const std::filesystem::path &a_path) noexcept
{
    const DWORD attributes = GetFileAttributesW(a_path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

/// @brief PATHのAbsolute Directoryだけから指定Executableを検索する
[[nodiscard]] std::optional<std::filesystem::path> executable_from_path(std::wstring_view a_name) noexcept
{
    const auto pathValue = environment_value(L"PATH");
    if (!pathValue)
    {
        return std::nullopt;
    }
    std::size_t offset = 0U;
    while (offset <= pathValue->size())
    {
        const std::size_t separator = pathValue->find(L';', offset);
        const std::size_t end = separator == std::wstring::npos ? pathValue->size() : separator;
        std::wstring directory = pathValue->substr(offset, end - offset);
        if (directory.size() >= 2U && directory.front() == L'"' && directory.back() == L'"')
        {
            directory = directory.substr(1U, directory.size() - 2U);
        }
        const std::filesystem::path root(directory);
        if (!directory.empty() && root.is_absolute())
        {
            const std::filesystem::path candidate = root / a_name;
            if (is_plain_file(candidate))
            {
                return candidate;
            }
        }
        if (separator == std::wstring::npos)
        {
            break;
        }
        offset = separator + 1U;
    }
    return std::nullopt;
}

/// @brief Registryの固定文字列値を取得する
[[nodiscard]] std::optional<std::wstring> registry_string(HKEY a_root, const wchar_t *a_subkey,
                                                          const wchar_t *a_value) noexcept
{
    DWORD bytes = 0U;
    const DWORD flags = RRF_RT_REG_SZ | RRF_ZEROONFAILURE;
    if (RegGetValueW(a_root, a_subkey, a_value, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t))
    {
        return std::nullopt;
    }
    std::wstring result(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(a_root, a_subkey, a_value, flags, nullptr, result.data(), &bytes) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    while (!result.empty() && result.back() == L'\0')
    {
        result.pop_back();
    }
    return result.empty() ? std::nullopt : std::optional<std::wstring>(std::move(result));
}

/// @brief PE Version ResourceからFile Versionを取得する
[[nodiscard]] std::optional<ToolVersion> file_version(const std::filesystem::path &a_path) noexcept
{
    DWORD ignored = 0U;
    const DWORD bytes = GetFileVersionInfoSizeW(a_path.c_str(), &ignored);
    if (bytes == 0U)
    {
        return std::nullopt;
    }
    std::vector<std::byte> storage(bytes);
    if (GetFileVersionInfoW(a_path.c_str(), 0U, bytes, storage.data()) == FALSE)
    {
        return std::nullopt;
    }
    VS_FIXEDFILEINFO *information = nullptr;
    UINT informationBytes = 0U;
    if (VerQueryValueW(storage.data(), L"\\", reinterpret_cast<void **>(&information), &informationBytes) == FALSE ||
        information == nullptr || informationBytes < sizeof(VS_FIXEDFILEINFO) ||
        information->dwSignature != VS_FFI_SIGNATURE)
    {
        return std::nullopt;
    }
    ToolVersion result;
    result.values = {HIWORD(information->dwFileVersionMS), LOWORD(information->dwFileVersionMS),
                     HIWORD(information->dwFileVersionLS), LOWORD(information->dwFileVersionLS)};
    return result;
}

/// @brief Child Process出力を一つの診断文字列へ連結する
[[nodiscard]] std::string process_output(const cue::ChildProcessResult &a_result)
{
    std::string output;
    for (const cue::ChildProcessOutputChunk &chunk : a_result.output())
    {
        output.append(chunk.bytes);
    }
    return output;
}

/// @brief Git Candidateを実行しGit for Windows Identityと最低Versionを検証する
[[nodiscard]] bool is_compatible_git_for_windows(cue::ChildProcessRunner &a_runner,
                                                 const std::filesystem::path &a_executable,
                                                 const ToolVersion &a_minimum) noexcept
{
    try
    {
        const auto executableUtf8 = to_utf8(a_executable.native());
        const auto workingDirectoryUtf8 = to_utf8(a_executable.parent_path().native());
        if (!executableUtf8 || !workingDirectoryUtf8)
        {
            return false;
        }
        std::vector<cue::ChildProcessEnvironmentEntry> environment;
        if (const auto systemRoot = environment_value(L"SystemRoot"))
        {
            const auto systemRootUtf8 = to_utf8(*systemRoot);
            if (systemRootUtf8)
            {
                environment.push_back({"SystemRoot", std::move(*systemRootUtf8)});
            }
        }
        cue::ChildProcessRequest request(*executableUtf8, {"--version"}, *workingDirectoryUtf8,
                                         std::move(environment), k_toolTimeout, k_maximumToolOutputBytes);
        cue::ChildProcessCancellation cancellation;
        auto result = a_runner.run(request, cancellation);
        if (!result || result.try_value()->outcome() != cue::ChildProcessOutcome::Exited ||
            !result.try_value()->exit_code() || *result.try_value()->exit_code() != 0U)
        {
            return false;
        }
        std::string output = process_output(*result.try_value());
        while (!output.empty() && (output.back() == '\r' || output.back() == '\n'))
        {
            output.pop_back();
        }
        constexpr std::string_view prefix = "git version ";
        constexpr std::string_view suffix = ".windows.";
        if (!output.starts_with(prefix))
        {
            return false;
        }
        const std::size_t suffixOffset = output.find(suffix, prefix.size());
        if (suffixOffset == std::string::npos || suffixOffset + suffix.size() >= output.size())
        {
            return false;
        }
        const auto version = parse_version(std::string_view(output).substr(prefix.size(), suffixOffset - prefix.size()));
        std::uint32_t windowsRevision = 0U;
        const std::string_view revision = std::string_view(output).substr(suffixOffset + suffix.size());
        const auto [end, error] =
            std::from_chars(revision.data(), revision.data() + revision.size(), windowsRevision);
        return version && !(*version < a_minimum) && error == std::errc{} && end == revision.data() + revision.size();
    }
    catch (...)
    {
        return false;
    }
}

/// @brief Visual Studio Installer所有のvswhereで指定Patternに一致するPathを列挙する
[[nodiscard]] cue::Result<std::vector<std::filesystem::path>> find_visual_studio_files(
    cue::ChildProcessRunner &a_runner, std::string a_pattern, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        const auto programFiles = environment_value(L"ProgramFiles(x86)");
        if (!programFiles)
        {
            return cue::Result<std::vector<std::filesystem::path>>::success({});
        }
        const std::filesystem::path executable =
            std::filesystem::path(*programFiles) / L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";
        if (!is_plain_file(executable))
        {
            return cue::Result<std::vector<std::filesystem::path>>::success({});
        }
        const auto executableUtf8 = to_utf8(executable.native());
        const auto workingDirectoryUtf8 = to_utf8(executable.parent_path().native());
        if (!executableUtf8 || !workingDirectoryUtf8)
        {
            return cue::Result<std::vector<std::filesystem::path>>::failure(
                prerequisite_error(a_assertContext, "Visual Studio discovery path could not be encoded"));
        }
        std::vector<cue::ChildProcessEnvironmentEntry> environment;
        if (const auto systemRoot = environment_value(L"SystemRoot"))
        {
            const auto systemRootUtf8 = to_utf8(*systemRoot);
            if (systemRootUtf8)
            {
                environment.push_back({"SystemRoot", std::move(*systemRootUtf8)});
            }
        }
        if (const auto programData = environment_value(L"ProgramData"))
        {
            const auto programDataUtf8 = to_utf8(*programData);
            if (programDataUtf8)
            {
                environment.push_back({"ProgramData", std::move(*programDataUtf8)});
            }
        }
        cue::ChildProcessRequest request(
            *executableUtf8,
            {"-all", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-find", std::move(a_pattern)},
            *workingDirectoryUtf8, std::move(environment), k_toolTimeout, k_maximumToolOutputBytes);
        cue::ChildProcessCancellation cancellation;
        auto result = a_runner.run(request, cancellation);
        if (!result)
        {
            return cue::Result<std::vector<std::filesystem::path>>::failure(std::move(*result.try_error()));
        }
        if (result.try_value()->outcome() != cue::ChildProcessOutcome::Exited ||
            !result.try_value()->exit_code() || *result.try_value()->exit_code() != 0U)
        {
            return cue::Result<std::vector<std::filesystem::path>>::success({});
        }
        const std::string output = process_output(*result.try_value());
        std::vector<std::filesystem::path> paths;
        std::size_t offset = 0U;
        while (offset < output.size())
        {
            const std::size_t end = output.find_first_of("\r\n", offset);
            const std::string_view line(output.data() + offset,
                                        (end == std::string::npos ? output.size() : end) - offset);
            if (!line.empty())
            {
                const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(),
                                                         static_cast<int>(line.size()), nullptr, 0);
                if (required > 0)
                {
                    std::wstring wide(static_cast<std::size_t>(required), L'\0');
                    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(),
                                            static_cast<int>(line.size()), wide.data(), required) == required)
                    {
                        std::filesystem::path path(std::move(wide));
                        if (path.is_absolute() && is_plain_file(path))
                        {
                            paths.push_back(std::move(path));
                        }
                    }
                }
            }
            if (end == std::string::npos)
            {
                break;
            }
            offset = output.find_first_not_of("\r\n", end);
            if (offset == std::string::npos)
            {
                break;
            }
        }
        return cue::Result<std::vector<std::filesystem::path>>::success(std::move(paths));
    }
    catch (...)
    {
        return cue::Result<std::vector<std::filesystem::path>>::failure(
            prerequisite_error(a_assertContext, "Visual Studio discovery allocation failed"));
    }
}

/// @brief Candidate列から最低Version以上のFileを選択する
[[nodiscard]] std::optional<std::filesystem::path> select_compatible_file(
    const std::vector<std::filesystem::path> &a_candidates, const ToolVersion &a_minimum) noexcept
{
    std::optional<std::filesystem::path> selected;
    std::optional<ToolVersion> selectedVersion;
    for (const std::filesystem::path &candidate : a_candidates)
    {
        const auto version = file_version(candidate);
        if (!version || *version < a_minimum || (selectedVersion && *version < *selectedVersion))
        {
            continue;
        }
        selected = candidate;
        selectedVersion = version;
    }
    return selected;
}

/// @brief Windows SDKのHeaderとx64 Libraryから最低Version以上のSDKを検出する
[[nodiscard]] bool has_compatible_windows_sdk(const ToolVersion &a_minimum) noexcept
{
    const auto kitsRoot = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots",
                                          L"KitsRoot10");
    if (!kitsRoot)
    {
        return false;
    }
    try
    {
        const std::filesystem::path includeRoot = std::filesystem::path(*kitsRoot) / L"Include";
        std::error_code error;
        for (std::filesystem::directory_iterator iterator(includeRoot, error), end; !error && iterator != end;
             iterator.increment(error))
        {
            if (!iterator->is_directory(error) || error)
            {
                continue;
            }
            const auto nameUtf8 = to_utf8(iterator->path().filename().native());
            const auto version = nameUtf8 ? parse_version(*nameUtf8) : std::nullopt;
            if (version && !(*version < a_minimum) && is_plain_file(iterator->path() / L"um" / L"Windows.h") &&
                is_plain_file(iterator->path() / L"ucrt" / L"corecrt.h") &&
                is_plain_file(std::filesystem::path(*kitsRoot) / L"Lib" / iterator->path().filename() / L"um" /
                              L"x64" / L"kernel32.lib") &&
                is_plain_file(std::filesystem::path(*kitsRoot) / L"Lib" / iterator->path().filename() / L"ucrt" /
                              L"x64" / L"ucrt.lib"))
            {
                return true;
            }
        }
    }
    catch (...)
    {
        return false;
    }
    return false;
}
} // namespace

cue::Result<void> cue::distribution::validate_windows_developer_prerequisites(
    const MinimumToolchain &a_minimumToolchain, const AssertContext &a_assertContext) noexcept
{
    try
    {
        const auto cmakeMinimum = parse_version(a_minimumToolchain.cmakeVersion);
        const auto gitMinimum = parse_version(a_minimumToolchain.gitVersion);
        const auto compilerMinimum = parse_version(a_minimumToolchain.compilerVersion);
        const auto sdkMinimum = parse_version(a_minimumToolchain.windowsSdkVersion);
        if (!cmakeMinimum || !gitMinimum || !compilerMinimum || !sdkMinimum ||
            a_minimumToolchain.compilerVendor != "msvc")
        {
            return Result<void>::failure(
                prerequisite_error(a_assertContext, "Developer prerequisite contract is invalid"));
        }

        std::vector<std::filesystem::path> cmakeCandidates;
        if (const auto cmake = executable_from_path(L"cmake.exe"))
        {
            cmakeCandidates.push_back(*cmake);
        }

        auto runner = create_windows_child_process_runner(a_assertContext);
        if (!runner)
        {
            return Result<void>::failure(std::move(*runner.try_error()));
        }
        auto visualStudioCMake = find_visual_studio_files(
            **runner.try_value(), "Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe",
            a_assertContext);
        if (!visualStudioCMake)
        {
            return Result<void>::failure(std::move(*visualStudioCMake.try_error()));
        }
        cmakeCandidates.insert(cmakeCandidates.end(), visualStudioCMake.try_value()->begin(),
                               visualStudioCMake.try_value()->end());
        if (!select_compatible_file(cmakeCandidates, *cmakeMinimum))
        {
            return Result<void>::failure(prerequisite_error(
                a_assertContext, "CMake prerequisite is missing or older than the Bundle minimum version"));
        }

        std::vector<std::filesystem::path> gitCandidates;
        if (const auto gitRoot =
                registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GitForWindows", L"InstallPath"))
        {
            const std::filesystem::path git = std::filesystem::path(*gitRoot) / L"cmd" / L"git.exe";
            if (is_plain_file(git))
            {
                gitCandidates.push_back(git);
            }
        }
        if (const auto git = executable_from_path(L"git.exe"))
        {
            gitCandidates.push_back(*git);
        }
        const bool hasCompatibleGit = std::ranges::any_of(
            gitCandidates, [&runner, &gitMinimum](const std::filesystem::path &a_candidate) noexcept
            { return is_compatible_git_for_windows(**runner.try_value(), a_candidate, *gitMinimum); });
        if (!hasCompatibleGit)
        {
            return Result<void>::failure(prerequisite_error(
                a_assertContext, "Git for Windows prerequisite is missing or older than 2.44.0"));
        }

        auto compilerCandidates = find_visual_studio_files(
            **runner.try_value(), "VC\\Tools\\MSVC\\**\\bin\\Hostx64\\x64\\cl.exe", a_assertContext);
        if (!compilerCandidates)
        {
            return Result<void>::failure(std::move(*compilerCandidates.try_error()));
        }
        if (!select_compatible_file(*compilerCandidates.try_value(), *compilerMinimum))
        {
            return Result<void>::failure(prerequisite_error(
                a_assertContext, "MSVC x64 prerequisite is missing or older than the Bundle minimum version"));
        }

        if (!has_compatible_windows_sdk(*sdkMinimum))
        {
            return Result<void>::failure(prerequisite_error(
                a_assertContext, "Windows SDK prerequisite is missing or older than the Bundle minimum version"));
        }
        return Result<void>::success();
    }
    catch (...)
    {
        return Result<void>::failure(
            prerequisite_error(a_assertContext, "Developer prerequisite diagnosis allocation failed"));
    }
}
