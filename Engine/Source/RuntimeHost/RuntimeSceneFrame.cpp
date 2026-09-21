#include "RuntimeSceneFrame.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Math/Matrix.h>
#include <Cue/Math/Scalar.h>
#include <Cue/Renderer/Camera.h>
#include <Cue/Renderer/Error.h>

#include <cstddef>
#include <exception>
#include <new>
#include <string_view>
#include <utility>

namespace
{
/// @brief Row-major MatrixをRHI固定配列へ変換する
[[nodiscard]] std::array<float, 16> flatten(const cue::math::Matrix4 &a_matrix) noexcept
{
    std::array<float, 16> result = {};
    for (std::size_t row = 0U; row < 4U; ++row)
    {
        for (std::size_t column = 0U; column < 4U; ++column)
        {
            result[row * 4U + column] = a_matrix.values[row][column];
        }
    }
    return result;
}

/// @brief RuntimeHost Frame変換ErrorをRenderer診断境界で生成する
[[nodiscard]] cue::Error make_frame_error(const cue::AssertContext &a_assertContext,
                                          cue::renderer::RendererError a_code,
                                          std::string_view a_summary) noexcept
{
    return cue::renderer::make_renderer_error(a_assertContext, a_code, a_summary);
}
} // namespace

namespace cue::runtime_host
{
RuntimePresentationFrame::RuntimePresentationFrame(RuntimePresentationFrameMode a_mode,
                                                   std::array<float, 4> a_clearColor,
                                                   std::array<float, 16> a_viewProjection,
                                                   std::vector<PresentationSceneCube> a_cubes) noexcept
    : m_mode(a_mode), m_clearColor(a_clearColor), m_viewProjection(a_viewProjection), m_cubes(std::move(a_cubes))
{
}

RuntimePresentationFrameMode RuntimePresentationFrame::mode() const noexcept
{
    return m_mode;
}

PresentationFrameDescriptor RuntimePresentationFrame::clear_descriptor() const noexcept
{
    return {m_clearColor};
}

PresentationSceneFrameDescriptor RuntimePresentationFrame::scene_descriptor() const noexcept
{
    return {m_clearColor, m_viewProjection, m_cubes};
}

std::size_t RuntimePresentationFrame::cube_count() const noexcept
{
    return m_cubes.size();
}

Result<RuntimePresentationFrame> make_runtime_presentation_frame(const renderer::RenderSnapshot &a_snapshot,
                                                                 std::uint32_t a_width, std::uint32_t a_height,
                                                                 std::array<float, 4> a_clearColor,
                                                                 const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_width == 0U || a_height == 0U)
        {
            return Result<RuntimePresentationFrame>::failure(
                make_frame_error(a_assertContext, renderer::RendererError::InvalidRuntimeBinding,
                                 "Runtime Presentation requires a non-zero output size"));
        }
        for (float channel : a_clearColor)
        {
            if (!math::is_finite(channel))
            {
                return Result<RuntimePresentationFrame>::failure(
                    make_frame_error(a_assertContext, renderer::RendererError::InvalidRuntimeBinding,
                                     "Runtime Presentation clear color must be finite"));
            }
        }
        if (a_snapshot.meshes().size() > k_presentationSceneMaxCubeCount)
        {
            return Result<RuntimePresentationFrame>::failure(
                make_frame_error(a_assertContext, renderer::RendererError::InvalidRuntimeBinding,
                                 "Runtime Presentation Cube count exceeds the fixed Scene limit"));
        }

        std::vector<PresentationSceneCube> cubes;
        cubes.reserve(a_snapshot.meshes().size());
        for (const renderer::RenderMeshInstance &mesh : a_snapshot.meshes())
        {
            if (mesh.mesh != renderer::RenderMesh::Cube)
            {
                return Result<RuntimePresentationFrame>::failure(
                    make_frame_error(a_assertContext, renderer::RendererError::UnsupportedMesh,
                                     "Runtime Presentation encountered an unsupported Mesh"));
            }
            const math::Matrix4 world = renderer::make_world_matrix(mesh.transform);
            if (!math::is_finite(world))
            {
                return Result<RuntimePresentationFrame>::failure(
                    make_frame_error(a_assertContext, renderer::RendererError::InvalidRuntimeBinding,
                                     "Runtime Presentation Cube transform is not finite"));
            }
            cubes.push_back(PresentationSceneCube{flatten(world)});
        }

        if (a_snapshot.main_camera_status() == renderer::MainCameraStatus::Missing ||
            a_snapshot.main_camera_status() == renderer::MainCameraStatus::Multiple)
        {
            return Result<RuntimePresentationFrame>::success(RuntimePresentationFrame(
                RuntimePresentationFrameMode::DiagnosticClear, a_clearColor, {}, std::move(cubes)));
        }
        const renderer::PerspectiveCamera *camera = a_snapshot.try_main_camera();
        if (a_snapshot.main_camera_status() != renderer::MainCameraStatus::Ready || camera == nullptr ||
            !renderer::is_valid(*camera))
        {
            return Result<RuntimePresentationFrame>::failure(
                make_frame_error(a_assertContext, renderer::RendererError::InvalidCamera,
                                 "Runtime Presentation requires one valid Main Camera"));
        }

        const float aspectRatio = static_cast<float>(a_width) / static_cast<float>(a_height);
        const math::Matrix4 viewProjection = renderer::make_view_projection(*camera, aspectRatio);
        if (!math::is_finite(aspectRatio) || aspectRatio <= 0.0F || !math::is_finite(viewProjection))
        {
            return Result<RuntimePresentationFrame>::failure(
                make_frame_error(a_assertContext, renderer::RendererError::InvalidCamera,
                                 "Runtime Presentation produced an invalid Camera matrix"));
        }
        return Result<RuntimePresentationFrame>::success(RuntimePresentationFrame(
            RuntimePresentationFrameMode::Scene, a_clearColor, flatten(viewProjection), std::move(cubes)));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Runtime Presentation Frame allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Runtime Presentation Frame conversion caught an unexpected exception");
    }
    std::terminate();
}

Result<PresentationFrameStatus> present_runtime_presentation_frame(PresentationContext &a_presentation,
                                                                    const RuntimePresentationFrame &a_frame) noexcept
{
    if (a_frame.mode() == RuntimePresentationFrameMode::Scene)
    {
        return a_presentation.present_scene_frame(a_frame.scene_descriptor());
    }
    return a_presentation.present_frame(a_frame.clear_descriptor());
}
} // namespace cue::runtime_host
