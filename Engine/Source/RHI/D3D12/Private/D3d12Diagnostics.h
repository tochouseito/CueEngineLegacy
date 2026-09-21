// D3D12 Debug Layer、InfoQueue、DRED、Live Object の設定と収集を統括する内部診断契約
// Device 生成前後で必要な設定時点を分け、DRED が有効な場合は Native Object 解放前の原因収集を試行する

#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/RHI/D3D12/D3d12Backend.h>

#include <string>
#include <string_view>

struct ID3D12Device;

namespace cue
{
class AssertContext;

struct D3d12DiagnosticsStatus final
{
    bool isDebugLayerEnabled;
    bool isDredEnabled;
    bool isInfoQueueEnabled;
};

/// @brief DRED 診断で利用可能な Object 名表現を優先順位に従って選択する
[[nodiscard]] std::string_view select_d3d12_dred_name(
    const char *a_utf8Name, const wchar_t *a_utf16Name, std::string_view a_fallback,
    std::string &a_storage, const AssertContext &a_assertContext) noexcept;

// Process の診断許可だけを副作用なしで問い合わせ、Release Build の既定無効化を一箇所へ集約する
/// @brief Build 設定と実行 Mode から D3D12 診断を有効化できるか判定する
[[nodiscard]] bool are_d3d12_diagnostics_allowed() noexcept;

// Debug Layer と DRED は Device 生成前に有効化する必要があるため、Device Factory 処理より先に呼び出す
/// @brief D3D12 Device 生成前に Debug Layer と DRED 設定を適用する
[[nodiscard]] Result<D3d12DiagnosticsStatus> configure_d3d12_pre_device_diagnostics(
    const D3d12BackendDescriptor &a_descriptor, const AssertContext &a_assertContext) noexcept;

/// @brief D3D12 Info Queue の Filter と Break 条件を診断設定へ合わせる
[[nodiscard]] Result<void> configure_d3d12_info_queue(ID3D12Device *a_device,
                                                      D3d12DiagnosticsStatus &a_status,
                                                      const AssertContext &a_assertContext) noexcept;

/// @brief Debug Layer 有効時に M03 Device の Live Object 診断を出力する
[[nodiscard]] Result<void> report_d3d12_live_device_objects(
    ID3D12Device *a_device, const D3d12DiagnosticsStatus &a_status,
    const AssertContext &a_assertContext) noexcept;

/// @brief InfoQueue が有効な場合に、D3D12 Message を追加生成できない静止点で記録して消費する
/// @param a_device 診断対象の D3D12 Device
/// @param a_status Device へ適用済みの診断状態
/// @param a_context Message へ付与する上位 Context
/// @param a_assertContext Foundation 診断 Context
/// @return InfoQueue 無効時または記録と消費の成功時は Success、取得または記録失敗時は Error
/// @pre 通常終了では全 Command Queue の対象 Work が Fence 完了済みであること
/// @pre Device Removal では有効な場合に DRED 収集を一度試行した後、Queue と Fence を解放済みであること
/// @pre どちらの経路も他 Thread を含め D3D12 API 呼び出しが停止していること
[[nodiscard]] Result<void> log_d3d12_messages_at_quiescent_point(
    ID3D12Device *a_device, const D3d12DiagnosticsStatus &a_status, std::string_view a_context,
    const AssertContext &a_assertContext) noexcept;

/// @brief Device Removal 後の DRED と Removal Reason を収集し、収集Interfaceの可用性を返す
[[nodiscard]] Result<void> collect_d3d12_device_removed_diagnostics(
    ID3D12Device *a_device, const D3d12DiagnosticsStatus &a_status,
    bool &a_isDredCollectionInterfaceAvailable, const AssertContext &a_assertContext) noexcept;
} // namespace cue
