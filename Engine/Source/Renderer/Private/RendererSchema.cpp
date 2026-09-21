#include <Cue/Renderer/RendererSchema.h>

#include <Cue/EngineAssets/BuiltInAssetCatalog.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Math/Scalar.h>
#include <Cue/Renderer/Camera.h>
#include <Cue/Renderer/Error.h>
#include <Cue/Schema/Descriptor.h>
#include <Cue/Schema/Registry.h>

#include <cmath>
#include <exception>
#include <initializer_list>
#include <new>
#include <string_view>
#include <utility>

namespace
{
constexpr std::string_view k_cameraTypeId = "70000000-0000-4000-8000-000000000001";
constexpr std::string_view k_meshTypeId = "70000000-0000-4000-8000-000000000002";

/// @brief Renderer Schema Version 1を生成する
[[nodiscard]] cue::Result<cue::schema::SchemaVersion> make_version(const cue::AssertContext &a_assertContext) noexcept
{
    return cue::schema::SchemaVersion::create(1U, a_assertContext);
}

/// @brief Stable Field IDと診断名からField Descriptorを追加する
[[nodiscard]] cue::Result<void> add_field(std::vector<cue::schema::FieldDescriptor> &a_fields, std::uint32_t a_id,
                                          std::string_view a_name, const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::schema::FieldId> fieldId = cue::schema::FieldId::create(a_id, a_assertContext);
    if (!fieldId)
    {
        return cue::Result<void>::failure(std::move(*fieldId.try_error()));
    }
    cue::Result<cue::schema::FieldDescriptor> field =
        cue::schema::create_field_descriptor(std::move(*fieldId.try_value()), a_name, a_assertContext);
    if (!field)
    {
        return cue::Result<void>::failure(std::move(*field.try_error()));
    }
    a_fields.push_back(std::move(*field.try_value()));
    return cue::Result<void>::success();
}

/// @brief Camera Type Descriptorを生成する
[[nodiscard]] cue::Result<cue::schema::TypeDescriptor> make_camera_descriptor(
    cue::schema::TypeId a_typeId, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::vector<cue::schema::FieldDescriptor> fields;
        fields.reserve(4U);
        for (const auto &[id, name] : std::initializer_list<std::pair<std::uint32_t, std::string_view>>{
                 {1U, "isMain"}, {2U, "verticalFovDegrees"}, {3U, "nearPlane"}, {4U, "farPlane"}})
        {
            cue::Result<void> added = add_field(fields, id, name, a_assertContext);
            if (!added)
            {
                return cue::Result<cue::schema::TypeDescriptor>::failure(std::move(*added.try_error()));
            }
        }
        cue::Result<cue::schema::SchemaVersion> version = make_version(a_assertContext);
        if (!version)
        {
            return cue::Result<cue::schema::TypeDescriptor>::failure(std::move(*version.try_error()));
        }
        std::vector<cue::schema::FieldId> reserved;
        return cue::schema::create_type_descriptor(std::move(a_typeId), "Cue.Renderer.Camera",
                                                   std::move(*version.try_value()), std::move(fields),
                                                   std::move(reserved), a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.Renderer camera schema allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.Renderer camera schema creation caught an unexpected exception");
    }
    std::terminate();
}

/// @brief Mesh Type Descriptorを生成する
[[nodiscard]] cue::Result<cue::schema::TypeDescriptor> make_mesh_descriptor(
    cue::schema::TypeId a_typeId, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::vector<cue::schema::FieldDescriptor> fields;
        cue::Result<void> added = add_field(fields, 1U, "asset", a_assertContext);
        if (!added)
        {
            return cue::Result<cue::schema::TypeDescriptor>::failure(std::move(*added.try_error()));
        }
        cue::Result<cue::schema::SchemaVersion> version = make_version(a_assertContext);
        if (!version)
        {
            return cue::Result<cue::schema::TypeDescriptor>::failure(std::move(*version.try_error()));
        }
        std::vector<cue::schema::FieldId> reserved;
        return cue::schema::create_type_descriptor(std::move(a_typeId), "Cue.Renderer.Mesh",
                                                   std::move(*version.try_value()), std::move(fields),
                                                   std::move(reserved), a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.Renderer mesh schema allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.Renderer mesh schema creation caught an unexpected exception");
    }
    std::terminate();
}
} // namespace

namespace cue::renderer
{
Result<RendererSchemaTypeIds> make_renderer_schema_type_ids(const AssertContext &a_assertContext) noexcept
{
    Result<schema::TypeId> camera = schema::TypeId::parse(k_cameraTypeId, a_assertContext);
    if (!camera)
    {
        return Result<RendererSchemaTypeIds>::failure(std::move(*camera.try_error()));
    }
    Result<schema::TypeId> mesh = schema::TypeId::parse(k_meshTypeId, a_assertContext);
    if (!mesh)
    {
        return Result<RendererSchemaTypeIds>::failure(std::move(*mesh.try_error()));
    }
    return Result<RendererSchemaTypeIds>::success({std::move(*camera.try_value()), std::move(*mesh.try_value())});
}

Result<CameraFieldIds> make_camera_field_ids(const AssertContext &a_assertContext) noexcept
{
    Result<schema::FieldId> isMain = schema::FieldId::create(1U, a_assertContext);
    Result<schema::FieldId> fov = schema::FieldId::create(2U, a_assertContext);
    Result<schema::FieldId> nearPlane = schema::FieldId::create(3U, a_assertContext);
    Result<schema::FieldId> farPlane = schema::FieldId::create(4U, a_assertContext);
    if (!isMain)
    {
        return Result<CameraFieldIds>::failure(std::move(*isMain.try_error()));
    }
    if (!fov)
    {
        return Result<CameraFieldIds>::failure(std::move(*fov.try_error()));
    }
    if (!nearPlane)
    {
        return Result<CameraFieldIds>::failure(std::move(*nearPlane.try_error()));
    }
    if (!farPlane)
    {
        return Result<CameraFieldIds>::failure(std::move(*farPlane.try_error()));
    }
    return Result<CameraFieldIds>::success({std::move(*isMain.try_value()), std::move(*fov.try_value()),
                                            std::move(*nearPlane.try_value()), std::move(*farPlane.try_value())});
}

Result<MeshFieldIds> make_mesh_field_ids(const AssertContext &a_assertContext) noexcept
{
    Result<schema::FieldId> asset = schema::FieldId::create(1U, a_assertContext);
    if (!asset)
    {
        return Result<MeshFieldIds>::failure(std::move(*asset.try_error()));
    }
    return Result<MeshFieldIds>::success({std::move(*asset.try_value())});
}

Result<void> add_renderer_schema_types(schema::SchemaRegistryBuilder &a_builder,
                                       const AssertContext &a_assertContext) noexcept
{
    Result<RendererSchemaTypeIds> typeIds = make_renderer_schema_type_ids(a_assertContext);
    if (!typeIds)
    {
        return Result<void>::failure(std::move(*typeIds.try_error()));
    }
    Result<schema::TypeDescriptor> camera = make_camera_descriptor(typeIds.try_value()->camera, a_assertContext);
    if (!camera)
    {
        return Result<void>::failure(std::move(*camera.try_error()));
    }
    Result<void> addedCamera = a_builder.add_type(std::move(*camera.try_value()));
    if (!addedCamera)
    {
        return addedCamera;
    }
    Result<schema::TypeDescriptor> mesh = make_mesh_descriptor(typeIds.try_value()->mesh, a_assertContext);
    if (!mesh)
    {
        return Result<void>::failure(std::move(*mesh.try_error()));
    }
    return a_builder.add_type(std::move(*mesh.try_value()));
}

Result<std::vector<scene::ComponentValueSchema>> make_renderer_value_schemas(
    const schema::SchemaRegistry &a_schemaRegistry, const AssertContext &a_assertContext) noexcept
{
    Result<RendererSchemaTypeIds> typeIds = make_renderer_schema_type_ids(a_assertContext);
    Result<CameraFieldIds> cameraFields = make_camera_field_ids(a_assertContext);
    Result<MeshFieldIds> meshFields = make_mesh_field_ids(a_assertContext);
    Result<schema::SchemaVersion> version = make_version(a_assertContext);
    if (!typeIds)
    {
        return Result<std::vector<scene::ComponentValueSchema>>::failure(std::move(*typeIds.try_error()));
    }
    if (!cameraFields)
    {
        return Result<std::vector<scene::ComponentValueSchema>>::failure(std::move(*cameraFields.try_error()));
    }
    if (!meshFields)
    {
        return Result<std::vector<scene::ComponentValueSchema>>::failure(std::move(*meshFields.try_error()));
    }
    if (!version)
    {
        return Result<std::vector<scene::ComponentValueSchema>>::failure(std::move(*version.try_error()));
    }

    try
    {
        std::vector<scene::FieldKindBinding> cameraBindings{
            {cameraFields.try_value()->isMain, scene::FieldValueKind::Boolean},
            {cameraFields.try_value()->verticalFovDegrees, scene::FieldValueKind::FloatingPoint},
            {cameraFields.try_value()->nearPlane, scene::FieldValueKind::FloatingPoint},
            {cameraFields.try_value()->farPlane, scene::FieldValueKind::FloatingPoint}};
        Result<scene::ComponentValueSchema> camera =
            scene::create_component_value_schema(typeIds.try_value()->camera, *version.try_value(),
                                                 std::move(cameraBindings), a_schemaRegistry, a_assertContext);
        if (!camera)
        {
            return Result<std::vector<scene::ComponentValueSchema>>::failure(std::move(*camera.try_error()));
        }

        std::vector<scene::FieldKindBinding> meshBindings{
            {meshFields.try_value()->asset, scene::FieldValueKind::AssetReference}};
        Result<scene::ComponentValueSchema> mesh =
            scene::create_component_value_schema(typeIds.try_value()->mesh, *version.try_value(),
                                                 std::move(meshBindings), a_schemaRegistry, a_assertContext);
        if (!mesh)
        {
            return Result<std::vector<scene::ComponentValueSchema>>::failure(std::move(*mesh.try_error()));
        }

        std::vector<scene::ComponentValueSchema> schemas;
        schemas.reserve(2U);
        schemas.push_back(std::move(*camera.try_value()));
        schemas.push_back(std::move(*mesh.try_value()));
        return Result<std::vector<scene::ComponentValueSchema>>::success(std::move(schemas));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.Renderer value schema allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.Renderer value schema creation caught an unexpected exception");
    }
    std::terminate();
}

Result<void> validate_runtime_scene_component(const scene::SceneComponent &a_component,
                                              const AssertContext &a_assertContext) noexcept
{
    const scene::KnownComponentData *known = a_component.try_known();
    if (!a_component.is_valid() || known == nullptr || known->schema_version().value() != 1U ||
        !known->unknown_fields().empty())
    {
        return Result<void>::failure(make_renderer_error(a_assertContext, RendererError::InvalidRuntimeBinding,
                                                         "Runtime Scene requires a complete known v1 component"));
    }
    auto ids = make_renderer_schema_type_ids(a_assertContext);
    if (!ids)
    {
        return Result<void>::failure(std::move(*ids.try_error()));
    }
    const auto fields = known->known_fields();
    if (known->type_id() == ids.try_value()->camera)
    {
        if (fields.size() != 4U || fields[0].id().value() != 1U || fields[1].id().value() != 2U ||
            fields[2].id().value() != 3U || fields[3].id().value() != 4U ||
            fields[0].value().try_boolean() == nullptr || fields[1].value().try_floating_point() == nullptr ||
            fields[2].value().try_floating_point() == nullptr || fields[3].value().try_floating_point() == nullptr)
        {
            return Result<void>::failure(make_renderer_error(a_assertContext, RendererError::InvalidCamera,
                                                             "Runtime Scene camera fields are incomplete"));
        }
        const double fov = *fields[1].value().try_floating_point();
        const double nearPlane = *fields[2].value().try_floating_point();
        const double farPlane = *fields[3].value().try_floating_point();
        if (!std::isfinite(fov) || !std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
            !(fov > 0.0 && fov < 180.0 && nearPlane > 0.0 && nearPlane < farPlane) ||
            !is_valid(PerspectiveCamera{math::Transform{},
                                        math::to_radians(math::Degrees(static_cast<float>(fov))).value,
                                        static_cast<float>(nearPlane), static_cast<float>(farPlane)}))
        {
            return Result<void>::failure(make_renderer_error(a_assertContext, RendererError::InvalidCamera,
                                                             "Runtime Scene camera projection is invalid"));
        }
        return Result<void>::success();
    }
    if (known->type_id() == ids.try_value()->mesh && fields.size() == 1U && fields[0].id().value() == 1U)
    {
        const scene::AssetReferenceValue *asset = fields[0].value().try_asset_reference();
        if (asset != nullptr && asset->token() == engine_assets::k_cubeMeshAssetId)
        {
            return Result<void>::success();
        }
    }
    return Result<void>::failure(make_renderer_error(a_assertContext, RendererError::UnsupportedMesh,
                                                     "Runtime Scene supports only the built-in Cube Mesh"));
}

Result<scene::SceneComponent> make_camera_component(scene::ComponentInstanceId a_instanceId, bool a_isMain,
                                                    const schema::SchemaRegistry &a_schemaRegistry,
                                                    const scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry,
                                                    const AssertContext &a_assertContext) noexcept
{
    Result<RendererSchemaTypeIds> typeIds = make_renderer_schema_type_ids(a_assertContext);
    Result<CameraFieldIds> fieldIds = make_camera_field_ids(a_assertContext);
    Result<schema::SchemaVersion> version = make_version(a_assertContext);
    if (!typeIds)
    {
        return Result<scene::SceneComponent>::failure(std::move(*typeIds.try_error()));
    }
    if (!fieldIds)
    {
        return Result<scene::SceneComponent>::failure(std::move(*fieldIds.try_error()));
    }
    if (!version)
    {
        return Result<scene::SceneComponent>::failure(std::move(*version.try_error()));
    }

    try
    {
        std::vector<scene::KnownFieldData> fields;
        fields.reserve(4U);
        /// @brief 一つのCamera Fieldを既知Field集合へ追加する
        const auto add = [&](schema::FieldId a_id, scene::FieldValue a_value,
                             scene::FieldValueKind a_kind) -> Result<void>
        {
            Result<scene::KnownFieldData> field =
                scene::create_known_field(std::move(a_id), std::move(a_value), a_kind, a_assertContext);
            if (!field)
            {
                return Result<void>::failure(std::move(*field.try_error()));
            }
            fields.push_back(std::move(*field.try_value()));
            return Result<void>::success();
        };

        Result<scene::FieldValue> fov = scene::FieldValue::floating_point(60.0, a_assertContext);
        Result<scene::FieldValue> nearPlane = scene::FieldValue::floating_point(0.1, a_assertContext);
        Result<scene::FieldValue> farPlane = scene::FieldValue::floating_point(1000.0, a_assertContext);
        if (!fov)
        {
            return Result<scene::SceneComponent>::failure(std::move(*fov.try_error()));
        }
        if (!nearPlane)
        {
            return Result<scene::SceneComponent>::failure(std::move(*nearPlane.try_error()));
        }
        if (!farPlane)
        {
            return Result<scene::SceneComponent>::failure(std::move(*farPlane.try_error()));
        }

        Result<void> added =
            add(fieldIds.try_value()->isMain, scene::FieldValue::boolean(a_isMain), scene::FieldValueKind::Boolean);
        if (added)
        {
            added = add(fieldIds.try_value()->verticalFovDegrees, std::move(*fov.try_value()),
                        scene::FieldValueKind::FloatingPoint);
        }
        if (added)
        {
            added = add(fieldIds.try_value()->nearPlane, std::move(*nearPlane.try_value()),
                        scene::FieldValueKind::FloatingPoint);
        }
        if (added)
        {
            added = add(fieldIds.try_value()->farPlane, std::move(*farPlane.try_value()),
                        scene::FieldValueKind::FloatingPoint);
        }
        if (!added)
        {
            return Result<scene::SceneComponent>::failure(std::move(*added.try_error()));
        }

        std::vector<scene::OpaqueFieldData> unknownFields;
        Result<scene::KnownComponentData> component = scene::create_known_component(
            std::move(a_instanceId), typeIds.try_value()->camera, *version.try_value(), std::move(fields),
            std::move(unknownFields), a_schemaRegistry, a_valueSchemaRegistry, a_assertContext);
        if (!component)
        {
            return Result<scene::SceneComponent>::failure(std::move(*component.try_error()));
        }
        return Result<scene::SceneComponent>::success(scene::SceneComponent::known(std::move(*component.try_value())));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.Renderer camera component allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate(
            "Cue.Renderer camera component creation caught an unexpected exception");
    }
    std::terminate();
}

Result<scene::SceneComponent> make_cube_mesh_component(scene::ComponentInstanceId a_instanceId,
                                                       const schema::SchemaRegistry &a_schemaRegistry,
                                                       const scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry,
                                                       const AssertContext &a_assertContext) noexcept
{
    return make_builtin_mesh_component(std::move(a_instanceId), engine_assets::k_cubeMeshAssetId, a_schemaRegistry,
                                       a_valueSchemaRegistry, a_assertContext);
}

Result<scene::SceneComponent> make_builtin_mesh_component(
    scene::ComponentInstanceId a_instanceId, std::string_view a_assetId, const schema::SchemaRegistry &a_schemaRegistry,
    const scene::ComponentValueSchemaRegistry &a_valueSchemaRegistry, const AssertContext &a_assertContext) noexcept
{
    Result<const engine_assets::BuiltInMeshDescriptor *> descriptor =
        engine_assets::resolve_builtin_mesh_descriptor(a_assetId, a_assertContext);
    if (!descriptor)
    {
        return Result<scene::SceneComponent>::failure(std::move(*descriptor.try_error()));
    }
    Result<RendererSchemaTypeIds> typeIds = make_renderer_schema_type_ids(a_assertContext);
    Result<MeshFieldIds> fieldIds = make_mesh_field_ids(a_assertContext);
    Result<schema::SchemaVersion> version = make_version(a_assertContext);
    if (!typeIds)
    {
        return Result<scene::SceneComponent>::failure(std::move(*typeIds.try_error()));
    }
    if (!fieldIds)
    {
        return Result<scene::SceneComponent>::failure(std::move(*fieldIds.try_error()));
    }
    if (!version)
    {
        return Result<scene::SceneComponent>::failure(std::move(*version.try_error()));
    }

    Result<scene::AssetReferenceValue> asset = scene::AssetReferenceValue::create(a_assetId, a_assertContext);
    if (!asset)
    {
        return Result<scene::SceneComponent>::failure(std::move(*asset.try_error()));
    }
    Result<scene::KnownFieldData> assetField = scene::create_known_field(
        fieldIds.try_value()->asset, scene::FieldValue::asset_reference(std::move(*asset.try_value())),
        scene::FieldValueKind::AssetReference, a_assertContext);
    if (!assetField)
    {
        return Result<scene::SceneComponent>::failure(std::move(*assetField.try_error()));
    }

    try
    {
        std::vector<scene::KnownFieldData> fields;
        fields.push_back(std::move(*assetField.try_value()));
        std::vector<scene::OpaqueFieldData> unknownFields;
        Result<scene::KnownComponentData> component = scene::create_known_component(
            std::move(a_instanceId), typeIds.try_value()->mesh, *version.try_value(), std::move(fields),
            std::move(unknownFields), a_schemaRegistry, a_valueSchemaRegistry, a_assertContext);
        if (!component)
        {
            return Result<scene::SceneComponent>::failure(std::move(*component.try_error()));
        }
        return Result<scene::SceneComponent>::success(scene::SceneComponent::known(std::move(*component.try_value())));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Cue.Renderer mesh component allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate(
            "Cue.Renderer mesh component creation caught an unexpected exception");
    }
    std::terminate();
}

bool is_render_mesh_supported(std::string_view a_assetId) noexcept
{
    return a_assetId == engine_assets::k_cubeMeshAssetId;
}
} // namespace cue::renderer
