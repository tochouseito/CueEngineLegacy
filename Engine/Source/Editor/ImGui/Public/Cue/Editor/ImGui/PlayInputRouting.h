#pragma once

#include <Cue/Input/FrameInputSnapshot.h>
#include <Cue/Input/InputEvent.h>

#include <cstddef>
#include <span>

namespace cue::editor
{
/// @brief Editor UIとViewportが現在Frameで占有するPortable Inputの判定値
struct PlayInputRoutingContext final
{
    InputCapture uiCapture;
    bool isModalOpen = false;
    bool isTextInputActive = false;
    bool isGameViewFocused = false;
    bool isGameViewportHovered = false;
    bool isGameViewportActive = false;
    bool isDebugViewportHovered = false;
    bool isDebugViewportActive = false;
};

/// @brief Runtimeへ渡すEvent数と、既存押下を安全に解放するCapture値
struct PlayInputRoutingResult final
{
    InputCapture capture;
    std::size_t eventCount = 0U;
};

/// @brief 同じBuffer内でRuntime向けEventをFIFO順に選別しCapture値を返す
/// @details FocusとDeviceResetはUI Captureに関係なく残す。Play停止中のEvent破棄は呼び出し側が所有する
[[nodiscard]] PlayInputRoutingResult route_play_input_events(std::span<InputEvent> a_events,
                                                             PlayInputRoutingContext a_context) noexcept;
} // namespace cue::editor
