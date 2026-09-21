// D3D12 Backend の Factory、Adapter、Device、Queue と Window ごとの Presentation Resource を統括する
// 生成、Frame 処理、Resize、Device Removal、Shutdown の依存順を統括し、各所有型の寿命管理を接続する

#include <Cue/RHI/D3D12/D3d12Backend.h>
#include <Cue/RHI/D3D12/TestSupport/D3d12SwapChainProbe.h>

#include "D3d12AdapterSelection.h"
#include "D3d12CapabilityQuery.h"
#include "D3d12DeviceCreation.h"
#include "D3d12Diagnostics.h"
#include "D3d12Error.h"
#include "D3d12FrameCommandState.h"
#include "D3d12QueueState.h"
#include "D3d12RtvHeap.h"
#include "D3d12ScenePass.h"
#include "D3d12SwapChainState.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
using cue::d3d12_private::make_error;
using cue::d3d12_private::make_native_error;

constexpr std::int64_t k_deviceNameFailed = 31;
constexpr std::int64_t k_backendUnavailable = 33;
constexpr std::int64_t k_deviceRemoved = 34;
constexpr std::int64_t k_invalidWaitTimeout = 35;
constexpr std::int64_t k_activePresentationContexts = 87;
constexpr std::int64_t k_presentationUnavailable = 88;
constexpr std::int64_t k_deviceRemovalProbeUnavailable = 89;
constexpr std::int64_t k_backBufferRtvMismatch = 91;
constexpr std::int64_t k_backBufferRtvCreationFailed = 92;
constexpr std::int64_t k_resizeNotAtFrameBoundary = 93;
constexpr std::int64_t k_invalidSceneFrame = 308;
constexpr std::uint32_t k_maximumWaitTimeoutMilliseconds = 60000;

struct RtvCreationProbeState final
{
    bool isRebuildFailureEnabled;
    std::uint32_t callCount;
};

thread_local RtvCreationProbeState g_rtvCreationProbeState = {};

struct QueueLifecycleProbeState final
{
    bool useProbeFunctions;
    bool failSignalAfterForwarding;
    bool failWaitWithoutCompletion;
    bool removeDeviceBeforeSignal;
    bool failWaitAfterCompletion;
    std::uint32_t hiddenCompletedValueCount;
};

thread_local QueueLifecycleProbeState g_queueLifecycleProbeState = {};

struct PresentationFrameProbeState final
{
    bool useProbeFunctions;
    bool failPresent;
    bool removeDeviceBeforePresent;
    bool failCloseWithDeviceRemoval;
    std::uint32_t closeCommandListCallCount;
};

thread_local PresentationFrameProbeState g_presentationFrameProbeState = {};
thread_local bool g_deviceRemovalProbeUnavailable = false;
thread_local bool g_reportDeviceRemovedForProbe = false;
thread_local ID3D12CommandQueue *g_lifecycleProbeQueue = nullptr;

/// @brief 合成 Device Removal を通知する前に Native Fence の実完了を待機する
HRESULT wait_for_native_fence_completion_for_probe(ID3D12Fence *a_fence, std::uint64_t a_value) noexcept
{
    const cue::D3d12QueueNativeFunctions &functions = cue::default_d3d12_queue_native_functions();

    if (functions.getCompletedValue(a_fence) >= a_value)
    {
        return S_OK;
    }

    HANDLE completionEvent = functions.createEvent(nullptr, FALSE, FALSE, nullptr);

    if (completionEvent == nullptr)
    {
        return HRESULT_FROM_WIN32(functions.getLastError());
    }

    HRESULT result = functions.setEventOnCompletion(a_fence, a_value, completionEvent);

    if (SUCCEEDED(result))
    {
        const DWORD waitResult = functions.waitForSingleObject(completionEvent, k_maximumWaitTimeoutMilliseconds);

        if (waitResult == WAIT_TIMEOUT)
        {
            result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        else if (waitResult != WAIT_OBJECT_0)
        {
            result = HRESULT_FROM_WIN32(functions.getLastError());
        }
    }

    if (!functions.closeHandle(completionEvent) && SUCCEEDED(result))
    {
        result = HRESULT_FROM_WIN32(functions.getLastError());
    }

    return result;
}

/// @brief 合成 Device Removal 後の解放に備えて Probe Queue の全実Workを完了させる
HRESULT wait_for_native_queue_idle_for_probe(ID3D12CommandQueue *a_queue) noexcept
{
    if (a_queue == nullptr)
    {
        return E_POINTER;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    HRESULT result = a_queue->GetDevice(IID_PPV_ARGS(device.ReleaseAndGetAddressOf()));

    if (FAILED(result))
    {
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D12Fence> completionFence;
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completionFence.ReleaseAndGetAddressOf()));

    if (FAILED(result))
    {
        return result;
    }

    constexpr std::uint64_t completionValue = 1;
    result = cue::default_d3d12_queue_native_functions().signal(a_queue, completionFence.Get(), completionValue);

    if (FAILED(result))
    {
        return result;
    }

    return wait_for_native_fence_completion_for_probe(completionFence.Get(), completionValue);
}

/// @brief D3D12 Backend の Command Lists For Lifecycle Probe を GPU 実行順と Resource State を守って投入する
void execute_command_lists_for_lifecycle_probe(ID3D12CommandQueue *a_queue, UINT a_count,
                                               ID3D12CommandList *const *a_lists) noexcept
{
    g_lifecycleProbeQueue = a_queue;
    cue::default_d3d12_queue_native_functions().executeCommandLists(a_queue, a_count, a_lists);
}

/// @brief D3D12 Backend の For Lifecycle Probe へ完了通知を発行し、追跡する Fence 値を確定する
HRESULT signal_for_lifecycle_probe(ID3D12CommandQueue *a_queue, ID3D12Fence *a_fence, std::uint64_t a_value) noexcept
{
    if (g_queueLifecycleProbeState.removeDeviceBeforeSignal)
    {
        // Hosted Runner の WARP Device を実際に破棄すると Process 外で Access Violation になる場合があるため、
        // Fence は完了可能にした上で Production と同じ Device Removed 分類を通す Native 結果を注入する。
        const HRESULT signalResult = cue::default_d3d12_queue_native_functions().signal(a_queue, a_fence, a_value);

        if (FAILED(signalResult))
        {
            return signalResult;
        }

        const HRESULT completionResult = wait_for_native_fence_completion_for_probe(a_fence, a_value);

        if (FAILED(completionResult))
        {
            g_deviceRemovalProbeUnavailable = true;
            return completionResult;
        }

        g_reportDeviceRemovedForProbe = true;
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    const HRESULT result = cue::default_d3d12_queue_native_functions().signal(a_queue, a_fence, a_value);
    return SUCCEEDED(result) && g_queueLifecycleProbeState.failSignalAfterForwarding ? E_FAIL : result;
}

/// @brief Lifecycle Probe が使用する Fence 完了値を Native Queue 経路へ返す
std::uint64_t completed_value_for_lifecycle_probe(ID3D12Fence *a_fence) noexcept
{
    if (g_reportDeviceRemovedForProbe)
    {
        return std::numeric_limits<std::uint64_t>::max();
    }

    if (g_queueLifecycleProbeState.hiddenCompletedValueCount > 0)
    {
        --g_queueLifecycleProbeState.hiddenCompletedValueCount;
        return 0;
    }

    if (g_queueLifecycleProbeState.failWaitWithoutCompletion)
    {
        return 0;
    }

    if (g_queueLifecycleProbeState.failWaitAfterCompletion)
    {
        for (std::uint32_t attempt = 0; attempt < 100; ++attempt)
        {
            const std::uint64_t completedValue = cue::default_d3d12_queue_native_functions().getCompletedValue(a_fence);

            if (completedValue != 0)
            {
                return completedValue;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    return cue::default_d3d12_queue_native_functions().getCompletedValue(a_fence);
}

/// @brief D3D12 Backend の Event For Lifecycle Probe を整合性を保って更新する
HRESULT set_event_for_lifecycle_probe(ID3D12Fence *a_fence, std::uint64_t a_value, HANDLE a_event) noexcept
{
    return cue::default_d3d12_queue_native_functions().setEventOnCompletion(a_fence, a_value, a_event);
}

/// @brief D3D12 Backend の For Lifecycle Probe 完了を待機し、後続処理を安全に進められる状態を返す
DWORD WINAPI wait_for_lifecycle_probe(HANDLE a_event, DWORD a_timeout)
{
    if (g_queueLifecycleProbeState.failWaitWithoutCompletion || g_queueLifecycleProbeState.failWaitAfterCompletion)
    {
        return WAIT_TIMEOUT;
    }

    return cue::default_d3d12_queue_native_functions().waitForSingleObject(a_event, a_timeout);
}

/// @brief D3D12 Backend が保持する Get Device Removed Reason For Lifecycle Probe を呼び出し元へ返す
HRESULT get_device_removed_reason_for_lifecycle_probe(ID3D12Device *a_device) noexcept
{
    return g_reportDeviceRemovedForProbe ? DXGI_ERROR_DEVICE_REMOVED
                                         : cue::default_d3d12_queue_native_functions().getDeviceRemovedReason(a_device);
}

/// @brief D3D12 Backend で使用する Queue Lifecycle Probe Functions を生成し、呼び出し元へ返す
[[nodiscard]] cue::D3d12QueueNativeFunctions make_queue_lifecycle_probe_functions() noexcept
{
    cue::D3d12QueueNativeFunctions functions = cue::default_d3d12_queue_native_functions();
    functions.executeCommandLists = execute_command_lists_for_lifecycle_probe;
    functions.signal = signal_for_lifecycle_probe;
    functions.getCompletedValue = completed_value_for_lifecycle_probe;
    functions.setEventOnCompletion = set_event_for_lifecycle_probe;
    functions.waitForSingleObject = wait_for_lifecycle_probe;
    functions.getDeviceRemovedReason = get_device_removed_reason_for_lifecycle_probe;
    return functions;
}

/// @brief D3D12 Backend の Command List For Lifecycle Probe を依存関係と完了条件を守って安全に解放または停止する
HRESULT close_command_list_for_lifecycle_probe(ID3D12GraphicsCommandList *a_commandList) noexcept
{
    ++g_presentationFrameProbeState.closeCommandListCallCount;

    if (g_presentationFrameProbeState.failCloseWithDeviceRemoval &&
        g_presentationFrameProbeState.closeCommandListCallCount > 1)
    {
        g_reportDeviceRemovedForProbe = true;
        return E_FAIL;
    }

    return cue::default_d3d12_frame_command_native_functions().closeCommandList(a_commandList);
}

/// @brief D3D12 Backend で使用する Frame Command Lifecycle Probe Functions を生成し、呼び出し元へ返す
[[nodiscard]] cue::D3d12FrameCommandNativeFunctions make_frame_command_lifecycle_probe_functions() noexcept
{
    cue::D3d12FrameCommandNativeFunctions functions = cue::default_d3d12_frame_command_native_functions();
    functions.closeCommandList = close_command_list_for_lifecycle_probe;
    return functions;
}

/// @brief D3D12 Backend の For Lifecycle Probe を GPU 実行順と Resource State を守って投入する
HRESULT present_for_lifecycle_probe(IDXGISwapChain3 *a_swapChain, UINT a_syncInterval, UINT a_flags) noexcept
{
    if (g_presentationFrameProbeState.removeDeviceBeforePresent)
    {
        const HRESULT presentResult =
            cue::default_d3d12_swap_chain_native_functions().present(a_swapChain, a_syncInterval, a_flags);

        if (FAILED(presentResult))
        {
            return presentResult;
        }

        const HRESULT completionResult = wait_for_native_queue_idle_for_probe(g_lifecycleProbeQueue);

        if (FAILED(completionResult))
        {
            g_deviceRemovalProbeUnavailable = true;
            return completionResult;
        }

        // Present 自体は実行し、直後の Device Removed 応答だけを注入して Driver 依存の Device 破壊を避ける。
        g_reportDeviceRemovedForProbe = true;
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    if (g_presentationFrameProbeState.failPresent)
    {
        if (g_queueLifecycleProbeState.removeDeviceBeforeSignal)
        {
            const HRESULT presentResult =
                cue::default_d3d12_swap_chain_native_functions().present(a_swapChain, a_syncInterval, a_flags);

            if (FAILED(presentResult))
            {
                return presentResult;
            }
        }

        return E_FAIL;
    }

    return cue::default_d3d12_swap_chain_native_functions().present(a_swapChain, a_syncInterval, a_flags);
}

/// @brief D3D12 Backend で使用する Swap Chain Lifecycle Probe Functions を生成し、呼び出し元へ返す
[[nodiscard]] cue::D3d12SwapChainNativeFunctions make_swap_chain_lifecycle_probe_functions() noexcept
{
    cue::D3d12SwapChainNativeFunctions functions = cue::default_d3d12_swap_chain_native_functions();
    functions.present = present_for_lifecycle_probe;
    return functions;
}

/// @brief Allocation 失敗を追加 Allocation なしで Fatal 終了境界へ渡し、復帰時も Process を停止する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("D3D12 Backend allocation failed");
    std::abort();
}

/// @brief 主因 Error を失わず Secondary Error の識別情報を Context へ追加する
void add_secondary_error_context(cue::Error &a_primaryError, const cue::Error &a_secondaryError,
                                 std::string_view a_context, const cue::AssertContext &a_assertContext,
                                 std::source_location a_location = std::source_location::current()) noexcept
{
    a_primaryError.append_secondary_diagnostics(a_assertContext, a_secondaryError, a_context,
                                                "Secondary shutdown Error", a_location);
}

/// @brief Error に指定された診断 Context が含まれるかを判定する
[[nodiscard]] bool has_error_context(const cue::Error &a_error, std::string_view a_expected) noexcept
{
    for (const cue::ErrorContext &context : a_error.contexts())
    {
        if (context.message().find(a_expected) != std::string_view::npos)
        {
            return true;
        }
    }

    return false;
}

/// @brief D3D12 Backend の Error Contexts In Order 条件を判定して返す
[[nodiscard]] bool has_error_contexts_in_order(const cue::Error &a_error, std::string_view a_first,
                                               std::string_view a_second, std::string_view a_third = {}) noexcept
{
    std::uint32_t expectedIndex = 0;

    for (const cue::ErrorContext &context : a_error.contexts())
    {
        const std::string_view message = context.message();

        if (expectedIndex == 0 && message.find(a_first) != std::string_view::npos)
        {
            expectedIndex = 1;
        }
        else if (expectedIndex == 1 && message.find(a_second) != std::string_view::npos)
        {
            expectedIndex = a_third.empty() ? 3 : 2;
        }
        else if (expectedIndex == 2 && message.find(a_third) != std::string_view::npos)
        {
            expectedIndex = 3;
        }
    }

    return expectedIndex == 3;
}

/// @brief D3D12 Backend で使用する Device Removed Error With Present Cause を生成し、呼び出し元へ返す
[[nodiscard]] cue::Error make_device_removed_error_with_present_cause(
    cue::Error &&a_deviceRemovedError, cue::Error &&a_presentError, const cue::AssertContext &a_assertContext) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), a_deviceRemovedError.code().domain(),
                                                 a_deviceRemovedError.code().value());
    const cue::NativeError *nativeError = a_deviceRemovedError.try_native_error();
    cue::Error reorderedError =
        nativeError != nullptr
            ? cue::Error::reclassify(a_assertContext.fatal_handler(), std::move(code), a_deviceRemovedError.summary(),
                                     cue::NativeError::create(a_assertContext.fatal_handler(), nativeError->domain(),
                                                              nativeError->value()),
                                     std::move(a_presentError))
            : cue::Error::reclassify(a_assertContext.fatal_handler(), std::move(code), a_deviceRemovedError.summary(),
                                     std::move(a_presentError));
    add_secondary_error_context(reorderedError, a_deviceRemovedError, "D3D12 recovery Signal failed after DXGI Present",
                                a_assertContext);
    return reorderedError;
}

/// @brief D3D12 Backend の Shutdown Error を診断と Lifecycle の規則に従って更新する
void retain_shutdown_error(std::optional<cue::Error> &a_firstError, cue::Result<void> &a_result,
                           std::string_view a_context, const cue::AssertContext &a_assertContext,
                           std::source_location a_location = std::source_location::current()) noexcept
{
    if (a_result)
    {
        return;
    }

    cue::Error *error = a_result.try_error();

    if (!a_firstError)
    {
        a_firstError.emplace(std::move(*error));
        return;
    }

    add_secondary_error_context(*a_firstError, *error, a_context, a_assertContext, a_location);
}

using UnregisterPresentation = void (*)(void *) noexcept;

#if CUE_D3D12_TESTING
struct SceneOwnerProbeReport final
{
    bool hasDepthDsv;
    bool hasGeometry;
    bool hasConstants;
};
#endif

struct PresentationCleanupOwnerShape final
{
    std::uint32_t allocatorCount;
    std::uint32_t backBufferCount;
    std::uint32_t rtvCount;
    bool hasCommandList;
    bool hasSwapChain;
    bool hasRtvHeap;
#if CUE_D3D12_TESTING
    bool hasSceneDepthDsv;
    bool hasSceneGeometry;
    bool hasSceneConstants;
#endif
};

using PreparePresentationCleanup = cue::Result<void> (*)(void *, const PresentationCleanupOwnerShape &) noexcept;
using ClassifyPresentationNativeFailure = cue::Result<void> (*)(void *, cue::Error &&) noexcept;

/// @brief D3D12 Backend で使用する Render Target View を生成し、呼び出し元へ返す
[[nodiscard]] HRESULT create_render_target_view(ID3D12Device *a_device, ID3D12Resource *a_resource,
                                                const D3D12_RENDER_TARGET_VIEW_DESC *a_descriptor,
                                                D3D12_CPU_DESCRIPTOR_HANDLE a_handle) noexcept
{
    ++g_rtvCreationProbeState.callCount;

    if (g_rtvCreationProbeState.isRebuildFailureEnabled &&
        g_rtvCreationProbeState.callCount > cue::k_d3d12SwapChainBufferCount)
    {
        return E_FAIL;
    }

    a_device->CreateRenderTargetView(a_resource, a_descriptor, a_handle);
    return S_OK;
}

/// @brief D3D12 Backend の Swap Chain Back Buffers を所有権と Lifecycle 規則を守って関連付ける
[[nodiscard]] cue::Result<void> bind_swap_chain_back_buffers(cue::D3d12SwapChainState &a_swapChain,
                                                             cue::D3d12FrameCommandState &a_frameState) noexcept
{
    cue::Result<cue::D3d12SwapChainBackBuffers> backBuffersResult = a_swapChain.take_back_buffers();

    if (!backBuffersResult)
    {
        return cue::Result<void>::failure(std::move(*backBuffersResult.try_error()));
    }

    return a_frameState.bind_back_buffers(std::move(*backBuffersResult.try_value()));
}

/// @brief D3D12 Backend で使用する Back Buffer Rtvs を生成し、呼び出し元へ返す
[[nodiscard]] cue::Result<void> create_back_buffer_rtvs(ID3D12Device *a_device,
                                                        cue::D3d12FrameCommandState &a_frameState, DXGI_FORMAT a_format,
                                                        cue::D3d12RtvHeap &a_heap,
                                                        const cue::AssertContext &a_assertContext) noexcept
{
    CUE_ASSERT(a_assertContext, a_device != nullptr, "D3D12 Back Buffer RTV creation requires a Device");
    CUE_ASSERT(a_assertContext, a_heap.used_count() == 0, "D3D12 Back Buffer RTV creation requires an empty Heap");

    for (std::uint32_t index = 0; index < cue::k_d3d12SwapChainBufferCount; ++index)
    {
        cue::Result<ID3D12Resource *> backBufferResult = a_frameState.back_buffer(index);

        if (!backBufferResult)
        {
            static_cast<void>(a_frameState.release_rtv_slots(a_heap));
            return cue::Result<void>::failure(std::move(*backBufferResult.try_error()));
        }

        ID3D12Resource *backBuffer = *backBufferResult.try_value();
        const D3D12_RESOURCE_DESC resourceDescriptor = backBuffer->GetDesc();

        if (resourceDescriptor.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || resourceDescriptor.Format != a_format)
        {
            static_cast<void>(a_frameState.release_rtv_slots(a_heap));
            return cue::Result<void>::failure(
                make_error(a_assertContext, k_backBufferRtvMismatch, "D3D12 Back Buffer and RTV Format do not match"));
        }

        cue::Result<cue::D3d12RtvSlot> slotResult = a_heap.allocate();

        if (!slotResult)
        {
            static_cast<void>(a_frameState.release_rtv_slots(a_heap));
            return cue::Result<void>::failure(std::move(*slotResult.try_error()));
        }

        cue::D3d12RtvSlot slot = *slotResult.try_value();
        cue::Result<D3D12_CPU_DESCRIPTOR_HANDLE> handleResult = a_heap.cpu_handle(slot);

        if (!handleResult)
        {
            static_cast<void>(a_heap.release(slot));
            static_cast<void>(a_frameState.release_rtv_slots(a_heap));
            return cue::Result<void>::failure(std::move(*handleResult.try_error()));
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtvDescriptor = {};
        rtvDescriptor.Format = a_format;
        rtvDescriptor.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDescriptor.Texture2D.MipSlice = 0;
        rtvDescriptor.Texture2D.PlaneSlice = 0;
        const HRESULT creationResult =
            create_render_target_view(a_device, backBuffer, &rtvDescriptor, *handleResult.try_value());

        if (FAILED(creationResult))
        {
            static_cast<void>(a_heap.release(slot));
            static_cast<void>(a_frameState.release_rtv_slots(a_heap));
            return cue::Result<void>::failure(make_native_error(a_assertContext, k_backBufferRtvCreationFailed,
                                                                "D3D12 Back Buffer RTV creation failed",
                                                                creationResult));
        }

        cue::Result<void> bindingResult = a_frameState.bind_rtv_slot(index, slot);

        if (!bindingResult)
        {
            static_cast<void>(a_heap.release(slot));
            static_cast<void>(a_frameState.release_rtv_slots(a_heap));
            return bindingResult;
        }
    }

    return cue::Result<void>::success();
}

#if CUE_D3D12_TESTING
struct D3d12ScenePixelCapture final
{
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};
#endif

// Window 固有の Swap Chain、Back Buffer、RTV、Frame Command を所有する Presentation 実装
// Backend の Device と Queue を借用するため、必ず Backend より先に Shutdown して登録を解除する
class D3d12PresentationContext final : public cue::PresentationContext
{
  public:
    /// @brief D3d12PresentationContext を必要な依存と初期状態から構築する
    D3d12PresentationContext(cue::D3d12SwapChainState &&a_swapChain, cue::D3d12FrameCommandState &&a_frameCommandState,
                             cue::D3d12RtvHeap &&a_rtvHeap, ID3D12Device *a_device, cue::GraphicsBackend &a_backend,
                             void *a_backendOwner, UnregisterPresentation a_unregisterPresentation,
                             PreparePresentationCleanup a_preparePresentationCleanup,
                             ClassifyPresentationNativeFailure a_classifyNativeFailure,
                             cue::AssertContext &a_assertContext) noexcept
        : m_swapChain(std::move(a_swapChain)), m_frameCommandState(std::move(a_frameCommandState)),
          m_rtvHeap(std::move(a_rtvHeap)), m_device(a_device), m_backend(&a_backend), m_backendOwner(a_backendOwner),
          m_unregisterPresentation(a_unregisterPresentation),
          m_preparePresentationCleanup(a_preparePresentationCleanup), m_classifyNativeFailure(a_classifyNativeFailure),
          m_assertContext(&a_assertContext), m_creationThread(std::this_thread::get_id()),
          m_state(cue::PresentationContextState::Ready), m_isRegistered(true), m_isResizePending(false)
    {
    }

    /// @brief D3d12PresentationContext が保持する Resource を所有権規則に従って破棄する
    ~D3d12PresentationContext() noexcept override
    {
        CUE_ASSERT(*m_assertContext, std::this_thread::get_id() == m_creationThread,
                   "D3D12 Presentation Context must be destroyed on the creation thread");

        if (m_state != cue::PresentationContextState::Shutdown || m_isRegistered ||
            m_frameCommandState.has_native_objects() || m_swapChain.has_native_objects() ||
            m_rtvHeap.has_native_object() || m_scenePass.has_native_objects())
        {
            m_assertContext->fatal_handler().terminate(
                "D3D12 Presentation Context owner was destroyed before shutdown");
        }
    }

    /// @brief D3D12 Backend が保持する State を呼び出し元へ返す
    [[nodiscard]] cue::PresentationContextState state() const noexcept override
    {
        assert_thread("D3D12 Presentation state must be queried on the creation thread");
        return m_state;
    }

    /// @brief D3D12 Backend が保持する Width を呼び出し元へ返す
    [[nodiscard]] std::uint32_t width() const noexcept override
    {
        assert_thread("D3D12 Presentation width must be queried on the creation thread");
        return m_swapChain.width();
    }

    /// @brief D3D12 Backend が保持する Height を呼び出し元へ返す
    [[nodiscard]] std::uint32_t height() const noexcept override
    {
        assert_thread("D3D12 Presentation height must be queried on the creation thread");
        return m_swapChain.height();
    }

    /// @brief D3D12 Backend が保持する Buffer Count を呼び出し元へ返す
    [[nodiscard]] std::uint32_t buffer_count() const noexcept override
    {
        assert_thread("D3D12 Presentation buffer count must be queried on the creation thread");
        return m_swapChain.buffer_count();
    }

    /// @brief D3D12 Backend が保持する Current Back Buffer Index を呼び出し元へ返す
    [[nodiscard]] std::uint32_t current_back_buffer_index() const noexcept override
    {
        assert_thread("D3D12 Presentation index must be queried on the creation thread");
        return m_swapChain.current_back_buffer_index();
    }

    /// @brief D3D12 Backend の VSync Enabled 条件を判定して返す
    [[nodiscard]] bool is_vsync_enabled() const noexcept override
    {
        assert_thread("D3D12 Presentation VSync state must be queried on the creation thread");
        return m_swapChain.is_vsync_enabled();
    }

    /// @brief D3D12 Backend の Tearing Supported 条件を判定して返す
    [[nodiscard]] bool is_tearing_supported() const noexcept override
    {
        assert_thread("D3D12 Presentation tearing capability must be queried on the creation thread");
        return m_swapChain.is_tearing_supported();
    }

    /// @brief D3D12 Backend の Tearing Enabled 条件を判定して返す
    [[nodiscard]] bool is_tearing_enabled() const noexcept override
    {
        assert_thread("D3D12 Presentation tearing state must be queried on the creation thread");
        return m_swapChain.is_tearing_enabled();
    }

    /// @brief D3D12 Backend の Resize Pending 条件を判定して返す
    [[nodiscard]] bool is_resize_pending() const noexcept override
    {
        assert_thread("D3D12 Presentation Resize pending state must be queried on the creation thread");
        return m_isResizePending;
    }

    // Fence 待機から Signal までを一連の Frame 処理として順序付ける
    /// @brief D3D12 Backend の Frame を GPU 実行順と Resource State を守って投入する
    [[nodiscard]] cue::Result<cue::PresentationFrameStatus> present_frame(
        const cue::PresentationFrameDescriptor &a_descriptor) noexcept override
    {
        return present_frame_impl(a_descriptor.clearColor, nullptr);
    }

    /// @brief 検証済みCube入力を既存のFrame同期・Present経路へ追加する
    [[nodiscard]] cue::Result<cue::PresentationFrameStatus> present_scene_frame(
        const cue::PresentationSceneFrameDescriptor &a_descriptor) noexcept override
    {
        assert_thread("D3D12 Scene Frame must run on the creation thread");
        if (a_descriptor.cubes.size() > cue::k_presentationSceneMaxCubeCount ||
            m_swapChain.width() > static_cast<std::uint32_t>((std::numeric_limits<LONG>::max)()) ||
            m_swapChain.height() > static_cast<std::uint32_t>((std::numeric_limits<LONG>::max)()))
        {
            return cue::Result<cue::PresentationFrameStatus>::failure(
                make_error(*m_assertContext, k_invalidSceneFrame, "D3D12 Scene Frame dimensions or count are invalid"));
        }
        for (float channel : a_descriptor.clearColor)
        {
            if (!std::isfinite(channel))
            {
                return cue::Result<cue::PresentationFrameStatus>::failure(
                    make_error(*m_assertContext, k_invalidSceneFrame, "D3D12 Scene Clear Color is not finite"));
            }
        }
        for (float value : a_descriptor.viewProjection)
        {
            if (!std::isfinite(value))
            {
                return cue::Result<cue::PresentationFrameStatus>::failure(
                    make_error(*m_assertContext, k_invalidSceneFrame, "D3D12 Scene Camera Matrix is not finite"));
            }
        }
        for (const cue::PresentationSceneCube &cube : a_descriptor.cubes)
        {
            for (float value : cube.localToWorld)
            {
                if (!std::isfinite(value))
                {
                    return cue::Result<cue::PresentationFrameStatus>::failure(
                        make_error(*m_assertContext, k_invalidSceneFrame, "D3D12 Scene Instance Matrix is not finite"));
                }
            }
        }
        if (m_state != cue::PresentationContextState::Ready || m_backend->state() != cue::GraphicsBackendState::Ready ||
            m_isResizePending)
        {
            return cue::Result<cue::PresentationFrameStatus>::failure(
                make_error(*m_assertContext, k_presentationUnavailable, "D3D12 Scene Frame is unavailable"));
        }
        if (!m_scenePass.has_native_objects())
        {
            cue::Result<void> sceneResult = m_scenePass.initialize(m_device, m_swapChain.format(), m_swapChain.width(),
                                                                   m_swapChain.height(), *m_assertContext);
            if (!sceneResult)
            {
                cue::Result<void> classificationResult =
                    m_classifyNativeFailure(m_backendOwner, std::move(*sceneResult.try_error()));
                CUE_ASSERT(*m_assertContext, !classificationResult,
                           "D3D12 Scene initialization failure classification must retain an Error");
                cue::Error sceneError = std::move(*classificationResult.try_error());
                cue::Result<void> cleanupPreparation = prepare_backend_cleanup();
                if (!cleanupPreparation)
                {
                    add_secondary_error_context(
                        sceneError, *cleanupPreparation.try_error(),
                        "D3D12 Backend cleanup preparation also failed after Scene initialization", *m_assertContext);
                }
                // 初回 Scene はまだ Submit していないため、診断収集後に部分生成 Resource を戻せる
                m_scenePass.release();
                if (m_backend->state() == cue::GraphicsBackendState::DeviceRemoved)
                {
                    m_state = cue::PresentationContextState::DeviceRemoved;
                }
                else if (m_backend->state() == cue::GraphicsBackendState::Unavailable)
                {
                    m_state = cue::PresentationContextState::Unavailable;
                }
                return cue::Result<cue::PresentationFrameStatus>::failure(std::move(sceneError));
            }
        }
        return present_frame_impl(a_descriptor.clearColor, &a_descriptor);
    }

    /// @brief Clear専用と固定Sceneの両方を一つのSubmit／Present／Signal順序で実行する
    [[nodiscard]] cue::Result<cue::PresentationFrameStatus> present_frame_impl(
        const std::array<float, 4> &a_clearColor, const cue::PresentationSceneFrameDescriptor *a_scene) noexcept
    {
        assert_thread("D3D12 Presentation Frame must run on the creation thread");

        if (m_state != cue::PresentationContextState::Ready || m_backend->state() != cue::GraphicsBackendState::Ready ||
            m_isResizePending)
        {
            return cue::Result<cue::PresentationFrameStatus>::failure(
                make_error(*m_assertContext, k_presentationUnavailable, "D3D12 Presentation Frame is unavailable"));
        }

        /// @brief Frame 操作失敗を Device Removal 状態と統合し、最初の失敗を失わずに呼び出し元へ返す
        const auto failFrameOperation = [&](cue::Error &&a_error, std::string_view a_operation) noexcept
        {
            if (m_frameCommandState.status() == cue::D3d12FrameCommandStatus::DeviceRemoved)
            {
                m_state = cue::PresentationContextState::DeviceRemoved;
                cue::Result<void> backendResult = prepare_backend_cleanup();

                if (!backendResult)
                {
                    add_secondary_error_context(a_error, *backendResult.try_error(), a_operation, *m_assertContext);
                }
            }
            else if (m_frameCommandState.status() == cue::D3d12FrameCommandStatus::Unavailable)
            {
                m_state = cue::PresentationContextState::Unavailable;
                cue::Result<void> backendResult = prepare_backend_cleanup();

                if (!backendResult)
                {
                    add_secondary_error_context(a_error, *backendResult.try_error(), a_operation, *m_assertContext);
                }
            }

            return cue::Result<cue::PresentationFrameStatus>::failure(std::move(a_error));
        };

        const std::uint32_t frameIndex = m_swapChain.current_back_buffer_index();
        cue::Result<void> beginResult = m_frameCommandState.begin_frame(frameIndex);

        if (!beginResult)
        {
            return failFrameOperation(std::move(*beginResult.try_error()),
                                      "D3D12 Backend cleanup preparation also failed after Frame reuse Wait");
        }

        cue::Result<void> renderTargetResult =
            m_frameCommandState.transition_back_buffer(frameIndex, cue::D3d12BackBufferState::RenderTarget);

        if (!renderTargetResult)
        {
            return failFrameOperation(std::move(*renderTargetResult.try_error()),
                                      "D3D12 Backend cleanup preparation also failed after Render Target Barrier");
        }

        cue::Result<void> clearResult = m_frameCommandState.clear_back_buffer(frameIndex, m_rtvHeap, a_clearColor);

        if (!clearResult)
        {
            return failFrameOperation(std::move(*clearResult.try_error()),
                                      "D3D12 Backend cleanup preparation also failed after Render Target Clear");
        }

        if (a_scene != nullptr)
        {
            cue::Result<void> sceneResult = m_frameCommandState.record_scene(
                frameIndex, m_rtvHeap, m_scenePass, m_swapChain.width(), m_swapChain.height(), *a_scene);
            if (!sceneResult)
            {
                return failFrameOperation(std::move(*sceneResult.try_error()),
                                          "D3D12 Backend cleanup preparation also failed after Scene Draw");
            }
#if CUE_D3D12_TESTING
            if (m_sceneCaptureForProbe != nullptr &&
                !m_frameCommandState.copy_scene_back_buffer_for_probe(
                    frameIndex, m_sceneCaptureForProbe->readback.Get(), m_sceneCaptureForProbe->footprint))
            {
                return failFrameOperation(make_error(*m_assertContext, k_invalidSceneFrame,
                                                     "D3D12 Scene pixel capture could not be recorded"),
                                          "D3D12 Scene pixel capture also failed");
            }
#endif
        }

        cue::Result<void> presentStateResult =
            m_frameCommandState.transition_back_buffer(frameIndex, cue::D3d12BackBufferState::Present);

        if (!presentStateResult)
        {
            return failFrameOperation(std::move(*presentStateResult.try_error()),
                                      "D3D12 Backend cleanup preparation also failed after Present Barrier");
        }

        cue::Result<void> closeResult = m_frameCommandState.close_frame();

        if (!closeResult)
        {
            return failFrameOperation(std::move(*closeResult.try_error()),
                                      "D3D12 Backend cleanup preparation also failed after Command List Close");
        }

        cue::Result<void> executeResult = m_frameCommandState.execute_frame();

        if (!executeResult)
        {
            return failFrameOperation(std::move(*executeResult.try_error()),
                                      "D3D12 Backend cleanup preparation also failed after Frame Execute");
        }

        cue::Result<void> presentMarkerResult = m_frameCommandState.mark_present_attempted();

        if (!presentMarkerResult)
        {
            return failFrameOperation(std::move(*presentMarkerResult.try_error()),
                                      "D3D12 Backend cleanup preparation also failed after Present marker");
        }

        cue::Result<cue::D3d12PresentStatus> presentResult = m_swapChain.present();

        if (!presentResult)
        {
            cue::Error presentError = std::move(*presentResult.try_error());

            if (m_backend->state() == cue::GraphicsBackendState::DeviceRemoved)
            {
                m_state = cue::PresentationContextState::DeviceRemoved;
                cue::Result<void> frameStopResult = m_frameCommandState.stop_after_device_removal();

                if (!frameStopResult)
                {
                    add_secondary_error_context(presentError, *frameStopResult.try_error(),
                                                "D3D12 Frame acceptance also failed to stop after Device Removal",
                                                *m_assertContext);
                }

                cue::Result<void> backendResult = prepare_backend_cleanup();

                if (!backendResult)
                {
                    add_secondary_error_context(presentError, *backendResult.try_error(),
                                                "D3D12 Device Removal diagnostics also failed after Present",
                                                *m_assertContext);
                }

                return cue::Result<cue::PresentationFrameStatus>::failure(std::move(presentError));
            }

            cue::Result<std::uint64_t> recoverySignalResult =
                m_frameCommandState.signal_frame(cue::D3d12FrameSignalPurpose::PresentFailureRecovery);

            if (!recoverySignalResult)
            {
                cue::Error signalError = std::move(*recoverySignalResult.try_error());

                if (m_frameCommandState.status() == cue::D3d12FrameCommandStatus::DeviceRemoved)
                {
                    m_state = cue::PresentationContextState::DeviceRemoved;
                    cue::Result<void> backendResult = prepare_backend_cleanup();

                    if (!backendResult)
                    {
                        add_secondary_error_context(
                            signalError, *backendResult.try_error(),
                            "D3D12 Device Removal diagnostics also failed after recovery Signal", *m_assertContext);
                    }

                    cue::Error deviceRemovedError = make_device_removed_error_with_present_cause(
                        std::move(signalError), std::move(presentError), *m_assertContext);
                    return cue::Result<cue::PresentationFrameStatus>::failure(std::move(deviceRemovedError));
                }

                if (m_frameCommandState.status() == cue::D3d12FrameCommandStatus::Unavailable)
                {
                    m_state = cue::PresentationContextState::Unavailable;
                    cue::Result<void> backendResult = prepare_backend_cleanup();

                    if (!backendResult)
                    {
                        add_secondary_error_context(signalError, *backendResult.try_error(),
                                                    "D3D12 Backend also became unavailable after recovery Signal",
                                                    *m_assertContext);
                    }
                }

                add_secondary_error_context(presentError, signalError,
                                            "D3D12 recovery Signal also failed after Present", *m_assertContext);
            }

            return cue::Result<cue::PresentationFrameStatus>::failure(std::move(presentError));
        }

        cue::Result<std::uint64_t> signalResult = m_frameCommandState.signal_frame();

        if (!signalResult)
        {
            if (m_frameCommandState.status() == cue::D3d12FrameCommandStatus::DeviceRemoved)
            {
                m_state = cue::PresentationContextState::DeviceRemoved;
                cue::Result<void> backendResult = prepare_backend_cleanup();

                if (!backendResult)
                {
                    add_secondary_error_context(*signalResult.try_error(), *backendResult.try_error(),
                                                "D3D12 Device Removal diagnostics also failed after Frame Signal",
                                                *m_assertContext);
                }
            }
            else if (m_frameCommandState.status() == cue::D3d12FrameCommandStatus::Unavailable)
            {
                m_state = cue::PresentationContextState::Unavailable;
                cue::Result<void> backendResult = prepare_backend_cleanup();

                if (!backendResult)
                {
                    add_secondary_error_context(*signalResult.try_error(), *backendResult.try_error(),
                                                "D3D12 Backend also became unavailable after Frame Signal",
                                                *m_assertContext);
                }
            }

            return cue::Result<cue::PresentationFrameStatus>::failure(std::move(*signalResult.try_error()));
        }

        cue::Result<std::uint32_t> indexResult = m_swapChain.refresh_current_back_buffer_index();

        if (!indexResult)
        {
            cue::Error indexError = std::move(*indexResult.try_error());
            cue::Result<void> stopResult = m_frameCommandState.stop_after_presentation_error();

            if (!stopResult)
            {
                add_secondary_error_context(indexError, *stopResult.try_error(),
                                            "D3D12 Frame acceptance also failed to stop after index refresh",
                                            *m_assertContext);
            }

            return cue::Result<cue::PresentationFrameStatus>::failure(std::move(indexError));
        }

        cue::PresentationFrameStatus status = *presentResult.try_value() == cue::D3d12PresentStatus::Occluded
                                                  ? cue::PresentationFrameStatus::Occluded
                                                  : cue::PresentationFrameStatus::Presented;
        return cue::Result<cue::PresentationFrameStatus>::success(std::move(status));
    }

    // 有効な Size 変更では Frame 受付停止と GPU Idle 証明後に旧 Resource を解放して再構築する
    /// @brief D3D12 Backend を指定 Size へ再構築し、後続処理へ反映する
    [[nodiscard]] cue::Result<void> resize(std::uint32_t a_width, std::uint32_t a_height) noexcept override
    {
        assert_thread("D3D12 Presentation Resize must run on the creation thread");

        if (m_state != cue::PresentationContextState::Ready || m_backend->state() != cue::GraphicsBackendState::Ready)
        {
            return cue::Result<void>::failure(
                make_error(*m_assertContext, k_presentationUnavailable, "D3D12 Presentation Resize is unavailable"));
        }

        const cue::D3d12CommandListState commandListState = m_frameCommandState.command_list_state();

        if (commandListState != cue::D3d12CommandListState::IdleClosed &&
            commandListState != cue::D3d12CommandListState::Submitted)
        {
            return cue::Result<void>::failure(make_error(*m_assertContext, k_resizeNotAtFrameBoundary,
                                                         "D3D12 Presentation Resize requires a Frame boundary"));
        }

        if (a_width == 0 || a_height == 0)
        {
            if (m_isResizePending)
            {
                return cue::Result<void>::success();
            }

            cue::Result<void> suspendResult = m_frameCommandState.suspend_for_resize();

            if (!suspendResult)
            {
                return suspendResult;
            }

            m_isResizePending = true;
            return cue::Result<void>::success();
        }

        if (a_width == m_swapChain.width() && a_height == m_swapChain.height())
        {
            if (!m_isResizePending && !m_frameCommandState.is_accepting_frames())
            {
                return cue::Result<void>::failure(
                    make_error(*m_assertContext, k_presentationUnavailable,
                               "D3D12 Presentation Frame acceptance was stopped before same-size Resize"));
            }

            if (m_isResizePending)
            {
                cue::Result<void> resumeResult = m_frameCommandState.resume_after_resize();

                if (!resumeResult)
                {
                    return resumeResult;
                }
            }

            m_isResizePending = false;
            return cue::Result<void>::success();
        }

        cue::Result<void> prepareResult =
            m_frameCommandState.prepare_for_resize(m_swapChain.current_back_buffer_index());

        if (!prepareResult)
        {
            cue::Error prepareError = std::move(*prepareResult.try_error());
            cue::Result<void> cleanupPreparation = prepare_backend_cleanup();

            if (!cleanupPreparation)
            {
                add_secondary_error_context(prepareError, *cleanupPreparation.try_error(),
                                            "D3D12 Device Removal diagnostics also failed after Resize preparation",
                                            *m_assertContext);
            }

            if (m_backend->state() == cue::GraphicsBackendState::DeviceRemoved)
            {
                return shutdown_after_resize_failure(std::move(prepareError), true);
            }

            if (m_frameCommandState.status() == cue::D3d12FrameCommandStatus::Unavailable ||
                m_backend->state() == cue::GraphicsBackendState::Unavailable)
            {
                m_state = cue::PresentationContextState::Unavailable;
                return cue::Result<void>::failure(std::move(prepareError));
            }

            if (m_frameCommandState.was_resize_gpu_idle_proven())
            {
                return shutdown_after_resize_failure(std::move(prepareError), false);
            }

            return cue::Result<void>::failure(std::move(prepareError));
        }

        cue::Result<void> resourceReleasePreparation = prepare_backend_cleanup();

        if (m_backend->state() == cue::GraphicsBackendState::DeviceRemoved)
        {
            const HRESULT removalReason = m_device->GetDeviceRemovedReason();
            cue::Error removalError =
                make_native_error(*m_assertContext, k_deviceRemoved,
                                  "D3D12 Device was removed before Resize resource release", removalReason);

            if (!resourceReleasePreparation)
            {
                add_secondary_error_context(removalError, *resourceReleasePreparation.try_error(),
                                            "D3D12 Device Removal diagnostics also failed before Resize cleanup",
                                            *m_assertContext);
            }

            return shutdown_after_resize_failure(std::move(removalError), true);
        }

        if (!resourceReleasePreparation)
        {
            return shutdown_after_resize_failure(std::move(*resourceReleasePreparation.try_error()), false);
        }

        // GPU Idle確認後に旧寸法のDepthを退役し、次のScene Frameで新寸法を遅延生成する
        m_scenePass.release();
        cue::Result<void> rtvReleaseResult = m_frameCommandState.release_rtv_slots(m_rtvHeap);

        if (!rtvReleaseResult)
        {
            return shutdown_after_resize_failure(std::move(*rtvReleaseResult.try_error()), false);
        }

        m_frameCommandState.release_back_buffers();

        cue::Result<void> resizeResult = m_swapChain.resize(a_width, a_height);

        if (!resizeResult)
        {
            static_cast<void>(prepare_backend_cleanup());
            const bool isDeviceRemoved = m_backend->state() == cue::GraphicsBackendState::DeviceRemoved;
            return shutdown_after_resize_failure(std::move(*resizeResult.try_error()), isDeviceRemoved);
        }

        cue::Result<void> bindingResult = bind_swap_chain_back_buffers(m_swapChain, m_frameCommandState);

        if (!bindingResult)
        {
            return shutdown_after_resize_failure(std::move(*bindingResult.try_error()), false);
        }

        cue::Result<void> rtvResult =
            create_back_buffer_rtvs(m_device, m_frameCommandState, m_swapChain.format(), m_rtvHeap, *m_assertContext);

        if (!rtvResult)
        {
            return shutdown_after_resize_failure(std::move(*rtvResult.try_error()), false);
        }

        cue::Result<void> resumeResult = m_frameCommandState.resume_after_resize();

        if (!resumeResult)
        {
            return shutdown_after_resize_failure(std::move(*resumeResult.try_error()), false);
        }

        m_isResizePending = false;
        return cue::Result<void>::success();
    }

    // 通常終了では Fence による GPU 完了証明を要求し、Device Removal では有効な場合に DRED 収集を試行する
    // GPU 完了を証明できた通常終了と Device Removal 専用解放の成功経路では、Frame State を
    // CleanupPending へ移してから RTV、Back Buffer、Allocator の依存順で解放する
    /// @brief 保持する Native Resource を依存関係と完了条件に従って停止し、安全な解放結果を返す
    [[nodiscard]] cue::Result<void> shutdown() noexcept override
    {
        assert_thread("D3D12 Presentation shutdown must run on the creation thread");

        if (m_state == cue::PresentationContextState::Shutdown)
        {
            return cue::Result<void>::success();
        }

        if (m_state == cue::PresentationContextState::Unavailable ||
            m_backend->state() == cue::GraphicsBackendState::Unavailable)
        {
            m_state = cue::PresentationContextState::Unavailable;
            return cue::Result<void>::failure(
                make_error(*m_assertContext, k_presentationUnavailable, "D3D12 Presentation Context is unavailable"));
        }

        std::optional<cue::Error> firstError;
        cue::Result<void> diagnosticsResult = prepare_backend_cleanup();
        retain_shutdown_error(firstError, diagnosticsResult,
                              "D3D12 Device Removal diagnostics failed before Presentation cleanup", *m_assertContext);

        if (m_backend->state() == cue::GraphicsBackendState::DeviceRemoved)
        {
            m_state = cue::PresentationContextState::DeviceRemoved;
        }

        cue::Result<void> frameResult = m_backend->state() == cue::GraphicsBackendState::DeviceRemoved
                                            ? m_frameCommandState.begin_release_after_device_removed()
                                            : m_frameCommandState.begin_shutdown();
        retain_shutdown_error(firstError, frameResult, "D3D12 Frame Command cleanup also failed", *m_assertContext);

        if (m_frameCommandState.status() == cue::D3d12FrameCommandStatus::DeviceRemoved)
        {
            cue::Result<void> removalPreparation = prepare_backend_cleanup();
            retain_shutdown_error(firstError, removalPreparation,
                                  "D3D12 Device Removal diagnostics also failed during Presentation shutdown",
                                  *m_assertContext);
            cue::Result<void> releaseResult = m_frameCommandState.begin_release_after_device_removed();
            retain_shutdown_error(firstError, releaseResult, "D3D12 Frame Command Device Removal cleanup also failed",
                                  *m_assertContext);
        }

        if (m_frameCommandState.status() == cue::D3d12FrameCommandStatus::Unavailable)
        {
            m_state = cue::PresentationContextState::Unavailable;
            return cue::Result<void>::failure(std::move(*firstError));
        }

        cue::Result<void> resourceReleasePreparation = prepare_backend_cleanup();
        retain_shutdown_error(
            firstError, resourceReleasePreparation,
            "D3D12 Device Removal diagnostics failed immediately before Presentation resource release",
            *m_assertContext);

        if (m_backend->state() == cue::GraphicsBackendState::DeviceRemoved)
        {
            m_state = cue::PresentationContextState::DeviceRemoved;
            cue::Result<void> releaseResult = m_frameCommandState.begin_release_after_device_removed();
            retain_shutdown_error(firstError, releaseResult,
                                  "D3D12 Frame Command Device Removal cleanup also failed before resource release",
                                  *m_assertContext);
        }

        if (m_frameCommandState.status() != cue::D3d12FrameCommandStatus::CleanupPending)
        {
            return cue::Result<void>::failure(std::move(*firstError));
        }

        m_scenePass.release();
        cue::Result<void> rtvReleaseResult = m_frameCommandState.release_rtv_slots(m_rtvHeap);
        m_frameCommandState.release_back_buffers();
        cue::Result<void> allocatorResult = m_frameCommandState.release_allocators_after_presentation_cleanup();
        cue::Result<void> rtvHeapResult = m_rtvHeap.shutdown();
        cue::Result<void> swapChainResult = m_swapChain.shutdown();
        retain_shutdown_error(firstError, rtvReleaseResult, "D3D12 RTV Slot cleanup also failed", *m_assertContext);
        retain_shutdown_error(firstError, allocatorResult, "D3D12 Frame Allocator cleanup also failed",
                              *m_assertContext);
        retain_shutdown_error(firstError, rtvHeapResult, "D3D12 RTV Heap cleanup also failed", *m_assertContext);
        retain_shutdown_error(firstError, swapChainResult, "D3D12 Swap Chain cleanup also failed", *m_assertContext);

        if (!rtvReleaseResult || !allocatorResult || !swapChainResult || !rtvHeapResult)
        {
            m_state = cue::PresentationContextState::Unavailable;
            return cue::Result<void>::failure(std::move(*firstError));
        }

        unregister_from_backend();
        m_state = cue::PresentationContextState::Shutdown;

        if (firstError)
        {
            return cue::Result<void>::failure(std::move(*firstError));
        }

        return cue::Result<void>::success();
    }

    /// @brief D3D12 Backend が保持する Probe Report を呼び出し元へ返す
    [[nodiscard]] cue::D3d12PresentationProbeReport probe_report() const noexcept
    {
        cue::D3d12PresentationProbeReport report = {};
        report.lastSubmittedFence = m_frameCommandState.last_submitted_fence();
        report.frameReuseFences = {
            m_frameCommandState.frame_reuse_fence(0),
            m_frameCommandState.frame_reuse_fence(1),
        };
        report.formatsMatch = true;

        for (std::uint32_t index = 0; index < cue::k_d3d12SwapChainBufferCount; ++index)
        {
            if (m_frameCommandState.rtv_count() != cue::k_d3d12SwapChainBufferCount)
            {
                report.formatsMatch = false;
                break;
            }

            cue::Result<ID3D12Resource *> backBufferResult = m_frameCommandState.back_buffer(index);

            if (!backBufferResult || (*backBufferResult.try_value())->GetDesc().Format != m_swapChain.format())
            {
                report.formatsMatch = false;
            }
        }

        report.allocatorCount = m_frameCommandState.allocator_count();
        report.backBufferCount = m_frameCommandState.back_buffer_count();
        report.rtvCount = m_frameCommandState.rtv_count();
        report.formatsMatch = report.formatsMatch && m_frameCommandState.are_back_buffers_present();
        report.isAcceptingFrames = m_frameCommandState.is_accepting_frames();
        report.hasCommandList = m_frameCommandState.has_command_list();
        report.hasSwapChain = m_swapChain.has_native_objects();
        report.hasRtvHeap = m_rtvHeap.has_native_object();
        report.isRegistered = m_isRegistered;
        const std::array<std::uint32_t, 2> depthExtent = m_scenePass.depth_extent();
        report.sceneDepthWidth = depthExtent[0];
        report.sceneDepthHeight = depthExtent[1];
        report.hasSceneDepthDsv = m_scenePass.has_depth_dsv();
        report.sceneDepthIdentity = m_scenePass.depth_identity_for_probe();
        report.hasSceneNativeObjects = m_scenePass.has_native_objects();
        return report;
    }

#if CUE_D3D12_TESTING
    /// @brief Scene障害ProbeへGPU所有Resourceの個別保持状態を返す
    [[nodiscard]] SceneOwnerProbeReport scene_owner_report_for_probe() const noexcept
    {
        return {m_scenePass.has_depth_dsv(), m_scenePass.has_geometry_for_probe(),
                m_scenePass.has_constants_for_probe()};
    }

    /// @brief このPresentationだけの次回Scene生成失敗をProbeから指定する
    void set_scene_creation_fault_for_probe(cue::D3d12SceneCreationFault a_fault) noexcept
    {
        m_scenePass.set_creation_fault_for_probe(a_fault);
    }

    /// @brief 現寸法のSwap Chain Back Bufferを読み戻すBufferをProbeへ作成する
    [[nodiscard]] bool create_scene_capture_for_probe(D3d12ScenePixelCapture &a_capture) noexcept
    {
        cue::Result<ID3D12Resource *> backBuffer =
            m_frameCommandState.back_buffer(m_swapChain.current_back_buffer_index());
        if (!backBuffer)
        {
            return false;
        }
        const D3D12_RESOURCE_DESC texture = (*backBuffer.try_value())->GetDesc();
        UINT rowCount = 0U;
        UINT64 rowSize = 0U;
        UINT64 readbackSize = 0U;
        m_device->GetCopyableFootprints(&texture, 0U, 1U, 0U, &a_capture.footprint, &rowCount, &rowSize, &readbackSize);
        if (readbackSize == 0U)
        {
            return false;
        }
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = readbackSize;
        buffer.Height = 1U;
        buffer.DepthOrArraySize = 1U;
        buffer.MipLevels = 1U;
        buffer.SampleDesc.Count = 1U;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(a_capture.readback.GetAddressOf()))))
        {
            return false;
        }
        a_capture.width = m_swapChain.width();
        a_capture.height = m_swapChain.height();
        return true;
    }

    /// @brief 次のScene Frameに限り、Present前のReadback Copy先を指定する
    void set_scene_capture_for_probe(D3d12ScenePixelCapture *a_capture) noexcept
    {
        m_sceneCaptureForProbe = a_capture;
    }

#endif

    /// @brief D3D12 Backend の Transition Frame For Probe を GPU 実行順と Resource State を守って投入する
    [[nodiscard]] cue::Result<std::uint64_t> submit_transition_frame_for_probe() noexcept
    {
        const std::uint32_t frameIndex = m_swapChain.current_back_buffer_index();
        cue::Result<void> beginResult = m_frameCommandState.begin_frame(frameIndex);

        if (!beginResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*beginResult.try_error()));
        }

        cue::Result<void> renderTargetResult =
            m_frameCommandState.transition_back_buffer(frameIndex, cue::D3d12BackBufferState::RenderTarget);

        if (!renderTargetResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*renderTargetResult.try_error()));
        }

        cue::Result<void> presentStateResult =
            m_frameCommandState.transition_back_buffer(frameIndex, cue::D3d12BackBufferState::Present);

        if (!presentStateResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*presentStateResult.try_error()));
        }

        cue::Result<void> closeResult = m_frameCommandState.close_frame();

        if (!closeResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*closeResult.try_error()));
        }

        cue::Result<void> executeResult = m_frameCommandState.execute_frame();

        if (!executeResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*executeResult.try_error()));
        }

        cue::Result<void> presentResult = m_frameCommandState.mark_present_attempted();

        if (!presentResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*presentResult.try_error()));
        }

        return m_frameCommandState.signal_frame();
    }

    /// @brief D3D12 Backend の Clear Frame For Probe を GPU 実行順と Resource State を守って投入する
    [[nodiscard]] cue::Result<std::uint64_t> submit_clear_frame_for_probe(const std::array<float, 4> &a_color) noexcept
    {
        const std::uint32_t frameIndex = m_swapChain.current_back_buffer_index();
        cue::Result<void> beginResult = m_frameCommandState.begin_frame(frameIndex);

        if (!beginResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*beginResult.try_error()));
        }

        cue::Result<void> renderTargetResult =
            m_frameCommandState.transition_back_buffer(frameIndex, cue::D3d12BackBufferState::RenderTarget);

        if (!renderTargetResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*renderTargetResult.try_error()));
        }

        cue::Result<void> clearResult = m_frameCommandState.clear_back_buffer(frameIndex, m_rtvHeap, a_color);

        if (!clearResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*clearResult.try_error()));
        }

        cue::Result<void> presentStateResult =
            m_frameCommandState.transition_back_buffer(frameIndex, cue::D3d12BackBufferState::Present);

        if (!presentStateResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*presentStateResult.try_error()));
        }

        cue::Result<void> closeResult = m_frameCommandState.close_frame();

        if (!closeResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*closeResult.try_error()));
        }

        cue::Result<void> executeResult = m_frameCommandState.execute_frame();

        if (!executeResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*executeResult.try_error()));
        }

        cue::Result<void> presentResult = m_frameCommandState.mark_present_attempted();

        if (!presentResult)
        {
            return cue::Result<std::uint64_t>::failure(std::move(*presentResult.try_error()));
        }

        return m_frameCommandState.signal_frame();
    }

  private:
    /// @brief D3D12 Backend を Backend Cleanup へ安全に移行できる所有状態へ整える
    [[nodiscard]] cue::Result<void> prepare_backend_cleanup() noexcept
    {
        PresentationCleanupOwnerShape shape = {
            m_frameCommandState.allocator_count(), m_frameCommandState.back_buffer_count(),
            m_frameCommandState.rtv_count(),       m_frameCommandState.has_command_list(),
            m_swapChain.has_native_objects(),      m_rtvHeap.has_native_object(),
#if CUE_D3D12_TESTING
            m_scenePass.has_depth_dsv(),           m_scenePass.has_geometry_for_probe(),
            m_scenePass.has_constants_for_probe(),
#endif
        };
        return m_preparePresentationCleanup(m_backendOwner, shape);
    }

    /// @brief D3D12 Backend の After Resize Failure を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] cue::Result<void> shutdown_after_resize_failure(cue::Error &&a_error, bool a_isDeviceRemoved) noexcept
    {
        std::optional<cue::Error> firstError;
        firstError.emplace(std::move(a_error));
        cue::Result<void> frameResult = a_isDeviceRemoved ? m_frameCommandState.begin_release_after_device_removed()
                                                          : m_frameCommandState.begin_release_after_gpu_idle();
        retain_shutdown_error(firstError, frameResult, "D3D12 Frame Command cleanup also failed", *m_assertContext);

        if (m_frameCommandState.status() != cue::D3d12FrameCommandStatus::CleanupPending)
        {
            m_state = cue::PresentationContextState::Unavailable;
            return cue::Result<void>::failure(std::move(*firstError));
        }

        m_scenePass.release();
        cue::Result<void> rtvReleaseResult = m_frameCommandState.release_rtv_slots(m_rtvHeap);
        m_frameCommandState.release_back_buffers();
        cue::Result<void> allocatorResult = m_frameCommandState.release_allocators_after_presentation_cleanup();
        cue::Result<void> rtvHeapResult = m_rtvHeap.shutdown();
        cue::Result<void> swapChainResult = m_swapChain.shutdown();
        retain_shutdown_error(firstError, rtvReleaseResult, "D3D12 RTV Slot cleanup also failed", *m_assertContext);
        retain_shutdown_error(firstError, allocatorResult, "D3D12 Frame Allocator cleanup also failed",
                              *m_assertContext);
        retain_shutdown_error(firstError, rtvHeapResult, "D3D12 RTV Heap cleanup also failed", *m_assertContext);
        retain_shutdown_error(firstError, swapChainResult, "D3D12 Swap Chain cleanup also failed", *m_assertContext);

        if (!rtvReleaseResult || !allocatorResult || !rtvHeapResult || !swapChainResult)
        {
            m_state = cue::PresentationContextState::Unavailable;
            return cue::Result<void>::failure(std::move(*firstError));
        }

        unregister_from_backend();
        m_state = cue::PresentationContextState::Shutdown;
        return cue::Result<void>::failure(std::move(*firstError));
    }

    /// @brief 生成 Thread 以外からの D3D12 Backend 操作を検出して診断する
    void assert_thread(std::string_view a_message) const noexcept
    {
        static_cast<void>(a_message);
        CUE_ASSERT(*m_assertContext, std::this_thread::get_id() == m_creationThread, a_message);
    }

    /// @brief D3D12 Backend の From Backend を依存関係と完了条件を守って安全に解放または停止する
    void unregister_from_backend() noexcept
    {
        CUE_ASSERT(*m_assertContext, m_isRegistered, "D3D12 Presentation registration was already released");
        m_unregisterPresentation(m_backendOwner);
        m_isRegistered = false;
    }

    cue::D3d12SwapChainState m_swapChain;
    cue::D3d12FrameCommandState m_frameCommandState;
    cue::D3d12RtvHeap m_rtvHeap;
    cue::D3d12ScenePass m_scenePass;
    ID3D12Device *m_device;
    cue::GraphicsBackend *m_backend;
    void *m_backendOwner;
    UnregisterPresentation m_unregisterPresentation;
    PreparePresentationCleanup m_preparePresentationCleanup;
    ClassifyPresentationNativeFailure m_classifyNativeFailure;
    cue::AssertContext *m_assertContext;
    std::thread::id m_creationThread;
    cue::PresentationContextState m_state;
    bool m_isRegistered;
    bool m_isResizePending;
#if CUE_D3D12_TESTING
    D3d12ScenePixelCapture *m_sceneCaptureForProbe = nullptr;
#endif
};

// Process 側の Factory、Adapter、Device、Queue を所有し、Presentation へ長寿命 Resource を貸し出す Backend 実装
class D3d12BackendImpl final : public cue::D3d12Backend
{
  public:
    /// @brief D3d12BackendImpl を必要な依存と初期状態から構築する
    D3d12BackendImpl(cue::D3d12AdapterSelection &&a_selection, Microsoft::WRL::ComPtr<ID3D12Device> a_device,
                     cue::D3d12QueueState &&a_queueState, cue::D3d12DiagnosticsStatus a_diagnostics,
                     cue::CapabilityReport &&a_capabilities, cue::AssertContext &a_assertContext) noexcept
        : m_factory(std::move(a_selection.factory)), m_adapter(std::move(a_selection.adapter)),
          m_device(std::move(a_device)), m_queueState(std::move(a_queueState)),
          m_capabilities(std::move(a_capabilities)), m_diagnostics(a_diagnostics), m_assertContext(&a_assertContext),
          m_creationThread(std::this_thread::get_id()), m_state(cue::GraphicsBackendState::Ready),
          m_activePresentationCount(0), m_dredCollectionAttemptCount(0), m_lastDredOwnerReport{}
#if CUE_D3D12_TESTING
          ,
          m_lastDredSceneOwnerReport{}
#endif
    {
    }

    /// @brief D3d12BackendImpl が保持する Resource を所有権規則に従って破棄する
    ~D3d12BackendImpl() noexcept override
    {
        CUE_ASSERT(*m_assertContext, std::this_thread::get_id() == m_creationThread,
                   "D3D12 Backend must be destroyed on the creation thread");

        if (m_state != cue::GraphicsBackendState::Shutdown || m_activePresentationCount != 0 ||
            m_queueState.has_gpu_objects() || m_device != nullptr || m_adapter != nullptr || m_factory != nullptr)
        {
            m_assertContext->fatal_handler().terminate("D3D12 Backend owner was destroyed before shutdown");
        }
    }

    /// @brief 選択 Adapter と D3D12 Device から確定した Graphics 能力 Report を返す
    [[nodiscard]] const cue::CapabilityReport &capabilities() const noexcept override
    {
        CUE_ASSERT(*m_assertContext, std::this_thread::get_id() == m_creationThread,
                   "D3D12 Backend capabilities must be queried on the creation thread");
        return m_capabilities;
    }

    /// @brief D3D12 Backend が保持する State を呼び出し元へ返す
    [[nodiscard]] cue::GraphicsBackendState state() const noexcept override
    {
        CUE_ASSERT(*m_assertContext, std::this_thread::get_id() == m_creationThread,
                   "D3D12 Backend state must be queried on the creation thread");
        return m_state;
    }

    /// @brief D3D12 Backend の Device Removal For Probe を診断と Lifecycle の規則に従って更新する
    [[nodiscard]] cue::Result<void> force_device_removal_for_probe() noexcept
    {
        cue::Result<void> removalResult = remove_device_without_classification_for_probe();

        if (!removalResult)
        {
            return removalResult;
        }

        cue::Error removalError = make_native_error(
            *m_assertContext, k_deviceRemoved, "D3D12 Device Removal probe was requested", DXGI_ERROR_DEVICE_REMOVED);
        return classify_presentation_native_failure(std::move(removalError));
    }

    /// @brief D3D12 Backend の Device Without Classification For Probe
    /// を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] cue::Result<void> remove_device_without_classification_for_probe() noexcept
    {
        CUE_ASSERT(*m_assertContext, std::this_thread::get_id() == m_creationThread,
                   "D3D12 Device Removal probe must run on the Backend creation thread");
        Microsoft::WRL::ComPtr<ID3D12Device5> device5;
        const HRESULT interfaceResult = m_device.As(&device5);

        if (FAILED(interfaceResult))
        {
            return cue::Result<void>::failure(make_native_error(*m_assertContext, k_deviceRemovalProbeUnavailable,
                                                                "ID3D12Device5 is unavailable for removal probe",
                                                                interfaceResult));
        }

        device5->RemoveDevice();
        return cue::Result<void>::success();
    }

    /// @brief DRED 収集が試行された回数を Probe 検証用に返す
    [[nodiscard]] std::uint32_t dred_attempt_count_for_probe() const noexcept
    {
        return m_dredCollectionAttemptCount;
    }

    /// @brief D3D12 Backend が保持する Native Owner 状態を Probe Report として返す
    [[nodiscard]] cue::D3d12BackendOwnerProbeReport owner_report_for_probe() const noexcept
    {
        return {
            m_queueState.last_signaled_fence(),
            m_queueState.has_queue(),
            m_queueState.has_fence(),
            m_queueState.has_fence_event(),
            m_device != nullptr,
            m_adapter != nullptr,
            m_factory != nullptr,
        };
    }

    /// @brief 直近 DRED 収集時点の Native Owner 状態を Probe Report として返す
    [[nodiscard]] cue::D3d12DredOwnerProbeReport dred_owner_report_for_probe() const noexcept
    {
        return m_lastDredOwnerReport;
    }

#if CUE_D3D12_TESTING
    /// @brief 直近DRED収集時点のScene Resource保持状態を障害Probeへ返す
    [[nodiscard]] SceneOwnerProbeReport dred_scene_owner_report_for_probe() const noexcept
    {
        return m_lastDredSceneOwnerReport;
    }
#endif

    // Active Presentation が残る間は借用先が存在するため Shutdown を拒否し、先行解放を防ぐ
    // Device Removal 時は有効な診断だけ収集を試行する
    // DRED は Queue と Fence の解放前、InfoQueue は両者の解放後かつ Device 解放前に扱う
    /// @brief 保持する Native Resource を依存関係と完了条件に従って停止し、安全な解放結果を返す
    [[nodiscard]] cue::Result<void> shutdown() noexcept override
    {
        CUE_ASSERT(*m_assertContext, std::this_thread::get_id() == m_creationThread,
                   "D3D12 Backend shutdown must run on the creation thread");

        if (m_state == cue::GraphicsBackendState::Shutdown)
        {
            return cue::Result<void>::success();
        }

        if (m_state == cue::GraphicsBackendState::Unavailable)
        {
            return cue::Result<void>::failure(
                make_error(*m_assertContext, k_backendUnavailable, "D3D12 Backend is unavailable"));
        }

        std::optional<cue::Error> firstError;
        const bool isDeviceRemoved = m_state == cue::GraphicsBackendState::DeviceRemoved ||
                                     m_queueState.status() == cue::D3d12QueueStateStatus::DeviceRemoved;

        if (isDeviceRemoved)
        {
            m_state = cue::GraphicsBackendState::DeviceRemoved;
            cue::Result<void> dredResult = ensure_device_removed_diagnostics();
            retain_shutdown_error(firstError, dredResult, "D3D12 DRED diagnostics failed before Presentation cleanup",
                                  *m_assertContext);
        }

        if (m_activePresentationCount != 0)
        {
            cue::Error activeError = make_error(*m_assertContext, k_activePresentationContexts,
                                                "D3D12 Backend has active Presentation Contexts");

            if (firstError)
            {
                add_secondary_error_context(activeError, *firstError,
                                            "D3D12 Device Removal diagnostics also failed before active Context gate",
                                            *m_assertContext);
            }

            return cue::Result<void>::failure(std::move(activeError));
        }

        if (m_queueState.status() == cue::D3d12QueueStateStatus::DeviceRemoved)
        {
            cue::Result<void> queueReleaseResult = m_queueState.release_after_device_removed();
            retain_shutdown_error(firstError, queueReleaseResult,
                                  "D3D12 Queue cleanup also failed after device removal", *m_assertContext);

            if (m_queueState.status() == cue::D3d12QueueStateStatus::Unavailable)
            {
                m_state = cue::GraphicsBackendState::Unavailable;
                return cue::Result<void>::failure(std::move(*firstError));
            }
        }
        else
        {
            cue::Result<void> queueShutdownResult = m_queueState.shutdown();

            if (!queueShutdownResult)
            {
                firstError.emplace(std::move(*queueShutdownResult.try_error()));

                if (m_queueState.status() == cue::D3d12QueueStateStatus::Unavailable)
                {
                    m_state = cue::GraphicsBackendState::Unavailable;
                    return cue::Result<void>::failure(std::move(*firstError));
                }
            }

            if (m_queueState.status() == cue::D3d12QueueStateStatus::DeviceRemoved)
            {
                m_state = cue::GraphicsBackendState::DeviceRemoved;

                cue::Result<void> dredResult = ensure_device_removed_diagnostics();
                retain_shutdown_error(firstError, dredResult,
                                      "D3D12 DRED diagnostics also failed while handling device removal",
                                      *m_assertContext);

                cue::Result<void> queueReleaseResult = m_queueState.release_after_device_removed();
                retain_shutdown_error(firstError, queueReleaseResult,
                                      "D3D12 Queue cleanup also failed after device removal", *m_assertContext);

                if (m_queueState.status() == cue::D3d12QueueStateStatus::Unavailable)
                {
                    m_state = cue::GraphicsBackendState::Unavailable;
                    return cue::Result<void>::failure(std::move(*firstError));
                }
            }
        }

        cue::Result<void> liveObjectResult =
            cue::report_d3d12_live_device_objects(m_device.Get(), m_diagnostics, *m_assertContext);

        retain_shutdown_error(firstError, liveObjectResult, "D3D12 Live Object diagnostics also failed",
                              *m_assertContext);

        cue::Result<void> messageResult = cue::log_d3d12_messages_at_quiescent_point(
            m_device.Get(), m_diagnostics, "D3D12 Backend shutdown", *m_assertContext);

        retain_shutdown_error(firstError, messageResult, "D3D12 InfoQueue diagnostics also failed", *m_assertContext);

        m_device.Reset();
        m_adapter.Reset();
        m_factory.Reset();
        m_state = cue::GraphicsBackendState::Shutdown;

        if (firstError)
        {
            return cue::Result<void>::failure(std::move(*firstError));
        }

        return cue::Result<void>::success();
    }

  private:
    /// @brief Device Removal 診断を一度だけ収集し、後続 Cleanup から再利用できる状態にする
    [[nodiscard]] cue::Result<void> ensure_device_removed_diagnostics(
        const PresentationCleanupOwnerShape *a_presentationShape = nullptr) noexcept
    {
        if (a_presentationShape != nullptr)
        {
            m_lastDredOwnerReport = {
                a_presentationShape->allocatorCount,
                a_presentationShape->backBufferCount,
                a_presentationShape->rtvCount,
                a_presentationShape->hasCommandList,
                a_presentationShape->hasSwapChain,
                a_presentationShape->hasRtvHeap,
                m_queueState.has_queue(),
                m_queueState.has_fence(),
                m_queueState.has_fence_event(),
#if CUE_D3D12_TESTING
            };
            m_lastDredSceneOwnerReport = {
                a_presentationShape->hasSceneDepthDsv,
                a_presentationShape->hasSceneGeometry,
                a_presentationShape->hasSceneConstants,
#endif
            };
        }

        if (m_dredCollectionAttemptCount != 0)
        {
            return cue::Result<void>::success();
        }

        ++m_dredCollectionAttemptCount;
        return cue::collect_d3d12_device_removed_diagnostics(m_device.Get(), m_diagnostics, *m_assertContext);
    }

    /// @brief Presentation が Backend 診断へ使用する非所有 Assert Context を返す
    [[nodiscard]] const cue::AssertContext &assert_context_for_presentation() const noexcept override
    {
        return *m_assertContext;
    }

    // Swap Chain、RTV Heap、Frame State、Back Buffer Binding、RTV 生成の依存順で Presentation を構築する
    /// @brief D3D12 Backend で使用する Windows Presentation を生成し、呼び出し元へ返す
    [[nodiscard]] cue::Result<std::unique_ptr<cue::PresentationContext>> create_windows_presentation(
        const void *a_nativeWindow, std::uint32_t a_width, std::uint32_t a_height,
        const cue::PresentationDescriptor &a_descriptor) noexcept override
    {
        CUE_ASSERT(*m_assertContext, std::this_thread::get_id() == m_creationThread,
                   "D3D12 Presentation must be created on the Backend creation thread");

        if (m_state != cue::GraphicsBackendState::Ready)
        {
            return cue::Result<std::unique_ptr<cue::PresentationContext>>::failure(make_error(
                *m_assertContext, k_backendUnavailable, "D3D12 Backend cannot create a Presentation Context"));
        }

        cue::D3d12SwapChainDescriptor swapChainDescriptor = {
            static_cast<HWND>(const_cast<void *>(a_nativeWindow)),
            a_width,
            a_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            a_descriptor.isVsyncEnabled,
        };
        cue::D3d12SwapChainFailureHandler failureHandler = {
            this,
            handle_swap_chain_native_failure,
        };
        cue::D3d12SwapChainNativeFunctions swapChainFunctions = g_presentationFrameProbeState.useProbeFunctions
                                                                    ? make_swap_chain_lifecycle_probe_functions()
                                                                    : cue::default_d3d12_swap_chain_native_functions();
        cue::Result<cue::D3d12SwapChainState> swapChainResult = cue::create_d3d12_swap_chain_state(
            m_factory.Get(), m_queueState.native_queue_for_presentation(), swapChainDescriptor, *m_assertContext,
            swapChainFunctions, failureHandler);

        if (!swapChainResult)
        {
            return cue::Result<std::unique_ptr<cue::PresentationContext>>::failure(
                std::move(*swapChainResult.try_error()));
        }

        cue::D3d12SwapChainState swapChain = std::move(*swapChainResult.try_value());
        cue::D3d12RtvHeapFailureHandler rtvFailureHandler = {
            this,
            handle_rtv_heap_native_failure,
        };
        cue::Result<cue::D3d12RtvHeap> rtvHeapResult = cue::create_d3d12_rtv_heap(
            m_device.Get(), *m_assertContext, cue::default_d3d12_rtv_heap_native_functions(), rtvFailureHandler);

        if (!rtvHeapResult)
        {
            cue::Result<void> swapChainShutdownResult = swapChain.shutdown();
            cue::Error failureError = std::move(*rtvHeapResult.try_error());

            if (!swapChainShutdownResult)
            {
                add_secondary_error_context(failureError, *swapChainShutdownResult.try_error(),
                                            "D3D12 Swap Chain rollback also failed", *m_assertContext);
            }

            return cue::Result<std::unique_ptr<cue::PresentationContext>>::failure(std::move(failureError));
        }

        cue::D3d12RtvHeap rtvHeap = std::move(*rtvHeapResult.try_value());
        cue::D3d12FrameCommandNativeFunctions frameCommandFunctions =
            g_presentationFrameProbeState.useProbeFunctions ? make_frame_command_lifecycle_probe_functions()
                                                            : cue::default_d3d12_frame_command_native_functions();
        cue::Result<cue::D3d12FrameCommandState> frameStateResult = cue::create_d3d12_frame_command_state(
            m_device.Get(), m_queueState, *m_assertContext, frameCommandFunctions);

        if (!frameStateResult)
        {
            cue::Error failureError = std::move(*frameStateResult.try_error());
            cue::Result<void> heapShutdownResult = rtvHeap.shutdown();
            cue::Result<void> swapChainShutdownResult = swapChain.shutdown();

            if (!heapShutdownResult)
            {
                add_secondary_error_context(failureError, *heapShutdownResult.try_error(),
                                            "D3D12 RTV Heap rollback also failed", *m_assertContext);
            }

            if (!swapChainShutdownResult)
            {
                add_secondary_error_context(failureError, *swapChainShutdownResult.try_error(),
                                            "D3D12 Swap Chain rollback also failed", *m_assertContext);
            }

            return cue::Result<std::unique_ptr<cue::PresentationContext>>::failure(std::move(failureError));
        }

        cue::D3d12FrameCommandState frameState = std::move(*frameStateResult.try_value());
        cue::Result<void> bindingResult = bind_swap_chain_back_buffers(swapChain, frameState);
        cue::Result<void> rtvResult =
            bindingResult
                ? create_back_buffer_rtvs(m_device.Get(), frameState, swapChain.format(), rtvHeap, *m_assertContext)
                : cue::Result<void>::failure(std::move(*bindingResult.try_error()));

        if (!rtvResult)
        {
            cue::Error failureError = std::move(*rtvResult.try_error());
            static_cast<void>(frameState.suspend_for_resize());
            cue::Result<void> frameBeginResult = frameState.begin_release_after_gpu_idle();
            cue::Result<void> rtvReleaseResult = frameState.release_rtv_slots(rtvHeap);
            frameState.release_back_buffers();
            cue::Result<void> allocatorResult =
                frameState.status() == cue::D3d12FrameCommandStatus::CleanupPending
                    ? frameState.release_allocators_after_presentation_cleanup()
                    : cue::Result<void>::failure(make_error(*m_assertContext, k_presentationUnavailable,
                                                            "D3D12 Frame rollback could not release Allocators"));
            cue::Result<void> heapShutdownResult = rtvHeap.shutdown();
            cue::Result<void> swapChainShutdownResult = swapChain.shutdown();

            if (!frameBeginResult)
            {
                add_secondary_error_context(failureError, *frameBeginResult.try_error(),
                                            "D3D12 Frame Command rollback also failed", *m_assertContext);
            }

            if (!rtvReleaseResult)
            {
                add_secondary_error_context(failureError, *rtvReleaseResult.try_error(),
                                            "D3D12 RTV Slot rollback also failed", *m_assertContext);
            }

            if (!allocatorResult)
            {
                add_secondary_error_context(failureError, *allocatorResult.try_error(),
                                            "D3D12 Frame Allocator rollback also failed", *m_assertContext);
            }

            if (!heapShutdownResult)
            {
                add_secondary_error_context(failureError, *heapShutdownResult.try_error(),
                                            "D3D12 RTV Heap rollback also failed", *m_assertContext);
            }

            if (!swapChainShutdownResult)
            {
                add_secondary_error_context(failureError, *swapChainShutdownResult.try_error(),
                                            "D3D12 Swap Chain rollback also failed", *m_assertContext);
            }

            return cue::Result<std::unique_ptr<cue::PresentationContext>>::failure(std::move(failureError));
        }

        try
        {
            std::unique_ptr<cue::PresentationContext> presentation = std::make_unique<D3d12PresentationContext>(
                std::move(swapChain), std::move(frameState), std::move(rtvHeap), m_device.Get(), *this, this,
                unregister_presentation, prepare_presentation_cleanup, classify_scene_native_failure, *m_assertContext);
            ++m_activePresentationCount;
            return cue::Result<std::unique_ptr<cue::PresentationContext>>::success(std::move(presentation));
        }
        catch (...)
        {
            terminate_allocation(*m_assertContext);
        }
    }

    /// @brief Presentation の Native 失敗を Device Removal または通常 Error へ分類する
    [[nodiscard]] cue::Result<void> classify_presentation_native_failure(cue::Error &&a_error) noexcept
    {
        cue::Result<void> classificationResult = m_queueState.reclassify_device_failure(std::move(a_error));

        if (classificationResult)
        {
            m_assertContext->fatal_handler().terminate("D3D12 native failure classification did not retain an Error");
        }

        std::optional<cue::Error> firstError;
        firstError.emplace(std::move(*classificationResult.try_error()));

        if (m_queueState.status() == cue::D3d12QueueStateStatus::DeviceRemoved)
        {
            m_state = cue::GraphicsBackendState::DeviceRemoved;
            cue::Result<void> dredResult = ensure_device_removed_diagnostics();
            retain_shutdown_error(firstError, dredResult,
                                  "D3D12 DRED diagnostics also failed during Presentation creation", *m_assertContext);
        }

        return cue::Result<void>::failure(std::move(*firstError));
    }

    /// @brief D3D12 Backend の Swap Chain Native Failure を規定された順序と失敗規則で処理する
    [[nodiscard]] static cue::Result<void> handle_swap_chain_native_failure(
        void *a_backend, cue::Error &&a_error, const cue::D3d12SwapChainFailureResources &a_resources) noexcept
    {
        static_cast<void>(a_resources);
        D3d12BackendImpl &backend = *static_cast<D3d12BackendImpl *>(a_backend);
        CUE_ASSERT(*backend.m_assertContext, std::this_thread::get_id() == backend.m_creationThread,
                   "D3D12 Swap Chain failure must be classified on the Backend creation thread");
        return backend.classify_presentation_native_failure(std::move(a_error));
    }

    /// @brief D3D12 Backend の RTV Heap Native Failure を規定された順序と失敗規則で処理する
    [[nodiscard]] static cue::Result<void> handle_rtv_heap_native_failure(
        void *a_backend, cue::Error &&a_error, const cue::D3d12RtvHeapFailureResources &a_resources) noexcept
    {
        static_cast<void>(a_resources);
        D3d12BackendImpl &backend = *static_cast<D3d12BackendImpl *>(a_backend);
        CUE_ASSERT(*backend.m_assertContext, std::this_thread::get_id() == backend.m_creationThread,
                   "D3D12 RTV Heap failure must be classified on the Backend creation thread");
        return backend.classify_presentation_native_failure(std::move(a_error));
    }

    /// @brief Scene 初期化の Native 失敗を部分 Resource 解放前に Device Removal と DRED へ分類する
    [[nodiscard]] static cue::Result<void> classify_scene_native_failure(void *a_backend, cue::Error &&a_error) noexcept
    {
        D3d12BackendImpl &backend = *static_cast<D3d12BackendImpl *>(a_backend);
        CUE_ASSERT(*backend.m_assertContext, std::this_thread::get_id() == backend.m_creationThread,
                   "D3D12 Scene failure must be classified on the Backend creation thread");
        return backend.classify_presentation_native_failure(std::move(a_error));
    }

    /// @brief D3D12 Backend の Presentation を依存関係と完了条件を守って安全に解放または停止する
    static void unregister_presentation(void *a_backend) noexcept
    {
        D3d12BackendImpl &backend = *static_cast<D3d12BackendImpl *>(a_backend);
        CUE_ASSERT(*backend.m_assertContext, std::this_thread::get_id() == backend.m_creationThread,
                   "D3D12 Presentation must be unregistered on the Backend creation thread");
        CUE_ASSERT(*backend.m_assertContext, backend.m_activePresentationCount > 0,
                   "D3D12 Presentation registration count underflow");
        --backend.m_activePresentationCount;
    }

    /// @brief D3D12 Backend を Presentation Cleanup へ安全に移行できる所有状態へ整える
    [[nodiscard]] static cue::Result<void> prepare_presentation_cleanup(
        void *a_backend, const PresentationCleanupOwnerShape &a_shape) noexcept
    {
        D3d12BackendImpl &backend = *static_cast<D3d12BackendImpl *>(a_backend);
        CUE_ASSERT(*backend.m_assertContext, std::this_thread::get_id() == backend.m_creationThread,
                   "D3D12 Presentation cleanup preparation must run on the Backend creation thread");

        if (backend.m_queueState.status() == cue::D3d12QueueStateStatus::Unavailable)
        {
            backend.m_state = cue::GraphicsBackendState::Unavailable;
            return cue::Result<void>::failure(
                make_error(*backend.m_assertContext, k_backendUnavailable, "D3D12 Backend is unavailable"));
        }

        if (backend.m_queueState.refresh_device_removed_status())
        {
            backend.m_state = cue::GraphicsBackendState::DeviceRemoved;
        }

        if (backend.m_state != cue::GraphicsBackendState::DeviceRemoved)
        {
            return cue::Result<void>::success();
        }

        return backend.ensure_device_removed_diagnostics(&a_shape);
    }

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter4> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    cue::D3d12QueueState m_queueState;
    cue::CapabilityReport m_capabilities;
    cue::D3d12DiagnosticsStatus m_diagnostics;
    cue::AssertContext *m_assertContext;
    std::thread::id m_creationThread;
    cue::GraphicsBackendState m_state;
    std::uint32_t m_activePresentationCount;
    std::uint32_t m_dredCollectionAttemptCount;
    cue::D3d12DredOwnerProbeReport m_lastDredOwnerReport;
#if CUE_D3D12_TESTING
    SceneOwnerProbeReport m_lastDredSceneOwnerReport;
#endif
};
} // namespace

namespace cue
{
D3d12Backend::~D3d12Backend() noexcept = default;

// === Test Probe Boundary ===
// 以下は Production API から検証専用状態へ到達する薄い入口であり、通常の Frame Loop では使用しない
D3d12PresentationProbeReport probe_d3d12_presentation(PresentationContext &a_presentation) noexcept
{
    D3d12PresentationContext *presentation = dynamic_cast<D3d12PresentationContext *>(&a_presentation);

    if (presentation == nullptr)
    {
        return {};
    }

    return presentation->probe_report();
}

bool submit_d3d12_transition_frame_for_probe(PresentationContext &a_presentation) noexcept
{
    D3d12PresentationContext *presentation = dynamic_cast<D3d12PresentationContext *>(&a_presentation);

    if (presentation == nullptr)
    {
        return false;
    }

    return static_cast<bool>(presentation->submit_transition_frame_for_probe());
}

bool submit_d3d12_clear_frame_for_probe(PresentationContext &a_presentation,
                                        const std::array<float, 4> &a_color) noexcept
{
    D3d12PresentationContext *presentation = dynamic_cast<D3d12PresentationContext *>(&a_presentation);

    if (presentation == nullptr)
    {
        return false;
    }

    return static_cast<bool>(presentation->submit_clear_frame_for_probe(a_color));
}

Result<void> force_d3d12_device_removal_for_probe(D3d12Backend &a_backend) noexcept
{
    D3d12BackendImpl *backend = dynamic_cast<D3d12BackendImpl *>(&a_backend);

    if (backend == nullptr)
    {
        return Result<void>::failure(make_error(a_backend.assert_context_for_presentation(),
                                                k_deviceRemovalProbeUnavailable,
                                                "D3D12 Device Removal probe requires the production Backend"));
    }

    return backend->force_device_removal_for_probe();
}

Result<void> remove_d3d12_device_without_classification_for_probe(D3d12Backend &a_backend) noexcept
{
    D3d12BackendImpl *backend = dynamic_cast<D3d12BackendImpl *>(&a_backend);

    if (backend == nullptr)
    {
        return Result<void>::failure(make_error(a_backend.assert_context_for_presentation(),
                                                k_deviceRemovalProbeUnavailable,
                                                "D3D12 Device Removal probe requires the production Backend"));
    }

    return backend->remove_device_without_classification_for_probe();
}

Result<std::uint32_t> d3d12_dred_attempt_count_for_probe(D3d12Backend &a_backend) noexcept
{
    D3d12BackendImpl *backend = dynamic_cast<D3d12BackendImpl *>(&a_backend);

    if (backend == nullptr)
    {
        return Result<std::uint32_t>::failure(make_error(a_backend.assert_context_for_presentation(),
                                                         k_deviceRemovalProbeUnavailable,
                                                         "D3D12 DRED attempt probe requires the production Backend"));
    }

    std::uint32_t attemptCount = backend->dred_attempt_count_for_probe();
    return Result<std::uint32_t>::success(std::move(attemptCount));
}

Result<D3d12BackendOwnerProbeReport> probe_d3d12_backend_owners_for_probe(D3d12Backend &a_backend) noexcept
{
    D3d12BackendImpl *backend = dynamic_cast<D3d12BackendImpl *>(&a_backend);

    if (backend == nullptr)
    {
        return Result<D3d12BackendOwnerProbeReport>::failure(
            make_error(a_backend.assert_context_for_presentation(), k_deviceRemovalProbeUnavailable,
                       "D3D12 Backend owner probe requires the production Backend"));
    }

    D3d12BackendOwnerProbeReport report = backend->owner_report_for_probe();
    return Result<D3d12BackendOwnerProbeReport>::success(std::move(report));
}

Result<D3d12DredOwnerProbeReport> probe_d3d12_dred_owners_for_probe(D3d12Backend &a_backend) noexcept
{
    D3d12BackendImpl *backend = dynamic_cast<D3d12BackendImpl *>(&a_backend);

    if (backend == nullptr)
    {
        return Result<D3d12DredOwnerProbeReport>::failure(
            make_error(a_backend.assert_context_for_presentation(), k_deviceRemovalProbeUnavailable,
                       "D3D12 DRED owner probe requires the production Backend"));
    }

    D3d12DredOwnerProbeReport report = backend->dred_owner_report_for_probe();
    return Result<D3d12DredOwnerProbeReport>::success(std::move(report));
}

#if CUE_D3D12_TESTING
/// @brief Production Presentationが保持するScene Resourceの個別状態を障害Probeへ返す
[[nodiscard]] SceneOwnerProbeReport probe_scene_owners_for_fault(PresentationContext &a_presentation) noexcept
{
    D3d12PresentationContext *presentation = dynamic_cast<D3d12PresentationContext *>(&a_presentation);
    return presentation != nullptr ? presentation->scene_owner_report_for_probe() : SceneOwnerProbeReport{};
}

/// @brief 直近DRED収集時点のScene Resource保持状態を障害Probeへ返す
[[nodiscard]] SceneOwnerProbeReport probe_dred_scene_owners_for_fault(D3d12Backend &a_backend) noexcept
{
    D3d12BackendImpl *backend = dynamic_cast<D3d12BackendImpl *>(&a_backend);
    return backend != nullptr ? backend->dred_scene_owner_report_for_probe() : SceneOwnerProbeReport{};
}

/// @brief RTV再構築失敗前にScene投入した場合もGPU完了後だけScene Resourceを解放する
[[nodiscard]] bool verify_d3d12_rtv_rebuild_failure_impl(const void *a_nativeWindow, std::uint32_t a_width,
                                                         std::uint32_t a_height, bool a_useScene,
                                                         AssertContext &a_assertContext) noexcept
{
    struct ProbeReset final
    {
        /// @brief RTV 再構築 Probe の Fault Injection 状態を検証終了時に必ず初期化する
        ~ProbeReset() noexcept
        {
            g_rtvCreationProbeState = {};
        }
    } probeReset;

    static_cast<void>(probeReset);
    g_rtvCreationProbeState = {true, 0};
    D3d12BackendDescriptor backendDescriptor = {
        D3d12AdapterPolicy::Warp,
        D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    Result<std::unique_ptr<D3d12Backend>> backendResult = create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return false;
    }

    std::unique_ptr<D3d12Backend> backend = std::move(*backendResult.try_value());
    PresentationDescriptor presentationDescriptor = {true};
    Result<std::unique_ptr<PresentationContext>> presentationResult =
        backend->create_windows_presentation(a_nativeWindow, a_width, a_height, presentationDescriptor);

    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return false;
    }

    std::unique_ptr<PresentationContext> presentation = std::move(*presentationResult.try_value());
    constexpr std::array<float, 16> identity = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const std::array sceneCubes = {PresentationSceneCube{identity}};
    const PresentationSceneFrameDescriptor scene = {{0.0F, 0.0F, 0.0F, 1.0F}, identity, sceneCubes};
    if (a_useScene && !presentation->present_scene_frame(scene))
    {
        static_cast<void>(presentation->shutdown());
        static_cast<void>(backend->shutdown());
        return false;
    }
    const D3d12PresentationProbeReport beforeResize = probe_d3d12_presentation(*presentation);
    const SceneOwnerProbeReport sceneOwnersBeforeResize = probe_scene_owners_for_fault(*presentation);
    Result<void> resizeResult = presentation->resize(a_width + 1, a_height + 1);
    const D3d12PresentationProbeReport afterResize = probe_d3d12_presentation(*presentation);
    const SceneOwnerProbeReport sceneOwnersAfterResize = probe_scene_owners_for_fault(*presentation);
    const Error *resizeError = resizeResult.try_error();
    const NativeError *nativeError = resizeError != nullptr ? resizeError->try_native_error() : nullptr;
    const bool failureValid =
        !resizeResult && resizeError->code().domain() == "Cue.RHI.D3D12" &&
        resizeError->code().value() == k_backBufferRtvCreationFailed && nativeError != nullptr &&
        nativeError->domain() == "D3D12" && presentation->state() == PresentationContextState::Shutdown &&
        backend->state() == GraphicsBackendState::Ready &&
        (!a_useScene || (beforeResize.lastSubmittedFence == 1U && beforeResize.hasSceneDepthDsv &&
                         sceneOwnersBeforeResize.hasGeometry && sceneOwnersBeforeResize.hasConstants &&
                         beforeResize.sceneDepthIdentity != 0U && !afterResize.hasSceneNativeObjects &&
                         !afterResize.hasSceneDepthDsv && !sceneOwnersAfterResize.hasGeometry &&
                         !sceneOwnersAfterResize.hasConstants));
    Result<void> presentationShutdownResult = presentation->shutdown();
    presentation.reset();
    Result<void> backendShutdownResult = backend->shutdown();
    const bool cleanupValid =
        presentationShutdownResult && backendShutdownResult && backend->state() == GraphicsBackendState::Shutdown;
    backend.reset();
    return failureValid && cleanupValid;
}

bool verify_d3d12_rtv_rebuild_failure_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                std::uint32_t a_height, AssertContext &a_assertContext) noexcept
{
    return verify_d3d12_rtv_rebuild_failure_impl(a_nativeWindow, a_width, a_height, false, a_assertContext);
}

bool verify_d3d12_scene_rtv_rebuild_failure_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                      std::uint32_t a_height, AssertContext &a_assertContext) noexcept
{
    return verify_d3d12_rtv_rebuild_failure_impl(a_nativeWindow, a_width, a_height, true, a_assertContext);
}

/// @brief GPU完了証明後にReadbackの中心・角画素から固定SceneのDepth結果を検査する
[[nodiscard]] bool validate_scene_resize_capture(const D3d12ScenePixelCapture &a_capture,
                                                 bool a_expectNearCube) noexcept
{
    if (!a_capture.readback || a_capture.width < 5U || a_capture.height < 5U)
    {
        return false;
    }
    const D3D12_RANGE readRange = {0U, static_cast<SIZE_T>(a_capture.readback->GetDesc().Width)};
    void *mapped = nullptr;
    if (FAILED(a_capture.readback->Map(0U, &readRange, &mapped)))
    {
        return false;
    }
    const auto *pixels = static_cast<const std::uint8_t *>(mapped);
    const std::size_t centerOffset = a_capture.footprint.Offset +
                                     (a_capture.height / 2U) * a_capture.footprint.Footprint.RowPitch +
                                     (a_capture.width / 2U) * 4U;
    const std::size_t cornerOffset = a_capture.footprint.Offset + 2U * a_capture.footprint.Footprint.RowPitch + 2U * 4U;
    const auto *center = pixels + centerOffset;
    const auto *corner = pixels + cornerOffset;
    const bool cornerIsClear = corner[0] == 0U && corner[1] == 0U && corner[2] == 0U && corner[3] == 255U;
    const bool depthColorIsCorrect = a_expectNearCube ? center[2] > 200U && center[0] < 100U && center[3] == 255U
                                                      : center[0] > 200U && center[2] < 100U && center[3] == 255U;
    const D3D12_RANGE writtenRange = {0U, 0U};
    a_capture.readback->Unmap(0U, &writtenRange);
    return cornerIsClear && depthColorIsCorrect;
}

D3d12ResizeScenePixelResult verify_d3d12_scene_resize_pixel_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                                      std::uint32_t a_height, bool a_useHardware,
                                                                      AssertContext &a_assertContext) noexcept
{
    constexpr std::int64_t k_noHardwareAdapterForProbe = 24;
    constexpr std::int64_t k_noSuitableAdapterForProbe = 25;
    const D3d12BackendDescriptor backendDescriptor = {
        a_useHardware ? D3d12AdapterPolicy::HighPerformanceHardware : D3d12AdapterPolicy::Warp,
        D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    Result<std::unique_ptr<D3d12Backend>> backendResult = create_d3d12_backend(backendDescriptor, a_assertContext);
    if (!backendResult)
    {
        const Error *error = backendResult.try_error();
        return a_useHardware && error != nullptr && error->code().domain() == "Cue.RHI.D3D12" &&
                       (error->code().value() == k_noHardwareAdapterForProbe ||
                        error->code().value() == k_noSuitableAdapterForProbe)
                   ? D3d12ResizeScenePixelResult::HardwareUnavailable
                   : D3d12ResizeScenePixelResult::Failed;
    }
    std::unique_ptr<D3d12Backend> backend = std::move(*backendResult.try_value());
    Result<std::unique_ptr<PresentationContext>> presentationResult =
        backend->create_windows_presentation(a_nativeWindow, a_width, a_height, PresentationDescriptor{false});
    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return D3d12ResizeScenePixelResult::Failed;
    }
    std::unique_ptr<PresentationContext> presentation = std::move(*presentationResult.try_value());
    D3d12PresentationContext *d3d12Presentation = dynamic_cast<D3d12PresentationContext *>(presentation.get());
    std::array<D3d12ScenePixelCapture, 6> captures;
    constexpr std::array<float, 16> identity = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    constexpr std::array<float, 16> rotationY90 = {0.0F, 0.0F, -1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                   1.0F, 0.0F, 0.0F,  0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    /// @brief 指定Depth順のSceneを実Presentationへ描き、Present前画素を個別Readbackへ記録する
    const auto captureFrame = [&](std::size_t a_index) noexcept
    {
        if (d3d12Presentation == nullptr || !d3d12Presentation->create_scene_capture_for_probe(captures[a_index]))
        {
            return false;
        }
        const std::uint32_t submittedIndex = presentation->current_back_buffer_index();
        std::array<PresentationSceneCube, 2> cubes = {PresentationSceneCube{identity},
                                                      PresentationSceneCube{rotationY90}};
        cubes[0].localToWorld[14] = 0.75F;
        cubes[1].localToWorld[14] = 0.95F;
        const std::size_t depthCase = a_index % 3U;
        if (depthCase == 2U)
        {
            std::swap(cubes[0], cubes[1]);
        }
        const std::span<const PresentationSceneCube> instances =
            depthCase == 0U ? std::span<const PresentationSceneCube>(cubes.data() + 1U, 1U)
                            : std::span<const PresentationSceneCube>(cubes);
        const PresentationSceneFrameDescriptor scene = {{0.0F, 0.0F, 0.0F, 1.0F}, identity, instances};
        d3d12Presentation->set_scene_capture_for_probe(&captures[a_index]);
        Result<PresentationFrameStatus> frame = presentation->present_scene_frame(scene);
        d3d12Presentation->set_scene_capture_for_probe(nullptr);
        const D3d12PresentationProbeReport report = d3d12Presentation->probe_report();
        const SceneOwnerProbeReport sceneOwners = d3d12Presentation->scene_owner_report_for_probe();
        const std::uint64_t submittedFence = static_cast<std::uint64_t>(a_index) + 1U;
        return frame && submittedIndex < k_d3d12FrameContextCount && report.lastSubmittedFence == submittedFence &&
               report.frameReuseFences[submittedIndex] == submittedFence && sceneOwners.hasDepthDsv &&
               sceneOwners.hasGeometry && sceneOwners.hasConstants;
    };

    bool valid = d3d12Presentation != nullptr && captureFrame(0U) && captureFrame(1U) && captureFrame(2U);
    const D3d12PresentationProbeReport beforeResize =
        valid ? d3d12Presentation->probe_report() : D3d12PresentationProbeReport{};
    const std::uint32_t nextWidth = a_width + 32U;
    const std::uint32_t nextHeight = a_height + 24U;
    if (valid)
    {
        valid = static_cast<bool>(presentation->resize(nextWidth, nextHeight));
    }
    const D3d12PresentationProbeReport afterResize =
        valid ? d3d12Presentation->probe_report() : D3d12PresentationProbeReport{};
    if (valid)
    {
        valid = captureFrame(3U) && captureFrame(4U) && captureFrame(5U);
    }
    const D3d12PresentationProbeReport afterDraw =
        valid ? d3d12Presentation->probe_report() : D3d12PresentationProbeReport{};
    valid = valid && beforeResize.hasSceneDepthDsv && beforeResize.sceneDepthWidth == a_width &&
            beforeResize.sceneDepthHeight == a_height && beforeResize.lastSubmittedFence == 3U &&
            !afterResize.hasSceneNativeObjects && afterResize.sceneDepthIdentity == 0U &&
            afterResize.lastSubmittedFence == 3U && afterDraw.hasSceneDepthDsv &&
            afterDraw.sceneDepthWidth == nextWidth && afterDraw.sceneDepthHeight == nextHeight &&
            afterDraw.lastSubmittedFence == 6U;
    Result<void> presentationShutdown = presentation->shutdown();
    presentation.reset();
    Result<void> backendShutdown = backend->shutdown();
    backend.reset();
    if (!presentationShutdown || !backendShutdown)
    {
        // GPU完了が未証明のままProbeのReadbackを解放しない
        std::abort();
    }
    for (std::size_t index = 0U; index < captures.size(); ++index)
    {
        const bool captureValid = validate_scene_resize_capture(captures[index], index % 3U != 0U);
        valid = valid && captureValid;
    }
    return valid ? D3d12ResizeScenePixelResult::Passed : D3d12ResizeScenePixelResult::Failed;
}

bool verify_d3d12_scene_resize_creation_failure_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                          std::uint32_t a_height,
                                                          AssertContext &a_assertContext) noexcept
{
    constexpr std::array faults = {D3d12SceneCreationFault::Depth, D3d12SceneCreationFault::DsvHeap};
    constexpr std::array<float, 16> identity = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const PresentationSceneFrameDescriptor scene = {{0.0F, 0.0F, 0.0F, 1.0F}, identity, {}};
    for (D3d12SceneCreationFault fault : faults)
    {
        D3d12BackendDescriptor backendDescriptor = {
            D3d12AdapterPolicy::Warp,
            D3d12ValidationMode::Disabled,
            false,
            5'000,
        };
        Result<std::unique_ptr<D3d12Backend>> backendResult = create_d3d12_backend(backendDescriptor, a_assertContext);
        if (!backendResult)
        {
            return false;
        }
        std::unique_ptr<D3d12Backend> backend = std::move(*backendResult.try_value());
        Result<std::unique_ptr<PresentationContext>> presentationResult =
            backend->create_windows_presentation(a_nativeWindow, a_width, a_height, PresentationDescriptor{true});
        if (!presentationResult)
        {
            static_cast<void>(backend->shutdown());
            return false;
        }
        std::unique_ptr<PresentationContext> presentation = std::move(*presentationResult.try_value());
        D3d12PresentationContext *d3d12Presentation = dynamic_cast<D3d12PresentationContext *>(presentation.get());
        if (d3d12Presentation == nullptr)
        {
            static_cast<void>(presentation->shutdown());
            presentation.reset();
            static_cast<void>(backend->shutdown());
            return false;
        }
        Result<PresentationFrameStatus> firstFrame = presentation->present_scene_frame(scene);
        if (!firstFrame)
        {
            static_cast<void>(presentation->shutdown());
            presentation.reset();
            static_cast<void>(backend->shutdown());
            return false;
        }
        Result<void> resizeResult = presentation->resize(a_width + 19U, a_height + 23U);
        const D3d12PresentationProbeReport resizedReport = probe_d3d12_presentation(*presentation);
        if (!resizeResult)
        {
            static_cast<void>(presentation->shutdown());
            presentation.reset();
            static_cast<void>(backend->shutdown());
            return false;
        }
        d3d12Presentation->set_scene_creation_fault_for_probe(fault);
        Result<PresentationFrameStatus> failedFrame = presentation->present_scene_frame(scene);
        d3d12Presentation->set_scene_creation_fault_for_probe(D3d12SceneCreationFault::None);
        const D3d12PresentationProbeReport failedReport = probe_d3d12_presentation(*presentation);
        Result<PresentationFrameStatus> recoveredFrame = presentation->present_scene_frame(scene);
        const D3d12PresentationProbeReport recoveredReport = probe_d3d12_presentation(*presentation);
        const Error *failureError = failedFrame.try_error();
        const NativeError *nativeError = failureError != nullptr ? failureError->try_native_error() : nullptr;
        const std::int64_t expectedCode = fault == D3d12SceneCreationFault::Depth ? 309 : 310;
        const bool valid =
            !resizedReport.hasSceneNativeObjects && resizedReport.sceneDepthIdentity == 0U && !failedFrame &&
            failureError != nullptr && failureError->code().domain() == "Cue.RHI.D3D12" &&
            failureError->code().value() == expectedCode && nativeError != nullptr &&
            nativeError->domain() == "D3D12" && presentation->state() == PresentationContextState::Ready &&
            backend->state() == GraphicsBackendState::Ready && !failedReport.hasSceneNativeObjects &&
            failedReport.sceneDepthIdentity == 0U && failedReport.lastSubmittedFence == 1U && recoveredFrame &&
            recoveredReport.hasSceneDepthDsv && recoveredReport.sceneDepthWidth == a_width + 19U &&
            recoveredReport.sceneDepthHeight == a_height + 23U && recoveredReport.lastSubmittedFence == 2U;
        Result<void> presentationShutdown = presentation->shutdown();
        presentation.reset();
        Result<void> backendShutdown = backend->shutdown();
        if (!valid || !presentationShutdown || !backendShutdown)
        {
            return false;
        }
    }
    return true;
}
#endif

bool verify_d3d12_terminal_resize_rejection_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                      std::uint32_t a_height, AssertContext &a_assertContext) noexcept
{
    struct ProbeReset final
    {
        /// @brief Queue Lifecycle Probe の Fault Injection 状態を検証終了時に必ず初期化する
        ~ProbeReset() noexcept
        {
            g_queueLifecycleProbeState = {};
        }
    } probeReset;

    static_cast<void>(probeReset);
    g_queueLifecycleProbeState.useProbeFunctions = true;
    D3d12BackendDescriptor backendDescriptor = {
        D3d12AdapterPolicy::Warp,
        D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    Result<std::unique_ptr<D3d12Backend>> backendResult = create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return false;
    }

    std::unique_ptr<D3d12Backend> backend = std::move(*backendResult.try_value());
    PresentationDescriptor presentationDescriptor = {true};
    Result<std::unique_ptr<PresentationContext>> presentationResult =
        backend->create_windows_presentation(a_nativeWindow, a_width, a_height, presentationDescriptor);

    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return false;
    }

    std::unique_ptr<PresentationContext> presentation = std::move(*presentationResult.try_value());
    D3d12PresentationContext *d3d12Presentation = dynamic_cast<D3d12PresentationContext *>(presentation.get());

    if (d3d12Presentation == nullptr)
    {
        static_cast<void>(presentation->shutdown());
        presentation.reset();
        static_cast<void>(backend->shutdown());
        return false;
    }

    g_queueLifecycleProbeState.failSignalAfterForwarding = true;
    Result<std::uint64_t> submitResult = d3d12Presentation->submit_transition_frame_for_probe();
    g_queueLifecycleProbeState.failSignalAfterForwarding = false;
    Result<void> resizeResult = presentation->resize(0, a_height);
    Result<void> sameSizeResult = presentation->resize(a_width, a_height);
    D3d12PresentationProbeReport report = d3d12Presentation->probe_report();
    const bool rejectionValid =
        !submitResult && !resizeResult && !sameSizeResult &&
        resizeResult.try_error()->code().domain() == "Cue.RHI.D3D12" &&
        resizeResult.try_error()->code().value() == 64 && presentation->state() == PresentationContextState::Ready &&
        !presentation->is_resize_pending() && !report.isAcceptingFrames && report.hasCommandList &&
        report.allocatorCount == k_d3d12FrameContextCount && report.backBufferCount == k_d3d12SwapChainBufferCount &&
        report.rtvCount == k_d3d12SwapChainBufferCount && report.hasSwapChain && report.hasRtvHeap &&
        report.isRegistered;
    Result<void> presentationShutdownResult = presentation->shutdown();
    presentation.reset();
    Result<void> backendShutdownResult = backend->shutdown();
    const bool cleanupValid =
        presentationShutdownResult && backendShutdownResult && backend->state() == GraphicsBackendState::Shutdown;
    backend.reset();
    return rejectionValid && cleanupValid;
}

#if CUE_D3D12_TESTING
bool verify_d3d12_present_signal_recovery_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                    std::uint32_t a_height, D3d12PresentFailureProbeMode a_mode,
                                                    AssertContext &a_assertContext) noexcept
{
    g_deviceRemovalProbeUnavailable = false;
    g_reportDeviceRemovedForProbe = false;
    g_lifecycleProbeQueue = nullptr;

    struct ProbeReset final
    {
        /// @brief Present と Queue の Fault Injection 状態を各検証 Case 後に必ず初期化する
        ~ProbeReset() noexcept
        {
            g_presentationFrameProbeState = {};
            g_queueLifecycleProbeState = {};
            g_reportDeviceRemovedForProbe = false;
            g_lifecycleProbeQueue = nullptr;
        }
    } probeReset;

    static_cast<void>(probeReset);
    constexpr std::array<float, 16> identity = {
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
    };
    const std::array sceneCubes = {PresentationSceneCube{identity}};
    const PresentationSceneFrameDescriptor scene = {{0.06F, 0.18F, 0.32F, 1.0F}, identity, sceneCubes};
    /// @brief Present、Signal、完了待機の失敗組み合わせが期待した主 Error を保持するか検証する
    const auto runCase =
        [&](bool a_failPresent, bool a_failSignal, bool a_failWaitAfterCompletion, std::int64_t a_expectedCode) noexcept
    {
        g_presentationFrameProbeState = {true, false, false};
        g_queueLifecycleProbeState = {true, false, false, false, false, 0};
        g_lifecycleProbeQueue = nullptr;
        D3d12BackendDescriptor backendDescriptor = {
            D3d12AdapterPolicy::Warp,
            D3d12ValidationMode::Disabled,
            false,
            5'000,
        };
        Result<std::unique_ptr<D3d12Backend>> backendResult = create_d3d12_backend(backendDescriptor, a_assertContext);

        if (!backendResult)
        {
            return false;
        }

        std::unique_ptr<D3d12Backend> backend = std::move(*backendResult.try_value());
        PresentationDescriptor presentationDescriptor = {true};
        Result<std::unique_ptr<PresentationContext>> presentationResult =
            backend->create_windows_presentation(a_nativeWindow, a_width, a_height, presentationDescriptor);

        if (!presentationResult)
        {
            static_cast<void>(backend->shutdown());
            return false;
        }

        std::unique_ptr<PresentationContext> presentation = std::move(*presentationResult.try_value());
        const std::uint32_t submittedIndex = presentation->current_back_buffer_index();
        g_presentationFrameProbeState.failPresent = a_failPresent;
        g_queueLifecycleProbeState.failSignalAfterForwarding = a_failSignal;
        g_queueLifecycleProbeState.failWaitAfterCompletion = a_failWaitAfterCompletion;
        g_queueLifecycleProbeState.hiddenCompletedValueCount = a_failWaitAfterCompletion ? 1 : 0;
        constexpr std::array<float, 4> color = {0.06F, 0.18F, 0.32F, 1.0F};
        PresentationFrameDescriptor frameDescriptor = {color};
        Result<PresentationFrameStatus> frameResult = presentation->present_frame(frameDescriptor);
        g_presentationFrameProbeState.failPresent = false;
        g_queueLifecycleProbeState.failSignalAfterForwarding = false;
        g_queueLifecycleProbeState.failWaitAfterCompletion = false;
        g_queueLifecycleProbeState.hiddenCompletedValueCount = 0;
        D3d12PresentationProbeReport report = probe_d3d12_presentation(*presentation);
        Result<D3d12BackendOwnerProbeReport> backendOwnerResult = probe_d3d12_backend_owners_for_probe(*backend);
        const Error *frameError = frameResult.try_error();
        const D3d12BackendOwnerProbeReport *backendOwners = backendOwnerResult.try_value();
        const bool signalErrorRetained =
            !a_failSignal || (frameError != nullptr &&
                              (frameError->code().value() == 46 || has_error_context(*frameError, "Cue.RHI.D3D12/46")));
        const bool waitErrorRetained =
            !a_failWaitAfterCompletion || (frameError != nullptr && has_error_context(*frameError, "Cue.RHI.D3D12/48"));
        const bool recoveryOrderValid =
            !a_failPresent || !a_failSignal || !a_failWaitAfterCompletion ||
            (frameError != nullptr && has_error_contexts_in_order(*frameError, "Cue.RHI.D3D12/46", "Cue.RHI.D3D12/48"));

        const bool frameValid =
            !frameResult && frameError != nullptr && signalErrorRetained && waitErrorRetained && recoveryOrderValid &&
            frameError->code().domain() == "Cue.RHI.D3D12" && frameError->code().value() == a_expectedCode &&
            presentation->state() == PresentationContextState::Ready && report.lastSubmittedFence == 1 &&
            report.frameReuseFences[submittedIndex] == 1 && !report.isAcceptingFrames && backendOwners != nullptr &&
            backendOwners->lastSignaledFence == 1;
        Result<void> presentationShutdownResult = presentation->shutdown();
        presentation.reset();
        Result<void> backendShutdownResult = backend->shutdown();
        const bool cleanupValid =
            presentationShutdownResult && backendShutdownResult && backend->state() == GraphicsBackendState::Shutdown;
        backend.reset();
        return frameValid && cleanupValid;
    };

    /// @brief Begin Frame が利用不能な場合に既存 Frame 状態が保持されることを検証する
    const auto runBeginFrameUnavailableCase = [&](bool a_useScene) noexcept
    {
        g_presentationFrameProbeState = {true, false, false};
        g_queueLifecycleProbeState = {true, false, false, false, false, 0};
        D3d12BackendDescriptor backendDescriptor = {
            D3d12AdapterPolicy::Warp,
            D3d12ValidationMode::Disabled,
            false,
            5'000,
        };
        Result<std::unique_ptr<D3d12Backend>> backendResult = create_d3d12_backend(backendDescriptor, a_assertContext);

        if (!backendResult)
        {
            return false;
        }

        std::unique_ptr<D3d12Backend> backend = std::move(*backendResult.try_value());
        PresentationDescriptor presentationDescriptor = {true};
        Result<std::unique_ptr<PresentationContext>> presentationResult =
            backend->create_windows_presentation(a_nativeWindow, a_width, a_height, presentationDescriptor);

        if (!presentationResult)
        {
            static_cast<void>(backend->shutdown());
            return false;
        }

        std::unique_ptr<PresentationContext> presentation = std::move(*presentationResult.try_value());
        constexpr std::array<float, 4> color = {0.06F, 0.18F, 0.32F, 1.0F};
        PresentationFrameDescriptor frameDescriptor = {color};
        Result<PresentationFrameStatus> firstFrameResult =
            a_useScene ? presentation->present_scene_frame(scene) : presentation->present_frame(frameDescriptor);
        Result<PresentationFrameStatus> secondFrameResult =
            a_useScene ? presentation->present_scene_frame(scene) : presentation->present_frame(frameDescriptor);
        D3d12PresentationProbeReport reportBeforeFailure = probe_d3d12_presentation(*presentation);
        const SceneOwnerProbeReport sceneOwnersBeforeFailure = probe_scene_owners_for_fault(*presentation);
        g_queueLifecycleProbeState.failWaitWithoutCompletion = true;
        Result<PresentationFrameStatus> frameResult =
            a_useScene ? presentation->present_scene_frame(scene) : presentation->present_frame(frameDescriptor);
        D3d12PresentationProbeReport report = probe_d3d12_presentation(*presentation);
        const SceneOwnerProbeReport sceneOwners = probe_scene_owners_for_fault(*presentation);
        Result<D3d12BackendOwnerProbeReport> backendOwnerResult = probe_d3d12_backend_owners_for_probe(*backend);
        Result<void> presentationShutdownResult = presentation->shutdown();
        Result<void> backendShutdownResult = backend->shutdown();
        const D3d12PresentationProbeReport afterShutdown = probe_d3d12_presentation(*presentation);
        const SceneOwnerProbeReport sceneOwnersAfterShutdown = probe_scene_owners_for_fault(*presentation);
        const Error *frameError = frameResult.try_error();
        const D3d12BackendOwnerProbeReport *backendOwners = backendOwnerResult.try_value();
        const bool valid =
            firstFrameResult && secondFrameResult && !frameResult && frameError != nullptr &&
            frameError->code().domain() == "Cue.RHI.D3D12" && frameError->code().value() == 48 &&
            has_error_context(*frameError, "Cue.RHI.D3D12/33") &&
            presentation->state() == PresentationContextState::Unavailable &&
            backend->state() == GraphicsBackendState::Unavailable && report.lastSubmittedFence == 2 &&
            reportBeforeFailure.lastSubmittedFence == 2 &&
            report.frameReuseFences == reportBeforeFailure.frameReuseFences && !report.isAcceptingFrames &&
            report.hasCommandList && report.allocatorCount == k_d3d12FrameContextCount &&
            report.backBufferCount == k_d3d12SwapChainBufferCount && report.rtvCount == k_d3d12SwapChainBufferCount &&
            report.formatsMatch && report.hasSwapChain && report.hasRtvHeap && report.isRegistered &&
            (!a_useScene || (sceneOwners.hasDepthDsv && sceneOwners.hasGeometry && sceneOwners.hasConstants &&
                             sceneOwnersBeforeFailure.hasGeometry && sceneOwnersBeforeFailure.hasConstants &&
                             report.sceneDepthIdentity != 0U &&
                             report.sceneDepthIdentity == reportBeforeFailure.sceneDepthIdentity)) &&
            backendOwners != nullptr && backendOwners->lastSignaledFence == 2 && backendOwners->hasQueue &&
            backendOwners->hasFence && backendOwners->hasFenceEvent && backendOwners->hasDevice &&
            backendOwners->hasAdapter && backendOwners->hasFactory && !presentationShutdownResult &&
            !backendShutdownResult &&
            (!a_useScene ||
             (sceneOwnersAfterShutdown.hasDepthDsv && sceneOwnersAfterShutdown.hasGeometry &&
              sceneOwnersAfterShutdown.hasConstants && afterShutdown.sceneDepthIdentity == report.sceneDepthIdentity));
        static_cast<void>(presentation.release());
        static_cast<void>(backend.release());
        g_presentationFrameProbeState = {};
        g_queueLifecycleProbeState = {};
        return valid;
    };

    /// @brief Command List Close 中の Device Removal が Backend 状態へ伝播することを検証する
    const auto runCloseFrameDeviceRemovedCase = [&]() noexcept
    {
        g_presentationFrameProbeState = {true, false, false, true, 0};
        g_queueLifecycleProbeState = {true, false, false, false, false, 0};
        D3d12BackendDescriptor backendDescriptor = {
            D3d12AdapterPolicy::Warp,
            D3d12ValidationMode::Disabled,
            false,
            5'000,
        };
        Result<std::unique_ptr<D3d12Backend>> backendResult = create_d3d12_backend(backendDescriptor, a_assertContext);

        if (!backendResult)
        {
            return false;
        }

        std::unique_ptr<D3d12Backend> backend = std::move(*backendResult.try_value());
        PresentationDescriptor presentationDescriptor = {true};
        Result<std::unique_ptr<PresentationContext>> presentationResult =
            backend->create_windows_presentation(a_nativeWindow, a_width, a_height, presentationDescriptor);

        if (!presentationResult)
        {
            static_cast<void>(backend->shutdown());
            return false;
        }

        std::unique_ptr<PresentationContext> presentation = std::move(*presentationResult.try_value());
        constexpr std::array<float, 4> color = {0.06F, 0.18F, 0.32F, 1.0F};
        PresentationFrameDescriptor frameDescriptor = {color};
        Result<PresentationFrameStatus> frameResult = presentation->present_frame(frameDescriptor);
        D3d12PresentationProbeReport report = probe_d3d12_presentation(*presentation);
        Result<D3d12BackendOwnerProbeReport> backendOwnerResult = probe_d3d12_backend_owners_for_probe(*backend);
        Result<D3d12DredOwnerProbeReport> dredOwnerResult = probe_d3d12_dred_owners_for_probe(*backend);
        Result<std::uint32_t> dredCountResult = d3d12_dred_attempt_count_for_probe(*backend);
        const Error *frameError = frameResult.try_error();
        const D3d12BackendOwnerProbeReport *backendOwners = backendOwnerResult.try_value();
        const D3d12DredOwnerProbeReport *dredOwners = dredOwnerResult.try_value();
        const bool frameValid =
            !frameResult && frameError != nullptr && frameError->code().domain() == "Cue.RHI.D3D12" &&
            frameError->code().value() == 52 && frameError->try_native_error() != nullptr &&
            !frameError->causes().empty() && frameError->causes().front().code().value() == 63 &&
            presentation->state() == PresentationContextState::DeviceRemoved &&
            backend->state() == GraphicsBackendState::DeviceRemoved && report.lastSubmittedFence == 0 &&
            report.frameReuseFences[0] == 0 && report.frameReuseFences[1] == 0 && !report.isAcceptingFrames &&
            report.hasCommandList && report.allocatorCount == k_d3d12FrameContextCount &&
            report.backBufferCount == k_d3d12SwapChainBufferCount && report.rtvCount == k_d3d12SwapChainBufferCount &&
            report.formatsMatch && report.hasSwapChain && report.hasRtvHeap && report.isRegistered &&
            backendOwners != nullptr && backendOwners->lastSignaledFence == 0 && backendOwners->hasQueue &&
            backendOwners->hasFence && backendOwners->hasFenceEvent && dredCountResult &&
            *dredCountResult.try_value() == 1 && dredOwners != nullptr && dredOwners->hasCommandList &&
            dredOwners->allocatorCount == 2 && dredOwners->backBufferCount == 2 && dredOwners->rtvCount == 2 &&
            dredOwners->hasSwapChain && dredOwners->hasRtvHeap && dredOwners->hasQueue && dredOwners->hasFence &&
            dredOwners->hasFenceEvent;
        Result<void> presentationShutdownResult = presentation->shutdown();
        presentation.reset();
        Result<void> backendShutdownResult = backend->shutdown();
        const bool cleanupValid =
            presentationShutdownResult && backendShutdownResult && backend->state() == GraphicsBackendState::Shutdown;
        backend.reset();
        g_presentationFrameProbeState = {};
        g_queueLifecycleProbeState = {};
        g_reportDeviceRemovedForProbe = false;
        return frameValid && cleanupValid;
    };

    /// @brief Present 経路が利用不能でも Frame 所有状態と期待 Error が保持されることを検証する
    const auto runUnavailableCase = [&](bool a_failPresent, std::int64_t a_expectedCode, bool a_useScene) noexcept
    {
        g_presentationFrameProbeState = {true, false, false};
        g_queueLifecycleProbeState = {true, false, false, false, false, 0};
        D3d12BackendDescriptor backendDescriptor = {
            D3d12AdapterPolicy::Warp,
            D3d12ValidationMode::Disabled,
            false,
            5'000,
        };
        Result<std::unique_ptr<D3d12Backend>> backendResult = create_d3d12_backend(backendDescriptor, a_assertContext);

        if (!backendResult)
        {
            return false;
        }

        std::unique_ptr<D3d12Backend> backend = std::move(*backendResult.try_value());
        PresentationDescriptor presentationDescriptor = {true};
        Result<std::unique_ptr<PresentationContext>> presentationResult =
            backend->create_windows_presentation(a_nativeWindow, a_width, a_height, presentationDescriptor);

        if (!presentationResult)
        {
            static_cast<void>(backend->shutdown());
            return false;
        }

        std::unique_ptr<PresentationContext> presentation = std::move(*presentationResult.try_value());
        if (a_useScene && !presentation->present_scene_frame(scene))
        {
            static_cast<void>(presentation->shutdown());
            static_cast<void>(backend->shutdown());
            return false;
        }
        const D3d12PresentationProbeReport beforeFault = probe_d3d12_presentation(*presentation);
        const SceneOwnerProbeReport sceneOwnersBeforeFault = probe_scene_owners_for_fault(*presentation);
        g_presentationFrameProbeState.failPresent = a_failPresent;
        g_queueLifecycleProbeState.failSignalAfterForwarding = true;
        g_queueLifecycleProbeState.failWaitWithoutCompletion = true;
        constexpr std::array<float, 4> color = {0.06F, 0.18F, 0.32F, 1.0F};
        PresentationFrameDescriptor frameDescriptor = {color};
        Result<PresentationFrameStatus> frameResult =
            a_useScene ? presentation->present_scene_frame(scene) : presentation->present_frame(frameDescriptor);
        D3d12PresentationProbeReport report = probe_d3d12_presentation(*presentation);
        const SceneOwnerProbeReport sceneOwners = probe_scene_owners_for_fault(*presentation);
        Result<D3d12BackendOwnerProbeReport> backendOwnerResult = probe_d3d12_backend_owners_for_probe(*backend);
        Result<void> presentationShutdownResult = presentation->shutdown();
        Result<void> backendShutdownResult = backend->shutdown();
        const D3d12PresentationProbeReport afterShutdown = probe_d3d12_presentation(*presentation);
        const SceneOwnerProbeReport sceneOwnersAfterShutdown = probe_scene_owners_for_fault(*presentation);
        const Error *frameError = frameResult.try_error();
        const D3d12BackendOwnerProbeReport *backendOwners = backendOwnerResult.try_value();
        const bool errorOrderValid =
            frameError != nullptr &&
            (a_failPresent
                 ? has_error_contexts_in_order(*frameError, "Cue.RHI.D3D12/46", "Cue.RHI.D3D12/48", "Cue.RHI.D3D12/33")
                 : has_error_contexts_in_order(*frameError, "Cue.RHI.D3D12/48", "Cue.RHI.D3D12/33"));
        const bool valid =
            !frameResult && frameError != nullptr && frameError->code().domain() == "Cue.RHI.D3D12" &&
            frameError->code().value() == a_expectedCode && errorOrderValid &&
            presentation->state() == PresentationContextState::Unavailable &&
            backend->state() == GraphicsBackendState::Unavailable &&
            report.lastSubmittedFence == beforeFault.lastSubmittedFence &&
            report.frameReuseFences == beforeFault.frameReuseFences && !report.isAcceptingFrames &&
            report.hasCommandList && report.allocatorCount == k_d3d12FrameContextCount &&
            report.backBufferCount == k_d3d12SwapChainBufferCount && report.rtvCount == k_d3d12SwapChainBufferCount &&
            report.formatsMatch && report.hasSwapChain && report.hasRtvHeap && report.isRegistered &&
            (!a_useScene || (beforeFault.lastSubmittedFence == 1U && sceneOwnersBeforeFault.hasGeometry &&
                             sceneOwnersBeforeFault.hasConstants && sceneOwners.hasDepthDsv &&
                             sceneOwners.hasGeometry && sceneOwners.hasConstants && report.sceneDepthIdentity != 0U &&
                             report.sceneDepthIdentity == beforeFault.sceneDepthIdentity)) &&
            backendOwners != nullptr && backendOwners->lastSignaledFence == beforeFault.lastSubmittedFence &&
            backendOwners->hasQueue && backendOwners->hasFence && backendOwners->hasFenceEvent &&
            backendOwners->hasDevice && backendOwners->hasAdapter && backendOwners->hasFactory &&
            !presentationShutdownResult && !backendShutdownResult &&
            (!a_useScene ||
             (sceneOwnersAfterShutdown.hasDepthDsv && sceneOwnersAfterShutdown.hasGeometry &&
              sceneOwnersAfterShutdown.hasConstants && afterShutdown.sceneDepthIdentity == report.sceneDepthIdentity));
        static_cast<void>(presentation.release());
        static_cast<void>(backend.release());
        g_presentationFrameProbeState = {};
        g_queueLifecycleProbeState = {};
        return valid;
    };

    /// @brief Present 前後の Device Removal 位置ごとに原因 Code と Backend 状態を検証する
    const auto runDeviceRemovedCase = [&](bool a_removeBeforePresent, bool a_failPresent, bool a_removeBeforeSignal,
                                          std::int64_t a_expectedCauseCode, bool a_useScene) noexcept
    {
        g_presentationFrameProbeState = {true, false, false};
        g_queueLifecycleProbeState = {true, false, false, false, false, 0};
        D3d12BackendDescriptor backendDescriptor = {
            D3d12AdapterPolicy::Warp,
            D3d12ValidationMode::Disabled,
            false,
            5'000,
        };
        Result<std::unique_ptr<D3d12Backend>> backendResult = create_d3d12_backend(backendDescriptor, a_assertContext);

        if (!backendResult)
        {
            return false;
        }

        std::unique_ptr<D3d12Backend> backend = std::move(*backendResult.try_value());
        PresentationDescriptor presentationDescriptor = {true};
        Result<std::unique_ptr<PresentationContext>> presentationResult =
            backend->create_windows_presentation(a_nativeWindow, a_width, a_height, presentationDescriptor);

        if (!presentationResult)
        {
            static_cast<void>(backend->shutdown());
            return false;
        }

        std::unique_ptr<PresentationContext> presentation = std::move(*presentationResult.try_value());
        if (a_useScene && !presentation->present_scene_frame(scene))
        {
            static_cast<void>(presentation->shutdown());
            static_cast<void>(backend->shutdown());
            return false;
        }
        const D3d12PresentationProbeReport beforeFault = probe_d3d12_presentation(*presentation);
        const SceneOwnerProbeReport sceneOwnersBeforeFault = probe_scene_owners_for_fault(*presentation);
        g_presentationFrameProbeState.failPresent = a_failPresent;
        g_presentationFrameProbeState.removeDeviceBeforePresent = a_removeBeforePresent;
        g_queueLifecycleProbeState.removeDeviceBeforeSignal = a_removeBeforeSignal;
        g_queueLifecycleProbeState.failSignalAfterForwarding = a_removeBeforeSignal;
        constexpr std::array<float, 4> color = {0.06F, 0.18F, 0.32F, 1.0F};
        PresentationFrameDescriptor frameDescriptor = {color};
        Result<PresentationFrameStatus> frameResult =
            a_useScene ? presentation->present_scene_frame(scene) : presentation->present_frame(frameDescriptor);
        const bool removalProbeAvailable = !g_deviceRemovalProbeUnavailable;
        g_presentationFrameProbeState = {};
        g_queueLifecycleProbeState = {};
        D3d12PresentationProbeReport report = probe_d3d12_presentation(*presentation);
        const SceneOwnerProbeReport sceneOwners = probe_scene_owners_for_fault(*presentation);
        const SceneOwnerProbeReport dredSceneOwners = probe_dred_scene_owners_for_fault(*backend);
        Result<D3d12BackendOwnerProbeReport> backendOwnerResult = probe_d3d12_backend_owners_for_probe(*backend);
        Result<D3d12DredOwnerProbeReport> dredOwnerResult = probe_d3d12_dred_owners_for_probe(*backend);
        Result<std::uint32_t> dredCountResult = d3d12_dred_attempt_count_for_probe(*backend);
        const Error *frameError = frameResult.try_error();
        const D3d12BackendOwnerProbeReport *backendOwners = backendOwnerResult.try_value();
        const D3d12DredOwnerProbeReport *dredOwners = dredOwnerResult.try_value();
        const PresentationContextState presentationStateBeforeShutdown = presentation->state();
        const GraphicsBackendState backendStateBeforeShutdown = backend->state();
        const bool recoverySignalErrorValid =
            !a_failPresent || !a_removeBeforeSignal ||
            (frameError != nullptr &&
             has_error_contexts_in_order(*frameError, "Cue.RHI.D3D12/46", "Cue.RHI.D3D12/50") &&
             has_error_context(*frameError, "Secondary shutdown Error Cause NativeError=D3D12/-2005270523"));
        const NativeError *immediateCauseNativeError = frameError == nullptr || frameError->causes().empty()
                                                           ? nullptr
                                                           : frameError->causes().front().try_native_error();
        const bool directPresentNativeErrorValid =
            !a_removeBeforePresent ||
            (immediateCauseNativeError != nullptr &&
             immediateCauseNativeError->value() == static_cast<std::int64_t>(DXGI_ERROR_DEVICE_REMOVED));
        const bool regularSignalNativeErrorValid =
            !a_removeBeforeSignal || a_failPresent ||
            (immediateCauseNativeError != nullptr &&
             immediateCauseNativeError->value() == static_cast<std::int64_t>(DXGI_ERROR_DEVICE_REMOVED));
        const bool frameValid =
            removalProbeAvailable && !frameResult && frameError != nullptr &&
            frameError->code().domain() == "Cue.RHI.D3D12" && frameError->code().value() == 52 &&
            frameError->try_native_error() != nullptr && !frameError->causes().empty() &&
            frameError->causes().front().code().value() == a_expectedCauseCode && recoverySignalErrorValid &&
            directPresentNativeErrorValid && regularSignalNativeErrorValid &&
            presentationStateBeforeShutdown == PresentationContextState::DeviceRemoved &&
            backendStateBeforeShutdown == GraphicsBackendState::DeviceRemoved &&
            report.lastSubmittedFence == beforeFault.lastSubmittedFence &&
            report.frameReuseFences == beforeFault.frameReuseFences && !report.isAcceptingFrames &&
            report.hasCommandList && report.hasSwapChain && report.hasRtvHeap && report.isRegistered &&
            (!a_useScene || (beforeFault.lastSubmittedFence == 1U && sceneOwnersBeforeFault.hasGeometry &&
                             sceneOwnersBeforeFault.hasConstants && sceneOwners.hasDepthDsv &&
                             sceneOwners.hasGeometry && sceneOwners.hasConstants && report.sceneDepthIdentity != 0U &&
                             report.sceneDepthIdentity == beforeFault.sceneDepthIdentity)) &&
            backendOwners != nullptr && backendOwners->lastSignaledFence == beforeFault.lastSubmittedFence &&
            backendOwners->hasQueue && backendOwners->hasFence && backendOwners->hasFenceEvent && dredCountResult &&
            *dredCountResult.try_value() == 1 && dredOwners != nullptr && dredOwners->hasCommandList &&
            dredOwners->allocatorCount == 2 && dredOwners->backBufferCount == 2 && dredOwners->rtvCount == 2 &&
            dredOwners->hasSwapChain && dredOwners->hasRtvHeap && dredOwners->hasQueue && dredOwners->hasFence &&
            dredOwners->hasFenceEvent &&
            (!a_useScene ||
             (dredSceneOwners.hasDepthDsv && dredSceneOwners.hasGeometry && dredSceneOwners.hasConstants));
        Result<void> presentationShutdownResult = presentation->shutdown();
        const D3d12PresentationProbeReport afterShutdown = probe_d3d12_presentation(*presentation);
        presentation.reset();
        Result<void> backendShutdownResult = backend->shutdown();
        const bool cleanupValid = presentationShutdownResult && backendShutdownResult &&
                                  backend->state() == GraphicsBackendState::Shutdown &&
                                  (!a_useScene || !afterShutdown.hasSceneNativeObjects);
        backend.reset();
        return frameValid && cleanupValid;
    };

    if (a_mode == D3d12PresentFailureProbeMode::PresentUnavailable)
    {
        return runUnavailableCase(true, 98, false);
    }

    if (a_mode == D3d12PresentFailureProbeMode::BeginFrameUnavailable)
    {
        return runBeginFrameUnavailableCase(false);
    }

    if (a_mode == D3d12PresentFailureProbeMode::CloseFrameDeviceRemoved)
    {
        return runCloseFrameDeviceRemovedCase();
    }

    if (a_mode == D3d12PresentFailureProbeMode::SignalUnavailable)
    {
        return runUnavailableCase(false, 46, false);
    }

    if (a_mode == D3d12PresentFailureProbeMode::DirectPresentDeviceRemoved)
    {
        return runDeviceRemovedCase(true, false, false, 98, false);
    }

    if (a_mode == D3d12PresentFailureProbeMode::RecoverySignalDeviceRemoved)
    {
        return runDeviceRemovedCase(false, true, true, 98, false);
    }

    if (a_mode == D3d12PresentFailureProbeMode::RegularSignalDeviceRemoved)
    {
        return runDeviceRemovedCase(false, false, true, 46, false);
    }

    if (a_mode == D3d12PresentFailureProbeMode::SceneBeginFrameUnavailable)
    {
        return runBeginFrameUnavailableCase(true);
    }

    if (a_mode == D3d12PresentFailureProbeMode::SceneSignalUnavailable)
    {
        return runUnavailableCase(false, 46, true);
    }

    if (a_mode == D3d12PresentFailureProbeMode::SceneDirectPresentDeviceRemoved)
    {
        return runDeviceRemovedCase(true, false, false, 98, true);
    }

    return runCase(true, false, false, 98) && runCase(false, true, false, 46) && runCase(true, true, true, 98);
}

bool was_d3d12_present_device_removal_probe_unavailable() noexcept
{
    return g_deviceRemovalProbeUnavailable;
}
#endif

bool verify_d3d12_resize_unavailable_retention_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                         std::uint32_t a_height,
                                                         AssertContext &a_assertContext) noexcept
{
    struct ProbeReset final
    {
        /// @brief Queue Lifecycle Probe の状態を Signal Recovery 検証終了時に必ず初期化する
        ~ProbeReset() noexcept
        {
            g_queueLifecycleProbeState = {};
        }
    } probeReset;

    static_cast<void>(probeReset);
    g_queueLifecycleProbeState.useProbeFunctions = true;
    D3d12BackendDescriptor backendDescriptor = {
        D3d12AdapterPolicy::Warp,
        D3d12ValidationMode::Disabled,
        false,
        5'000,
    };
    Result<std::unique_ptr<D3d12Backend>> backendResult = create_d3d12_backend(backendDescriptor, a_assertContext);

    if (!backendResult)
    {
        return false;
    }

    std::unique_ptr<D3d12Backend> backend = std::move(*backendResult.try_value());
    PresentationDescriptor presentationDescriptor = {true};
    Result<std::unique_ptr<PresentationContext>> presentationResult =
        backend->create_windows_presentation(a_nativeWindow, a_width, a_height, presentationDescriptor);

    if (!presentationResult)
    {
        static_cast<void>(backend->shutdown());
        return false;
    }

    std::unique_ptr<PresentationContext> presentation = std::move(*presentationResult.try_value());
    D3d12PresentationContext *d3d12Presentation = dynamic_cast<D3d12PresentationContext *>(presentation.get());

    if (d3d12Presentation == nullptr)
    {
        static_cast<void>(presentation->shutdown());
        presentation.reset();
        static_cast<void>(backend->shutdown());
        return false;
    }

    constexpr std::array<float, 16> identity = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const PresentationSceneFrameDescriptor scene = {{0.0F, 0.0F, 0.0F, 1.0F}, identity, {}};
    Result<PresentationFrameStatus> submitResult = presentation->present_scene_frame(scene);
    const D3d12PresentationProbeReport beforeResize = d3d12Presentation->probe_report();
    g_queueLifecycleProbeState.failWaitWithoutCompletion = true;
    Result<void> resizeResult = presentation->resize(a_width + 1, a_height + 1);
    D3d12PresentationProbeReport report = d3d12Presentation->probe_report();
    Result<D3d12BackendOwnerProbeReport> backendOwnerResult = probe_d3d12_backend_owners_for_probe(*backend);
    Result<void> presentationShutdownResult = presentation->shutdown();
    Result<void> backendShutdownResult = backend->shutdown();
    const D3d12BackendOwnerProbeReport *backendOwners = backendOwnerResult.try_value();
    const bool retentionValid =
        submitResult && beforeResize.hasSceneDepthDsv && beforeResize.sceneDepthIdentity != 0U &&
        beforeResize.sceneDepthWidth == a_width && beforeResize.sceneDepthHeight == a_height && !resizeResult &&
        presentation->state() == PresentationContextState::Unavailable &&
        backend->state() == GraphicsBackendState::Unavailable && !presentationShutdownResult &&
        !backendShutdownResult && report.hasCommandList && report.allocatorCount == k_d3d12FrameContextCount &&
        report.backBufferCount == k_d3d12SwapChainBufferCount && report.rtvCount == k_d3d12SwapChainBufferCount &&
        report.formatsMatch && !report.isAcceptingFrames && report.hasSwapChain && report.hasRtvHeap &&
        report.hasSceneDepthDsv && report.sceneDepthIdentity == beforeResize.sceneDepthIdentity &&
        report.sceneDepthWidth == a_width && report.sceneDepthHeight == a_height && report.isRegistered &&
        backendOwners != nullptr && backendOwners->hasQueue && backendOwners->hasFence &&
        backendOwners->hasFenceEvent && backendOwners->hasDevice && backendOwners->hasAdapter &&
        backendOwners->hasFactory;

    static_cast<void>(presentation.release());
    static_cast<void>(backend.release());
    return retentionValid;
}

// === Production Factory ===
// 検証専用 Probe 群を終え、正式な D3D12 Backend 生成 API を実装する
Result<std::unique_ptr<D3d12Backend>> create_d3d12_backend(const D3d12BackendDescriptor &a_descriptor,
                                                           AssertContext &a_assertContext) noexcept
{
    if (a_descriptor.gpuWaitTimeoutMilliseconds == 0 ||
        a_descriptor.gpuWaitTimeoutMilliseconds > k_maximumWaitTimeoutMilliseconds)
    {
        return Result<std::unique_ptr<D3d12Backend>>::failure(make_error(
            a_assertContext, k_invalidWaitTimeout, "D3D12 GPU Wait Timeout must be between 1 and 60000 milliseconds"));
    }

    Result<D3d12DiagnosticsStatus> diagnosticsResult =
        configure_d3d12_pre_device_diagnostics(a_descriptor, a_assertContext);

    if (!diagnosticsResult)
    {
        return Result<std::unique_ptr<D3d12Backend>>::failure(std::move(*diagnosticsResult.try_error()));
    }

    D3d12DiagnosticsStatus diagnostics = *diagnosticsResult.try_value();
    Result<D3d12AdapterSelection> selectionResult =
        select_d3d12_adapter(a_descriptor.adapterPolicy, diagnostics, a_assertContext);

    if (!selectionResult)
    {
        return Result<std::unique_ptr<D3d12Backend>>::failure(std::move(*selectionResult.try_error()));
    }

    D3d12AdapterSelection selection = std::move(*selectionResult.try_value());
    Result<Microsoft::WRL::ComPtr<ID3D12Device>> deviceResult =
        create_d3d12_device(selection.adapter.Get(), selection.featureLevel, a_assertContext);

    if (!deviceResult)
    {
        return Result<std::unique_ptr<D3d12Backend>>::failure(std::move(*deviceResult.try_error()));
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device = std::move(*deviceResult.try_value());
    HRESULT nameResult = device->SetName(L"CueEngine D3D12 Device");

    if (FAILED(nameResult))
    {
        return Result<std::unique_ptr<D3d12Backend>>::failure(make_native_error(
            a_assertContext, k_deviceNameFailed, "D3D12 Device diagnostic name could not be set", nameResult));
    }

    Result<void> infoQueueResult = configure_d3d12_info_queue(device.Get(), diagnostics, a_assertContext);

    if (!infoQueueResult)
    {
        return Result<std::unique_ptr<D3d12Backend>>::failure(std::move(*infoQueueResult.try_error()));
    }

    Result<CapabilityReport> capabilityReportResult =
        query_d3d12_capability_report(device.Get(), selection.report, a_assertContext);

    if (!capabilityReportResult)
    {
        return Result<std::unique_ptr<D3d12Backend>>::failure(std::move(*capabilityReportResult.try_error()));
    }

    D3d12QueueNativeFunctions queueFunctions = g_queueLifecycleProbeState.useProbeFunctions
                                                   ? make_queue_lifecycle_probe_functions()
                                                   : default_d3d12_queue_native_functions();
    Result<D3d12QueueState> queueStateResult = create_d3d12_queue_state(
        device.Get(), a_descriptor.gpuWaitTimeoutMilliseconds, a_assertContext, queueFunctions);

    if (!queueStateResult)
    {
        return Result<std::unique_ptr<D3d12Backend>>::failure(std::move(*queueStateResult.try_error()));
    }

    try
    {
        std::unique_ptr<D3d12Backend> backend = std::make_unique<D3d12BackendImpl>(
            std::move(selection), std::move(device), std::move(*queueStateResult.try_value()), diagnostics,
            std::move(*capabilityReportResult.try_value()), a_assertContext);
        return Result<std::unique_ptr<D3d12Backend>>::success(std::move(backend));
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
}
} // namespace cue
