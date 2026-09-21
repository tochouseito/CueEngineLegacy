#include "RuntimeSceneFrame.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Renderer/Camera.h>
#include <Cue/Renderer/Error.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Unit Test中のFatalを固定Process失敗へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(76);
    }

    /// @brief Unit Test中のMessage付きFatalを固定Process失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

class FakePresentation final : public cue::PresentationContext
{
  public:
    /// @brief 現在のFake Lifecycle状態を返す
    [[nodiscard]] cue::PresentationContextState state() const noexcept override
    {
        return m_state;
    }

    /// @brief 固定幅を返す
    [[nodiscard]] std::uint32_t width() const noexcept override
    {
        return 1280U;
    }

    /// @brief 固定高さを返す
    [[nodiscard]] std::uint32_t height() const noexcept override
    {
        return 720U;
    }

    /// @brief 固定Buffer数を返す
    [[nodiscard]] std::uint32_t buffer_count() const noexcept override
    {
        return 2U;
    }

    /// @brief 固定Back Buffer Indexを返す
    [[nodiscard]] std::uint32_t current_back_buffer_index() const noexcept override
    {
        return 0U;
    }

    /// @brief Fake VSync設定を返す
    [[nodiscard]] bool is_vsync_enabled() const noexcept override
    {
        return true;
    }

    /// @brief FakeではTearing非対応を返す
    [[nodiscard]] bool is_tearing_supported() const noexcept override
    {
        return false;
    }

    /// @brief FakeではTearing無効を返す
    [[nodiscard]] bool is_tearing_enabled() const noexcept override
    {
        return false;
    }

    /// @brief FakeではResize待機なしを返す
    [[nodiscard]] bool is_resize_pending() const noexcept override
    {
        return false;
    }

    /// @brief Clear経路の同期呼出しと値借用を記録する
    [[nodiscard]] cue::Result<cue::PresentationFrameStatus> present_frame(
        const cue::PresentationFrameDescriptor &a_descriptor) noexcept override
    {
        ++clearCount;
        lastClearColor = a_descriptor.clearColor;
        return cue::Result<cue::PresentationFrameStatus>::success(cue::PresentationFrameStatus::Presented);
    }

    /// @brief Scene経路の同期呼出しと値借用を記録する
    [[nodiscard]] cue::Result<cue::PresentationFrameStatus> present_scene_frame(
        const cue::PresentationSceneFrameDescriptor &a_descriptor) noexcept override
    {
        ++sceneCount;
        lastClearColor = a_descriptor.clearColor;
        lastViewProjection = a_descriptor.viewProjection;
        lastCubeCount = a_descriptor.cubes.size();
        return cue::Result<cue::PresentationFrameStatus>::success(cue::PresentationFrameStatus::Presented);
    }

    /// @brief Fake Resizeを成功させる
    [[nodiscard]] cue::Result<void> resize(std::uint32_t, std::uint32_t) noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief Fake PresentationをShutdownへ移行する
    [[nodiscard]] cue::Result<void> shutdown() noexcept override
    {
        m_state = cue::PresentationContextState::Shutdown;
        return cue::Result<void>::success();
    }

    std::uint32_t clearCount = 0U;
    std::uint32_t sceneCount = 0U;
    std::size_t lastCubeCount = 0U;
    std::array<float, 4> lastClearColor = {};
    std::array<float, 16> lastViewProjection = {};

  private:
    cue::PresentationContextState m_state = cue::PresentationContextState::Ready;
};

/// @brief 条件違反をRuntime Scene Frame Test失敗へ変換する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::_Exit(2);
    }
}

/// @brief Renderer Error Codeが期待値と一致するか返す
[[nodiscard]] bool has_error(const cue::Error *a_error, cue::renderer::RendererError a_expected) noexcept
{
    return a_error != nullptr && a_error->code().domain() == "Cue.Renderer" &&
           a_error->code().value() == static_cast<std::int64_t>(a_expected);
}

/// @brief 既定Cameraを持つReady Snapshotを生成する
[[nodiscard]] cue::renderer::RenderSnapshot make_ready_snapshot(cue::FatalHandler &a_fatalHandler) noexcept
{
    cue::Result<cue::renderer::DebugCamera> camera = cue::renderer::DebugCamera::create_default(a_fatalHandler);
    require(camera.has_value());
    std::vector<cue::renderer::RenderMeshInstance> meshes = {
        {cue::math::Transform{}, cue::renderer::RenderMesh::Cube},
    };
    return cue::renderer::RenderSnapshot(cue::renderer::MainCameraStatus::Ready,
                                         camera.try_value()->camera(), std::move(meshes), 7U);
}

/// @brief Missing／Multiple CameraがClearを選び、Ready CameraがSceneを選ぶことを検証する
void test_mode_selection(const cue::AssertContext &a_assertContext) noexcept
{
    constexpr std::array<float, 4> clearColor = {0.06F, 0.18F, 0.32F, 1.0F};
    std::vector<cue::renderer::RenderMeshInstance> missingMeshes = {
        {cue::math::Transform{}, cue::renderer::RenderMesh::Cube},
    };
    cue::renderer::RenderSnapshot missing(cue::renderer::MainCameraStatus::Missing, std::nullopt,
                                          std::move(missingMeshes), 1U);
    cue::renderer::RenderSnapshot multiple(cue::renderer::MainCameraStatus::Multiple, std::nullopt, {}, 2U);
    cue::renderer::RenderSnapshot ready = make_ready_snapshot(a_assertContext.fatal_handler());
    cue::Result<cue::runtime_host::RuntimePresentationFrame> missingFrame =
        cue::runtime_host::make_runtime_presentation_frame(missing, 1280U, 720U, clearColor, a_assertContext);
    cue::Result<cue::runtime_host::RuntimePresentationFrame> multipleFrame =
        cue::runtime_host::make_runtime_presentation_frame(multiple, 1280U, 720U, clearColor, a_assertContext);
    cue::Result<cue::runtime_host::RuntimePresentationFrame> sceneFrame =
        cue::runtime_host::make_runtime_presentation_frame(ready, 1280U, 720U, clearColor, a_assertContext);
    require(missingFrame && multipleFrame && sceneFrame);
    require(missingFrame.try_value()->mode() == cue::runtime_host::RuntimePresentationFrameMode::DiagnosticClear &&
            missingFrame.try_value()->cube_count() == 1U);
    require(multipleFrame.try_value()->mode() == cue::runtime_host::RuntimePresentationFrameMode::DiagnosticClear);
    require(sceneFrame.try_value()->mode() == cue::runtime_host::RuntimePresentationFrameMode::Scene &&
            sceneFrame.try_value()->cube_count() == 1U);

    FakePresentation presentation;
    require(cue::runtime_host::present_runtime_presentation_frame(presentation, *missingFrame.try_value()).has_value());
    require(cue::runtime_host::present_runtime_presentation_frame(presentation, *sceneFrame.try_value()).has_value());
    require(presentation.clearCount == 1U && presentation.sceneCount == 1U && presentation.lastCubeCount == 1U &&
            presentation.lastClearColor == clearColor);
    require(presentation.shutdown().has_value());
}

/// @brief 未知Mesh、不正Camera、無効出力Size、Cube上限超過をErrorにすることを検証する
void test_invalid_snapshot(const cue::AssertContext &a_assertContext) noexcept
{
    constexpr std::array<float, 4> clearColor = {0.06F, 0.18F, 0.32F, 1.0F};
    std::vector<cue::renderer::RenderMeshInstance> unknownMeshes = {
        {cue::math::Transform{}, static_cast<cue::renderer::RenderMesh>(0xffU)},
    };
    cue::renderer::RenderSnapshot unknown(cue::renderer::MainCameraStatus::Missing, std::nullopt,
                                          std::move(unknownMeshes), 1U);
    cue::renderer::RenderSnapshot invalidCamera(cue::renderer::MainCameraStatus::Ready, std::nullopt, {}, 2U);
    std::vector<cue::renderer::RenderMeshInstance> tooManyMeshes(
        cue::k_presentationSceneMaxCubeCount + 1U,
        cue::renderer::RenderMeshInstance{cue::math::Transform{}, cue::renderer::RenderMesh::Cube});
    cue::renderer::RenderSnapshot tooMany(cue::renderer::MainCameraStatus::Missing, std::nullopt,
                                          std::move(tooManyMeshes), 3U);

    auto unknownFrame =
        cue::runtime_host::make_runtime_presentation_frame(unknown, 1280U, 720U, clearColor, a_assertContext);
    auto cameraFrame =
        cue::runtime_host::make_runtime_presentation_frame(invalidCamera, 1280U, 720U, clearColor, a_assertContext);
    auto sizeFrame =
        cue::runtime_host::make_runtime_presentation_frame(invalidCamera, 0U, 720U, clearColor, a_assertContext);
    auto countFrame =
        cue::runtime_host::make_runtime_presentation_frame(tooMany, 1280U, 720U, clearColor, a_assertContext);
    require(!unknownFrame && has_error(unknownFrame.try_error(), cue::renderer::RendererError::UnsupportedMesh));
    require(!cameraFrame && has_error(cameraFrame.try_error(), cue::renderer::RendererError::InvalidCamera));
    require(!sizeFrame && has_error(sizeFrame.try_error(), cue::renderer::RendererError::InvalidRuntimeBinding));
    require(!countFrame && has_error(countFrame.try_error(), cue::renderer::RendererError::InvalidRuntimeBinding));
}
} // namespace

int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    test_mode_selection(assertContext);
    test_invalid_snapshot(assertContext);
    return 0;
}
