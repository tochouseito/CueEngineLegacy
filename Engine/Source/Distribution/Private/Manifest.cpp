#include <Cue/Distribution/Manifest.h>

#include <Cue/Distribution/Error.h>
#include <Cue/Distribution/Publisher.h>
#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_maximumManifestBytes = 16U * 1024U * 1024U;
constexpr std::size_t k_maximumManifestFiles = 65536U;
constexpr std::size_t k_maximumPathBytes = 1024U;
constexpr std::size_t k_maximumSegmentBytes = 255U;
constexpr std::size_t k_maximumJsonStringBytes = 4096U;

/// @brief M18の配布Pathとして使用できるportable printable ASCIIか返す
[[nodiscard]] bool is_portable_path_character(unsigned char a_value) noexcept
{
    return a_value >= 0x20U && a_value <= 0x7eU;
}

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

/// @brief UTF-8継続Byteか返す
[[nodiscard]] bool is_continuation(std::uint8_t a_value) noexcept
{
    return (a_value & 0xc0U) == 0x80U;
}

/// @brief ASCIIだけをCase-insensitive比較する
[[nodiscard]] bool equals_ascii_insensitive(std::string_view a_left, std::string_view a_right) noexcept
{
    if (a_left.size() != a_right.size())
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_left.size(); ++index)
    {
        char left = a_left[index];
        char right = a_right[index];
        if (left >= 'A' && left <= 'Z')
        {
            left = static_cast<char>(left + ('a' - 'A'));
        }
        if (right >= 'A' && right <= 'Z')
        {
            right = static_cast<char>(right + ('a' - 'A'));
        }
        if (left != right)
        {
            return false;
        }
    }
    return true;
}

/// @brief WindowsがDeviceへ解釈する予約Segmentか返す
[[nodiscard]] bool is_windows_reserved_segment(std::string_view a_segment) noexcept
{
    const std::size_t dot = a_segment.find('.');
    const std::string_view stem = a_segment.substr(0U, dot);
    if (equals_ascii_insensitive(stem, "con") || equals_ascii_insensitive(stem, "prn") ||
        equals_ascii_insensitive(stem, "aux") || equals_ascii_insensitive(stem, "nul"))
    {
        return true;
    }
    if (stem.size() == 4U && stem[3U] >= '1' && stem[3U] <= '9')
    {
        return equals_ascii_insensitive(stem.substr(0U, 3U), "com") ||
               equals_ascii_insensitive(stem.substr(0U, 3U), "lpt");
    }
    return false;
}

/// @brief Windows上のPortable比較用ASCII lowercase Keyを作る
[[nodiscard]] std::string make_portable_path_key(std::string_view a_path)
{
    std::string result(a_path);
    std::ranges::transform(
        result, result.begin(), [](char a_value) noexcept
        { return a_value >= 'A' && a_value <= 'Z' ? static_cast<char>(a_value + ('a' - 'A')) : a_value; });
    return result;
}

/// @brief UTF-8 Scalar列が正しくControl文字を含まないか返す
[[nodiscard]] bool is_valid_utf8_text(std::string_view a_value) noexcept
{
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(a_value.data());
    for (std::size_t index = 0U; index < a_value.size();)
    {
        const std::uint8_t first = bytes[index];
        if (first <= 0x7fU)
        {
            if (first <= 0x1fU || first == 0x7fU)
            {
                return false;
            }
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU)
        {
            if (index + 1U >= a_value.size() || !is_continuation(bytes[index + 1U]))
            {
                return false;
            }
            index += 2U;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU)
        {
            if (index + 2U >= a_value.size() || !is_continuation(bytes[index + 1U]) ||
                !is_continuation(bytes[index + 2U]))
            {
                return false;
            }
            const std::uint8_t second = bytes[index + 1U];
            if ((first == 0xe0U && second < 0xa0U) || (first == 0xedU && second >= 0xa0U))
            {
                return false;
            }
            index += 3U;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U)
        {
            if (index + 3U >= a_value.size() || !is_continuation(bytes[index + 1U]) ||
                !is_continuation(bytes[index + 2U]) || !is_continuation(bytes[index + 3U]))
            {
                return false;
            }
            const std::uint8_t second = bytes[index + 1U];
            if ((first == 0xf0U && second < 0x90U) || (first == 0xf4U && second > 0x8fU))
            {
                return false;
            }
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

/// @brief JSON Escape不要の安全なManifest文字列か返す
[[nodiscard]] bool is_manifest_string(std::string_view a_value, std::size_t a_maximumBytes) noexcept
{
    return !a_value.empty() && a_value.size() <= a_maximumBytes && is_valid_utf8_text(a_value) &&
           a_value.find('"') == std::string_view::npos && a_value.find('\\') == std::string_view::npos;
}

/// @brief 数値Version Tokenか返す
[[nodiscard]] bool is_tool_version(std::string_view a_value) noexcept
{
    if (a_value.empty() || a_value.size() > 64U || a_value.front() == '.' || a_value.back() == '.')
    {
        return false;
    }
    std::size_t componentStart = 0U;
    std::size_t componentCount = 0U;
    for (std::size_t index = 0U; index <= a_value.size(); ++index)
    {
        if (index < a_value.size() && a_value[index] != '.')
        {
            if (a_value[index] < '0' || a_value[index] > '9')
            {
                return false;
            }
            continue;
        }
        const std::size_t length = index - componentStart;
        if (length == 0U || (length > 1U && a_value[componentStart] == '0'))
        {
            return false;
        }
        ++componentCount;
        componentStart = index + 1U;
    }
    return componentCount >= 2U && componentCount <= 4U;
}

/// @brief 数値Versionが指定下限以上か返す
[[nodiscard]] bool is_tool_version_at_least(std::string_view a_value, std::string_view a_minimum) noexcept
{
    if (!is_tool_version(a_value) || !is_tool_version(a_minimum))
    {
        return false;
    }
    const auto split = [](std::string_view a_version) noexcept
    {
        std::array<std::string_view, 4U> components{"0", "0", "0", "0"};
        std::size_t start = 0U;
        std::size_t count = 0U;
        while (start < a_version.size())
        {
            const std::size_t end = a_version.find('.', start);
            components[count++] = a_version.substr(
                start, end == std::string_view::npos ? a_version.size() - start : end - start);
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1U;
        }
        return components;
    };
    const auto valueComponents = split(a_value);
    const auto minimumComponents = split(a_minimum);
    for (std::size_t index = 0U; index < valueComponents.size(); ++index)
    {
        if (valueComponents[index].size() != minimumComponents[index].size())
        {
            return valueComponents[index].size() > minimumComponents[index].size();
        }
        if (valueComponents[index] != minimumComponents[index])
        {
            return valueComponents[index] > minimumComponents[index];
        }
    }
    return true;
}

/// @brief 生成Binary Roleか返す
[[nodiscard]] bool is_tool_role(cue::distribution::DistributionFileRole a_role) noexcept
{
    using cue::distribution::DistributionFileRole;
    return a_role == DistributionFileRole::Bootstrap || a_role == DistributionFileRole::ProjectHub ||
           a_role == DistributionFileRole::Editor || a_role == DistributionFileRole::RuntimeHost ||
           a_role == DistributionFileRole::Installer || a_role == DistributionFileRole::InstallWorker;
}

/// @brief 生成Tool Roleに対応する固定Bundle Path
struct ExpectedToolPath final
{
    cue::distribution::DistributionFileRole role;
    std::string_view path;
};

constexpr std::array k_expectedToolPaths = {
    ExpectedToolPath{cue::distribution::DistributionFileRole::Bootstrap, "Bin/CueEngineBootstrap.exe"},
    ExpectedToolPath{cue::distribution::DistributionFileRole::ProjectHub, "Bin/CueProjectHubTool.exe"},
    ExpectedToolPath{cue::distribution::DistributionFileRole::Editor, "Bin/CueEditorTool.exe"},
    ExpectedToolPath{cue::distribution::DistributionFileRole::RuntimeHost, "Bin/CueRuntimeHost.exe"},
    ExpectedToolPath{cue::distribution::DistributionFileRole::Installer, "Bin/CueEngineInstallerTool.exe"},
    ExpectedToolPath{cue::distribution::DistributionFileRole::InstallWorker, "Bin/CueEngineInstallWorker.exe"},
};

/// @brief Tool Roleに対応する固定Bundle Pathを返す
[[nodiscard]] std::string_view expected_tool_path(cue::distribution::DistributionFileRole a_role) noexcept
{
    const auto iterator =
        std::ranges::find_if(k_expectedToolPaths, [a_role](const ExpectedToolPath &a_expected) noexcept
                             { return a_expected.role == a_role; });
    return iterator == k_expectedToolPaths.end() ? std::string_view{} : iterator->path;
}

/// @brief Inventoryが指定Roleを一つ以上含むか返す
[[nodiscard]] bool has_role(std::span<const cue::distribution::DistributionFileEntry> a_files,
                            cue::distribution::DistributionFileRole a_role) noexcept
{
    return std::ranges::any_of(a_files, [a_role](const auto &a_file) noexcept { return a_file.role == a_role; });
}

/// @brief Inventoryが指定Pathを含むか返す
[[nodiscard]] bool has_path(std::span<const cue::distribution::DistributionFileEntry> a_files,
                            std::string_view a_path) noexcept
{
    return std::ranges::any_of(a_files,
                               [a_path](const auto &a_file) noexcept { return a_file.relativePath == a_path; });
}

/// @brief Entry Point Pathと対応Roleを一つのInventoryへ照合する
[[nodiscard]] bool has_entry_point(std::span<const cue::distribution::DistributionFileEntry> a_files,
                                   std::string_view a_path, cue::distribution::DistributionFileRole a_role) noexcept
{
    return std::ranges::any_of(a_files, [a_path, a_role](const auto &a_file) noexcept
                               { return a_file.role == a_role && a_file.relativePath == a_path; });
}

/// @brief Manifest全Valueの不変条件を検証する
[[nodiscard]] cue::Result<void> validate_manifest(const cue::distribution::DistributionManifest &a_manifest,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    using namespace cue::distribution;
    if (!is_canonical_bundle_id(a_manifest.bundleId) || !is_canonical_engine_version(a_manifest.engineVersion) ||
        !is_canonical_git_revision(a_manifest.engineSourceRevision) ||
        !is_canonical_sha256(a_manifest.sourceInventoryHash) ||
        !is_canonical_sha256(a_manifest.dependencyDefinitionId) ||
        !is_valid_publisher_build_identity(a_manifest.publisherBuildIdentity) ||
        a_manifest.publisherBuildIdentity.builtFromRevision != a_manifest.engineSourceRevision)
    {
        return cue::Result<void>::failure(make_distribution_error(a_assertContext, DistributionError::InvalidIdentity,
                                                                  "Distribution Manifest identity is invalid"));
    }
    if (!is_tool_version_at_least(a_manifest.minimumToolchain.cmakeVersion, "4.2.0") ||
        !is_tool_version_at_least(a_manifest.minimumToolchain.gitVersion, "2.44.0") ||
        a_manifest.minimumToolchain.compilerVendor != "msvc" ||
        !is_tool_version(a_manifest.minimumToolchain.compilerVersion) ||
        !is_tool_version(a_manifest.minimumToolchain.windowsSdkVersion))
    {
        return cue::Result<void>::failure(make_distribution_error(a_assertContext, DistributionError::InvalidManifest,
                                                                  "Minimum Toolchain identity is invalid"));
    }
    if (a_manifest.files.empty() || a_manifest.files.size() > k_maximumManifestFiles)
    {
        return cue::Result<void>::failure(make_distribution_error(
            a_assertContext, DistributionError::ResourceLimitExceeded, "Distribution Manifest file count is invalid"));
    }

    std::vector<std::string> paths;
    try
    {
        paths.reserve(a_manifest.files.size());
        for (const DistributionFileEntry &file : a_manifest.files)
        {
            if (distribution_file_role_name(file.role).empty() || !is_canonical_distribution_path(file.relativePath) ||
                file.relativePath == "CueEngineDistribution.json" || !is_canonical_sha256(file.sha256) ||
                (is_tool_role(file.role) && file.byteSize == 0U))
            {
                return cue::Result<void>::failure(
                    make_distribution_error(a_assertContext, DistributionError::InvalidManifest,
                                            "Distribution Manifest file entry is invalid"));
            }
            if (is_tool_role(file.role))
            {
                if (file.relativePath != expected_tool_path(file.role))
                {
                    return cue::Result<void>::failure(
                        make_distribution_error(a_assertContext, DistributionError::InvalidManifest,
                                                "Generated Tool role and path do not match"));
                }
            }
            else
            {
                cue::Result<DistributionFileRole> classified =
                    classify_distribution_source_path(file.relativePath, a_assertContext);
                if (!classified || *classified.try_value() != file.role)
                {
                    return cue::Result<void>::failure(
                        make_distribution_error(a_assertContext, DistributionError::InvalidManifest,
                                                "Source payload role and path do not match"));
                }
            }
            paths.push_back(make_portable_path_key(file.relativePath));
        }
        std::ranges::sort(paths);
        if (std::adjacent_find(paths.begin(), paths.end()) != paths.end())
        {
            return cue::Result<void>::failure(
                make_distribution_error(a_assertContext, DistributionError::DuplicatePayload,
                                        "Distribution Manifest contains duplicate paths"));
        }
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }

    constexpr std::array requiredSourceRoles = {
        DistributionFileRole::EngineSource,
        DistributionFileRole::Hlsl,
        DistributionFileRole::CMake,
        DistributionFileRole::Script,
        DistributionFileRole::Document,
        DistributionFileRole::License,
        DistributionFileRole::DependencyDefinition,
        DistributionFileRole::ThirdPartyNotice,
        DistributionFileRole::ThirdPartyLicense,
    };
    if (std::ranges::any_of(requiredSourceRoles, [&a_manifest](DistributionFileRole a_role) noexcept
                            { return !has_role(a_manifest.files, a_role); }))
    {
        return cue::Result<void>::failure(
            make_distribution_error(a_assertContext, DistributionError::MissingRequiredPayload,
                                    "Required source-managed Distribution payload is missing from Manifest"));
    }
    constexpr std::array requiredSourcePaths = {
        std::string_view("Tools/Dependencies/RestoreVcpkg.ps1"), std::string_view("ThirdParty/vcpkg.json"),
        std::string_view("ThirdParty/vcpkg-configuration.json"), std::string_view("ThirdParty/vcpkg-tool.json"),
        std::string_view("ThirdParty/THIRD_PARTY_NOTICES.md"),   std::string_view("LICENSE.txt"),
    };
    if (std::ranges::any_of(requiredSourcePaths, [&a_manifest](std::string_view a_path) noexcept
                            { return !has_path(a_manifest.files, a_path); }))
    {
        return cue::Result<void>::failure(
            make_distribution_error(a_assertContext, DistributionError::MissingRequiredPayload,
                                    "Required fixed-path Distribution payload is missing from Manifest"));
    }

    const DistributionEntryPoints &entryPoints = a_manifest.entryPoints;
    if (entryPoints.bootstrap != expected_tool_path(DistributionFileRole::Bootstrap) ||
        entryPoints.projectHub != expected_tool_path(DistributionFileRole::ProjectHub) ||
        entryPoints.editor != expected_tool_path(DistributionFileRole::Editor) ||
        entryPoints.runtimeHost != expected_tool_path(DistributionFileRole::RuntimeHost) ||
        entryPoints.installer != expected_tool_path(DistributionFileRole::Installer) ||
        entryPoints.installWorker != expected_tool_path(DistributionFileRole::InstallWorker) ||
        !has_entry_point(a_manifest.files, entryPoints.bootstrap, DistributionFileRole::Bootstrap) ||
        !has_entry_point(a_manifest.files, entryPoints.projectHub, DistributionFileRole::ProjectHub) ||
        !has_entry_point(a_manifest.files, entryPoints.editor, DistributionFileRole::Editor) ||
        !has_entry_point(a_manifest.files, entryPoints.runtimeHost, DistributionFileRole::RuntimeHost) ||
        !has_entry_point(a_manifest.files, entryPoints.installer, DistributionFileRole::Installer) ||
        !has_entry_point(a_manifest.files, entryPoints.installWorker, DistributionFileRole::InstallWorker))
    {
        return cue::Result<void>::failure(
            make_distribution_error(a_assertContext, DistributionError::MissingRequiredPayload,
                                    "Distribution entry point is missing from Inventory"));
    }
    return cue::Result<void>::success();
}

/// @brief Role文字列をEnumへ変換する
[[nodiscard]] std::optional<cue::distribution::DistributionFileRole> parse_role(std::string_view a_value) noexcept
{
    using cue::distribution::DistributionFileRole;
    constexpr std::array roles = {
        DistributionFileRole::EngineSource,
        DistributionFileRole::Hlsl,
        DistributionFileRole::CMake,
        DistributionFileRole::Script,
        DistributionFileRole::Template,
        DistributionFileRole::Document,
        DistributionFileRole::License,
        DistributionFileRole::DependencyDefinition,
        DistributionFileRole::ThirdPartyNotice,
        DistributionFileRole::ThirdPartyLicense,
        DistributionFileRole::Bootstrap,
        DistributionFileRole::ProjectHub,
        DistributionFileRole::Editor,
        DistributionFileRole::RuntimeHost,
        DistributionFileRole::Installer,
        DistributionFileRole::InstallWorker,
    };
    for (const DistributionFileRole role : roles)
    {
        if (cue::distribution::distribution_file_role_name(role) == a_value)
        {
            return role;
        }
    }
    return std::nullopt;
}

/// @brief Canonical JSONの固定順序だけを読むCursor
class ManifestCursor final
{
  public:
    explicit ManifestCursor(std::string_view a_bytes) noexcept : m_bytes(a_bytes)
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

    [[nodiscard]] bool parse_string(std::string &a_output)
    {
        if (!expect("\""))
        {
            return false;
        }
        const std::size_t start = m_offset;
        while (m_offset < m_bytes.size() && m_bytes[m_offset] != '"')
        {
            const unsigned char value = static_cast<unsigned char>(m_bytes[m_offset]);
            if (value <= 0x1fU || value == 0x7fU || value == '\\' || m_offset - start >= k_maximumJsonStringBytes)
            {
                return false;
            }
            ++m_offset;
        }
        if (m_offset >= m_bytes.size())
        {
            return false;
        }
        const std::string_view value = m_bytes.substr(start, m_offset - start);
        if (!is_valid_utf8_text(value))
        {
            return false;
        }
        a_output.assign(value);
        ++m_offset;
        return true;
    }

    [[nodiscard]] bool parse_uint64(std::uint64_t &a_output) noexcept
    {
        if (m_offset >= m_bytes.size() || m_bytes[m_offset] < '0' || m_bytes[m_offset] > '9')
        {
            return false;
        }
        if (m_bytes[m_offset] == '0' && m_offset + 1U < m_bytes.size() && m_bytes[m_offset + 1U] >= '0' &&
            m_bytes[m_offset + 1U] <= '9')
        {
            return false;
        }
        const char *begin = m_bytes.data() + m_offset;
        const char *end = m_bytes.data() + m_bytes.size();
        const auto result = std::from_chars(begin, end, a_output);
        if (result.ec != std::errc{})
        {
            return false;
        }
        m_offset = static_cast<std::size_t>(result.ptr - m_bytes.data());
        return true;
    }

    [[nodiscard]] bool finished() const noexcept
    {
        return m_offset == m_bytes.size();
    }

  private:
    std::string_view m_bytes;
    std::size_t m_offset = 0U;
};

/// @brief JSON String Memberを固定順序で読む
[[nodiscard]] bool parse_string_member(ManifestCursor &a_cursor, std::string_view a_prefix, std::string &a_value)
{
    return a_cursor.expect(a_prefix) && a_cursor.parse_string(a_value);
}

/// @brief Publisher Build Identityを固定Member順序で読む
[[nodiscard]] bool parse_publisher_identity(ManifestCursor &a_cursor,
                                            cue::distribution::PublisherBuildIdentity &a_identity)
{
    std::string hostArchitecture;
    std::string targetArchitecture;
    return parse_string_member(a_cursor, "\"builtFromRevision\":", a_identity.builtFromRevision) &&
           parse_string_member(a_cursor, ",\"targetTriplet\":", a_identity.targetTriplet) &&
           parse_string_member(a_cursor, ",\"hostArchitecture\":", hostArchitecture) && hostArchitecture == "x64" &&
           parse_string_member(a_cursor, ",\"targetArchitecture\":", targetArchitecture) &&
           targetArchitecture == "x64" &&
           parse_string_member(a_cursor, ",\"compilerVendor\":", a_identity.compilerVendor) &&
           parse_string_member(a_cursor, ",\"compilerVersion\":", a_identity.compilerVersion) &&
           parse_string_member(a_cursor, ",\"toolsetVersion\":", a_identity.toolsetVersion) &&
           parse_string_member(a_cursor, ",\"crtLinkage\":", a_identity.crtLinkage) &&
           parse_string_member(a_cursor, ",\"crtVersion\":", a_identity.crtVersion) &&
           parse_string_member(a_cursor, ",\"windowsSdkTargetVersion\":", a_identity.windowsSdkTargetVersion) &&
           parse_string_member(a_cursor, ",\"configuration\":", a_identity.configuration) && a_cursor.expect("}");
}

/// @brief StringをCanonical JSONへ追加する
void append_json_string(std::string &a_output, std::string_view a_value)
{
    a_output.push_back('"');
    a_output.append(a_value);
    a_output.push_back('"');
}

/// @brief Publisher Build IdentityをCanonical JSONへ追加する
void append_publisher_identity(std::string &a_output, const cue::distribution::PublisherBuildIdentity &a_identity)
{
    a_output.append("{\"builtFromRevision\":");
    append_json_string(a_output, a_identity.builtFromRevision);
    a_output.append(",\"targetTriplet\":");
    append_json_string(a_output, a_identity.targetTriplet);
    a_output.append(",\"hostArchitecture\":\"x64\",\"targetArchitecture\":\"x64\",\"compilerVendor\":");
    append_json_string(a_output, a_identity.compilerVendor);
    a_output.append(",\"compilerVersion\":");
    append_json_string(a_output, a_identity.compilerVersion);
    a_output.append(",\"toolsetVersion\":");
    append_json_string(a_output, a_identity.toolsetVersion);
    a_output.append(",\"crtLinkage\":");
    append_json_string(a_output, a_identity.crtLinkage);
    a_output.append(",\"crtVersion\":");
    append_json_string(a_output, a_identity.crtVersion);
    a_output.append(",\"windowsSdkTargetVersion\":");
    append_json_string(a_output, a_identity.windowsSdkTargetVersion);
    a_output.append(",\"configuration\":");
    append_json_string(a_output, a_identity.configuration);
    a_output.push_back('}');
}
} // namespace

namespace cue::distribution
{
bool is_canonical_distribution_path(std::string_view a_path) noexcept
{
    if (a_path.empty() || a_path.size() > k_maximumPathBytes || a_path.front() == '/' || a_path.back() == '/' ||
        !is_valid_utf8_text(a_path))
    {
        return false;
    }
    std::size_t segmentStart = 0U;
    for (std::size_t index = 0U; index <= a_path.size(); ++index)
    {
        if (index < a_path.size())
        {
            const char value = a_path[index];
            if (!is_portable_path_character(static_cast<unsigned char>(value)) || value == '\\' || value == ':' ||
                value == '"' || value == '<' || value == '>' || value == '|' || value == '?' || value == '*')
            {
                return false;
            }
            if (value != '/')
            {
                continue;
            }
        }
        const std::string_view segment = a_path.substr(segmentStart, index - segmentStart);
        if (segment.empty() || segment == "." || segment == ".." || segment.size() > k_maximumSegmentBytes ||
            segment.back() == '.' || segment.back() == ' ' || is_windows_reserved_segment(segment))
        {
            return false;
        }
        segmentStart = index + 1U;
    }
    return true;
}

std::string_view distribution_file_role_name(DistributionFileRole a_role) noexcept
{
    switch (a_role)
    {
    case DistributionFileRole::EngineSource:
        return "engineSource";
    case DistributionFileRole::Hlsl:
        return "hlsl";
    case DistributionFileRole::CMake:
        return "cmake";
    case DistributionFileRole::Script:
        return "script";
    case DistributionFileRole::Template:
        return "template";
    case DistributionFileRole::Document:
        return "document";
    case DistributionFileRole::License:
        return "license";
    case DistributionFileRole::DependencyDefinition:
        return "dependencyDefinition";
    case DistributionFileRole::ThirdPartyNotice:
        return "thirdPartyNotice";
    case DistributionFileRole::ThirdPartyLicense:
        return "thirdPartyLicense";
    case DistributionFileRole::Bootstrap:
        return "bootstrap";
    case DistributionFileRole::ProjectHub:
        return "projectHub";
    case DistributionFileRole::Editor:
        return "editor";
    case DistributionFileRole::RuntimeHost:
        return "runtimeHost";
    case DistributionFileRole::Installer:
        return "installer";
    case DistributionFileRole::InstallWorker:
        return "installWorker";
    }
    return {};
}

Result<std::string> write_distribution_manifest(const DistributionManifest &a_manifest,
                                                const AssertContext &a_assertContext) noexcept
{
    try
    {
        Result<void> validation = validate_manifest(a_manifest, a_assertContext);
        if (!validation)
        {
            return Result<std::string>::failure(std::move(*validation.try_error()));
        }
        std::vector<const DistributionFileEntry *> files;
        files.reserve(a_manifest.files.size());
        for (const DistributionFileEntry &file : a_manifest.files)
        {
            files.push_back(&file);
        }
        std::ranges::sort(files, [](const auto *a_left, const auto *a_right) noexcept
                          { return a_left->relativePath < a_right->relativePath; });

        std::string output;
        output.reserve(2048U + files.size() * 192U);
        output.append("{\"schemaVersion\":1,\"distributionKind\":\"DeveloperSourceSdk\",\"bundleId\":");
        append_json_string(output, a_manifest.bundleId);
        output.append(",\"engineVersion\":");
        append_json_string(output, a_manifest.engineVersion);
        output.append(",\"engineSourceRevision\":");
        append_json_string(output, a_manifest.engineSourceRevision);
        output.append(",\"engineSourceState\":\"clean\",\"sourceInventoryHash\":");
        append_json_string(output, a_manifest.sourceInventoryHash);
        output.append(",\"dependencyDefinitionId\":");
        append_json_string(output, a_manifest.dependencyDefinitionId);
        output.append(",\"publisherBuildIdentity\":");
        append_publisher_identity(output, a_manifest.publisherBuildIdentity);
        output.append(
            ",\"host\":{\"os\":\"windows\",\"architecture\":\"x64\"},\"minimumToolchain\":{\"cmakeVersion\":");
        append_json_string(output, a_manifest.minimumToolchain.cmakeVersion);
        output.append(",\"gitVersion\":");
        append_json_string(output, a_manifest.minimumToolchain.gitVersion);
        output.append(",\"compilerVendor\":");
        append_json_string(output, a_manifest.minimumToolchain.compilerVendor);
        output.append(",\"compilerVersion\":");
        append_json_string(output, a_manifest.minimumToolchain.compilerVersion);
        output.append(",\"windowsSdkVersion\":");
        append_json_string(output, a_manifest.minimumToolchain.windowsSdkVersion);
        output.append("},\"entryPoints\":{\"bootstrap\":");
        append_json_string(output, a_manifest.entryPoints.bootstrap);
        output.append(",\"projectHub\":");
        append_json_string(output, a_manifest.entryPoints.projectHub);
        output.append(",\"editor\":");
        append_json_string(output, a_manifest.entryPoints.editor);
        output.append(",\"runtimeHost\":");
        append_json_string(output, a_manifest.entryPoints.runtimeHost);
        output.append(",\"installer\":");
        append_json_string(output, a_manifest.entryPoints.installer);
        output.append(",\"installWorker\":");
        append_json_string(output, a_manifest.entryPoints.installWorker);
        output.append("},\"files\":[");
        for (std::size_t index = 0U; index < files.size(); ++index)
        {
            if (index > 0U)
            {
                output.push_back(',');
            }
            const DistributionFileEntry &file = *files[index];
            output.append("{\"role\":");
            append_json_string(output, distribution_file_role_name(file.role));
            output.append(",\"path\":");
            append_json_string(output, file.relativePath);
            output.append(",\"sizeBytes\":");
            output.append(std::to_string(file.byteSize));
            output.append(",\"sha256\":");
            append_json_string(output, file.sha256);
            output.push_back('}');
        }
        output.append("]}\n");
        if (output.size() > k_maximumManifestBytes)
        {
            return Result<std::string>::failure(make_distribution_error(
                a_assertContext, DistributionError::ResourceLimitExceeded, "Distribution Manifest size is invalid"));
        }
        return Result<std::string>::success(std::move(output));
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

Result<DistributionManifest> read_distribution_manifest(std::string_view a_bytes,
                                                        const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_bytes.empty() || a_bytes.size() > k_maximumManifestBytes)
        {
            return Result<DistributionManifest>::failure(make_distribution_error(
                a_assertContext, DistributionError::ResourceLimitExceeded, "Distribution Manifest size is invalid"));
        }
        ManifestCursor cursor(a_bytes);
        std::uint64_t schemaVersion = 0U;
        DistributionManifest manifest;
        std::string distributionKind;
        std::string sourceState;
        std::string hostOs;
        std::string hostArchitecture;
        if (!cursor.expect("{\"schemaVersion\":") || !cursor.parse_uint64(schemaVersion))
        {
            return Result<DistributionManifest>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidManifest, "Distribution Manifest prefix is invalid"));
        }
        if (schemaVersion != 1U)
        {
            return Result<DistributionManifest>::failure(
                make_distribution_error(a_assertContext, DistributionError::UnsupportedManifestVersion,
                                        "Distribution Manifest version is unsupported"));
        }
        bool parsed =
            parse_string_member(cursor, ",\"distributionKind\":", distributionKind) &&
            distributionKind == "DeveloperSourceSdk" &&
            parse_string_member(cursor, ",\"bundleId\":", manifest.bundleId) &&
            parse_string_member(cursor, ",\"engineVersion\":", manifest.engineVersion) &&
            parse_string_member(cursor, ",\"engineSourceRevision\":", manifest.engineSourceRevision) &&
            parse_string_member(cursor, ",\"engineSourceState\":", sourceState) && sourceState == "clean" &&
            parse_string_member(cursor, ",\"sourceInventoryHash\":", manifest.sourceInventoryHash) &&
            parse_string_member(cursor, ",\"dependencyDefinitionId\":", manifest.dependencyDefinitionId) &&
            cursor.expect(",\"publisherBuildIdentity\":{") &&
            parse_publisher_identity(cursor, manifest.publisherBuildIdentity) && cursor.expect(",\"host\":{") &&
            parse_string_member(cursor, "\"os\":", hostOs) && hostOs == "windows" &&
            parse_string_member(cursor, ",\"architecture\":", hostArchitecture) && hostArchitecture == "x64" &&
            cursor.expect("},\"minimumToolchain\":{") &&
            parse_string_member(cursor, "\"cmakeVersion\":", manifest.minimumToolchain.cmakeVersion) &&
            parse_string_member(cursor, ",\"gitVersion\":", manifest.minimumToolchain.gitVersion) &&
            parse_string_member(cursor, ",\"compilerVendor\":", manifest.minimumToolchain.compilerVendor) &&
            parse_string_member(cursor, ",\"compilerVersion\":", manifest.minimumToolchain.compilerVersion) &&
            parse_string_member(cursor, ",\"windowsSdkVersion\":", manifest.minimumToolchain.windowsSdkVersion) &&
            cursor.expect("},\"entryPoints\":{") &&
            parse_string_member(cursor, "\"bootstrap\":", manifest.entryPoints.bootstrap) &&
            parse_string_member(cursor, ",\"projectHub\":", manifest.entryPoints.projectHub) &&
            parse_string_member(cursor, ",\"editor\":", manifest.entryPoints.editor) &&
            parse_string_member(cursor, ",\"runtimeHost\":", manifest.entryPoints.runtimeHost) &&
            parse_string_member(cursor, ",\"installer\":", manifest.entryPoints.installer) &&
            parse_string_member(cursor, ",\"installWorker\":", manifest.entryPoints.installWorker) &&
            cursor.expect("},\"files\":[");
        if (!parsed)
        {
            return Result<DistributionManifest>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidManifest, "Distribution Manifest members are invalid"));
        }

        if (!cursor.expect("]"))
        {
            for (;;)
            {
                if (manifest.files.size() >= k_maximumManifestFiles || !cursor.expect("{\"role\":"))
                {
                    parsed = false;
                    break;
                }
                std::string roleName;
                DistributionFileEntry file;
                if (!cursor.parse_string(roleName))
                {
                    parsed = false;
                    break;
                }
                const std::optional<DistributionFileRole> role = parse_role(roleName);
                if (!role)
                {
                    parsed = false;
                    break;
                }
                file.role = *role;
                if (!parse_string_member(cursor, ",\"path\":", file.relativePath) ||
                    !cursor.expect(",\"sizeBytes\":") || !cursor.parse_uint64(file.byteSize) ||
                    !parse_string_member(cursor, ",\"sha256\":", file.sha256) || !cursor.expect("}"))
                {
                    parsed = false;
                    break;
                }
                manifest.files.push_back(std::move(file));
                if (cursor.expect("]"))
                {
                    break;
                }
                if (!cursor.expect(","))
                {
                    parsed = false;
                    break;
                }
            }
        }
        if (!parsed || !cursor.expect("}\n") || !cursor.finished())
        {
            return Result<DistributionManifest>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidManifest, "Distribution Manifest JSON is invalid"));
        }

        Result<void> validation = validate_manifest(manifest, a_assertContext);
        if (!validation)
        {
            return Result<DistributionManifest>::failure(std::move(*validation.try_error()));
        }
        Result<std::string> canonical = write_distribution_manifest(manifest, a_assertContext);
        if (!canonical)
        {
            return Result<DistributionManifest>::failure(std::move(*canonical.try_error()));
        }
        if (*canonical.try_value() != a_bytes)
        {
            return Result<DistributionManifest>::failure(make_distribution_error(
                a_assertContext, DistributionError::NonCanonicalManifest, "Distribution Manifest is not canonical"));
        }
        return Result<DistributionManifest>::success(std::move(manifest));
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
