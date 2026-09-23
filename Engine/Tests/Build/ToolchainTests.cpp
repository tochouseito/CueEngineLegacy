#include <Cue/Build/Toolchain.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <array>
#include <cstdlib>
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

/// @brief 5種Toolを要求するx64 Engine契約を返す
[[nodiscard]] cue::BuildEnvironmentRequirements make_requirements()
{
    cue::BuildEnvironmentRequirements requirements;
    requirements.hostArchitecture = cue::BuildArchitecture::X64;
    requirements.supportedConfigurations = {cue::BuildConfiguration::Debug, cue::BuildConfiguration::Development,
                                            cue::BuildConfiguration::Release};
    requirements.tools = {
        {cue::BuildToolKind::CMake, {4U, 2U, 0U, 0U}, {5U, 0U, 0U, 0U}, cue::BuildArchitecture::X64},
        {cue::BuildToolKind::VisualStudio, {18U, 0U, 0U, 0U}, {19U, 0U, 0U, 0U}, cue::BuildArchitecture::X64},
        {cue::BuildToolKind::MsvcCompiler, {19U, 51U, 0U, 0U}, {19U, 52U, 0U, 0U}, cue::BuildArchitecture::X64},
        {cue::BuildToolKind::WindowsSdk, {10U, 0U, 26100U, 0U}, {10U, 0U, 26100U, 1U}, cue::BuildArchitecture::X64},
        {cue::BuildToolKind::Git, {2U, 44U, 0U, 0U}, {3U, 0U, 0U, 0U}, cue::BuildArchitecture::X64}};
    return requirements;
}

/// @brief 検証済み候補を5種持つInventoryを返す
[[nodiscard]] cue::BuildEnvironmentInventory make_inventory()
{
    cue::BuildEnvironmentInventory inventory;
    inventory.hostArchitecture = cue::BuildArchitecture::X64;
    inventory.engineSourceRoot = "C:/CueEngine";
    inventory.engineBinaryRoot = "C:/CueEngine/out/build";
    inventory.engineSourceAvailable = true;
    inventory.engineBinaryAvailable = true;
    inventory.candidates = {{cue::BuildToolKind::CMake, "C:/Tools/CMake/cmake.exe", "C:/Tools/CMake",
                             cue::BuildToolVersion{4U, 4U, 2U, 0U}, cue::BuildArchitecture::X64, true},
                            {cue::BuildToolKind::VisualStudio, "C:/VS/MSBuild.exe", "C:/VS",
                             cue::BuildToolVersion{18U, 9U, 1U, 0U}, cue::BuildArchitecture::X64, true},
                            {cue::BuildToolKind::MsvcCompiler, "C:/VS/cl.exe", "C:/VS/VC/Tools/MSVC/14.51",
                             cue::BuildToolVersion{19U, 51U, 36256U, 0U}, cue::BuildArchitecture::X64, true},
                            {cue::BuildToolKind::WindowsSdk, "C:/Kits/10/Include/10.0.26100.0", "C:/Kits/10",
                             cue::BuildToolVersion{10U, 0U, 26100U, 0U}, cue::BuildArchitecture::X64, true},
                            {cue::BuildToolKind::Git, "C:/Program Files/Git/cmd/git.exe", "C:/Program Files/Git",
                             cue::BuildToolVersion{2U, 47U, 1U, 2U}, cue::BuildArchitecture::X64, true}};
    return inventory;
}

/// @brief 診断CodeがReportに含まれるか判定する
[[nodiscard]] bool has_diagnostic(const cue::BuildEnvironmentReport &a_report,
                                  cue::BuildEnvironmentDiagnosticCode a_code) noexcept
{
    for (const auto &diagnostic : a_report.diagnostics)
    {
        if (diagnostic.code == a_code)
        {
            return true;
        }
    }
    return false;
}

/// @brief 既知の互換候補が全5種選択されるか検証する
[[nodiscard]] bool test_supported(const cue::AssertContext &a_assertContext)
{
    const auto report = cue::validate_build_environment(make_inventory(), make_requirements(), a_assertContext);
    return report.support == cue::BuildEnvironmentSupport::Supported && report.selectedTools.size() == 5U &&
           report.supportedConfigurations.size() == 3U && report.diagnostics.empty();
}

/// @brief 欠損候補をUnsupportedではなくUnknownとして診断するか検証する
[[nodiscard]] bool test_missing(const cue::AssertContext &a_assertContext)
{
    auto inventory = make_inventory();
    inventory.candidates.erase(inventory.candidates.begin());
    const auto report = cue::validate_build_environment(inventory, make_requirements(), a_assertContext);
    return report.support == cue::BuildEnvironmentSupport::Unknown &&
           has_diagnostic(report, cue::BuildEnvironmentDiagnosticCode::MissingTool);
}

/// @brief Installed Source SDK契約が事前Build TreeなしでToolchainを利用可能と判定するか検証する
[[nodiscard]] bool test_optional_engine_binary(const cue::AssertContext &a_assertContext)
{
    auto inventory = make_inventory();
    inventory.engineBinaryRoot.clear();
    inventory.engineBinaryAvailable = false;
    auto requirements = make_requirements();
    requirements.requiresEngineBinary = false;
    const auto report = cue::validate_build_environment(inventory, requirements, a_assertContext);
    return report.support == cue::BuildEnvironmentSupport::Supported &&
           !has_diagnostic(report, cue::BuildEnvironmentDiagnosticCode::MissingEngineBinary);
}

/// @brief 既知だが範囲外のVersionをUnsupportedとして診断するか検証する
[[nodiscard]] bool test_unsupported(const cue::AssertContext &a_assertContext)
{
    auto inventory = make_inventory();
    inventory.candidates.front().version = cue::BuildToolVersion{3U, 31U, 0U, 0U};
    const auto report = cue::validate_build_environment(inventory, make_requirements(), a_assertContext);
    return report.support == cue::BuildEnvironmentSupport::Unsupported &&
           has_diagnostic(report, cue::BuildEnvironmentDiagnosticCode::UnsupportedTool);
}

/// @brief Git for Windows 2.44未満をBuild／Restore前の非対応Toolとして診断するか検証する
[[nodiscard]] bool test_unsupported_git(const cue::AssertContext &a_assertContext)
{
    auto inventory = make_inventory();
    inventory.candidates.back().version = cue::BuildToolVersion{2U, 43U, 9U, 0U};
    const auto report = cue::validate_build_environment(inventory, make_requirements(), a_assertContext);
    return report.support == cue::BuildEnvironmentSupport::Unsupported &&
           has_diagnostic(report, cue::BuildEnvironmentDiagnosticCode::UnsupportedTool) &&
           report.selectedTools.size() == 4U;
}

/// @brief 既知の非x64 ArchitectureをUnknownではなくUnsupportedとして診断するか検証する
[[nodiscard]] bool test_unsupported_architecture(const cue::AssertContext &a_assertContext)
{
    auto inventory = make_inventory();
    inventory.candidates.front().architecture = cue::BuildArchitecture::X86;
    const auto report = cue::validate_build_environment(inventory, make_requirements(), a_assertContext);
    return report.support == cue::BuildEnvironmentSupport::Unsupported &&
           has_diagnostic(report, cue::BuildEnvironmentDiagnosticCode::UnsupportedTool) &&
           !has_diagnostic(report, cue::BuildEnvironmentDiagnosticCode::UnknownToolIdentity);
}

/// @brief 複数の互換候補を暗黙選択せずUnknownとして診断するか検証する
[[nodiscard]] bool test_ambiguous(const cue::AssertContext &a_assertContext)
{
    auto inventory = make_inventory();
    inventory.candidates.push_back({cue::BuildToolKind::CMake, "D:/Tools/CMake/cmake.exe", "D:/Tools/CMake",
                                    cue::BuildToolVersion{4U, 3U, 0U, 0U}, cue::BuildArchitecture::X64, true});
    const auto report = cue::validate_build_environment(inventory, make_requirements(), a_assertContext);
    return report.support == cue::BuildEnvironmentSupport::Unknown &&
           has_diagnostic(report, cue::BuildEnvironmentDiagnosticCode::AmbiguousTool);
}

/// @brief Native Pathの改行、Quote、制御文字がLog Field内へEscapeされるか検証する
[[nodiscard]] bool test_safe_path_format(const cue::AssertContext &a_assertContext)
{
    const std::string formatted =
        cue::format_native_path_for_log("C:\\Tool\n\"bad\"\x01\xC2\x85\xE2\x80\xA8\xE2\x80\xAE"
                                        "\xF0\x93\x90\xB0\xF0\x9B\xB2\xA0\xF0\x9D\x85\xB3",
                                        a_assertContext);
    return formatted ==
           "\"C:\\\\Tool\\n\\\"bad\\\"\\x01\\u{0085}\\u{2028}\\u{202E}\\u{013430}\\u{01BCA0}\\u{01D173}\"";
}

/// @brief Command Lineの各Argumentが境界を保ってLog用にEscapeされるか検証する
[[nodiscard]] bool test_safe_command_line_format(const cue::AssertContext &a_assertContext)
{
    constexpr std::array<std::string_view, 3U> arguments = {"C:\\Program Files\\CMake\\cmake.exe", "-S",
                                                            "C:\\Project\nInjected"};
    const std::string formatted = cue::format_command_line_for_log(arguments, a_assertContext);
    return formatted == "\"C:\\\\Program Files\\\\CMake\\\\cmake.exe\" \"-S\" \"C:\\\\Project\\nInjected\"";
}
} // namespace

/// @brief Toolchain選択、欠損、非対応、複数候補、Log安全化を終了Codeで検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_supported(assertContext) && test_missing(assertContext) && test_optional_engine_binary(assertContext) &&
                   test_unsupported(assertContext) &&
                   test_unsupported_git(assertContext) &&
                   test_unsupported_architecture(assertContext) && test_ambiguous(assertContext) &&
                   test_safe_path_format(assertContext) && test_safe_command_line_format(assertContext)
               ? 0
               : 1;
}
