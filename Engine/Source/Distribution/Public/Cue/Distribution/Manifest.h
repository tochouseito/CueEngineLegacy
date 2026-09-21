#pragma once

#include <Cue/Distribution/Identity.h>
#include <Cue/Foundation/Result.h>

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
/// @brief Developer Source SDK Inventoryに登録できるPayload Role
enum class DistributionFileRole : std::uint8_t
{
    EngineSource,
    Hlsl,
    CMake,
    Script,
    Template,
    Document,
    License,
    DependencyDefinition,
    ThirdPartyNotice,
    ThirdPartyLicense,
    Bootstrap,
    ProjectHub,
    Editor,
    RuntimeHost,
    Installer,
    InstallWorker
};

/// @brief 最低Toolchain契約をCanonical Version文字列で保持する
struct MinimumToolchain final
{
    std::string cmakeVersion;
    std::string gitVersion;
    std::string compilerVendor;
    std::string compilerVersion;
    std::string windowsSdkVersion;

    [[nodiscard]] bool operator==(const MinimumToolchain &) const noexcept = default;
};

/// @brief Tool起動に使うManifest検証済みRelative Path
struct DistributionEntryPoints final
{
    std::string bootstrap;
    std::string projectHub;
    std::string editor;
    std::string runtimeHost;
    std::string installer;
    std::string installWorker;

    [[nodiscard]] bool operator==(const DistributionEntryPoints &) const noexcept = default;
};

/// @brief 一つのPayload FileのRole、Path、Size、Digest
struct DistributionFileEntry final
{
    DistributionFileRole role = DistributionFileRole::EngineSource;
    std::string relativePath;
    std::uint64_t byteSize = 0U;
    std::string sha256;

    [[nodiscard]] bool operator==(const DistributionFileEntry &) const noexcept = default;
};

/// @brief CueEngineDistribution.json v1の所有値
struct DistributionManifest final
{
    std::string bundleId;
    std::string engineVersion;
    std::string engineSourceRevision;
    std::string sourceInventoryHash;
    std::string dependencyDefinitionId;
    PublisherBuildIdentity publisherBuildIdentity;
    MinimumToolchain minimumToolchain;
    DistributionEntryPoints entryPoints;
    std::vector<DistributionFileEntry> files;

    [[nodiscard]] bool operator==(const DistributionManifest &) const noexcept = default;
};

/// @brief 検証済みManifestをCanonical UTF-8、BOMなし、LF終端JSONへ直列化する
[[nodiscard]] Result<std::string> write_distribution_manifest(const DistributionManifest &a_manifest,
                                                              const AssertContext &a_assertContext) noexcept;

/// @brief Canonical CueEngineDistribution.json v1だけをFail-closedで読み込む
[[nodiscard]] Result<DistributionManifest> read_distribution_manifest(std::string_view a_bytes,
                                                                      const AssertContext &a_assertContext) noexcept;

/// @brief Bundle相対PathがCanonicalでRoot外参照を含まないか返す
[[nodiscard]] bool is_canonical_distribution_path(std::string_view a_path) noexcept;
/// @brief RoleをCanonical Manifest文字列へ変換する
[[nodiscard]] std::string_view distribution_file_role_name(DistributionFileRole a_role) noexcept;
} // namespace cue::distribution
