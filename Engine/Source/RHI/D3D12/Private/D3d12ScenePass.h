#pragma once

#include "D3d12FrameCommandState.h"

#include <Cue/Foundation/Result.h>
#include <Cue/RHI/PresentationContext.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace cue
{
class AssertContext;

/// @brief M24固定Cube PassのPipelineとFrame Slot別GPU入力をPresentation Owner内で保持する
class D3d12ScenePass final
{
  public:
    /// @brief Native Resourceを持たない初期状態を作る
    D3d12ScenePass() noexcept = default;
    /// @brief 明示解放済みResourceのOwnerを破棄する
    ~D3d12ScenePass() noexcept;
    /// @brief GPU Resourceの単一Ownerを維持するためCopyを禁止する
    D3d12ScenePass(const D3d12ScenePass &) = delete;
    /// @brief GPU Resourceの単一Ownerを維持するためCopy代入を禁止する
    D3d12ScenePass &operator=(const D3d12ScenePass &) = delete;
    /// @brief Presentation Contextの固定所有域から移動させない
    D3d12ScenePass(D3d12ScenePass &&) noexcept = delete;
    /// @brief Presentation Contextの固定所有域から移動させない
    D3d12ScenePass &operator=(D3d12ScenePass &&) noexcept = delete;

    /// @brief Shader、Cube Buffer、Frame Slot別Constantを生成し、失敗時は部分Resourceを戻す
    [[nodiscard]] Result<void> initialize(ID3D12Device *a_device, DXGI_FORMAT a_format,
                                          const AssertContext &a_assertContext) noexcept;
    /// @brief Fence完了またはDevice Removal確定後に保持Resourceを解放する
    void release() noexcept;
    /// @brief Native Resourceが残っている場合にtrueを返す
    [[nodiscard]] bool has_native_objects() const noexcept;
    /// @brief 検証済み入力を再利用Fence待機済みSlotへ書き、一つのCommand ListへCubeを記録する
    void record(ID3D12GraphicsCommandList *a_commandList, D3D12_CPU_DESCRIPTOR_HANDLE a_rtv, std::uint32_t a_frameIndex,
                std::uint32_t a_width, std::uint32_t a_height,
                const PresentationSceneFrameDescriptor &a_descriptor) noexcept;

  private:
    /// @brief 初期化中だけ部分Resourceを許し、呼出し側が失敗時に一括解放する
    [[nodiscard]] Result<void> create_resources(ID3D12Device *a_device, DXGI_FORMAT a_format,
                                                const AssertContext &a_assertContext) noexcept;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertices;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indices;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, k_d3d12FrameContextCount> m_constants;
    std::array<std::byte *, k_d3d12FrameContextCount> m_mappedConstants = {};
};
} // namespace cue
