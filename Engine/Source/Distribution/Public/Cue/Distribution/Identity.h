#pragma once

#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::distribution
{
/// @brief Developer SDKが対象とするHost／Target Architecture
enum class DistributionArchitecture : std::uint8_t
{
    X64
};

/// @brief 固定Commitの隔離Buildを識別するCanonical値
struct PublisherBuildIdentity final
{
    std::string builtFromRevision;
    std::string targetTriplet;
    DistributionArchitecture hostArchitecture = DistributionArchitecture::X64;
    DistributionArchitecture targetArchitecture = DistributionArchitecture::X64;
    std::string compilerVendor;
    std::string compilerVersion;
    std::string toolsetVersion;
    std::string crtLinkage;
    std::string crtVersion;
    std::string windowsSdkTargetVersion;
    std::string configuration;

    [[nodiscard]] bool operator==(const PublisherBuildIdentity &) const noexcept = default;
};

/// @brief 導入先で選択したToolchainに対応するDependency Binary Root Identity入力
struct DependencyBuildIdentity final
{
    std::string targetTriplet;
    DistributionArchitecture hostArchitecture = DistributionArchitecture::X64;
    DistributionArchitecture targetArchitecture = DistributionArchitecture::X64;
    std::string compilerVendor;
    std::string compilerVersion;
    std::string toolsetVersion;
    std::string crtLinkage;
    std::string crtVersion;
    std::string windowsSdkTargetVersion;

    [[nodiscard]] bool operator==(const DependencyBuildIdentity &) const noexcept = default;
};

/// @brief Canonical vcpkg定義だけからDependency Definition IDを導出する
[[nodiscard]] Result<std::string> make_dependency_definition_id(std::string_view a_vcpkgManifest,
                                                                std::string_view a_vcpkgConfiguration,
                                                                std::string_view a_vcpkgToolPin,
                                                                const AssertContext &a_assertContext) noexcept;

/// @brief Definition IDと検証済みToolchain IdentityからDependency Root IDを導出する
[[nodiscard]] Result<std::string> make_dependency_root_id(std::string_view a_definitionId,
                                                          const DependencyBuildIdentity &a_buildIdentity,
                                                          const AssertContext &a_assertContext) noexcept;

/// @brief 検証済みEngine VersionとBundle IDからVersions直下の単一Directory名を生成する
[[nodiscard]] Result<std::string> make_distribution_version_directory(std::string_view a_engineVersion,
                                                                      std::string_view a_bundleId,
                                                                      const AssertContext &a_assertContext) noexcept;

/// @brief 検証済みDependency Root IDからDependencies直下の単一Directory名を生成する
[[nodiscard]] Result<std::string> make_dependency_root_directory(std::string_view a_rootId,
                                                                 const AssertContext &a_assertContext) noexcept;

/// @brief Engine Versionが先頭ZeroなしMAJOR.MINOR.PATCHか返す
[[nodiscard]] bool is_canonical_engine_version(std::string_view a_value) noexcept;
/// @brief Bundle IDがlowercase canonical UUID v4か返す
[[nodiscard]] bool is_canonical_bundle_id(std::string_view a_value) noexcept;
/// @brief 値が64文字lowercase SHA-256 hexadecimalか返す
[[nodiscard]] bool is_canonical_sha256(std::string_view a_value) noexcept;
/// @brief Git Revisionがlowercase SHA-1またはSHA-256 hexadecimalか返す
[[nodiscard]] bool is_canonical_git_revision(std::string_view a_value) noexcept;
/// @brief Publisher Build Identityの全Fieldを検証する
[[nodiscard]] bool is_valid_publisher_build_identity(const PublisherBuildIdentity &a_identity) noexcept;
/// @brief Dependency Build Identityの全Fieldを検証する
[[nodiscard]] bool is_valid_dependency_build_identity(const DependencyBuildIdentity &a_identity) noexcept;
} // namespace cue::distribution
