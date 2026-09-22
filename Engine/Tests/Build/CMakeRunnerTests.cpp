#include <Cue/Build/CMakeRunner.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 1U};

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

class RecordingRunner final : public cue::ChildProcessRunner
{
  public:
    /// @brief 返却予定Result列の所有権を取得して呼出記録用Runnerを構築する
    explicit RecordingRunner(std::vector<cue::ChildProcessResult> a_results) noexcept : m_results(std::move(a_results))
    {
    }

    /// @brief Process要求を記録しCancelまたは次の予定Resultを返す
    [[nodiscard]] cue::Result<cue::ChildProcessResult> run(
        const cue::ChildProcessRequest &a_request,
        const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        m_executables.emplace_back(a_request.executable());
        m_arguments.push_back(a_request.arguments());
        m_workingDirectories.emplace_back(a_request.working_directory());
        m_environments.push_back(a_request.environment_allowlist());
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::cancelled({}));
        }
        if (m_next >= m_results.size())
        {
            return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::exited(0U, {}));
        }
        return cue::Result<cue::ChildProcessResult>::success(std::move(m_results[m_next++]));
    }

    std::vector<std::string> m_executables;
    std::vector<std::vector<std::string>> m_arguments;
    std::vector<std::string> m_workingDirectories;
    std::vector<std::vector<cue::ChildProcessEnvironmentEntry>> m_environments;

  private:
    std::vector<cue::ChildProcessResult> m_results;
    std::size_t m_next = 0U;
};

class RecordingObserver final : public cue::CMakeStageObserver
{
  public:
    /// @brief Stage開始通知を受信順に記録する
    void on_stage_started(cue::BuildStage a_stage) noexcept override
    {
        m_started.push_back(a_stage);
    }

    /// @brief Stage完了通知を受信順に記録する
    void on_stage_completed(const cue::CMakeStageRecord &a_record) noexcept override
    {
        m_completed.push_back(a_record.result.stage());
    }

    std::vector<cue::BuildStage> m_started;
    std::vector<cue::BuildStage> m_completed;
};

/// @brief 指定ConfigurationからRunner検証用Build Planを作成する
[[nodiscard]] cue::BuildPlan make_plan(cue::BuildConfiguration a_configuration,
                                       const cue::AssertContext &a_assertContext)
{
    auto profile = cue::BuildProfile::create(a_configuration, cue::BuildTarget::GameModule, a_assertContext);
    const auto projectRoot = std::filesystem::current_path().generic_string();
    cue::BuildRequest request{projectRoot, *profile.try_value(), "01234567-89ab-4cde-8f01-23456789abcd",
                              k_workspaceCompatibility};
    auto plan = cue::create_build_plan(request, a_assertContext);
    return std::move(*plan.try_value());
}

/// @brief Unsigned Local Shipping Product用Runner検証Planを作成する
[[nodiscard]] cue::BuildPlan make_shipping_plan(const cue::AssertContext &a_assertContext)
{
    auto profile = cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::UnsignedLocal, {}, a_assertContext);
    const auto projectRoot = std::filesystem::current_path().generic_string();
    cue::BuildRequest request{projectRoot, *profile.try_value(), "11234567-89ab-4cde-8f01-23456789abcd",
                              k_workspaceCompatibility};
    auto plan = cue::create_build_plan(request, a_assertContext);
    return std::move(*plan.try_value());
}

/// @brief Processを起動しないTest用のAbsolute Runner設定を返す
[[nodiscard]] cue::CMakeRunnerSettings make_settings()
{
    return {"C:/Tools/cmake.exe",
            "C:/CueEngine",
            {{"SYSTEMROOT", "C:/Windows"}},
            std::chrono::seconds(30),
            std::chrono::minutes(5),
            "14.51.36231"};
}

/// @brief 各ConfigurationがPresetと固定TargetをArgument Vectorへ変換するか検証する
[[nodiscard]] bool test_configuration(cue::BuildConfiguration a_configuration, std::string_view a_preset,
                                      std::string_view a_configurationName, const cue::AssertContext &a_assertContext)
{
    cue::BuildPlan plan = make_plan(a_configuration, a_assertContext);
    RecordingRunner runner({cue::ChildProcessResult::exited(0U, {{0U, cue::ChildProcessStream::StandardOutput, "c"}}),
                            cue::ChildProcessResult::exited(0U, {{0U, cue::ChildProcessStream::StandardError, "b"}})});
    RecordingObserver observer;
    cue::ChildProcessCancellation cancellation;
    auto result = cue::run_cmake_build(plan, make_settings(), cue::CMakeConfigureMode::Required, runner, cancellation,
                                       observer, a_assertContext);
    if (!result || !result.try_value()->succeeded() || result.try_value()->stages().size() != 2U ||
        runner.m_arguments.size() != 2U || observer.m_started.size() != 2U || observer.m_completed.size() != 2U)
    {
        return false;
    }
    const std::vector<std::string> expectedConfigure = {"--preset", std::string(a_preset), "-B",
                                                        std::string(plan.binary_directory()), "-T",
                                                        "version=14.51.36231"};
    const std::vector<std::string> expectedBuild = {"--build",  std::string(plan.binary_directory()),
                                                    "--config", std::string(a_configurationName),
                                                    "--target", "CueGameModule", "--",
                                                    "/p:VCToolsVersion=14.51.36231"};
    return runner.m_executables[0] == "C:/Tools/cmake.exe" && runner.m_arguments[0] == expectedConfigure &&
           runner.m_arguments[1] == expectedBuild && runner.m_workingDirectories[0] == plan.project_root() &&
           runner.m_environments[0].size() == 2U && runner.m_environments[0][1].name == "CUE_ENGINE_ROOT" &&
           runner.m_environments[0][1].value == "C:/CueEngine" &&
           result.try_value()->stages()[0].output[0].bytes == "c" &&
           result.try_value()->stages()[1].output[0].bytes == "b";
}

/// @brief Shipping ProductをReleaseの固定CMake Targetへ変換するか検証する
[[nodiscard]] bool test_shipping_target(const cue::AssertContext &a_assertContext)
{
    cue::BuildPlan plan = make_shipping_plan(a_assertContext);
    RecordingRunner runner({cue::ChildProcessResult::exited(0U, {})});
    RecordingObserver observer;
    cue::ChildProcessCancellation cancellation;
    auto result = cue::run_cmake_build(plan, make_settings(), cue::CMakeConfigureMode::ReuseCompatibleTree, runner,
                                       cancellation, observer, a_assertContext);
    const std::vector<std::string> expectedBuild = {
        "--build", std::string(plan.binary_directory()), "--config", "Release", "--target", "CueGameProduct", "--",
        "/p:VCToolsVersion=14.51.36231"};
    return result && result.try_value()->succeeded() && runner.m_arguments.size() == 1U &&
           runner.m_arguments[0] == expectedBuild;
}

/// @brief Installed SDK設定ではGame ModuleとConfiguration一致RuntimeHostを同じTreeでBuildするか検証する
[[nodiscard]] bool test_runtime_host_target(const cue::AssertContext &a_assertContext)
{
    cue::BuildPlan plan = make_plan(cue::BuildConfiguration::Development, a_assertContext);
    RecordingRunner runner({cue::ChildProcessResult::exited(0U, {})});
    RecordingObserver observer;
    cue::ChildProcessCancellation cancellation;
    cue::CMakeRunnerSettings settings = make_settings();
    settings.buildsRuntimeHost = true;
    auto result = cue::run_cmake_build(plan, settings, cue::CMakeConfigureMode::ReuseCompatibleTree, runner,
                                       cancellation, observer, a_assertContext);
    const std::vector<std::string> expectedBuild = {
        "--build", std::string(plan.binary_directory()), "--config", "Development", "--target", "CueGameModule",
        "CueRuntimeHostForProject", "--", "/p:VCToolsVersion=14.51.36231"};
    return result && result.try_value()->succeeded() && runner.m_arguments.size() == 1U &&
           runner.m_arguments[0] == expectedBuild;
}

/// @brief Configure失敗時にBuildを開始せず、既存Tree再利用時はBuildだけ実行するか検証する
[[nodiscard]] bool test_configure_boundaries(const cue::AssertContext &a_assertContext)
{
    cue::BuildPlan plan = make_plan(cue::BuildConfiguration::Debug, a_assertContext);
    RecordingRunner failedRunner({cue::ChildProcessResult::exited(2U, {})});
    RecordingObserver failedObserver;
    cue::ChildProcessCancellation cancellation;
    auto failed = cue::run_cmake_build(plan, make_settings(), cue::CMakeConfigureMode::Required, failedRunner,
                                       cancellation, failedObserver, a_assertContext);

    RecordingRunner reuseRunner({cue::ChildProcessResult::exited(0U, {})});
    RecordingObserver reuseObserver;
    auto reused = cue::run_cmake_build(plan, make_settings(), cue::CMakeConfigureMode::ReuseCompatibleTree, reuseRunner,
                                       cancellation, reuseObserver, a_assertContext);
    return failed && !failed.try_value()->succeeded() && failed.try_value()->stages().size() == 1U &&
           failedRunner.m_arguments.size() == 1U && reused && reused.try_value()->succeeded() &&
           reuseRunner.m_arguments.size() == 1U && reuseRunner.m_arguments[0][0] == "--build";
}

/// @brief Cancel後に新しいCancellationで同じRunnerを再実行でき、Timeoutを区別するか検証する
[[nodiscard]] bool test_cancel_retry_timeout(const cue::AssertContext &a_assertContext)
{
    cue::BuildPlan plan = make_plan(cue::BuildConfiguration::Debug, a_assertContext);
    RecordingRunner runner({cue::ChildProcessResult::exited(0U, {}), cue::ChildProcessResult::timed_out({})});
    RecordingObserver observer;
    cue::ChildProcessCancellation cancelled;
    cancelled.request_cancel();
    auto first = cue::run_cmake_build(plan, make_settings(), cue::CMakeConfigureMode::ReuseCompatibleTree, runner,
                                      cancelled, observer, a_assertContext);
    cue::ChildProcessCancellation retry;
    auto second = cue::run_cmake_build(plan, make_settings(), cue::CMakeConfigureMode::ReuseCompatibleTree, runner,
                                       retry, observer, a_assertContext);
    auto third = cue::run_cmake_build(plan, make_settings(), cue::CMakeConfigureMode::ReuseCompatibleTree, runner,
                                      retry, observer, a_assertContext);
    return first && first.try_value()->stages()[0].result.outcome() == cue::BuildStageOutcome::Cancelled && second &&
           second.try_value()->succeeded() && third &&
           third.try_value()->stages()[0].result.outcome() == cue::BuildStageOutcome::TimedOut;
}

/// @brief 不正Path、Environment、TimeoutをStage開始前に設定Errorとして拒否するか検証する
[[nodiscard]] bool test_invalid_settings(const cue::AssertContext &a_assertContext)
{
    cue::BuildPlan plan = make_plan(cue::BuildConfiguration::Debug, a_assertContext);
    cue::CMakeRunnerSettings settings = make_settings();
    settings.environmentAllowlist.push_back({"cue_engine_root", "wrong"});
    RecordingRunner runner({});
    RecordingObserver observer;
    cue::ChildProcessCancellation cancellation;
    auto invalid = cue::run_cmake_build(plan, settings, cue::CMakeConfigureMode::Required, runner, cancellation,
                                        observer, a_assertContext);
    cue::CMakeRunnerSettings relativeExecutable = make_settings();
    relativeExecutable.cmakeExecutable = "cmake.exe";
    auto invalidExecutable = cue::run_cmake_build(plan, relativeExecutable, cue::CMakeConfigureMode::Required, runner,
                                                  cancellation, observer, a_assertContext);
    cue::CMakeRunnerSettings relativeEngineRoot = make_settings();
    relativeEngineRoot.engineSourceRoot = "CueEngine";
    auto invalidEngineRoot = cue::run_cmake_build(plan, relativeEngineRoot, cue::CMakeConfigureMode::Required, runner,
                                                  cancellation, observer, a_assertContext);
    cue::CMakeRunnerSettings invalidName = make_settings();
    invalidName.environmentAllowlist.push_back({"BAD=NAME", "value"});
    auto invalidEnvironmentName = cue::run_cmake_build(plan, invalidName, cue::CMakeConfigureMode::Required, runner,
                                                       cancellation, observer, a_assertContext);
    cue::CMakeRunnerSettings duplicateName = make_settings();
    duplicateName.environmentAllowlist.push_back({"systemroot", "duplicate"});
    auto duplicateEnvironmentName = cue::run_cmake_build(plan, duplicateName, cue::CMakeConfigureMode::Required, runner,
                                                         cancellation, observer, a_assertContext);
    cue::CMakeRunnerSettings nonAsciiName = make_settings();
    nonAsciiName.environmentAllowlist.push_back({"\xC3\x85_VAR", "value"});
    auto invalidNonAsciiName = cue::run_cmake_build(plan, nonAsciiName, cue::CMakeConfigureMode::Required, runner,
                                                    cancellation, observer, a_assertContext);
    cue::CMakeRunnerSettings invalidUtf8Value = make_settings();
    invalidUtf8Value.environmentAllowlist.push_back({"VALID_NAME", std::string("\xC3", 1U)});
    auto invalidEnvironmentValue = cue::run_cmake_build(plan, invalidUtf8Value, cue::CMakeConfigureMode::Required,
                                                        runner, cancellation, observer, a_assertContext);
    cue::CMakeRunnerSettings oversizedEnvironment = make_settings();
    oversizedEnvironment.environmentAllowlist.push_back({"VALID_NAME", std::string(32767U, 'a')});
    auto invalidEnvironmentLength = cue::run_cmake_build(plan, oversizedEnvironment, cue::CMakeConfigureMode::Required,
                                                         runner, cancellation, observer, a_assertContext);
    cue::CMakeRunnerSettings invalidToolsetVersion = make_settings();
    invalidToolsetVersion.visualStudioToolsetVersion = "14.51,36231";
    auto invalidToolset = cue::run_cmake_build(plan, invalidToolsetVersion, cue::CMakeConfigureMode::Required, runner,
                                               cancellation, observer, a_assertContext);
    return !invalid && !invalidExecutable && !invalidEngineRoot && !invalidEnvironmentName &&
           !duplicateEnvironmentName && !invalidNonAsciiName && !invalidEnvironmentValue && !invalidEnvironmentLength &&
           !invalidToolset && runner.m_arguments.empty() && observer.m_started.empty() && observer.m_completed.empty();
}
} // namespace

/// @brief CMake RunnerのArgument、Stage、失敗、Cancel／Retry契約をUIなしで検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_configuration(cue::BuildConfiguration::Debug, "windows-vs2026-debug", "Debug", assertContext) &&
                   test_configuration(cue::BuildConfiguration::Development, "windows-vs2026-development", "Development",
                                      assertContext) &&
                   test_configuration(cue::BuildConfiguration::Release, "windows-vs2026-release", "Release",
                                      assertContext) &&
                   test_shipping_target(assertContext) && test_runtime_host_target(assertContext) &&
                   test_configure_boundaries(assertContext) &&
                   test_cancel_retry_timeout(assertContext) && test_invalid_settings(assertContext)
               ? 0
               : 1;
}
