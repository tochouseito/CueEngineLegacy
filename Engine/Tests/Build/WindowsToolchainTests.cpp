#include <Cue/Build/Windows/WindowsToolchain.h>

#include "WindowsToolchainInternal.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <Windows.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の回復不能失敗を終了Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief Message付き回復不能失敗を終了Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

/// @brief Test ExecutableのNative PathをProcess API用UTF-8へ変換する
[[nodiscard]] std::optional<std::string> to_utf8(std::wstring_view a_text)
{
    if (a_text.empty())
    {
        return std::nullopt;
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(),
                                             static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        return std::nullopt;
    }
    std::string converted(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(), static_cast<int>(a_text.size()),
                            converted.data(), required, nullptr, nullptr) != required)
    {
        return std::nullopt;
    }
    return converted;
}
} // namespace

/// @brief Engine Build Metadataから現在のWindows Toolchainを実行前に検証できるか確認する
int main(int a_argumentCount, char **a_arguments)
{
    if (a_argumentCount == 2 && std::string_view(a_arguments[1]) == "--version")
    {
        std::cout << "git version 2.50.0\n";
        return 0;
    }
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    const cue::BuildEnvironmentInventory inventory = cue::discover_current_windows_build_environment(assertContext);
    const cue::BuildEnvironmentReport report = cue::validate_current_windows_build_environment(assertContext);
    std::array<wchar_t, 32768U> modulePath{};
    const DWORD modulePathSize = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    const auto invalidGitPath = to_utf8(std::wstring_view(modulePath.data(), modulePathSize));
    const auto invalidGitRoot = to_utf8(std::filesystem::path(modulePath.data()).parent_path().native());
    cue::BuildToolCandidate invalidGit;
    if (invalidGitPath && invalidGitRoot)
    {
        invalidGit = cue::detail::probe_git_for_windows(*invalidGitPath, *invalidGitRoot, assertContext);
    }
    SYSTEM_INFO nativeSystem{};
    GetNativeSystemInfo(&nativeSystem);
    const bool nativeArchitectureMatches = (nativeSystem.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 &&
                                            inventory.hostArchitecture == cue::BuildArchitecture::X64) ||
                                           (nativeSystem.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL &&
                                            inventory.hostArchitecture == cue::BuildArchitecture::X86) ||
                                           (nativeSystem.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 &&
                                            inventory.hostArchitecture == cue::BuildArchitecture::Arm64) ||
                                           (nativeSystem.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64 &&
                                            nativeSystem.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_INTEL &&
                                            nativeSystem.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_ARM64 &&
                                            inventory.hostArchitecture == cue::BuildArchitecture::Unknown);
    if (inventory.candidates.size() == 5U && report.support == cue::BuildEnvironmentSupport::Supported &&
        report.selectedTools.size() == 5U && report.supportedConfigurations.size() == 3U &&
        report.diagnostics.empty() && nativeArchitectureMatches && modulePathSize > 0U &&
        modulePathSize < modulePath.size() && invalidGitPath && invalidGitRoot && !invalidGit.available &&
        !invalidGit.version)
    {
        return 0;
    }
    for (const auto &diagnostic : report.diagnostics)
    {
        std::cerr << static_cast<int>(diagnostic.code) << ": " << diagnostic.summary
                  << " path=" << cue::format_native_path_for_log(diagnostic.nativePath, assertContext) << '\n';
    }
    return 1;
}
