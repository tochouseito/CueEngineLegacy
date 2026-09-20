#include <Cue/Renderer/Camera.h>

#include <Cue/Foundation/EmergencyHandler.h>
#include <Cue/Math/Angle.h>
#include <Cue/Renderer/Error.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace
{
constexpr float k_maximumDebugCameraPitchRadians = cue::math::pi() * 89.0F / 180.0F;

/// @brief 非有限なCamera操作量をRenderer Domain Errorへ変換する
[[nodiscard]] cue::Error make_invalid_motion_error(cue::EmergencyHandler &a_emergencyHandler) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(
        a_emergencyHandler, "Cue.Renderer", static_cast<std::int64_t>(cue::renderer::RendererError::InvalidCamera));
    return cue::Error::create(a_emergencyHandler, std::move(code), "Debug Camera motion must be finite");
}

/// @brief Quaternionを行Vector規約の回転Matrixへ展開する
[[nodiscard]] cue::math::Matrix4 make_rotation(cue::math::Quaternion a_rotation) noexcept
{
    const float xx = a_rotation.x * a_rotation.x;
    const float yy = a_rotation.y * a_rotation.y;
    const float zz = a_rotation.z * a_rotation.z;
    const float xy = a_rotation.x * a_rotation.y;
    const float xz = a_rotation.x * a_rotation.z;
    const float yz = a_rotation.y * a_rotation.z;
    const float xw = a_rotation.x * a_rotation.w;
    const float yw = a_rotation.y * a_rotation.w;
    const float zw = a_rotation.z * a_rotation.w;

    cue::math::Matrix4 result;
    result.values[0][0] = 1.0F - 2.0F * (yy + zz);
    result.values[0][1] = 2.0F * (xy + zw);
    result.values[0][2] = 2.0F * (xz - yw);
    result.values[1][0] = 2.0F * (xy - zw);
    result.values[1][1] = 1.0F - 2.0F * (xx + zz);
    result.values[1][2] = 2.0F * (yz + xw);
    result.values[2][0] = 2.0F * (xz + yw);
    result.values[2][1] = 2.0F * (yz - xw);
    result.values[2][2] = 1.0F - 2.0F * (xx + yy);
    return result;
}

/// @brief CameraのScaleを無視したRigid Transform逆Matrixを生成する
[[nodiscard]] cue::math::Matrix4 make_view(const cue::math::Transform &a_transform) noexcept
{
    const cue::math::Matrix4 rotation = make_rotation(a_transform.rotation());
    const cue::math::Vector3 position = a_transform.translation();
    cue::math::Matrix4 view;

    for (std::size_t row = 0U; row < 3U; ++row)
    {
        for (std::size_t column = 0U; column < 3U; ++column)
        {
            view.values[row][column] = rotation.values[column][row];
        }
    }
    view.values[3][0] =
        -(position.x * rotation.values[0][0] + position.y * rotation.values[0][1] + position.z * rotation.values[0][2]);
    view.values[3][1] =
        -(position.x * rotation.values[1][0] + position.y * rotation.values[1][1] + position.z * rotation.values[1][2]);
    view.values[3][2] =
        -(position.x * rotation.values[2][0] + position.y * rotation.values[2][1] + position.z * rotation.values[2][2]);
    return view;
}
} // namespace

namespace cue::renderer
{
Result<DebugCamera> DebugCamera::create_default(EmergencyHandler &a_emergencyHandler) noexcept
{
    Result<math::Tolerance> tolerance = math::Tolerance::create(a_emergencyHandler, 0.00001F, 0.00001F);
    if (!tolerance)
    {
        return Result<DebugCamera>::failure(std::move(*tolerance.try_error()));
    }

    const float pitchRadians = math::to_radians(math::Degrees(18.0F)).value;
    const float halfAngle = pitchRadians * 0.5F;
    const math::Quaternion rotation{std::sin(halfAngle), 0.0F, 0.0F, std::cos(halfAngle)};
    Result<math::Transform> transform = math::Transform::create(a_emergencyHandler, {0.0F, 3.0F, -6.0F}, rotation,
                                                                {1.0F, 1.0F, 1.0F}, *tolerance.try_value());
    if (!transform)
    {
        return Result<DebugCamera>::failure(std::move(*transform.try_error()));
    }

    PerspectiveCamera camera{std::move(*transform.try_value()), math::to_radians(math::Degrees(60.0F)).value, 0.1F,
                             1000.0F};
    return Result<DebugCamera>::success(
        DebugCamera(std::move(camera), std::move(*tolerance.try_value()), 0.0F, pitchRadians));
}

DebugCamera::DebugCamera(PerspectiveCamera a_camera, math::Tolerance a_tolerance, float a_yawRadians,
                         float a_pitchRadians) noexcept
    : m_camera(std::move(a_camera)), m_tolerance(std::move(a_tolerance)), m_yawRadians(a_yawRadians),
      m_pitchRadians(a_pitchRadians)
{
}

const PerspectiveCamera &DebugCamera::camera() const noexcept
{
    return m_camera;
}

Result<void> DebugCamera::apply_motion(EmergencyHandler &a_emergencyHandler, DebugCameraMotion a_motion) noexcept
{
    if (!math::is_finite(a_motion.yawDeltaRadians) || !math::is_finite(a_motion.pitchDeltaRadians) ||
        !math::is_finite(a_motion.rightTranslation) || !math::is_finite(a_motion.upTranslation) ||
        !math::is_finite(a_motion.forwardTranslation))
    {
        return Result<void>::failure(make_invalid_motion_error(a_emergencyHandler));
    }

    float yawRadians = m_yawRadians + a_motion.yawDeltaRadians;
    if (math::is_finite(yawRadians))
    {
        yawRadians = std::remainder(yawRadians, 2.0F * math::pi());
    }
    const float pitchRadians = std::clamp(m_pitchRadians + a_motion.pitchDeltaRadians,
                                          -k_maximumDebugCameraPitchRadians, k_maximumDebugCameraPitchRadians);

    Result<math::Quaternion> pitch =
        math::from_axis_angle(a_emergencyHandler, {1.0F, 0.0F, 0.0F}, math::Radians(pitchRadians), m_tolerance);
    if (!pitch)
    {
        return Result<void>::failure(std::move(*pitch.try_error()));
    }
    Result<math::Quaternion> yaw =
        math::from_axis_angle(a_emergencyHandler, {0.0F, 1.0F, 0.0F}, math::Radians(yawRadians), m_tolerance);
    if (!yaw)
    {
        return Result<void>::failure(std::move(*yaw.try_error()));
    }
    Result<math::Quaternion> rotation =
        math::compose_rotation(a_emergencyHandler, *pitch.try_value(), *yaw.try_value(), m_tolerance);
    if (!rotation)
    {
        return Result<void>::failure(std::move(*rotation.try_error()));
    }

    Result<math::Vector3> right =
        math::rotate(a_emergencyHandler, {1.0F, 0.0F, 0.0F}, *rotation.try_value(), m_tolerance);
    if (!right)
    {
        return Result<void>::failure(std::move(*right.try_error()));
    }
    Result<math::Vector3> up = math::rotate(a_emergencyHandler, {0.0F, 1.0F, 0.0F}, *rotation.try_value(), m_tolerance);
    if (!up)
    {
        return Result<void>::failure(std::move(*up.try_error()));
    }
    Result<math::Vector3> forward =
        math::rotate(a_emergencyHandler, {0.0F, 0.0F, 1.0F}, *rotation.try_value(), m_tolerance);
    if (!forward)
    {
        return Result<void>::failure(std::move(*forward.try_error()));
    }

    const math::Vector3 translation =
        m_camera.transform.translation() + *right.try_value() * a_motion.rightTranslation +
        *up.try_value() * a_motion.upTranslation + *forward.try_value() * a_motion.forwardTranslation;
    Result<math::Transform> transform = math::Transform::create(a_emergencyHandler, translation, *rotation.try_value(),
                                                                {1.0F, 1.0F, 1.0F}, m_tolerance);
    if (!transform)
    {
        return Result<void>::failure(std::move(*transform.try_error()));
    }

    m_camera.transform = std::move(*transform.try_value());
    m_yawRadians = yawRadians;
    m_pitchRadians = pitchRadians;
    return Result<void>::success();
}

math::Matrix4 make_world_matrix(const math::Transform &a_transform) noexcept
{
    math::Matrix4 result = make_rotation(a_transform.rotation());
    const math::Vector3 scale = a_transform.scale();
    for (std::size_t column = 0U; column < 3U; ++column)
    {
        result.values[0][column] *= scale.x;
        result.values[1][column] *= scale.y;
        result.values[2][column] *= scale.z;
    }
    const math::Vector3 translation = a_transform.translation();
    result.values[3][0] = translation.x;
    result.values[3][1] = translation.y;
    result.values[3][2] = translation.z;
    return result;
}

math::Matrix4 make_view_projection(const PerspectiveCamera &a_camera, float a_aspectRatio) noexcept
{
    const float halfFov = a_camera.verticalFovRadians * 0.5F;
    const float yScale = 1.0F / std::tan(halfFov);
    const float xScale = yScale / a_aspectRatio;
    const float depthScale = a_camera.farPlane / (a_camera.farPlane - a_camera.nearPlane);

    math::Matrix4 projection = math::zero_matrix4();
    projection.values[0][0] = xScale;
    projection.values[1][1] = yScale;
    projection.values[2][2] = depthScale;
    projection.values[2][3] = 1.0F;
    projection.values[3][2] = -a_camera.nearPlane * depthScale;
    return make_view(a_camera.transform) * projection;
}

bool is_valid(const PerspectiveCamera &a_camera) noexcept
{
    return math::is_finite(a_camera.verticalFovRadians) && math::is_finite(a_camera.nearPlane) &&
           math::is_finite(a_camera.farPlane) && a_camera.verticalFovRadians > 0.0F &&
           a_camera.verticalFovRadians < math::pi() && a_camera.nearPlane > 0.0F &&
           a_camera.farPlane > a_camera.nearPlane;
}
} // namespace cue::renderer
