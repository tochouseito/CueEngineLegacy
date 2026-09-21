#include "TestSupport/RhiProcessTestFixture.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Platform/WindowSystem.h>
#include <Cue/Platform/Windows/WindowsPlatform.h>
#include <Cue/Platform/Windows/WindowsWindowInterop.h>
#include <Cue/RHI/D3D12/D3d12Backend.h>
#include <Cue/RHI/D3D12/TestSupport/D3d12DiagnosticsProbe.h>
#include <Cue/RHI/D3D12/TestSupport/D3d12SwapChainProbe.h>
#include <Cue/RHI/D3D12/Windows/D3d12WindowsPresentation.h>

#include <array>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class ProcessLogSink final : public cue::LogSink
{
  public:
    /// @brief 受け取った Log Record を対象 Sink へ書き込み、出力成否を返す
    [[nodiscard]] bool write(const cue::LogRecord &a_record) override
    {
        if (a_record.level() == cue::LogLevel::Error || a_record.level() == cue::LogLevel::Fatal)
        {
            ++m_errorCount;
        }

        return m_isEnabled;
    }

    /// @brief 対象 Sink に保留中の Log 出力を反映し、完了成否を返す
    [[nodiscard]] bool flush() override
    {
        return true;
    }

    /// @brief D3d12SwapChainProcess Test の Enabled を整合性を保って更新する
    void set_enabled(bool a_isEnabled) noexcept
    {
        m_isEnabled = a_isEnabled;
    }

    /// @brief D3d12SwapChainProcess Test が保持する Error Count を呼び出し元へ返す
    [[nodiscard]] std::uint32_t error_count() const noexcept
    {
        return m_errorCount;
    }

  private:
    std::uint32_t m_errorCount = 0;
    bool m_isEnabled = true;
};

class FailingLogSink final : public cue::LogSink
{
  public:
    /// @brief 受け取った Log Record を対象 Sink へ書き込み、出力成否を返す
    [[nodiscard]] bool write(const cue::LogRecord &) override
    {
        return false;
    }

    /// @brief 対象 Sink に保留中の Log 出力を反映し、完了成否を返す
    [[nodiscard]] bool flush() override
    {
        return true;
    }
};

class ForeignWindow final : public cue::Window
{
  public:
    /// @brief 生成済み Window を表示可能な状態へ移し、Native 表示結果を返す
    [[nodiscard]] cue::Result<void> show() noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief D3d12SwapChainProcess Test を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] cue::Result<void> destroy() noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief D3d12SwapChainProcess Test が保持する State を呼び出し元へ返す
    [[nodiscard]] cue::WindowState state() const noexcept override
    {
        return cue::WindowState::Created;
    }

    /// @brief D3d12SwapChainProcess Test が保持する Client Size を呼び出し元へ返す
    [[nodiscard]] cue::WindowSize client_size() const noexcept override
    {
        return {320, 180};
    }

    /// @brief D3d12SwapChainProcess Test の Pop Event へ安全に Access できる場合だけ参照を返す
    [[nodiscard]] bool try_pop_event(cue::WindowEvent &) noexcept override
    {
        return false;
    }
};

/// @brief Error が指定された Domain と Code を保持しているかを判定する
[[nodiscard]] bool has_error_code(const cue::Error *a_error, std::int64_t a_value) noexcept
{
    return a_error != nullptr && a_error->code().domain() == "Cue.RHI.D3D12" && a_error->code().value() == a_value;
}

/// @brief D3d12SwapChainProcess Test の Platform Error Code 条件を判定して返す
[[nodiscard]] bool has_platform_error_code(const cue::Error *a_error, std::int64_t a_value) noexcept
{
    return a_error != nullptr && a_error->code().domain() == "Cue.Platform.Windows" &&
           a_error->code().value() == a_value;
}

/// @brief D3d12SwapChainProcess Test の Production OwnershipScenario を実行し、検証結果を返す
[[nodiscard]] bool run_production_ownership(cue::Window &a_window, cue::AssertContext &a_assertContext) noexcept
{
    cue::D3d12BackendDescriptor backendDescriptor = {
        cue::D3d12AdapterPolicy::Warp,
        cue::D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return false;
    }

    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::PresentationDescriptor presentationDescriptor = {true};
    ForeignWindow foreignWindow;
    cue::Result<std::unique_ptr<cue::PresentationContext>> foreignResult =
        cue::create_d3d12_windows_presentation(*backend, foreignWindow, presentationDescriptor);

    if (foreignResult || !has_platform_error_code(foreignResult.try_error(), 12) ||
        backend->state() != cue::GraphicsBackendState::Ready)
    {
        static_cast<void>(backend->shutdown());
        return false;
    }

    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, presentationDescriptor);

    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return false;
    }

    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    cue::Result<void> activeGateResult = backend->shutdown();
    const bool valid = presentation->state() == cue::PresentationContextState::Ready && presentation->width() == 640 &&
                       presentation->height() == 360 && presentation->buffer_count() == 2 &&
                       presentation->current_back_buffer_index() < 2 && presentation->is_vsync_enabled() &&
                       !presentation->is_tearing_enabled() && !activeGateResult &&
                       has_error_code(activeGateResult.try_error(), 87) &&
                       backend->state() == cue::GraphicsBackendState::Ready;
    cue::Result<void> presentationShutdownResult = presentation->shutdown();

    if (!presentationShutdownResult)
    {
        return false;
    }

    presentation.reset();
    cue::Result<void> backendShutdownResult = backend->shutdown();

    if (!backendShutdownResult)
    {
        return false;
    }

    backend.reset();
    return valid;
}

/// @brief D3d12SwapChainProcess Test の Clear SmokeScenario を実行し、検証結果を返す
[[nodiscard]] bool run_clear_smoke(cue::Window &a_window, ProcessLogSink &a_logSink,
                                   cue::AssertContext &a_assertContext) noexcept
{
    const std::uint32_t initialErrorCount = a_logSink.error_count();
    cue::D3d12BackendDescriptor backendDescriptor = {
        cue::D3d12AdapterPolicy::Warp,
        cue::are_d3d12_diagnostics_allowed_for_probe() ? cue::D3d12ValidationMode::Standard
                                                       : cue::D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return false;
    }

    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::PresentationDescriptor descriptor = {true};
    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, descriptor);

    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return false;
    }

    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    constexpr std::array<float, 4> firstColor = {0.125F, 0.25F, 0.5F, 1.0F};
    constexpr std::array<float, 4> secondColor = {0.75F, 0.375F, 0.0625F, 1.0F};
    bool valid = cue::submit_d3d12_clear_frame_for_probe(*presentation, firstColor) &&
                 cue::submit_d3d12_clear_frame_for_probe(*presentation, secondColor);
    cue::Result<void> presentationShutdownResult = presentation->shutdown();
    presentation.reset();
    cue::Result<void> backendShutdownResult = backend->shutdown();
    valid = valid && presentationShutdownResult && backendShutdownResult &&
            backend->state() == cue::GraphicsBackendState::Shutdown &&
            a_logSink.error_count() == initialErrorCount;
    backend.reset();
    return valid;
}

/// @brief WARPで固定Cube Frameの入力拒否、単一Submit、Clear経路への復帰を検証する
[[nodiscard]] bool run_scene_smoke(cue::Window &a_window, ProcessLogSink &a_logSink,
                                   cue::AssertContext &a_assertContext) noexcept
{
    const std::uint32_t initialErrorCount = a_logSink.error_count();
    cue::D3d12BackendDescriptor backendDescriptor = {
        cue::D3d12AdapterPolicy::Warp,
        cue::are_d3d12_diagnostics_allowed_for_probe() ? cue::D3d12ValidationMode::Standard
                                                       : cue::D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);
    if (!backendResult || !a_window.show())
    {
        return false;
    }
    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, cue::PresentationDescriptor{true});
    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return false;
    }
    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    constexpr std::array<float, 16> identity = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    cue::PresentationSceneCube cube = {identity};
    cube.localToWorld[14] = 0.5F;
    const std::array cubes = {cube};
    cue::PresentationSceneFrameDescriptor scene = {{0.05F, 0.1F, 0.2F, 1.0F}, identity, cubes};
    scene.viewProjection[0] = (std::numeric_limits<float>::quiet_NaN)();
    cue::Result<cue::PresentationFrameStatus> invalidCamera = presentation->present_scene_frame(scene);
    scene.viewProjection[0] = 1.0F;
    scene.clearColor[0] = (std::numeric_limits<float>::infinity)();
    cue::Result<cue::PresentationFrameStatus> invalidColor = presentation->present_scene_frame(scene);
    scene.clearColor[0] = 0.05F;
    cue::PresentationSceneCube invalidCube = cube;
    invalidCube.localToWorld[0] = (std::numeric_limits<float>::quiet_NaN)();
    const std::array invalidCubes = {invalidCube};
    scene.cubes = invalidCubes;
    cue::Result<cue::PresentationFrameStatus> invalidInstance = presentation->present_scene_frame(scene);
    const std::vector<cue::PresentationSceneCube> tooManyCubes(cue::k_presentationSceneMaxCubeCount + 1U, cube);
    scene.cubes = tooManyCubes;
    cue::Result<cue::PresentationFrameStatus> invalidCount = presentation->present_scene_frame(scene);
    const cue::D3d12PresentationProbeReport before = cue::probe_d3d12_presentation(*presentation);
    bool valid = !invalidCamera && has_error_code(invalidCamera.try_error(), 308) && !invalidColor &&
                 has_error_code(invalidColor.try_error(), 308) && !invalidInstance &&
                 has_error_code(invalidInstance.try_error(), 308) && !invalidCount &&
                 has_error_code(invalidCount.try_error(), 308) && before.lastSubmittedFence == 0U &&
                 presentation->state() == cue::PresentationContextState::Ready;
    scene.cubes = cubes;
    cue::Result<cue::PresentationFrameStatus> sceneResult = presentation->present_scene_frame(scene);
    const cue::D3d12PresentationProbeReport afterScene = cue::probe_d3d12_presentation(*presentation);
    cue::Result<cue::PresentationFrameStatus> clearResult =
        presentation->present_frame(cue::PresentationFrameDescriptor{{0.1F, 0.1F, 0.1F, 1.0F}});
    const cue::D3d12PresentationProbeReport afterClear = cue::probe_d3d12_presentation(*presentation);
    valid = valid && sceneResult && afterScene.lastSubmittedFence == 1U && clearResult &&
            afterClear.lastSubmittedFence == 2U && afterClear.isAcceptingFrames;
    cue::Result<void> presentationShutdown = presentation->shutdown();
    presentation.reset();
    cue::Result<void> backendShutdown = backend->shutdown();
    return valid && presentationShutdown && backendShutdown && a_logSink.error_count() == initialErrorCount;
}

/// @brief 遅延Scene初期化で未分類Device Removalを検出したときの状態とDREDを検証する
[[nodiscard]] int run_scene_device_removal(cue::Window &a_window, cue::AssertContext &a_assertContext) noexcept
{
    cue::D3d12BackendDescriptor backendDescriptor = {
        cue::D3d12AdapterPolicy::Warp,
        cue::D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);
    if (!backendResult)
    {
        return 39;
    }
    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, cue::PresentationDescriptor{true});
    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return 40;
    }
    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    cue::Result<void> removalResult = cue::remove_d3d12_device_without_classification_for_probe(*backend);
    if (has_error_code(removalResult.try_error(), 89))
    {
        static_cast<void>(presentation->shutdown());
        presentation.reset();
        static_cast<void>(backend->shutdown());
        return 77;
    }
    cue::Result<std::uint32_t> beforeCount = cue::d3d12_dred_attempt_count_for_probe(*backend);
    constexpr std::array<float, 16> identity = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    cue::PresentationSceneFrameDescriptor scene = {{0.0F, 0.0F, 0.0F, 1.0F}, identity, {}};
    cue::Result<cue::PresentationFrameStatus> frameResult = presentation->present_scene_frame(scene);
    cue::Result<std::uint32_t> afterCount = cue::d3d12_dred_attempt_count_for_probe(*backend);
    const cue::D3d12PresentationProbeReport report = cue::probe_d3d12_presentation(*presentation);
    const bool classified = removalResult && beforeCount && *beforeCount.try_value() == 0U && !frameResult &&
                            presentation->state() == cue::PresentationContextState::DeviceRemoved &&
                            backend->state() == cue::GraphicsBackendState::DeviceRemoved && afterCount &&
                            *afterCount.try_value() == 1U && report.lastSubmittedFence == 0U;
    cue::Result<void> presentationShutdown = presentation->shutdown();
    presentation.reset();
    cue::Result<void> backendShutdown = backend->shutdown();
    return classified && presentationShutdown && backendShutdown &&
                   backend->state() == cue::GraphicsBackendState::Shutdown
               ? 0
               : 41;
}

/// @brief D3d12SwapChainProcess Test の Present Frames 300Scenario を実行し、検証結果を返す
[[nodiscard]] bool run_present_frames_300(cue::Window &a_window, ProcessLogSink &a_logSink,
                                          cue::AssertContext &a_assertContext) noexcept
{
    const std::uint32_t initialErrorCount = a_logSink.error_count();
    cue::D3d12BackendDescriptor backendDescriptor = {
        cue::D3d12AdapterPolicy::Warp,
        cue::are_d3d12_diagnostics_allowed_for_probe() ? cue::D3d12ValidationMode::Standard
                                                       : cue::D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult || !a_window.show())
    {
        return false;
    }

    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::PresentationDescriptor descriptor = {true};
    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, descriptor);

    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return false;
    }

    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    constexpr std::array<float, 4> color = {0.06F, 0.18F, 0.32F, 1.0F};
    bool valid = true;

    for (std::uint64_t frameNumber = 1; frameNumber <= 300 && valid; ++frameNumber)
    {
        const std::uint32_t submittedIndex = presentation->current_back_buffer_index();
        cue::PresentationFrameDescriptor frameDescriptor = {color};
        cue::Result<cue::PresentationFrameStatus> frameResult = presentation->present_frame(frameDescriptor);
        cue::D3d12PresentationProbeReport report = cue::probe_d3d12_presentation(*presentation);
        valid = frameResult && submittedIndex < 2 && presentation->current_back_buffer_index() < 2 &&
                report.lastSubmittedFence == frameNumber && report.frameReuseFences[submittedIndex] == frameNumber &&
                report.isAcceptingFrames;
    }

    cue::Result<void> presentationShutdownResult = presentation->shutdown();
    presentation.reset();
    cue::Result<void> backendShutdownResult = backend->shutdown();
    valid = valid && presentationShutdownResult && backendShutdownResult &&
            backend->state() == cue::GraphicsBackendState::Shutdown &&
            a_logSink.error_count() == initialErrorCount;
    backend.reset();
    return valid;
}

/// @brief D3d12SwapChainProcess Test の Resize LifecycleScenario を実行し、検証結果を返す
[[nodiscard]] bool run_resize_lifecycle(cue::Window &a_window, ProcessLogSink &a_logSink,
                                        cue::AssertContext &a_assertContext) noexcept
{
    const std::uint32_t initialErrorCount = a_logSink.error_count();
    cue::D3d12BackendDescriptor backendDescriptor = {
        cue::D3d12AdapterPolicy::Warp,
        cue::are_d3d12_diagnostics_allowed_for_probe() ? cue::D3d12ValidationMode::Standard
                                                       : cue::D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return false;
    }

    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::PresentationDescriptor descriptor = {true};
    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, descriptor);

    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return false;
    }

    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    cue::D3d12PresentationProbeReport initialReport = cue::probe_d3d12_presentation(*presentation);
    bool valid = initialReport.rtvCount == 2 && initialReport.formatsMatch && initialReport.isAcceptingFrames;
    valid = valid && cue::submit_d3d12_transition_frame_for_probe(*presentation);
    valid = valid && presentation->resize(640, 360) && !presentation->is_resize_pending();
    valid = valid && cue::submit_d3d12_transition_frame_for_probe(*presentation);
    valid = valid && presentation->resize(0, 360) && presentation->is_resize_pending() &&
            presentation->width() == 640 && presentation->height() == 360;
    cue::D3d12PresentationProbeReport suspendedReport = cue::probe_d3d12_presentation(*presentation);
    valid = valid && suspendedReport.rtvCount == 2 && suspendedReport.formatsMatch &&
            !suspendedReport.isAcceptingFrames;
    valid = valid && presentation->resize(640, 360) && !presentation->is_resize_pending();

    for (std::uint32_t iteration = 0; iteration < 50 && valid; ++iteration)
    {
        const std::uint32_t width = 641 + iteration;
        const std::uint32_t height = 361 + (iteration % 7);
        cue::Result<void> resizeResult = presentation->resize(width, height);
        cue::D3d12PresentationProbeReport report = cue::probe_d3d12_presentation(*presentation);
        valid = resizeResult && presentation->state() == cue::PresentationContextState::Ready &&
                presentation->width() == width && presentation->height() == height &&
                presentation->current_back_buffer_index() < 2 && report.rtvCount == 2 && report.formatsMatch &&
                report.isAcceptingFrames;

        if (iteration == 0 && valid)
        {
            valid = cue::submit_d3d12_transition_frame_for_probe(*presentation);
        }
    }

    cue::Result<void> presentationShutdownResult = presentation->shutdown();
    presentation.reset();
    cue::Result<void> backendShutdownResult = backend->shutdown();
    valid = valid && presentationShutdownResult && backendShutdownResult &&
            backend->state() == cue::GraphicsBackendState::Shutdown &&
            a_logSink.error_count() == initialErrorCount;
    backend.reset();
    return valid;
}

/// @brief D3d12SwapChainProcess Test の Device Removal LifecycleScenario を実行し、検証結果を返す
[[nodiscard]] int run_device_removal_lifecycle(cue::Window &a_window, cue::AssertContext &a_assertContext) noexcept
{
    cue::D3d12BackendDescriptor backendDescriptor = {
        cue::D3d12AdapterPolicy::Warp,
        cue::D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return 13;
    }

    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::PresentationDescriptor descriptor = {true};
    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, descriptor);

    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return 14;
    }

    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    cue::Result<void> removalResult = cue::remove_d3d12_device_without_classification_for_probe(*backend);

    if (has_error_code(removalResult.try_error(), 89))
    {
        static_cast<void>(presentation->shutdown());
        presentation.reset();
        static_cast<void>(backend->shutdown());
        backend.reset();
        return 77;
    }

    cue::Result<std::uint32_t> firstCountResult = cue::d3d12_dred_attempt_count_for_probe(*backend);
    const bool unclassifiedRemovalValid = removalResult && firstCountResult && *firstCountResult.try_value() == 0 &&
                                          backend->state() == cue::GraphicsBackendState::Ready;
    cue::Result<void> presentationShutdownResult = presentation->shutdown();
    cue::Result<std::uint32_t> contextCountResult = cue::d3d12_dred_attempt_count_for_probe(*backend);
    const bool contextValid = presentationShutdownResult &&
                              presentation->state() == cue::PresentationContextState::Shutdown &&
                              backend->state() == cue::GraphicsBackendState::DeviceRemoved && contextCountResult &&
                              *contextCountResult.try_value() == 1;
    presentation.reset();
    cue::Result<void> backendShutdownResult = backend->shutdown();
    const bool backendValid = backendShutdownResult && backend->state() == cue::GraphicsBackendState::Shutdown;
    backend.reset();
    return unclassifiedRemovalValid && contextValid && backendValid ? 0 : 15;
}

/// @brief D3d12SwapChainProcess Test の Device Removal ResizeScenario を実行し、検証結果を返す
[[nodiscard]] int run_device_removal_resize(cue::Window &a_window, cue::AssertContext &a_assertContext) noexcept
{
    cue::D3d12BackendDescriptor backendDescriptor = {
        cue::D3d12AdapterPolicy::Warp,
        cue::D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return 22;
    }

    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::PresentationDescriptor descriptor = {true};
    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, descriptor);

    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return 23;
    }

    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    cue::Result<void> removalResult = cue::remove_d3d12_device_without_classification_for_probe(*backend);

    if (has_error_code(removalResult.try_error(), 89))
    {
        static_cast<void>(presentation->shutdown());
        presentation.reset();
        static_cast<void>(backend->shutdown());
        return 77;
    }

    cue::Result<std::uint32_t> firstCountResult = cue::d3d12_dred_attempt_count_for_probe(*backend);
    cue::Result<void> resizeResult = presentation->resize(641, 361);
    cue::Result<std::uint32_t> resizeCountResult = cue::d3d12_dred_attempt_count_for_probe(*backend);
    cue::Result<cue::D3d12DredOwnerProbeReport> dredOwnerResult =
        cue::probe_d3d12_dred_owners_for_probe(*backend);
    const cue::D3d12DredOwnerProbeReport *dredOwners = dredOwnerResult.try_value();
    const bool resizeValid = removalResult && firstCountResult && *firstCountResult.try_value() == 0 &&
                             !resizeResult && has_error_code(resizeResult.try_error(), 34) && resizeCountResult &&
                             *resizeCountResult.try_value() == 1 &&
                             dredOwners != nullptr && dredOwners->hasCommandList &&
                             dredOwners->allocatorCount == 2 && dredOwners->backBufferCount == 2 &&
                             dredOwners->rtvCount == 2 &&
                             dredOwners->hasSwapChain && dredOwners->hasRtvHeap && dredOwners->hasQueue &&
                             dredOwners->hasFence && dredOwners->hasFenceEvent &&
                             presentation->state() == cue::PresentationContextState::Shutdown &&
                             backend->state() == cue::GraphicsBackendState::DeviceRemoved;
    cue::Result<void> presentationShutdownResult = presentation->shutdown();
    presentation.reset();
    cue::Result<void> backendShutdownResult = backend->shutdown();
    const bool cleanupValid = presentationShutdownResult && backendShutdownResult &&
                              backend->state() == cue::GraphicsBackendState::Shutdown;
    backend.reset();
    return resizeValid && cleanupValid ? 0 : 24;
}

/// @brief D3d12SwapChainProcess Test の Device Removal DRED FailureScenario を実行し、検証結果を返す
[[nodiscard]] int run_device_removal_dred_failure(cue::Window &a_window, ProcessLogSink &a_logSink,
                                                  cue::AssertContext &a_assertContext) noexcept
{
    if (!cue::are_d3d12_diagnostics_allowed_for_probe())
    {
        return 77;
    }

    cue::D3d12BackendDescriptor backendDescriptor = {
        cue::D3d12AdapterPolicy::Warp,
        cue::D3d12ValidationMode::Standard,
        true,
        5'000,
    };
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return 16;
    }

    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::PresentationDescriptor descriptor = {true};
    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, descriptor);

    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return 17;
    }

    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    cue::Result<void> removalResult = cue::remove_d3d12_device_without_classification_for_probe(*backend);
    cue::Result<std::uint32_t> firstCountResult = cue::d3d12_dred_attempt_count_for_probe(*backend);
    const bool removalValid = removalResult && firstCountResult && *firstCountResult.try_value() == 0 &&
                              backend->state() == cue::GraphicsBackendState::Ready;
    a_logSink.set_enabled(false);
    cue::Result<void> presentationShutdownResult = presentation->shutdown();
    a_logSink.set_enabled(true);
    cue::Result<std::uint32_t> contextCountResult = cue::d3d12_dred_attempt_count_for_probe(*backend);
    const bool contextValid = !presentationShutdownResult &&
                              presentation->state() == cue::PresentationContextState::Shutdown &&
                              backend->state() == cue::GraphicsBackendState::DeviceRemoved && contextCountResult &&
                              *contextCountResult.try_value() == 1;
    presentation.reset();
    cue::Result<void> backendShutdownResult = backend->shutdown();
    const bool cleanupValid =
        backendShutdownResult && backend->state() == cue::GraphicsBackendState::Shutdown;
    backend.reset();
    return removalValid && contextValid && cleanupValid ? 0 : 18;
}
} // namespace

/// @brief 指定 Scenario で Swap Chain 処理を実行し、Present、Resize、Device Removal を Process 単位で検証する
int main(int a_argumentCount, char **a_arguments)
{
    if (a_argumentCount != 2)
    {
        return 1;
    }

    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    std::unique_ptr<ProcessLogSink> processSink = std::make_unique<ProcessLogSink>();
    ProcessLogSink *processSinkView = processSink.get();
    sinks.push_back(std::move(processSink));
    cue::test::RhiProcessTestFixture fixture(std::move(sinks));
    cue::AssertContext &assertContext = fixture.assert_context();
    cue::Result<std::unique_ptr<cue::WindowSystem>> systemResult = cue::create_windows_window_system(assertContext);

    if (!systemResult)
    {
        return 2;
    }

    std::unique_ptr<cue::WindowSystem> windowSystem = std::move(*systemResult.try_value());
    cue::WindowDescriptor windowDescriptor = {"CueEngine D3D12 Swap Chain Probe", {640, 360}};
    cue::Result<std::unique_ptr<cue::Window>> windowResult = windowSystem->create_window(windowDescriptor);

    if (!windowResult)
    {
        return 3;
    }

    std::unique_ptr<cue::Window> window = std::move(*windowResult.try_value());
    cue::Result<cue::NativeWindowView> nativeWindowResult = cue::get_native_window_view(*window, assertContext);

    if (!nativeWindowResult)
    {
        static_cast<void>(window->destroy());
        return 4;
    }

    const void *nativeWindow = nativeWindowResult.try_value()->value();
    const std::string_view mode = a_arguments[1];
    bool valid = false;
    int failureCode = 10;

    if (mode == "SmokeVsync" || mode == "SmokeImmediate" || mode == "InfoQueue")
    {
        const bool isVsyncEnabled = mode != "SmokeImmediate";
        cue::Result<cue::D3d12SwapChainProbeReport> result =
            cue::probe_d3d12_swap_chain(nativeWindow, 640, 360, isVsyncEnabled, assertContext);
        const cue::D3d12SwapChainProbeReport *report = result.try_value();

        if (mode == "InfoQueue" && report != nullptr && !report->diagnosticsAvailable)
        {
            valid = true;
            failureCode = 77;
        }
        else
        {
            valid = report != nullptr && report->width == 640 && report->height == 360 && report->bufferCount == 2 &&
                    report->currentBackBufferIndex < 2 && report->descriptorShapeIsValid &&
                    report->backBuffersAreAvailable && report->altEnterWasDisabled &&
                    report->isVsyncEnabled == isVsyncEnabled &&
                    report->isTearingEnabled == (!isVsyncEnabled && report->isTearingSupported) &&
                    report->infoQueueErrorCount == 0;
            failureCode = valid ? 0 : 5;
        }
    }
    else if (mode == "TearingMatrix")
    {
        valid = cue::verify_d3d12_swap_chain_tearing_matrix_for_probe(nativeWindow, 640, 360, assertContext);
        failureCode = valid ? 0 : 6;
    }
    else if (mode == "CreationFaults")
    {
        valid = cue::verify_d3d12_swap_chain_faults_for_probe(nativeWindow, 640, 360, assertContext);
        failureCode = valid ? 0 : 7;
    }
    else if (mode == "LogFailure")
    {
        std::vector<std::unique_ptr<cue::LogSink>> failingSinks;
        failingSinks.push_back(std::make_unique<FailingLogSink>());
        cue::test::RhiProcessTestFixture failingFixture(std::move(failingSinks));
        cue::AssertContext &failingAssertContext = failingFixture.assert_context();
        valid = cue::verify_d3d12_swap_chain_log_failure_for_probe(nativeWindow, 640, 360, assertContext,
                                                                   failingAssertContext);
        failureCode = valid ? 0 : 12;
    }
    else if (mode == "Validation")
    {
        valid = cue::verify_d3d12_swap_chain_validation_for_probe(nativeWindow, 640, 360, assertContext);
        failureCode = valid ? 0 : 8;
    }
    else if (mode == "ProductionOwnership")
    {
        valid = run_production_ownership(*window, assertContext);
        failureCode = valid ? 0 : 9;
    }
    else if (mode == "ClearSmoke")
    {
        valid = run_clear_smoke(*window, *processSinkView, assertContext);
        failureCode = valid ? 0 : 27;
    }
    else if (mode == "SceneSmoke")
    {
        valid = run_scene_smoke(*window, *processSinkView, assertContext);
        failureCode = valid ? 0 : 38;
    }
    else if (mode == "SceneDeviceRemoval")
    {
        failureCode = run_scene_device_removal(*window, assertContext);
        valid = failureCode == 0;
    }
    else if (mode == "PresentFrames300")
    {
        valid = run_present_frames_300(*window, *processSinkView, assertContext);
        failureCode = valid ? 0 : 28;
    }
    else if (mode == "PresentMatrix")
    {
        valid = cue::verify_d3d12_swap_chain_present_matrix_for_probe(nativeWindow, 640, 360, assertContext);
        failureCode = valid ? 0 : 29;
    }
    else if (mode == "PresentSignalRecovery")
    {
        valid = cue::verify_d3d12_present_signal_recovery_for_probe(
            nativeWindow, 640, 360, cue::D3d12PresentFailureProbeMode::RecoveryMatrix, assertContext);
        failureCode = valid ? 0 : 30;
    }
    else if (mode == "BeginFrameUnavailableRetention")
    {
        valid = cue::verify_d3d12_present_signal_recovery_for_probe(
            nativeWindow, 640, 360, cue::D3d12PresentFailureProbeMode::BeginFrameUnavailable, assertContext);
        failureCode = valid ? 0 : 36;
    }
    else if (mode == "CloseFrameDeviceRemoved")
    {
        valid = cue::verify_d3d12_present_signal_recovery_for_probe(
            nativeWindow, 640, 360, cue::D3d12PresentFailureProbeMode::CloseFrameDeviceRemoved, assertContext);
        failureCode = valid ? 0 : 37;
    }
    else if (mode == "PresentUnavailableRetention")
    {
        valid = cue::verify_d3d12_present_signal_recovery_for_probe(
            nativeWindow, 640, 360, cue::D3d12PresentFailureProbeMode::PresentUnavailable, assertContext);
        failureCode = valid ? 0 : 31;
    }
    else if (mode == "SignalUnavailableRetention")
    {
        valid = cue::verify_d3d12_present_signal_recovery_for_probe(
            nativeWindow, 640, 360, cue::D3d12PresentFailureProbeMode::SignalUnavailable, assertContext);
        failureCode = valid ? 0 : 32;
    }
    else if (mode == "DirectPresentDeviceRemoved")
    {
        valid = cue::verify_d3d12_present_signal_recovery_for_probe(
            nativeWindow, 640, 360, cue::D3d12PresentFailureProbeMode::DirectPresentDeviceRemoved, assertContext);
        failureCode = valid ? 0 : (cue::was_d3d12_present_device_removal_probe_unavailable() ? 77 : 33);
    }
    else if (mode == "RecoverySignalDeviceRemoved")
    {
        valid = cue::verify_d3d12_present_signal_recovery_for_probe(
            nativeWindow, 640, 360, cue::D3d12PresentFailureProbeMode::RecoverySignalDeviceRemoved, assertContext);
        failureCode = valid ? 0 : (cue::was_d3d12_present_device_removal_probe_unavailable() ? 77 : 34);
    }
    else if (mode == "RegularSignalDeviceRemoved")
    {
        valid = cue::verify_d3d12_present_signal_recovery_for_probe(
            nativeWindow, 640, 360, cue::D3d12PresentFailureProbeMode::RegularSignalDeviceRemoved, assertContext);
        failureCode = valid ? 0 : (cue::was_d3d12_present_device_removal_probe_unavailable() ? 77 : 35);
    }
    else if (mode == "ResizeLifecycle")
    {
        valid = run_resize_lifecycle(*window, *processSinkView, assertContext);
        failureCode = valid ? 0 : 19;
    }
    else if (mode == "ResizeFailure")
    {
        valid = cue::verify_d3d12_swap_chain_resize_failure_for_probe(nativeWindow, 640, 360, assertContext);
        failureCode = valid ? 0 : 20;
    }
    else if (mode == "RtvRebuildFailure")
    {
        valid = cue::verify_d3d12_rtv_rebuild_failure_for_probe(nativeWindow, 640, 360, assertContext);
        failureCode = valid ? 0 : 21;
    }
    else if (mode == "TerminalResizeRejection")
    {
        valid = cue::verify_d3d12_terminal_resize_rejection_for_probe(nativeWindow, 640, 360, assertContext);
        failureCode = valid ? 0 : 25;
    }
    else if (mode == "ResizeUnavailableRetention")
    {
        valid = cue::verify_d3d12_resize_unavailable_retention_for_probe(nativeWindow, 640, 360, assertContext);
        failureCode = valid ? 0 : 26;
    }
    else if (mode == "DeviceRemovalLifecycle")
    {
        failureCode = run_device_removal_lifecycle(*window, assertContext);
        valid = failureCode == 0 || failureCode == 77;
    }
    else if (mode == "DeviceRemovalResize")
    {
        failureCode = run_device_removal_resize(*window, assertContext);
        valid = failureCode == 0 || failureCode == 77;
    }
    else if (mode == "DeviceRemovalDredFailure")
    {
        failureCode = run_device_removal_dred_failure(*window, *processSinkView, assertContext);
        valid = failureCode == 0 || failureCode == 77;
    }

    cue::Result<void> destroyResult = window->destroy();

    if (!destroyResult)
    {
        return 11;
    }

    window.reset();
    windowSystem.reset();
    return failureCode;
}
