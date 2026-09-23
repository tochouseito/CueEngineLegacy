#include <Cue/Build/Windows/WindowsToolchain.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <memory>
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
} // namespace

/// @brief Engine Build Metadataから現在のWindows Toolchainを実行前に検証できるか確認する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    const cue::BuildEnvironmentInventory inventory = cue::discover_current_windows_build_environment(assertContext);
    const cue::BuildEnvironmentReport report = cue::validate_current_windows_build_environment(assertContext);
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
        report.diagnostics.empty() && nativeArchitectureMatches)
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
