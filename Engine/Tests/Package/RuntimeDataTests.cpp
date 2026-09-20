#include <Cue/Package/RuntimeData.h>

#include "Sha256.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Math/Scalar.h>
#include <Cue/Math/Transform.h>
#include <Cue/Package/Error.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Renderer/RendererSchema.h>
#include <Cue/Runtime/RuntimeSchema.h>
#include <Cue/Scene/ComponentData.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Schema/Registry.h>
#include <Cue/Schema/Types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_projectId = "00000000-0000-4000-8000-000000000901";
constexpr std::string_view k_sceneId = "10000000-0000-4000-8000-000000000001";
constexpr std::string_view k_otherSceneId = "10000000-0000-4000-8000-000000000002";

/// @brief Test内のFatalを固定Exit Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(76);
    }

    /// @brief Message付きFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

/// @brief Test用Project Descriptor v2を生成する
[[nodiscard]] cue::Result<cue::ProjectDescriptor> make_descriptor(std::string_view a_sceneId,
                                                                  const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::ProjectId> projectId = cue::ProjectId::parse(k_projectId, a_assertContext);
    if (!projectId)
    {
        return cue::Result<cue::ProjectDescriptor>::failure(std::move(*projectId.try_error()));
    }
    return cue::create_blank_project_descriptor(
        *projectId.try_value(), "Package Test",
        cue::EngineCompatibility{cue::EngineVersion{1U, 2U, 3U}, cue::EngineVersion{2U, 0U, 0U}}, a_sceneId,
        a_assertContext);
}

/// @brief Test用Scene Identityから空Documentを生成する
[[nodiscard]] cue::Result<cue::scene::SceneDocument> make_scene(std::string_view a_sceneId,
                                                                const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::scene::SceneAssetId> sceneId = cue::scene::SceneAssetId::parse(a_sceneId, a_assertContext);
    if (!sceneId)
    {
        return cue::Result<cue::scene::SceneDocument>::failure(std::move(*sceneId.try_error()));
    }
    return cue::Result<cue::scene::SceneDocument>::success(
        cue::scene::SceneDocument::create(*sceneId.try_value(), a_assertContext));
}

/// @brief Package Error Codeが期待値と一致するか返す
template <typename Value>
[[nodiscard]] bool has_package_error(const cue::Result<Value> &a_result, cue::package::PackageError a_error) noexcept
{
    return !a_result && a_result.try_error()->root_code().domain() == "Cue.Package" &&
           a_result.try_error()->root_code().value() == static_cast<std::int64_t>(a_error);
}

/// @brief SHA-256 DigestをTest比較用lowercase hexadecimalへ変換する
[[nodiscard]] std::string digest_text(const cue::package_private::Sha256Digest &a_digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(a_digest.size() * 2U);
    for (const std::uint8_t byte : a_digest)
    {
        output.push_back(digits[(byte >> 4U) & 0x0fU]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

/// @brief SHA-256実装が標準VectorとPadding境界で一致するか検証する
[[nodiscard]] bool test_sha256_vector()
{
    constexpr std::array bytes = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    const cue::package_private::Sha256Digest digest = cue::package_private::compute_sha256(bytes);
    constexpr std::array<std::uint8_t, 32U> expected = {
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU, 0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
        0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU, 0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU};
    const std::string empty;
    const std::string length55(55U, 'a');
    const std::string length56(56U, 'a');
    const std::string length64(64U, 'a');
    const std::string length65(65U, 'a');
    /// @brief Test文字列のSHA-256をlowercase hexadecimalで返す
    const auto hash = [](std::string_view a_value)
    {
        return digest_text(
            cue::package_private::compute_sha256(std::as_bytes(std::span(a_value.data(), a_value.size()))));
    };
    return digest == expected && hash(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" &&
           hash(length55) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" &&
           hash(length56) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" &&
           hash(length64) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" &&
           hash(length65) == "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0";
}

/// @brief 同じSnapshotが同一Byte列、Path、Size、Hashを生成するか検証する
[[nodiscard]] bool test_deterministic_empty_scene(const cue::AssertContext &a_assertContext)
{
    auto descriptor = make_descriptor(k_sceneId, a_assertContext);
    auto scene = make_scene(k_sceneId, a_assertContext);
    if (!descriptor || !scene)
    {
        return false;
    }
    auto firstSnapshot = cue::scene::create_scene_snapshot(*scene.try_value(), a_assertContext);
    auto secondSnapshot = cue::scene::create_scene_snapshot(*scene.try_value(), a_assertContext);
    if (!firstSnapshot || !secondSnapshot)
    {
        return false;
    }
    auto first = cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *firstSnapshot.try_value(),
                                                            a_assertContext);
    auto second = cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *secondSnapshot.try_value(),
                                                             a_assertContext);
    constexpr std::string_view expectedProject =
        "{\"schemaVersion\":1,\"projectId\":\"00000000-0000-4000-8000-000000000901\","
        "\"engineCompatibility\":{\"minimum\":\"1.2.3\",\"maximumExclusive\":\"2.0.0\"},"
        "\"requiredCapabilities\":[],"
        "\"startupSceneAssetId\":\"10000000-0000-4000-8000-000000000001\"}\n";
    constexpr std::string_view expectedScene =
        "{\"schemaVersion\":1,\"sceneAssetId\":\"10000000-0000-4000-8000-000000000001\","
        "\"objects\":[]}\n";
    return first && second && first.try_value()->project_id() == k_projectId &&
           first.try_value()->startup_scene_asset_id() == k_sceneId &&
           first.try_value()->project_data().relative_path() == "Data/CueProject.runtime.json" &&
           first.try_value()->startup_scene_data().relative_path() ==
               "Data/Scenes/10000000-0000-4000-8000-000000000001.cueruntime.json" &&
           first.try_value()->project_data().bytes() == expectedProject &&
           first.try_value()->startup_scene_data().bytes() == expectedScene &&
           first.try_value()->project_data().byte_size() == expectedProject.size() &&
           first.try_value()->startup_scene_data().byte_size() == expectedScene.size() &&
           first.try_value()->project_data().sha256().size() == 64U &&
           first.try_value()->startup_scene_data().sha256().size() == 64U &&
           first.try_value()->project_data().bytes() == second.try_value()->project_data().bytes() &&
           first.try_value()->project_data().sha256() == second.try_value()->project_data().sha256() &&
           first.try_value()->startup_scene_data().bytes() == second.try_value()->startup_scene_data().bytes() &&
           first.try_value()->startup_scene_data().sha256() == second.try_value()->startup_scene_data().sha256();
}

/// @brief Runtime SceneがStable Object Dataだけを保持しAuthoring名を含めないか検証する
[[nodiscard]] bool test_runtime_object_projection(const cue::AssertContext &a_assertContext)
{
    auto descriptor = make_descriptor(k_sceneId, a_assertContext);
    auto scene = make_scene(k_sceneId, a_assertContext);
    auto parentId = cue::scene::ObjectId::parse("20000000-0000-4000-8000-000000000001", a_assertContext);
    auto childId = cue::scene::ObjectId::parse("20000000-0000-4000-8000-000000000002", a_assertContext);
    if (!descriptor || !scene || !parentId || !childId ||
        !scene.try_value()->add_object(*parentId.try_value(), "AuthoringOnlyParentName", true, std::nullopt,
                                       cue::math::Transform{}) ||
        !scene.try_value()->add_object(*childId.try_value(), "AuthoringOnlyChildName", false, *parentId.try_value(),
                                       cue::math::Transform{}))
    {
        return false;
    }
    auto snapshot = cue::scene::create_scene_snapshot(*scene.try_value(), a_assertContext);
    auto published =
        snapshot ? cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *snapshot.try_value(),
                                                              a_assertContext)
                 : cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(std::move(*snapshot.try_error()));
    return published &&
           published.try_value()->startup_scene_data().bytes().find("AuthoringOnly") == std::string_view::npos &&
           published.try_value()->startup_scene_data().bytes().find(
               "\"parentObjectId\":\"20000000-0000-4000-8000-000000000001\"") != std::string_view::npos &&
           published.try_value()->startup_scene_data().bytes().find("\"active\":false") != std::string_view::npos &&
           published.try_value()->startup_scene_data().bytes().find("\"extensions\"") == std::string_view::npos;
}

/// @brief Runtime Transformのbinary32値が最短Round-trip表現で直列化されるか検証する
[[nodiscard]] bool test_shortest_float_serialization(const cue::AssertContext &a_assertContext)
{
    auto descriptor = make_descriptor(k_sceneId, a_assertContext);
    auto scene = make_scene(k_sceneId, a_assertContext);
    auto objectId = cue::scene::ObjectId::parse("20000000-0000-4000-8000-000000000001", a_assertContext);
    auto tolerance = cue::math::Tolerance::create(a_assertContext.fatal_handler(), 0.00001F, 0.00001F);
    if (!descriptor || !scene || !objectId || !tolerance)
    {
        return false;
    }
    auto transform = cue::math::Transform::create(a_assertContext.fatal_handler(),
                                                  cue::math::Vector3{0.1F, -0.2F, 1.25F}, cue::math::Quaternion{},
                                                  cue::math::Vector3{1.0F, 1.0F, 1.0F}, *tolerance.try_value());
    if (!transform ||
        !scene.try_value()->add_object(*objectId.try_value(), "Object", true, std::nullopt, *transform.try_value()))
    {
        return false;
    }
    auto snapshot = cue::scene::create_scene_snapshot(*scene.try_value(), a_assertContext);
    auto published =
        snapshot ? cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *snapshot.try_value(),
                                                              a_assertContext)
                 : cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(std::move(*snapshot.try_error()));
    return published && published.try_value()->startup_scene_data().bytes().find("\"translation\":[0.1,-0.2,1.25]") !=
                            std::string_view::npos;
}

/// @brief CameraとBuilt-in Cubeを含むSceneだけが完全なv2を生成し不正投影を拒否するか検証する
[[nodiscard]] bool test_renderer_scene_v2(const cue::AssertContext &a_assertContext)
{
    auto descriptor = make_descriptor(k_sceneId, a_assertContext);
    auto scene = make_scene(k_sceneId, a_assertContext);
    auto objectId = cue::scene::ObjectId::parse("20000000-0000-4000-8000-000000000001", a_assertContext);
    auto cameraId = cue::scene::ComponentInstanceId::parse("30000000-0000-4000-8000-000000000001", a_assertContext);
    auto meshId = cue::scene::ComponentInstanceId::parse("30000000-0000-4000-8000-000000000002", a_assertContext);
    if (!descriptor || !scene || !objectId || !cameraId || !meshId)
    {
        return false;
    }
    cue::schema::SchemaRegistryIdentitySource identitySource;
    cue::schema::SchemaRegistryBuilder builder(identitySource, a_assertContext);
    auto addedCore = cue::runtime::add_runtime_schema_types(builder, a_assertContext);
    auto addedRenderer = addedCore ? cue::renderer::add_renderer_schema_types(builder, a_assertContext)
                                    : cue::Result<void>::failure(std::move(*addedCore.try_error()));
    if (!addedRenderer)
    {
        return false;
    }
    auto registry = builder.seal();
    if (!registry)
    {
        return false;
    }
    auto schemas = cue::renderer::make_renderer_value_schemas(**registry.try_value(), a_assertContext);
    if (!schemas)
    {
        return false;
    }
    auto values = cue::scene::ComponentValueSchemaRegistry::create(std::move(*schemas.try_value()),
                                                                    **registry.try_value(), a_assertContext);
    if (!values)
    {
        return false;
    }
    auto camera = cue::renderer::make_camera_component(*cameraId.try_value(), true, **registry.try_value(),
                                                        *values.try_value(), a_assertContext);
    auto mesh = cue::renderer::make_cube_mesh_component(*meshId.try_value(), **registry.try_value(),
                                                         *values.try_value(), a_assertContext);
    if (!camera || !mesh || !scene.try_value()->add_object(*objectId.try_value(), "Authoring Camera And Cube", true,
                                                           std::nullopt, cue::math::Transform{}) ||
        !scene.try_value()->add_component(*objectId.try_value(), std::move(*camera.try_value())) ||
        !scene.try_value()->add_component(*objectId.try_value(), std::move(*mesh.try_value())))
    {
        return false;
    }
    auto snapshot = cue::scene::create_scene_snapshot(*scene.try_value(), a_assertContext);
    auto published = snapshot ? cue::package::publish_minimal_runtime_data(*descriptor.try_value(),
                                                                            *snapshot.try_value(), a_assertContext)
                              : cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(
                                    std::move(*snapshot.try_error()));
    if (!published)
    {
        return false;
    }
    const std::string_view bytes = published.try_value()->startup_scene_data().bytes();
    if (!bytes.starts_with("{\"schemaVersion\":2,") ||
        bytes.find("\"typeId\":\"70000000-0000-4000-8000-000000000001\"") == std::string_view::npos ||
        bytes.find("\"typeId\":\"70000000-0000-4000-8000-000000000002\"") == std::string_view::npos ||
        bytes.find("cue://engine/mesh/cube") == std::string_view::npos ||
        bytes.find("Authoring Camera And Cube") != std::string_view::npos)
    {
        return false;
    }
    auto secondId = cue::scene::ObjectId::parse("20000000-0000-4000-8000-000000000002", a_assertContext);
    auto sceneId = cue::scene::SceneAssetId::parse(k_sceneId, a_assertContext);
    if (!secondId || !sceneId)
    {
        return false;
    }
    const cue::scene::SceneComponent &sharedComponent = scene.try_value()->objects()[0].components()[0];
    std::vector<cue::scene::RuntimeSceneObjectData> duplicateObjects;
    duplicateObjects.push_back({*objectId.try_value(), std::nullopt, true, cue::math::Transform{},
                                {sharedComponent}});
    duplicateObjects.push_back({*secondId.try_value(), std::nullopt, true, cue::math::Transform{},
                                {sharedComponent}});
    auto duplicateSnapshot = cue::scene::create_runtime_scene_snapshot(*sceneId.try_value(),
                                                                        std::move(duplicateObjects), a_assertContext);
    if (duplicateSnapshot)
    {
        auto duplicate = cue::package::publish_minimal_runtime_data(*descriptor.try_value(),
                                                                     *duplicateSnapshot.try_value(), a_assertContext);
        if (!has_package_error(duplicate, cue::package::PackageError::UnsupportedRuntimeSceneData))
        {
            return false;
        }
    }
    auto fovId = cue::schema::FieldId::create(2U, a_assertContext);
    auto invalidFov = cue::scene::FieldValue::floating_point(200.0, a_assertContext);
    if (!fovId || !invalidFov || !scene.try_value()->set_component_field(
            *objectId.try_value(), *cameraId.try_value(), *fovId.try_value(), std::move(*invalidFov.try_value())))
    {
        return false;
    }
    auto invalidSnapshot = cue::scene::create_scene_snapshot(*scene.try_value(), a_assertContext);
    if (!invalidSnapshot)
    {
        return false;
    }
    auto invalid = cue::package::publish_minimal_runtime_data(*descriptor.try_value(),
                                                               *invalidSnapshot.try_value(), a_assertContext);
    return has_package_error(invalid, cue::package::PackageError::UnsupportedRuntimeSceneData);
}

/// @brief 緩い生成Toleranceを通過した非単位QuaternionをPublisher境界で拒否するか検証する
[[nodiscard]] bool test_runtime_rotation_revalidated(const cue::AssertContext &a_assertContext)
{
    auto descriptor = make_descriptor(k_sceneId, a_assertContext);
    auto scene = make_scene(k_sceneId, a_assertContext);
    auto objectId = cue::scene::ObjectId::parse("20000000-0000-4000-8000-000000000001", a_assertContext);
    auto permissiveTolerance = cue::math::Tolerance::create(a_assertContext.fatal_handler(), 1.0F, 1.0F);
    if (!descriptor || !scene || !objectId || !permissiveTolerance)
    {
        return false;
    }
    auto transform = cue::math::Transform::create(
        a_assertContext.fatal_handler(), cue::math::Vector3{}, cue::math::Quaternion{0.0F, 0.0F, 0.0F, 2.0F},
        cue::math::Vector3{1.0F, 1.0F, 1.0F}, *permissiveTolerance.try_value());
    if (!transform ||
        !scene.try_value()->add_object(*objectId.try_value(), "Object", true, std::nullopt, *transform.try_value()))
    {
        return false;
    }
    auto snapshot = cue::scene::create_scene_snapshot(*scene.try_value(), a_assertContext);
    if (!snapshot)
    {
        return false;
    }
    auto published =
        cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *snapshot.try_value(), a_assertContext);
    return has_package_error(published, cue::package::PackageError::UnsupportedRuntimeSceneData);
}

/// @brief Runtime Scene v1が既知Componentも省略せずPublication全体で拒否するか検証する
[[nodiscard]] bool test_known_component_rejected(const cue::AssertContext &a_assertContext)
{
    auto descriptor = make_descriptor(k_sceneId, a_assertContext);
    auto scene = make_scene(k_sceneId, a_assertContext);
    auto objectId = cue::scene::ObjectId::parse("20000000-0000-4000-8000-000000000001", a_assertContext);
    auto componentId = cue::scene::ComponentInstanceId::parse("30000000-0000-4000-8000-000000000001", a_assertContext);
    auto typeId = cue::schema::TypeId::parse("40000000-0000-4000-8000-000000000001", a_assertContext);
    auto schemaVersion = cue::schema::SchemaVersion::create(1U, a_assertContext);
    auto fieldId = cue::schema::FieldId::create(7U, a_assertContext);
    if (!descriptor || !scene || !objectId || !componentId || !typeId || !schemaVersion || !fieldId)
    {
        return false;
    }

    std::vector<cue::schema::FieldDescriptor> fieldDescriptors;
    auto fieldDescriptor = cue::schema::create_field_descriptor(*fieldId.try_value(), "health", a_assertContext);
    if (!fieldDescriptor)
    {
        return false;
    }
    fieldDescriptors.push_back(std::move(*fieldDescriptor.try_value()));
    std::vector<cue::schema::FieldId> reservedFields;
    auto typeDescriptor = cue::schema::create_type_descriptor(*typeId.try_value(), "Cue.Package.TestComponent",
                                                              *schemaVersion.try_value(), std::move(fieldDescriptors),
                                                              std::move(reservedFields), a_assertContext);
    cue::schema::SchemaRegistryIdentitySource identitySource;
    cue::schema::SchemaRegistryBuilder registryBuilder(identitySource, a_assertContext);
    if (!typeDescriptor || !registryBuilder.add_type(std::move(*typeDescriptor.try_value())))
    {
        return false;
    }
    auto registry = registryBuilder.seal();
    if (!registry)
    {
        return false;
    }

    std::vector<cue::scene::FieldKindBinding> bindings{
        {*fieldId.try_value(), cue::scene::FieldValueKind::SignedInteger}};
    auto valueSchema = cue::scene::create_component_value_schema(
        *typeId.try_value(), *schemaVersion.try_value(), std::move(bindings), **registry.try_value(), a_assertContext);
    if (!valueSchema)
    {
        return false;
    }
    std::vector<cue::scene::ComponentValueSchema> valueSchemas;
    valueSchemas.push_back(std::move(*valueSchema.try_value()));
    auto valueRegistry = cue::scene::ComponentValueSchemaRegistry::create(std::move(valueSchemas),
                                                                          **registry.try_value(), a_assertContext);
    std::vector<cue::scene::KnownFieldData> knownFields;
    auto knownField = cue::scene::create_known_field(*fieldId.try_value(), cue::scene::FieldValue::signed_integer(42),
                                                     cue::scene::FieldValueKind::SignedInteger, a_assertContext);
    if (!valueRegistry || !knownField)
    {
        return false;
    }
    knownFields.push_back(std::move(*knownField.try_value()));
    std::vector<cue::scene::OpaqueFieldData> unknownFields;
    auto component = cue::scene::create_known_component(
        *componentId.try_value(), *typeId.try_value(), *schemaVersion.try_value(), std::move(knownFields),
        std::move(unknownFields), **registry.try_value(), *valueRegistry.try_value(), a_assertContext);
    if (!component ||
        !scene.try_value()->add_object(*objectId.try_value(), "AuthoringOnly", true, std::nullopt,
                                       cue::math::Transform{}) ||
        !scene.try_value()->add_component(*objectId.try_value(),
                                          cue::scene::SceneComponent::known(std::move(*component.try_value()))))
    {
        return false;
    }
    auto snapshot = cue::scene::create_scene_snapshot(*scene.try_value(), a_assertContext);
    auto published =
        snapshot ? cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *snapshot.try_value(),
                                                              a_assertContext)
                 : cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(std::move(*snapshot.try_error()));
    return has_package_error(published, cue::package::PackageError::UnsupportedRuntimeSceneData);
}

/// @brief Runtime解決基盤のないAsset ReferenceをPublication全体で拒否するか検証する
[[nodiscard]] bool test_asset_reference_rejected(const cue::AssertContext &a_assertContext)
{
    auto descriptor = make_descriptor(k_sceneId, a_assertContext);
    auto scene = make_scene(k_sceneId, a_assertContext);
    auto objectId = cue::scene::ObjectId::parse("20000000-0000-4000-8000-000000000001", a_assertContext);
    auto componentId = cue::scene::ComponentInstanceId::parse("30000000-0000-4000-8000-000000000001", a_assertContext);
    auto typeId = cue::schema::TypeId::parse("40000000-0000-4000-8000-000000000001", a_assertContext);
    auto schemaVersion = cue::schema::SchemaVersion::create(1U, a_assertContext);
    auto fieldId = cue::schema::FieldId::create(7U, a_assertContext);
    auto assetReference = cue::scene::AssetReferenceValue::create("asset://unresolved", a_assertContext);
    if (!descriptor || !scene || !objectId || !componentId || !typeId || !schemaVersion || !fieldId || !assetReference)
    {
        return false;
    }

    std::vector<cue::schema::FieldDescriptor> fieldDescriptors;
    auto fieldDescriptor = cue::schema::create_field_descriptor(*fieldId.try_value(), "asset", a_assertContext);
    if (!fieldDescriptor)
    {
        return false;
    }
    fieldDescriptors.push_back(std::move(*fieldDescriptor.try_value()));
    std::vector<cue::schema::FieldId> reservedFields;
    auto typeDescriptor = cue::schema::create_type_descriptor(*typeId.try_value(), "Cue.Package.AssetComponent",
                                                              *schemaVersion.try_value(), std::move(fieldDescriptors),
                                                              std::move(reservedFields), a_assertContext);
    cue::schema::SchemaRegistryIdentitySource identitySource;
    cue::schema::SchemaRegistryBuilder registryBuilder(identitySource, a_assertContext);
    if (!typeDescriptor || !registryBuilder.add_type(std::move(*typeDescriptor.try_value())))
    {
        return false;
    }
    auto registry = registryBuilder.seal();
    if (!registry)
    {
        return false;
    }

    std::vector<cue::scene::FieldKindBinding> bindings{
        {*fieldId.try_value(), cue::scene::FieldValueKind::AssetReference}};
    auto valueSchema = cue::scene::create_component_value_schema(
        *typeId.try_value(), *schemaVersion.try_value(), std::move(bindings), **registry.try_value(), a_assertContext);
    if (!valueSchema)
    {
        return false;
    }
    std::vector<cue::scene::ComponentValueSchema> valueSchemas;
    valueSchemas.push_back(std::move(*valueSchema.try_value()));
    auto valueRegistry = cue::scene::ComponentValueSchemaRegistry::create(std::move(valueSchemas),
                                                                          **registry.try_value(), a_assertContext);
    auto knownField = cue::scene::create_known_field(
        *fieldId.try_value(), cue::scene::FieldValue::asset_reference(std::move(*assetReference.try_value())),
        cue::scene::FieldValueKind::AssetReference, a_assertContext);
    if (!valueRegistry || !knownField)
    {
        return false;
    }
    std::vector<cue::scene::KnownFieldData> knownFields;
    knownFields.push_back(std::move(*knownField.try_value()));
    std::vector<cue::scene::OpaqueFieldData> unknownFields;
    auto component = cue::scene::create_known_component(
        *componentId.try_value(), *typeId.try_value(), *schemaVersion.try_value(), std::move(knownFields),
        std::move(unknownFields), **registry.try_value(), *valueRegistry.try_value(), a_assertContext);
    if (!component ||
        !scene.try_value()->add_object(*objectId.try_value(), "Asset", true, std::nullopt, cue::math::Transform{}) ||
        !scene.try_value()->add_component(*objectId.try_value(),
                                          cue::scene::SceneComponent::known(std::move(*component.try_value()))))
    {
        return false;
    }
    auto snapshot = cue::scene::create_scene_snapshot(*scene.try_value(), a_assertContext);
    auto published =
        snapshot ? cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *snapshot.try_value(),
                                                              a_assertContext)
                 : cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(std::move(*snapshot.try_error()));
    return has_package_error(published, cue::package::PackageError::UnsupportedRuntimeSceneData);
}

/// @brief Opaque Componentを黙って省略せずPublication全体を拒否するか検証する
[[nodiscard]] bool test_opaque_component_rejected(const cue::AssertContext &a_assertContext)
{
    auto descriptor = make_descriptor(k_sceneId, a_assertContext);
    auto scene = make_scene(k_sceneId, a_assertContext);
    auto objectId = cue::scene::ObjectId::parse("20000000-0000-4000-8000-000000000001", a_assertContext);
    auto componentId = cue::scene::ComponentInstanceId::parse("30000000-0000-4000-8000-000000000001", a_assertContext);
    auto typeId = cue::schema::TypeId::parse("40000000-0000-4000-8000-000000000001", a_assertContext);
    auto schemaVersion = cue::schema::SchemaVersion::create(1U, a_assertContext);
    cue::schema::SchemaRegistryIdentitySource identitySource;
    cue::schema::SchemaRegistryBuilder registryBuilder(identitySource, a_assertContext);
    auto registry = registryBuilder.seal();
    if (!descriptor || !scene || !objectId || !componentId || !typeId || !schemaVersion || !registry ||
        !scene.try_value()->add_object(*objectId.try_value(), "Opaque", true, std::nullopt, cue::math::Transform{}))
    {
        return false;
    }
    auto opaque = cue::scene::OpaqueComponentData::create(*componentId.try_value(), *typeId.try_value(),
                                                          *schemaVersion.try_value(), "{}", **registry.try_value(),
                                                          a_assertContext);
    if (!opaque || !scene.try_value()->add_component(
                       *objectId.try_value(), cue::scene::SceneComponent::opaque(std::move(*opaque.try_value()))))
    {
        return false;
    }
    auto snapshot = cue::scene::create_scene_snapshot(*scene.try_value(), a_assertContext);
    if (!snapshot)
    {
        return false;
    }
    auto published =
        cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *snapshot.try_value(), a_assertContext);
    return has_package_error(published, cue::package::PackageError::UnsupportedRuntimeSceneData);
}

/// @brief Startup Scene欠損とDescriptor／Scene Identity不一致をFail-closedにするか検証する
[[nodiscard]] bool test_startup_scene_contract(const cue::AssertContext &a_assertContext)
{
    constexpr std::string_view versionOneDescriptor =
        "{\"schemaVersion\":1,\"projectId\":\"00000000-0000-4000-8000-000000000901\","
        "\"displayName\":\"Legacy\",\"engineCompatibility\":{\"minimum\":\"1.0.0\","
        "\"maximumExclusive\":\"2.0.0\"},\"roots\":{\"sourceAssets\":\"Assets/Source\","
        "\"runtimeAssets\":\"Assets/Runtime\",\"generated\":\"Generated\",\"saved\":\"Saved\"},"
        "\"defaultScene\":null,\"requiredCapabilities\":[],\"extensions\":{}}";
    auto missing = cue::parse_project_descriptor(versionOneDescriptor, a_assertContext);
    auto matchingScene = make_scene(k_sceneId, a_assertContext);
    auto mismatchedDescriptor = make_descriptor(k_otherSceneId, a_assertContext);
    if (!missing || !matchingScene || !mismatchedDescriptor)
    {
        return false;
    }
    auto firstSnapshot = cue::scene::create_scene_snapshot(*matchingScene.try_value(), a_assertContext);
    auto secondSnapshot = cue::scene::create_scene_snapshot(*matchingScene.try_value(), a_assertContext);
    if (!firstSnapshot || !secondSnapshot)
    {
        return false;
    }
    auto missingResult =
        cue::package::publish_minimal_runtime_data(*missing.try_value(), *firstSnapshot.try_value(), a_assertContext);
    auto mismatchResult = cue::package::publish_minimal_runtime_data(*mismatchedDescriptor.try_value(),
                                                                     *secondSnapshot.try_value(), a_assertContext);
    return has_package_error(missingResult, cue::package::PackageError::MissingStartupScene) &&
           has_package_error(mismatchResult, cue::package::PackageError::StartupSceneIdentityMismatch);
}
} // namespace

/// @brief Minimal Runtime Dataの決定性、Projection、未知Data拒否、Identity契約を検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_sha256_vector() && test_deterministic_empty_scene(assertContext) &&
                   test_runtime_object_projection(assertContext) && test_shortest_float_serialization(assertContext) &&
                   test_renderer_scene_v2(assertContext) &&
                   test_runtime_rotation_revalidated(assertContext) && test_known_component_rejected(assertContext) &&
                   test_asset_reference_rejected(assertContext) && test_opaque_component_rejected(assertContext) &&
                   test_startup_scene_contract(assertContext)
               ? 0
               : 1;
}
