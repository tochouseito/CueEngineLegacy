#pragma once

#include <Cue/Foundation/Error.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::distribution
{
/// @brief Developer Distribution境界の回復可能な失敗を分類するCode
enum class DistributionError : std::int64_t
{
    InvalidIdentity = 1,
    InvalidManifest = 2,
    UnsupportedManifestVersion = 3,
    NonCanonicalManifest = 4,
    InvalidPayloadPath = 5,
    InvalidPayloadRole = 6,
    DuplicatePayload = 7,
    MissingRequiredPayload = 8,
    InvalidDependencyIdentity = 9,
    DirtyRepository = 10,
    RepositoryChanged = 11,
    PayloadNotAllowlisted = 12,
    ForbiddenPayload = 13,
    SourceBlobMismatch = 14,
    SourceInventoryMismatch = 15,
    InvalidGeneratedTool = 16,
    ResourceLimitExceeded = 17,
    PlatformOperationFailed = 18,
    BuildFailed = 19,
    PublishConflict = 20,
    BundleValidationFailed = 21
};

/// @brief Distribution Domainの回復可能Errorを生成する
[[nodiscard]] Error make_distribution_error(const AssertContext &a_assertContext, DistributionError a_error,
                                            std::string_view a_summary) noexcept;
} // namespace cue::distribution
