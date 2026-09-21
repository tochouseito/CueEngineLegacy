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
#include <string_view>

namespace
{
constexpr std::string_view k_approvedVcpkgRepository = "https://github.com/microsoft/vcpkg.git";

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

/// @brief Canonical Dependency JSONを固定順序で読むCursor
class DefinitionCursor final
{
  public:
    explicit DefinitionCursor(std::string_view a_bytes) noexcept : m_bytes(a_bytes)
    {
    }

    [[nodiscard]] bool expect(std::string_view a_literal) noexcept
    {
        if (m_bytes.substr(m_offset, a_literal.size()) != a_literal)
        {
            return false;
        }
        m_offset += a_literal.size();
        return true;
    }

    [[nodiscard]] bool parse_string(std::string_view &a_output) noexcept
    {
        if (!expect("\""))
        {
            return false;
        }
        const std::size_t start = m_offset;
        while (m_offset < m_bytes.size() && m_bytes[m_offset] != '"')
        {
            const unsigned char value = static_cast<unsigned char>(m_bytes[m_offset]);
            if (value < 0x21U || value > 0x7eU || value == '\\' || m_offset - start >= k_maximumIdentityFieldBytes)
            {
                return false;
            }
            ++m_offset;
        }
        if (m_offset >= m_bytes.size() || m_offset == start)
        {
            return false;
        }
        a_output = m_bytes.substr(start, m_offset - start);
        ++m_offset;
        return true;
    }

    [[nodiscard]] bool parse_boolean(bool &a_output) noexcept
    {
        if (expect("false"))
        {
            a_output = false;
            return true;
        }
        if (expect("true"))
        {
            a_output = true;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool finished() const noexcept
    {
        return m_offset == m_bytes.size();
    }

  private:
    std::string_view m_bytes;
    std::size_t m_offset = 0U;
};

/// @brief Dependency定義Byte列の共通Envelopeを検証する
[[nodiscard]] bool has_valid_definition_envelope(std::string_view a_value) noexcept
{
    if (a_value.empty() || a_value.size() > k_maximumDependencyDefinitionBytes || a_value.front() == '\xef' ||
        !a_value.ends_with('\n'))
    {
        return false;
    }
    return std::ranges::none_of(a_value, [](unsigned char a_character) noexcept
                                { return a_character == 0U || a_character == '\r'; });
}

/// @brief Canonical Tokenとして使用できるDependency定義Stringか返す
[[nodiscard]] bool is_definition_token(std::string_view a_value) noexcept
{
    return !a_value.empty() && a_value.size() <= k_maximumIdentityFieldBytes;
}

/// @brief Canonical vcpkg.json Schemaを検証する
[[nodiscard]] bool is_canonical_vcpkg_manifest(std::string_view a_value) noexcept
{
    if (!has_valid_definition_envelope(a_value))
    {
        return false;
    }
    DefinitionCursor cursor(a_value);
    std::string_view name;
    std::string_view version;
    if (!cursor.expect("{\n    \"name\": ") || !cursor.parse_string(name) || name != "cue-engine" ||
        !cursor.expect(",\n    \"version-string\": ") || !cursor.parse_string(version) ||
        !is_definition_token(version) || !cursor.expect(",\n    \"dependencies\": [\n"))
    {
        return false;
    }

    std::string_view previousDependency;
    bool firstDependency = true;
    while (true)
    {
        if (!firstDependency && !cursor.expect(",\n"))
        {
            return false;
        }
        std::string_view dependency;
        bool defaultFeatures = false;
        if (!cursor.expect("        {\n            \"name\": ") || !cursor.parse_string(dependency) ||
            !is_definition_token(dependency) || (!previousDependency.empty() && dependency <= previousDependency) ||
            !cursor.expect(",\n            \"default-features\": ") || !cursor.parse_boolean(defaultFeatures) ||
            !cursor.expect(",\n            \"features\": [\n"))
        {
            return false;
        }
        previousDependency = dependency;

        std::string_view previousFeature;
        bool firstFeature = true;
        while (true)
        {
            if (!firstFeature && !cursor.expect(",\n"))
            {
                return false;
            }
            std::string_view feature;
            if (!cursor.expect("                ") || !cursor.parse_string(feature) || !is_definition_token(feature) ||
                (!previousFeature.empty() && feature <= previousFeature))
            {
                return false;
            }
            previousFeature = feature;
            firstFeature = false;
            if (cursor.expect("\n            ]"))
            {
                break;
            }
        }
        if (!cursor.expect("\n        }"))
        {
            return false;
        }
        firstDependency = false;
        if (cursor.expect("\n    ]"))
        {
            break;
        }
    }
    return cursor.expect("\n}\n") && cursor.finished();
}

/// @brief Canonical vcpkg-configuration.json Schemaを検証する
[[nodiscard]] bool is_canonical_vcpkg_configuration(std::string_view a_value) noexcept
{
    if (!has_valid_definition_envelope(a_value))
    {
        return false;
    }
    DefinitionCursor cursor(a_value);
    std::string_view kind;
    std::string_view baseline;
    return cursor.expect("{\n    \"default-registry\": {\n        \"kind\": ") && cursor.parse_string(kind) &&
           kind == "builtin" && cursor.expect(",\n        \"baseline\": ") && cursor.parse_string(baseline) &&
           cue::distribution::is_canonical_git_revision(baseline) && cursor.expect("\n    }\n}\n") && cursor.finished();
}

/// @brief Canonical vcpkg-tool.json Schemaを検証する
[[nodiscard]] bool is_canonical_vcpkg_tool_pin(std::string_view a_value) noexcept
{
    if (!has_valid_definition_envelope(a_value))
    {
        return false;
    }
    DefinitionCursor cursor(a_value);
    std::string_view repository;
    std::string_view commit;
    std::string_view release;
    std::string_view windowsVersion;
    std::string_view windowsSha256;
    std::string_view sourceSha512;
    return cursor.expect("{\n    \"repository\": ") && cursor.parse_string(repository) &&
           repository == k_approvedVcpkgRepository &&
           cursor.expect(",\n    \"commit\": ") && cursor.parse_string(commit) &&
           cue::distribution::is_canonical_git_revision(commit) && cursor.expect(",\n    \"release\": ") &&
           cursor.parse_string(release) && is_definition_token(release) &&
           cursor.expect(",\n    \"windowsX64Version\": ") && cursor.parse_string(windowsVersion) &&
           is_definition_token(windowsVersion) && cursor.expect(",\n    \"windowsX64Sha256\": ") &&
           cursor.parse_string(windowsSha256) && cue::distribution::is_canonical_sha256(windowsSha256) &&
           cursor.expect(",\n    \"sourceSha512\": ") && cursor.parse_string(sourceSha512) &&
           sourceSha512.size() == 128U &&
           std::ranges::all_of(sourceSha512, [](char a_character) noexcept { return is_lower_hex(a_character); }) &&
           cursor.expect("\n}\n") && cursor.finished();
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
           is_canonical_git_revision(a_identity.builtFromRevision) && a_identity.targetTriplet == "x64-windows" &&
           a_identity.compilerVendor == "msvc" && is_identity_token(a_identity.compilerVersion) &&
           is_identity_token(a_identity.toolsetVersion) && a_identity.crtLinkage == "dynamic" &&
           is_identity_token(a_identity.crtVersion) && is_identity_token(a_identity.windowsSdkTargetVersion) &&
           a_identity.configuration == "Release";
}

bool is_valid_dependency_build_identity(const DependencyBuildIdentity &a_identity) noexcept
{
    return a_identity.hostArchitecture == DistributionArchitecture::X64 &&
           a_identity.targetArchitecture == DistributionArchitecture::X64 &&
           a_identity.targetTriplet == "x64-windows" && a_identity.compilerVendor == "msvc" &&
           is_identity_token(a_identity.compilerVersion) && is_identity_token(a_identity.toolsetVersion) &&
           a_identity.crtLinkage == "dynamic" && is_identity_token(a_identity.crtVersion) &&
           is_identity_token(a_identity.windowsSdkTargetVersion);
}

Result<std::string> make_dependency_definition_id(std::string_view a_vcpkgManifest,
                                                  std::string_view a_vcpkgConfiguration,
                                                  std::string_view a_vcpkgToolPin,
                                                  const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_canonical_vcpkg_manifest(a_vcpkgManifest) || !is_canonical_vcpkg_configuration(a_vcpkgConfiguration) ||
            !is_canonical_vcpkg_tool_pin(a_vcpkgToolPin))
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
