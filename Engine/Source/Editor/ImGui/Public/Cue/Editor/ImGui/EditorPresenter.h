#pragma once

#include <Cue/EditorCore/EditorController.h>
#include <Cue/EditorCore/EditorIntent.h>
#include <Cue/Schema/Registry.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::editor
{
/// @brief Editor Shellへ返すDocument外Workflow要求
enum class EditorWorkflowRequest : std::uint8_t
{
    NewScene,
    OpenScene,
    SaveScene,
    SaveSceneAs,
    ReloadScene,
    CloseProject,
};

/// @brief EditorControllerのRead-only ViewをHierarchy・Inspector操作へ変換するPresentation Adapter
///
/// Controller、Identity Source、Schema Registry、Assert ContextはPresenterより長く生存させ、
/// 生成Threadだけでdrawとsubmitを呼び出す。Component／Primitive Template群はPresenterが所有する
class EditorPresenter final
{
  public:
    /// @brief Presentation Stateと注入参照の一意性を保つためCopy構築を禁止する
    EditorPresenter(const EditorPresenter &) = delete;
    /// @brief Presentation Stateと注入参照の一意性を保つためCopy代入を禁止する
    EditorPresenter &operator=(const EditorPresenter &) = delete;
    /// @brief ImGui Session中のAddress安定性を保つためMove構築を禁止する
    EditorPresenter(EditorPresenter &&) = delete;
    /// @brief ImGui Session中のAddress安定性を保つためMove代入を禁止する
    EditorPresenter &operator=(EditorPresenter &&) = delete;
    /// @brief Presentation Stateを破棄し、注入Objectの所有権は変更しない
    ~EditorPresenter() = default;

    /// @brief 一つのOpen Documentへ結び付くHierarchy・Inspector Adapterを生成する
    /// @param a_componentTemplates Presenterが所有権を受け取りAdd Component候補として保持する
    /// @param a_primitiveTemplates Presenterが所有権を受け取りCreate Primitive候補として保持する
    [[nodiscard]] static std::unique_ptr<EditorPresenter> create(
        editor_core::EditorController &a_controller, editor_core::EditorDocumentId a_documentId,
        scene::SceneIdentitySource &a_identitySource, const schema::SchemaRegistry &a_schemaRegistry,
        std::vector<editor_core::EditorComponentTemplate> a_componentTemplates,
        std::vector<editor_core::EditorPrimitiveTemplate> a_primitiveTemplates,
        const AssertContext &a_assertContext) noexcept;

    /// @brief 現在DocumentのRead-only ViewからHierarchy・Inspectorを描画し、Frame末尾で最大一Intentを適用する
    void draw() noexcept;

    /// @brief 現在FrameまでにFile Menuから発行されたWorkflow要求を一度だけ返す
    [[nodiscard]] std::optional<EditorWorkflowRequest> take_workflow_request() noexcept;

    /// @brief Workflow層の回復可能な失敗をPresenter診断へ反映する
    void report_workflow_error(const Error &a_error) noexcept;
    /// @brief Workflow層の成功状態をPresenter Messageへ反映する
    void report_workflow_status(std::string_view a_status) noexcept;

    /// @brief 意味IntentをEditorControllerの一元実行入口へ渡して結果をPresentation Messageへ反映する
    /// @details 失敗時はAuthoring SceneをControllerのRollback規則に従って維持し、Errorと日本語診断を返す
    [[nodiscard]] Result<void> submit(editor_core::EditorIntent a_intent) noexcept;

    /// @brief 最後の操作結果または診断Messageを返す
    [[nodiscard]] std::string_view message() const noexcept;
    /// @brief 現在Messageが回復可能な操作失敗を表すか返す
    [[nodiscard]] bool has_error_message() const noexcept;

  private:
    /// @brief 注入依存と初期Presentation Stateを保持する
    EditorPresenter(editor_core::EditorController &a_controller, editor_core::EditorDocumentId a_documentId,
                    scene::SceneIdentitySource &a_identitySource, const schema::SchemaRegistry &a_schemaRegistry,
                    std::vector<editor_core::EditorComponentTemplate> a_componentTemplates,
                    std::vector<editor_core::EditorPrimitiveTemplate> a_primitiveTemplates,
                    const AssertContext &a_assertContext) noexcept;

    /// @brief Undo／Redo MenuとShortcutを描画して意味Intentを予約する
    void draw_menu(const editor_core::EditorDocument &a_document,
                   std::optional<editor_core::EditorIntent> &a_pendingIntent);
    /// @brief Stable ObjectIdをImGui IDに使用してHierarchy TreeとObject操作を描画する
    void draw_hierarchy(const editor_core::EditorDocument &a_document,
                        std::optional<editor_core::EditorIntent> &a_pendingIntent);
    /// @brief Primary SelectionのName、Parent、Transform、Component操作を描画する
    void draw_inspector(const editor_core::EditorDocument &a_document,
                        std::optional<editor_core::EditorIntent> &a_pendingIntent);
    /// @brief SelectionまたはDocument State変更時に編集BufferをRead-only正本へ同期する
    void sync_inspector(const editor_core::EditorDocument &a_document, const scene::SceneObject &a_object);
    /// @brief Error分類と開発者診断を日本語の回復可能Messageへ変換する
    void set_error(const Error &a_error) noexcept;
    /// @brief 成功した操作の短いMessageを設定する
    void set_status(std::string_view a_status) noexcept;

    editor_core::EditorController *m_controller;
    scene::SceneIdentitySource *m_identitySource;
    const schema::SchemaRegistry *m_schemaRegistry;
    const AssertContext *m_assertContext;
    std::vector<editor_core::EditorComponentTemplate> m_componentTemplates;
    std::vector<editor_core::EditorPrimitiveTemplate> m_primitiveTemplates;
    editor_core::EditorDocumentId m_documentId;
    std::optional<editor_core::EditorIntent> m_deferredIntent;
    std::optional<EditorWorkflowRequest> m_workflowRequest;
    std::optional<scene::ObjectId> m_inspectorObjectId;
    std::uint64_t m_inspectorStateValue = 0U;
    std::string m_name;
    std::array<float, 3> m_translation{};
    std::array<float, 4> m_rotation{};
    std::array<float, 3> m_scale{1.0F, 1.0F, 1.0F};
    bool m_nameDirty = false;
    std::array<bool, 3> m_translationDirty{};
    std::array<bool, 4> m_rotationDirty{};
    std::array<bool, 3> m_scaleDirty{};
    std::string m_message;
    bool m_hasError = false;
    bool m_preserveInspectorErrorAfterIntent = false;
};
} // namespace cue::editor
