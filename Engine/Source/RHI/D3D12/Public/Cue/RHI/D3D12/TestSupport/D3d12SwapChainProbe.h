#pragma once

#include <Cue/Foundation/Result.h>

#include <array>
#include <cstdint>

namespace cue
{
class AssertContext;
class D3d12Backend;
class PresentationContext;

enum class D3d12PresentFailureProbeMode
{
    RecoveryMatrix,
    BeginFrameUnavailable,
    CloseFrameDeviceRemoved,
    PresentUnavailable,
    SignalUnavailable,
    DirectPresentDeviceRemoved,
    RecoverySignalDeviceRemoved,
    RegularSignalDeviceRemoved,
};

struct D3d12SwapChainProbeReport final
{
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t bufferCount;
    std::uint32_t currentBackBufferIndex;
    std::uint64_t infoQueueErrorCount;
    bool descriptorShapeIsValid;
    bool backBuffersAreAvailable;
    bool altEnterWasDisabled;
    bool isVsyncEnabled;
    bool isTearingSupported;
    bool isTearingEnabled;
    bool diagnosticsAvailable;
};

struct D3d12PresentationProbeReport final
{
    std::uint64_t lastSubmittedFence;
    std::array<std::uint64_t, 2> frameReuseFences;
    std::uint32_t allocatorCount;
    std::uint32_t backBufferCount;
    std::uint32_t rtvCount;
    bool formatsMatch;
    bool isAcceptingFrames;
    bool hasCommandList;
    bool hasSwapChain;
    bool hasRtvHeap;
    bool isRegistered;
    std::uint32_t sceneDepthWidth;
    std::uint32_t sceneDepthHeight;
    bool hasSceneDepthDsv;
};

struct D3d12BackendOwnerProbeReport final
{
    std::uint64_t lastSignaledFence;
    bool hasQueue;
    bool hasFence;
    bool hasFenceEvent;
    bool hasDevice;
    bool hasAdapter;
    bool hasFactory;
};

struct D3d12DredOwnerProbeReport final
{
    std::uint32_t allocatorCount;
    std::uint32_t backBufferCount;
    std::uint32_t rtvCount;
    bool hasCommandList;
    bool hasSwapChain;
    bool hasRtvHeap;
    bool hasQueue;
    bool hasFence;
    bool hasFenceEvent;
};

/// @brief 実Win32 WindowとWARP QueueでSwap Chain生成・解放を検証する
[[nodiscard]] Result<D3d12SwapChainProbeReport> probe_d3d12_swap_chain(const void *a_nativeWindow,
                                                                       std::uint32_t a_width, std::uint32_t a_height,
                                                                       bool a_isVsyncEnabled,
                                                                       const AssertContext &a_assertContext) noexcept;

/// @brief VSyncとTearing capabilityからSwap Chain flagが一意に決まることを検証する
[[nodiscard]] bool verify_d3d12_swap_chain_tearing_matrix_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                                    std::uint32_t a_height,
                                                                    const AssertContext &a_assertContext) noexcept;

/// @brief VSync、Tearing、OccludedのPresent引数と結果を検証する
[[nodiscard]] bool verify_d3d12_swap_chain_present_matrix_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                                    std::uint32_t a_height,
                                                                    const AssertContext &a_assertContext) noexcept;

/// @brief DXGI生成、Association、Interface、Back Buffer取得、命名失敗のrollbackを検証する
[[nodiscard]] bool verify_d3d12_swap_chain_faults_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                            std::uint32_t a_height,
                                                            const AssertContext &a_assertContext) noexcept;

/// @brief 診断Log失敗時のrollbackと再生成を検証する
[[nodiscard]] bool verify_d3d12_swap_chain_log_failure_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                                 std::uint32_t a_height,
                                                                 const AssertContext &a_setupAssertContext,
                                                                 const AssertContext &a_failingAssertContext) noexcept;

/// @brief 無効DescriptorとCurrent Back Buffer Indexを診断Errorとして検証する
[[nodiscard]] bool verify_d3d12_swap_chain_validation_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                                std::uint32_t a_height,
                                                                const AssertContext &a_assertContext) noexcept;

/// @brief ResizeBuffers失敗後に旧状態へ戻さず診断可能な所有状態を保つことを検証する
[[nodiscard]] bool verify_d3d12_swap_chain_resize_failure_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                                    std::uint32_t a_height,
                                                                    const AssertContext &a_assertContext) noexcept;

/// @brief RTV再構築失敗後にPresentationを規定順で解放し、Backend登録を解除することを検証する
[[nodiscard]] bool verify_d3d12_rtv_rebuild_failure_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                              std::uint32_t a_height,
                                                              AssertContext &a_assertContext) noexcept;

/// @brief Terminal Signal Errorで停止したProduction PresentationをResizeが再開しないことを検証する
[[nodiscard]] bool verify_d3d12_terminal_resize_rejection_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                                    std::uint32_t a_height,
                                                                    AssertContext &a_assertContext) noexcept;

/// @brief Present失敗後の補完Signalと通常Present後のSignal失敗回収を検証する
[[nodiscard]] bool verify_d3d12_present_signal_recovery_for_probe(const void *a_nativeWindow, std::uint32_t a_width,
                                                                  std::uint32_t a_height,
                                                                  D3d12PresentFailureProbeMode a_mode,
                                                                  AssertContext &a_assertContext) noexcept;

/// @brief Present経路のDevice Removal Probeが現在のD3D12 Deviceで利用できなかったかを返す
[[nodiscard]] bool was_d3d12_present_device_removal_probe_unavailable() noexcept;

/// @brief GPU完了未証明時にProduction Presentationが全ResourceとBackend登録を保持することを検証する
[[nodiscard]] bool verify_d3d12_resize_unavailable_retention_for_probe(const void *a_nativeWindow,
                                                                       std::uint32_t a_width, std::uint32_t a_height,
                                                                       AssertContext &a_assertContext) noexcept;

/// @brief Test用にD3D12 Device Removalを発生させ、Backendへ分類させる
[[nodiscard]] Result<void> force_d3d12_device_removal_for_probe(D3d12Backend &a_backend) noexcept;

/// @brief Test用にBackendへ未分類のD3D12 Device Removalを発生させる
[[nodiscard]] Result<void> remove_d3d12_device_without_classification_for_probe(D3d12Backend &a_backend) noexcept;

/// @brief BackendがDRED収集を試行した回数を返す
[[nodiscard]] Result<std::uint32_t> d3d12_dred_attempt_count_for_probe(D3d12Backend &a_backend) noexcept;

/// @brief Production BackendのNative Owner個別保持状態を返す
[[nodiscard]] Result<D3d12BackendOwnerProbeReport> probe_d3d12_backend_owners_for_probe(
    D3d12Backend &a_backend) noexcept;

/// @brief 直近DRED試行時点のPresentationとQueueのOwner個別保持状態を返す
[[nodiscard]] Result<D3d12DredOwnerProbeReport> probe_d3d12_dred_owners_for_probe(D3d12Backend &a_backend) noexcept;

/// @brief Production PresentationのBack BufferとRTV対応状態を返す
[[nodiscard]] D3d12PresentationProbeReport probe_d3d12_presentation(PresentationContext &a_presentation) noexcept;

/// @brief Production PresentationのCurrent Back Bufferへ往復Barrierを記録してSubmitする
[[nodiscard]] bool submit_d3d12_transition_frame_for_probe(PresentationContext &a_presentation) noexcept;

/// @brief Production PresentationのCurrent Back Bufferを指定色でClearしてSubmitする
[[nodiscard]] bool submit_d3d12_clear_frame_for_probe(PresentationContext &a_presentation,
                                                      const std::array<float, 4> &a_color) noexcept;
} // namespace cue
