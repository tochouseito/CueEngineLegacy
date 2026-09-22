#pragma once

#include <Cue/Distribution/InstallState.h>
#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::distribution
{
/// @brief Local Developer BundleをPer-user Install Rootへ導入する要求
struct WindowsInstallRequest final
{
    std::string bundleRoot;
    std::string installRoot;
    bool isUpdate = false;
    bool allowUnsignedLocal = false;
};

/// @brief 完了したInstall Transactionの検証済みIdentity
struct WindowsInstallOutcome final
{
    std::string operationId;
    std::string versionDirectory;
    std::string workerId;
    std::uint64_t registryRevision = 0U;
    bool wasAlreadyInstalled = false;
};

/// @brief Installerが継承HandleとOperation Identityを結び付けて起動する専用Probe要求
struct WindowsInstallProbeRequest final
{
    std::uintptr_t leaseHandle = 0U;
    std::string installRoot;
    std::string versionDirectory;
    std::string operationId;
    std::string manifestDigest;
};

/// @brief Install済みVersionを選択、起動保護、または削除対象として識別する要求
struct WindowsInstalledVersionRequest final
{
    std::string installRoot;
    std::string versionDirectory;
};

/// @brief Project Hubが表示する一つのInstalled Version検査結果
struct WindowsInstalledVersionInspection final
{
    std::string directoryName;
    std::string engineVersion;
    std::string bundleId;
    std::string manifestDigest;
    std::string editorExecutable;
    std::string engineSourceRoot;
    std::string diagnostic;
    InstalledVersionState state = InstalledVersionState::Selectable;
    bool isSelected = false;
    bool isAvailable = false;
};

/// @brief 共有Control Lease下で取得したInstalled Versions RegistryとVersion証拠の所有Snapshot
struct WindowsInstalledVersionsInspection final
{
    std::string installRoot;
    std::uint64_t registryRevision = 0U;
    std::vector<WindowsInstalledVersionInspection> versions;
};

/// @brief Child Processが継承Execution LeaseとDistribution Identityを再検証する要求
struct WindowsInheritedVersionExecutionLeaseRequest final
{
    std::uintptr_t inheritedLeaseHandle = 0U;
    std::string installRoot;
    std::string versionDirectory;
    std::string bundleId;
    std::string manifestDigest;
};

/// @brief Rollback選択Transactionの検証済み結果
struct WindowsRollbackOutcome final
{
    std::string operationId;
    std::string versionDirectory;
    std::uint64_t registryRevision = 0U;
    bool wasAlreadySelected = false;
};

/// @brief 外部Install Workerへ委譲したUninstall Transactionの識別結果
struct WindowsUninstallOutcome final
{
    std::string operationId;
    std::string versionDirectory;
    std::string workerId;
    std::uint32_t workerProcessId = 0U;
};

/// @brief 継承済みGate／Worker Evidence HandleからUninstall Journalを再開する外部Worker要求
///
/// Worker Directory、Executable、Marker Handleは検証済みFile IdentityをChild終了まで置換不能にする。
/// `sourceProcessHandle`は呼出元Imageが削除対象Version内にある場合だけ設定され、Childは終了を待ってから削除する。
struct WindowsUninstallWorkerRequest final
{
    std::uintptr_t gateHandle = 0U;
    std::uintptr_t workerDirectoryHandle = 0U;
    std::uintptr_t workerExecutableHandle = 0U;
    std::uintptr_t workerMarkerHandle = 0U;
    std::uintptr_t sourceProcessHandle = 0U;
    std::string installRoot;
    std::string operationId;
    std::string workerId;
};

/// @brief Installed VersionをUninstallから保護する共有Execution Lease
///
/// `native_handle`は継承可能なWindows Handleである。呼出側は対象ChildだけへHandleを継承し、Child作成後に本Objectを
/// 破棄する。Childが継承Handleを閉じるか終了するまで、同じVersionの排他Uninstall Leaseは取得できない。
class WindowsInstalledVersionExecutionLease final
{
  public:
    WindowsInstalledVersionExecutionLease() = delete;
    WindowsInstalledVersionExecutionLease(const WindowsInstalledVersionExecutionLease &) = delete;
    WindowsInstalledVersionExecutionLease &operator=(const WindowsInstalledVersionExecutionLease &) = delete;
    /// @brief 共有Lease Handleと検証済みIdentityの所有権を移動する
    WindowsInstalledVersionExecutionLease(WindowsInstalledVersionExecutionLease &&a_other) noexcept;
    /// @brief 現在のLeaseを解放して所有権を移動する
    WindowsInstalledVersionExecutionLease &operator=(WindowsInstalledVersionExecutionLease &&a_other) noexcept;
    /// @brief 共有Execution Lease Handleを閉じる
    ~WindowsInstalledVersionExecutionLease() noexcept;
    /// @brief 対象Childへ継承するNative Handle値を返す
    [[nodiscard]] std::uintptr_t native_handle() const noexcept;
    /// @brief Leaseが保護するInstall Rootを返す
    [[nodiscard]] const std::string &install_root() const noexcept;
    /// @brief Leaseが保護するVersion Directoryを返す
    [[nodiscard]] const std::string &version_directory() const noexcept;
    /// @brief 再検証済みEngine Version文字列を返す
    [[nodiscard]] const std::string &engine_version() const noexcept;
    /// @brief 再検証済みBundle Identityを返す
    [[nodiscard]] const std::string &bundle_id() const noexcept;
    /// @brief 再検証済みDistribution Manifest Digestを返す
    [[nodiscard]] const std::string &manifest_digest() const noexcept;
    /// @brief 再検証済みEditor Entry Pointの絶対Pathを返す
    [[nodiscard]] const std::string &editor_executable() const noexcept;
    /// @brief CMake／Engine Source／Templateを含むImmutable Version Rootを返す
    [[nodiscard]] const std::string &engine_source_root() const noexcept;
    /// @brief Manifestで固定されたEngine Source Revisionを返す
    [[nodiscard]] const std::string &engine_source_revision() const noexcept;
    /// @brief Manifestで固定されたSource Inventory Hashを返す
    [[nodiscard]] const std::string &source_inventory_hash() const noexcept;
    /// @brief Manifest Publisher Build IdentityのCanonical Digestを返す
    [[nodiscard]] const std::string &publisher_build_identity_digest() const noexcept;

  private:
    friend Result<WindowsInstalledVersionExecutionLease> acquire_windows_installed_version_execution_lease(
        const WindowsInstalledVersionRequest &a_request, const AssertContext &a_assertContext) noexcept;
    friend Result<WindowsInstalledVersionExecutionLease> adopt_windows_inherited_version_execution_lease(
        const WindowsInheritedVersionExecutionLeaseRequest &a_request,
        const AssertContext &a_assertContext) noexcept;
    /// @brief 検証済み共有Handle、Identity、Entry PointからLeaseを構築する
    WindowsInstalledVersionExecutionLease(std::uintptr_t a_handle, std::string a_installRoot,
                                          std::string a_versionDirectory, std::string a_engineVersion,
                                          std::string a_bundleId, std::string a_manifestDigest,
                                          std::string a_editorExecutable, std::string a_engineSourceRoot,
                                          std::string a_engineSourceRevision, std::string a_sourceInventoryHash,
                                          std::string a_publisherBuildIdentityDigest) noexcept;

    std::uintptr_t m_handle = 0U;
    std::string m_installRoot;
    std::string m_versionDirectory;
    std::string m_engineVersion;
    std::string m_bundleId;
    std::string m_manifestDigest;
    std::string m_editorExecutable;
    std::string m_engineSourceRoot;
    std::string m_engineSourceRevision;
    std::string m_sourceInventoryHash;
    std::string m_publisherBuildIdentityDigest;
};

/// @brief 検証済みLocal Developer Source SDKを排他Install Transactionで導入する
///
/// 呼出中にInstall Rootの排他Control Leaseを保持する。同じBundleの未完了Journalは証明済みStageから再開し、
/// 成功時はVersion、Probe Marker、Version外Worker、Registryを再読込検証してからOperation状態を削除する。
[[nodiscard]] Result<WindowsInstallOutcome> install_windows_source_sdk(const WindowsInstallRequest &a_request,
                                                                       const AssertContext &a_assertContext) noexcept;

/// @brief 継承済みControl Lease所有下でVersion Payloadの読取専用自己診断を行う
///
/// RegistryとProject状態を変更せず、Journal、Manifest、Inventory、呼出元から渡されたIdentityだけを検証する。
[[nodiscard]] Result<void> run_windows_install_probe(const WindowsInstallProbeRequest &a_request,
                                                     const AssertContext &a_assertContext) noexcept;

/// @brief 検証済みInstalled VersionをRegistry選択VersionとしてAtomic Publishする
[[nodiscard]] Result<WindowsRollbackOutcome> rollback_windows_installed_version(
    const WindowsInstalledVersionRequest &a_request, const AssertContext &a_assertContext) noexcept;

/// @brief Installed Versions Registryと各VersionのManifest／Marker／Worker証拠を共有Control Lease下で列挙する
[[nodiscard]] Result<WindowsInstalledVersionsInspection> inspect_windows_installed_versions(
    std::string_view a_installRoot, const AssertContext &a_assertContext) noexcept;

/// @brief 起動直前のVersion Evidenceを共有Control Lease下で再検証して共有Execution Leaseを返す
[[nodiscard]] Result<WindowsInstalledVersionExecutionLease> acquire_windows_installed_version_execution_lease(
    const WindowsInstalledVersionRequest &a_request, const AssertContext &a_assertContext) noexcept;

/// @brief 親から継承したExecution LeaseのFile Identityを検証しChild所有Leaseへ切れ目なく引き継ぐ
[[nodiscard]] Result<WindowsInstalledVersionExecutionLease> adopt_windows_inherited_version_execution_lease(
    const WindowsInheritedVersionExecutionLeaseRequest &a_request, const AssertContext &a_assertContext) noexcept;

/// @brief 検証済みVersion外Workerへ回復可能Uninstall Transactionを委譲する
[[nodiscard]] Result<WindowsUninstallOutcome> uninstall_windows_installed_version(
    const WindowsInstalledVersionRequest &a_request, const AssertContext &a_assertContext) noexcept;

/// @brief 外部Install Workerが排他Control／Execution Lease下でUninstall Journalを再開する
[[nodiscard]] Result<void> run_windows_uninstall_worker(const WindowsUninstallWorkerRequest &a_request,
                                                        const AssertContext &a_assertContext) noexcept;
} // namespace cue::distribution
