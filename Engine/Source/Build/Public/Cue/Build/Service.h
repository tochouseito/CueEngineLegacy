#pragma once

#include <Cue/Build/CMakeRunner.h>

#include <chrono>
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

/// @brief Game Build ServiceとArtifact Modelの安定した失敗分類
enum class GameBuildServiceError : std::int64_t
{
    InvalidArtifact = 1,
    MissingDependency,
    OperationAlreadyRunning,
    NoRetryableOperation,
    NoActiveOperation,
    OwnerThreadViolation,
    ArtifactPublicationFailed
};

/// @brief Platform非依存なArtifact Publisher Timeoutの分類
enum class BuildArtifactPublisherError : std::int64_t
{
    LockWaitTimedOut = 1,
    ModuleProbeTimedOut
};

/// @brief Publisherの取消可能処理を単調Clock上で打ち切る絶対時刻
using BuildArtifactLockDeadline = std::optional<std::chrono::steady_clock::time_point>;

/// @brief Artifact Read Lease待機中の取消状態をPlatform非依存に参照する境界
class BuildArtifactReadCancellation
{
  public:
    BuildArtifactReadCancellation(const BuildArtifactReadCancellation &) = delete;
    BuildArtifactReadCancellation &operator=(const BuildArtifactReadCancellation &) = delete;
    /// @brief 派生取消状態を正しく破棄する
    virtual ~BuildArtifactReadCancellation() = default;
    /// @brief 呼出Operationの取消が要求済みか返す
    [[nodiscard]] virtual bool is_cancel_requested() const noexcept = 0;

  protected:
    /// @brief 派生取消実装だけに構築を許可する
    BuildArtifactReadCancellation() noexcept = default;
};

/// @brief Current Artifact VersionをCleanupから保護するProcess間Shared Read Lease
class BuildArtifactReadLease
{
  public:
    BuildArtifactReadLease(const BuildArtifactReadLease &) = delete;
    BuildArtifactReadLease &operator=(const BuildArtifactReadLease &) = delete;
    /// @brief 派生Leaseを通してNative Lockを解放する
    virtual ~BuildArtifactReadLease() = default;
    /// @brief Lease発行元Artifact StoreへBindingされたProject Identityを返す
    [[nodiscard]] virtual std::string_view project_id() const noexcept = 0;

  protected:
    /// @brief 派生Leaseだけに構築を許可する
    BuildArtifactReadLease() noexcept = default;
};

/// @brief Artifact Fileを配布、Runtime Metadata、開発Symbolへ分類する
enum class BuildArtifactFilePurpose : std::uint8_t
{
    Unspecified,
    DistributionPayload,
    RuntimeMetadata,
    DevelopmentSymbol
};

/// @brief Artifact Version Directory内の一FileをHashと用途付きで識別する
struct BuildArtifactFile final
{
    std::string relativePath;
    std::uint64_t byteSize = 0U;
    std::string contentHash;
    BuildArtifactFilePurpose purpose = BuildArtifactFilePurpose::Unspecified;
};

/// @brief 一つのBuild WorkspaceをProcess間で排他的に保護するRAII Token
class BuildWorkspaceLease
{
  public:
    /// @brief Process間排他所有権のCopy構築を禁止する
    BuildWorkspaceLease(const BuildWorkspaceLease &) = delete;
    /// @brief Process間排他所有権のCopy代入を禁止する
    BuildWorkspaceLease &operator=(const BuildWorkspaceLease &) = delete;
    /// @brief 派生Leaseを通してNative Lockを解放する
    virtual ~BuildWorkspaceLease() = default;

  protected:
    /// @brief 派生Leaseだけに構築を許可する
    BuildWorkspaceLease() noexcept = default;
};

/// @brief 一回のBuildが参照する外部入力を完了まで置換不能に保つRAII Token
class BuildInputLease
{
  public:
    BuildInputLease(const BuildInputLease &) = delete;
    BuildInputLease &operator=(const BuildInputLease &) = delete;
    /// @brief 派生Leaseを通してPlatform固有Lockを解放する
    virtual ~BuildInputLease() = default;

  protected:
    /// @brief 派生Leaseだけに構築を許可する
    BuildInputLease() noexcept = default;
};

/// @brief Build開始直前に外部入力を再検証してBuild-scoped Leaseへ変換する境界
class BuildInputLeaseProvider
{
  public:
    BuildInputLeaseProvider(const BuildInputLeaseProvider &) = delete;
    BuildInputLeaseProvider &operator=(const BuildInputLeaseProvider &) = delete;
    /// @brief 派生Providerを正しく破棄する
    virtual ~BuildInputLeaseProvider() = default;
    /// @brief Worker上でPlan入力を再検証し、完了まで保持する取消可能Leaseを返す
    [[nodiscard]] virtual Result<std::unique_ptr<BuildInputLease>>
    acquire(const BuildPlan &a_plan, const ChildProcessCancellation &a_cancellation) noexcept = 0;

  protected:
    /// @brief 派生Providerだけに構築を許可する
    BuildInputLeaseProvider() noexcept = default;
};

/// @brief Publish済み不変Build Target Artifact集合
class BuildArtifactInventory final
{
  public:
    /// @brief 検証されていない空Inventoryの生成を禁止する
    BuildArtifactInventory() = delete;

    /// @brief Planと検証済みFile一覧からPublish済みInventoryを構築する
    ///
    /// PlanとAssertContextは呼出中だけ借用し、Artifact IDとFile一覧は返却Inventoryへ移動する。File一覧はPath昇順へ
    /// 正規化する。ID、相対Path、Hash、Size、用途、重複、Target別必須Payload／Metadataが不正な場合は
    /// InvalidArtifactを返す。
    /// 共有状態を変更しないため別入力から同時に呼べる。Allocation等の回復不能例外はFatalHandlerへ渡す。
    [[nodiscard]] static Result<BuildArtifactInventory> create(const BuildPlan &a_plan, std::string a_artifactId,
                                                               std::vector<BuildArtifactFile> a_files,
                                                               const AssertContext &a_assertContext) noexcept;

    /// @brief M15 Layoutで公開済みのGame Module Artifactを移行期間中の読取Inventoryとして構築する
    ///
    /// File検証はcreateと同じ契約を使用し、Version Directoryだけを旧
    /// `Generated/Artifacts/<Configuration>/Versions/<ArtifactId>`へ設定する。ShippingProductには使用できない。
    [[nodiscard]] static Result<BuildArtifactInventory> create_legacy_game_module(
        const BuildPlan &a_plan, std::string a_artifactId, std::vector<BuildArtifactFile> a_files,
        const AssertContext &a_assertContext) noexcept;

    /// @brief Artifact Versionを識別するlowercase UUID v4を返す
    [[nodiscard]] std::string_view artifact_id() const noexcept;
    /// @brief Build時のConfigurationを返す
    [[nodiscard]] BuildConfiguration configuration() const noexcept;
    /// @brief Target、Configuration、Trust Identityを含むBuild Profileを返す
    [[nodiscard]] const BuildProfile &profile() const noexcept;
    /// @brief Project Root内の不変Version Directoryを返す
    [[nodiscard]] std::string_view version_directory() const noexcept;
    /// @brief Path昇順のFile Inventoryを返す
    [[nodiscard]] std::span<const BuildArtifactFile> files() const noexcept;

  private:
    /// @brief 検証済みArtifact情報の所有権を取得してInventoryを構築する
    BuildArtifactInventory(std::string a_artifactId, BuildProfile a_profile, std::string a_versionDirectory,
                           std::vector<BuildArtifactFile> a_files) noexcept;

    std::string m_artifactId;
    BuildProfile m_profile;
    std::string m_versionDirectory;
    std::vector<BuildArtifactFile> m_files;
};

/// @brief Current Manifest v1／v2が期待Artifact Inventoryを意味的に選択しているか検証する
///
/// JSONとInventoryとAssertContextは呼出中だけ借用する。Member順と意味を持たない空白には依存せず、未知／重複／欠落Member、
/// 型不一致、未知Schema、末尾Data、Profile／Inventory不一致をInvalidArtifactとして拒否する。v1はGameModuleだけに許可し、
/// v2はTarget、Trust Identity、File用途も照合する。共有状態を変更しないため同時に呼べる。
[[nodiscard]] Result<void> validate_build_artifact_current_manifest(std::string_view a_json,
                                                                    const BuildArtifactInventory &a_expected,
                                                                    const AssertContext &a_assertContext) noexcept;

/// @brief Current Manifestと参照Versionを検証してShared Read Leaseを発行するArtifact Store Reader
class BuildArtifactReader
{
  public:
    BuildArtifactReader(const BuildArtifactReader &) = delete;
    BuildArtifactReader &operator=(const BuildArtifactReader &) = delete;
    /// @brief 派生Readerを正しく破棄する
    virtual ~BuildArtifactReader() = default;

    /// @brief Expected InventoryがCurrentの間だけ参照Versionを保護するShared Read Leaseを取得する
    ///
    /// Inventory、Cancellation、Deadlineは呼出中だけ借用する。実装はProjectとConfigurationにBindingされた
    /// Artifact StoreのShared Read LeaseをCurrent再読込前に取得し、Currentと全FileのSize／Hashを検証してから返す。
    /// 取消要求を観測した場合は成功のnulloptを返し、同期なしのFilesystem読込へFallbackしない。
    [[nodiscard]] virtual Result<std::optional<std::unique_ptr<BuildArtifactReadLease>>> acquire_current_read_lease(
        const BuildArtifactInventory &a_expected, const BuildArtifactReadCancellation &a_cancellation,
        BuildArtifactLockDeadline a_deadline) noexcept = 0;

  protected:
    /// @brief 派生Readerだけに構築を許可する
    BuildArtifactReader() noexcept = default;
};

/// @brief Build成功Candidateを検証して不変Artifactとして公開する注入境界
class BuildArtifactPublisher
{
  public:
    /// @brief Publisher実装の暗黙Copy構築を禁止する
    BuildArtifactPublisher(const BuildArtifactPublisher &) = delete;
    /// @brief Publisher実装の暗黙Copy代入を禁止する
    BuildArtifactPublisher &operator=(const BuildArtifactPublisher &) = delete;
    /// @brief 派生Publisherを正しく破棄する
    virtual ~BuildArtifactPublisher() = default;

    /// @brief Configure開始前にPlan固有Build WorkspaceのExclusive Leaseを取得する
    ///
    /// Plan、Cancellation、Deadlineは呼出中だけ借用する。取消要求を観測した場合は成功のnulloptを返す。Deadline到達は
    /// BuildArtifactPublisherError::LockWaitTimedOutを返す。成功Leaseは同じWorker上で
    /// Publishへ移すか破棄し、Build ProcessとCandidate確定が終わるまで保持する。回復可能なLock失敗はErrorを返す。
    [[nodiscard]] virtual Result<std::optional<std::unique_ptr<BuildWorkspaceLease>>> acquire_build_lease(
        const BuildPlan &a_plan, const ChildProcessCancellation &a_cancellation,
        BuildArtifactLockDeadline a_deadline) noexcept = 0;

    /// @brief Build Plan固有Candidateを検証・公開し、成功時だけInventoryを返す
    ///
    /// PlanとCancellationは呼出中だけ借用し、Build Leaseの所有権を取得する。実装はCandidate Snapshot確定後にBuild
    /// Leaseを解放してから Artifact Mutation Leaseを取得し、二つのLeaseを同時保持しない。一つのGameBuildService
    /// Workerから直列に呼ばれ、返却Inventoryが
    /// 全値を所有する。取消要求は不可逆なCurrent更新前まで監視し、公開せず成功のnulloptを返す。
    /// Inventory返却後の取消は確定済みArtifactを巻き戻さない。DeadlineはArtifact Mutation
    /// Lock待機とArtifact Probeを制限し、到達時は対応するBuildArtifactPublisherErrorを返す。回復可能な
    /// 検証・IO失敗はErrorを返し、例外を境界外へ送出しない。
    [[nodiscard]] virtual Result<std::optional<BuildArtifactInventory>> publish(
        const BuildPlan &a_plan, const ChildProcessCancellation &a_cancellation,
        std::unique_ptr<BuildWorkspaceLease> a_buildLease, BuildArtifactLockDeadline a_deadline) noexcept = 0;

  protected:
    /// @brief 派生Publisherを初期化する
    BuildArtifactPublisher() noexcept = default;
};

/// @brief UIが表示するBuild Operation全体の状態
enum class GameBuildOperationState : std::uint8_t
{
    Idle,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut
};

/// @brief Native API固有のError DomainとCodeをUI再表示可能な所有値にした診断
struct BuildNativeErrorSnapshot final
{
    /// @brief Win32等のNative診断Domain
    std::string domain;
    /// @brief Native Domain内の診断Code
    std::int64_t code = 0;
};

/// @brief Foundation ErrorをUI再表示可能な所有値へ平坦化した診断
struct BuildDiagnosticSnapshot final
{
    std::string domain;
    std::int64_t code = 0;
    std::string summary;
    std::vector<std::string> contexts;
    std::optional<BuildNativeErrorSnapshot> nativeError;
};

/// @brief OperationとStageへ関連付いた一件のCapture Log
struct BuildLogSnapshot final
{
    std::string operationId;
    BuildStage stage = BuildStage::Configure;
    std::uint64_t sequence = 0U;
    ChildProcessStream stream = ChildProcessStream::StandardOutput;
    std::string bytes;
};

/// @brief 完了したBuild StageのBundle化可能な値Snapshot
struct BuildStageSnapshot final
{
    BuildStage stage = BuildStage::Configure;
    BuildStageOutcome outcome = BuildStageOutcome::Failed;
    std::optional<std::uint32_t> exitCode;
};

/// @brief LockなしでUIが保持できる一時点のBuild状態
struct BuildOperationSnapshot final
{
    GameBuildOperationState state = GameBuildOperationState::Idle;
    std::string operationId;
    std::optional<BuildProfile> profile;
    std::optional<BuildStage> activeStage;
    std::vector<BuildStageSnapshot> stages;
    std::vector<BuildLogSnapshot> logs;
    std::vector<BuildDiagnosticSnapshot> diagnostics;
    std::optional<BuildArtifactInventory> artifact;
    std::optional<BuildArtifactInventory> latestSuccessfulArtifact;
};

/// @brief 一つのProject Buildを所有Workerで直列実行するApplication Service
///
/// start、retry、wait_for_completionはcreate呼出Threadだけで使用する。snapshotとrequest_cancelは任意Threadから呼べる。
/// 注入するProcess RunnerとArtifact
/// PublisherはServiceが所有する。AssertContextは借用し、Serviceより長く生存する必要がある。
/// Destructorは進行中Cancellationを通知してWorkerをJoinし、Child Processを残さない。
class GameBuildService final
{
  public:
    /// @brief 単一WorkerとProcess所有権のCopy構築を禁止する
    GameBuildService(const GameBuildService &) = delete;
    /// @brief 単一WorkerとProcess所有権のCopy代入を禁止する
    GameBuildService &operator=(const GameBuildService &) = delete;
    /// @brief 実行中Workerを持つServiceのMove構築を禁止する
    GameBuildService(GameBuildService &&) = delete;
    /// @brief 実行中Workerを持つServiceのMove代入を禁止する
    GameBuildService &operator=(GameBuildService &&) = delete;
    /// @brief 実行中OperationをCancelしてWorkerをJoinする
    ~GameBuildService();

    /// @brief Process RunnerとPublisherの所有権を取得してIdle Serviceを構築する
    ///
    /// SettingsはServiceが所有し、AssertContextは借用する。Null依存はMissingDependencyを返す。返却Serviceはcreate呼出Threadを
    /// Owner Threadとして記録する。AllocationまたはThread基盤の回復不能例外はFatalHandlerへ渡す。
    [[nodiscard]] static Result<std::unique_ptr<GameBuildService>> create(
        CMakeRunnerSettings a_settings, std::unique_ptr<ChildProcessRunner> a_processRunner,
        std::unique_ptr<BuildArtifactPublisher> a_artifactPublisher, const AssertContext &a_assertContext) noexcept;
    /// @brief Buildごとの外部入力再検証Providerも所有してIdle Serviceを構築する
    ///
    /// ProviderはBuild Worker上で呼び出され、外部入力の変更をCMake Treeへ反映するためConfigureを必ず実行する。
    [[nodiscard]] static Result<std::unique_ptr<GameBuildService>> create(
        CMakeRunnerSettings a_settings, std::unique_ptr<ChildProcessRunner> a_processRunner,
        std::unique_ptr<BuildArtifactPublisher> a_artifactPublisher,
        std::unique_ptr<BuildInputLeaseProvider> a_inputLeaseProvider,
        const AssertContext &a_assertContext) noexcept;

    /// @brief 検証済みPlanを作り、単一Active OperationとしてWorker上で開始する
    ///
    /// Requestを値で受け取り、Retry用に所有する。Owner Thread以外はOwnerThreadViolation、実行中は
    /// OperationAlreadyRunning、Plan不正はBuildPlanErrorを返す。成功後の処理は非同期で、snapshotまたは
    /// wait_for_completionから観測する。
    [[nodiscard]] Result<void> start(BuildRequest a_request, CMakeConfigureMode a_configureMode) noexcept;
    /// @brief 最後のRequestを新しいOperation IDでConfigureから再実行する
    ///
    /// Operation IDは値で受け取る。Owner Thread以外、実行中、Retry対象なし、不正IDをそれぞれErrorとして返す。
    [[nodiscard]] Result<void> retry(std::string a_operationId) noexcept;
    /// @brief 進行中OperationへThread-safeにCancelを通知する
    ///
    /// 任意Threadから呼べる。実行中Operationがない場合はNoActiveOperationを返す。
    [[nodiscard]] Result<void> request_cancel() noexcept;
    /// @brief 現在State、Log、診断、Artifactを所有Copyとして返す
    ///
    /// 任意Threadから呼べ、返却SnapshotはService内部への参照を保持しない。
    [[nodiscard]] BuildOperationSnapshot snapshot() const noexcept;
    /// @brief Owner Threadで現在Workerの完了を待ち、Joinする
    ///
    /// Owner Thread以外はOwnerThreadViolationを返す。完了済みまたは未開始の場合も成功する。
    [[nodiscard]] Result<void> wait_for_completion() noexcept;

  private:
    struct Impl;
    /// @brief 構築済み実装の所有権を取得してServiceを構築する
    explicit GameBuildService(std::unique_ptr<Impl> a_impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};
} // namespace cue
