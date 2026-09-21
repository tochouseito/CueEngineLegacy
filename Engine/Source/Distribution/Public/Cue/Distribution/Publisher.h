#pragma once

#include <Cue/Distribution/Manifest.h>
#include <Cue/Foundation/Result.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::distribution
{
/// @brief Publisher開始／Inventory後に再取得するRepository State Evidence
struct RepositoryStateEvidence final
{
    std::string headRevision;
    std::string indexTreeId;
    std::string trackedStateHash;
    std::string untrackedStateHash;
    bool clean = false;

    [[nodiscard]] bool operator==(const RepositoryStateEvidence &) const noexcept = default;
};

/// @brief 固定Commit BlobとStaging Byteの一致Evidence
struct SourceBlobEvidence final
{
    std::string relativePath;
    std::string gitBlobId;
    std::uint64_t commitByteSize = 0U;
    std::string commitSha256;
    std::uint64_t stagedByteSize = 0U;
    std::string stagedSha256;
};

/// @brief 隔離Release Buildから得た生成Toolの検証Evidence
struct GeneratedToolEvidence final
{
    DistributionFileRole role = DistributionFileRole::ProjectHub;
    std::string targetName;
    std::string relativePath;
    std::string configuration;
    std::string builtFromRevision;
    PublisherBuildIdentity publisherBuildIdentity;
    DistributionArchitecture peArchitecture = DistributionArchitecture::X64;
    std::uint64_t byteSize = 0U;
    std::string sha256;
};

/// @brief Sourceと生成Toolの検証結果をManifest Inventoryへ束ねる
struct DistributionPublisherInventory final
{
    std::string sourceInventoryHash;
    std::vector<DistributionFileEntry> files;
};

/// @brief 固定Commit Treeから配布可能なSource PathをRoleへ分類する
[[nodiscard]] Result<DistributionFileRole> classify_distribution_source_path(
    std::string_view a_relativePath, const AssertContext &a_assertContext) noexcept;

/// @brief Publisher開始時とInventory後のRepository Evidenceが同一か検証する
[[nodiscard]] Result<void> validate_repository_evidence(const RepositoryStateEvidence &a_started,
                                                        const RepositoryStateEvidence &a_completed,
                                                        const AssertContext &a_assertContext) noexcept;

/// @brief Commit Blob／Staging／生成Tool Evidenceを検証してCanonical Inventoryを構築する
[[nodiscard]] Result<DistributionPublisherInventory> make_distribution_publisher_inventory(
    std::span<const SourceBlobEvidence> a_sources, std::span<const GeneratedToolEvidence> a_tools,
    std::string_view a_fixedRevision, const PublisherBuildIdentity &a_publisherBuildIdentity,
    const AssertContext &a_assertContext) noexcept;

/// @brief Platform Adapterが読んだPayload Byte列のSHA-256をlowercase hexadecimalで返す
[[nodiscard]] Result<std::string> compute_distribution_sha256(std::span<const std::byte> a_bytes,
                                                               const AssertContext &a_assertContext) noexcept;
} // namespace cue::distribution
