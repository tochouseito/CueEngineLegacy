#pragma once

#include <Cue/Math/Transform.h>
#include <Cue/Scene/ComponentData.h>
#include <Cue/Scene/Identity.h>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cue::editor_core
{
/// @brief Add Component操作へ公開する表示名と検証済み初期値Template
struct EditorComponentTemplate final
{
    std::string displayName;
    scene::SceneComponent prototype;
};

/// @brief Create Primitiveへ公開するCatalog Identity、検証済みMesh初期値、現在の描画可否
struct EditorPrimitiveTemplate final
{
    std::string displayName;
    std::string assetId;
    scene::SceneComponent meshPrototype;
    std::string unavailableReason;
    bool isEnabled = false;
};

/// @brief Stable Object Identity集合へEditor Selectionを切り替えるIntent
struct SelectObjectsIntent final
{
    std::vector<scene::ObjectId> objectIds;
    std::optional<scene::ObjectId> primaryObjectId;
};

/// @brief 指定ParentまたはScene Rootへ初期Objectを追加するIntent
struct AddObjectIntent final
{
    std::optional<scene::ObjectId> parentId;
    std::string name;
};

/// @brief Catalog候補IndexからMesh Component付きPrimitive Objectを一Transactionで追加するIntent
struct CreatePrimitiveIntent final
{
    std::optional<scene::ObjectId> parentId;
    std::size_t primitiveTemplateIndex;
};

/// @brief Stable Object IdentityのSubtreeを削除するIntent
struct DeleteObjectIntent final
{
    scene::ObjectId objectId;
};

/// @brief Stable Object IdentityのSubtreeを新しいIdentity群へ複製するIntent
struct DuplicateObjectIntent final
{
    scene::ObjectId objectId;
};

/// @brief Stable Object Identityの永続名を変更するIntent
struct RenameObjectIntent final
{
    scene::ObjectId objectId;
    std::string name;
};

/// @brief Stable Object Identityを別Parentへ移動またはRootへ切り離すIntent
struct ReparentObjectIntent final
{
    scene::ObjectId objectId;
    std::optional<scene::ObjectId> parentId;
};

/// @brief Stable Object IdentityのCore Transformを置き換えるIntent
struct EditTransformIntent final
{
    scene::ObjectId objectId;
    math::Transform transform;
};

/// @brief Template Type Identityと候補Indexから新しいComponent Instanceを追加するIntent
struct AddComponentIntent final
{
    scene::ObjectId objectId;
    schema::TypeId componentTypeId;
    std::size_t componentTemplateIndex;
};

/// @brief Stable Object／Component IdentityのComponentを削除するIntent
struct RemoveComponentIntent final
{
    scene::ObjectId objectId;
    scene::ComponentInstanceId componentId;
};

/// @brief 現在Documentの直前Transactionを取り消すIntent
struct UndoIntent final
{
};

/// @brief 現在Documentの取り消し済みTransactionを再適用するIntent
struct RedoIntent final
{
};

/// @brief Hierarchy・Inspector操作をEditorControllerへ渡す意味Intentの閉じた集合
using EditorIntent = std::variant<SelectObjectsIntent, AddObjectIntent, CreatePrimitiveIntent, DeleteObjectIntent,
                                  DuplicateObjectIntent, RenameObjectIntent, ReparentObjectIntent, EditTransformIntent,
                                  AddComponentIntent, RemoveComponentIntent, UndoIntent, RedoIntent>;
} // namespace cue::editor_core
