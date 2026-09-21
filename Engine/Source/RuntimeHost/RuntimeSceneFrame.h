#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/RHI/PresentationContext.h>
#include <Cue/Renderer/RenderSnapshot.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::runtime_host
{
/// @brief Runtime Snapshotから選択されたStandalone表示方式
enum class RuntimePresentationFrameMode : std::uint8_t
{
    DiagnosticClear,
    Scene
};

/// @brief RHI同期呼出し中に借用するScene値をRuntimeHost側で所有する一Frame値
class RuntimePresentationFrame final
{
  public:
    /// @brief 表示方式、Clear色、Camera Matrix、Cube列を所有する
    RuntimePresentationFrame(RuntimePresentationFrameMode a_mode, std::array<float, 4> a_clearColor,
                             std::array<float, 16> a_viewProjection,
                             std::vector<PresentationSceneCube> a_cubes) noexcept;

    /// @brief 選択された表示方式を返す
    [[nodiscard]] RuntimePresentationFrameMode mode() const noexcept;
    /// @brief 所有値を借用するClear Frame Descriptorを返す
    [[nodiscard]] PresentationFrameDescriptor clear_descriptor() const noexcept;
    /// @brief 所有値を借用するScene Frame Descriptorを返す
    [[nodiscard]] PresentationSceneFrameDescriptor scene_descriptor() const noexcept;
    /// @brief 所有するCube数を返す
    [[nodiscard]] std::size_t cube_count() const noexcept;

  private:
    RuntimePresentationFrameMode m_mode;
    std::array<float, 4> m_clearColor;
    std::array<float, 16> m_viewProjection;
    std::vector<PresentationSceneCube> m_cubes;
};

/// @brief CPU RenderSnapshotを検証してStandalone RHI用の所有Frame値へ変換する
[[nodiscard]] Result<RuntimePresentationFrame> make_runtime_presentation_frame(
    const renderer::RenderSnapshot &a_snapshot, std::uint32_t a_width, std::uint32_t a_height,
    std::array<float, 4> a_clearColor, const AssertContext &a_assertContext) noexcept;

/// @brief RuntimeHost所有Frame値を選択済みのRHI同期経路へ投入する
[[nodiscard]] Result<PresentationFrameStatus> present_runtime_presentation_frame(
    PresentationContext &a_presentation, const RuntimePresentationFrame &a_frame) noexcept;
} // namespace cue::runtime_host
