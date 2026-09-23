#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Project/Compatibility.h>
#include <Cue/Project/Generator.h>
#include <Cue/Project/Registry.h>

#include <cstddef>
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
class FilesystemRoot;
} // namespace cue

namespace cue::project_hub
{
inline constexpr std::uint32_t k_editorLaunchProtocolVersion = cue::k_editorLaunchProtocolVersion;
inline constexpr std::string_view k_blank3dTemplateId = "cue.blank-3d";

/// @brief 一覧更新時にProject Folderから取得した表示専用Storage Snapshot
struct ProjectStorageMetadata final
{
    std::uint64_t latestWriteMilliseconds;
    std::uint64_t byteSize;
};

/// @brief Project Hubが表示し選択する一つのInstalled Engine Version
struct InstalledEngineVersionView final
{
    std::string directoryName;
    std::string displayName;
    EngineVersion engineVersion;
    std::string bundleId;
    std::string manifestDigest;
    std::string diagnostic;
    bool isAvailable;
    bool isSelected;
};

/// @brief Editor起動直前にDistribution層で再検証するInstalled Version Identity
struct InstalledEngineLaunchIdentity final
{
    std::string versionDirectory;
    std::string bundleId;
    std::string manifestDigest;
};

/// @brief Stored Locator を開く Platform Composition 境界
class ProjectHubPlatform
{
  public:
    ProjectHubPlatform(const ProjectHubPlatform &) = delete;
    ProjectHubPlatform &operator=(const ProjectHubPlatform &) = delete;
    virtual ~ProjectHubPlatform() = default;

    /// @brief UI入力LocatorをProcess間受け渡し可能な絶対UTF-8 Locatorへ正規化する
    [[nodiscard]] virtual Result<std::string> normalize_project_locator(std::string_view a_locator) noexcept = 0;
    /// @brief 正規化済み親LocatorとProject名から正規化済みProject Locatorを作る
    [[nodiscard]] virtual Result<std::string> compose_project_locator(std::string_view a_parentLocator,
                                                                      std::string_view a_projectName) noexcept = 0;
    /// @brief Project Root LocatorからCueProject.jsonの正規化済みLocatorを作る
    [[nodiscard]] virtual Result<std::string> compose_descriptor_locator(
        std::string_view a_projectLocator) noexcept = 0;
    /// @brief LocatorのRootを開く。存在しない場合は成功したnullptr、その他の失敗はErrorを返す
    [[nodiscard]] virtual Result<std::unique_ptr<FilesystemRoot>> open_root(std::string_view a_locator) noexcept = 0;
    /// @brief LocatorのRootを開き、存在しない場合は親Directoryを含めて作成する
    [[nodiscard]] virtual Result<std::unique_ptr<FilesystemRoot>> create_or_open_root(
        std::string_view a_locator) noexcept = 0;
    /// @brief Project FolderをReparse Point非追跡で走査し、表示専用の更新時刻と合計File Sizeを返す
    [[nodiscard]] virtual Result<ProjectStorageMetadata> inspect_project_storage(
        std::string_view a_locator) noexcept = 0;
    /// @brief Project FolderをPlatform標準のFile Managerで開く
    [[nodiscard]] virtual Result<void> open_project_folder(std::string_view a_locator) noexcept = 0;
    /// @brief 新規Project用のUUID Version 4を返す
    [[nodiscard]] virtual Result<ProjectId> next_project_id() noexcept = 0;
    /// @brief Blank Default Scene用のcanonical SceneAssetId UUID Version 4文字列を返す
    [[nodiscard]] virtual Result<std::string> next_scene_asset_id() noexcept = 0;

  protected:
    ProjectHubPlatform() noexcept = default;
};

/// @brief Project Hub Sessionに注入するEngineとCapabilityの所有設定
struct ProjectHubConfiguration final
{
    std::uint32_t supportedProjectFormatVersion;
    EngineVersion currentEngineVersion;
    ProjectCapabilityProfile capabilityProfile;
    ProjectCapabilitySnapshot capabilitySnapshot;
    EngineCompatibility blankProjectCompatibility;
    std::vector<InstalledEngineVersionView> installedEngineVersions;
};

/// @brief Project HubがUIへ提示する作成Template
struct ProjectTemplateView final
{
    std::string id;
    std::string displayName;
    EngineCompatibility engineCompatibility;
};

/// @brief Project一覧のDescriptorおよびLocator状態
enum class ProjectEntryState : std::uint8_t
{
    Available,
    Missing,
    Moved,
    Broken
};

/// @brief Broken状態をUI診断へ変換する安定分類
enum class ProjectEntryProblem : std::uint8_t
{
    None,
    LocatorAccessFailed,
    DescriptorInvalid,
    IdentityMismatch,
    CompatibilityInvalid
};

/// @brief UIがProject一覧を描画するための所有Snapshot Row
struct ProjectRowView final
{
    std::string projectId;
    std::string displayName;
    std::string locator;
    std::uint64_t lastOpenedMilliseconds;
    std::string editorVersion;
    std::optional<std::uint64_t> latestWriteMilliseconds;
    std::optional<std::uint64_t> byteSize;
    bool isPinned;
    ProjectEntryState state;
    ProjectEntryProblem problem;
    ProjectCompatibilityStatus compatibilityStatus;
    bool canOpen;
    bool canMigrate;
    std::optional<EngineCompatibility> engineCompatibility;
    std::vector<ProjectCompatibilityReason> compatibilityReasons;
};

/// @brief Blank Project生成後のRecent登録状態と復旧情報
class ProjectCreationOutcome final
{
  public:
    ProjectCreationOutcome() = delete;
    ProjectCreationOutcome(const ProjectCreationOutcome &) = delete;
    ProjectCreationOutcome &operator=(const ProjectCreationOutcome &) = delete;
    ProjectCreationOutcome(ProjectCreationOutcome &&) noexcept = default;
    ProjectCreationOutcome &operator=(ProjectCreationOutcome &&) noexcept = default;
    ~ProjectCreationOutcome() = default;

    /// @brief 生成ProjectがRecent Registryへ永続登録済みならtrueを返す
    [[nodiscard]] bool is_recent_registered() const noexcept;
    /// @brief 生成済みProjectの正規化Locatorを返す
    [[nodiscard]] std::string_view project_locator() const noexcept;
    /// @brief Project公開のDurabilityUnknown診断を返す。耐久性確認済みならnullptr
    [[nodiscard]] const Error *try_creation_durability_error() const noexcept;
    /// @brief Recent永続化の失敗またはDurabilityUnknown診断を返す。診断がなければnullptr
    [[nodiscard]] const Error *try_recent_persistence_error() const noexcept;

  private:
    friend class ProjectHubService;
    ProjectCreationOutcome(std::string &&a_projectLocator, bool a_isRecentRegistered,
                           std::optional<Error> &&a_creationDurabilityError,
                           std::optional<Error> &&a_recentPersistenceError) noexcept;

    std::string m_projectLocator;
    bool m_isRecentRegistered;
    std::optional<Error> m_creationDurabilityError;
    std::optional<Error> m_recentPersistenceError;
};

/// @brief Project HubからEditor Processへ値だけで渡すLaunch契約
class EditorLaunchRequest final
{
  public:
    EditorLaunchRequest() = delete;
    EditorLaunchRequest(const EditorLaunchRequest &) = delete;
    EditorLaunchRequest &operator=(const EditorLaunchRequest &) = delete;
    EditorLaunchRequest(EditorLaunchRequest &&) noexcept = default;
    EditorLaunchRequest &operator=(EditorLaunchRequest &&) noexcept = default;
    ~EditorLaunchRequest() = default;

    [[nodiscard]] std::uint32_t protocol_version() const noexcept;
    [[nodiscard]] std::string_view project_descriptor_locator() const noexcept;
    [[nodiscard]] std::string_view expected_project_id() const noexcept;
    [[nodiscard]] std::string_view engine_compatibility_id() const noexcept;
    [[nodiscard]] const std::optional<std::string> &initial_scene_locator() const noexcept;
    [[nodiscard]] const std::optional<std::string> &expected_initial_scene_asset_id() const noexcept;
    [[nodiscard]] const std::optional<InstalledEngineLaunchIdentity> &installed_engine_identity() const noexcept;

  private:
    friend class ProjectHubService;
    EditorLaunchRequest(std::string &&a_projectDescriptorLocator, std::string &&a_expectedProjectId,
                        std::string &&a_engineCompatibilityId, std::optional<std::string> &&a_initialSceneLocator,
                        std::optional<std::string> &&a_expectedInitialSceneAssetId,
                        std::optional<InstalledEngineLaunchIdentity> &&a_installedEngineIdentity) noexcept;

    std::string m_projectDescriptorLocator;
    std::string m_expectedProjectId;
    std::string m_engineCompatibilityId;
    std::optional<std::string> m_initialSceneLocator;
    std::optional<std::string> m_expectedInitialSceneAssetId;
    std::optional<InstalledEngineLaunchIdentity> m_installedEngineIdentity;
};

/// @brief Project Registry、Descriptor、Compatibilityを束ねるUI非依存Application Service
///
/// 同一Instanceは作成Threadだけで使用し、返すViewは次のMutationまで有効とする
/// createへ渡すWorkspace Filesystem、Platform、AssertContextは非所有で保持し、Serviceより長く生存させる
class ProjectHubService final
{
  public:
    ProjectHubService(const ProjectHubService &) = delete;
    ProjectHubService &operator=(const ProjectHubService &) = delete;
    ProjectHubService(ProjectHubService &&) = delete;
    ProjectHubService &operator=(ProjectHubService &&) = delete;
    ~ProjectHubService() = default;

    /// @brief Workspace Registryを読込み、初期ViewModelを構築する
    /// @param a_workspaceFilesystem 返却Serviceより長く生存する非所有Workspace Root
    /// @param a_platform 返却Serviceより長く生存する非所有Platform Composition
    /// @param a_configuration Serviceが所有権を取得するEngine・Capability設定
    /// @param a_assertContext 返却Serviceと注入Platformより長く生存する非所有診断Context
    [[nodiscard]] static Result<std::unique_ptr<ProjectHubService>> create(
        FilesystemRoot &a_workspaceFilesystem, ProjectHubPlatform &a_platform,
        ProjectHubConfiguration &&a_configuration, const AssertContext &a_assertContext) noexcept;

    [[nodiscard]] std::span<const ProjectTemplateView> templates() const noexcept;
    [[nodiscard]] std::span<const ProjectRowView> projects() const noexcept;
    [[nodiscard]] std::span<const InstalledEngineVersionView> installed_engine_versions() const noexcept;

    /// @brief Session内で使う検証済みInstalled Engine Versionを選択しProject互換Viewを再評価する
    [[nodiscard]] Result<void> select_installed_engine_version(std::string_view a_versionDirectory) noexcept;

    /// @brief Distribution層で再検査したInstalled Version一覧へ置換しProject互換Viewを再評価する
    [[nodiscard]] Result<void> replace_installed_engine_versions(
        std::vector<InstalledEngineVersionView> a_versions) noexcept;

    /// @brief 全Recent Locatorを再検査し、欠損や破損をEntry単位で隔離してViewModelを更新する
    /// @note ErrorのRoot CodeがCue.IO/IoError::DurabilityUnknownなら変更は公開済みでprojectsの旧Spanは無効
    [[nodiscard]] Result<void> refresh() noexcept;
    /// @brief Blank TemplateでProjectをAtomic生成しRecentへ登録する
    ///
    /// 公開後のDurabilityUnknownは再Open検証し、成功Outcomeのcreation durability診断へ保持する。再Openで生成Projectを
    /// 確認できない場合は失敗を返し、公開済みとは扱わない。Recent未登録の場合はproject_locatorをregister_projectへ渡して
    /// Recent登録だけを再試行する
    [[nodiscard]] Result<ProjectCreationOutcome> create_blank_project(std::string_view a_parentLocator,
                                                                      std::string_view a_projectName,
                                                                      std::string_view a_templateId,
                                                                      std::uint64_t a_openedMilliseconds) noexcept;
    /// @brief 既存Projectを登録し、明示時だけ同一ProjectIdの移動を再関連付けする
    /// @note ErrorのRoot CodeがCue.IO/IoError::DurabilityUnknownなら登録は公開済みでprojectsの旧Spanは無効
    [[nodiscard]] Result<void> register_project(std::string_view a_locator, std::uint64_t a_openedMilliseconds,
                                                bool a_confirmMovedProject) noexcept;
    /// @brief Descriptorを再検証し、互換ProjectのEditor Launch Requestを生成する
    /// @note 戻り値にかかわらず再検証でViewModelが更新され得るため、呼出し前に取得したprojectsのSpanは再利用しない
    /// @note ErrorのRoot CodeがCue.IO/IoError::DurabilityUnknownならOpen時刻は公開済みでprojectsの旧Spanは無効
    /// @note Error CodeがOpenRejectedViewDurabilityUnknownならLaunch
    /// RequestとOpen時刻更新は未生成だが、拒否後の一覧状態は 公開済みでprojectsの旧Spanは無効。Immediate
    /// Causeは元のOpen拒否Categoryを保持する
    [[nodiscard]] Result<EditorLaunchRequest> open_project(
        std::string_view a_projectId, std::uint64_t a_openedMilliseconds,
        std::optional<std::string_view> a_initialSceneLocator = std::nullopt) noexcept;
    /// @brief 選択Projectの旧DescriptorをUser確認後にCurrent Schemaへ明示Migrationする
    ///
    /// 公開後のDurabilityUnknownは成功Outcomeで返し、Project一覧はMigration後Modelへ更新する。
    [[nodiscard]] Result<ProjectDescriptorMigrationOutcome> migrate_project(std::string_view a_projectId) noexcept;
    /// @brief Recent EntryのPin状態を変更する
    /// @note ErrorのRoot CodeがCue.IO/IoError::DurabilityUnknownならPin変更は公開済みでprojectsの旧Spanは無効
    [[nodiscard]] Result<void> set_project_pinned(std::string_view a_projectId, bool a_isPinned) noexcept;
    /// @brief Pin EntryをPin一覧内の位置へ移動する
    /// @note ErrorのRoot CodeがCue.IO/IoError::DurabilityUnknownなら並べ替えは公開済みでprojectsの旧Spanは無効
    [[nodiscard]] Result<void> move_pinned_project(std::string_view a_projectId, std::size_t a_targetIndex) noexcept;
    /// @brief Recent EntryのLocatorを再検証しPlatform標準のFile Managerで開く
    [[nodiscard]] Result<void> open_project_folder(std::string_view a_projectId) noexcept;
    /// @brief Recent Entryだけを除外しProject Folderには触れない
    /// @note ErrorのRoot CodeがCue.IO/IoError::DurabilityUnknownなら除外は公開済みでprojectsの旧Spanは無効
    [[nodiscard]] Result<void> remove_project(std::string_view a_projectId) noexcept;

  private:
    struct ConstructionKey final
    {
    };

    ProjectHubService(ConstructionKey, FilesystemRoot &a_workspaceFilesystem, ProjectHubPlatform &a_platform,
                      ProjectHubConfiguration &&a_configuration, RecentProjectRegistry &&a_registry,
                      const AssertContext &a_assertContext) noexcept;

    struct PreparedRegistrySnapshot final
    {
        std::vector<ProjectRowView> projects;
        bool registryChanged;
    };

    [[nodiscard]] Result<PreparedRegistrySnapshot> prepare_registry_snapshot(
        RecentProjectRegistry &a_registry) noexcept;
    void refresh_after_open_failure(Error &a_primary) noexcept;
    [[nodiscard]] Result<RecentProjectRegistry> clone_registry() const noexcept;
    [[nodiscard]] Result<void> commit_registry(RecentProjectRegistry &&a_registry) noexcept;
    [[nodiscard]] Result<ProjectId> parse_project_id(std::string_view a_projectId) const noexcept;

    FilesystemRoot *m_workspaceFilesystem;
    ProjectHubPlatform *m_platform;
    const AssertContext *m_assertContext;
    ProjectHubConfiguration m_configuration;
    RecentProjectRegistry m_registry;
    std::vector<ProjectTemplateView> m_templates;
    std::vector<ProjectRowView> m_projects;
};
} // namespace cue::project_hub
