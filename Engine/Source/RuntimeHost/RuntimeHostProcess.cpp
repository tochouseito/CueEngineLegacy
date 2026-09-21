#include <Cue/RuntimeHost/RuntimeHostProcess.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Capability.h>
#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Foundation/NumberParsing.h>
#include <Cue/Platform/WindowSystem.h>
#include <Cue/Platform/Windows/WindowsPlatform.h>
#include <Cue/Renderer/RenderSnapshot.h>
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
#include <Cue/Platform/Windows/TestSupport/WindowsWindowLifecycleProbe.h>
#endif
#include <Cue/RHI/D3D12/D3d12Backend.h>
#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
#include <Cue/RHI/D3D12/TestSupport/D3d12SwapChainProbe.h>
#endif
#include <Cue/RHI/D3D12/Windows/D3d12WindowsPresentation.h>
#include <Cue/RuntimeHost/RuntimeHostApplication.h>

#include "RuntimeSceneFrame.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef CUE_RUNTIME_GRAPHICS_DIAGNOSTICS_DEFAULT
#error CUE_RUNTIME_GRAPHICS_DIAGNOSTICS_DEFAULT must be provided by the RuntimeHost Core CMake target
#endif

// Runtime Host は Platform と RHI を組み立て、起動から安全な終了までの Runtime 実行順を所有する
namespace
{
constexpr int k_invalidArguments = 64;
constexpr int k_systemCreationFailed = 1;
constexpr int k_windowCreationFailed = 2;
constexpr int k_windowShowFailed = 3;
constexpr int k_messagePumpFailed = 4;
constexpr int k_windowDestroyFailed = 5;
constexpr int k_graphicsBackendCreationFailed = 6;
constexpr int k_graphicsBackendShutdownFailed = 7;
constexpr int k_graphicsLogFailed = 8;
constexpr int k_presentationCreationFailed = 9;
constexpr int k_presentationShutdownFailed = 10;
constexpr int k_presentationFrameFailed = 11;
constexpr int k_presentationResizeFailed = 12;
constexpr int k_runtimeApplicationCreationFailed = 14;
constexpr int k_runtimeApplicationFrameFailed = 15;
constexpr int k_runtimeApplicationShutdownFailed = 16;
#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
constexpr int k_scenePixelSmokeFailed = 17;
#endif
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
constexpr int k_resizeSmokeFailed = 13;
constexpr std::uint32_t k_resizeSmokeCycleCount = 50;
constexpr std::uint32_t k_resizeSmokeResizeActionCount = k_resizeSmokeCycleCount * 3;
constexpr std::uint32_t k_resizeSmokeActionCount = k_resizeSmokeResizeActionCount + 1;
#endif

struct RuntimeOptions final
{
    std::string title = "CueEngine Runtime Host";
    cue::WindowSize clientSize = {1280, 720};
    bool isSmokeTest = false;
    bool isPackageRuntime = false;
    bool isGraphicsSmoke = false;
    bool isPresentationSmoke = false;
    bool isRenderSmoke = false;
#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
    bool isPackageScenePixelSmoke = false;
#endif
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
    bool isResizeSmoke = false;
#endif
    cue::D3d12AdapterPolicy graphicsAdapterPolicy = cue::D3d12AdapterPolicy::HighPerformanceHardware;
};

/// @brief Runtime停止理由をHost診断用の安定した文字列へ変換する
[[nodiscard]] std::string_view describe_runtime_stop_reason(
    cue::runtime::RuntimeApplicationStopReason a_reason) noexcept
{
    switch (a_reason)
    {
    case cue::runtime::RuntimeApplicationStopReason::Requested:
        return "Requested";
    case cue::runtime::RuntimeApplicationStopReason::WindowClosed:
        return "WindowClosed";
    case cue::runtime::RuntimeApplicationStopReason::HostFailure:
        return "HostFailure";
    case cue::runtime::RuntimeApplicationStopReason::RuntimeFailure:
        return "RuntimeFailure";
    default:
        return "None";
    }
}

/// @brief Main Cameraの選択結果をPackage起動診断へ変換する
[[nodiscard]] std::string_view describe_main_camera_status(cue::renderer::MainCameraStatus a_status) noexcept
{
    switch (a_status)
    {
    case cue::renderer::MainCameraStatus::Ready:
        return "Ready";
    case cue::renderer::MainCameraStatus::Multiple:
        return "Multiple";
    case cue::renderer::MainCameraStatus::InvalidProjection:
        return "InvalidProjection";
    default:
        return "Missing";
    }
}

/// @brief System Architecture を Native 定数へ依存しない診断文字列へ変換する
[[nodiscard]] std::string_view describe_system_architecture(cue::SystemArchitecture a_architecture) noexcept
{
    switch (a_architecture)
    {
    case cue::SystemArchitecture::X64:
        return "X64";
    case cue::SystemArchitecture::Arm64:
        return "Arm64";
    default:
        return "Unknown";
    }
}

/// @brief Capability Query 状態を診断文字列へ変換する
[[nodiscard]] std::string_view describe_query_status(cue::CapabilityQueryStatus a_status) noexcept
{
    switch (a_status)
    {
    case cue::CapabilityQueryStatus::Succeeded:
        return "Succeeded";
    case cue::CapabilityQueryStatus::Failed:
        return "Failed";
    default:
        return "NotQueried";
    }
}

/// @brief Hardware Support 状態を診断文字列へ変換する
[[nodiscard]] std::string_view describe_support(cue::CapabilitySupport a_support) noexcept
{
    switch (a_support)
    {
    case cue::CapabilitySupport::Supported:
        return "Supported";
    case cue::CapabilitySupport::Unsupported:
        return "Unsupported";
    default:
        return "Unknown";
    }
}

/// @brief Engine 実装状態を Hardware 対応状態と混同しない診断文字列へ変換する
[[nodiscard]] std::string_view describe_implementation(cue::CapabilityImplementation a_implementation) noexcept
{
    return a_implementation == cue::CapabilityImplementation::Implemented ? "Implemented" : "NotImplemented";
}

/// @brief Runtime Policy の有効化状態を Hardware と Engine 実装から独立した診断文字列へ変換する
[[nodiscard]] std::string_view describe_enablement(cue::CapabilityEnablement a_enablement) noexcept
{
    switch (a_enablement)
    {
    case cue::CapabilityEnablement::Enabled:
        return "Enabled";
    case cue::CapabilityEnablement::Disabled:
        return "Disabled";
    default:
        return "NotApplicable";
    }
}

/// @brief 数値 System Query を成功値または失敗理由が分かる診断文字列へ変換する
template <typename Value>
[[nodiscard]] std::string describe_system_value(const cue::SystemCapabilityValue<Value> &a_value)
{
    const Value *value = a_value.try_value();
    return value != nullptr ? std::to_string(*value) : std::string(describe_query_status(a_value.query_status()));
}

/// @brief Hardware Query 結果を未実装 Capability の有効な3状態へ変換する
[[nodiscard]] constexpr cue::CapabilityState make_not_implemented_capability_state(
    cue::CapabilitySupportState a_hardware) noexcept
{
    if (a_hardware.query_status() == cue::CapabilityQueryStatus::NotQueried)
    {
        return cue::CapabilityState::not_queried_not_implemented();
    }
    if (a_hardware.query_status() == cue::CapabilityQueryStatus::Failed)
    {
        return cue::CapabilityState::query_failed_not_implemented();
    }
    return a_hardware.support() == cue::CapabilitySupport::Supported
               ? cue::CapabilityState::supported_not_implemented()
               : cue::CapabilityState::unsupported_not_implemented();
}

/// @brief 現在 Machine の System Capability Snapshot を再現可能な単一診断へ記録する
[[nodiscard]] cue::LogResult log_system_capabilities(cue::Logger &a_logger, const cue::AssertContext &a_assertContext)
{
    cue::SystemCapabilityQueryReport report = cue::query_windows_system_capabilities(a_assertContext);
    const cue::SystemCapabilitySnapshot &snapshot = report.snapshot;
    const cue::CpuInstructionCapabilities &instructions = snapshot.instructions();
    std::string message =
        "System Capability Snapshot: ProcessArchitecture=" +
        std::string(describe_system_architecture(snapshot.process_architecture())) +
        ", NativeArchitecture=" + std::string(describe_system_architecture(snapshot.native_architecture())) +
        ", LogicalProcessorCount=" + describe_system_value(snapshot.logical_processor_count()) +
        ", PhysicalMemoryBytes=" + describe_system_value(snapshot.physical_memory_bytes()) +
        ", PageSizeBytes=" + describe_system_value(snapshot.page_size_bytes()) +
        ", CacheLineSizeBytes=" + describe_system_value(snapshot.cache_line_size_bytes()) +
        ", SSE2=" + std::string(describe_support(instructions.sse2.support())) +
        ", SSE3=" + std::string(describe_support(instructions.sse3.support())) +
        ", SSSE3=" + std::string(describe_support(instructions.ssse3.support())) +
        ", SSE4.1=" + std::string(describe_support(instructions.sse41.support())) +
        ", SSE4.2=" + std::string(describe_support(instructions.sse42.support())) +
        ", AVX=" + std::string(describe_support(instructions.avx.support())) +
        ", AVX2=" + std::string(describe_support(instructions.avx2.support())) +
        ", FMA=" + std::string(describe_support(instructions.fma.support())) +
        ", OsExtendedState=" + std::string(describe_support(instructions.osExtendedState.support()));
    const cue::LogResult snapshotResult = a_logger.log(cue::LogLevel::Info, message);
    return report.diagnosticResult == cue::LogResult::Success ? snapshotResult : report.diagnosticResult;
}

/// @brief Capability の Hardware、Engine 実装、Runtime 有効化を独立 Field として診断へ記録する
[[nodiscard]] cue::LogResult log_capability_state(cue::Logger &a_logger, std::string_view a_name,
                                                  cue::CapabilityState a_state)
{
    const cue::CapabilitySupportState hardware = a_state.hardware();
    std::string message = "Capability State: Name=" + std::string(a_name) +
                          ", Query=" + std::string(describe_query_status(hardware.query_status())) +
                          ", Support=" + std::string(describe_support(hardware.support())) +
                          ", Implementation=" + std::string(describe_implementation(a_state.implementation())) +
                          ", Enablement=" + std::string(describe_enablement(a_state.enablement()));
    return a_logger.log(cue::LogLevel::Info, message);
}

/// @brief Backend Snapshot と現在の Engine 実装を組み合わせて Feature 選択用の3状態診断を記録する
[[nodiscard]] cue::LogResult log_graphics_capability_states(cue::Logger &a_logger,
                                                            const cue::CapabilityReport &a_report)
{
    const std::array states = {
        std::pair<std::string_view, cue::CapabilityState>{"Baseline3D", cue::CapabilityState::supported_enabled()},
        std::pair<std::string_view, cue::CapabilityState>{
            "RayTracing", make_not_implemented_capability_state(a_report.rayTracing.support_state())},
        std::pair<std::string_view, cue::CapabilityState>{
            "MeshShader", make_not_implemented_capability_state(a_report.meshShader.support_state())},
        std::pair<std::string_view, cue::CapabilityState>{
            "VariableRateShading", make_not_implemented_capability_state(a_report.variableRateShading.support_state())},
        std::pair<std::string_view, cue::CapabilityState>{
            "SamplerFeedback", make_not_implemented_capability_state(a_report.samplerFeedback.support_state())},
        std::pair<std::string_view, cue::CapabilityState>{
            "WaveOperations", make_not_implemented_capability_state(a_report.waveOperations)},
        std::pair<std::string_view, cue::CapabilityState>{
            "EnhancedBarriers", make_not_implemented_capability_state(a_report.enhancedBarriers)},
    };

    for (const auto &[name, state] : states)
    {
        const cue::LogResult result = log_capability_state(a_logger, name, state);
        if (result != cue::LogResult::Success)
        {
            return result;
        }
    }
    return cue::LogResult::Success;
}

/// @brief Command Line を実行 Mode と Window 設定へ変換し、排他的な Mode 指定を検証する
[[nodiscard]] cue::Result<bool> parse_options(int a_argumentCount, wchar_t **a_arguments, RuntimeOptions &a_options,
                                              const cue::AssertContext &a_assertContext) noexcept
{
    int modeArgumentCount = 0;
    for (int index = 1; index < a_argumentCount; ++index)
    {
        std::wstring_view argument = a_arguments[index];

        if (argument == L"--smoke-test")
        {
            ++modeArgumentCount;
            a_options.isSmokeTest = true;
            continue;
        }

        if (argument == L"--package")
        {
            ++modeArgumentCount;
            a_options.isPackageRuntime = true;
            continue;
        }

        if (argument == L"--package-smoke-test")
        {
            ++modeArgumentCount;
            a_options.isPackageRuntime = true;
            a_options.isSmokeTest = true;
            a_options.graphicsAdapterPolicy = cue::D3d12AdapterPolicy::Warp;
            continue;
        }

        bool isGraphicsModeArgument =
            argument == L"--graphics-smoke" || argument == L"--presentation-smoke" || argument == L"--render-smoke";
#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
        isGraphicsModeArgument = isGraphicsModeArgument || argument == L"--package-scene-smoke-test";
#endif
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
        isGraphicsModeArgument = isGraphicsModeArgument || argument == L"--resize-smoke";
#endif

        if (isGraphicsModeArgument)
        {
            if (index + 1 >= a_argumentCount)
            {
                return cue::Result<bool>::success(false);
            }

            std::wstring_view value = a_arguments[++index];

            if (value == L"hardware")
            {
                a_options.graphicsAdapterPolicy = cue::D3d12AdapterPolicy::HighPerformanceHardware;
            }
            else if (value == L"warp")
            {
                a_options.graphicsAdapterPolicy = cue::D3d12AdapterPolicy::Warp;
            }
            else
            {
                return cue::Result<bool>::success(false);
            }

            ++modeArgumentCount;
            if (argument == L"--graphics-smoke")
            {
                a_options.isGraphicsSmoke = true;
            }
            else if (argument == L"--presentation-smoke")
            {
                a_options.isPresentationSmoke = true;
            }
            else if (argument == L"--render-smoke")
            {
                a_options.isRenderSmoke = true;
            }
#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
            else if (argument == L"--package-scene-smoke-test")
            {
                a_options.isPackageRuntime = true;
                a_options.isSmokeTest = true;
                a_options.isPackageScenePixelSmoke = true;
            }
#endif
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
            else
            {
                a_options.isResizeSmoke = true;
            }
#endif
            continue;
        }

        if (index + 1 >= a_argumentCount)
        {
            return cue::Result<bool>::success(false);
        }

        std::wstring_view value = a_arguments[++index];

        if (argument == L"--title")
        {
            cue::Result<std::string> titleResult = cue::convert_windows_argument_to_utf8(value, a_assertContext);

            if (!titleResult)
            {
                return cue::Result<bool>::failure(std::move(*titleResult.try_error()));
            }

            a_options.title = std::move(*titleResult.try_value());
        }
        else if (argument == L"--width")
        {
            std::optional<std::uint32_t> width = cue::parse_unsigned_decimal<std::uint32_t>(value);

            if (!width.has_value() || *width == 0)
            {
                return cue::Result<bool>::success(false);
            }

            a_options.clientSize.width = *width;
        }
        else if (argument == L"--height")
        {
            std::optional<std::uint32_t> height = cue::parse_unsigned_decimal<std::uint32_t>(value);

            if (!height.has_value() || *height == 0)
            {
                return cue::Result<bool>::success(false);
            }

            a_options.clientSize.height = *height;
        }
        else
        {
            return cue::Result<bool>::success(false);
        }
    }

    return cue::Result<bool>::success(modeArgumentCount <= 1);
}

/// @brief 無効な Command Line に対して利用可能な Runtime Host 引数を標準 Error へ表示する
void print_usage() noexcept
{
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
    std::fputws(L"Usage: CueRuntimeHost [--smoke-test | --package | --package-smoke-test | "
#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
                L"--package-scene-smoke-test <hardware|warp> | "
#endif
                L"--graphics-smoke <hardware|warp> | "
                L"--presentation-smoke <hardware|warp> | --render-smoke <hardware|warp> | "
                L"--resize-smoke <hardware|warp>] "
                L"[--title <title>] [--width <pixels>] [--height <pixels>]\n",
                stderr);
#else
    std::fputws(L"Usage: CueRuntimeHost [--smoke-test | --package | --package-smoke-test | "
#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
                L"--package-scene-smoke-test <hardware|warp> | "
#endif
                L"--graphics-smoke <hardware|warp> | "
                L"--presentation-smoke <hardware|warp> | --render-smoke <hardware|warp>] "
                L"[--title <title>] [--width <pixels>] [--height <pixels>]\n",
                stderr);
#endif
}

/// @brief Runtime Error を Logger へ記録し、呼び出し元へ対応する終了 Code を返す
[[nodiscard]] int report_error(cue::Logger &a_logger, std::string_view a_message, cue::Error &&a_error,
                               int a_exitCode) noexcept;

#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
/// @brief Scene Pixel Smoke失敗をRuntime Host Domainの診断可能なErrorへ変換する
[[nodiscard]] cue::Error make_scene_pixel_smoke_error(const cue::AssertContext &a_assertContext,
                                                      std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.RuntimeHost", k_scenePixelSmokeFailed);
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}
#endif

/// @brief Runtime Option と Build 設定から再現可能な D3D12 Backend 生成条件を構築する
[[nodiscard]] cue::D3d12BackendDescriptor make_backend_descriptor(const RuntimeOptions &a_options) noexcept
{
    // 同じ Build の診断条件を再現できるよう、Graphics 診断の既定値は Target 定義から一元的に決める
    constexpr bool enableDiagnostics = CUE_RUNTIME_GRAPHICS_DIAGNOSTICS_DEFAULT != 0;
    return {
        a_options.graphicsAdapterPolicy,
        enableDiagnostics ? cue::D3d12ValidationMode::Standard : cue::D3d12ValidationMode::Disabled,
        enableDiagnostics,
        5000,
    };
}

#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
// === Resize Smoke Test Probe ===
/// @brief Resize Smoke 失敗を Runtime Host Domain の診断可能な Error へ変換する
[[nodiscard]] cue::Error make_runtime_error(const cue::AssertContext &a_assertContext,
                                            std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.RuntimeHost", k_resizeSmokeFailed);
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Resize Smoke の Action 番号を Resize、Minimize、Restore、Close 操作へ変換して発行する
[[nodiscard]] cue::Result<void> issue_resize_smoke_action(cue::Window &a_window, std::uint32_t a_actionIndex,
                                                          const cue::AssertContext &a_assertContext) noexcept
{
    if (a_actionIndex == k_resizeSmokeResizeActionCount)
    {
        return cue::issue_windows_window_lifecycle_probe_action(
            a_window, cue::WindowsWindowLifecycleProbeAction::ResizeThenClose, {832, 468}, {768, 432}, a_assertContext);
    }

    const std::uint32_t phase = a_actionIndex % 3;

    if (phase == 0)
    {
        const std::uint32_t cycleIndex = a_actionIndex / 3;
        const cue::WindowSize finalSize = cycleIndex % 2 == 0 ? cue::WindowSize{768, 432} : cue::WindowSize{704, 396};
        return cue::issue_windows_window_lifecycle_probe_action(
            a_window, cue::WindowsWindowLifecycleProbeAction::Resize, {832, 468}, finalSize, a_assertContext);
    }

    const cue::WindowsWindowLifecycleProbeAction action =
        phase == 1 ? cue::WindowsWindowLifecycleProbeAction::Minimize : cue::WindowsWindowLifecycleProbeAction::Restore;
    return cue::issue_windows_window_lifecycle_probe_action(a_window, action, {}, {}, a_assertContext);
}
// === Resize Smoke Test Probe End ===
#endif

/// @brief 主因 Error を保持したまま Cleanup 中の Secondary Error を診断 Context へ追加する
void add_secondary_runtime_error(cue::Error &a_primaryError, const cue::Error &a_secondaryError,
                                 std::string_view a_context, const cue::AssertContext &a_assertContext,
                                 std::source_location a_location = std::source_location::current()) noexcept
{
    // 最初に処理を失敗させた Error を主因として保ち、後始末の失敗も同じ診断から追跡できるようにする
    a_primaryError.append_secondary_diagnostics(a_assertContext, a_secondaryError, a_context, "Secondary Runtime Error",
                                                a_location);
}

/// @brief Window を生成せず D3D12 Backend の生成、能力取得、停止経路を検証する
[[nodiscard]] int run_graphics_smoke(const RuntimeOptions &a_options, cue::Logger &a_logger,
                                     cue::AssertContext &a_assertContext)
{
    // Window を必要としない最小経路で Adapter と Device の生成、能力取得、安全な終了を検証する
    cue::D3d12BackendDescriptor descriptor = make_backend_descriptor(a_options);
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(descriptor, a_assertContext);

    if (!backendResult)
    {
        return report_error(a_logger, "Runtime Host failed to create D3D12 Backend",
                            std::move(*backendResult.try_error()), k_graphicsBackendCreationFailed);
    }

    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    const cue::CapabilityReport &capabilities = backend->capabilities();
    std::string capabilityMessage =
        "D3D12 Device Smoke ready: Adapter=" + capabilities.adapterName +
        ", VendorId=" + std::to_string(capabilities.vendorId) + ", DeviceId=" + std::to_string(capabilities.deviceId) +
        ", DedicatedVideoMemoryBytes=" + std::to_string(capabilities.dedicatedVideoMemoryBytes) + ", UMA=" +
        (capabilities.uma.support() == cue::CapabilitySupport::Supported ? "supported" : "unsupported-or-unknown");
    cue::LogResult capabilityLogResult = a_logger.log(cue::LogLevel::Info, capabilityMessage);
    cue::LogResult capabilityStateLogResult = log_graphics_capability_states(a_logger, capabilities);
    cue::Result<void> shutdownResult = backend->shutdown();

    if (!shutdownResult)
    {
        if (backend->state() == cue::GraphicsBackendState::Unavailable)
        {
            cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                              "Runtime Host could not prove safe D3D12 Backend shutdown",
                              std::move(*shutdownResult.try_error()));
        }

        backend.reset();
        return report_error(a_logger, "Runtime Host failed to shutdown D3D12 Backend",
                            std::move(*shutdownResult.try_error()), k_graphicsBackendShutdownFailed);
    }

    backend.reset();

    if (capabilityLogResult != cue::LogResult::Success || capabilityStateLogResult != cue::LogResult::Success)
    {
        return k_graphicsLogFailed;
    }

    cue::LogResult shutdownLogResult = a_logger.log(cue::LogLevel::Info, "D3D12 Device Smoke shutdown completed");
    cue::LogResult flushResult = a_logger.flush();
    return shutdownLogResult == cue::LogResult::Success && flushResult == cue::LogResult::Success ? 0
                                                                                                  : k_graphicsLogFailed;
}

/// @brief Win32 Window に対する D3D12 Presentation の生成と停止経路を検証する
[[nodiscard]] int run_presentation_smoke(const RuntimeOptions &a_options, cue::Window &a_window, cue::Logger &a_logger,
                                         cue::AssertContext &a_assertContext)
{
    // Presentation が参照する Device と Window を先に存続させ、描画 Loop を通さず Presentation Resource
    // 一式の寿命を検証する
    cue::D3d12BackendDescriptor backendDescriptor = make_backend_descriptor(a_options);
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return report_error(a_logger, "Runtime Host failed to create D3D12 Backend",
                            std::move(*backendResult.try_error()), k_graphicsBackendCreationFailed);
    }

    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::LogResult capabilityStateLogResult = log_graphics_capability_states(a_logger, backend->capabilities());
    cue::PresentationDescriptor presentationDescriptor = {true};
    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, presentationDescriptor);

    if (!presentationResult)
    {
        cue::Error presentationError = std::move(*presentationResult.try_error());
        cue::Result<void> backendShutdownResult = backend->shutdown();

        if (!backendShutdownResult)
        {
            cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                              "Runtime Host could not cleanup Backend after Presentation creation failure",
                              std::move(*backendShutdownResult.try_error()));
        }

        backend.reset();
        return report_error(a_logger, "Runtime Host failed to create D3D12 Presentation", std::move(presentationError),
                            k_presentationCreationFailed);
    }

    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    std::string presentationMessage =
        "D3D12 Presentation Smoke ready: Width=" + std::to_string(presentation->width()) +
        ", Height=" + std::to_string(presentation->height()) +
        ", BufferCount=" + std::to_string(presentation->buffer_count()) +
        ", CurrentBackBufferIndex=" + std::to_string(presentation->current_back_buffer_index()) +
        ", VSync=" + (presentation->is_vsync_enabled() ? "true" : "false") +
        ", TearingSupported=" + (presentation->is_tearing_supported() ? "true" : "false") +
        ", TearingEnabled=" + (presentation->is_tearing_enabled() ? "true" : "false");
    cue::LogResult presentationLogResult = a_logger.log(cue::LogLevel::Info, presentationMessage);

    // Presentation が保持する GPU Resource を先に解放し、参照先の Backend を最後まで有効に保つ
    cue::Result<void> presentationShutdownResult = presentation->shutdown();

    if (!presentationShutdownResult)
    {
        cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                          "Runtime Host could not prove safe D3D12 Presentation shutdown",
                          std::move(*presentationShutdownResult.try_error()));
    }

    presentation.reset();
    cue::Result<void> backendShutdownResult = backend->shutdown();

    if (!backendShutdownResult)
    {
        if (backend->state() == cue::GraphicsBackendState::Unavailable)
        {
            cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                              "Runtime Host could not prove safe D3D12 Backend shutdown",
                              std::move(*backendShutdownResult.try_error()));
        }

        backend.reset();
        return report_error(a_logger, "Runtime Host failed to shutdown D3D12 Backend",
                            std::move(*backendShutdownResult.try_error()), k_graphicsBackendShutdownFailed);
    }

    backend.reset();

    if (capabilityStateLogResult != cue::LogResult::Success || presentationLogResult != cue::LogResult::Success)
    {
        return k_graphicsLogFailed;
    }

    cue::LogResult shutdownLogResult = a_logger.log(cue::LogLevel::Info, "D3D12 Presentation Smoke shutdown completed");
    cue::LogResult flushResult = a_logger.flush();
    return shutdownLogResult == cue::LogResult::Success && flushResult == cue::LogResult::Success
               ? 0
               : k_presentationShutdownFailed;
}

/// @brief Fixed SmokeまたはExecutableが選択したProviderから共通RuntimeHost Applicationを開始する
[[nodiscard]] cue::Result<std::unique_ptr<cue::runtime_host::RuntimeHostApplication>> start_runtime_application(
    cue::Window &a_window, bool a_isPackageRuntime,
    cue::runtime_host::RuntimeHostStartupFactory a_startupFactory,
    const cue::AssertContext &a_assertContext) noexcept
{
    if (!a_isPackageRuntime)
    {
        return cue::runtime_host::RuntimeHostApplication::start_fixed_smoke(a_window, a_assertContext);
    }
    cue::Result<cue::runtime_host::RuntimeHostStartup> startup = a_startupFactory(a_assertContext);
    if (!startup)
    {
        return cue::Result<std::unique_ptr<cue::runtime_host::RuntimeHostApplication>>::failure(
            std::move(*startup.try_error()));
    }
    return cue::runtime_host::RuntimeHostApplication::start(
        a_window, std::move(*startup.try_value()), a_assertContext);
}

/// @brief Window Event、Resize、Frame 投入、GPU 停止を規定順で処理する Render Loop を実行する
[[nodiscard]] int run_render_loop(const RuntimeOptions &a_options, cue::WindowSystem &a_windowSystem,
                                  cue::Window &a_window, cue::Logger &a_logger, cue::AssertContext &a_assertContext,
                                  cue::runtime_host::RuntimeHostStartupFactory a_startupFactory)
{
    constexpr std::uint64_t renderSmokeFrameCount = 300;
    constexpr std::uint64_t packageSmokeFrameCount = 1;
    constexpr std::array<float, 4> clearColor = {0.06F, 0.18F, 0.32F, 1.0F};

    // Window を Presentation の Native Surface として使える状態で、Backend から依存順に描画資源を構築する
    cue::D3d12BackendDescriptor backendDescriptor = make_backend_descriptor(a_options);
    cue::Result<std::unique_ptr<cue::D3d12Backend>> backendResult =
        cue::create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return report_error(a_logger, "Runtime Host failed to create D3D12 Backend",
                            std::move(*backendResult.try_error()), k_graphicsBackendCreationFailed);
    }

    std::unique_ptr<cue::D3d12Backend> backend = std::move(*backendResult.try_value());
    cue::LogResult capabilityStateLogResult = log_graphics_capability_states(a_logger, backend->capabilities());
    cue::PresentationDescriptor presentationDescriptor = {true};
    cue::Result<std::unique_ptr<cue::PresentationContext>> presentationResult =
        cue::create_d3d12_windows_presentation(*backend, a_window, presentationDescriptor);

    if (!presentationResult)
    {
        cue::Error presentationError = std::move(*presentationResult.try_error());
        cue::Result<void> backendShutdownResult = backend->shutdown();

        if (!backendShutdownResult)
        {
            cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                              "Runtime Host could not cleanup Backend after Presentation creation failure",
                              std::move(*backendShutdownResult.try_error()));
        }

        backend.reset();
        return report_error(a_logger, "Runtime Host failed to create D3D12 Presentation", std::move(presentationError),
                            k_presentationCreationFailed);
    }

    std::unique_ptr<cue::PresentationContext> presentation = std::move(*presentationResult.try_value());
    std::string readyMessage = "D3D12 Render Loop ready: Width=" + std::to_string(presentation->width()) +
                               ", Height=" + std::to_string(presentation->height()) +
                               ", BufferCount=" + std::to_string(presentation->buffer_count()) +
                               ", VSync=" + (presentation->is_vsync_enabled() ? "true" : "false");
    cue::LogResult readyLogResult = a_logger.log(cue::LogLevel::Info, readyMessage);
    cue::Result<std::unique_ptr<cue::runtime_host::RuntimeHostApplication>> applicationResult =
        start_runtime_application(a_window, a_options.isPackageRuntime, a_startupFactory, a_assertContext);
    if (!applicationResult)
    {
        cue::Error applicationError = std::move(*applicationResult.try_error());
        cue::Result<void> presentationShutdown = presentation->shutdown();
        if (!presentationShutdown && presentation->state() == cue::PresentationContextState::Unavailable)
        {
            cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                              "Runtime Host could not cleanup Presentation after Runtime start failure",
                              std::move(*presentationShutdown.try_error()));
        }
        if (!presentationShutdown)
        {
            add_secondary_runtime_error(applicationError, *presentationShutdown.try_error(),
                                        "D3D12 Presentation shutdown also failed after Runtime start Error",
                                        a_assertContext);
        }
        presentation.reset();

        cue::Result<void> backendShutdown = backend->shutdown();
        if (!backendShutdown && backend->state() == cue::GraphicsBackendState::Unavailable)
        {
            cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                              "Runtime Host could not cleanup Backend after Runtime start failure",
                              std::move(*backendShutdown.try_error()));
        }
        if (!backendShutdown)
        {
            add_secondary_runtime_error(applicationError, *backendShutdown.try_error(),
                                        "D3D12 Backend shutdown also failed after Runtime start Error",
                                        a_assertContext);
        }
        backend.reset();
        return report_error(a_logger, "Runtime Host failed to start Runtime Application", std::move(applicationError),
                            k_runtimeApplicationCreationFailed);
    }

    std::unique_ptr<cue::runtime_host::RuntimeHostApplication> application = std::move(*applicationResult.try_value());
    const cue::LogResult runtimeReadyLogResult =
        a_logger.log(cue::LogLevel::Info,
                     "Runtime Application Session started: Generation=" + std::to_string(application->generation()) +
                         ", WorldId=" + std::to_string(application->world_id()));
    cue::LogResult renderSnapshotLogResult = cue::LogResult::Success;
    cue::LogResult presentationFrameLogResult = cue::LogResult::Success;
    if (a_options.isPackageRuntime)
    {
        const cue::renderer::RenderSnapshot &snapshot = application->render_snapshot();
        renderSnapshotLogResult = a_logger.log(
            cue::LogLevel::Info,
            "Runtime Render Snapshot: MainCamera=" +
                std::string(describe_main_camera_status(snapshot.main_camera_status())) +
                ", MeshCount=" + std::to_string(snapshot.meshes().size()));
    }
    std::optional<cue::Error> frameError;
    std::optional<cue::Error> applicationError;
    std::string_view loopErrorMessage = "Runtime Host rendering Frame failed";
    int loopErrorExitCode = k_presentationFrameFailed;
    std::uint64_t frameCount = 0;
    std::uint64_t occludedFrameCount = 0;
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
    std::uint64_t resizeEventCount = 0;
    std::uint64_t minimizeEventCount = 0;
    std::uint64_t restoreEventCount = 0;
    std::uint64_t resizeApplyCount = 0;
    std::uint64_t minimizedFrameSkipCount = 0;
    std::uint64_t resizeSmokePresentedFrameCount = 0;
    std::uint32_t resizeSmokeActionIndex = 0;
    bool isResizeSmokeBatchProbeIssued = false;
    bool isResizeSmokeStarted = false;
    bool isPackageSmokeCloseProbeIssued = false;
#endif
    bool isMinimized = false;
    bool isShutdownRequested = false;
    bool wasWindowCloseRequested = false;
    bool hasRuntimeFrameFailure = false;
    bool hasLoggedPresentationFrame = false;
#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
    bool isScenePixelCaptureArmed = false;
#endif

    while (!isShutdownRequested)
    {
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
        // === Resize Smoke Test Probe ===
        if (a_options.isResizeSmoke && !isResizeSmokeStarted && !isResizeSmokeBatchProbeIssued)
        {
            cue::Result<void> minimizeResult = issue_resize_smoke_action(a_window, 1, a_assertContext);
            cue::Result<void> restoreResult =
                minimizeResult ? issue_resize_smoke_action(a_window, 2, a_assertContext) : cue::Result<void>::success();

            if (!minimizeResult || !restoreResult)
            {
                frameError.emplace(minimizeResult ? std::move(*restoreResult.try_error())
                                                  : std::move(*minimizeResult.try_error()));
                loopErrorMessage = "Runtime Host Resize Smoke batch probe failed";
                loopErrorExitCode = k_resizeSmokeFailed;
                break;
            }

            isResizeSmokeBatchProbeIssued = true;
        }

        if (a_options.isResizeSmoke && isResizeSmokeStarted && resizeSmokeActionIndex < k_resizeSmokeActionCount)
        {
            cue::Result<void> actionResult =
                issue_resize_smoke_action(a_window, resizeSmokeActionIndex, a_assertContext);

            if (!actionResult)
            {
                frameError.emplace(std::move(*actionResult.try_error()));
                loopErrorMessage = "Runtime Host Resize Smoke Window operation failed";
                loopErrorExitCode = k_resizeSmokeFailed;
                break;
            }

            ++resizeSmokeActionIndex;
        }
        // === Resize Smoke Test Probe End ===
#endif

        // OS Event を先に全て取り込み、終了や最新の Window 状態を描画判断へ反映する
        cue::Result<cue::PumpStatus> pumpResult = a_windowSystem.pump_events();

        if (!pumpResult)
        {
            frameError.emplace(std::move(*pumpResult.try_error()));
            loopErrorMessage = "Runtime Host Message Pump failed";
            loopErrorExitCode = k_messagePumpFailed;
            break;
        }

        if (*pumpResult.try_value() == cue::PumpStatus::QuitRequested)
        {
            isShutdownRequested = true;
            wasWindowCloseRequested = true;
        }

        cue::WindowEvent event = {};
        // 同一 Pump 内の連続 Resize は最後の Client Size へ集約し、古い中間 Size で SwapChain を作り直さない
        std::optional<cue::WindowSize> pendingResize;

        while (a_window.try_pop_event(event))
        {
            if (event.type == cue::WindowEventType::CloseRequested || event.type == cue::WindowEventType::Destroyed)
            {
                isShutdownRequested = true;
                wasWindowCloseRequested = true;
            }
            else if (event.type == cue::WindowEventType::Resized)
            {
                pendingResize = event.clientSize;
                isMinimized = false;
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
                ++resizeEventCount;
#endif
            }
            else if (event.type == cue::WindowEventType::Minimized)
            {
                pendingResize = cue::WindowSize{0, 0};
                isMinimized = true;
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
                ++minimizeEventCount;
#endif
            }
            else if (event.type == cue::WindowEventType::Restored)
            {
                pendingResize = event.clientSize;
                isMinimized = false;
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
                ++restoreEventCount;
#endif
            }
        }

        if (isShutdownRequested)
        {
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
            if (a_options.isResizeSmoke)
            {
                const bool smokeSequenceValid = resizeSmokeActionIndex == k_resizeSmokeActionCount &&
                                                resizeEventCount == k_resizeSmokeCycleCount * 2 + 2 &&
                                                minimizeEventCount == k_resizeSmokeCycleCount &&
                                                restoreEventCount == k_resizeSmokeCycleCount &&
                                                resizeApplyCount == k_resizeSmokeResizeActionCount &&
                                                minimizedFrameSkipCount == k_resizeSmokeCycleCount &&
                                                resizeSmokePresentedFrameCount == k_resizeSmokeCycleCount * 2 &&
                                                presentation->width() == 704 && presentation->height() == 396;

                if (!smokeSequenceValid)
                {
                    frameError.emplace(make_runtime_error(
                        a_assertContext, "Runtime Host Resize Smoke observed an incomplete Window Event sequence"));
                    loopErrorMessage = "Runtime Host Resize Smoke sequence failed";
                    loopErrorExitCode = k_resizeSmokeFailed;
                }
            }
#endif

            break;
        }

        if (pendingResize)
        {
            // Event drain 後に一度だけ適用し、非 0 Size は同期し、0 Size は Native Resize を延期する
            cue::Result<void> resizeResult = presentation->resize(pendingResize->width, pendingResize->height);

            if (!resizeResult)
            {
                frameError.emplace(std::move(*resizeResult.try_error()));
                loopErrorMessage = "Runtime Host failed to resize D3D12 Presentation";
                loopErrorExitCode = k_presentationResizeFailed;
                break;
            }

#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
            ++resizeApplyCount;
            if (a_options.isResizeSmoke && pendingResize->width != 0 && pendingResize->height != 0 &&
                (presentation->width() != pendingResize->width || presentation->height() != pendingResize->height))
            {
                frameError.emplace(make_runtime_error(
                    a_assertContext, "Runtime Host Resize Smoke did not apply the latest Window Client Size"));
                loopErrorMessage = "Runtime Host Resize Smoke size verification failed";
                loopErrorExitCode = k_resizeSmokeFailed;
                break;
            }
#endif
        }

        // Pumpで蓄積したPortable Inputを確定し、Clock、System、Structural Safe PointをPresentより先に実行する
        cue::Result<void> runtimeFrame = application->advance_frame();
        if (!runtimeFrame)
        {
            frameError.emplace(std::move(*runtimeFrame.try_error()));
            hasRuntimeFrameFailure = true;
            isShutdownRequested = true;
            loopErrorMessage = "Runtime Host Runtime Application Frame failed";
            loopErrorExitCode = k_runtimeApplicationFrameFailed;
            break;
        }

        if (isMinimized || presentation->is_resize_pending())
        {
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
            ++minimizedFrameSkipCount;
#endif
            // 描画可能な Client Area がない間は GPU へ Frame を投入せず、待機中の Busy Loop も避ける
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
        // 初回Pumpで届くDPI/Resizeを反映したBack Bufferに対してのみFootprintを作る
        if (a_options.isPackageScenePixelSmoke && !isScenePixelCaptureArmed)
        {
            if (!cue::arm_d3d12_scene_pixel_capture_for_probe(*presentation))
            {
                frameError.emplace(make_scene_pixel_smoke_error(
                    a_assertContext, "Runtime Package Scene pixel capture could not be armed"));
                loopErrorMessage = "Runtime Package Scene pixel capture setup failed";
                loopErrorExitCode = k_scenePixelSmokeFailed;
                break;
            }

            isScenePixelCaptureArmed = true;
        }
#endif

        cue::Result<cue::runtime_host::RuntimePresentationFrame> presentationFrame =
            cue::runtime_host::make_runtime_presentation_frame(application->render_snapshot(), presentation->width(),
                                                               presentation->height(), clearColor, a_assertContext);
        if (!presentationFrame)
        {
            frameError.emplace(std::move(*presentationFrame.try_error()));
            loopErrorMessage = "Runtime Host failed to convert Render Snapshot for Presentation";
            break;
        }
        if (a_options.isPackageRuntime && !hasLoggedPresentationFrame)
        {
            const bool isScene = presentationFrame.try_value()->mode() ==
                                 cue::runtime_host::RuntimePresentationFrameMode::Scene;
            presentationFrameLogResult = a_logger.log(
                cue::LogLevel::Info,
                "Runtime Presentation Frame: Mode=" + std::string(isScene ? "Scene" : "DiagnosticClear") +
                    ", CubeCount=" + std::to_string(presentationFrame.try_value()->cube_count()));
            hasLoggedPresentationFrame = true;
        }
        cue::Result<cue::PresentationFrameStatus> frameResult =
            cue::runtime_host::present_runtime_presentation_frame(*presentation, *presentationFrame.try_value());

        if (!frameResult)
        {
            frameError.emplace(std::move(*frameResult.try_error()));
            break;
        }

        ++frameCount;

        if (a_options.isPackageRuntime && a_options.isSmokeTest && frameCount >= packageSmokeFrameCount)
        {
#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
            if (!isPackageSmokeCloseProbeIssued)
            {
                cue::Result<void> closeResult = cue::issue_windows_window_lifecycle_probe_action(
                    a_window, cue::WindowsWindowLifecycleProbeAction::ResizeThenClose, a_options.clientSize,
                    a_options.clientSize, a_assertContext);
                if (!closeResult)
                {
                    frameError.emplace(std::move(*closeResult.try_error()));
                    loopErrorMessage = "Runtime Package Window Close probe failed";
                    loopErrorExitCode = k_messagePumpFailed;
                    break;
                }
                isPackageSmokeCloseProbeIssued = true;
            }
#else
            isShutdownRequested = true;
#endif
        }

        if (*frameResult.try_value() == cue::PresentationFrameStatus::Occluded)
        {
            ++occludedFrameCount;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
        if (a_options.isResizeSmoke)
        {
            if (!isResizeSmokeStarted)
            {
                const bool batchProbeValid = isResizeSmokeBatchProbeIssued && minimizeEventCount == 1 &&
                                             restoreEventCount == 1 && !isMinimized &&
                                             !presentation->is_resize_pending();

                if (!batchProbeValid)
                {
                    frameError.emplace(make_runtime_error(
                        a_assertContext, "Runtime Host Resize Smoke did not preserve the latest batched Window state"));
                    loopErrorMessage = "Runtime Host Resize Smoke batch verification failed";
                    loopErrorExitCode = k_resizeSmokeFailed;
                    break;
                }

                resizeEventCount = 0;
                minimizeEventCount = 0;
                restoreEventCount = 0;
                resizeApplyCount = 0;
                minimizedFrameSkipCount = 0;
                isResizeSmokeStarted = true;
            }
            else
            {
                ++resizeSmokePresentedFrameCount;
            }
        }
#endif

        if (a_options.isRenderSmoke && frameCount >= renderSmokeFrameCount)
        {
            isShutdownRequested = true;
        }
    }

    const cue::runtime::RuntimeApplicationStopReason runtimeStopReason =
        hasRuntimeFrameFailure || frameError.has_value()
            ? cue::runtime::RuntimeApplicationStopReason::HostFailure
            : (wasWindowCloseRequested ? cue::runtime::RuntimeApplicationStopReason::WindowClosed
                                       : cue::runtime::RuntimeApplicationStopReason::Requested);
    cue::Result<void> runtimeStopped = application->stop(runtimeStopReason);
    if (!runtimeStopped && !application->is_cleanup_complete())
    {
        cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                          "Runtime Host could not prove safe Runtime Application shutdown",
                          std::move(*runtimeStopped.try_error()));
    }
    if (!runtimeStopped)
    {
        applicationError.emplace(std::move(*runtimeStopped.try_error()));
        if (!hasRuntimeFrameFailure)
        {
            loopErrorMessage = "Runtime Host failed to shutdown Runtime Application";
            loopErrorExitCode = k_runtimeApplicationShutdownFailed;
        }
    }
    const cue::LogResult runtimeShutdownLogResult =
        a_logger.log(cue::LogLevel::Info, "Runtime Application Session stopped: Reason=" +
                                              std::string(describe_runtime_stop_reason(application->stop_reason())) +
                                              ", FrameCount=" + std::to_string(application->frame_count()));
    application.reset();

    const std::uint32_t finalBackBufferIndex = presentation->current_back_buffer_index();

    // GPU 使用中の Presentation を先に停止し、依存先の Backend はその完了確認まで存続させる
    cue::Result<void> presentationShutdownResult = presentation->shutdown();

    if (!presentationShutdownResult && presentation->state() == cue::PresentationContextState::Unavailable)
    {
        cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                          "Runtime Host could not prove safe D3D12 Presentation shutdown",
                          std::move(*presentationShutdownResult.try_error()));
    }

    std::optional<cue::Error> presentationShutdownError;

    if (!presentationShutdownResult)
    {
        presentationShutdownError.emplace(std::move(*presentationShutdownResult.try_error()));
    }

    cue::LogResult scenePixelSmokeLogResult = cue::LogResult::Success;
#if defined(CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT) && CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT
    if (a_options.isPackageScenePixelSmoke && presentationShutdownResult)
    {
        if (!cue::validate_d3d12_scene_pixel_capture_for_probe(*presentation, clearColor))
        {
            if (!frameError)
            {
                frameError.emplace(make_scene_pixel_smoke_error(
                    a_assertContext, "Runtime Package Scene did not produce a clear corner and non-clear Scene pixel"));
            }
            loopErrorMessage = "Runtime Package Scene pixel verification failed";
            loopErrorExitCode = k_scenePixelSmokeFailed;
        }
        else
        {
            scenePixelSmokeLogResult =
                a_logger.log(cue::LogLevel::Info, "Runtime Package Scene Pixel Probe: Passed");
        }
    }
#endif

    presentation.reset();

    // Presentation の所有物がなくなってから Device を停止し、参照先を先に破棄する順序を防ぐ
    cue::Result<void> backendShutdownResult = backend->shutdown();

    if (!backendShutdownResult && backend->state() == cue::GraphicsBackendState::Unavailable)
    {
        cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                          "Runtime Host could not prove safe D3D12 Backend shutdown",
                          std::move(*backendShutdownResult.try_error()));
    }

    std::optional<cue::Error> backendShutdownError;

    if (!backendShutdownResult)
    {
        backendShutdownError.emplace(std::move(*backendShutdownResult.try_error()));
    }

    backend.reset();

    // Render Loop 中に先行した Error を Primary とし、終了処理で増えた Error は Secondary として診断情報へ統合する
    if (frameError)
    {
        if (applicationError)
        {
            add_secondary_runtime_error(*frameError, *applicationError,
                                        "Runtime Application shutdown also reported an Error", a_assertContext);
        }

        if (presentationShutdownError)
        {
            add_secondary_runtime_error(*frameError, *presentationShutdownError,
                                        "D3D12 Presentation shutdown also failed after rendering Error",
                                        a_assertContext);
        }

        if (backendShutdownError)
        {
            add_secondary_runtime_error(*frameError, *backendShutdownError,
                                        "D3D12 Backend shutdown also failed after rendering Error", a_assertContext);
        }

        return report_error(a_logger, loopErrorMessage, std::move(*frameError), loopErrorExitCode);
    }

    if (applicationError)
    {
        if (presentationShutdownError)
        {
            add_secondary_runtime_error(*applicationError, *presentationShutdownError,
                                        "D3D12 Presentation shutdown also failed after Runtime Error", a_assertContext);
        }
        if (backendShutdownError)
        {
            add_secondary_runtime_error(*applicationError, *backendShutdownError,
                                        "D3D12 Backend shutdown also failed after Runtime Error", a_assertContext);
        }
        return report_error(a_logger, loopErrorMessage, std::move(*applicationError), loopErrorExitCode);
    }

    if (presentationShutdownError)
    {
        // Render Loop 中に先行 Error がない場合は、先に失敗した Presentation 終了を Primary として扱う
        if (backendShutdownError)
        {
            add_secondary_runtime_error(*presentationShutdownError, *backendShutdownError,
                                        "D3D12 Backend shutdown also failed after Presentation shutdown Error",
                                        a_assertContext);
        }

        return report_error(a_logger, "Runtime Host failed to shutdown D3D12 Presentation",
                            std::move(*presentationShutdownError), k_presentationShutdownFailed);
    }

    if (backendShutdownError)
    {
        return report_error(a_logger, "Runtime Host failed to shutdown D3D12 Backend", std::move(*backendShutdownError),
                            k_graphicsBackendShutdownFailed);
    }

    cue::LogResult resizeSmokeLogResult = cue::LogResult::Success;

#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
    if (a_options.isResizeSmoke)
    {
        std::string resizeSmokeMessage =
            "D3D12 Resize Smoke completed: ResizeEventCount=" + std::to_string(resizeEventCount) +
            ", MinimizeEventCount=" + std::to_string(minimizeEventCount) +
            ", RestoreEventCount=" + std::to_string(restoreEventCount) +
            ", ResizeApplyCount=" + std::to_string(resizeApplyCount) +
            ", MinimizedFrameSkipCount=" + std::to_string(minimizedFrameSkipCount) +
            ", PresentedFrameCount=" + std::to_string(resizeSmokePresentedFrameCount);
        resizeSmokeLogResult = a_logger.log(cue::LogLevel::Info, resizeSmokeMessage);
    }
#endif

    std::string completionMessage = "D3D12 Render Loop completed: FrameCount=" + std::to_string(frameCount) +
                                    ", OccludedFrameCount=" + std::to_string(occludedFrameCount) +
                                    ", FinalBackBufferIndex=" + std::to_string(finalBackBufferIndex);
    cue::LogResult completionLogResult = a_logger.log(cue::LogLevel::Info, completionMessage);
    cue::LogResult shutdownLogResult = a_logger.log(cue::LogLevel::Info, "D3D12 Render Loop shutdown completed");
    cue::LogResult flushResult = a_logger.flush();
    return capabilityStateLogResult == cue::LogResult::Success && readyLogResult == cue::LogResult::Success &&
                   runtimeReadyLogResult == cue::LogResult::Success &&
                   renderSnapshotLogResult == cue::LogResult::Success &&
                   presentationFrameLogResult == cue::LogResult::Success &&
                   runtimeShutdownLogResult == cue::LogResult::Success &&
                   scenePixelSmokeLogResult == cue::LogResult::Success &&
                   resizeSmokeLogResult == cue::LogResult::Success && completionLogResult == cue::LogResult::Success &&
                   shutdownLogResult == cue::LogResult::Success && flushResult == cue::LogResult::Success
               ? 0
               : k_graphicsLogFailed;
}

[[nodiscard]] int report_error(cue::Logger &a_logger, std::string_view a_message, cue::Error &&a_error,
                               int a_exitCode) noexcept
{
    static_cast<void>(a_logger.log(cue::LogLevel::Error, a_message, std::move(a_error)));
    static_cast<void>(a_logger.flush());
    return a_exitCode;
}

/// @brief 選択された Runtime Mode に必要な Platform と RHI を組み立て、安全な終了まで実行する
[[nodiscard]] int run(const RuntimeOptions &a_options, cue::Logger &a_logger, cue::AssertContext &a_assertContext,
                      cue::runtime_host::RuntimeHostStartupFactory a_startupFactory)
{
    static_cast<void>(a_logger.log(cue::LogLevel::Info, "Runtime Host initialization started"));

    // Platform Query は全 Runtime Mode 共通の Composition Root で一度だけ実行し、起動経路による診断欠落を防ぐ
    if (log_system_capabilities(a_logger, a_assertContext) != cue::LogResult::Success)
    {
        return k_graphicsLogFailed;
    }

    // Graphics Smoke は Window 依存を除外し、Backend 単体の失敗範囲を明確にする
    if (a_options.isGraphicsSmoke)
    {
        return run_graphics_smoke(a_options, a_logger, a_assertContext);
    }

    cue::Result<std::unique_ptr<cue::WindowSystem>> systemResult = cue::create_windows_window_system(a_assertContext);

    if (!systemResult)
    {
        return report_error(a_logger, "Runtime Host failed to create Window System",
                            std::move(*systemResult.try_error()), k_systemCreationFailed);
    }

    // WindowSystem を Window より長生きさせ、Platform Event と Native Window の所有順を固定する
    std::unique_ptr<cue::WindowSystem> windowSystem = std::move(*systemResult.try_value());
    cue::WindowDescriptor descriptor = {a_options.title, a_options.clientSize};
    cue::Result<std::unique_ptr<cue::Window>> windowResult = windowSystem->create_window(descriptor);

    if (!windowResult)
    {
        return report_error(a_logger, "Runtime Host failed to create Window", std::move(*windowResult.try_error()),
                            k_windowCreationFailed);
    }

    std::unique_ptr<cue::Window> window = std::move(*windowResult.try_value());

    if (a_options.isPresentationSmoke)
    {
        // Presentation Smoke は表示や Main Loop を省き、Presentation Resource 一式の生成と終了順を検証する
        const int presentationResult = run_presentation_smoke(a_options, *window, a_logger, a_assertContext);
        cue::Result<void> destroyResult = window->destroy();

        if (!destroyResult)
        {
            return report_error(a_logger, "Runtime Host failed to destroy Window",
                                std::move(*destroyResult.try_error()), k_windowDestroyFailed);
        }

        window.reset();
        windowSystem.reset();
        return presentationResult;
    }

    cue::Result<void> showResult = window->show();

    if (!showResult)
    {
        return report_error(a_logger, "Runtime Host failed to show Window", std::move(*showResult.try_error()),
                            k_windowShowFailed);
    }

    if (!a_options.isSmokeTest || a_options.isPackageRuntime)
    {
        // 通常実行、Rendering系Smoke、Package実行は同じClear／Present経路と停止順を通す
        const int renderResult =
            run_render_loop(a_options, *windowSystem, *window, a_logger, a_assertContext, a_startupFactory);
        cue::Result<void> destroyResult = window->destroy();

        if (!destroyResult)
        {
            return report_error(a_logger, "Runtime Host failed to destroy Window",
                                std::move(*destroyResult.try_error()), k_windowDestroyFailed);
        }

        window.reset();
        windowSystem.reset();
        return renderResult;
    }

    cue::Result<std::unique_ptr<cue::runtime_host::RuntimeHostApplication>> applicationResult =
        cue::runtime_host::RuntimeHostApplication::start_fixed_smoke(*window, a_assertContext);
    if (!applicationResult)
    {
        cue::Error applicationError = std::move(*applicationResult.try_error());
        cue::Result<void> destroyResult = window->destroy();
        if (!destroyResult)
        {
            add_secondary_runtime_error(applicationError, *destroyResult.try_error(),
                                        "Window destruction also failed after Runtime start Error", a_assertContext);
        }
        window.reset();
        windowSystem.reset();
        return report_error(a_logger, "Runtime Host failed to start Runtime Application", std::move(applicationError),
                            k_runtimeApplicationCreationFailed);
    }

    std::unique_ptr<cue::runtime_host::RuntimeHostApplication> application = std::move(*applicationResult.try_value());
    const cue::LogResult runtimeReadyLogResult =
        a_logger.log(cue::LogLevel::Info,
                     "Runtime Application Session started: Generation=" + std::to_string(application->generation()) +
                         ", WorldId=" + std::to_string(application->world_id()));
    const cue::LogResult loopReadyLogResult = a_logger.log(cue::LogLevel::Info, "Runtime Host Main Loop started");
    cue::LogResult runtimeShutdownLogResult = cue::LogResult::Success;

    bool isShutdownRequested = false;
    bool wasWindowCloseRequested = false;
    std::optional<cue::Error> hostError;
    std::string_view hostErrorMessage = "Runtime Host Message Pump failed";
    int hostErrorExitCode = k_messagePumpFailed;

#if defined(CUE_RUNTIME_RESIZE_SMOKE_SUPPORT) && CUE_RUNTIME_RESIZE_SMOKE_SUPPORT
    // Test Buildでは実WindowへWM_CLOSEを発行し、Window EventからRuntime停止へ到達する経路を検証する
    if (a_options.isSmokeTest)
    {
        cue::Result<void> closeResult = hostError
                                            ? cue::Result<void>::success()
                                            : cue::issue_windows_window_lifecycle_probe_action(
                                                  *window, cue::WindowsWindowLifecycleProbeAction::ResizeThenClose,
                                                  a_options.clientSize, a_options.clientSize, a_assertContext);
        if (!closeResult)
        {
            hostError.emplace(std::move(*closeResult.try_error()));
            hostErrorMessage = "Runtime Host Window Close probe failed";
            isShutdownRequested = true;
        }
    }
#else
    // Production BuildのSmokeはTest専用Native操作へ依存せず、明示要求として同じ停止順だけを通す
    if (a_options.isSmokeTest)
    {
        isShutdownRequested = true;
    }
#endif

    while (true)
    {
        cue::Result<cue::PumpStatus> pumpResult =
            hostError ? cue::Result<cue::PumpStatus>::success(cue::PumpStatus::Running) : windowSystem->pump_events();

        if (!pumpResult)
        {
            hostError.emplace(std::move(*pumpResult.try_error()));
            isShutdownRequested = true;
        }

        bool isQuitRequested = pumpResult && *pumpResult.try_value() == cue::PumpStatus::QuitRequested;
        bool hasEvent = false;
        cue::WindowEvent event = {};

        while (window->try_pop_event(event))
        {
            hasEvent = true;

            if (event.type == cue::WindowEventType::CloseRequested || event.type == cue::WindowEventType::Destroyed)
            {
                isShutdownRequested = true;
                wasWindowCloseRequested = true;
            }
        }

        if (isQuitRequested)
        {
            isShutdownRequested = true;
            wasWindowCloseRequested = true;
        }

        if (!isShutdownRequested && application != nullptr)
        {
            cue::Result<void> frame = application->advance_frame();
            if (!frame)
            {
                hostError.emplace(std::move(*frame.try_error()));
                hostErrorMessage = "Runtime Application frame failed";
                hostErrorExitCode = k_runtimeApplicationFrameFailed;
                isShutdownRequested = true;
            }
        }

        if (isShutdownRequested && application != nullptr)
        {
            const cue::runtime::RuntimeApplicationStopReason stopReason =
                hostError ? cue::runtime::RuntimeApplicationStopReason::HostFailure
                          : (wasWindowCloseRequested ? cue::runtime::RuntimeApplicationStopReason::WindowClosed
                                                     : cue::runtime::RuntimeApplicationStopReason::Requested);
            cue::Result<void> stopped = application->stop(stopReason);
            if (!stopped && !application->is_cleanup_complete())
            {
                cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                                  "Runtime Host could not prove safe Runtime Application shutdown",
                                  std::move(*stopped.try_error()));
            }
            if (!stopped)
            {
                if (hostError)
                {
                    add_secondary_runtime_error(*hostError, *stopped.try_error(),
                                                "Runtime Application shutdown also reported an Error", a_assertContext);
                }
                else
                {
                    hostError.emplace(std::move(*stopped.try_error()));
                    hostErrorMessage = "Runtime Host failed to shutdown Runtime Application";
                    hostErrorExitCode = k_runtimeApplicationShutdownFailed;
                }
            }

            runtimeShutdownLogResult = a_logger.log(
                cue::LogLevel::Info, "Runtime Application Session stopped: Reason=" +
                                         std::string(describe_runtime_stop_reason(application->stop_reason())) +
                                         ", FrameCount=" + std::to_string(application->frame_count()));
            application.reset();
        }

        bool didDestroy = false;

        if (isShutdownRequested && window->state() != cue::WindowState::Destroyed)
        {
            // Destroy 後の Quit Event まで Pump を続け、Platform 側の Window 終了完了を確認する
            cue::Result<void> destroyResult = window->destroy();

            if (!destroyResult)
            {
                if (hostError)
                {
                    add_secondary_runtime_error(*hostError, *destroyResult.try_error(),
                                                "Window destruction also failed during Runtime shutdown",
                                                a_assertContext);
                }
                else
                {
                    hostError.emplace(std::move(*destroyResult.try_error()));
                    hostErrorMessage = "Runtime Host failed to destroy Window";
                    hostErrorExitCode = k_windowDestroyFailed;
                }
                break;
            }

            didDestroy = true;
        }

        if (hostError)
        {
            break;
        }

        if (isQuitRequested && window->state() == cue::WindowState::Destroyed && !didDestroy)
        {
            break;
        }

        if (!hasEvent && !isShutdownRequested)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    window.reset();
    windowSystem.reset();
    if (hostError)
    {
        return report_error(a_logger, hostErrorMessage, std::move(*hostError), hostErrorExitCode);
    }

    const cue::LogResult shutdownLogResult = a_logger.log(cue::LogLevel::Info, "Runtime Host shutdown completed");
    const cue::LogResult flushResult = a_logger.flush();
    return runtimeReadyLogResult == cue::LogResult::Success && loopReadyLogResult == cue::LogResult::Success &&
                   runtimeShutdownLogResult == cue::LogResult::Success &&
                   shutdownLogResult == cue::LogResult::Success && flushResult == cue::LogResult::Success
               ? 0
               : k_graphicsLogFailed;
}
} // namespace

namespace cue::runtime_host
{
/// @brief Runtime Host の依存を構築し、Command Line 解析から終了 Code 返却までを統括する
int run_runtime_host_process(int a_argumentCount, wchar_t **a_arguments,
                             const RuntimeHostProcessDescriptor &a_descriptor) noexcept
{
    // FatalHandler を最外側に置き、Logger と AssertContext の構築から破棄まで異常終了先を存続させる
    cue::AbortFatalHandler fatalHandler;

    try
    {
        std::vector<std::unique_ptr<cue::LogSink>> sinks;
        sinks.push_back(std::make_unique<cue::ConsoleLogSink>());
        cue::Logger logger(fatalHandler, std::move(sinks));

        // AssertContext を Logger より後に構築し、逆順破棄でも診断先が先に失われないようにする
        cue::AssertContext assertContext(logger, fatalHandler);
        if (a_descriptor.startupFactory == nullptr)
        {
            cue::ErrorCode code = cue::ErrorCode::create(
                fatalHandler, "Cue.RuntimeHost", k_runtimeApplicationCreationFailed);
            return report_error(
                logger, "Runtime Host startup provider is missing",
                cue::Error::create(fatalHandler, std::move(code),
                                   "Runtime Host Process Descriptor requires a Startup Factory"),
                k_runtimeApplicationCreationFailed);
        }
        RuntimeOptions options;
        cue::Result<bool> optionsResult = parse_options(a_argumentCount, a_arguments, options, assertContext);

        if (!optionsResult)
        {
            return report_error(logger, "Runtime Host failed to convert command line",
                                std::move(*optionsResult.try_error()), k_invalidArguments);
        }

        if (!*optionsResult.try_value())
        {
            print_usage();
            return k_invalidArguments;
        }

        if (a_descriptor.startGameModuleByDefault)
        {
            options.isPackageRuntime = true;
        }

        return run(options, logger, assertContext, a_descriptor.startupFactory);
    }
    catch (...)
    {
        fatalHandler.terminate("Runtime Host allocation failed");
    }
}
} // namespace cue::runtime_host
