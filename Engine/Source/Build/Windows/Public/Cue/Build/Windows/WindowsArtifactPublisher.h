#pragma once

#include <Cue/Build/Service.h>

#include <cstdint>
#include <memory>
#include <string>

namespace cue
{
class AssertContext;
class ProjectDescriptor;

/// @brief Windows Build Artifact公開の安定した失敗分類
enum class WindowsBuildArtifactError : std::int64_t
{
    InvalidSettings = 1,
    WorkspaceLockFailed,
    CandidateInvalid,
    ModuleContractMismatch,
    ArtifactLockFailed,
    ArtifactAlreadyExists,
    CurrentManifestFailed,
    CurrentManifestDurabilityUnknown,
    ArtifactVersionDurabilityUnknown,
    PublisherUnavailable,
    SecurityPolicyViolation,
    SignatureVerificationFailed,
    PublisherMismatch
};

/// @brief Installed Distribution Manifestから引き継ぐGit非依存のEngine Source Provenance
struct WindowsInstalledEngineSourceProvenance final
{
    std::string sourceRoot;
    std::string sourceRevision;
    std::string sourceInventoryHash;
    std::string publisherBuildIdentityDigest;
};

/// @brief Project契約を所有するWindows Build Artifact Publisherを構築する
///
/// Project RootとDescriptorは呼出中だけ借用し、必要なProject IDとCompatibilityを返却PublisherへCopyする。
/// AssertContextは返却Publisherが全操作で借用するため、Publisherより長く生存しなければならない。
/// Publisherは一つのGameBuildService Workerから直列使用し、Build／Artifact LockをProcess間で共有する。
/// 回復可能なPath、Lock、Metadata初期化失敗はErrorを返し、Allocation等の回復不能例外はFatalHandlerへ渡す。
[[nodiscard]] Result<std::unique_ptr<BuildArtifactPublisher>> create_windows_build_artifact_publisher(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor, const AssertContext &a_assertContext) noexcept;

/// @brief Installed SDKのManifest ProvenanceへBindingしたWindows Build Artifact Publisherを構築する
[[nodiscard]] Result<std::unique_ptr<BuildArtifactPublisher>> create_windows_build_artifact_publisher(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor,
    WindowsInstalledEngineSourceProvenance a_engineSourceProvenance,
    const AssertContext &a_assertContext) noexcept;

/// @brief Project契約へBindingしたWindows Build Artifact Store Readerを構築する
///
/// Project RootとDescriptorは呼出中だけ借用し、Project Identityは返却ReaderへCopyする。AssertContextはReaderより
/// 長く生存しなければならない。ReaderはCurrent再読込とInventory検証を同じShared Read Lease内で行う。
[[nodiscard]] Result<std::unique_ptr<BuildArtifactReader>> create_windows_build_artifact_reader(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor, const AssertContext &a_assertContext) noexcept;
} // namespace cue
