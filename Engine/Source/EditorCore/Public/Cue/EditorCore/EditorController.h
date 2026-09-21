#pragma once

#include <Cue/EditorCore/EditorDocument.h>
#include <Cue/EditorCore/EditorIntent.h>
#include <Cue/EditorCore/Persistence.h>
#include <Cue/EditorCore/SceneCommand.h>
#include <Cue/Foundation/Result.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Scene/Serialization.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::editor_core
{
class FilesWorkspaceService;

/// @brief 一つの Project Descriptor と開いた EditorDocument 群の一意 Owner
class ProjectWorkspaceSession final
{
  public:
    /// @brief Project Workspace Session の一意所有を保つため Copy 構築を禁止する
    ProjectWorkspaceSession(const ProjectWorkspaceSession &) = delete;
    /// @brief Project Workspace Session の一意所有を保つため Copy 代入を禁止する
    ProjectWorkspaceSession &operator=(const ProjectWorkspaceSession &) = delete;
    /// @brief Project Workspace Session の所有状態を移動する
    ProjectWorkspaceSession(ProjectWorkspaceSession &&) noexcept = default;
    /// @brief Project Workspace Session の所有状態を移動代入する
    ProjectWorkspaceSession &operator=(ProjectWorkspaceSession &&) noexcept = default;
    /// @brief Project Workspace Session の所有 Data を破棄する
    ~ProjectWorkspaceSession() noexcept = default;

    /// @brief Session 破棄まで有効な Project Descriptor を返す
    [[nodiscard]] const ProjectDescriptor &project_descriptor() const noexcept;
    /// @brief 次の Controller Mutation または Controller 破棄まで有効な Document View を返す
    [[nodiscard]] std::span<const EditorDocument> documents() const noexcept;
    /// @brief Identity に対応し次の Controller Mutation または破棄まで有効な Read-only Document を返す
    [[nodiscard]] const EditorDocument *find_document(EditorDocumentId a_id) const noexcept;

  private:
    friend class EditorController;

    /// @brief 検証済み Project Descriptor の所有権を Session へ移す
    explicit ProjectWorkspaceSession(ProjectDescriptor &&a_descriptor) noexcept;

    ProjectDescriptor m_descriptor;
    std::vector<EditorDocument> m_documents;
};

/// @brief Editor Intent を UI 非依存 Document 状態遷移へ変換する Owner Thread Controller
///
/// Controller、返した Session View、Document View は作成 Thread だけで使用する
class EditorController final
{
  private:
    friend class FilesWorkspaceService;

    /// @brief make_unique 経由の Factory 構築だけを許可する非公開 Key
    struct ConstructionKey final
    {
      private:
        friend class EditorController;

        /// @brief EditorController Factory だけが生成できる構築 Key を作る
        ConstructionKey() noexcept
        {
        }
    };

  public:
    /// @brief Editor Controller の一意所有を保つため Copy 構築を禁止する
    EditorController(const EditorController &) = delete;
    /// @brief Editor Controller の一意所有を保つため Copy 代入を禁止する
    EditorController &operator=(const EditorController &) = delete;
    /// @brief Owner Thread と Session Identity を固定するため Move 構築を禁止する
    EditorController(EditorController &&) = delete;
    /// @brief Owner Thread と Session Identity を固定するため Move 代入を禁止する
    EditorController &operator=(EditorController &&) = delete;
    /// @brief Owner Thread 上で Controller 所有 Data を破棄する
    ~EditorController() noexcept;

    /// @brief Project Descriptor の所有権を受け取って空 Workspace Session を生成する
    /// @param a_assertContext Controller より長く生存させる診断 Context
    [[nodiscard]] static std::unique_ptr<EditorController> create(ProjectDescriptor &&a_descriptor,
                                                                  const AssertContext &a_assertContext) noexcept;
    /// @brief Scene Save／Reload／Recovery依存を束ねたProject Workspace Sessionを生成する
    [[nodiscard]] static std::unique_ptr<EditorController> create(ProjectDescriptor &&a_descriptor,
                                                                  ScenePersistenceServices a_persistenceServices,
                                                                  const AssertContext &a_assertContext) noexcept;

    /// @brief Factory Passkey と Project Descriptor から Controller を構築する
    EditorController(ConstructionKey, ProjectDescriptor &&a_descriptor, const AssertContext &a_assertContext);
    /// @brief Factory Passkey、Project Descriptor、Persistence依存からControllerを構築する
    EditorController(ConstructionKey, ProjectDescriptor &&a_descriptor, ScenePersistenceServices a_persistenceServices,
                     const AssertContext &a_assertContext);

    /// @brief Controller 破棄まで有効な Project Workspace Session の Read-only View を返す
    [[nodiscard]] const ProjectWorkspaceSession &session() const noexcept;

    /// @brief 検証済み SceneDocument を一意な Scene と Locator で開く
    [[nodiscard]] Result<EditorDocumentId> open_document(scene::SceneDocument &&a_document, RelativePath &&a_locator,
                                                         bool a_hasSavedDestination) noexcept;
    /// @brief Source Assets RootからSceneを完全Loadし、Base FingerprintとRecovery有無を記録して開く
    [[nodiscard]] Result<EditorDocumentId> open_document_from_storage(RelativePath a_locator) noexcept;
    /// @brief 現在のProject Descriptorと保存済みStartup Sceneを操作開始時に再読込して不変Snapshotを返す
    /// @details Project Rootが注入されている場合はDescriptorを一度再読込し、Root変更はProject再Open要求として拒否する
    [[nodiscard]] Result<scene::SceneSnapshot> load_saved_startup_scene_snapshot() noexcept;
    /// @brief Semantic Intentを検証し、Scene CommandまたはEditor Workflowへ一元変換する
    /// @param a_identitySource 新規Object／ComponentへStable Identity候補を供給する注入境界
    /// @param a_componentTemplates Add Componentで使用可能な検証済み初期値Template
    /// @param a_primitiveTemplates Create Primitiveで使用可能なCatalog由来Template
    [[nodiscard]] Result<void> execute_intent(
        EditorDocumentId a_documentId, EditorIntent a_intent, scene::SceneIdentitySource &a_identitySource,
        std::span<const EditorComponentTemplate> a_componentTemplates,
        std::span<const EditorPrimitiveTemplate> a_primitiveTemplates = {}) noexcept;
    /// @brief Stable Identity だけを保持する Scene 編集 Command を検証し一つの Revision として適用する
    /// @details Open 中の対象 Document と Scene Identity の一致を要求し、失敗時は Authoring Scene、Selection、
    /// Revision を呼び出し前の状態に維持する。同値更新は現在 Revision を返し新しい State を発行しない
    [[nodiscard]] Result<DocumentStateId> execute_command(SceneCommandRequest a_request) noexcept;
    /// @brief 一つ以上のCommandを一つのRevisionとHistory EntryとしてAtomicに適用する
    [[nodiscard]] Result<DocumentStateId> execute_transaction(EditorTransaction a_transaction) noexcept;
    /// @brief 直前TransactionのBefore CheckpointとStateを復元する
    [[nodiscard]] Result<DocumentStateId> undo(EditorDocumentId a_documentId) noexcept;
    /// @brief Undo済みTransactionのAfter CheckpointとStateを再適用する
    [[nodiscard]] Result<DocumentStateId> redo(EditorDocumentId a_documentId) noexcept;
    /// @brief 現在Locatorへ外部競合を検査してAtomic Saveする
    [[nodiscard]] Result<scene::SceneSaveOutcome> save_document(EditorDocumentId a_documentId) noexcept;
    /// @brief 指定Locatorを事前検査し、Committed時だけDocumentの正本Locatorへ切り替える
    [[nodiscard]] Result<scene::SceneSaveOutcome> save_document_as(EditorDocumentId a_documentId,
                                                                   RelativePath a_locator) noexcept;
    /// @brief 未作成Locatorだけを許可し、競合なしでDocumentの正本Locatorへ切り替える
    [[nodiscard]] Result<scene::SceneSaveOutcome> save_document_as_new(EditorDocumentId a_documentId,
                                                                       RelativePath a_locator) noexcept;
    /// @brief 開いているDirty Documentを順番に保存し、各Atomic Save状態を返す
    [[nodiscard]] Result<std::vector<scene::SceneSaveStatus>> save_all_documents() noexcept;
    /// @brief Save UncertainのCandidateを結果種別に応じて再検証または再保存する
    [[nodiscard]] Result<scene::SceneSaveStatus> retry_uncertain_save(EditorDocumentId a_documentId) noexcept;
    /// @brief Save Uncertain Recordを明示破棄し、現在DocumentのDirty状態は維持する
    [[nodiscard]] Result<void> discard_uncertain_save(EditorDocumentId a_documentId) noexcept;
    /// @brief 現在Fileを一時Documentへ完全LoadしてからHistoryを破棄して入れ替える
    [[nodiscard]] Result<DocumentStateId> reload_document(EditorDocumentId a_documentId) noexcept;
    /// @brief 正本Fingerprintとの差を検査してExternal Change状態を更新する
    [[nodiscard]] Result<ExternalChangeState> poll_external_change(EditorDocumentId a_documentId) noexcept;
    /// @brief Dirty SceneをSaved RootのVersion付きRecovery EnvelopeへAtomic保存する
    [[nodiscard]] Result<void> autosave_recovery(EditorDocumentId a_documentId) noexcept;
    /// @brief Recovery Registryから現在Projectの検証済み候補を列挙する
    [[nodiscard]] Result<std::vector<RecoveryCandidateInspection>> list_recovery_candidates() noexcept;
    /// @brief Scene Identityに対応するRecovery EnvelopeからDirty Documentを直接開く
    [[nodiscard]] Result<EditorDocumentId> open_document_from_recovery(std::string_view a_sceneId) noexcept;
    /// @brief Documentに対応するRecovery Envelopeを検証してMetadataを返す
    [[nodiscard]] Result<RecoveryMetadata> inspect_recovery(EditorDocumentId a_documentId) noexcept;
    /// @brief 検証済みRecovery本文をDirtyな新StateとしてDocumentへ適用する
    [[nodiscard]] Result<DocumentStateId> recover_document(EditorDocumentId a_documentId) noexcept;
    /// @brief Recovery Fileを変更せず、このSessionでの候補表示だけを解除する
    [[nodiscard]] Result<void> ignore_recovery(EditorDocumentId a_documentId) noexcept;
    /// @brief Recovery Fileを永続的に削除し、成功時だけ候補表示を解除する
    [[nodiscard]] Result<void> discard_recovery(EditorDocumentId a_documentId) noexcept;
    /// @brief Stable ObjectId だけを保持し、存在しない ID と重複を安全に除く
    [[nodiscard]] Result<void> set_selection(EditorDocumentId a_documentId,
                                             std::span<const scene::ObjectId> a_objectIds,
                                             const scene::ObjectId *a_primaryObjectId = nullptr) noexcept;
    /// @brief Selection と Primary Selection を空にする
    [[nodiscard]] Result<void> clear_selection(EditorDocumentId a_documentId) noexcept;
    /// @brief 現在 Scene に存在しない選択 ID を除去する
    [[nodiscard]] Result<void> reconcile_selection(EditorDocumentId a_documentId) noexcept;
    /// @brief History外で成功した永続 Data 変更に一つの新しい State Identity を発行する
    /// @details Checkpointを持たない変更から古い状態へ遷移しないよう、成功時は既存Historyを破棄する
    [[nodiscard]] Result<DocumentStateId> record_persistent_change(EditorDocumentId a_documentId) noexcept;
    /// @brief 確定保存された発行済み State を保存地点として記録する
    /// @details SaveRequested 中は Clean かつ外部変更なしなら Close し、それ以外は AwaitingDecision へ戻す
    [[nodiscard]] Result<void> mark_saved(EditorDocumentId a_documentId, DocumentStateId a_savedStateId) noexcept;
    /// @brief Close Save の失敗を通知して利用者判断待ちへ戻す
    /// @details SaveRequested 以外では InvalidCloseTransition を返し、Document 状態を変更しない
    [[nodiscard]] Result<DocumentCloseState> report_save_failure(EditorDocumentId a_documentId) noexcept;
    /// @brief Scene 正本に対する外部変更の観測状態を更新する
    [[nodiscard]] Result<void> set_external_change_state(EditorDocumentId a_documentId,
                                                         ExternalChangeState a_state) noexcept;
    /// @brief Document Close を開始し、明示判断の要否を状態として返す
    [[nodiscard]] Result<DocumentCloseState> request_close(EditorDocumentId a_documentId) noexcept;
    /// @brief AwaitingDecision 中の Save、Discard、Cancel 判断を適用する
    /// @details 外部変更中の Save は InvalidCloseTransition を返し、AwaitingDecision を維持する
    [[nodiscard]] Result<DocumentCloseState> respond_to_close(EditorDocumentId a_documentId,
                                                              CloseDecision a_decision) noexcept;

  private:
    /// @brief Document Identity に対応する可変 Document を返す
    [[nodiscard]] EditorDocument *find_document(EditorDocumentId a_id) noexcept;
    /// @brief Closed Document の所有 Data を Session から解放する
    void erase_closed_document(EditorDocumentId a_id) noexcept;
    /// @brief 永続変更のStateを発行し、要求された場合はCheckpointを持たない既存Historyを破棄する
    [[nodiscard]] Result<DocumentStateId> issue_persistent_state(EditorDocumentId a_documentId,
                                                                 bool a_invalidateHistory) noexcept;
    /// @brief Persistence依存が注入済みであることを検証する
    [[nodiscard]] Result<void> require_persistence_services() const noexcept;
    /// @brief 指定Destinationへ競合検査付きSaveを実行する共通経路
    [[nodiscard]] Result<scene::SceneSaveOutcome> save_document_to(EditorDocumentId a_documentId,
                                                                   RelativePath a_locator, bool a_switchDestination,
                                                                   bool a_requireMissingDestination) noexcept;
    /// @brief 現在 Thread が Controller 作成 Thread であることを全構成で検証する
    void assert_owner_thread() const noexcept;
    /// @brief Allocation 失敗を Fatal 境界へ変換する
    [[noreturn]] void terminate_allocation() const noexcept;
    /// @brief 予期しない例外を Fatal 境界へ変換する
    [[noreturn]] void terminate_exception() const noexcept;

    std::shared_ptr<const DocumentStateOrigin> m_stateOrigin;
    ProjectWorkspaceSession m_session;
    const AssertContext *m_assertContext;
    std::thread::id m_ownerThread;
    std::uint64_t m_nextDocumentId = 1U;
    FilesystemRoot *m_projectRoot = nullptr;
    FilesystemRoot *m_sourceAssetsRoot = nullptr;
    FilesystemRoot *m_savedRoot = nullptr;
    const schema::SchemaRegistry *m_schemaRegistry = nullptr;
    const scene::ComponentValueSchemaRegistry *m_valueSchemaRegistry = nullptr;
    const scene::SceneMigrationRegistry *m_sceneMigrations = nullptr;
    const scene::ComponentMigrationRegistry *m_componentMigrations = nullptr;
};
} // namespace cue::editor_core
