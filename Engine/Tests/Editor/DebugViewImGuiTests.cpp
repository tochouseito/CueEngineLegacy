#include <Cue/Editor/ImGui/DebugView.h>
#include <Cue/Editor/ImGui/EditorDockspace.h>
#include <Cue/Input/InputState.h>

#include <cmath>
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

/// @brief 固定Editor DockspaceとDebug Viewの一Frameを構築する
[[nodiscard]] cue::editor::DebugViewRequest draw_frame(cue::editor::DebugViewSurface a_surface) noexcept
{
    ImGui::NewFrame();
    static_cast<void>(cue::editor::begin_editor_dockspace_host());
    cue::editor::end_editor_dockspace_host();
    const cue::editor::DebugViewRequest request = cue::editor::draw_debug_view(a_surface);
    ImGui::Render();
    return request;
}

/// @brief Portable Mouse入力をDebug View内だけで回転、Pan、Dollyへ変換することを検証する
void test_camera_input() noexcept
{
    cue::InputState input;
    cue::editor::DebugViewCameraInput cameraInput;
    const cue::editor::DebugViewRequest interactive{640U, 360U, true, true, true};

    input.begin_frame({});
    input.apply_event({cue::InputEventType::MouseMove, cue::InputKey::None, cue::InputMouseButton::None, {10, 20}});
    input.apply_event(
        {cue::InputEventType::MouseButtonDown, cue::InputKey::None, cue::InputMouseButton::Right, {10, 20}});
    const cue::editor::DebugViewCameraMotion pressed = cameraInput.update(input.snapshot(), interactive);
    require(pressed.yawDeltaRadians == 0.0F && pressed.pitchDeltaRadians == 0.0F, 6);

    input.begin_frame({});
    input.apply_event({cue::InputEventType::MouseMove, cue::InputKey::None, cue::InputMouseButton::None, {30, 30}});
    const cue::editor::DebugViewCameraMotion orbit = cameraInput.update(input.snapshot(), interactive);
    require(std::abs(orbit.yawDeltaRadians - 0.1F) < 0.000001F && std::abs(orbit.pitchDeltaRadians - 0.05F) < 0.000001F,
            7);

    input.begin_frame({});
    input.apply_event({cue::InputEventType::FocusLost});
    const cue::editor::DebugViewCameraMotion unfocused = cameraInput.update(input.snapshot(), interactive);
    require(unfocused.yawDeltaRadians == 0.0F && unfocused.pitchDeltaRadians == 0.0F, 8);

    input.begin_frame({});
    input.apply_event({cue::InputEventType::FocusGained});
    input.apply_event(
        {cue::InputEventType::MouseWheel, cue::InputKey::None, cue::InputMouseButton::None, {30, 30}, 120});
    cue::editor::DebugViewRequest hovered = interactive;
    hovered.isViewportActive = false;
    const cue::editor::DebugViewCameraMotion dolly = cameraInput.update(input.snapshot(), hovered);
    require(std::abs(dolly.forwardTranslation - 0.6F) < 0.000001F, 9);

    cue::InputState panInput;
    cue::editor::DebugViewCameraInput panCameraInput;
    panInput.begin_frame({});
    panInput.apply_event({cue::InputEventType::MouseMove, cue::InputKey::None, cue::InputMouseButton::None, {50, 60}});
    panInput.apply_event(
        {cue::InputEventType::MouseButtonDown, cue::InputKey::None, cue::InputMouseButton::Middle, {50, 60}});
    static_cast<void>(panCameraInput.update(panInput.snapshot(), interactive));
    panInput.begin_frame({});
    panInput.apply_event({cue::InputEventType::MouseMove, cue::InputKey::None, cue::InputMouseButton::None, {60, 65}});
    const cue::editor::DebugViewCameraMotion pan = panCameraInput.update(panInput.snapshot(), interactive);
    require(std::abs(pan.rightTranslation + 0.1F) < 0.000001F && std::abs(pan.upTranslation - 0.05F) < 0.000001F, 10);

    input.begin_frame({});
    input.apply_event(
        {cue::InputEventType::MouseButtonDown, cue::InputKey::None, cue::InputMouseButton::Right, {30, 30}});
    cue::editor::DebugViewRequest outside = interactive;
    outside.isViewportHovered = false;
    outside.isViewportActive = false;
    const cue::editor::DebugViewCameraMotion ignored = cameraInput.update(input.snapshot(), outside);
    require(ignored.yawDeltaRadians == 0.0F && ignored.pitchDeltaRadians == 0.0F && ignored.forwardTranslation == 0.0F,
            11);
}
} // namespace

/// @brief Debug ViewがEditor Dockspace内で有効なSurface寸法を要求することを検証する
int main()
{
    require(ImGui::CreateContext() != nullptr, 1);
    ImGuiIO &input = ImGui::GetIO();
    input.IniFilename = nullptr;
    input.DisplaySize = ImVec2(1280.0F, 720.0F);
    input.DeltaTime = 1.0F / 60.0F;
    cue::editor::enable_editor_docking();
    static_cast<void>(input.Fonts->Build());

    const cue::editor::DebugViewSurface surface{1U, 640U, 360U};
    const cue::editor::DebugViewRequest visible = draw_frame(surface);
    const ImGuiWindow *debugView = ImGui::FindWindowByName("Debug View");
    require(debugView != nullptr && debugView->DockId != 0U, 2);
    require(visible.isVisible && visible.width != 0U && visible.height != 0U, 3);

    const ImVec2 viewportCenter{(debugView->InnerRect.Min.x + debugView->InnerRect.Max.x) * 0.5F,
                                (debugView->InnerRect.Min.y + debugView->InnerRect.Max.y) * 0.5F};
    input.AddMousePosEvent(viewportCenter.x, viewportCenter.y);
    static_cast<void>(draw_frame(surface));
    const cue::editor::DebugViewRequest hovered = draw_frame(surface);
    require(hovered.isViewportHovered && !hovered.isViewportActive, 4);

    input.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    const cue::editor::DebugViewRequest active = draw_frame(surface);
    require(active.isViewportHovered && active.isViewportActive, 5);
    input.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    static_cast<void>(draw_frame(surface));
    ImGui::DestroyContext();
    test_camera_input();
    return 0;
}
