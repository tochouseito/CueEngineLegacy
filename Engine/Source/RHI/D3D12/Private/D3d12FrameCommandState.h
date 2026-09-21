// Frame ごとの Command Allocator、Command List、Back Buffer、RTV と再利用 Fence を管理する内部状態
// CPU が次 Frame を準備する間も GPU が前 Frame を処理できるよう、2個の Frame Context を交互に使用する

#pragma once

#include "D3d12RtvHeap.h"

#include <Cue/Foundation/Result.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <optional>

namespace cue
{
class AssertContext;
class D3d12QueueState;
class D3d12ScenePass;
struct PresentationSceneFrameDescriptor;

constexpr std::uint32_t k_d3d12FrameContextCount = 2;

// Command List の Native Lifecycle を明示し、Reset や再実行を不正な順序で行わないための状態
enum class D3d12CommandListState
{
    Initial,
    IdleClosed,
    Submitted,
    FrameResetFailed,
    Recording,
    RecordingCloseFailed,
    Closed,
    ExecutedAwaitingPresent,
    // GPU へ渡したが完了 Fence を発行できず、安全な Resource 再利用を証明できない状態
    ExecutedUnfenced,
};

enum class D3d12FrameCommandStatus
{
    Ready,
    DeviceRemoved,
    Unavailable,
    CleanupPending,
    Shutdown,
};

enum class D3d12FrameSignalPurpose
{
    Regular,
    PresentFailureRecovery,
};

enum class D3d12BackBufferState
{
    Unknown,
    Present,
    RenderTarget,
};

using D3d12FrameBackBuffers =
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, k_d3d12FrameContextCount>;

struct D3d12FrameContext final
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer;
    std::optional<D3d12RtvSlot> rtvSlot;
    D3d12BackBufferState backBufferState;
    std::uint64_t reuseFenceValue;
};

struct D3d12FrameCommandNativeFunctions final
{
    HRESULT (*createCommandAllocator)(ID3D12Device *, ID3D12CommandAllocator **) noexcept;
    HRESULT (*createCommandList)(ID3D12Device *, ID3D12CommandAllocator *, ID3D12GraphicsCommandList **) noexcept;
    HRESULT (*setObjectName)(ID3D12Object *, LPCWSTR) noexcept;
    HRESULT (*resetCommandAllocator)(ID3D12CommandAllocator *) noexcept;
    HRESULT (*resetCommandList)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *) noexcept;
    HRESULT (*closeCommandList)(ID3D12GraphicsCommandList *) noexcept;
    void (*resourceBarrier)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *) noexcept;
    void (*setMarker)(ID3D12GraphicsCommandList *, PCSTR) noexcept;
    void (*clearRenderTargetView)(ID3D12GraphicsCommandList *, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT,
                                  const D3D12_RECT *) noexcept;
};

/// @brief Production 経路で使用する D3D12 Native API 関数 Table を構築して返す
[[nodiscard]] const D3d12FrameCommandNativeFunctions &default_d3d12_frame_command_native_functions() noexcept;

class D3d12FrameCommandState final
{
  public:
    /// @brief D3d12FrameCommandState の一意所有を保つため Copy 構築を禁止する
    D3d12FrameCommandState(const D3d12FrameCommandState &) = delete;
    /// @brief D3d12FrameCommandState の一意所有を保つため Copy 代入を禁止する
    D3d12FrameCommandState &operator=(const D3d12FrameCommandState &) = delete;
    /// @brief D3d12FrameCommandState の所有状態を Move 構築し、移動元を安全な空状態へ戻す
    D3d12FrameCommandState(D3d12FrameCommandState &&a_other) noexcept;
    /// @brief D3d12FrameCommandState の所有状態を Move 代入し、代入元を安全な空状態へ移す
    D3d12FrameCommandState &operator=(D3d12FrameCommandState &&a_other) noexcept;
    /// @brief 明示 Shutdown 前に Native Resource が残っていれば契約違反として Process を強制終了する
    ~D3d12FrameCommandState() noexcept;

    /// @brief 再利用 Fence の完了後に Allocator と Command List を Reset し、Frame Recording を開始する
    [[nodiscard]] Result<void> begin_frame(std::uint32_t a_frameIndex) noexcept;
    /// @brief D3D12 Frame Command State の Back Buffer を GPU 実行順と Resource State を守って投入する
    [[nodiscard]] Result<void> transition_back_buffer(std::uint32_t a_frameIndex,
                                                      D3d12BackBufferState a_targetState) noexcept;
    /// @brief Current Back Buffer を指定 Clear Color で初期化する Command を記録する
    [[nodiscard]] Result<void> clear_back_buffer(std::uint32_t a_frameIndex, D3d12RtvHeap &a_heap,
                                                 const std::array<float, 4> &a_color) noexcept;
    /// @brief RenderTarget状態のCurrent Back Bufferへ固定Cube Passを追加記録する
    [[nodiscard]] Result<void> record_scene(std::uint32_t a_frameIndex, D3d12RtvHeap &a_heap,
                                           D3d12ScenePass &a_pass, std::uint32_t a_width, std::uint32_t a_height,
                                           const PresentationSceneFrameDescriptor &a_descriptor) noexcept;
#if CUE_D3D12_TESTING
    /// @brief Scene描画直後のBack Bufferを、Present前にProbe所有のReadbackへ複製する
    [[nodiscard]] bool copy_scene_back_buffer_for_probe(std::uint32_t a_frameIndex, ID3D12Resource *a_readback,
                                                        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &a_footprint) noexcept;
#endif
    /// @brief Frame Recording を終了して Command List を実行可能な状態へ確定する
    [[nodiscard]] Result<void> close_frame() noexcept;
    /// @brief 記録済み Frame Command List を D3D12 Queue へ投入する
    [[nodiscard]] Result<void> execute_frame() noexcept;
    /// @brief Frame の Present 試行を記録し、後続 Signal との順序検証に使用する
    [[nodiscard]] Result<void> mark_present_attempted() noexcept;
    /// @brief D3D12 Frame Command State の Frame へ完了通知を発行し、追跡する Fence 値を確定する
    [[nodiscard]] Result<std::uint64_t> signal_frame(
        D3d12FrameSignalPurpose a_purpose = D3d12FrameSignalPurpose::Regular) noexcept;
    /// @brief Presentation 失敗後の追加 Frame 受付を停止して所有状態を固定する
    [[nodiscard]] Result<void> stop_after_presentation_error() noexcept;
    /// @brief Device Removal 後の新規 GPU 処理を停止し、診断と Cleanup へ状態を進める
    [[nodiscard]] Result<void> stop_after_device_removal() noexcept;
    /// @brief D3D12 Frame Command State を For Resize へ安全に移行できる所有状態へ整える
    [[nodiscard]] Result<void> suspend_for_resize() noexcept;
    /// @brief D3D12 Frame Command State を For Resize へ安全に移行できる所有状態へ整える
    [[nodiscard]] Result<void> prepare_for_resize(std::uint32_t a_frameIndex) noexcept;
    /// @brief D3D12 Frame Command State を After Resize へ安全に移行できる所有状態へ整える
    [[nodiscard]] Result<void> resume_after_resize() noexcept;
    /// @brief D3D12 Frame Command State を Release After GPU Idle へ安全に移行できる所有状態へ整える
    [[nodiscard]] Result<void> begin_release_after_gpu_idle() noexcept;
    /// @brief D3D12 Frame Command State の After GPU Idle を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] Result<void> release_after_gpu_idle() noexcept;
    /// @brief D3D12 Frame Command State を Shutdown へ安全に移行できる所有状態へ整える
    [[nodiscard]] Result<void> begin_shutdown() noexcept;
    /// @brief 保持する Native Resource を依存関係と完了条件に従って停止し、安全な解放結果を返す
    [[nodiscard]] Result<void> shutdown() noexcept;
    /// @brief D3D12 Frame Command State を Release After Device Removed へ安全に移行できる所有状態へ整える
    [[nodiscard]] Result<void> begin_release_after_device_removed() noexcept;
    /// @brief D3D12 Frame Command State の After Device Removed を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] Result<void> release_after_device_removed() noexcept;
    /// @brief D3D12 Frame Command State の Allocators After Presentation Cleanup を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] Result<void> release_allocators_after_presentation_cleanup() noexcept;
    /// @brief D3D12 Frame Command State の Back Buffers を所有権と Lifecycle 規則を守って関連付ける
    [[nodiscard]] Result<void> bind_back_buffers(D3d12FrameBackBuffers &&a_backBuffers) noexcept;
    /// @brief D3D12 Frame Command State が保持する Back Buffer を呼び出し元へ返す
    [[nodiscard]] Result<ID3D12Resource *> back_buffer(std::uint32_t a_frameIndex) const noexcept;
    /// @brief D3D12 Frame Command State の RTV Slot を所有権と Lifecycle 規則を守って関連付ける
    [[nodiscard]] Result<void> bind_rtv_slot(std::uint32_t a_frameIndex, D3d12RtvSlot a_slot) noexcept;
    /// @brief D3D12 Frame Command State の RTV Slots を依存関係と完了条件を守って安全に解放または停止する
    [[nodiscard]] Result<void> release_rtv_slots(D3d12RtvHeap &a_heap) noexcept;
    /// @brief D3D12 Frame Command State の Back Buffers を依存関係と完了条件を守って安全に解放または停止する
    void release_back_buffers() noexcept;

    /// @brief D3D12 Frame Command State が保持する Command List State を呼び出し元へ返す
    [[nodiscard]] D3d12CommandListState command_list_state() const noexcept;
    /// @brief D3D12 Frame Command State が保持する Status を呼び出し元へ返す
    [[nodiscard]] D3d12FrameCommandStatus status() const noexcept;
    /// @brief D3D12 Frame Command State が保持する Frame Reuse Fence を呼び出し元へ返す
    [[nodiscard]] std::uint64_t frame_reuse_fence(std::uint32_t a_frameIndex) const noexcept;
    /// @brief D3D12 Frame Command State が保持する Last Submitted Fence を呼び出し元へ返す
    [[nodiscard]] std::uint64_t last_submitted_fence() const noexcept;
    /// @brief D3D12 Frame Command State の Accepting Frames 条件を判定して返す
    [[nodiscard]] bool is_accepting_frames() const noexcept;
    /// @brief Resize 前に GPU Idle が証明されたかを返す
    [[nodiscard]] bool was_resize_gpu_idle_proven() const noexcept;
    /// @brief D3D12 Frame Command State の Command List 条件を判定して返す
    [[nodiscard]] bool has_command_list() const noexcept;
    /// @brief D3D12 Frame Command State が保持する Allocator Count を呼び出し元へ返す
    [[nodiscard]] std::uint32_t allocator_count() const noexcept;
    /// @brief D3D12 Frame Command State が保持する Back Buffer Count を呼び出し元へ返す
    [[nodiscard]] std::uint32_t back_buffer_count() const noexcept;
    /// @brief D3D12 Frame Command State が保持する RTV Count を呼び出し元へ返す
    [[nodiscard]] std::uint32_t rtv_count() const noexcept;
    /// @brief Swap Chain の全 Back Buffer 取得が完了しているかを返す
    [[nodiscard]] bool has_all_back_buffers() const noexcept;
    /// @brief D3D12 Frame Command State の Back Buffers Present 条件を判定して返す
    [[nodiscard]] bool are_back_buffers_present() const noexcept;
    /// @brief 安全な解放判断に必要な Native Object が残存しているかを返す
    [[nodiscard]] bool has_native_objects() const noexcept;

  private:
    /// @brief D3D12 Frame Command State で使用する D3D12 Frame Command State を生成し、呼び出し元へ返す
    friend Result<D3d12FrameCommandState> create_d3d12_frame_command_state(
        ID3D12Device *, D3d12QueueState &, const AssertContext &, const D3d12FrameCommandNativeFunctions &) noexcept;

    /// @brief D3d12FrameCommandState を必要な依存と初期状態から構築する
    D3d12FrameCommandState(std::array<D3d12FrameContext, k_d3d12FrameContextCount> &&a_frames,
                           Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> a_commandList,
                           D3d12QueueState &a_queueState, const AssertContext &a_assertContext,
                           const D3d12FrameCommandNativeFunctions &a_functions) noexcept;

    /// @brief 移動元の Native 所有権と Lifecycle 状態を受け取り、移動元を安全な空状態へ戻す
    void take_from(D3d12FrameCommandState &&a_other) noexcept;
    /// @brief D3D12 Frame Command State の Status From Queue を整合性を保って更新する
    void update_status_from_queue() noexcept;
    /// @brief D3D12 Frame Command State の Command List を依存関係と完了条件を守って安全に解放または停止する
    void release_command_list() noexcept;
    /// @brief D3D12 Frame Command State の Allocators を依存関係と完了条件を守って安全に解放または停止する
    void release_allocators() noexcept;
    /// @brief Fence 値を予約できない閉じた Frame を未実行のまま破棄し、再利用状態へ戻す
    [[nodiscard]] Result<void> discard_closed_frame_after_exhaustion() noexcept;
    /// @brief D3D12 Frame Command State の Reset Failure を規定された順序と失敗規則で処理する
    [[nodiscard]] Result<void> handle_reset_failure(Error &&a_error) noexcept;
    /// @brief D3D12 Frame Command State の Close Failure を規定された順序と失敗規則で処理する
    [[nodiscard]] Result<void> handle_close_failure(Error &&a_error) noexcept;

    std::array<D3d12FrameContext, k_d3d12FrameContextCount> m_frames;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    D3d12QueueState *m_queueState;
    const AssertContext *m_assertContext;
    D3d12FrameCommandNativeFunctions m_functions;
    std::uint64_t m_lastSubmittedFence;
    std::uint64_t m_pendingFenceValue;
    std::uint32_t m_activeFrameIndex;
    D3d12CommandListState m_commandListState;
    D3d12FrameCommandStatus m_status;
    bool m_acceptingFrames;
    bool m_isResizeSuspended;
    bool m_resizeGpuIdleProven;
};

[[nodiscard]] Result<D3d12FrameCommandState> create_d3d12_frame_command_state(
    ID3D12Device *a_device, D3d12QueueState &a_queueState, const AssertContext &a_assertContext,
    const D3d12FrameCommandNativeFunctions &a_functions = default_d3d12_frame_command_native_functions()) noexcept;
} // namespace cue
