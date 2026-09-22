#pragma once

#include <Cue/Distribution/Manifest.h>
#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::distribution
{
/// @brief Installed Versionが新規起動可能かをRegistry内で表す
enum class InstalledVersionState : std::uint8_t
{
    Selectable,
    PendingRemoval
};

/// @brief 一つのImmutable Installed Versionを再検証するための耐久Evidence
struct InstalledVersionEntry final
{
    std::string directoryName;
    std::string engineVersion;
    std::string bundleId;
    std::string manifestDigest;
    std::string payloadMarkerDigest;
    std::string probeMarkerDigest;
    std::string workerId;
    std::string workerExecutableDigest;
    std::string workerMarkerDigest;
    InstalledVersionState state = InstalledVersionState::Selectable;

    [[nodiscard]] bool operator==(const InstalledVersionEntry &) const noexcept = default;
};

/// @brief InstalledVersions.json v1の所有値
struct InstalledVersionsRegistry final
{
    std::string generationId;
    std::uint64_t revision = 0U;
    std::string selectedVersion;
    std::vector<InstalledVersionEntry> versions;

    [[nodiscard]] bool operator==(const InstalledVersionsRegistry &) const noexcept = default;
};

/// @brief Operation Journalが識別する更新種別
enum class InstallOperationKind : std::uint8_t
{
    Install,
    Update,
    Rollback,
    Uninstall,
    RegistryRecovery
};

/// @brief Journal v1が証明済みとして扱う最後の耐久副作用
enum class InstallOperationStage : std::uint8_t
{
    Prepared,
    PayloadStaged,
    VersionPublished,
    ProbeSucceeded,
    WorkerPublished,
    RegistryPublished,
    SelectionPublished,
    RemovalBlocked,
    VersionQuarantined,
    RegistryEntryRemoved,
    CandidatesValidated
};

/// @brief 通常操作が上書き前に照合するRegistry Generation／Revision
struct ExpectedRegistry final
{
    std::string generationId;
    std::uint64_t revision = 0U;

    [[nodiscard]] bool operator==(const ExpectedRegistry &) const noexcept = default;
};

/// @brief Install／Update／Rollback／Uninstallが対象とするVersion Identity
struct InstallOperationTarget final
{
    std::string directoryName;
    std::string bundleId;
    std::string manifestDigest;

    [[nodiscard]] bool operator==(const InstallOperationTarget &) const noexcept = default;
};

/// @brief Registry Recovery開始時に観測したRegistry Fileの状態
struct RegistrySourceEvidence final
{
    bool wasMissing = true;
    std::string evidenceName;
    std::uint64_t byteSize = 0U;
    std::string sha256;

    [[nodiscard]] bool operator==(const RegistrySourceEvidence &) const noexcept = default;
};

/// @brief RecoveryがSelectable候補から除外した未完了Operation
struct BlockedInstallOperation final
{
    std::string operationId;
    InstallOperationKind kind = InstallOperationKind::Install;
    std::string directoryName;
    InstallOperationStage stage = InstallOperationStage::Prepared;
    std::string journalDigest;

    [[nodiscard]] bool operator==(const BlockedInstallOperation &) const noexcept = default;
};

/// @brief Registry Recoveryが全Evidenceを再検証したInstalled Version候補
struct RegistryRecoveryCandidate final
{
    InstalledVersionEntry version;

    [[nodiscard]] bool operator==(const RegistryRecoveryCandidate &) const noexcept = default;
};

/// @brief CueEngine Install Operation Journal v1の所有値
struct InstallOperationJournal final
{
    std::string operationId;
    InstallOperationKind kind = InstallOperationKind::Install;
    InstallOperationStage stage = InstallOperationStage::Prepared;
    std::string workerId;
    std::optional<ExpectedRegistry> expectedRegistry;
    std::optional<InstallOperationTarget> target;
    std::optional<RegistrySourceEvidence> sourceRegistryEvidence;
    std::vector<BlockedInstallOperation> blockedOperations;
    std::vector<RegistryRecoveryCandidate> candidates;

    [[nodiscard]] bool operator==(const InstallOperationJournal &) const noexcept = default;
};

/// @brief Version PayloadがManifestどおり完成したことを示すMarker v1
struct PayloadCompleteMarker final
{
    std::string directoryName;
    std::string bundleId;
    std::string manifestDigest;

    [[nodiscard]] bool operator==(const PayloadCompleteMarker &) const noexcept = default;
};

/// @brief 専用Install Probeが成功したことを示すMarker v1
struct InstallProbeMarker final
{
    std::string operationId;
    std::string directoryName;
    std::string bundleId;
    std::string manifestDigest;

    [[nodiscard]] bool operator==(const InstallProbeMarker &) const noexcept = default;
};

/// @brief Version外へAtomic PublishしたInstall WorkerのMarker v1
struct InstallWorkerMarker final
{
    std::string workerId;
    std::string bundleId;
    std::string engineSourceRevision;
    std::string publisherBuildIdentityDigest;
    DistributionFileEntry executable;
    DistributionArchitecture peArchitecture = DistributionArchitecture::X64;

    [[nodiscard]] bool operator==(const InstallWorkerMarker &) const noexcept = default;
};

/// @brief Registry v1をCanonical UTF-8、BOMなし、Whitespaceなし、LF終端JSONへ直列化する
[[nodiscard]] Result<std::string> write_installed_versions_registry(const InstalledVersionsRegistry &a_registry,
                                                                    const AssertContext &a_assertContext) noexcept;
/// @brief Canonical InstalledVersions.json v1だけをFail-closedで読み込む
[[nodiscard]] Result<InstalledVersionsRegistry> read_installed_versions_registry(
    std::string_view a_bytes, const AssertContext &a_assertContext) noexcept;
/// @brief Operation Journal v1をKind固有Memberと固定順序のCanonical JSONへ直列化する
[[nodiscard]] Result<std::string> write_install_operation_journal(const InstallOperationJournal &a_journal,
                                                                  const AssertContext &a_assertContext) noexcept;
/// @brief Canonical Operation Journal v1だけをFail-closedで読み込む
[[nodiscard]] Result<InstallOperationJournal> read_install_operation_journal(
    std::string_view a_bytes, const AssertContext &a_assertContext) noexcept;
/// @brief Payload完了Marker v1をCanonical JSONへ直列化する
[[nodiscard]] Result<std::string> write_payload_complete_marker(const PayloadCompleteMarker &a_marker,
                                                                const AssertContext &a_assertContext) noexcept;
/// @brief Canonical Payload完了Marker v1だけをFail-closedで読み込む
[[nodiscard]] Result<PayloadCompleteMarker> read_payload_complete_marker(std::string_view a_bytes,
                                                                         const AssertContext &a_assertContext) noexcept;
/// @brief Probe成功Marker v1をCanonical JSONへ直列化する
[[nodiscard]] Result<std::string> write_install_probe_marker(const InstallProbeMarker &a_marker,
                                                             const AssertContext &a_assertContext) noexcept;
/// @brief Canonical Probe成功Marker v1だけをFail-closedで読み込む
[[nodiscard]] Result<InstallProbeMarker> read_install_probe_marker(std::string_view a_bytes,
                                                                   const AssertContext &a_assertContext) noexcept;
/// @brief Install Worker完了Marker v1をCanonical JSONへ直列化する
[[nodiscard]] Result<std::string> write_install_worker_marker(const InstallWorkerMarker &a_marker,
                                                              const AssertContext &a_assertContext) noexcept;
/// @brief Canonical Install Worker完了Marker v1だけをFail-closedで読み込む
[[nodiscard]] Result<InstallWorkerMarker> read_install_worker_marker(std::string_view a_bytes,
                                                                     const AssertContext &a_assertContext) noexcept;

/// @brief Manifest IdentityとInstall Worker InventoryからCanonical Worker IDを導出する
[[nodiscard]] Result<std::string> make_install_worker_id(const DistributionManifest &a_manifest,
                                                         const AssertContext &a_assertContext) noexcept;
/// @brief Publisher Build IdentityをMarkerへ結び付けるCanonical SHA-256を導出する
[[nodiscard]] Result<std::string> make_publisher_build_identity_digest(const PublisherBuildIdentity &a_identity,
                                                                       const AssertContext &a_assertContext) noexcept;
/// @brief Journal v1で現在Stageから次Stageへの遷移が許可されるか返す
[[nodiscard]] bool is_valid_install_stage_transition(InstallOperationKind a_kind, InstallOperationStage a_current,
                                                     InstallOperationStage a_next) noexcept;
/// @brief Operation KindをCanonical Journal文字列へ変換する
[[nodiscard]] std::string_view install_operation_kind_name(InstallOperationKind a_kind) noexcept;
/// @brief Operation StageをCanonical Journal文字列へ変換する
[[nodiscard]] std::string_view install_operation_stage_name(InstallOperationStage a_stage) noexcept;
/// @brief Installed Version StateをCanonical Registry文字列へ変換する
[[nodiscard]] std::string_view installed_version_state_name(InstalledVersionState a_state) noexcept;
} // namespace cue::distribution
