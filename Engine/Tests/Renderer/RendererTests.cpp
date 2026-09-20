#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/World.h>
#include <Cue/Renderer/Camera.h>
#include <Cue/Renderer/RenderExtraction.h>
#include <Cue/Renderer/RendererRuntimeSystem.h>
#include <Cue/Renderer/RendererSchema.h>
#include <Cue/Runtime/RuntimeApplicationSession.h>
#include <Cue/Runtime/RuntimeSchema.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Schema/Registry.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
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
    /// @brief Unit Test中の通常FatalをProcess失敗へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(77);
    }

    /// @brief Unit Test中のMessage付きFatalをProcess失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(78);
    }
};

class TestClock final : public cue::game_core::MonotonicClock
{
  public:
    /// @brief 固定16msずつ進む再現可能な単調Sampleを返す
    [[nodiscard]] cue::Result<cue::game_core::MonotonicClockSample> sample(const cue::AssertContext &) noexcept override
    {
        cue::game_core::MonotonicClockSample sample{m_nanoseconds};
        m_nanoseconds += 16'000'000;
        return cue::Result<cue::game_core::MonotonicClockSample>::success(std::move(sample));
    }

  private:
    std::int64_t m_nanoseconds = 0;
};

/// @brief 条件違反をRenderer Test失敗へ変換する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::_Exit(2);
    }
}

/// @brief 成功Resultから所有Valueを取り出す
template <typename T> [[nodiscard]] T take_value(cue::Result<T> &&a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief 固定UUIDからScene Asset Identityを生成する
[[nodiscard]] cue::scene::SceneAssetId make_scene_id(std::string_view a_id,
                                                     const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::scene::SceneAssetId::parse(a_id, a_assertContext));
}

/// @brief 固定UUIDからObject Identityを生成する
[[nodiscard]] cue::scene::ObjectId make_object_id(std::string_view a_id,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::scene::ObjectId::parse(a_id, a_assertContext));
}

/// @brief 固定UUIDからComponent Instance Identityを生成する
[[nodiscard]] cue::scene::ComponentInstanceId make_component_id(std::string_view a_id,
                                                                const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::scene::ComponentInstanceId::parse(a_id, a_assertContext));
}

/// @brief Runtime CoreとRenderer Schemaを一つのRegistryへ登録する
[[nodiscard]] std::unique_ptr<cue::schema::SchemaRegistry> make_schema_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    require(cue::runtime::add_runtime_schema_types(builder, a_assertContext).has_value());
    require(cue::renderer::add_renderer_schema_types(builder, a_assertContext).has_value());
    return take_value(builder.seal());
}

/// @brief Renderer Authoring Field Kind Registryを生成する
[[nodiscard]] cue::scene::ComponentValueSchemaRegistry make_value_schema_registry(
    const cue::schema::SchemaRegistry &a_schemaRegistry, const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::scene::ComponentValueSchema> schemas =
        take_value(cue::renderer::make_renderer_value_schemas(a_schemaRegistry, a_assertContext));
    return take_value(
        cue::scene::ComponentValueSchemaRegistry::create(std::move(schemas), a_schemaRegistry, a_assertContext));
}

/// @brief Main CameraとCubeを持つAuthoring Sceneを生成する
[[nodiscard]] cue::scene::SceneDocument make_render_scene(
    const cue::schema::SchemaRegistry &a_schemaRegistry,
    const cue::scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry,
    const cue::AssertContext &a_assertContext) noexcept
{
    cue::scene::SceneDocument document = cue::scene::SceneDocument::create(
        make_scene_id("71000000-0000-4000-8000-000000000001", a_assertContext), a_assertContext);
    const cue::scene::ObjectId cameraId = make_object_id("71000000-0000-4000-8000-000000000002", a_assertContext);
    const cue::scene::ObjectId cubeId = make_object_id("71000000-0000-4000-8000-000000000003", a_assertContext);
    cue::renderer::DebugCamera debugCamera =
        take_value(cue::renderer::DebugCamera::create_default(a_assertContext.fatal_handler()));

    require(
        document.add_object(cameraId, "Main Camera", true, std::nullopt, debugCamera.camera().transform).has_value());
    require(document
                .add_component(cameraId, take_value(cue::renderer::make_camera_component(
                                             make_component_id("71000000-0000-4000-8000-000000000004", a_assertContext),
                                             true, a_schemaRegistry, a_valueSchemaRegistry, a_assertContext)))
                .has_value());
    require(document.add_object(cubeId, "Cube", true, std::nullopt, cue::math::Transform{}).has_value());
    require(document
                .add_component(cubeId, take_value(cue::renderer::make_cube_mesh_component(
                                           make_component_id("71000000-0000-4000-8000-000000000005", a_assertContext),
                                           a_schemaRegistry, a_valueSchemaRegistry, a_assertContext)))
                .has_value());
    return document;
}

/// @brief Authoring抽出のMissing、Ready、Multiple状態とCube抽出を検証する
[[nodiscard]] cue::scene::SceneSnapshot test_authoring_extraction(
    cue::scene::SceneDocument &a_document, const cue::renderer::RendererSchemaTypeIds &a_typeIds,
    const cue::schema::SchemaRegistry &a_schemaRegistry,
    const cue::scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry,
    const cue::AssertContext &a_assertContext) noexcept
{
    cue::scene::SceneDocument empty = cue::scene::SceneDocument::create(
        make_scene_id("71000000-0000-4000-8000-000000000006", a_assertContext), a_assertContext);
    cue::renderer::RenderSnapshot missing =
        take_value(cue::renderer::extract_render_snapshot(empty, a_typeIds, 1U, a_assertContext));
    require(missing.main_camera_status() == cue::renderer::MainCameraStatus::Missing);
    require(missing.try_main_camera() == nullptr && missing.meshes().empty());

    cue::renderer::RenderSnapshot ready =
        take_value(cue::renderer::extract_render_snapshot(a_document, a_typeIds, 2U, a_assertContext));
    require(ready.main_camera_status() == cue::renderer::MainCameraStatus::Ready);
    require(ready.try_main_camera() != nullptr && ready.meshes().size() == 1U && ready.generation() == 2U);
    require(cue::renderer::is_valid(*ready.try_main_camera()));

    cue::scene::SceneSnapshot runtimeSnapshot =
        take_value(cue::scene::create_scene_snapshot(a_document, a_assertContext));

    const cue::scene::ObjectId secondCameraId = make_object_id("71000000-0000-4000-8000-000000000007", a_assertContext);
    require(
        a_document.add_object(secondCameraId, "Second Camera", true, std::nullopt, cue::math::Transform{}).has_value());
    require(a_document
                .add_component(secondCameraId,
                               take_value(cue::renderer::make_camera_component(
                                   make_component_id("71000000-0000-4000-8000-000000000008", a_assertContext), true,
                                   a_schemaRegistry, a_valueSchemaRegistry, a_assertContext)))
                .has_value());
    cue::renderer::RenderSnapshot multiple =
        take_value(cue::renderer::extract_render_snapshot(a_document, a_typeIds, 3U, a_assertContext));
    require(multiple.main_camera_status() == cue::renderer::MainCameraStatus::Multiple);
    require(multiple.try_main_camera() == nullptr && multiple.meshes().size() == 1U);
    return runtimeSnapshot;
}

/// @brief Debug Cameraの回転Clamp、Local移動、失敗時Rollbackを検証する
void test_debug_camera_motion(const cue::AssertContext &a_assertContext) noexcept
{
    cue::renderer::DebugCamera camera =
        take_value(cue::renderer::DebugCamera::create_default(a_assertContext.fatal_handler()));
    const cue::math::Vector3 initialTranslation = camera.camera().transform.translation();
    const cue::math::Quaternion initialRotation = camera.camera().transform.rotation();
    require(camera.apply_motion(a_assertContext.fatal_handler(), {0.25F, -0.1F, 0.0F, 0.0F, 0.0F}).has_value());
    require(camera.camera().transform.translation() == initialTranslation);
    require(camera.camera().transform.rotation() != initialRotation);

    require(camera.apply_motion(a_assertContext.fatal_handler(), {0.0F, 1000.0F, -0.5F, 0.25F, 1.0F}).has_value());
    const cue::math::Tolerance tolerance =
        take_value(cue::math::Tolerance::create(a_assertContext.fatal_handler(), 0.0001F, 0.0001F));
    const cue::math::Vector3 forward = take_value(cue::math::rotate(a_assertContext.fatal_handler(), {0.0F, 0.0F, 1.0F},
                                                                    camera.camera().transform.rotation(), tolerance));
    require(cue::math::is_finite(camera.camera().transform.translation()) && forward.y < -0.99F);

    const cue::math::Vector3 validTranslation = camera.camera().transform.translation();
    const cue::math::Quaternion validRotation = camera.camera().transform.rotation();
    cue::Result<void> invalid = camera.apply_motion(a_assertContext.fatal_handler(),
                                                    {std::numeric_limits<float>::infinity(), 0.0F, 0.0F, 0.0F, 0.0F});
    require(!invalid);
    require(!camera
                 .apply_motion(a_assertContext.fatal_handler(),
                               {0.0F, std::numeric_limits<float>::infinity(), 0.0F, 0.0F, 0.0F})
                 .has_value());
    require(camera.camera().transform.translation() == validTranslation);
    require(camera.camera().transform.rotation() == validRotation);
}

/// @brief Scene Component BuilderとGameCore Systemを通したRuntime Snapshot抽出を検証する
void test_runtime_extraction(const cue::scene::SceneSnapshot &a_snapshot,
                             const cue::schema::SchemaRegistry &a_schemaRegistry,
                             const cue::AssertContext &a_assertContext) noexcept
{
    cue::renderer::RenderSnapshotStore store;
    cue::renderer::RendererRuntimeSystemFactory factory(store);
    cue::runtime::RuntimeSystemRegistration registration = take_value(factory.create_system(a_assertContext));
    TestClock clock;
    std::unique_ptr<cue::runtime::RuntimeApplicationSession> session =
        take_value(cue::runtime::RuntimeApplicationSession::create(1U, clock, 100'000'000, a_assertContext));
    require(session
                ->register_system(std::move(registration.descriptor), std::move(registration.system),
                                  std::move(registration.componentBuilderFactories))
                .has_value());

    cue::game_core::WorldIdentitySource worldIdentitySource;
    const cue::runtime::RuntimeSchemaTypeIds runtimeIds =
        take_value(cue::runtime::make_runtime_schema_type_ids(a_assertContext));
    require(session
                ->start(a_snapshot, worldIdentitySource, a_schemaRegistry, runtimeIds.transform,
                        runtimeIds.sceneObjectState)
                .has_value());
    require(store.snapshot().main_camera_status() == cue::renderer::MainCameraStatus::Ready);
    require(store.snapshot().try_main_camera() != nullptr && store.snapshot().meshes().size() == 1U);
    require(session->advance_frame({}).has_value());
    require(store.snapshot().generation() == 2U);
    require(session->request_stop(cue::runtime::RuntimeApplicationStopReason::Requested).has_value());
    require(session->stop().has_value());
    require(store.snapshot().main_camera_status() == cue::renderer::MainCameraStatus::Missing);
}
} // namespace

int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource registryIdentitySource;
    std::unique_ptr<cue::schema::SchemaRegistry> schemaRegistry =
        make_schema_registry(registryIdentitySource, assertContext);
    cue::scene::ComponentValueSchemaRegistry valueSchemaRegistry =
        make_value_schema_registry(*schemaRegistry, assertContext);
    const cue::renderer::RendererSchemaTypeIds typeIds =
        take_value(cue::renderer::make_renderer_schema_type_ids(assertContext));
    cue::scene::SceneDocument document = make_render_scene(*schemaRegistry, valueSchemaRegistry, assertContext);
    cue::scene::SceneSnapshot runtimeSnapshot =
        test_authoring_extraction(document, typeIds, *schemaRegistry, valueSchemaRegistry, assertContext);
    test_debug_camera_motion(assertContext);
    test_runtime_extraction(runtimeSnapshot, *schemaRegistry, assertContext);
    return 0;
}
