#pragma once

#include <Cue/Math/Matrix.h>
#include <Cue/Math/Transform.h>

namespace cue::renderer
{
/// @brief Perspective CameraのPortable Projection値
struct PerspectiveCamera final
{
    math::Transform transform;
    float verticalFovRadians = 1.0471975512F;
    float nearPlane = 0.1F;
    float farPlane = 1000.0F;
};

/// @brief Editor入力から独立したDebug Cameraの一Frame移動量
struct DebugCameraMotion final
{
    float yawDeltaRadians = 0.0F;
    float pitchDeltaRadians = 0.0F;
    float rightTranslation = 0.0F;
    float upTranslation = 0.0F;
    float forwardTranslation = 0.0F;
};

/// @brief Editor専用CameraのPoseとProjectionをScene永続化から分離して所有する値
class DebugCamera final
{
  public:
    /// @brief Scene原点を見下ろす既定Debug Cameraを返す
    [[nodiscard]] static Result<DebugCamera> create_default(EmergencyHandler &a_emergencyHandler) noexcept;

    /// @brief 現在のPortable Camera値を返す
    [[nodiscard]] const PerspectiveCamera &camera() const noexcept;
    /// @brief 有限な回転とLocal移動を適用し、失敗時は現在Poseを保持する
    [[nodiscard]] Result<void> apply_motion(EmergencyHandler &a_emergencyHandler, DebugCameraMotion a_motion) noexcept;

  private:
    /// @brief 検証済みCamera値を保持する
    explicit DebugCamera(PerspectiveCamera a_camera, math::Tolerance a_tolerance, float a_yawRadians,
                         float a_pitchRadians) noexcept;

    PerspectiveCamera m_camera;
    math::Tolerance m_tolerance;
    float m_yawRadians;
    float m_pitchRadians;
};

/// @brief 行Vector規約のTransformからWorld Matrixを生成する
[[nodiscard]] math::Matrix4 make_world_matrix(const math::Transform &a_transform) noexcept;
/// @brief 左手系CameraとDirect3D Depth RangeからView Projection Matrixを生成する
[[nodiscard]] math::Matrix4 make_view_projection(const PerspectiveCamera &a_camera, float a_aspectRatio) noexcept;
/// @brief Camera Projection値が描画可能範囲ならtrueを返す
[[nodiscard]] bool is_valid(const PerspectiveCamera &a_camera) noexcept;
} // namespace cue::renderer
