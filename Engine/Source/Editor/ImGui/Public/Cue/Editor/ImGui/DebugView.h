#pragma once

#include <Cue/Input/FrameInputSnapshot.h>

#include <cstdint>

namespace cue::editor
{
/// @brief ToolHost所有TextureをDebugViewへ渡す非所有値
struct DebugViewSurface final
{
    std::uint64_t textureId = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

/// @brief DebugViewが次Frameに必要とする描画領域
struct DebugViewRequest final
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    bool isVisible = false;
    bool isViewportHovered = false;
    bool isViewportActive = false;
};

/// @brief Debug Viewが占有したMouse入力から生成するCamera操作量
struct DebugViewCameraMotion final
{
    float yawDeltaRadians = 0.0F;
    float pitchDeltaRadians = 0.0F;
    float rightTranslation = 0.0F;
    float upTranslation = 0.0F;
    float forwardTranslation = 0.0F;
};

/// @brief Debug ViewのHoverとActive状態に限定してPortable Mouse入力をCamera操作へ変換する
class DebugViewCameraInput final
{
  public:
    /// @brief 現在Frameの入力とViewport状態から一Frame分のCamera操作量を返す
    [[nodiscard]] DebugViewCameraMotion update(const FrameInputSnapshot &a_input,
                                               const DebugViewRequest &a_viewport) noexcept;
    /// @brief Focus喪失やCamera更新失敗後に継続中のDrag状態を解除する
    void reset() noexcept;

  private:
    bool m_isOrbiting = false;
    bool m_isPanning = false;
};

/// @brief DebugCameraのOffscreen Textureを表示し次Frameの描画寸法を返す
[[nodiscard]] DebugViewRequest draw_debug_view(DebugViewSurface a_surface) noexcept;
} // namespace cue::editor
