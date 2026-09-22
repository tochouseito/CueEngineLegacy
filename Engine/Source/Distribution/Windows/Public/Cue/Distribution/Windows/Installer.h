#pragma once

#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <string>

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

/// @brief 継承済みGate HandleからUninstall Journalを再開する外部Worker要求
struct WindowsUninstallWorkerRequest final
{
    std::uintptr_t gateHandle = 0U;
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

  private:
    friend Result<WindowsInstalledVersionExecutionLease> acquire_windows_installed_version_execution_lease(
        const WindowsInstalledVersionRequest &a_request, const AssertContext &a_assertContext) noexcept;
    /// @brief 検証済み共有HandleとIdentityからLeaseを構築する
    WindowsInstalledVersionExecutionLease(std::uintptr_t a_handle, std::string a_installRoot,
                                          std::string a_versionDirectory) noexcept;

    std::uintptr_t m_handle = 0U;
    std::string m_installRoot;
    std::string m_versionDirectory;
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

/// @brief 起動直前のVersion Evidenceを共有Control Lease下で再検証して共有Execution Leaseを返す
[[nodiscard]] Result<WindowsInstalledVersionExecutionLease> acquire_windows_installed_version_execution_lease(
    const WindowsInstalledVersionRequest &a_request, const AssertContext &a_assertContext) noexcept;

/// @brief 検証済みVersion外Workerへ回復可能Uninstall Transactionを委譲する
[[nodiscard]] Result<WindowsUninstallOutcome> uninstall_windows_installed_version(
    const WindowsInstalledVersionRequest &a_request, const AssertContext &a_assertContext) noexcept;

/// @brief 外部Install Workerが排他Control／Execution Lease下でUninstall Journalを再開する
[[nodiscard]] Result<void> run_windows_uninstall_worker(const WindowsUninstallWorkerRequest &a_request,
                                                        const AssertContext &a_assertContext) noexcept;
} // namespace cue::distribution
