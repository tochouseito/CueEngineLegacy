#include <Cue/Editor/ImGui/EditorPresenter.h>

#include <Cue/Editor/ImGui/EditorDockspace.h>

#include <Cue/EditorCore/Error.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Scene/Error.h>
#include <Cue/Schema/Descriptor.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <imgui.h>

namespace
{
/// @brief Allocation失敗をEditor PresentationのFatal境界へ渡す
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("Editor Presentation allocation failed");
    std::terminate();
}

/// @brief 予期しない例外をEditor PresentationのFatal境界へ渡す
[[noreturn]] void terminate_exception(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("Editor Presentation unexpected exception");
    std::terminate();
}

/// @brief SceneComponentが保持するStable Type Identityを返す
[[nodiscard]] std::optional<cue::schema::TypeId> component_type_id(
    const cue::scene::SceneComponent &a_component) noexcept
{
    if (const cue::scene::KnownComponentData *known = a_component.try_known(); known != nullptr)
    {
        return known->type_id();
    }
    if (const cue::scene::OpaqueComponentData *opaque = a_component.try_opaque(); opaque != nullptr)
    {
        return opaque->type_id();
    }
    return std::nullopt;
}

/// @brief Stable Type Identityを診断用の32桁Hex文字列へ変換する
[[nodiscard]] std::string type_id_text(cue::schema::TypeId a_typeId)
{
    constexpr char k_hexDigits[] = "0123456789abcdef";
    std::string text;
    text.resize(32U);
    std::size_t index = 0U;
    for (const std::uint8_t byte : a_typeId.bytes())
    {
        text[index++] = k_hexDigits[(byte >> 4U) & 0x0FU];
        text[index++] = k_hexDigits[byte & 0x0FU];
    }
    return text;
}

/// @brief 任意の表示Textに含まれる特殊ByteとFont非対応Unicode Scalarを一意な可視表現へ変換する
[[nodiscard]] std::string display_text_label(std::string_view a_text, bool *a_usedUnicodeEscape = nullptr)
{
    constexpr char k_hexDigits[] = "0123456789abcdef";
    std::string label;
    label.reserve(a_text.size());
    if (a_usedUnicodeEscape != nullptr)
    {
        *a_usedUnicodeEscape = false;
    }

    const char *cursor = a_text.data();
    const char *const end = cursor + a_text.size();
    ImFont *font = ImGui::GetFont();
    while (cursor < end)
    {
        const auto byte = static_cast<unsigned char>(*cursor);
        switch (byte)
        {
        case 0U:
            label.append("\\0");
            ++cursor;
            break;
        case static_cast<unsigned char>('\n'):
            label.append("\\n");
            ++cursor;
            break;
        case static_cast<unsigned char>('\r'):
            label.append("\\r");
            ++cursor;
            break;
        case static_cast<unsigned char>('\t'):
            label.append("\\t");
            ++cursor;
            break;
        case static_cast<unsigned char>('\\'):
            label.append("\\\\");
            ++cursor;
            break;
        case static_cast<unsigned char>('#'):
            label.append("\\#");
            ++cursor;
            break;
        default:
            if (byte < 0x20U || byte == 0x7FU)
            {
                label.append("\\x");
                label.push_back(k_hexDigits[(byte >> 4U) & 0x0FU]);
                label.push_back(k_hexDigits[byte & 0x0FU]);
                ++cursor;
            }
            else if (byte < 0x80U)
            {
                label.push_back(static_cast<char>(byte));
                ++cursor;
            }
            else
            {
                std::size_t byteCount = 0U;
                std::uint32_t scalar = 0U;
                if (byte >= 0xC2U && byte <= 0xDFU)
                {
                    byteCount = 2U;
                    scalar = byte & 0x1FU;
                }
                else if (byte >= 0xE0U && byte <= 0xEFU)
                {
                    byteCount = 3U;
                    scalar = byte & 0x0FU;
                }
                else if (byte >= 0xF0U && byte <= 0xF4U)
                {
                    byteCount = 4U;
                    scalar = byte & 0x07U;
                }

                bool isValid = byteCount > 0U && static_cast<std::size_t>(end - cursor) >= byteCount;
                for (std::size_t index = 1U; isValid && index < byteCount; ++index)
                {
                    const auto continuation = static_cast<unsigned char>(cursor[index]);
                    isValid = (continuation & 0xC0U) == 0x80U;
                    scalar = (scalar << 6U) | (continuation & 0x3FU);
                }
                isValid = isValid && scalar <= 0x10FFFFU && !(scalar >= 0xD800U && scalar <= 0xDFFFU) &&
                          !(byteCount == 3U && scalar < 0x800U) && !(byteCount == 4U && scalar < 0x10000U);
                if (!isValid)
                {
                    label.append("\\x");
                    label.push_back(k_hexDigits[(byte >> 4U) & 0x0FU]);
                    label.push_back(k_hexDigits[byte & 0x0FU]);
                    ++cursor;
                    break;
                }

                const bool isRepresentable =
                    scalar <= static_cast<std::uint32_t>((std::numeric_limits<ImWchar>::max)());
                if (font != nullptr && isRepresentable && font->IsGlyphInFont(static_cast<ImWchar>(scalar)))
                {
                    label.append(cursor, byteCount);
                }
                else
                {
                    if (a_usedUnicodeEscape != nullptr)
                    {
                        *a_usedUnicodeEscape = true;
                    }
                    label.append("\\u{");
                    int highestShift = 12;
                    while (highestShift < 20 && scalar >= (1U << (highestShift + 4)))
                    {
                        highestShift += 4;
                    }
                    for (int shift = highestShift; shift >= 0; shift -= 4)
                    {
                        label.push_back(k_hexDigits[(scalar >> shift) & 0x0FU]);
                    }
                    label.push_back('}');
                }
                cursor += byteCount;
            }
            break;
        }
    }
    return label;
}

/// @brief Object表示名へStable Identityを併記して同名候補を識別可能にする
[[nodiscard]] std::string object_reference_label(const cue::scene::SceneObject &a_object)
{
    std::string label = display_text_label(a_object.name());
    const cue::scene::IdentityText identity = a_object.id().canonical_text();
    label.append(" [");
    label.append(identity.data(), identity.size());
    label.push_back(']');
    return label;
}

/// @brief ImGui入力前後と正本値を比較し、未保存成分だけをDirtyとして保持する
template <std::size_t Size>
void update_component_dirty_state(const std::array<float, Size> &a_previous, const std::array<float, Size> &a_current,
                                  const std::array<float, Size> &a_authoritative,
                                  std::array<bool, Size> &a_dirty) noexcept
{
    for (std::size_t index = 0U; index < Size; ++index)
    {
        a_dirty[index] = a_current[index] == a_authoritative[index]
                             ? false
                             : a_dirty[index] || a_current[index] != a_previous[index];
    }
}

/// @brief 未編集成分だけを新しい正本値へ同期し、別Objectでは全成分を置き換える
template <std::size_t Size>
void sync_clean_components(std::array<float, Size> &a_current, std::array<bool, Size> &a_dirty,
                           const std::array<float, Size> &a_authoritative, bool a_isSameObject) noexcept
{
    for (std::size_t index = 0U; index < Size; ++index)
    {
        if (!a_isSameObject || !a_dirty[index])
        {
            a_current[index] = a_authoritative[index];
            a_dirty[index] = false;
        }
    }
}

/// @brief Stable Component IdentityをImGui ID用のCanonical文字列へ変換する
[[nodiscard]] cue::scene::IdentityText component_id_text(const cue::scene::ComponentInstanceId &a_componentId) noexcept
{
    return a_componentId.canonical_text();
}

/// @brief ImGuiの可変長Text編集へ所有StringとFatal境界を渡す
struct StringInputContext final
{
    std::string *value;
    const cue::AssertContext *assertContext;
};

/// @brief ImGuiのBuffer拡張要求へstd::stringの所有Bufferを追従させる
int resize_string_input(ImGuiInputTextCallbackData *a_data) noexcept
{
    auto *context = static_cast<StringInputContext *>(a_data->UserData);
    try
    {
        context->value->resize(static_cast<std::size_t>(a_data->BufTextLen));
        a_data->Buf = context->value->data();
        return 0;
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(*context->assertContext);
    }
    catch (...)
    {
        terminate_exception(*context->assertContext);
    }
}

/// @brief Scene Object名を切り捨てずにImGuiの可変長InputTextへ接続する
[[nodiscard]] bool input_object_name(std::string &a_value, const cue::AssertContext &a_assertContext)
{
    StringInputContext context{&a_value, &a_assertContext};
    const bool submitted = ImGui::InputText("Name", a_value.data(), a_value.capacity() + 1U,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
                                            resize_string_input, &context);
    a_value.resize(std::strlen(a_value.c_str()));
    return submitted;
}

/// @brief Stable Object Identityの16 byteをHierarchy索引用Hashへ変換する
struct ObjectIdHash final
{
    [[nodiscard]] std::size_t operator()(const cue::scene::ObjectId &a_objectId) const noexcept
    {
        std::size_t hash = 0U;
        for (const std::uint8_t byte : a_objectId.bytes())
        {
            hash = (hash * 131U) ^ static_cast<std::size_t>(byte);
        }
        return hash;
    }
};

using HierarchyChildIndex =
    std::unordered_map<cue::scene::ObjectId, std::vector<const cue::scene::SceneObject *>, ObjectIdHash>;
using HierarchySelectionIndex = std::unordered_set<cue::scene::ObjectId, ObjectIdHash>;

/// @brief Frame単位Selection索引にStable Object Identityが含まれるか判定する
[[nodiscard]] bool is_selected(const HierarchySelectionIndex &a_selection,
                               const cue::scene::ObjectId &a_objectId) noexcept
{
    return a_selection.contains(a_objectId);
}

/// @brief Candidateが対象Object自身またはそのDescendantかRead-only Parent Chainで判定する
[[nodiscard]] bool is_in_subtree(const cue::scene::SceneDocument &a_document, const cue::scene::ObjectId &a_candidateId,
                                 const cue::scene::ObjectId &a_rootId) noexcept
{
    const cue::scene::SceneObject *candidate = a_document.find_object(a_candidateId);
    while (candidate != nullptr)
    {
        if (candidate->id() == a_rootId)
        {
            return true;
        }
        const cue::scene::ObjectId *parentId = candidate->try_parent_id();
        candidate = parentId != nullptr ? a_document.find_object(*parentId) : nullptr;
    }
    return false;
}

/// @brief ObjectのScene Root始まりDepthをRead-only Parent Chainから返す
[[nodiscard]] std::size_t hierarchy_depth(const cue::scene::SceneDocument &a_document,
                                          const cue::scene::ObjectId &a_objectId) noexcept
{
    std::size_t depth = 0U;
    const cue::scene::SceneObject *current = a_document.find_object(a_objectId);
    while (current != nullptr && depth <= cue::scene::SceneDocument::maximum_hierarchy_depth())
    {
        ++depth;
        const cue::scene::ObjectId *parentId = current->try_parent_id();
        current = parentId != nullptr ? a_document.find_object(*parentId) : nullptr;
    }
    return depth;
}

/// @brief 対象Objectを1とするSubtree最大相対DepthをRead-only Parent Chainから返す
[[nodiscard]] std::size_t subtree_height(const cue::scene::SceneDocument &a_document,
                                         const cue::scene::ObjectId &a_rootId) noexcept
{
    std::size_t maximumHeight = 1U;
    for (const cue::scene::SceneObject &candidate : a_document.objects())
    {
        std::size_t height = 1U;
        const cue::scene::ObjectId *parentId = candidate.try_parent_id();
        while (parentId != nullptr && height <= cue::scene::SceneDocument::maximum_hierarchy_depth())
        {
            if (*parentId == a_rootId)
            {
                maximumHeight = (std::max)(maximumHeight, height + 1U);
                break;
            }
            const cue::scene::SceneObject *parent = a_document.find_object(*parentId);
            parentId = parent != nullptr ? parent->try_parent_id() : nullptr;
            ++height;
        }
    }
    return maximumHeight;
}

/// @brief Subtree複製のObject数とComponent複製可否を保持する
struct SubtreeDuplicateInfo final
{
    std::size_t objectCount = 1U;
    bool allComponentsDuplicable = true;
};

/// @brief Child索引から対象Subtreeの複製可否を返す
[[nodiscard]] SubtreeDuplicateInfo subtree_duplicate_info(const cue::scene::SceneObject &a_root,
                                                          const HierarchyChildIndex &a_childrenByParent) noexcept
{
    SubtreeDuplicateInfo info;
    for (const cue::scene::SceneComponent &component : a_root.components())
    {
        info.allComponentsDuplicable =
            info.allComponentsDuplicable && component.try_known() != nullptr && component.is_valid();
    }

    const auto children = a_childrenByParent.find(a_root.id());
    if (children == a_childrenByParent.end())
    {
        return info;
    }
    for (const cue::scene::SceneObject *child : children->second)
    {
        const SubtreeDuplicateInfo childInfo = subtree_duplicate_info(*child, a_childrenByParent);
        info.objectCount += childInfo.objectCount;
        info.allComponentsDuplicable = info.allComponentsDuplicable && childInfo.allComponentsDuplicable;
    }
    return info;
}

/// @brief Frame単位Child索引から一ObjectとChild群を再帰描画する
void draw_object_node(const cue::scene::SceneObject &a_object, const HierarchyChildIndex &a_childrenByParent,
                      const HierarchySelectionIndex &a_selectionIndex,
                      std::span<const cue::scene::ObjectId> a_selection, const cue::scene::ObjectId *a_primarySelection,
                      std::optional<cue::editor_core::EditorIntent> &a_pendingIntent)
{
    const auto children = a_childrenByParent.find(a_object.id());
    const bool hasChildren = children != a_childrenByParent.end() && !children->second.empty();

    const cue::scene::IdentityText objectIdText = a_object.id().canonical_text();
    ImGui::PushID(objectIdText.data(), objectIdText.data() + objectIdText.size());
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasChildren)
    {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (is_selected(a_selectionIndex, a_object.id()))
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    const std::string name = object_reference_label(a_object);
    const bool isOpen = ImGui::TreeNodeEx("##Object", flags, "%s", name.c_str());
    const bool mouseActivated = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen();
    const bool keyboardActivated = ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                                              ImGui::IsKeyPressed(ImGuiKey_Space, false));
    if (!a_pendingIntent.has_value() && (mouseActivated || keyboardActivated))
    {
        std::vector<cue::scene::ObjectId> nextSelection;
        std::optional<cue::scene::ObjectId> primary;
        if (ImGui::GetIO().KeyCtrl)
        {
            nextSelection.assign(a_selection.begin(), a_selection.end());
            const auto found = std::find(nextSelection.begin(), nextSelection.end(), a_object.id());
            if (found == nextSelection.end())
            {
                nextSelection.push_back(a_object.id());
                primary = a_object.id();
            }
            else
            {
                nextSelection.erase(found);
                if (!nextSelection.empty())
                {
                    const bool canKeepPrimary = a_primarySelection != nullptr && *a_primarySelection != a_object.id() &&
                                                std::find(nextSelection.begin(), nextSelection.end(),
                                                          *a_primarySelection) != nextSelection.end();
                    primary = canKeepPrimary ? std::optional<cue::scene::ObjectId>(*a_primarySelection)
                                             : std::optional<cue::scene::ObjectId>(nextSelection.front());
                }
            }
        }
        else
        {
            nextSelection.push_back(a_object.id());
            primary = a_object.id();
        }
        a_pendingIntent.emplace(cue::editor_core::SelectObjectsIntent{std::move(nextSelection), std::move(primary)});
    }

    if (hasChildren && isOpen)
    {
        for (const cue::scene::SceneObject *child : children->second)
        {
            draw_object_node(*child, a_childrenByParent, a_selectionIndex, a_selection, a_primarySelection,
                             a_pendingIntent);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

/// @brief Schema RegistryからComponent表示名を取得し、InstanceのStable Identityを併記する
[[nodiscard]] std::string component_label(const cue::scene::SceneComponent &a_component,
                                          const cue::schema::SchemaRegistry &a_registry,
                                          const cue::AssertContext &a_assertContext)
{
    std::string label;
    const std::optional<cue::schema::TypeId> typeId = component_type_id(a_component);
    if (!typeId.has_value())
    {
        label = "無効なComponent";
    }
    else
    {
        cue::Result<const cue::schema::TypeDescriptor *> descriptor = a_registry.find(*typeId, a_assertContext);
        label = descriptor ? display_text_label((*descriptor.try_value())->name())
                           : "Unknown Component [" + type_id_text(*typeId) + "]";
    }
    const cue::scene::IdentityText instanceId = component_id_text(a_component.instance_id());
    label.append(" [");
    label.append(instanceId.data(), instanceId.size());
    label.push_back(']');
    return label;
}

/// @brief Field Descriptorがあれば診断名を返し、未登録FieldはStable数値を返す
[[nodiscard]] std::string field_label(const cue::schema::TypeDescriptor *a_descriptor, cue::schema::FieldId a_fieldId,
                                      const cue::AssertContext &a_assertContext)
{
    if (a_descriptor != nullptr)
    {
        cue::Result<const cue::schema::FieldDescriptor *> field = a_descriptor->find_field(a_fieldId, a_assertContext);
        if (field)
        {
            return display_text_label((*field.try_value())->name());
        }
    }
    return "Field " + std::to_string(a_fieldId.value());
}

/// @brief 型付きAuthoring Field ValueをInspectorのRead-only表示へ変換する
void draw_field_value(const cue::scene::FieldValue &a_value)
{
    switch (a_value.kind())
    {
    case cue::scene::FieldValueKind::Boolean:
        ImGui::TextUnformatted(*a_value.try_boolean() ? "true" : "false");
        break;
    case cue::scene::FieldValueKind::SignedInteger:
        ImGui::Text("%lld", static_cast<long long>(*a_value.try_signed_integer()));
        break;
    case cue::scene::FieldValueKind::UnsignedInteger:
        ImGui::Text("%llu", static_cast<unsigned long long>(*a_value.try_unsigned_integer()));
        break;
    case cue::scene::FieldValueKind::FloatingPoint:
        ImGui::Text("%.6g", *a_value.try_floating_point());
        break;
    case cue::scene::FieldValueKind::String:
    {
        const std::string text = display_text_label(*a_value.try_string());
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        break;
    }
    case cue::scene::FieldValueKind::AssetReference:
    {
        const std::string_view token = a_value.try_asset_reference()->token();
        const std::string text = display_text_label(token);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        break;
    }
    }
}

/// @brief 成功したSemantic Intentを利用者向けStatusへ変換する
[[nodiscard]] std::string_view intent_status(const cue::editor_core::EditorIntent &a_intent) noexcept
{
    return std::visit(
        /// @brief Intent AlternativeごとにPresentationだけが所有する成功Messageを返す
        [](const auto &a_typedIntent) noexcept -> std::string_view
        {
            using Intent = std::remove_cvref_t<decltype(a_typedIntent)>;
            if constexpr (std::is_same_v<Intent, cue::editor_core::SelectObjectsIntent>)
            {
                return "選択を更新しました。";
            }
            else if constexpr (std::is_same_v<Intent, cue::editor_core::AddObjectIntent>)
            {
                return "Objectを追加しました。";
            }
            else if constexpr (std::is_same_v<Intent, cue::editor_core::CreatePrimitiveIntent>)
            {
                return "Primitiveを追加しました。";
            }
            else if constexpr (std::is_same_v<Intent, cue::editor_core::DeleteObjectIntent>)
            {
                return "ObjectとChildを削除しました。";
            }
            else if constexpr (std::is_same_v<Intent, cue::editor_core::DuplicateObjectIntent>)
            {
                return "ObjectとChildを複製しました。";
            }
            else if constexpr (std::is_same_v<Intent, cue::editor_core::RenameObjectIntent>)
            {
                return "Object名を変更しました。";
            }
            else if constexpr (std::is_same_v<Intent, cue::editor_core::ReparentObjectIntent>)
            {
                return a_typedIntent.parentId.has_value() ? "Parentを変更しました。" : "ObjectをRootへ移動しました。";
            }
            else if constexpr (std::is_same_v<Intent, cue::editor_core::EditTransformIntent>)
            {
                return "Transformを変更しました。";
            }
            else if constexpr (std::is_same_v<Intent, cue::editor_core::AddComponentIntent>)
            {
                return "Componentを追加しました。";
            }
            else if constexpr (std::is_same_v<Intent, cue::editor_core::RemoveComponentIntent>)
            {
                return "Componentを削除しました。";
            }
            else if constexpr (std::is_same_v<Intent, cue::editor_core::UndoIntent>)
            {
                return "直前の編集を元に戻しました。";
            }
            else
            {
                return "取り消した編集を再適用しました。";
            }
        },
        a_intent);
}

} // namespace

namespace cue::editor
{
using editor_core::AddComponentIntent;
using editor_core::AddObjectIntent;
using editor_core::CreatePrimitiveIntent;
using editor_core::DeleteObjectIntent;
using editor_core::DuplicateObjectIntent;
using editor_core::EditorComponentTemplate;
using editor_core::EditorIntent;
using editor_core::EditorPrimitiveTemplate;
using editor_core::EditTransformIntent;
using editor_core::RedoIntent;
using editor_core::RemoveComponentIntent;
using editor_core::RenameObjectIntent;
using editor_core::ReparentObjectIntent;
using editor_core::SelectObjectsIntent;
using editor_core::UndoIntent;

EditorPresenter::EditorPresenter(editor_core::EditorController &a_controller,
                                 editor_core::EditorDocumentId a_documentId,
                                 scene::SceneIdentitySource &a_identitySource,
                                 const schema::SchemaRegistry &a_schemaRegistry,
                                 std::vector<editor_core::EditorComponentTemplate> a_componentTemplates,
                                 std::vector<editor_core::EditorPrimitiveTemplate> a_primitiveTemplates,
                                 const AssertContext &a_assertContext) noexcept
    : m_controller(&a_controller), m_identitySource(&a_identitySource), m_schemaRegistry(&a_schemaRegistry),
      m_assertContext(&a_assertContext), m_componentTemplates(std::move(a_componentTemplates)),
      m_primitiveTemplates(std::move(a_primitiveTemplates)), m_documentId(a_documentId)
{
}

std::unique_ptr<EditorPresenter> EditorPresenter::create(
    editor_core::EditorController &a_controller, editor_core::EditorDocumentId a_documentId,
    scene::SceneIdentitySource &a_identitySource, const schema::SchemaRegistry &a_schemaRegistry,
    std::vector<editor_core::EditorComponentTemplate> a_componentTemplates,
    std::vector<editor_core::EditorPrimitiveTemplate> a_primitiveTemplates,
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        return std::unique_ptr<EditorPresenter>(new EditorPresenter(a_controller, a_documentId, a_identitySource,
                                                                    a_schemaRegistry, std::move(a_componentTemplates),
                                                                    std::move(a_primitiveTemplates), a_assertContext));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

void EditorPresenter::draw() noexcept
{
    try
    {
        m_preserveInspectorErrorAfterIntent = false;
        std::optional<EditorIntent> pendingIntent = std::move(m_deferredIntent);
        m_deferredIntent.reset();
        const bool isApplyingDeferredIntent = pendingIntent.has_value();
        const editor_core::EditorDocument *document = m_controller->session().find_document(m_documentId);

        if (begin_editor_dockspace_host())
        {
            if (document != nullptr)
            {
                ImGui::BeginDisabled(isApplyingDeferredIntent);
                draw_menu(*document, pendingIntent);
                ImGui::EndDisabled();
            }

            if (!m_message.empty())
            {
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      m_hasError ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F) : ImVec4(0.45F, 0.9F, 0.55F, 1.0F));
                ImGui::TextWrapped("%s", m_message.c_str());
                ImGui::PopStyleColor();
                ImGui::Separator();
            }

            if (document == nullptr)
            {
                ImGui::TextUnformatted("編集対象のScene Documentが開かれていません。");
            }
        }
        end_editor_dockspace_host();

        if (document != nullptr)
        {
            ImGui::BeginDisabled(isApplyingDeferredIntent);
            draw_hierarchy(*document, pendingIntent);
            draw_inspector(*document, pendingIntent);
            ImGui::EndDisabled();
        }

        // Scene ViewのPointerやSpanを使用し終えたFrame末尾だけでController Mutationを行う
        if (pendingIntent.has_value())
        {
            const bool preserveInspectorError = m_preserveInspectorErrorAfterIntent;
            std::string inspectorError = preserveInspectorError ? m_message : std::string{};
            Result<void> result = submit(std::move(*pendingIntent));
            if (!result)
            {
                // Name確定に失敗した場合は、入力Bufferを修正できるように遷移Intentを破棄する
                m_deferredIntent.reset();
            }
            else if (preserveInspectorError)
            {
                m_message = std::move(inspectorError);
                m_hasError = true;
            }
        }
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(*m_assertContext);
    }
    catch (...)
    {
        terminate_exception(*m_assertContext);
    }
}

std::optional<EditorWorkflowRequest> EditorPresenter::take_workflow_request() noexcept
{
    std::optional<EditorWorkflowRequest> request = m_workflowRequest;
    m_workflowRequest.reset();
    return request;
}

void EditorPresenter::report_workflow_error(const Error &a_error) noexcept
{
    set_error(a_error);
}

void EditorPresenter::report_workflow_status(std::string_view a_status) noexcept
{
    set_status(a_status);
}

Result<void> EditorPresenter::submit(editor_core::EditorIntent a_intent) noexcept
{
    try
    {
        const std::string_view status = intent_status(a_intent);
        const RenameObjectIntent *renameIntent = std::get_if<RenameObjectIntent>(&a_intent);
        const bool appliesInspectorName = renameIntent != nullptr && m_inspectorObjectId.has_value() &&
                                          renameIntent->objectId == *m_inspectorObjectId;
        std::string appliedName = appliesInspectorName ? renameIntent->name : std::string{};
        const EditTransformIntent *transformIntent = std::get_if<EditTransformIntent>(&a_intent);
        const bool appliesInspectorTransform = transformIntent != nullptr && m_inspectorObjectId.has_value() &&
                                               transformIntent->objectId == *m_inspectorObjectId;
        std::array<float, 3> appliedTranslation{};
        std::array<float, 4> appliedRotation{};
        std::array<float, 3> appliedScale{};
        if (appliesInspectorTransform)
        {
            const math::Vector3 translation = transformIntent->transform.translation();
            const math::Quaternion rotation = transformIntent->transform.rotation();
            const math::Vector3 scale = transformIntent->transform.scale();
            appliedTranslation = {translation.x, translation.y, translation.z};
            appliedRotation = {rotation.x, rotation.y, rotation.z, rotation.w};
            appliedScale = {scale.x, scale.y, scale.z};
        }
        Result<void> result = m_controller->execute_intent(m_documentId, std::move(a_intent), *m_identitySource,
                                                           m_componentTemplates, m_primitiveTemplates);
        if (!result)
        {
            set_error(*result.try_error());
            return result;
        }
        if (appliesInspectorTransform)
        {
            m_translation = appliedTranslation;
            m_rotation = appliedRotation;
            m_scale = appliedScale;
            m_translationDirty.fill(false);
            m_rotationDirty.fill(false);
            m_scaleDirty.fill(false);
        }
        if (appliesInspectorName)
        {
            m_name = std::move(appliedName);
            m_nameDirty = false;
        }
        set_status(status);
        return result;
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(*m_assertContext);
    }
    catch (...)
    {
        terminate_exception(*m_assertContext);
    }
}

void EditorPresenter::draw_menu(const editor_core::EditorDocument &a_document,
                                std::optional<EditorIntent> &a_pendingIntent)
{
    if (!ImGui::BeginMenuBar())
    {
        return;
    }
    if (ImGui::BeginMenu("ファイル"))
    {
        const bool canRequest = !m_workflowRequest.has_value() && !a_pendingIntent.has_value();
        if (ImGui::MenuItem("新しいScene", "Ctrl+N", false, canRequest))
        {
            m_workflowRequest = EditorWorkflowRequest::NewScene;
        }
        if (ImGui::MenuItem("Sceneを開く", "Ctrl+O", false, canRequest))
        {
            m_workflowRequest = EditorWorkflowRequest::OpenScene;
        }
        if (ImGui::MenuItem("保存", "Ctrl+S", false, canRequest))
        {
            m_workflowRequest = EditorWorkflowRequest::SaveScene;
        }
        if (ImGui::MenuItem("名前を付けて保存", "Ctrl+Shift+S", false, canRequest))
        {
            m_workflowRequest = EditorWorkflowRequest::SaveSceneAs;
        }
        if (ImGui::MenuItem("再読込", nullptr, false, canRequest && a_document.has_saved_destination()))
        {
            m_workflowRequest = EditorWorkflowRequest::ReloadScene;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("終了", nullptr, false, canRequest))
        {
            m_workflowRequest = EditorWorkflowRequest::CloseProject;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("編集"))
    {
        const bool canUndo = a_document.can_undo() && !a_pendingIntent.has_value();
        std::string undoLabel = "元に戻す";
        if (!a_document.undo_label().empty())
        {
            undoLabel.append(": ");
            undoLabel.append(display_text_label(a_document.undo_label()));
        }
        if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, canUndo))
        {
            a_pendingIntent.emplace(UndoIntent{});
        }

        const bool canRedo = a_document.can_redo() && !a_pendingIntent.has_value();
        std::string redoLabel = "やり直す";
        if (!a_document.redo_label().empty())
        {
            redoLabel.append(": ");
            redoLabel.append(display_text_label(a_document.redo_label()));
        }
        if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, canRedo))
        {
            a_pendingIntent.emplace(RedoIntent{});
        }
        ImGui::EndMenu();
    }
    const bool canUseShortcut =
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !ImGui::GetIO().WantTextInput;
    if (!m_workflowRequest.has_value() && !a_pendingIntent.has_value() && canUseShortcut &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N))
    {
        m_workflowRequest = EditorWorkflowRequest::NewScene;
    }
    if (!m_workflowRequest.has_value() && !a_pendingIntent.has_value() && canUseShortcut &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
    {
        m_workflowRequest = EditorWorkflowRequest::OpenScene;
    }
    if (!m_workflowRequest.has_value() && !a_pendingIntent.has_value() && canUseShortcut &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S))
    {
        m_workflowRequest = EditorWorkflowRequest::SaveSceneAs;
    }
    if (!m_workflowRequest.has_value() && !a_pendingIntent.has_value() && canUseShortcut &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
    {
        m_workflowRequest = EditorWorkflowRequest::SaveScene;
    }
    if (!a_pendingIntent.has_value() && canUseShortcut && a_document.can_undo() &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
    {
        a_pendingIntent.emplace(UndoIntent{});
    }
    if (!a_pendingIntent.has_value() && canUseShortcut && a_document.can_redo() &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y))
    {
        a_pendingIntent.emplace(RedoIntent{});
    }
    ImGui::EndMenuBar();
}

void EditorPresenter::draw_hierarchy(const editor_core::EditorDocument &a_document,
                                     std::optional<EditorIntent> &a_pendingIntent)
{
    const scene::SceneDocument &sceneDocument = a_document.scene_document();
    const scene::ObjectId *primarySelection = a_document.try_primary_selection();
    HierarchyChildIndex childrenByParent;
    childrenByParent.reserve(sceneDocument.object_count());
    for (const scene::SceneObject &object : sceneDocument.objects())
    {
        const scene::ObjectId *parentId = object.try_parent_id();
        if (parentId != nullptr)
        {
            childrenByParent[*parentId].push_back(&object);
        }
    }

    dock_editor_window_on_first_use();
    if (!ImGui::Begin("Hierarchy"))
    {
        ImGui::End();
        return;
    }

    const bool canAddObject = sceneDocument.object_count() < scene::k_maximumSceneObjectCount &&
                              (primarySelection == nullptr || hierarchy_depth(sceneDocument, *primarySelection) <
                                                                  scene::SceneDocument::maximum_hierarchy_depth());
    ImGui::BeginDisabled(!canAddObject);
    if (ImGui::Button("Objectを追加") && !a_pendingIntent.has_value())
    {
        const std::optional<scene::ObjectId> parentId =
            primarySelection != nullptr ? std::optional<scene::ObjectId>(*primarySelection) : std::nullopt;
        a_pendingIntent.emplace(AddObjectIntent{parentId, "GameObject"});
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::BeginCombo("Primitiveを追加", "選択してください", ImGuiComboFlags_NoPreview))
    {
        for (std::size_t templateIndex = 0U; templateIndex < m_primitiveTemplates.size(); ++templateIndex)
        {
            const EditorPrimitiveTemplate &primitiveTemplate = m_primitiveTemplates[templateIndex];
            const bool isValid = !primitiveTemplate.displayName.empty() && !primitiveTemplate.assetId.empty() &&
                                 primitiveTemplate.meshPrototype.try_known() != nullptr &&
                                 primitiveTemplate.meshPrototype.is_valid();
            const bool canCreate =
                canAddObject && !a_pendingIntent.has_value() && primitiveTemplate.isEnabled && isValid;
            ImGui::PushID(primitiveTemplate.assetId.data(),
                          primitiveTemplate.assetId.data() + primitiveTemplate.assetId.size());
            ImGui::BeginDisabled(!canCreate);
            std::string label = display_text_label(primitiveTemplate.displayName);
            label.append(" [");
            label.append(display_text_label(primitiveTemplate.assetId));
            label.push_back(']');
            if (ImGui::Selectable(label.c_str()) && canCreate)
            {
                const std::optional<scene::ObjectId> parentId =
                    primarySelection != nullptr ? std::optional<scene::ObjectId>(*primarySelection) : std::nullopt;
                a_pendingIntent.emplace(CreatePrimitiveIntent{parentId, templateIndex});
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canCreate &&
                !primitiveTemplate.unavailableReason.empty())
            {
                ImGui::SetTooltip("%s", primitiveTemplate.unavailableReason.c_str());
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const scene::SceneObject *primaryObject =
        primarySelection != nullptr ? sceneDocument.find_object(*primarySelection) : nullptr;
    const std::optional<SubtreeDuplicateInfo> duplicateInfo =
        primaryObject != nullptr
            ? std::optional<SubtreeDuplicateInfo>(subtree_duplicate_info(*primaryObject, childrenByParent))
            : std::nullopt;
    const bool canDuplicate =
        duplicateInfo.has_value() && duplicateInfo->allComponentsDuplicable &&
        duplicateInfo->objectCount <= scene::k_maximumSceneObjectCount - sceneDocument.object_count();
    ImGui::BeginDisabled(!canDuplicate);
    if (ImGui::Button("複製") && !a_pendingIntent.has_value())
    {
        a_pendingIntent.emplace(DuplicateObjectIntent{*primarySelection});
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(primarySelection == nullptr);
    if (ImGui::Button("削除") && !a_pendingIntent.has_value())
    {
        a_pendingIntent.emplace(DeleteObjectIntent{*primarySelection});
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    HierarchySelectionIndex selectionIndex;
    selectionIndex.reserve(a_document.selection().size());
    selectionIndex.insert(a_document.selection().begin(), a_document.selection().end());

    for (const scene::SceneObject &object : sceneDocument.objects())
    {
        if (object.try_parent_id() == nullptr)
        {
            draw_object_node(object, childrenByParent, selectionIndex, a_document.selection(), primarySelection,
                             a_pendingIntent);
        }
    }
    ImGui::End();
}

void EditorPresenter::draw_inspector(const editor_core::EditorDocument &a_document,
                                     std::optional<EditorIntent> &a_pendingIntent)
{
    dock_editor_window_on_first_use();
    if (!ImGui::Begin("Inspector"))
    {
        ImGui::End();
        return;
    }
    const scene::ObjectId *primarySelection = a_document.try_primary_selection();
    if (primarySelection == nullptr)
    {
        m_inspectorObjectId.reset();
        m_inspectorStateValue = 0U;
        m_nameDirty = false;
        m_translationDirty.fill(false);
        m_rotationDirty.fill(false);
        m_scaleDirty.fill(false);
        ImGui::TextDisabled("HierarchyからObjectを選択してください。");
        ImGui::End();
        return;
    }

    const scene::SceneDocument &sceneDocument = a_document.scene_document();
    const scene::SceneObject *object = sceneDocument.find_object(*primarySelection);
    if (object == nullptr)
    {
        m_inspectorObjectId.reset();
        m_inspectorStateValue = 0U;
        m_nameDirty = false;
        m_translationDirty.fill(false);
        m_rotationDirty.fill(false);
        m_scaleDirty.fill(false);
        ImGui::TextUnformatted("選択ObjectがSceneに存在しません。");
        ImGui::End();
        return;
    }
    sync_inspector(a_document, *object);

    bool commitName = false;
    if (m_name.find('\0') != std::string::npos)
    {
        ImGui::TextDisabled("NameにU+0000が含まれるためInspectorでは編集できません（%zu bytes）。", m_name.size());
    }
    else
    {
        commitName = input_object_name(m_name, *m_assertContext);
        m_nameDirty = m_nameDirty || ImGui::IsItemEdited();
        if (m_name == object->name())
        {
            m_nameDirty = false;
        }
        commitName = commitName || ImGui::IsItemDeactivatedAfterEdit();
        bool usedUnicodeEscape = false;
        const std::string namePreview = display_text_label(m_name, &usedUnicodeEscape);
        if (usedUnicodeEscape)
        {
            ImGui::TextDisabled("識別表示（Font非対応文字はUnicode Escape）");
            ImGui::TextUnformatted(namePreview.data(), namePreview.data() + namePreview.size());
        }
    }
    if (commitName)
    {
        const bool hasHistoryIntent =
            a_pendingIntent.has_value() && (std::holds_alternative<UndoIntent>(*a_pendingIntent) ||
                                            std::holds_alternative<RedoIntent>(*a_pendingIntent));
        if (!hasHistoryIntent && a_pendingIntent.has_value())
        {
            // Hierarchy操作よりName確定を優先し、成功後の次FrameまでScene遷移を遅延する
            m_deferredIntent.emplace(std::move(*a_pendingIntent));
        }
        if (!hasHistoryIntent)
        {
            a_pendingIntent.emplace(RenameObjectIntent{object->id(), m_name});
        }
    }
    const auto queueInspectorIntent = [this, &a_pendingIntent](EditorIntent a_intent)
    {
        if (!a_pendingIntent.has_value())
        {
            a_pendingIntent.emplace(std::move(a_intent));
        }
        else if (std::holds_alternative<RenameObjectIntent>(*a_pendingIntent) && !m_deferredIntent.has_value())
        {
            // Name確定を当Frameで適用し、同じClickが生成した後続操作を次Frameまで保持する
            m_deferredIntent.emplace(std::move(a_intent));
        }
    };
    const auto setInspectorError = [this, &a_pendingIntent](const Error &a_error) noexcept
    {
        set_error(a_error);
        m_preserveInspectorErrorAfterIntent =
            a_pendingIntent.has_value() && std::holds_alternative<RenameObjectIntent>(*a_pendingIntent);
    };

    const scene::ObjectId *parentId = object->try_parent_id();
    std::string parentPreview = "Scene Root";
    if (parentId != nullptr)
    {
        const scene::SceneObject *parent = sceneDocument.find_object(*parentId);
        parentPreview = parent != nullptr ? object_reference_label(*parent) : "Missing Parent";
    }
    if (ImGui::BeginCombo("Parent", parentPreview.c_str()))
    {
        const std::size_t selectedSubtreeHeight = subtree_height(sceneDocument, object->id());
        if (ImGui::Selectable("Scene Root", parentId == nullptr))
        {
            queueInspectorIntent(ReparentObjectIntent{object->id(), std::nullopt});
        }
        for (const scene::SceneObject &candidate : sceneDocument.objects())
        {
            if (is_in_subtree(sceneDocument, candidate.id(), object->id()))
            {
                continue;
            }
            const std::size_t targetDepth = hierarchy_depth(sceneDocument, candidate.id()) + 1U;
            const std::size_t maximumDepth = scene::SceneDocument::maximum_hierarchy_depth();
            if (targetDepth > maximumDepth || selectedSubtreeHeight > maximumDepth - targetDepth + 1U)
            {
                continue;
            }
            const scene::IdentityText candidateId = candidate.id().canonical_text();
            ImGui::PushID(candidateId.data(), candidateId.data() + candidateId.size());
            const bool isCurrent = parentId != nullptr && *parentId == candidate.id();
            const std::string candidateName = object_reference_label(candidate);
            if (ImGui::Selectable(candidateName.c_str(), isCurrent))
            {
                queueInspectorIntent(ReparentObjectIntent{object->id(), candidate.id()});
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    ImGui::SeparatorText("Core Transform");
    const std::array<float, 3> previousTranslation = m_translation;
    const std::array<float, 4> previousRotation = m_rotation;
    const std::array<float, 3> previousScale = m_scale;
    static_cast<void>(ImGui::InputFloat3("Translation", m_translation.data()));
    static_cast<void>(ImGui::InputFloat4("Rotation (Quaternion)", m_rotation.data()));
    static_cast<void>(ImGui::InputFloat3("Scale", m_scale.data()));
    const math::Vector3 authoritativeTranslation = object->transform().translation();
    const math::Quaternion authoritativeRotation = object->transform().rotation();
    const math::Vector3 authoritativeScale = object->transform().scale();
    update_component_dirty_state(
        previousTranslation, m_translation,
        std::array{authoritativeTranslation.x, authoritativeTranslation.y, authoritativeTranslation.z},
        m_translationDirty);
    update_component_dirty_state(
        previousRotation, m_rotation,
        std::array{authoritativeRotation.x, authoritativeRotation.y, authoritativeRotation.z, authoritativeRotation.w},
        m_rotationDirty);
    update_component_dirty_state(previousScale, m_scale,
                                 std::array{authoritativeScale.x, authoritativeScale.y, authoritativeScale.z},
                                 m_scaleDirty);
    if (ImGui::Button("Transformを適用"))
    {
        Result<math::Tolerance> tolerance =
            math::Tolerance::create(m_assertContext->fatal_handler(), 0.00001F, 0.00001F);
        if (!tolerance)
        {
            setInspectorError(*tolerance.try_error());
        }
        else
        {
            Result<math::Quaternion> rotation = math::normalize(
                m_assertContext->fatal_handler(),
                math::Quaternion{m_rotation[0], m_rotation[1], m_rotation[2], m_rotation[3]}, *tolerance.try_value());
            if (!rotation)
            {
                setInspectorError(*rotation.try_error());
            }
            else
            {
                Result<math::Transform> transform = math::Transform::create(
                    m_assertContext->fatal_handler(),
                    math::Vector3{m_translation[0], m_translation[1], m_translation[2]}, *rotation.try_value(),
                    math::Vector3{m_scale[0], m_scale[1], m_scale[2]}, *tolerance.try_value());
                if (!transform)
                {
                    setInspectorError(*transform.try_error());
                }
                else
                {
                    queueInspectorIntent(EditTransformIntent{object->id(), std::move(*transform.try_value())});
                }
            }
        }
    }

    ImGui::SeparatorText("Components");
    for (const scene::SceneComponent &component : object->components())
    {
        const scene::IdentityText componentId = component_id_text(component.instance_id());
        ImGui::PushID(componentId.data(), componentId.data() + componentId.size());
        const std::string label = component_label(component, *m_schemaRegistry, *m_assertContext);
        if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            const scene::KnownComponentData *known = component.try_known();
            if (known != nullptr)
            {
                const schema::TypeDescriptor *descriptor = nullptr;
                Result<const schema::TypeDescriptor *> found =
                    m_schemaRegistry->find(known->type_id(), *m_assertContext);
                if (found)
                {
                    descriptor = *found.try_value();
                }
                for (const scene::KnownFieldData &field : known->known_fields())
                {
                    const std::string fieldName = field_label(descriptor, field.id(), *m_assertContext);
                    ImGui::TextUnformatted(fieldName.c_str());
                    ImGui::SameLine();
                    draw_field_value(field.value());
                }
                if (!known->unknown_fields().empty())
                {
                    ImGui::TextDisabled("未知Field %zu件は保持されますが編集できません。",
                                        known->unknown_fields().size());
                }
            }
            else
            {
                ImGui::TextDisabled("未知Component Dataは保持されますが編集できません。");
            }
            if (ImGui::Button("Componentを削除"))
            {
                queueInspectorIntent(RemoveComponentIntent{object->id(), component.instance_id()});
            }
        }
        ImGui::PopID();
    }

    if (object->components().size() >= scene::k_maximumSceneComponentsPerObject)
    {
        ImGui::TextDisabled("Component数がScene上限に達しています。");
    }
    else if (m_componentTemplates.empty())
    {
        ImGui::TextDisabled("追加可能なComponentは登録されていません。");
    }
    else if (ImGui::BeginCombo("Componentを追加", "選択してください"))
    {
        for (std::size_t templateIndex = 0U; templateIndex < m_componentTemplates.size(); ++templateIndex)
        {
            const EditorComponentTemplate &componentTemplate = m_componentTemplates[templateIndex];
            const std::optional<schema::TypeId> typeId = component_type_id(componentTemplate.prototype);
            const bool canAddTemplate = typeId.has_value() && componentTemplate.prototype.try_known() != nullptr &&
                                        componentTemplate.prototype.is_valid();
            std::string templateIdentity;
            if (typeId.has_value())
            {
                templateIdentity = type_id_text(*typeId);
                templateIdentity.push_back(':');
                templateIdentity.append(std::to_string(templateIndex));
                ImGui::PushID(templateIdentity.data(), templateIdentity.data() + templateIdentity.size());
            }
            else
            {
                ImGui::PushID(&componentTemplate);
            }
            ImGui::BeginDisabled(!canAddTemplate);
            std::string componentName = display_text_label(componentTemplate.displayName);
            componentName.append(" [");
            componentName.append(typeId.has_value() ? type_id_text(*typeId) : "Invalid Type");
            componentName.append(" / Template ");
            componentName.append(std::to_string(templateIndex));
            componentName.push_back(']');
            if (ImGui::Selectable(componentName.c_str()) && canAddTemplate)
            {
                queueInspectorIntent(AddComponentIntent{object->id(), *typeId, templateIndex});
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    const bool hasHistoryIntent =
        a_pendingIntent.has_value() &&
        (std::holds_alternative<UndoIntent>(*a_pendingIntent) || std::holds_alternative<RedoIntent>(*a_pendingIntent));
    if (a_pendingIntent.has_value() && !std::holds_alternative<RenameObjectIntent>(*a_pendingIntent) &&
        !hasHistoryIntent && m_name.find('\0') == std::string::npos && m_name != object->name())
    {
        // 後続Inspector操作よりName確定を優先し、成功後の次Frameまで元の操作を遅延する
        m_deferredIntent.emplace(std::move(*a_pendingIntent));
        a_pendingIntent.emplace(RenameObjectIntent{object->id(), m_name});
    }
    ImGui::End();
}

void EditorPresenter::sync_inspector(const editor_core::EditorDocument &a_document, const scene::SceneObject &a_object)
{
    const std::uint64_t stateValue = a_document.current_state_id().value();
    const bool isSameObject = m_inspectorObjectId.has_value() && *m_inspectorObjectId == a_object.id();
    if (isSameObject && m_inspectorStateValue == stateValue)
    {
        return;
    }

    if (!isSameObject || !m_nameDirty)
    {
        m_name.assign(a_object.name());
        m_nameDirty = false;
    }
    const math::Vector3 translation = a_object.transform().translation();
    const math::Quaternion rotation = a_object.transform().rotation();
    const math::Vector3 scale = a_object.transform().scale();
    sync_clean_components(m_translation, m_translationDirty, std::array{translation.x, translation.y, translation.z},
                          isSameObject);
    sync_clean_components(m_rotation, m_rotationDirty, std::array{rotation.x, rotation.y, rotation.z, rotation.w},
                          isSameObject);
    sync_clean_components(m_scale, m_scaleDirty, std::array{scale.x, scale.y, scale.z}, isSameObject);
    m_inspectorObjectId = a_object.id();
    m_inspectorStateValue = stateValue;
}

void EditorPresenter::set_error(const Error &a_error) noexcept
{
    const ErrorCode &code = a_error.code();
    const ErrorCode &rootCode = a_error.root_code();
    const char *message = "編集操作に失敗しました。入力内容とLogを確認してください。";
    if (code.domain() == "Cue.EditorCore")
    {
        switch (static_cast<editor_core::EditorCoreError>(code.value()))
        {
        case editor_core::EditorCoreError::DocumentNotFound:
            message = "編集対象のScene Documentが見つかりません。";
            break;
        case editor_core::EditorCoreError::UndoUnavailable:
            message = "元に戻せる編集履歴がありません。";
            break;
        case editor_core::EditorCoreError::RedoUnavailable:
            message = "やり直せる編集履歴がありません。";
            break;
        case editor_core::EditorCoreError::InvalidDocumentState:
            message = "現在のDocument状態では編集できません。";
            break;
        default:
            break;
        }
    }
    if (rootCode.domain() == "Cue.Scene")
    {
        switch (static_cast<scene::SceneError>(rootCode.value()))
        {
        case scene::SceneError::InvalidName:
            message = "Object名は空でない有効なUTF-8文字列にしてください。";
            break;
        case scene::SceneError::ObjectNotFound:
            message = "対象ObjectがSceneに存在しません。Hierarchyを更新してください。";
            break;
        case scene::SceneError::HierarchyCycle:
            message = "ChildをParentに指定するとHierarchyが循環するため変更できません。";
            break;
        case scene::SceneError::HierarchyDepthExceeded:
            message = "Hierarchyの深さ上限を超えるため変更できません。";
            break;
        case scene::SceneError::DuplicateComponentId:
            message = "Component Identityが重複したため追加できません。";
            break;
        case scene::SceneError::ComponentNotFound:
            message = "対象Componentが見つかりません。Inspectorを更新してください。";
            break;
        case scene::SceneError::UnknownSchemaType:
            message = "追加するComponent Templateが登録されていません。";
            break;
        case scene::SceneError::UnsupportedComponentOperation:
            message = "未知Componentは安全に複製できないため、この操作を実行できません。";
            break;
        default:
            break;
        }
    }

    try
    {
        m_message.assign(message);
        m_message.append("\n診断: ");
        m_message.append(rootCode.domain());
        m_message.push_back('/');
        m_message.append(std::to_string(rootCode.value()));
        m_message.append(" - ");
        m_message.append(a_error.summary());
        m_hasError = true;
    }
    catch (...)
    {
        terminate_allocation(*m_assertContext);
    }
}

void EditorPresenter::set_status(std::string_view a_status) noexcept
{
    try
    {
        m_message.assign(a_status);
        m_hasError = false;
    }
    catch (...)
    {
        terminate_allocation(*m_assertContext);
    }
}

std::string_view EditorPresenter::message() const noexcept
{
    return m_message;
}

bool EditorPresenter::has_error_message() const noexcept
{
    return m_hasError;
}
} // namespace cue::editor
