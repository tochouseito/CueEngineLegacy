#include <Cue/Editor/ImGui/EditorPresenter.h>

#include <Cue/Editor/ImGui/EditorDockspace.h>

#include <Cue/EngineAssets/BuiltInMesh.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Renderer/RendererSchema.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Schema/Descriptor.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

static_assert(!std::is_copy_constructible_v<cue::editor::EditorPresenter>);
static_assert(!std::is_move_constructible_v<cue::editor::EditorPresenter>);

namespace
{
/// @brief Test中の回復不能状態をProcess失敗へ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalをProcess失敗へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }

    /// @brief Message付きFatalをProcess失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

/// @brief Object／Component生成要求へ重複しないUUID Version 4候補を返す
class TestSceneIdentitySource final : public cue::scene::SceneIdentitySource
{
  public:
    /// @brief Counterを埋め込んだRFC 4122 UUID Version 4候補を返す
    [[nodiscard]] cue::scene::IdentityBytes next_identity() noexcept override
    {
        cue::scene::IdentityBytes bytes{};
        for (std::size_t index = 0U; index < sizeof(m_next); ++index)
        {
            bytes[bytes.size() - 1U - index] = static_cast<std::uint8_t>((m_next >> (index * 8U)) & 0xFFU);
        }
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
        ++m_next;
        return bytes;
    }

  private:
    std::uint64_t m_next = 0x100U;
};

/// @brief 条件が偽ならTest Processを失敗終了する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::abort();
    }
}

/// @brief 成功Resultから所有Valueを取り出す
template <typename T> T take_value(cue::Result<T> a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief 固定IdentityからProject Descriptorを生成する
[[nodiscard]] cue::ProjectDescriptor make_project_descriptor(const cue::AssertContext &a_assertContext) noexcept
{
    cue::ProjectId projectId =
        take_value(cue::ProjectId::parse("00000000-0000-4000-8000-000000000001", a_assertContext));
    return take_value(cue::create_blank_project_descriptor(
        projectId, "Editor ImGui Test", cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, std::nullopt},
        "00000000-0000-4000-8000-000000000099", a_assertContext));
}

/// @brief Test用Component Type Identityを生成する
[[nodiscard]] cue::schema::TypeId make_component_type_id(const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::TypeId::parse("10000000-0000-4000-8000-000000000001", a_assertContext));
}

/// @brief Test用Component Field Identityを生成する
[[nodiscard]] cue::schema::FieldId make_field_id(const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::FieldId::create(1U, a_assertContext));
}

/// @brief Font非対応文字を含むString Field用Identityを生成する
[[nodiscard]] cue::schema::FieldId make_string_field_id(const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::FieldId::create(2U, a_assertContext));
}

/// @brief Primitive Test用Asset Reference Field Identityを生成する
[[nodiscard]] cue::schema::FieldId make_asset_field_id(const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::FieldId::create(3U, a_assertContext));
}

/// @brief Test用Component Schema Versionを生成する
[[nodiscard]] cue::schema::SchemaVersion make_schema_version(const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::SchemaVersion::create(1U, a_assertContext));
}

/// @brief Test用Component Descriptorを持つImmutable Schema Registryを構築する
[[nodiscard]] std::unique_ptr<cue::schema::SchemaRegistry> make_schema_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::schema::FieldDescriptor> fields;
    fields.push_back(
        take_value(cue::schema::create_field_descriptor(make_field_id(a_assertContext), "value", a_assertContext)));
    fields.push_back(take_value(cue::schema::create_field_descriptor(make_string_field_id(a_assertContext),
                                                                     "status \xF0\x9F\x98\x80", a_assertContext)));
    fields.push_back(take_value(
        cue::schema::create_field_descriptor(make_asset_field_id(a_assertContext), "asset", a_assertContext)));
    std::vector<cue::schema::FieldId> reserved;
    cue::schema::TypeDescriptor descriptor = take_value(cue::schema::create_type_descriptor(
        make_component_type_id(a_assertContext), "Cue.Editor.TestComponent", make_schema_version(a_assertContext),
        std::move(fields), std::move(reserved), a_assertContext));
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    require(builder.add_type(std::move(descriptor)).has_value());
    require(cue::renderer::add_renderer_schema_types(builder, a_assertContext).has_value());
    return take_value(builder.seal());
}

/// @brief Test用Component Field KindをSchema Typeへ結び付ける
[[nodiscard]] cue::scene::ComponentValueSchemaRegistry make_value_registry(
    const cue::schema::SchemaRegistry &a_registry, const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::scene::FieldKindBinding> bindings{
        {make_field_id(a_assertContext), cue::scene::FieldValueKind::SignedInteger},
        {make_string_field_id(a_assertContext), cue::scene::FieldValueKind::String},
        {make_asset_field_id(a_assertContext), cue::scene::FieldValueKind::AssetReference}};
    std::vector<cue::scene::ComponentValueSchema> schemas =
        take_value(cue::renderer::make_renderer_value_schemas(a_registry, a_assertContext));
    schemas.push_back(take_value(cue::scene::create_component_value_schema(
        make_component_type_id(a_assertContext), make_schema_version(a_assertContext), std::move(bindings), a_registry,
        a_assertContext)));
    return take_value(
        cue::scene::ComponentValueSchemaRegistry::create(std::move(schemas), a_registry, a_assertContext));
}

/// @brief 固定Identityと初期値からAdd Component用Prototypeを生成する
[[nodiscard]] cue::scene::SceneComponent make_component(const cue::schema::SchemaRegistry &a_registry,
                                                        const cue::scene::ComponentValueSchemaRegistry &a_valueRegistry,
                                                        const cue::AssertContext &a_assertContext,
                                                        std::int64_t a_value = 10) noexcept
{
    cue::scene::ComponentInstanceId componentId =
        take_value(cue::scene::ComponentInstanceId::parse("20000000-0000-4000-8000-000000000001", a_assertContext));
    std::vector<cue::scene::KnownFieldData> fields;
    fields.push_back(take_value(
        cue::scene::create_known_field(make_field_id(a_assertContext), cue::scene::FieldValue::signed_integer(a_value),
                                       cue::scene::FieldValueKind::SignedInteger, a_assertContext)));
    fields.push_back(take_value(cue::scene::create_known_field(
        make_string_field_id(a_assertContext),
        take_value(cue::scene::FieldValue::string("Ready \xF0\x9F\x98\x81", a_assertContext)),
        cue::scene::FieldValueKind::String, a_assertContext)));
    std::vector<cue::scene::OpaqueFieldData> unknownFields;
    cue::scene::KnownComponentData component = take_value(cue::scene::create_known_component(
        std::move(componentId), make_component_type_id(a_assertContext), make_schema_version(a_assertContext),
        std::move(fields), std::move(unknownFields), a_registry, a_valueRegistry, a_assertContext));
    return cue::scene::SceneComponent::known(std::move(component));
}

/// @brief 選択Built-in Asset IDだけを保持するCreate Primitive用Prototypeを生成する
[[nodiscard]] cue::scene::SceneComponent make_primitive_component(
    const cue::schema::SchemaRegistry &a_registry, const cue::scene::ComponentValueSchemaRegistry &a_valueRegistry,
    const cue::AssertContext &a_assertContext, std::string_view a_assetId) noexcept
{
    cue::scene::ComponentInstanceId componentId =
        take_value(cue::scene::ComponentInstanceId::parse("20000000-0000-4000-8000-000000000002", a_assertContext));
    cue::scene::AssetReferenceValue asset =
        take_value(cue::scene::AssetReferenceValue::create(a_assetId, a_assertContext));
    std::vector<cue::scene::KnownFieldData> fields;
    fields.push_back(take_value(cue::scene::create_known_field(
        make_asset_field_id(a_assertContext), cue::scene::FieldValue::asset_reference(std::move(asset)),
        cue::scene::FieldValueKind::AssetReference, a_assertContext)));
    std::vector<cue::scene::OpaqueFieldData> unknownFields;
    cue::scene::KnownComponentData component = take_value(cue::scene::create_known_component(
        std::move(componentId), make_component_type_id(a_assertContext), make_schema_version(a_assertContext),
        std::move(fields), std::move(unknownFields), a_registry, a_valueRegistry, a_assertContext));
    return cue::scene::SceneComponent::known(std::move(component));
}

/// @brief 固定文字列からScene Object Identityを生成する
[[nodiscard]] cue::scene::ObjectId make_object_id(std::string_view a_text,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::scene::ObjectId::parse(a_text, a_assertContext));
}

/// @brief Headless ImGui FrameでHierarchy・Inspector描画がBackend非依存で完了することを検証する
void draw_frame(cue::editor::EditorPresenter &a_presenter) noexcept
{
    ImGui::NewFrame();
    a_presenter.draw();
    const bool isVisible = ImGui::Begin("CueEngine Editor");
    require(isVisible);
    const ImGuiWindow *editorWindow = ImGui::GetCurrentWindowRead();
    require(editorWindow != nullptr);
    require((editorWindow->Flags & ImGuiWindowFlags_NoDocking) != 0);
    require((editorWindow->Flags & ImGuiWindowFlags_NoTitleBar) != 0);
    require((editorWindow->Flags & ImGuiWindowFlags_NoResize) != 0);
    require((editorWindow->Flags & ImGuiWindowFlags_NoMove) != 0);
    require((editorWindow->Flags & ImGuiWindowFlags_NoBringToFrontOnFocus) != 0);
    require((editorWindow->Flags & ImGuiWindowFlags_NoNavFocus) != 0);
    ImGui::End();
    const ImGuiWindow *hierarchyWindow = ImGui::FindWindowByName("Hierarchy");
    const ImGuiWindow *inspectorWindow = ImGui::FindWindowByName("Inspector");
    require(hierarchyWindow != nullptr && hierarchyWindow->DockId != 0U);
    require(inspectorWindow != nullptr && inspectorWindow->DockId != 0U);
    ImGui::Render();
}

/// @brief 全Editor IntentがController経由で適用されSelectionと診断が整合することを検証する
void test_hierarchy_inspector_intents() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource registryIdentitySource;
    std::unique_ptr<cue::schema::SchemaRegistry> registry = make_schema_registry(registryIdentitySource, assertContext);
    cue::scene::ComponentValueSchemaRegistry valueRegistry = make_value_registry(*registry, assertContext);
    TestSceneIdentitySource sceneIdentitySource;

    const cue::scene::ObjectId rootId = make_object_id("00000000-0000-4000-8000-000000000011", assertContext);
    const cue::scene::ObjectId childId = make_object_id("00000000-0000-4000-8000-000000000012", assertContext);
    cue::scene::SceneAssetId sceneAssetId =
        take_value(cue::scene::SceneAssetId::parse("00000000-0000-4000-8000-000000000010", assertContext));
    cue::scene::SceneDocument scene = cue::scene::SceneDocument::create(sceneAssetId, assertContext);
    require(scene.add_object(rootId, "Root", true, std::nullopt, cue::math::Transform{}).has_value());
    require(scene.add_object(childId, "Child", true, rootId, cue::math::Transform{}).has_value());

    std::unique_ptr<cue::editor_core::EditorController> controller =
        cue::editor_core::EditorController::create(make_project_descriptor(assertContext), assertContext);
    cue::RelativePath locator = take_value(cue::RelativePath::parse("Scenes/Test.cuescene", assertContext));
    const cue::editor_core::EditorDocumentId documentId =
        take_value(controller->open_document(std::move(scene), std::move(locator), true));
    std::vector<cue::editor_core::EditorComponentTemplate> templates;
    templates.push_back(cue::editor_core::EditorComponentTemplate{
        "Test Component", make_component(*registry, valueRegistry, assertContext)});
    templates.push_back(cue::editor_core::EditorComponentTemplate{
        "Test Component Alternative", make_component(*registry, valueRegistry, assertContext, 20)});
    std::vector<cue::editor_core::EditorPrimitiveTemplate> primitiveTemplates;
    primitiveTemplates.push_back(cue::editor_core::EditorPrimitiveTemplate{
        "Cube",
        std::string(cue::engine_assets::k_cubeMeshAssetId),
        take_value(cue::renderer::make_cube_mesh_component(
            take_value(cue::scene::ComponentInstanceId::parse("20000000-0000-4000-8000-000000000002", assertContext)),
            *registry, valueRegistry, assertContext)),
        {},
        true});
    primitiveTemplates.push_back(cue::editor_core::EditorPrimitiveTemplate{
        "Plane", std::string(cue::engine_assets::k_planeMeshAssetId),
        take_value(cue::renderer::make_builtin_mesh_component(
            take_value(cue::scene::ComponentInstanceId::parse("20000000-0000-4000-8000-000000000003", assertContext)),
            cue::engine_assets::k_planeMeshAssetId, *registry, valueRegistry, assertContext)),
        "Renderer未対応", false});
    primitiveTemplates.push_back(cue::editor_core::EditorPrimitiveTemplate{
        "Broken",
        std::string(cue::engine_assets::k_cubeMeshAssetId),
        make_primitive_component(*registry, valueRegistry, assertContext, cue::engine_assets::k_cubeMeshAssetId),
        {},
        true});
    std::unique_ptr<cue::editor::EditorPresenter> presenter =
        cue::editor::EditorPresenter::create(*controller, documentId, sceneIdentitySource, *registry,
                                             std::move(templates), std::move(primitiveTemplates), assertContext);

    require(presenter->submit(cue::editor_core::SelectObjectsIntent{{rootId}, rootId}).has_value());
    require(presenter->submit(cue::editor_core::RenameObjectIntent{rootId, "Renamed Root"}).has_value());
    const cue::editor_core::EditorDocument *document = controller->session().find_document(documentId);
    require(document != nullptr);
    require(document->scene_document().find_object(rootId)->name() == "Renamed Root");

    const cue::Result<void> rejectedCycle = presenter->submit(cue::editor_core::ReparentObjectIntent{rootId, childId});
    require(!rejectedCycle.has_value());
    require(presenter->has_error_message());
    require(!presenter->message().empty());
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(rootId)->try_parent_id() == nullptr);

    require(presenter->submit(cue::editor_core::ReparentObjectIntent{childId, std::nullopt}).has_value());
    require(presenter->submit(cue::editor_core::ReparentObjectIntent{childId, rootId}).has_value());

    cue::Result<cue::math::Tolerance> tolerance =
        cue::math::Tolerance::create(assertContext.fatal_handler(), 0.00001F, 0.00001F);
    require(tolerance.has_value());
    cue::Result<cue::math::Transform> transform = cue::math::Transform::create(
        assertContext.fatal_handler(), cue::math::Vector3{1.0F, 2.0F, 3.0F}, cue::math::Quaternion{},
        cue::math::Vector3{2.0F, 2.0F, 2.0F}, *tolerance.try_value());
    require(transform.has_value());
    require(presenter->submit(cue::editor_core::EditTransformIntent{rootId, std::move(*transform.try_value())})
                .has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(rootId)->transform().translation() ==
            cue::math::Vector3{1.0F, 2.0F, 3.0F});

    require(presenter->submit(cue::editor_core::AddComponentIntent{rootId, make_component_type_id(assertContext), 1U})
                .has_value());
    document = controller->session().find_document(documentId);
    const cue::scene::SceneObject *root = document->scene_document().find_object(rootId);
    require(root != nullptr && root->components().size() == 1U);
    const cue::scene::KnownComponentData *addedComponent = root->components()[0].try_known();
    require(addedComponent != nullptr && addedComponent->known_fields().size() == 2U);
    require(*addedComponent->known_fields()[0].value().try_signed_integer() == 20);
    const cue::scene::ComponentInstanceId addedComponentId = root->components()[0].instance_id();
    require(presenter->submit(cue::editor_core::RemoveComponentIntent{rootId, addedComponentId}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(rootId)->components().empty());

    require(presenter->submit(cue::editor_core::AddObjectIntent{rootId, "Added Child"}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().object_count() == 3U);
    require(document->selection().size() == 1U);
    const cue::scene::ObjectId addedObjectId = document->selection()[0];
    const cue::scene::SceneObject *addedObject = document->scene_document().find_object(addedObjectId);
    require(addedObject != nullptr && addedObject->try_parent_id() != nullptr &&
            *addedObject->try_parent_id() == rootId);

    require(presenter->submit(cue::editor_core::DuplicateObjectIntent{rootId}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().object_count() == 6U);
    require(document->selection().size() == 1U);
    const cue::scene::ObjectId duplicateRootId = document->selection()[0];
    require(document->scene_document().find_object(duplicateRootId)->name() == "Renamed Root Copy");

    require(presenter->submit(cue::editor_core::DeleteObjectIntent{duplicateRootId}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().object_count() == 3U);
    require(document->selection().empty());
    require(presenter->submit(cue::editor_core::UndoIntent{}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().object_count() == 6U);
    require(document->selection().empty());
    require(presenter->submit(cue::editor_core::RedoIntent{}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().object_count() == 3U);
    require(document->selection().empty());

    const cue::Result<void> disabledPrimitive =
        presenter->submit(cue::editor_core::CreatePrimitiveIntent{std::nullopt, 1U});
    require(!disabledPrimitive.has_value());
    require(presenter->has_error_message());
    document = controller->session().find_document(documentId);
    require(document->scene_document().object_count() == 3U);

    const cue::Result<void> mismatchedPrimitive =
        presenter->submit(cue::editor_core::CreatePrimitiveIntent{std::nullopt, 2U});
    require(!mismatchedPrimitive.has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().object_count() == 3U);

    require(presenter->submit(cue::editor_core::CreatePrimitiveIntent{std::nullopt, 0U}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().object_count() == 4U && document->selection().size() == 1U);
    const cue::scene::ObjectId primitiveId = document->selection()[0];
    const cue::scene::SceneObject *primitive = document->scene_document().find_object(primitiveId);
    require(primitive != nullptr && primitive->name() == "Cube" && primitive->try_parent_id() == nullptr);
    require(primitive->transform().translation() == cue::math::Vector3{});
    require(primitive->transform().scale() == cue::math::Vector3{1.0F, 1.0F, 1.0F});
    require(primitive->components().size() == 1U);
    const cue::scene::ComponentInstanceId primitiveComponentId = primitive->components()[0].instance_id();
    const cue::scene::KnownComponentData *primitiveComponent = primitive->components()[0].try_known();
    require(primitiveComponent != nullptr && primitiveComponent->known_fields().size() == 1U);
    const cue::scene::AssetReferenceValue *primitiveAsset =
        primitiveComponent->known_fields()[0].value().try_asset_reference();
    require(primitiveAsset != nullptr && primitiveAsset->token() == "cue://engine/mesh/cube");

    require(presenter->submit(cue::editor_core::UndoIntent{}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().object_count() == 3U &&
            document->scene_document().find_object(primitiveId) == nullptr);
    require(presenter->submit(cue::editor_core::RedoIntent{}).has_value());
    document = controller->session().find_document(documentId);
    primitive = document->scene_document().find_object(primitiveId);
    require(primitive != nullptr && primitive->components().size() == 1U &&
            primitive->components()[0].instance_id() == primitiveComponentId);
    require(primitive->transform().translation() == cue::math::Vector3{});
    require(primitive->transform().scale() == cue::math::Vector3{1.0F, 1.0F, 1.0F});
    require(presenter->submit(cue::editor_core::UndoIntent{}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().object_count() == 3U);

    require(presenter->submit(cue::editor_core::SelectObjectsIntent{{rootId}, rootId}).has_value());
    const std::string longName(512U, 'N');
    require(presenter->submit(cue::editor_core::RenameObjectIntent{rootId, longName}).has_value());
    require(ImGui::CreateContext() != nullptr);
    ImGuiIO &input = ImGui::GetIO();
    input.IniFilename = nullptr;
    cue::editor::enable_editor_docking();
    input.DisplaySize = ImVec2(1280.0F, 720.0F);
    input.DeltaTime = 1.0F / 60.0F;
    static_cast<void>(input.Fonts->Build());
    draw_frame(*presenter);
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(rootId)->name() == longName);

    const std::string unsupportedFontName = "Object \xF0\x9F\x98\x80";
    require(presenter->submit(cue::editor_core::RenameObjectIntent{rootId, unsupportedFontName}).has_value());
    draw_frame(*presenter);
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(rootId)->name() == unsupportedFontName);

    const std::string embeddedNullName("A\0B", 3U);
    require(presenter->submit(cue::editor_core::RenameObjectIntent{rootId, embeddedNullName}).has_value());
    draw_frame(*presenter);
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(rootId)->name() == std::string_view(embeddedNullName));

    const std::string historyLabel("History##A\0\xF0\x9F\x98\x80", 15U);
    cue::editor_core::EditorTransaction historyTransaction{historyLabel, {}};
    historyTransaction.commands.push_back(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "History Label Target"}});
    require(controller->execute_transaction(std::move(historyTransaction)).has_value());
    document = controller->session().find_document(documentId);
    require(document->undo_label() == std::string_view(historyLabel));
    draw_frame(*presenter);
    require(controller->undo(documentId).has_value());
    document = controller->session().find_document(documentId);
    require(document->redo_label() == std::string_view(historyLabel));
    draw_frame(*presenter);
    ImGui::DestroyContext();

    constexpr std::string_view duplicateSuffix = " Copy";
    std::string maximumName(cue::scene::k_maximumSceneStringBytes - duplicateSuffix.size() - 1U, 'A');
    maximumName.append("\xE3\x81\x82");
    maximumName.resize(cue::scene::k_maximumSceneStringBytes, 'B');
    require(presenter->submit(cue::editor_core::RenameObjectIntent{rootId, maximumName}).has_value());
    require(presenter->submit(cue::editor_core::DuplicateObjectIntent{rootId}).has_value());
    document = controller->session().find_document(documentId);
    require(document->selection().size() == 1U);
    const cue::scene::SceneObject *maximumNameDuplicate =
        document->scene_document().find_object(document->selection()[0]);
    require(maximumNameDuplicate != nullptr);
    require(maximumNameDuplicate->name().size() <= cue::scene::k_maximumSceneStringBytes);
    require(maximumNameDuplicate->name().ends_with(duplicateSuffix));
}
} // namespace

/// @brief Hierarchy・Inspector PresenterのController接続とHeadless描画を検証する
int main()
{
    test_hierarchy_inspector_intents();
    return 0;
}
