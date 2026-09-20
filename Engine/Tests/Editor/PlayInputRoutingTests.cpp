#include <Cue/Editor/ImGui/PlayInputRouting.h>
#include <Cue/Input/InputState.h>

#include <array>
#include <cstdlib>
#include <span>

namespace
{
/// @brief Routing契約違反をTest失敗へ変換する
void require(bool a_condition, int a_exitCode) noexcept
{
    if (!a_condition)
    {
        std::_Exit(a_exitCode);
    }
}

/// @brief UI Capture中もFocusとResetを元の順序でRuntimeへ残す
void test_lifecycle_and_ui_capture() noexcept
{
    std::array<cue::InputEvent, 6U> events = {
        cue::InputEvent{cue::InputEventType::KeyDown, cue::InputKey::A},
        cue::InputEvent{cue::InputEventType::MouseButtonDown, cue::InputKey::None, cue::InputMouseButton::Left},
        cue::InputEvent{cue::InputEventType::FocusLost},
        cue::InputEvent{cue::InputEventType::DeviceReset},
        cue::InputEvent{cue::InputEventType::FocusGained},
        cue::InputEvent{cue::InputEventType::KeyDown, cue::InputKey::B}};
    cue::editor::PlayInputRoutingContext context;
    context.uiCapture = {true, true};
    const cue::editor::PlayInputRoutingResult routed = cue::editor::route_play_input_events(events, context);
    require(routed.capture.isKeyboardCaptured && routed.capture.isMouseCaptured, 1);
    require(routed.eventCount == 3U, 2);
    require(events[0].type == cue::InputEventType::FocusLost && events[1].type == cue::InputEventType::DeviceReset &&
                events[2].type == cue::InputEventType::FocusGained,
            3);
}

/// @brief Game Viewは一般UI Captureを上書きし、Debug ViewはMouseを優先取得する
void test_viewport_priority_and_fifo() noexcept
{
    std::array<cue::InputEvent, 4U> events = {cue::InputEvent{cue::InputEventType::KeyDown, cue::InputKey::A},
                                              cue::InputEvent{cue::InputEventType::MouseMove},
                                              cue::InputEvent{cue::InputEventType::KeyUp, cue::InputKey::A},
                                              cue::InputEvent{cue::InputEventType::MouseWheel}};
    cue::editor::PlayInputRoutingContext context;
    context.uiCapture = {true, true};
    context.isGameViewFocused = true;
    context.isGameViewportHovered = true;
    const cue::editor::PlayInputRoutingResult game = cue::editor::route_play_input_events(events, context);
    require(!game.capture.isKeyboardCaptured && !game.capture.isMouseCaptured && game.eventCount == 4U, 4);
    require(events[0].type == cue::InputEventType::KeyDown && events[1].type == cue::InputEventType::MouseMove &&
                events[2].type == cue::InputEventType::KeyUp && events[3].type == cue::InputEventType::MouseWheel,
            5);

    context.isDebugViewportActive = true;
    const cue::editor::PlayInputRoutingResult debug = cue::editor::route_play_input_events(events, context);
    require(!debug.capture.isKeyboardCaptured && debug.capture.isMouseCaptured && debug.eventCount == 2U, 6);
    require(events[0].type == cue::InputEventType::KeyDown && events[1].type == cue::InputEventType::KeyUp, 7);
}

/// @brief ModalとText InputはViewport Focusより優先され、Capture遷移で既存押下を解放する
void test_modal_text_and_pressed_state() noexcept
{
    std::array<cue::InputEvent, 2U> events = {
        cue::InputEvent{cue::InputEventType::KeyDown, cue::InputKey::A},
        cue::InputEvent{cue::InputEventType::MouseButtonDown, cue::InputKey::None, cue::InputMouseButton::Left}};
    cue::InputState runtimeInput;
    cue::editor::PlayInputRoutingContext context;
    const cue::editor::PlayInputRoutingResult initial = cue::editor::route_play_input_events(events, context);
    runtimeInput.begin_frame(initial.capture);
    for (std::size_t index = 0U; index < initial.eventCount; ++index)
    {
        runtimeInput.apply_event(events[index]);
    }
    require(runtimeInput.snapshot().is_key_down(cue::InputKey::A) &&
                runtimeInput.snapshot().is_mouse_button_down(cue::InputMouseButton::Left),
            8);

    context.isGameViewFocused = true;
    context.isGameViewportHovered = true;
    context.isTextInputActive = true;
    const cue::editor::PlayInputRoutingResult text = cue::editor::route_play_input_events(events, context);
    require(text.capture.isKeyboardCaptured && !text.capture.isMouseCaptured && text.eventCount == 1U &&
                events[0].type == cue::InputEventType::MouseButtonDown,
            9);

    context.isModalOpen = true;
    const cue::editor::PlayInputRoutingResult modal = cue::editor::route_play_input_events(events, context);
    require(modal.capture.isKeyboardCaptured && modal.capture.isMouseCaptured && modal.eventCount == 0U, 10);
    runtimeInput.begin_frame(modal.capture);
    require(runtimeInput.snapshot().was_key_released(cue::InputKey::A) &&
                runtimeInput.snapshot().was_mouse_button_released(cue::InputMouseButton::Left),
            11);
    require(!runtimeInput.snapshot().is_key_down(cue::InputKey::A) &&
                !runtimeInput.snapshot().is_mouse_button_down(cue::InputMouseButton::Left),
            12);
}
} // namespace

/// @brief Editor ViewportとPlay Runtime間のPortable Input配送契約を検証する
int main()
{
    test_lifecycle_and_ui_capture();
    test_viewport_priority_and_fifo();
    test_modal_text_and_pressed_state();
    return 0;
}
