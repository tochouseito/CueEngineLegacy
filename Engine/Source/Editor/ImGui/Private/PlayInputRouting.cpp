#include <Cue/Editor/ImGui/PlayInputRouting.h>

namespace cue::editor
{
PlayInputRoutingResult route_play_input_events(std::span<InputEvent> a_events,
                                               PlayInputRoutingContext a_context) noexcept
{
    const bool gameMouseOwned = a_context.isGameViewportHovered || a_context.isGameViewportActive;
    const bool debugMouseOwned = a_context.isDebugViewportHovered || a_context.isDebugViewportActive;
    PlayInputRoutingResult result;
    result.capture.isKeyboardCaptured = a_context.isModalOpen || a_context.isTextInputActive ||
                                        (a_context.uiCapture.isKeyboardCaptured && !a_context.isGameViewFocused);
    result.capture.isMouseCaptured =
        a_context.isModalOpen || debugMouseOwned || (a_context.uiCapture.isMouseCaptured && !gameMouseOwned);

    for (const InputEvent &event : a_events)
    {
        const bool lifecycle = event.type == InputEventType::FocusGained || event.type == InputEventType::FocusLost ||
                               event.type == InputEventType::DeviceReset;
        const bool keyboard = event.type == InputEventType::KeyDown || event.type == InputEventType::KeyUp ||
                              event.type == InputEventType::KeyRepeat;
        const bool mouse = event.type == InputEventType::MouseMove || event.type == InputEventType::MouseButtonDown ||
                           event.type == InputEventType::MouseButtonUp || event.type == InputEventType::MouseWheel;
        if (lifecycle || (keyboard && !result.capture.isKeyboardCaptured) || (mouse && !result.capture.isMouseCaptured))
        {
            a_events[result.eventCount] = event;
            ++result.eventCount;
        }
    }
    return result;
}
} // namespace cue::editor
