// Command List の記録、Back Buffer 状態遷移、Clear、Submit 後の Fence 関連付けを Frame 単位で実行する
// Allocator や Back Buffer を再利用する前に対応 Fence を待ち、GPU 使用中の上書きを防ぐ

#include "D3d12FrameCommandState.h"

#include "D3d12Error.h"
#include "D3d12QueueState.h"
#include "D3d12ScenePass.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>

#include <limits>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include <pix.h>

namespace
{
using cue::d3d12_private::make_code;
using cue::d3d12_private::make_error;
using cue::d3d12_private::make_native_error;
using cue::d3d12_private::set_object_name;

constexpr std::int64_t k_commandAllocatorCreationFailed = 54;
constexpr std::int64_t k_commandAllocatorNameFailed = 55;
constexpr std::int64_t k_commandListCreationFailed = 56;
constexpr std::int64_t k_commandListNameFailed = 57;
constexpr std::int64_t k_commandListInitialCloseFailed = 58;
constexpr std::int64_t k_invalidFrameIndex = 59;
constexpr std::int64_t k_invalidCommandListState = 60;
constexpr std::int64_t k_commandAllocatorResetFailed = 61;
constexpr std::int64_t k_commandListResetFailed = 62;
constexpr std::int64_t k_commandListCloseFailed = 63;
constexpr std::int64_t k_frameAcceptanceStopped = 64;
constexpr std::int64_t k_invalidFrameResource = 65;
constexpr std::int64_t k_invalidBackBufferTransition = 94;
constexpr std::int64_t k_invalidBackBufferResource = 95;
constexpr std::int64_t k_invalidBackBufferClear = 97;
constexpr std::int64_t k_invalidSceneRecord = 307;
constexpr std::int64_t k_fenceValueExhausted = 45;
constexpr std::uint32_t k_invalidFrameIndexValue = (std::numeric_limits<std::uint32_t>::max)();

/// @brief D3D12 Frame Command State で使用する Command Allocator を生成し、呼び出し元へ返す
HRESULT create_command_allocator(ID3D12Device *a_device, ID3D12CommandAllocator **a_allocator) noexcept
{
    return a_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a_allocator));
}

/// @brief D3D12 Frame Command State で使用する Command List を生成し、呼び出し元へ返す
HRESULT create_command_list(ID3D12Device *a_device, ID3D12CommandAllocator *a_allocator,
                            ID3D12GraphicsCommandList **a_commandList) noexcept
{
    return a_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_allocator, nullptr,
                                       IID_PPV_ARGS(a_commandList));
}

/// @brief D3D12 Frame Command State の Command Allocator を次の処理で再利用可能な初期状態へ戻す
HRESULT reset_command_allocator(ID3D12CommandAllocator *a_allocator) noexcept
{
    return a_allocator->Reset();
}

/// @brief D3D12 Frame Command State の Command List を次の処理で再利用可能な初期状態へ戻す
HRESULT reset_command_list(ID3D12GraphicsCommandList *a_commandList, ID3D12CommandAllocator *a_allocator) noexcept
{
    return a_commandList->Reset(a_allocator, nullptr);
}

/// @brief D3D12 Command List の Recording を終了し、実行可能な状態へ確定する
HRESULT close_command_list(ID3D12GraphicsCommandList *a_commandList) noexcept
{
    return a_commandList->Close();
}

/// @brief Back Buffer の Resource State 遷移を D3D12 Command List へ記録する
void resource_barrier(ID3D12GraphicsCommandList *a_commandList, UINT a_barrierCount,
                      const D3D12_RESOURCE_BARRIER *a_barriers) noexcept
{
    a_commandList->ResourceBarrier(a_barrierCount, a_barriers);
}

/// @brief D3D12 Frame Command State の Marker を整合性を保って更新する
void set_marker(ID3D12GraphicsCommandList *a_commandList, PCSTR a_name) noexcept
{
    PIXSetMarker(a_commandList, PIX_COLOR_DEFAULT, a_name);
}

/// @brief 指定 RTV を Clear Color で初期化する Command を記録する
void clear_render_target_view(ID3D12GraphicsCommandList *a_commandList, D3D12_CPU_DESCRIPTOR_HANDLE a_handle,
                              const FLOAT a_color[4], UINT a_rectangleCount,
                              const D3D12_RECT *a_rectangles) noexcept
{
    a_commandList->ClearRenderTargetView(a_handle, a_color, a_rectangleCount, a_rectangles);
}

/// @brief D3D12 Frame Command State の Fence Exhaustion 条件を判定して返す
[[nodiscard]] bool is_fence_exhaustion(const cue::Error &a_error) noexcept
{
    return a_error.code().domain() == "Cue.RHI.D3D12" && a_error.code().value() == k_fenceValueExhausted;
}

/// @brief 主因 Error を失わず Secondary Error の識別情報を Context へ追加する
void add_secondary_error_context(cue::Error &a_primaryError, const cue::Error &a_secondaryError,
                                 std::string_view a_context, const cue::AssertContext &a_assertContext,
                                 std::source_location a_location = std::source_location::current()) noexcept
{
    a_primaryError.append_secondary_diagnostics(
        a_assertContext, a_secondaryError, a_context,
        "Secondary Frame Command Error", a_location);
}
} // namespace

namespace cue
{
const D3d12FrameCommandNativeFunctions &default_d3d12_frame_command_native_functions() noexcept
{
    static const D3d12FrameCommandNativeFunctions functions = {
        create_command_allocator, create_command_list, set_object_name,
        reset_command_allocator,  reset_command_list,  close_command_list,
        resource_barrier,         set_marker,          clear_render_target_view,
    };
    return functions;
}

D3d12FrameCommandState::D3d12FrameCommandState(std::array<D3d12FrameContext, k_d3d12FrameContextCount> &&a_frames,
                                               Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> a_commandList,
                                               D3d12QueueState &a_queueState, const AssertContext &a_assertContext,
                                               const D3d12FrameCommandNativeFunctions &a_functions) noexcept
    : m_frames(std::move(a_frames)), m_commandList(std::move(a_commandList)), m_queueState(&a_queueState),
      m_assertContext(&a_assertContext), m_functions(a_functions), m_lastSubmittedFence(0), m_pendingFenceValue(0),
      m_activeFrameIndex(k_invalidFrameIndexValue), m_commandListState(D3d12CommandListState::IdleClosed),
      m_status(D3d12FrameCommandStatus::Ready), m_acceptingFrames(true), m_isResizeSuspended(false),
      m_resizeGpuIdleProven(false)
{
}

D3d12FrameCommandState::D3d12FrameCommandState(D3d12FrameCommandState &&a_other) noexcept
    : m_queueState(nullptr), m_assertContext(nullptr), m_functions({}), m_lastSubmittedFence(0), m_pendingFenceValue(0),
      m_activeFrameIndex(k_invalidFrameIndexValue), m_commandListState(D3d12CommandListState::IdleClosed),
      m_status(D3d12FrameCommandStatus::Shutdown), m_acceptingFrames(false), m_isResizeSuspended(false),
      m_resizeGpuIdleProven(false)
{
    take_from(std::move(a_other));
}

D3d12FrameCommandState &D3d12FrameCommandState::operator=(D3d12FrameCommandState &&a_other) noexcept
{
    if (this != &a_other)
    {
        if (has_native_objects())
        {
            m_assertContext->fatal_handler().terminate(
                "D3D12 Frame Command owner move assignment requires an empty target");
        }

        take_from(std::move(a_other));
    }

    return *this;
}

D3d12FrameCommandState::~D3d12FrameCommandState() noexcept
{
    if (has_native_objects())
    {
        m_assertContext->fatal_handler().terminate("D3D12 Frame Command owner was destroyed before shutdown");
    }
}

void D3d12FrameCommandState::take_from(D3d12FrameCommandState &&a_other) noexcept
{
    m_frames = std::move(a_other.m_frames);
    m_commandList = std::move(a_other.m_commandList);
    m_queueState = a_other.m_queueState;
    m_assertContext = a_other.m_assertContext;
    m_functions = a_other.m_functions;
    m_lastSubmittedFence = a_other.m_lastSubmittedFence;
    m_pendingFenceValue = a_other.m_pendingFenceValue;
    m_activeFrameIndex = a_other.m_activeFrameIndex;
    m_commandListState = a_other.m_commandListState;
    m_status = a_other.m_status;
    m_acceptingFrames = a_other.m_acceptingFrames;
    m_isResizeSuspended = a_other.m_isResizeSuspended;
    m_resizeGpuIdleProven = a_other.m_resizeGpuIdleProven;

    a_other.m_queueState = nullptr;
    a_other.m_assertContext = nullptr;
    a_other.m_lastSubmittedFence = 0;
    a_other.m_pendingFenceValue = 0;
    a_other.m_activeFrameIndex = k_invalidFrameIndexValue;
    a_other.m_status = D3d12FrameCommandStatus::Shutdown;
    a_other.m_acceptingFrames = false;
    a_other.m_isResizeSuspended = false;
    a_other.m_resizeGpuIdleProven = false;

    for (D3d12FrameContext &frame : a_other.m_frames)
    {
        frame.backBuffer.Reset();
        frame.rtvSlot.reset();
        frame.backBufferState = D3d12BackBufferState::Unknown;
        frame.reuseFenceValue = 0;
    }
}

void D3d12FrameCommandState::update_status_from_queue() noexcept
{
    if (m_queueState->status() == D3d12QueueStateStatus::DeviceRemoved)
    {
        m_status = D3d12FrameCommandStatus::DeviceRemoved;
        m_acceptingFrames = false;
    }
    else if (m_queueState->status() == D3d12QueueStateStatus::Unavailable)
    {
        m_status = D3d12FrameCommandStatus::Unavailable;
        m_acceptingFrames = false;
    }
}

// 対応 Frame Context の Fence 完了後だけ Allocator と Command List を Reset し、GPU 使用中の Memory を上書きしない
Result<void> D3d12FrameCommandState::begin_frame(std::uint32_t a_frameIndex) noexcept
{
    update_status_from_queue();

    if (!m_acceptingFrames || m_status != D3d12FrameCommandStatus::Ready)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Frame acceptance is stopped"));
    }

    if (a_frameIndex >= k_d3d12FrameContextCount)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Frame Context index is out of range");
        m_acceptingFrames = false;
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidFrameIndex, "D3D12 Frame Context index is out of range"));
    }

    if (m_commandListState != D3d12CommandListState::IdleClosed &&
        m_commandListState != D3d12CommandListState::Submitted)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 begin_frame requires an idle or submitted Command List");
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidCommandListState, "D3D12 begin_frame state is invalid"));
    }

    D3d12FrameContext &frame = m_frames[a_frameIndex];
    Result<void> waitResult = m_queueState->wait_for_fence(frame.reuseFenceValue, D3d12FenceWaitPurpose::Reusable);

    if (!waitResult)
    {
        update_status_from_queue();
        return Result<void>::failure(std::move(*waitResult.try_error()));
    }

    const HRESULT allocatorResetResult = m_functions.resetCommandAllocator(frame.allocator.Get());

    if (FAILED(allocatorResetResult))
    {
        return handle_reset_failure(make_native_error(*m_assertContext, k_commandAllocatorResetFailed,
                                                      "D3D12 Frame Command Allocator reset failed",
                                                      allocatorResetResult));
    }

    const HRESULT listResetResult = m_functions.resetCommandList(m_commandList.Get(), frame.allocator.Get());

    if (FAILED(listResetResult))
    {
        return handle_reset_failure(make_native_error(*m_assertContext, k_commandListResetFailed,
                                                      "D3D12 Graphics Command List reset failed", listResetResult));
    }

    m_activeFrameIndex = a_frameIndex;
    m_commandListState = D3d12CommandListState::Recording;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::handle_reset_failure(Error &&a_error) noexcept
{
    Result<void> result = m_queueState->reclassify_device_failure(std::move(a_error));
    update_status_from_queue();

    if (m_status == D3d12FrameCommandStatus::Ready)
    {
        m_commandListState = D3d12CommandListState::FrameResetFailed;
        m_acceptingFrames = false;
    }

    return result;
}

Result<void> D3d12FrameCommandState::close_frame() noexcept
{
    update_status_from_queue();

    if (m_status != D3d12FrameCommandStatus::Ready)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Frame acceptance is stopped"));
    }

    if (m_commandListState != D3d12CommandListState::Recording)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 close_frame requires a recording Command List");
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidCommandListState, "D3D12 close_frame state is invalid"));
    }

    const HRESULT closeResult = m_functions.closeCommandList(m_commandList.Get());

    if (FAILED(closeResult))
    {
        return handle_close_failure(make_native_error(*m_assertContext, k_commandListCloseFailed,
                                                      "D3D12 Graphics Command List close failed", closeResult));
    }

    m_commandListState = D3d12CommandListState::Closed;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::handle_close_failure(Error &&a_error) noexcept
{
    Result<void> result = m_queueState->reclassify_device_failure(std::move(a_error));
    update_status_from_queue();

    if (m_status == D3d12FrameCommandStatus::Ready)
    {
        m_commandListState = D3d12CommandListState::RecordingCloseFailed;
        m_acceptingFrames = false;
    }

    return result;
}

Result<void> D3d12FrameCommandState::discard_closed_frame_after_exhaustion() noexcept
{
    D3d12FrameContext &frame = m_frames[m_activeFrameIndex];
    const HRESULT resetResult = m_functions.resetCommandList(m_commandList.Get(), frame.allocator.Get());

    if (FAILED(resetResult))
    {
        return handle_reset_failure(make_native_error(*m_assertContext, k_commandListResetFailed,
                                                      "D3D12 Graphics Command List discard reset failed", resetResult));
    }

    const HRESULT closeResult = m_functions.closeCommandList(m_commandList.Get());

    if (FAILED(closeResult))
    {
        return handle_close_failure(make_native_error(*m_assertContext, k_commandListCloseFailed,
                                                      "D3D12 Graphics Command List discard close failed", closeResult));
    }

    m_activeFrameIndex = k_invalidFrameIndexValue;
    m_commandListState = D3d12CommandListState::IdleClosed;
    return Result<void>::success();
}

// Signal 用 Fence 値を先に確保してから Command List を Submit し、実行済み Work の追跡不能を可能な限り避ける
Result<void> D3d12FrameCommandState::execute_frame() noexcept
{
    update_status_from_queue();

    if (m_status != D3d12FrameCommandStatus::Ready)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Frame acceptance is stopped"));
    }

    if (m_commandListState != D3d12CommandListState::Closed)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 execute_frame requires a closed Command List");
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidCommandListState, "D3D12 execute_frame state is invalid"));
    }

    Result<std::uint64_t> reservationResult = m_queueState->reserve_fence_value();

    if (!reservationResult)
    {
        Error reservationError = std::move(*reservationResult.try_error());

        if (is_fence_exhaustion(reservationError))
        {
            Result<void> discardResult = discard_closed_frame_after_exhaustion();

            if (!discardResult)
            {
                Error discardError = std::move(*discardResult.try_error());
                update_status_from_queue();

                if (m_status == D3d12FrameCommandStatus::DeviceRemoved)
                {
                    add_secondary_error_context(discardError, reservationError,
                                                "D3D12 Fence range was exhausted before Device Removal",
                                                *m_assertContext);
                    return Result<void>::failure(std::move(discardError));
                }

                add_secondary_error_context(reservationError, discardError,
                                            "D3D12 Command List discard also failed after Fence exhaustion",
                                            *m_assertContext);
                return Result<void>::failure(std::move(reservationError));
            }

            m_acceptingFrames = false;
        }

        return Result<void>::failure(std::move(reservationError));
    }

    m_pendingFenceValue = *reservationResult.try_value();
    m_queueState->execute_command_list(m_commandList.Get());
    m_commandListState = D3d12CommandListState::ExecutedAwaitingPresent;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::mark_present_attempted() noexcept
{
    update_status_from_queue();

    if (m_status != D3d12FrameCommandStatus::Ready)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Frame acceptance is stopped"));
    }

    if (m_commandListState != D3d12CommandListState::ExecutedAwaitingPresent)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Present marker requires an executed Command List");
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidCommandListState, "D3D12 Present marker state is invalid"));
    }

    const D3d12FrameContext &frame = m_frames[m_activeFrameIndex];

    if (frame.backBuffer != nullptr && frame.backBufferState != D3d12BackBufferState::Present)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Present requires the Current Back Buffer in Present state");
        m_commandListState = D3d12CommandListState::ExecutedUnfenced;
        return Result<void>::failure(make_error(*m_assertContext, k_invalidBackBufferTransition,
                                                "D3D12 Present Back Buffer state is invalid"));
    }

    m_commandListState = D3d12CommandListState::ExecutedUnfenced;
    return Result<void>::success();
}

// Present 成功後または非 Device Removal の失敗後に Submit 済み Work への Fence 付与を試行し、
// 成功時に後続の Frame Resource 再利用や Cleanup が完了を待てるようにする
Result<std::uint64_t> D3d12FrameCommandState::signal_frame(D3d12FrameSignalPurpose a_purpose) noexcept
{
    update_status_from_queue();

    if (m_status != D3d12FrameCommandStatus::Ready)
    {
        return Result<std::uint64_t>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Frame acceptance is stopped"));
    }

    if (m_commandListState != D3d12CommandListState::ExecutedUnfenced || m_pendingFenceValue == 0)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 signal_frame requires an executed unfenced Command List");
        return Result<std::uint64_t>::failure(
            make_error(*m_assertContext, k_invalidCommandListState, "D3D12 signal_frame state is invalid"));
    }

    std::uint64_t fenceValue = m_pendingFenceValue;
    Result<void> signalResult = m_queueState->signal_reserved(fenceValue);

    if (!signalResult)
    {
        Result<void> resolutionResult = m_queueState->resolve_failed_signal(
            fenceValue, std::move(*signalResult.try_error()), D3d12FenceWaitPurpose::Reusable);
        update_status_from_queue();

        if (m_status == D3d12FrameCommandStatus::Ready && m_queueState->last_signaled_fence() >= fenceValue)
        {
            m_frames[m_activeFrameIndex].reuseFenceValue = fenceValue;
            m_lastSubmittedFence = fenceValue;
            m_pendingFenceValue = 0;
            m_activeFrameIndex = k_invalidFrameIndexValue;
            m_commandListState = D3d12CommandListState::Submitted;
            m_acceptingFrames = false;
        }

        return Result<std::uint64_t>::failure(std::move(*resolutionResult.try_error()));
    }

    m_frames[m_activeFrameIndex].reuseFenceValue = fenceValue;
    m_lastSubmittedFence = fenceValue;
    m_pendingFenceValue = 0;
    m_activeFrameIndex = k_invalidFrameIndexValue;
    m_commandListState = D3d12CommandListState::Submitted;
    if (a_purpose == D3d12FrameSignalPurpose::PresentFailureRecovery)
    {
        m_acceptingFrames = false;
    }

    return Result<std::uint64_t>::success(std::move(fenceValue));
}

Result<void> D3d12FrameCommandState::stop_after_presentation_error() noexcept
{
    update_status_from_queue();

    if (m_status != D3d12FrameCommandStatus::Ready || m_commandListState != D3d12CommandListState::Submitted)
    {
        return Result<void>::failure(make_error(*m_assertContext, k_invalidCommandListState,
                                                "D3D12 Presentation Error requires a submitted Frame"));
    }

    m_acceptingFrames = false;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::stop_after_device_removal() noexcept
{
    update_status_from_queue();

    if (m_status != D3d12FrameCommandStatus::DeviceRemoved)
    {
        return Result<void>::failure(make_error(*m_assertContext, k_invalidCommandListState,
                                                "D3D12 Frame Command did not observe Device Removal"));
    }

    m_acceptingFrames = false;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::suspend_for_resize() noexcept
{
    update_status_from_queue();

    if (m_status != D3d12FrameCommandStatus::Ready)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Frame Command State is unavailable"));
    }

    if (!m_acceptingFrames)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped,
                       "D3D12 Frame acceptance was stopped before Resize suspension"));
    }

    if (m_commandListState != D3d12CommandListState::IdleClosed &&
        m_commandListState != D3d12CommandListState::Submitted)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Resize suspension requires an idle or submitted Command List");
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidCommandListState, "D3D12 Resize suspension state is invalid"));
    }

    m_acceptingFrames = false;
    m_isResizeSuspended = true;
    return Result<void>::success();
}

// Back Buffer を Clear 前に PRESENT から RENDER_TARGET へ、Present 前に PRESENT へ戻す順序を状態で検証する
Result<void> D3d12FrameCommandState::transition_back_buffer(std::uint32_t a_frameIndex,
                                                            D3d12BackBufferState a_targetState) noexcept
{
    update_status_from_queue();

    if (!m_acceptingFrames || m_status != D3d12FrameCommandStatus::Ready)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Back Buffer transition is unavailable"));
    }

    if (a_frameIndex >= k_d3d12FrameContextCount)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Back Buffer transition index is out of range");
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidFrameIndex, "D3D12 Back Buffer transition index is out of range"));
    }

    if (m_commandListState != D3d12CommandListState::Recording || a_frameIndex != m_activeFrameIndex)
    {
        CUE_ASSERT(*m_assertContext, false,
                   "D3D12 Back Buffer transition requires the Current Back Buffer while recording");
        return Result<void>::failure(make_error(*m_assertContext, k_invalidBackBufferTransition,
                                                "D3D12 Back Buffer transition Frame state is invalid"));
    }

    D3d12FrameContext &frame = m_frames[a_frameIndex];

    if (frame.backBuffer == nullptr || frame.backBufferState == D3d12BackBufferState::Unknown)
    {
        return Result<void>::failure(make_error(*m_assertContext, k_invalidBackBufferResource,
                                                "D3D12 Back Buffer transition Resource is unavailable"));
    }

    if (a_targetState != D3d12BackBufferState::Present &&
        a_targetState != D3d12BackBufferState::RenderTarget)
    {
        return Result<void>::failure(make_error(*m_assertContext, k_invalidBackBufferTransition,
                                                "D3D12 Back Buffer transition target State is invalid"));
    }

    if (frame.backBufferState == a_targetState)
    {
        return Result<void>::success();
    }

    const D3D12_RESOURCE_STATES beforeState = frame.backBufferState == D3d12BackBufferState::Present
                                                  ? D3D12_RESOURCE_STATE_PRESENT
                                                  : D3D12_RESOURCE_STATE_RENDER_TARGET;
    const D3D12_RESOURCE_STATES afterState = a_targetState == D3d12BackBufferState::Present
                                                 ? D3D12_RESOURCE_STATE_PRESENT
                                                 : D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = frame.backBuffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    m_functions.resourceBarrier(m_commandList.Get(), 1, &barrier);
    frame.backBufferState = a_targetState;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::clear_back_buffer(std::uint32_t a_frameIndex, D3d12RtvHeap &a_heap,
                                                       const std::array<float, 4> &a_color) noexcept
{
    update_status_from_queue();

    if (!m_acceptingFrames || m_status != D3d12FrameCommandStatus::Ready)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Back Buffer clear is unavailable"));
    }

    if (a_frameIndex >= k_d3d12FrameContextCount)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Back Buffer clear index is out of range");
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidFrameIndex, "D3D12 Back Buffer clear index is out of range"));
    }

    if (m_commandListState != D3d12CommandListState::Recording || a_frameIndex != m_activeFrameIndex)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Back Buffer clear requires the Current Back Buffer while recording");
        return Result<void>::failure(make_error(*m_assertContext, k_invalidBackBufferClear,
                                                "D3D12 Back Buffer clear Frame state is invalid"));
    }

    const D3d12FrameContext &frame = m_frames[a_frameIndex];

    if (frame.backBuffer == nullptr || frame.backBufferState != D3d12BackBufferState::RenderTarget ||
        !frame.rtvSlot.has_value())
    {
        return Result<void>::failure(make_error(*m_assertContext, k_invalidBackBufferClear,
                                                "D3D12 Back Buffer clear Resource state or RTV is invalid"));
    }

    Result<D3D12_CPU_DESCRIPTOR_HANDLE> handleResult = a_heap.cpu_handle(*frame.rtvSlot);

    if (!handleResult)
    {
        return Result<void>::failure(std::move(*handleResult.try_error()));
    }

    m_functions.setMarker(m_commandList.Get(), "ClearBackBuffer");
    m_functions.clearRenderTargetView(m_commandList.Get(), *handleResult.try_value(), a_color.data(), 0, nullptr);
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::record_scene(std::uint32_t a_frameIndex, D3d12RtvHeap &a_heap,
                                                  D3d12ScenePass &a_pass, std::uint32_t a_width,
                                                  std::uint32_t a_height,
                                                  const PresentationSceneFrameDescriptor &a_descriptor) noexcept
{
    update_status_from_queue();
    if (!m_acceptingFrames || m_status != D3d12FrameCommandStatus::Ready ||
        a_frameIndex >= k_d3d12FrameContextCount || m_commandListState != D3d12CommandListState::Recording ||
        a_frameIndex != m_activeFrameIndex)
    {
        return Result<void>::failure(make_error(*m_assertContext, k_invalidSceneRecord,
                                                "D3D12 Scene recording requires the active Frame"));
    }
    const D3d12FrameContext &frame = m_frames[a_frameIndex];
    if (frame.backBuffer == nullptr || frame.backBufferState != D3d12BackBufferState::RenderTarget ||
        !frame.rtvSlot.has_value())
    {
        return Result<void>::failure(make_error(*m_assertContext, k_invalidSceneRecord,
                                                "D3D12 Scene recording requires a Render Target and RTV"));
    }
    Result<D3D12_CPU_DESCRIPTOR_HANDLE> handle = a_heap.cpu_handle(*frame.rtvSlot);
    if (!handle)
    {
        return Result<void>::failure(std::move(*handle.try_error()));
    }
    m_functions.setMarker(m_commandList.Get(), "SceneCubeDraw");
    a_pass.record(m_commandList.Get(), *handle.try_value(), a_frameIndex, a_width, a_height, a_descriptor);
    return Result<void>::success();
}

#if CUE_D3D12_TESTING
bool D3d12FrameCommandState::copy_scene_back_buffer_for_probe(
    std::uint32_t a_frameIndex, ID3D12Resource *a_readback,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &a_footprint) noexcept
{
    if (a_frameIndex >= k_d3d12FrameContextCount || a_frameIndex != m_activeFrameIndex ||
        m_commandListState != D3d12CommandListState::Recording || a_readback == nullptr ||
        m_frames[a_frameIndex].backBuffer == nullptr ||
        m_frames[a_frameIndex].backBufferState != D3d12BackBufferState::RenderTarget)
    {
        return false;
    }
    ID3D12Resource *backBuffer = m_frames[a_frameIndex].backBuffer.Get();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_commandList->ResourceBarrier(1U, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = backBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = a_readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = a_footprint;
    m_commandList->CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1U, &barrier);
    return true;
}
#endif

// 新規 Frame 受付を止めて最後の Submit 完了を待ち、Back Buffer 参照を外せる GPU Idle 状態を証明する
Result<void> D3d12FrameCommandState::prepare_for_resize(std::uint32_t a_frameIndex) noexcept
{
    update_status_from_queue();
    m_resizeGpuIdleProven = false;

    if (m_status != D3d12FrameCommandStatus::Ready)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Frame Command State is unavailable"));
    }

    if (!m_acceptingFrames && !m_isResizeSuspended)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped,
                       "D3D12 Frame acceptance was stopped before Resize preparation"));
    }

    if (a_frameIndex >= k_d3d12FrameContextCount)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Resize Frame Context index is out of range");
        m_acceptingFrames = false;
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidFrameIndex, "D3D12 Resize Frame Context index is out of range"));
    }

    if (m_commandListState != D3d12CommandListState::IdleClosed &&
        m_commandListState != D3d12CommandListState::Submitted)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Resize requires an idle or submitted Command List");
        m_acceptingFrames = false;
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidCommandListState, "D3D12 Resize Command List state is invalid"));
    }

    m_acceptingFrames = false;
    m_isResizeSuspended = true;
    Result<void> waitResult =
        m_queueState->wait_for_fence(m_lastSubmittedFence, D3d12FenceWaitPurpose::Reusable);

    if (!waitResult)
    {
        m_resizeGpuIdleProven = m_queueState->was_last_wait_completion_proven();
        update_status_from_queue();
        return Result<void>::failure(std::move(*waitResult.try_error()));
    }

    m_resizeGpuIdleProven = true;

    D3d12FrameContext &frame = m_frames[a_frameIndex];
    const HRESULT allocatorResetResult = m_functions.resetCommandAllocator(frame.allocator.Get());

    if (FAILED(allocatorResetResult))
    {
        return handle_reset_failure(make_native_error(*m_assertContext, k_commandAllocatorResetFailed,
                                                      "D3D12 Resize Command Allocator reset failed",
                                                      allocatorResetResult));
    }

    const HRESULT listResetResult = m_functions.resetCommandList(m_commandList.Get(), frame.allocator.Get());

    if (FAILED(listResetResult))
    {
        return handle_reset_failure(make_native_error(*m_assertContext, k_commandListResetFailed,
                                                      "D3D12 Resize Graphics Command List reset failed",
                                                      listResetResult));
    }

    const HRESULT closeResult = m_functions.closeCommandList(m_commandList.Get());

    if (FAILED(closeResult))
    {
        return handle_close_failure(make_native_error(*m_assertContext, k_commandListCloseFailed,
                                                      "D3D12 Resize Graphics Command List close failed", closeResult));
    }

    for (D3d12FrameContext &completedFrame : m_frames)
    {
        completedFrame.reuseFenceValue = 0;
    }

    m_activeFrameIndex = k_invalidFrameIndexValue;
    m_pendingFenceValue = 0;
    m_commandListState = D3d12CommandListState::IdleClosed;
    return Result<void>::success();
}

// 新 Back Buffer と RTV の再 Binding 完了後だけ Frame 受付を再開し、古い Resource への記録を防ぐ
Result<void> D3d12FrameCommandState::resume_after_resize() noexcept
{
    update_status_from_queue();

    if (m_status != D3d12FrameCommandStatus::Ready || !m_isResizeSuspended ||
        (m_commandListState != D3d12CommandListState::IdleClosed &&
         m_commandListState != D3d12CommandListState::Submitted))
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Frame acceptance cannot resume"));
    }

    m_acceptingFrames = true;
    m_isResizeSuspended = false;
    m_resizeGpuIdleProven = false;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::begin_release_after_gpu_idle() noexcept
{
    update_status_from_queue();
    CUE_ASSERT(*m_assertContext, m_status == D3d12FrameCommandStatus::Ready && !m_acceptingFrames,
               "D3D12 Frame Command GPU-idle cleanup requires stopped frame acceptance");

    if (m_status != D3d12FrameCommandStatus::Ready || m_acceptingFrames)
    {
        return Result<void>::failure(make_error(*m_assertContext, k_invalidCommandListState,
                                                "D3D12 Frame Command GPU-idle cleanup state is invalid"));
    }

    release_command_list();
    m_status = D3d12FrameCommandStatus::CleanupPending;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::release_after_gpu_idle() noexcept
{
    Result<void> beginResult = begin_release_after_gpu_idle();

    if (!beginResult)
    {
        return beginResult;
    }

    return release_allocators_after_presentation_cleanup();
}

Result<void> D3d12FrameCommandState::begin_shutdown() noexcept
{
    if (m_status == D3d12FrameCommandStatus::Shutdown)
    {
        return Result<void>::success();
    }

    if (m_status == D3d12FrameCommandStatus::CleanupPending)
    {
        return Result<void>::success();
    }

    update_status_from_queue();
    m_acceptingFrames = false;

    if (m_status == D3d12FrameCommandStatus::Unavailable)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_frameAcceptanceStopped, "D3D12 Frame Command State is unavailable"));
    }

    if (m_status == D3d12FrameCommandStatus::DeviceRemoved)
    {
        return Result<void>::failure(make_error(*m_assertContext, k_frameAcceptanceStopped,
                                                "D3D12 Frame Command State requires Device Removed cleanup"));
    }

    const bool shutdownStateAllowed = m_commandListState == D3d12CommandListState::IdleClosed ||
                                      m_commandListState == D3d12CommandListState::Submitted ||
                                      m_commandListState == D3d12CommandListState::FrameResetFailed ||
                                      m_commandListState == D3d12CommandListState::RecordingCloseFailed;

    if (!shutdownStateAllowed)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Frame Command shutdown requires a terminal-safe state");
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidCommandListState, "D3D12 Frame Command shutdown state is invalid"));
    }

    std::optional<Error> firstError;
    std::uint64_t terminalFence = 0;
    Result<std::uint64_t> reservationResult = m_queueState->reserve_fence_value();

    if (!reservationResult)
    {
        Error reservationError = std::move(*reservationResult.try_error());

        if (!is_fence_exhaustion(reservationError))
        {
            return Result<void>::failure(std::move(reservationError));
        }

        firstError.emplace(std::move(reservationError));
        terminalFence = m_queueState->last_signaled_fence();
    }
    else
    {
        terminalFence = *reservationResult.try_value();
        Result<void> signalResult = m_queueState->signal_reserved(terminalFence);

        if (!signalResult)
        {
            Result<void> resolutionResult = m_queueState->resolve_failed_signal(
                terminalFence, std::move(*signalResult.try_error()), D3d12FenceWaitPurpose::Reusable);

            if (!resolutionResult)
            {
                firstError.emplace(std::move(*resolutionResult.try_error()));
            }

            update_status_from_queue();

            if (m_status == D3d12FrameCommandStatus::Unavailable)
            {
                return Result<void>::failure(std::move(*firstError));
            }

            if (m_status == D3d12FrameCommandStatus::DeviceRemoved)
            {
                return Result<void>::failure(std::move(*firstError));
            }

            if (m_queueState->last_signaled_fence() >= terminalFence)
            {
                m_lastSubmittedFence = terminalFence;
                release_command_list();
                m_status = D3d12FrameCommandStatus::CleanupPending;
            }

            return Result<void>::failure(std::move(*firstError));
        }

        if (m_queueState->last_signaled_fence() >= terminalFence)
        {
            m_lastSubmittedFence = terminalFence;
        }
    }

    Result<void> waitResult = m_queueState->wait_for_fence(terminalFence, D3d12FenceWaitPurpose::Reusable);

    if (!waitResult)
    {
        Error waitError = std::move(*waitResult.try_error());
        update_status_from_queue();

        if (m_status == D3d12FrameCommandStatus::DeviceRemoved)
        {
            if (firstError)
            {
                add_secondary_error_context(waitError, *firstError,
                                            "D3D12 Frame terminal operation failed before Device Removal",
                                            *m_assertContext);
            }

            return Result<void>::failure(std::move(waitError));
        }

        if (firstError)
        {
            add_secondary_error_context(*firstError, waitError, "D3D12 Frame terminal Fence Wait also failed",
                                        *m_assertContext);
        }
        else
        {
            firstError.emplace(std::move(waitError));
        }

        if (m_status == D3d12FrameCommandStatus::Unavailable)
        {
            return Result<void>::failure(std::move(*firstError));
        }
    }

    release_command_list();
    m_status = D3d12FrameCommandStatus::CleanupPending;

    if (firstError)
    {
        return Result<void>::failure(std::move(*firstError));
    }

    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::shutdown() noexcept
{
    Result<void> beginResult = begin_shutdown();

    if (m_status != D3d12FrameCommandStatus::CleanupPending)
    {
        return beginResult;
    }

    Result<void> allocatorResult = release_allocators_after_presentation_cleanup();

    if (!beginResult)
    {
        return beginResult;
    }

    return allocatorResult;
}

// Device Removal 時は通常の GPU 完了を証明できないため、呼出側が利用可能な診断を試行した後に
// 解放可能な専用状態へ移行する
Result<void> D3d12FrameCommandState::begin_release_after_device_removed() noexcept
{
    update_status_from_queue();
    CUE_ASSERT(*m_assertContext, m_status == D3d12FrameCommandStatus::DeviceRemoved,
               "D3D12 Frame Command Device Removed cleanup requires DeviceRemoved state");

    if (m_status != D3d12FrameCommandStatus::DeviceRemoved)
    {
        return Result<void>::failure(make_error(*m_assertContext, k_invalidCommandListState,
                                                "D3D12 Frame Command Device Removed cleanup state is invalid"));
    }

    release_command_list();
    m_status = D3d12FrameCommandStatus::CleanupPending;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::release_after_device_removed() noexcept
{
    Result<void> beginResult = begin_release_after_device_removed();

    if (!beginResult)
    {
        return beginResult;
    }

    return release_allocators_after_presentation_cleanup();
}

Result<void> D3d12FrameCommandState::release_allocators_after_presentation_cleanup() noexcept
{
    CUE_ASSERT(*m_assertContext, m_status == D3d12FrameCommandStatus::CleanupPending,
               "D3D12 Frame Allocator cleanup requires released Presentation resources");

    if (m_status != D3d12FrameCommandStatus::CleanupPending)
    {
        return Result<void>::failure(make_error(*m_assertContext, k_invalidCommandListState,
                                                "D3D12 Frame Allocator cleanup state is invalid"));
    }

    release_allocators();
    m_status = D3d12FrameCommandStatus::Shutdown;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::bind_back_buffers(D3d12FrameBackBuffers &&a_backBuffers) noexcept
{
    for (std::uint32_t index = 0; index < k_d3d12FrameContextCount; ++index)
    {
        CUE_ASSERT(*m_assertContext, m_frames[index].backBuffer == nullptr && !m_frames[index].rtvSlot,
                   "D3D12 Frame Back Buffer binding requires empty Frame resources");

        if (m_frames[index].backBuffer != nullptr || m_frames[index].rtvSlot || a_backBuffers[index] == nullptr)
        {
            return Result<void>::failure(make_error(*m_assertContext, k_invalidFrameResource,
                                                    "D3D12 Frame Back Buffer binding is invalid"));
        }
    }

    for (std::uint32_t index = 0; index < k_d3d12FrameContextCount; ++index)
    {
        m_frames[index].backBuffer = std::move(a_backBuffers[index]);
        m_frames[index].backBufferState = D3d12BackBufferState::Present;
    }

    return Result<void>::success();
}

Result<ID3D12Resource *> D3d12FrameCommandState::back_buffer(std::uint32_t a_frameIndex) const noexcept
{
    if (a_frameIndex >= k_d3d12FrameContextCount)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Frame Back Buffer index is out of range");
        return Result<ID3D12Resource *>::failure(
            make_error(*m_assertContext, k_invalidFrameIndex, "D3D12 Frame Back Buffer index is out of range"));
    }

    if (m_frames[a_frameIndex].backBuffer == nullptr)
    {
        return Result<ID3D12Resource *>::failure(
            make_error(*m_assertContext, k_invalidFrameResource, "D3D12 Frame Back Buffer is unavailable"));
    }

    ID3D12Resource *backBuffer = m_frames[a_frameIndex].backBuffer.Get();
    return Result<ID3D12Resource *>::success(std::move(backBuffer));
}

Result<void> D3d12FrameCommandState::bind_rtv_slot(std::uint32_t a_frameIndex, D3d12RtvSlot a_slot) noexcept
{
    if (a_frameIndex >= k_d3d12FrameContextCount)
    {
        CUE_ASSERT(*m_assertContext, false, "D3D12 Frame RTV Slot index is out of range");
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidFrameIndex, "D3D12 Frame RTV Slot index is out of range"));
    }

    D3d12FrameContext &frame = m_frames[a_frameIndex];
    CUE_ASSERT(*m_assertContext, frame.backBuffer != nullptr && !frame.rtvSlot,
               "D3D12 Frame RTV Slot binding requires a Back Buffer without an RTV");

    if (frame.backBuffer == nullptr || frame.rtvSlot)
    {
        return Result<void>::failure(
            make_error(*m_assertContext, k_invalidFrameResource, "D3D12 Frame RTV Slot binding is invalid"));
    }

    frame.rtvSlot = a_slot;
    return Result<void>::success();
}

Result<void> D3d12FrameCommandState::release_rtv_slots(D3d12RtvHeap &a_heap) noexcept
{
    std::optional<Error> firstError;

    for (D3d12FrameContext &frame : m_frames)
    {
        if (!frame.rtvSlot)
        {
            continue;
        }

        Result<void> releaseResult = a_heap.release(*frame.rtvSlot);

        if (!releaseResult && !firstError)
        {
            firstError.emplace(std::move(*releaseResult.try_error()));
        }

        frame.rtvSlot.reset();
    }

    if (firstError)
    {
        return Result<void>::failure(std::move(*firstError));
    }

    return Result<void>::success();
}

void D3d12FrameCommandState::release_back_buffers() noexcept
{
    for (D3d12FrameContext &frame : m_frames)
    {
        CUE_ASSERT(*m_assertContext, !frame.rtvSlot, "D3D12 Back Buffer release requires released RTV metadata");
        frame.backBuffer.Reset();
        frame.backBufferState = D3d12BackBufferState::Unknown;
    }
}

void D3d12FrameCommandState::release_command_list() noexcept
{
    m_commandList.Reset();

    m_activeFrameIndex = k_invalidFrameIndexValue;
    m_pendingFenceValue = 0;
}

void D3d12FrameCommandState::release_allocators() noexcept
{
    for (D3d12FrameContext &frame : m_frames)
    {
        frame.allocator.Reset();
        frame.reuseFenceValue = 0;
    }
}

D3d12CommandListState D3d12FrameCommandState::command_list_state() const noexcept
{
    return m_commandListState;
}

D3d12FrameCommandStatus D3d12FrameCommandState::status() const noexcept
{
    return m_status;
}

std::uint64_t D3d12FrameCommandState::frame_reuse_fence(std::uint32_t a_frameIndex) const noexcept
{
    CUE_ASSERT(*m_assertContext, a_frameIndex < k_d3d12FrameContextCount,
               "D3D12 Frame reuse Fence query index is out of range");
    return m_frames[a_frameIndex].reuseFenceValue;
}

std::uint64_t D3d12FrameCommandState::last_submitted_fence() const noexcept
{
    return m_lastSubmittedFence;
}

bool D3d12FrameCommandState::is_accepting_frames() const noexcept
{
    return m_acceptingFrames;
}

bool D3d12FrameCommandState::was_resize_gpu_idle_proven() const noexcept
{
    return m_resizeGpuIdleProven;
}

bool D3d12FrameCommandState::has_command_list() const noexcept
{
    return m_commandList != nullptr;
}

std::uint32_t D3d12FrameCommandState::allocator_count() const noexcept
{
    std::uint32_t count = 0;

    for (const D3d12FrameContext &frame : m_frames)
    {
        if (frame.allocator != nullptr)
        {
            ++count;
        }
    }

    return count;
}

std::uint32_t D3d12FrameCommandState::back_buffer_count() const noexcept
{
    std::uint32_t count = 0;

    for (const D3d12FrameContext &frame : m_frames)
    {
        if (frame.backBuffer != nullptr)
        {
            ++count;
        }
    }

    return count;
}

std::uint32_t D3d12FrameCommandState::rtv_count() const noexcept
{
    std::uint32_t count = 0;

    for (const D3d12FrameContext &frame : m_frames)
    {
        if (frame.rtvSlot)
        {
            ++count;
        }
    }

    return count;
}

bool D3d12FrameCommandState::has_all_back_buffers() const noexcept
{
    for (const D3d12FrameContext &frame : m_frames)
    {
        if (frame.backBuffer == nullptr)
        {
            return false;
        }
    }

    return true;
}

bool D3d12FrameCommandState::are_back_buffers_present() const noexcept
{
    for (const D3d12FrameContext &frame : m_frames)
    {
        if (frame.backBuffer == nullptr || frame.backBufferState != D3d12BackBufferState::Present)
        {
            return false;
        }
    }

    return true;
}

bool D3d12FrameCommandState::has_native_objects() const noexcept
{
    if (m_commandList != nullptr)
    {
        return true;
    }

    for (const D3d12FrameContext &frame : m_frames)
    {
        if (frame.allocator != nullptr || frame.backBuffer != nullptr || frame.rtvSlot)
        {
            return true;
        }
    }

    return false;
}

Result<D3d12FrameCommandState> create_d3d12_frame_command_state(
    ID3D12Device *a_device, D3d12QueueState &a_queueState, const AssertContext &a_assertContext,
    const D3d12FrameCommandNativeFunctions &a_functions) noexcept
{
    std::array<D3d12FrameContext, k_d3d12FrameContextCount> frames = {};
    constexpr LPCWSTR allocatorNames[k_d3d12FrameContextCount] = {
        L"CueEngine D3D12 Frame 0 Direct Command Allocator",
        L"CueEngine D3D12 Frame 1 Direct Command Allocator",
    };

    for (std::uint32_t frameIndex = 0; frameIndex < k_d3d12FrameContextCount; ++frameIndex)
    {
        const HRESULT creationResult =
            a_functions.createCommandAllocator(a_device, frames[frameIndex].allocator.GetAddressOf());

        if (FAILED(creationResult))
        {
            return Result<D3d12FrameCommandState>::failure(
                make_native_error(a_assertContext, k_commandAllocatorCreationFailed,
                                  "D3D12 Frame Command Allocator creation failed", creationResult));
        }

        const HRESULT nameResult =
            a_functions.setObjectName(frames[frameIndex].allocator.Get(), allocatorNames[frameIndex]);

        if (FAILED(nameResult))
        {
            return Result<D3d12FrameCommandState>::failure(
                make_native_error(a_assertContext, k_commandAllocatorNameFailed,
                                  "D3D12 Frame Command Allocator diagnostic name could not be set", nameResult));
        }

        frames[frameIndex].reuseFenceValue = 0;
        frames[frameIndex].backBufferState = D3d12BackBufferState::Unknown;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    const HRESULT listCreationResult =
        a_functions.createCommandList(a_device, frames[0].allocator.Get(), commandList.GetAddressOf());

    if (FAILED(listCreationResult))
    {
        return Result<D3d12FrameCommandState>::failure(make_native_error(a_assertContext, k_commandListCreationFailed,
                                                                         "D3D12 Graphics Command List creation failed",
                                                                         listCreationResult));
    }

    const HRESULT listNameResult =
        a_functions.setObjectName(commandList.Get(), L"CueEngine D3D12 Frames 0-1 Graphics Command List");

    if (FAILED(listNameResult))
    {
        return Result<D3d12FrameCommandState>::failure(
            make_native_error(a_assertContext, k_commandListNameFailed,
                              "D3D12 Graphics Command List diagnostic name could not be set", listNameResult));
    }

    const HRESULT closeResult = a_functions.closeCommandList(commandList.Get());

    if (FAILED(closeResult))
    {
        return Result<D3d12FrameCommandState>::failure(
            make_native_error(a_assertContext, k_commandListInitialCloseFailed,
                              "D3D12 Graphics Command List initial close failed", closeResult));
    }

    D3d12FrameCommandState state(std::move(frames), std::move(commandList), a_queueState, a_assertContext, a_functions);
    return Result<D3d12FrameCommandState>::success(std::move(state));
}
} // namespace cue
