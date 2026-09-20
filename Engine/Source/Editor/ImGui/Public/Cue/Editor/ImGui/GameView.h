#pragma once

#include <cstdint>

namespace cue::editor
{
/// @brief Game Viewが表示する非所有ImGui Textureと実Resource寸法
struct GameViewSurface final
{
    std::uint64_t textureId = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

/// @brief Game Viewの現在の表示領域から算出した次Frame Surface要求
struct GameViewRequest final
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    bool isVisible = false;
    bool isViewportHovered = false;
    bool isViewportActive = false;
    bool isWindowFocused = false;
};

/// @brief Docking可能なGame ViewへSurfaceを表示し次Frame要求を返す
/// @pre 呼び出しThreadに有効なImGui Contextが設定され、ImGui Frameが開始済みであること
[[nodiscard]] GameViewRequest draw_game_view(GameViewSurface a_surface) noexcept;
} // namespace cue::editor
