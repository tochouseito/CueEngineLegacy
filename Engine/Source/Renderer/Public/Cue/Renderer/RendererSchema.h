#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Scene/ComponentData.h>
#include <Cue/Schema/Types.h>

#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::schema
{
class SchemaRegistry;
class SchemaRegistryBuilder;
} // namespace cue::schema

namespace cue::renderer
{
/// @brief Rendererが永続化とRuntime Bindingに使用するStable Type Identity集合
struct RendererSchemaTypeIds final
{
    schema::TypeId camera;
    schema::TypeId mesh;
};

/// @brief Camera ComponentのStable Field Identity集合
struct CameraFieldIds final
{
    schema::FieldId isMain;
    schema::FieldId verticalFovDegrees;
    schema::FieldId nearPlane;
    schema::FieldId farPlane;
};

/// @brief Mesh ComponentのStable Field Identity集合
struct MeshFieldIds final
{
    schema::FieldId asset;
};

/// @brief Renderer Stable Type Identityを生成する
[[nodiscard]] Result<RendererSchemaTypeIds> make_renderer_schema_type_ids(
    const AssertContext &a_assertContext) noexcept;
/// @brief Camera Stable Field Identityを生成する
[[nodiscard]] Result<CameraFieldIds> make_camera_field_ids(const AssertContext &a_assertContext) noexcept;
/// @brief Mesh Stable Field Identityを生成する
[[nodiscard]] Result<MeshFieldIds> make_mesh_field_ids(const AssertContext &a_assertContext) noexcept;
/// @brief Camera／Mesh Type DescriptorをSchema Registry Builderへ登録する
[[nodiscard]] Result<void> add_renderer_schema_types(schema::SchemaRegistryBuilder &a_builder,
                                                     const AssertContext &a_assertContext) noexcept;
/// @brief Renderer ComponentのField Kind Schemaを生成する
[[nodiscard]] Result<std::vector<scene::ComponentValueSchema>> make_renderer_value_schemas(
    const schema::SchemaRegistry &a_schemaRegistry, const AssertContext &a_assertContext) noexcept;
/// @brief Package用Camera／Built-in Cubeの完全なv1 Field集合と投影値を検証する
[[nodiscard]] Result<void> validate_runtime_scene_component(const scene::SceneComponent &a_component,
                                                            const AssertContext &a_assertContext) noexcept;
/// @brief 既定Perspective値を持つCamera Authoring Componentを生成する
[[nodiscard]] Result<scene::SceneComponent> make_camera_component(
    scene::ComponentInstanceId a_instanceId, bool a_isMain, const schema::SchemaRegistry &a_schemaRegistry,
    const scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry, const AssertContext &a_assertContext) noexcept;
/// @brief Built-in Cube Assetを参照するMesh Authoring Componentを生成する
[[nodiscard]] Result<scene::SceneComponent> make_cube_mesh_component(
    scene::ComponentInstanceId a_instanceId, const schema::SchemaRegistry &a_schemaRegistry,
    const scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry, const AssertContext &a_assertContext) noexcept;
/// @brief Catalog検証済みBuilt-in Mesh Assetを明示参照するAuthoring Componentを生成する
[[nodiscard]] Result<scene::SceneComponent> make_builtin_mesh_component(
    scene::ComponentInstanceId a_instanceId, std::string_view a_assetId, const schema::SchemaRegistry &a_schemaRegistry,
    const scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry, const AssertContext &a_assertContext) noexcept;
/// @brief 現在のGameView／DebugView Render PathがBuilt-in Mesh IDを描画できる場合にtrueを返す
[[nodiscard]] bool is_render_mesh_supported(std::string_view a_assetId) noexcept;
} // namespace cue::renderer
