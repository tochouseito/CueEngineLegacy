#include <Cue/Editor/ImGui/GameView.h>

#include <Cue/Editor/ImGui/EditorDockspace.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <imgui.h>

namespace
{
constexpr std::uint32_t k_maximumGameViewDimension = 16384U;

/// @brief ImGui表示寸法を有効なRender Surface Pixel寸法へ切り詰める
[[nodiscard]] std::uint32_t surface_dimension(float a_value) noexcept
{
    if (!std::isfinite(a_value) || a_value < 1.0F)
    {
        return 0U;
    }
    const float clamped = (std::min)(a_value, static_cast<float>(k_maximumGameViewDimension));
    return static_cast<std::uint32_t>(clamped);
}
} // namespace

namespace cue::editor
{
GameViewRequest draw_game_view(GameViewSurface a_surface) noexcept
{
    dock_editor_window_on_first_use();
    GameViewRequest request;
    const bool contentsVisible = ImGui::Begin("Game View");
    if (contentsVisible)
    {
        request.isWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const ImVec2 available = ImGui::GetContentRegionAvail();
        request.width = surface_dimension(available.x);
        request.height = surface_dimension(available.y);
        request.isVisible = request.width != 0U && request.height != 0U;
        if (request.isVisible && a_surface.textureId != 0U)
        {
            const ImVec2 minimum = ImGui::GetCursorScreenPos();
            static_cast<void>(ImGui::InvisibleButton("##GameViewViewport", available,
                                                     ImGuiButtonFlags_MouseButtonLeft |
                                                         ImGuiButtonFlags_MouseButtonRight |
                                                         ImGuiButtonFlags_MouseButtonMiddle));
            request.isViewportHovered = ImGui::IsItemHovered();
            request.isViewportActive = ImGui::IsItemActive();
            const ImVec2 maximum{minimum.x + available.x, minimum.y + available.y};
            ImGui::GetWindowDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(a_surface.textureId)), minimum,
                                                 maximum);
        }
        else if (request.isVisible)
        {
            ImGui::TextUnformatted("Game Viewを準備しています...");
        }
    }
    ImGui::End();
    return request;
}
} // namespace cue::editor
