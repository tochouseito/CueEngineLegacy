#include <Cue/EditorCore/EditorController.h>

#include <Cue/EditorCore/Error.h>
#include <Cue/EngineAssets/BuiltInAssetCatalog.h>
#include <Cue/Renderer/RendererSchema.h>
#include <Cue/Scene/Error.h>

#include <algorithm>
#include <array>
#include <exception>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cue::editor_core
{
namespace
{
/// @brief Value付きResultを診断を保持したResult<void>へ変換する
template <typename T> [[nodiscard]] Result<void> discard_value(Result<T> a_result) noexcept
{
    if (!a_result)
    {
        return Result<void>::failure(std::move(*a_result.try_error()));
    }
    return Result<void>::success();
}

/// @brief SceneComponentが保持するStable Type Identityを返す
[[nodiscard]] std::optional<schema::TypeId> component_type_id(const scene::SceneComponent &a_component) noexcept
{
    if (const scene::KnownComponentData *known = a_component.try_known(); known != nullptr)
    {
        return known->type_id();
    }
    if (const scene::OpaqueComponentData *opaque = a_component.try_opaque(); opaque != nullptr)
    {
        return opaque->type_id();
    }
    return std::nullopt;
}

/// @brief CandidateがSource Root自身またはそのDescendantかRead-only Parent Chainで判定する
[[nodiscard]] bool is_in_subtree(const scene::SceneDocument &a_document, const scene::ObjectId &a_candidateId,
                                 const scene::ObjectId &a_sourceRootId) noexcept
{
    const scene::SceneObject *candidate = a_document.find_object(a_candidateId);
    while (candidate != nullptr)
    {
        if (candidate->id() == a_sourceRootId)
        {
            return true;
        }
        const scene::ObjectId *parentId = candidate->try_parent_id();
        candidate = parentId != nullptr ? a_document.find_object(*parentId) : nullptr;
    }
    return false;
}

/// @brief Subtree Duplicate Commandと複製後に選択するRoot Identityを所有する
struct GeneratedDuplicate final
{
    DuplicateObjectCommand command;
    scene::ObjectId duplicateRootId;
};

/// @brief Scene名上限内でUTF-8境界を保ちつつ複製Suffixを付加する
[[nodiscard]] std::string make_duplicate_name(std::string_view a_sourceName)
{
    constexpr std::string_view k_suffix = " Copy";
    const std::size_t maximumPrefixBytes = scene::k_maximumSceneStringBytes - k_suffix.size();
    std::size_t prefixBytes = (std::min)(a_sourceName.size(), maximumPrefixBytes);
    while (prefixBytes < a_sourceName.size() && prefixBytes > 0U &&
           (static_cast<unsigned char>(a_sourceName[prefixBytes]) & 0xC0U) == 0x80U)
    {
        --prefixBytes;
    }

    std::string result(a_sourceName.substr(0U, prefixBytes));
    result.append(k_suffix);
    return result;
}

/// @brief Read-only Scene ViewからSubtree全体の新しいObject／Component Identity対応を生成する
[[nodiscard]] Result<GeneratedDuplicate> generate_duplicate(const scene::SceneDocument &a_document,
                                                            const scene::ObjectId &a_sourceRootId,
                                                            scene::SceneIdentitySource &a_identitySource,
                                                            const AssertContext &a_assertContext)
{
    const scene::SceneObject *sourceRoot = a_document.find_object(a_sourceRootId);
    if (sourceRoot == nullptr)
    {
        return Result<GeneratedDuplicate>::failure(scene::make_scene_error(
            a_assertContext, scene::SceneError::ObjectNotFound, "Duplicate source object was not found"));
    }

    std::vector<DuplicateObjectTarget> targets;
    targets.reserve(a_document.object_count());
    std::optional<scene::ObjectId> duplicateRootId;
    for (const scene::SceneObject &object : a_document.objects())
    {
        if (!is_in_subtree(a_document, object.id(), a_sourceRootId))
        {
            continue;
        }

        Result<scene::ObjectId> generatedObjectId = scene::ObjectId::generate(a_identitySource, a_assertContext);
        if (!generatedObjectId)
        {
            return Result<GeneratedDuplicate>::failure(std::move(*generatedObjectId.try_error()));
        }
        scene::ObjectId objectId = std::move(*generatedObjectId.try_value());
        if (object.id() == a_sourceRootId)
        {
            duplicateRootId = objectId;
        }

        std::vector<scene::ComponentInstanceId> componentIds;
        componentIds.reserve(object.components().size());
        for (const scene::SceneComponent &component : object.components())
        {
            static_cast<void>(component);
            Result<scene::ComponentInstanceId> generatedComponentId =
                scene::ComponentInstanceId::generate(a_identitySource, a_assertContext);
            if (!generatedComponentId)
            {
                return Result<GeneratedDuplicate>::failure(std::move(*generatedComponentId.try_error()));
            }
            componentIds.push_back(std::move(*generatedComponentId.try_value()));
        }

        std::string duplicateName =
            object.id() == a_sourceRootId ? make_duplicate_name(object.name()) : std::string(object.name());
        targets.push_back(
            DuplicateObjectTarget{object.id(), std::move(objectId), std::move(duplicateName), std::move(componentIds)});
    }

    return Result<GeneratedDuplicate>::success(
        GeneratedDuplicate{DuplicateObjectCommand{a_sourceRootId, std::move(targets)}, std::move(*duplicateRootId)});
}

/// @brief 一つのSceneCommandを意味Label付きTransactionとしてControllerへ渡す
[[nodiscard]] Result<DocumentStateId> execute_named_command(EditorController &a_controller,
                                                            EditorDocumentId a_documentId,
                                                            const scene::SceneAssetId &a_sceneAssetId,
                                                            SceneEditCommand a_command, std::string_view a_label)
{
    EditorTransaction transaction{std::string(a_label), {}};
    transaction.commands.push_back(SceneCommandRequest{a_documentId, a_sceneAssetId, std::move(a_command)});
    return a_controller.execute_transaction(std::move(transaction));
}

/// @brief Component Type IdentityがTemplateのPrototypeと一致するか判定する
[[nodiscard]] bool template_matches(const EditorComponentTemplate &a_template, schema::TypeId a_typeId) noexcept
{
    const std::optional<schema::TypeId> templateTypeId = component_type_id(a_template.prototype);
    return templateTypeId.has_value() && *templateTypeId == a_typeId;
}

/// @brief Primitive Templateの表示Identityと永続Asset Referenceが一意に一致するか検証する
[[nodiscard]] bool primitive_template_matches(const EditorPrimitiveTemplate &a_template,
                                              const AssertContext &a_assertContext) noexcept
{
    const scene::KnownComponentData *known = a_template.meshPrototype.try_known();
    if (a_template.displayName.empty() || a_template.assetId.empty() || known == nullptr ||
        !a_template.meshPrototype.is_valid())
    {
        return false;
    }

    Result<const engine_assets::BuiltInMeshDescriptor *> descriptor =
        engine_assets::resolve_builtin_mesh_descriptor(a_template.assetId, a_assertContext);
    Result<renderer::RendererSchemaTypeIds> typeIds = renderer::make_renderer_schema_type_ids(a_assertContext);
    Result<renderer::MeshFieldIds> fieldIds = renderer::make_mesh_field_ids(a_assertContext);
    Result<void> rendererComponent =
        renderer::validate_runtime_scene_component(a_template.meshPrototype, a_assertContext);
    if (!descriptor || !typeIds || !fieldIds || !rendererComponent || known->type_id() != typeIds.try_value()->mesh ||
        known->known_fields().size() != 1U || known->known_fields()[0].id() != fieldIds.try_value()->asset ||
        !known->unknown_fields().empty())
    {
        return false;
    }

    const scene::AssetReferenceValue *asset = known->known_fields()[0].value().try_asset_reference();
    return asset != nullptr && asset->token() == a_template.assetId;
}
} // namespace

Result<void> EditorController::execute_intent(EditorDocumentId a_documentId, EditorIntent a_intent,
                                              scene::SceneIdentitySource &a_identitySource,
                                              std::span<const EditorComponentTemplate> a_componentTemplates,
                                              std::span<const EditorPrimitiveTemplate> a_primitiveTemplates) noexcept
{
    assert_owner_thread();
    try
    {
        const EditorDocument *document = m_session.find_document(a_documentId);
        if (document == nullptr)
        {
            return Result<void>::failure(make_editor_document_error(*m_assertContext, EditorCoreError::DocumentNotFound,
                                                                    "Editor intent document was not found",
                                                                    a_documentId.value()));
        }
        const scene::SceneAssetId sceneAssetId = document->scene_document().scene_asset_id();

        /// @brief Intent AlternativeごとにStable IdentityだけをCommandまたはWorkflowへ変換する
        auto dispatch = [this, document, a_documentId, &a_identitySource, a_componentTemplates, a_primitiveTemplates,
                         &sceneAssetId](auto &&a_typedIntent) -> Result<void>
        {
            using Intent = std::remove_cvref_t<decltype(a_typedIntent)>;
            if constexpr (std::is_same_v<Intent, SelectObjectsIntent>)
            {
                const scene::ObjectId *primary =
                    a_typedIntent.primaryObjectId.has_value() ? &*a_typedIntent.primaryObjectId : nullptr;
                return set_selection(a_documentId, a_typedIntent.objectIds, primary);
            }
            else if constexpr (std::is_same_v<Intent, AddObjectIntent>)
            {
                Result<scene::ObjectId> generated = scene::ObjectId::generate(a_identitySource, *m_assertContext);
                if (!generated)
                {
                    return Result<void>::failure(std::move(*generated.try_error()));
                }
                scene::ObjectId objectId = std::move(*generated.try_value());
                Result<DocumentStateId> applied =
                    execute_named_command(*this, a_documentId, sceneAssetId,
                                          AddObjectCommand{objectId, std::move(a_typedIntent.name), true,
                                                           std::move(a_typedIntent.parentId), math::Transform{}},
                                          "Objectを追加");
                if (!applied)
                {
                    return Result<void>::failure(std::move(*applied.try_error()));
                }
                const std::array<scene::ObjectId, 1> selection{objectId};
                return set_selection(a_documentId, selection, &selection[0]);
            }
            else if constexpr (std::is_same_v<Intent, CreatePrimitiveIntent>)
            {
                if (a_typedIntent.primitiveTemplateIndex >= a_primitiveTemplates.size())
                {
                    return Result<void>::failure(scene::make_scene_error(*m_assertContext,
                                                                         scene::SceneError::UnknownSchemaType,
                                                                         "Editor primitive template was not found"));
                }
                const EditorPrimitiveTemplate &primitiveTemplate =
                    a_primitiveTemplates[a_typedIntent.primitiveTemplateIndex];
                if (!primitiveTemplate.isEnabled)
                {
                    return Result<void>::failure(
                        scene::make_scene_error(*m_assertContext, scene::SceneError::UnknownSchemaType,
                                                "Editor primitive is not supported by the current renderer"));
                }
                if (!primitive_template_matches(primitiveTemplate, *m_assertContext))
                {
                    return Result<void>::failure(scene::make_scene_error(*m_assertContext,
                                                                         scene::SceneError::UnknownSchemaType,
                                                                         "Editor primitive template is invalid"));
                }

                Result<scene::ObjectId> generatedObject = scene::ObjectId::generate(a_identitySource, *m_assertContext);
                if (!generatedObject)
                {
                    return Result<void>::failure(std::move(*generatedObject.try_error()));
                }
                scene::ObjectId objectId = std::move(*generatedObject.try_value());
                Result<scene::ComponentInstanceId> generatedComponent =
                    scene::ComponentInstanceId::generate(a_identitySource, *m_assertContext);
                if (!generatedComponent)
                {
                    return Result<void>::failure(std::move(*generatedComponent.try_error()));
                }
                Result<scene::SceneComponent> mesh = primitiveTemplate.meshPrototype.duplicate_with_identity(
                    std::move(*generatedComponent.try_value()), *m_assertContext);
                if (!mesh)
                {
                    return Result<void>::failure(std::move(*mesh.try_error()));
                }

                EditorTransaction transaction{"Primitiveを追加", {}};
                transaction.commands.push_back(
                    SceneCommandRequest{a_documentId, sceneAssetId,
                                        AddObjectCommand{objectId, primitiveTemplate.displayName, true,
                                                         std::move(a_typedIntent.parentId), math::Transform{}}});
                transaction.commands.push_back(SceneCommandRequest{
                    a_documentId, sceneAssetId, AddComponentCommand{objectId, std::move(*mesh.try_value())}});
                Result<DocumentStateId> applied = execute_transaction(std::move(transaction));
                if (!applied)
                {
                    return Result<void>::failure(std::move(*applied.try_error()));
                }
                const std::array<scene::ObjectId, 1> selection{objectId};
                return set_selection(a_documentId, selection, &selection[0]);
            }
            else if constexpr (std::is_same_v<Intent, DeleteObjectIntent>)
            {
                return discard_value(execute_named_command(*this, a_documentId, sceneAssetId,
                                                           DeleteObjectCommand{std::move(a_typedIntent.objectId)},
                                                           "Objectを削除"));
            }
            else if constexpr (std::is_same_v<Intent, DuplicateObjectIntent>)
            {
                Result<GeneratedDuplicate> generated = generate_duplicate(
                    document->scene_document(), a_typedIntent.objectId, a_identitySource, *m_assertContext);
                if (!generated)
                {
                    return Result<void>::failure(std::move(*generated.try_error()));
                }
                const scene::ObjectId duplicateRootId = generated.try_value()->duplicateRootId;
                Result<DocumentStateId> applied = execute_named_command(
                    *this, a_documentId, sceneAssetId, std::move(generated.try_value()->command), "Objectを複製");
                if (!applied)
                {
                    return Result<void>::failure(std::move(*applied.try_error()));
                }
                const std::array<scene::ObjectId, 1> selection{duplicateRootId};
                return set_selection(a_documentId, selection, &selection[0]);
            }
            else if constexpr (std::is_same_v<Intent, RenameObjectIntent>)
            {
                return discard_value(execute_named_command(
                    *this, a_documentId, sceneAssetId,
                    RenameObjectCommand{std::move(a_typedIntent.objectId), std::move(a_typedIntent.name)},
                    "Object名を変更"));
            }
            else if constexpr (std::is_same_v<Intent, ReparentObjectIntent>)
            {
                return discard_value(execute_named_command(
                    *this, a_documentId, sceneAssetId,
                    ReparentObjectCommand{std::move(a_typedIntent.objectId), std::move(a_typedIntent.parentId)},
                    "Hierarchyを変更"));
            }
            else if constexpr (std::is_same_v<Intent, EditTransformIntent>)
            {
                return discard_value(execute_named_command(
                    *this, a_documentId, sceneAssetId,
                    EditTransformCommand{std::move(a_typedIntent.objectId), std::move(a_typedIntent.transform)},
                    "Transformを変更"));
            }
            else if constexpr (std::is_same_v<Intent, AddComponentIntent>)
            {
                if (a_typedIntent.componentTemplateIndex >= a_componentTemplates.size())
                {
                    return Result<void>::failure(scene::make_scene_error(*m_assertContext,
                                                                         scene::SceneError::UnknownSchemaType,
                                                                         "Editor component template was not found"));
                }
                const EditorComponentTemplate &componentTemplate =
                    a_componentTemplates[a_typedIntent.componentTemplateIndex];
                if (!template_matches(componentTemplate, a_typedIntent.componentTypeId))
                {
                    return Result<void>::failure(
                        scene::make_scene_error(*m_assertContext, scene::SceneError::UnknownSchemaType,
                                                "Editor component template identity did not match its type"));
                }
                Result<scene::ComponentInstanceId> generated =
                    scene::ComponentInstanceId::generate(a_identitySource, *m_assertContext);
                if (!generated)
                {
                    return Result<void>::failure(std::move(*generated.try_error()));
                }
                Result<scene::SceneComponent> component = componentTemplate.prototype.duplicate_with_identity(
                    std::move(*generated.try_value()), *m_assertContext);
                if (!component)
                {
                    return Result<void>::failure(std::move(*component.try_error()));
                }
                return discard_value(execute_named_command(
                    *this, a_documentId, sceneAssetId,
                    AddComponentCommand{std::move(a_typedIntent.objectId), std::move(*component.try_value())},
                    "Componentを追加"));
            }
            else if constexpr (std::is_same_v<Intent, RemoveComponentIntent>)
            {
                return discard_value(execute_named_command(
                    *this, a_documentId, sceneAssetId,
                    RemoveComponentCommand{std::move(a_typedIntent.objectId), std::move(a_typedIntent.componentId)},
                    "Componentを削除"));
            }
            else if constexpr (std::is_same_v<Intent, UndoIntent>)
            {
                return discard_value(undo(a_documentId));
            }
            else
            {
                return discard_value(redo(a_documentId));
            }
        };

        return std::visit(dispatch, std::move(a_intent));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation();
    }
    catch (...)
    {
        terminate_exception();
    }
}
} // namespace cue::editor_core
