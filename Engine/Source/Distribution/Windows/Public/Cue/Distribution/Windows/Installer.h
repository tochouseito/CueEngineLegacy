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
} // namespace cue::distribution
