#pragma once

namespace cue
{
class AssertContext;

/// @brief WARP上の固定Cube PassをOffscreen描画して中央画素がClear色と異なることを検証する
[[nodiscard]] bool verify_d3d12_scene_pixel_for_probe(const AssertContext &a_assertContext) noexcept;
} // namespace cue
