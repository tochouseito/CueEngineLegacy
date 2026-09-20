#include <Cue/Editor/ImGui/DebugView.h>

#include <Cue/Editor/ImGui/EditorDockspace.h>

#include <algorithm>
#include <cmath>

#include <imgui.h>

namespace
{
constexpr std::uint32_t k_maximumDebugViewDimension = 16384U;
constexpr float k_orbitRadiansPerPixel = 0.005F;
constexpr float k_panUnitsPerPixel = 0.01F;
constexpr float k_dollyUnitsPerWheelDelta = 0.005F;

/// @brief ImGuiの利用可能寸法をD3D12 Texture寸法へ安全に変換する
[[nodiscard]] std::uint32_t to_surface_dimension(float a_value) noexcept
{
    if (!std::isfinite(a_value) || a_value < 1.0F)
    {
        return 0U;
    }
    const float clamped = (std::min)(a_value, static_cast<float>(k_maximumDebugViewDimension));
    return static_cast<std::uint32_t>(clamped);
}
} // namespace

namespace cue::editor
{
DebugViewCameraMotion DebugViewCameraInput::update(const FrameInputSnapshot &a_input,
                                                   const DebugViewRequest &a_viewport) noexcept
{
    if (!a_input.has_focus() || !a_viewport.isVisible)
    {
        reset();
        return {};
    }

    if (!a_viewport.isViewportActive)
    {
        reset();
    }
    if (a_viewport.isViewportHovered && a_viewport.isViewportActive &&
        a_input.was_mouse_button_pressed(InputMouseButton::Right))
    {
        m_isOrbiting = true;
        m_isPanning = false;
    }
    else if (a_viewport.isViewportHovered && a_viewport.isViewportActive &&
             a_input.was_mouse_button_pressed(InputMouseButton::Middle))
    {
        m_isPanning = true;
        m_isOrbiting = false;
    }

    const InputDelta delta = a_input.mouse_delta();
    DebugViewCameraMotion motion;
    if (m_isOrbiting && a_input.is_mouse_button_down(InputMouseButton::Right) &&
        !a_input.was_mouse_button_pressed(InputMouseButton::Right))
    {
        motion.yawDeltaRadians = static_cast<float>(delta.x) * k_orbitRadiansPerPixel;
        motion.pitchDeltaRadians = static_cast<float>(delta.y) * k_orbitRadiansPerPixel;
    }
    if (m_isPanning && a_input.is_mouse_button_down(InputMouseButton::Middle) &&
        !a_input.was_mouse_button_pressed(InputMouseButton::Middle))
    {
        motion.rightTranslation = -static_cast<float>(delta.x) * k_panUnitsPerPixel;
        motion.upTranslation = static_cast<float>(delta.y) * k_panUnitsPerPixel;
    }
    if (a_viewport.isViewportHovered)
    {
        motion.forwardTranslation = static_cast<float>(a_input.mouse_wheel_delta()) * k_dollyUnitsPerWheelDelta;
    }

    if (!a_input.is_mouse_button_down(InputMouseButton::Right))
    {
        m_isOrbiting = false;
    }
    if (!a_input.is_mouse_button_down(InputMouseButton::Middle))
    {
        m_isPanning = false;
    }
    return motion;
}

void DebugViewCameraInput::reset() noexcept
{
    m_isOrbiting = false;
    m_isPanning = false;
}

DebugViewRequest draw_debug_view(DebugViewSurface a_surface) noexcept
{
    dock_editor_window_on_first_use();
    DebugViewRequest request;
    const bool isOpen = ImGui::Begin("Debug View");
    if (isOpen)
    {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        request.width = to_surface_dimension(available.x);
        request.height = to_surface_dimension(available.y);
        request.isVisible = request.width > 0U && request.height > 0U;
        if (request.isVisible && a_surface.textureId != 0U && a_surface.width > 0U && a_surface.height > 0U)
        {
            const ImVec2 minimum = ImGui::GetCursorScreenPos();
            static_cast<void>(
                ImGui::InvisibleButton("##DebugViewViewport", available,
                                       ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle));
            request.isViewportHovered = ImGui::IsItemHovered();
            request.isViewportActive = ImGui::IsItemActive();
            const ImVec2 maximum{minimum.x + available.x, minimum.y + available.y};
            ImGui::GetWindowDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(a_surface.textureId)), minimum,
                                                 maximum);
        }
        else
        {
            ImGui::TextDisabled("Debug Camera render surface is preparing...");
        }
    }
    ImGui::End();
    return request;
}
} // namespace cue::editor
