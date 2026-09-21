#include <Cue/Distribution/Identity.h>

#include "Sha256.h"

#include <Cue/Distribution/Error.h>
#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <new>
#include <string>

namespace
{
constexpr std::size_t k_maximumIdentityFieldBytes = 128U;
constexpr std::size_t k_maximumDependencyDefinitionBytes = 1024U * 1024U;

/// @brief Allocation失敗をDistribution Fatalへ変換する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Cue.Distribution allocation failed");
}

/// @brief 予期しない例外をDistribution Fatalへ変換する
[[noreturn]] void terminate_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Cue.Distribution unexpected exception");
}

/// @brief lowercase hexadecimal文字か返す
[[nodiscard]] bool is_lower_hex(char a_value) noexcept
{
    return (a_value >= '0' && a_value <= '9') || (a_value >= 'a' && a_value <= 'f');
}

/// @brief Identity Fieldに使用できるASCII Tokenか返す
[[nodiscard]] bool is_identity_token(std::string_view a_value) noexcept
{
    if (a_value.empty() || a_value.size() > k_maximumIdentityFieldBytes)
    {
        return false;
    }
    for (const unsigned char value : a_value)
    {
        if (value < 0x21U || value > 0x7eU || value == '"' || value == '\\')
        {
            return false;
        }
    }
    return true;
}

/// @brief Path結合へ使用できる単一要素か再検証する
[[nodiscard]] bool is_single_path_element(std::string_view a_value) noexcept
{
    return !a_value.empty() && a_value != "." && a_value != ".." && a_value.find('/') == std::string_view::npos &&
           a_value.find('\\') == std::string_view::npos && a_value.find(':') == std::string_view::npos;
}

/// @brief Length付きByte列を曖昧性のないCanonical結合へ追加する
void append_field(std::string &a_output, std::string_view a_name, std::string_view a_value)
{
    a_output.append(a_name);
    a_output.push_back('=');
    a_output.append(std::to_string(a_value.size()));
    a_output.push_back(':');
    a_output.append(a_value);
    a_output.push_back('\n');
}

/// @brief ArchitectureをCanonical Tokenへ変換する
[[nodiscard]] constexpr std::string_view architecture_name(cue::distribution::DistributionArchitecture) noexcept
{
    return "x64";
}

/// @brief Canonical Version要素を読み進める
[[nodiscard]] bool consume_version_component(std::string_view a_value, std::size_t &a_offset) noexcept
{
    if (a_offset >= a_value.size() || a_value[a_offset] < '0' || a_value[a_offset] > '9')
    {
        return false;
    }
    if (a_value[a_offset] == '0' && a_offset + 1U < a_value.size() && a_value[a_offset + 1U] >= '0' &&
        a_value[a_offset + 1U] <= '9')
    {
        return false;
    }
    std::uint64_t component = 0U;
    const char *begin = a_value.data() + a_offset;
    const char *end = a_value.data() + a_value.size();
    const auto result = std::from_chars(begin, end, component);
    if (result.ec != std::errc{})
    {
        return false;
    }
    a_offset = static_cast<std::size_t>(result.ptr - a_value.data());
    return component <= 0xffffffffULL;
}

/// @brief Dependency定義Byte列が固定Commit由来のCanonical入力として扱えるか返す
[[nodiscard]] bool is_valid_definition_bytes(std::string_view a_value) noexcept
{
    if (a_value.empty() || a_value.size() > k_maximumDependencyDefinitionBytes || a_value.front() == '\xef')
    {
        return false;
    }
    for (const unsigned char value : a_value)
    {
        if (value == 0U)
        {
            return false;
        }
    }
    return true;
}
} // namespace

namespace cue::distribution
{
bool is_canonical_engine_version(std::string_view a_value) noexcept
{
    std::size_t offset = 0U;
    for (std::size_t component = 0U; component < 3U; ++component)
    {
        if (!consume_version_component(a_value, offset))
        {
            return false;
        }
        if (component < 2U)
        {
            if (offset >= a_value.size() || a_value[offset] != '.')
            {
                return false;
            }
            ++offset;
        }
    }
    return offset == a_value.size();
}

bool is_canonical_bundle_id(std::string_view a_value) noexcept
{
    if (a_value.size() != 36U || a_value[8U] != '-' || a_value[13U] != '-' || a_value[18U] != '-' ||
        a_value[23U] != '-' || a_value[14U] != '4' ||
        (a_value[19U] != '8' && a_value[19U] != '9' && a_value[19U] != 'a' && a_value[19U] != 'b'))
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_value.size(); ++index)
    {
        if (index == 8U || index == 13U || index == 18U || index == 23U)
        {
            continue;
        }
        if (!is_lower_hex(a_value[index]))
        {
            return false;
        }
    }
    return true;
}

bool is_canonical_sha256(std::string_view a_value) noexcept
{
    return a_value.size() == 64U &&
           std::ranges::all_of(a_value, [](char a_character) noexcept { return is_lower_hex(a_character); });
}

bool is_canonical_git_revision(std::string_view a_value) noexcept
{
    return (a_value.size() == 40U || a_value.size() == 64U) &&
           std::ranges::all_of(a_value, [](char a_character) noexcept { return is_lower_hex(a_character); });
}

bool is_valid_publisher_build_identity(const PublisherBuildIdentity &a_identity) noexcept
{
    return a_identity.hostArchitecture == DistributionArchitecture::X64 &&
           a_identity.targetArchitecture == DistributionArchitecture::X64 &&
           is_canonical_git_revision(a_identity.builtFromRevision) && is_identity_token(a_identity.targetTriplet) &&
           is_identity_token(a_identity.compilerVendor) && is_identity_token(a_identity.compilerVersion) &&
           is_identity_token(a_identity.toolsetVersion) && is_identity_token(a_identity.crtLinkage) &&
           is_identity_token(a_identity.crtVersion) && is_identity_token(a_identity.windowsSdkTargetVersion) &&
           a_identity.configuration == "Release";
}

bool is_valid_dependency_build_identity(const DependencyBuildIdentity &a_identity) noexcept
{
    return a_identity.hostArchitecture == DistributionArchitecture::X64 &&
           a_identity.targetArchitecture == DistributionArchitecture::X64 &&
           is_identity_token(a_identity.targetTriplet) && is_identity_token(a_identity.compilerVendor) &&
           is_identity_token(a_identity.compilerVersion) && is_identity_token(a_identity.toolsetVersion) &&
           is_identity_token(a_identity.crtLinkage) && is_identity_token(a_identity.crtVersion) &&
           is_identity_token(a_identity.windowsSdkTargetVersion);
}

Result<std::string> make_dependency_definition_id(std::string_view a_vcpkgManifest,
                                                  std::string_view a_vcpkgConfiguration,
                                                  std::string_view a_vcpkgToolPin,
                                                  const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_valid_definition_bytes(a_vcpkgManifest) || !is_valid_definition_bytes(a_vcpkgConfiguration) ||
            !is_valid_definition_bytes(a_vcpkgToolPin))
        {
            return Result<std::string>::failure(make_distribution_error(a_assertContext,
                                                                        DistributionError::InvalidDependencyIdentity,
                                                                        "Dependency definition input is invalid"));
        }
        std::string canonical;
        canonical.reserve(a_vcpkgManifest.size() + a_vcpkgConfiguration.size() + a_vcpkgToolPin.size() + 96U);
        append_field(canonical, "vcpkgManifest", a_vcpkgManifest);
        append_field(canonical, "vcpkgConfiguration", a_vcpkgConfiguration);
        append_field(canonical, "vcpkgToolPin", a_vcpkgToolPin);
        return Result<std::string>::success(distribution_private::sha256_text(canonical));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<std::string> make_dependency_root_id(std::string_view a_definitionId,
                                            const DependencyBuildIdentity &a_buildIdentity,
                                            const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_canonical_sha256(a_definitionId) || !is_valid_dependency_build_identity(a_buildIdentity))
        {
            return Result<std::string>::failure(make_distribution_error(a_assertContext,
                                                                        DistributionError::InvalidDependencyIdentity,
                                                                        "Dependency root identity input is invalid"));
        }
        std::string canonical;
        canonical.reserve(640U);
        append_field(canonical, "definitionId", a_definitionId);
        append_field(canonical, "targetTriplet", a_buildIdentity.targetTriplet);
        append_field(canonical, "hostArchitecture", architecture_name(a_buildIdentity.hostArchitecture));
        append_field(canonical, "targetArchitecture", architecture_name(a_buildIdentity.targetArchitecture));
        append_field(canonical, "compilerVendor", a_buildIdentity.compilerVendor);
        append_field(canonical, "compilerVersion", a_buildIdentity.compilerVersion);
        append_field(canonical, "toolsetVersion", a_buildIdentity.toolsetVersion);
        append_field(canonical, "crtLinkage", a_buildIdentity.crtLinkage);
        append_field(canonical, "crtVersion", a_buildIdentity.crtVersion);
        append_field(canonical, "windowsSdkTargetVersion", a_buildIdentity.windowsSdkTargetVersion);
        return Result<std::string>::success(distribution_private::sha256_text(canonical));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<std::string> make_distribution_version_directory(std::string_view a_engineVersion, std::string_view a_bundleId,
                                                        const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_canonical_engine_version(a_engineVersion) || !is_canonical_bundle_id(a_bundleId))
        {
            return Result<std::string>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidIdentity, "Distribution version identity is invalid"));
        }
        std::string name("v");
        name.append(a_engineVersion);
        name.append("--");
        name.append(a_bundleId);
        if (!is_single_path_element(name))
        {
            return Result<std::string>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidIdentity, "Distribution version directory is invalid"));
        }
        return Result<std::string>::success(std::move(name));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<std::string> make_dependency_root_directory(std::string_view a_rootId,
                                                   const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_canonical_sha256(a_rootId) || !is_single_path_element(a_rootId))
        {
            return Result<std::string>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidDependencyIdentity, "Dependency root ID is invalid"));
        }
        return Result<std::string>::success(std::string(a_rootId));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}
} // namespace cue::distribution
