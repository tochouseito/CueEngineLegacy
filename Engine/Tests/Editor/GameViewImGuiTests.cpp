#include <Cue/Editor/ImGui/EditorDockspace.h>
#include <Cue/Editor/ImGui/GameView.h>

#include <cstdlib>

#include <imgui.h>
#include <imgui_internal.h>

namespace
{
/// @brief 条件違反をTest失敗へ変換する
void require(bool a_condition, int a_exitCode) noexcept
{
    if (!a_condition)
    {
        std::_Exit(a_exitCode);
    }
}

/// @brief 固定Editor DockspaceとGame Viewの一Frameを構築する
[[nodiscard]] cue::editor::GameViewRequest draw_frame(cue::editor::GameViewSurface a_surface = {}) noexcept
{
    ImGui::NewFrame();
    static_cast<void>(cue::editor::begin_editor_dockspace_host());
    cue::editor::end_editor_dockspace_host();
    const cue::editor::GameViewRequest request = cue::editor::draw_game_view(a_surface);
    ImGui::Render();
    return request;
}
} // namespace

/// @brief Game ViewがEditor Dockspace内で有効なSurface寸法を要求することを検証する
int main()
{
    require(ImGui::CreateContext() != nullptr, 1);
    ImGuiIO &input = ImGui::GetIO();
    input.IniFilename = nullptr;
    input.DisplaySize = ImVec2(1280.0F, 720.0F);
    input.DeltaTime = 1.0F / 60.0F;
    cue::editor::enable_editor_docking();
    static_cast<void>(input.Fonts->Build());

    const cue::editor::GameViewRequest visible = draw_frame();
    const ImGuiWindow *gameView = ImGui::FindWindowByName("Game View");
    require(gameView != nullptr && gameView->DockId != 0U, 2);
    require(visible.isVisible && visible.width != 0U && visible.height != 0U, 3);

    const cue::editor::GameViewSurface surface{1U, 640U, 360U};
    const ImVec2 viewportCenter{(gameView->InnerRect.Min.x + gameView->InnerRect.Max.x) * 0.5F,
                                (gameView->InnerRect.Min.y + gameView->InnerRect.Max.y) * 0.5F};
    input.AddMousePosEvent(viewportCenter.x, viewportCenter.y);
    static_cast<void>(draw_frame(surface));
    const cue::editor::GameViewRequest hovered = draw_frame(surface);
    require(hovered.isViewportHovered && !hovered.isViewportActive, 4);

    input.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    const cue::editor::GameViewRequest active = draw_frame(surface);
    require(active.isViewportHovered && active.isViewportActive && active.isWindowFocused, 5);
    input.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(draw_frame(surface));
    ImGui::DestroyContext();
    return 0;
}
