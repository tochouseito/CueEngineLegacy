#pragma once

#include <Cue/Build/Plan.h>
#include <Cue/Platform/Process.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cue
{
class AssertContext;

/// @brief CMake Runner固有の安定した失敗分類
enum class CMakeRunnerError : std::int64_t
{
    InvalidSettings = 1,
    ProcessExecutionFailed
};

/// @brief Binary TreeをConfigureするか検証済みTreeを再利用するか指定する
enum class CMakeConfigureMode : std::uint8_t
{
    Required,
    ReuseCompatibleTree
};

/// @brief Machine固有値をPlanから分離した一回のCMake実行設定
struct CMakeRunnerSettings final
{
    /// @brief 起動するCMake ExecutableのUTF-8 Absolute Path
    std::string cmakeExecutable;
    /// @brief ChildへCUE_ENGINE_ROOTとして渡すUTF-8 Absolute Engine Source Root
    std::string engineSourceRoot;
    /// @brief Host Environmentを継承せずChildへ明示公開する値
    std::vector<ChildProcessEnvironmentEntry> environmentAllowlist;
    /// @brief Configure Stageの任意Timeout
    std::optional<std::chrono::milliseconds> configureTimeout;
    /// @brief Build Stageの任意Timeout
    std::optional<std::chrono::milliseconds> buildTimeout;
    /// @brief CMake ConfigureとMSBuildへ固定するVisual Studio minor Toolset Version
    std::string visualStudioToolsetVersion;
    /// @brief Game Moduleと同じ外部Build TreeでConfiguration一致RuntimeHostもBuildする
    bool buildsRuntimeHost = false;
};

/// @brief 完了StageとCapture済みLogを一つの所有値へ束ねる
struct CMakeStageRecord final
{
    /// @brief Stage OutcomeとNative Exit Code
    BuildStageResult result;
    /// @brief stdout／stderrの観測順を保持する所有Log
    std::vector<ChildProcessOutputChunk> output;
};

/// @brief CMake Configure／Build全体の実行済みStage列
///
/// Accessorが返すspanはこのResultの寿命を超えて保持しない。
class CMakeBuildResult final
{
  public:
    /// @brief Stageなしの無効なBuild Result生成を禁止する
    CMakeBuildResult() = delete;
    /// @brief 所有Logを暗黙複製しないためCopy構築を禁止する
    CMakeBuildResult(const CMakeBuildResult &) = delete;
    /// @brief 所有Logを暗黙複製しないためCopy代入を禁止する
    CMakeBuildResult &operator=(const CMakeBuildResult &) = delete;
    /// @brief StageとLogの所有権を移動する
    CMakeBuildResult(CMakeBuildResult &&) noexcept = default;
    /// @brief StageとLogの所有権を移動代入する
    CMakeBuildResult &operator=(CMakeBuildResult &&) noexcept = default;
    /// @brief 所有するStageとLogを解放する
    ~CMakeBuildResult() = default;

    /// @brief 完了順のStage Recordを返す
    [[nodiscard]] std::span<const CMakeStageRecord> stages() const noexcept;
    /// @brief 最後に必要なBuild Stageまで成功したか返す
    [[nodiscard]] bool succeeded() const noexcept;

  private:
    friend Result<CMakeBuildResult> run_cmake_build(const BuildPlan &, const CMakeRunnerSettings &, CMakeConfigureMode,
                                                    ChildProcessRunner &, const ChildProcessCancellation &,
                                                    class CMakeStageObserver &, const AssertContext &) noexcept;

    /// @brief 完了順のStage Record所有権からBuild Resultを構築する
    explicit CMakeBuildResult(std::vector<CMakeStageRecord> a_stages) noexcept;

    std::vector<CMakeStageRecord> m_stages;
};

/// @brief 同期RunnerのStage境界だけを呼出Thread上で観測するInterface
class CMakeStageObserver
{
  public:
    /// @brief 観測先の暗黙複製を禁止する
    CMakeStageObserver(const CMakeStageObserver &) = delete;
    /// @brief 観測先の暗黙Copy代入を禁止する
    CMakeStageObserver &operator=(const CMakeStageObserver &) = delete;
    /// @brief 派生Observerを正しく破棄する
    virtual ~CMakeStageObserver() = default;

    /// @brief Process開始直前に一度だけ通知する
    virtual void on_stage_started(BuildStage a_stage) noexcept = 0;
    /// @brief Process完了後に所有権を移す前の借用Recordを一度だけ通知する
    virtual void on_stage_completed(const CMakeStageRecord &a_record) noexcept = 0;

  protected:
    /// @brief 派生Observerを初期化する
    CMakeStageObserver() noexcept = default;
};

/// @brief 検証済みPlanをCMake Configure／BuildのArgument Vectorとして同期実行する
///
/// Plan、Settings、Cancellation、Observer、AssertContextは呼出中だけ借用し、参照を保持しない。Observerは呼出Threadだけで呼ぶ。
/// Process Runnerの`run`も直列に呼び、同じInstanceの並行利用は行わない。正常なProcess非0終了、Cancel、Timeoutは
/// 成功Result内のStage Recordとして返す。SettingsまたはConfigure Mode不正はInvalidSettings、Process境界自体の失敗は
/// ProcessExecutionFailedまたは下位Errorを返す。Visual Studio minor ToolsetはConfigureとBuildの両方へ固定する。
/// Allocation等の予期しない例外はFatalHandlerへ渡す。
[[nodiscard]] Result<CMakeBuildResult> run_cmake_build(const BuildPlan &a_plan, const CMakeRunnerSettings &a_settings,
                                                       CMakeConfigureMode a_configureMode,
                                                       ChildProcessRunner &a_processRunner,
                                                       const ChildProcessCancellation &a_cancellation,
                                                       CMakeStageObserver &a_observer,
                                                       const AssertContext &a_assertContext) noexcept;
} // namespace cue
