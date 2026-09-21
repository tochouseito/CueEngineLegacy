#pragma once

#include <cstdint>

namespace cue
{
class AssertContext;

/// @brief 固定Scene Passで検証する画素契約を指定する
enum class D3d12ScenePixelCase
{
    Basic,
    DepthFarReference,
    DepthReverseOrder,
    Depth,
    BackFaceCull,
};

/// @brief Offscreen描画に用いるGPU種別を指定する
enum class D3d12SceneProbeAdapter
{
    Warp,
    Hardware,
};

/// @brief Scene画素検証の結果とHardware未搭載を区別する
enum class D3d12SceneProbeResult
{
    Passed,
    Failed,
    HardwareUnavailable,
};

/// @brief GPU上の固定Cube PassをOffscreen描画して色、Depth、裏面Cullの指定された画素を検証する
[[nodiscard]] D3d12SceneProbeResult verify_d3d12_scene_pixel_for_probe(const AssertContext &a_assertContext,
                                                                       D3d12ScenePixelCase a_case,
                                                                       D3d12SceneProbeAdapter a_adapter,
                                                                       std::uint32_t a_surfaceSize) noexcept;
} // namespace cue
