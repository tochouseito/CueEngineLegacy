#include <Cue/Distribution/Publisher.h>

#include "Sha256.h"

#include <Cue/Distribution/Error.h>
#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
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

/// @brief ASCII lowercase Copyを返す
[[nodiscard]] std::string ascii_lower(std::string_view a_value)
{
    std::string result(a_value);
    std::ranges::transform(result, result.begin(), [](unsigned char a_character) noexcept
                           { return static_cast<char>(std::tolower(a_character)); });
    return result;
}

/// @brief PathがPrefix自身またはPrefix配下か返す
[[nodiscard]] bool is_under(std::string_view a_path, std::string_view a_prefix) noexcept
{
    return a_path == a_prefix ||
           (a_path.size() > a_prefix.size() && a_path.starts_with(a_prefix) && a_path[a_prefix.size()] == '/');
}

/// @brief Path Segmentが配布禁止名か返す
[[nodiscard]] bool has_forbidden_segment(std::string_view a_path)
{
    const std::string lower = ascii_lower(a_path);
    std::size_t start = 0U;
    while (start <= lower.size())
    {
        const std::size_t end = lower.find('/', start);
        const std::string_view segment =
            lower.substr(start, end == std::string::npos ? lower.size() - start : end - start);
        if (segment == ".git" || segment == ".codex" || segment == ".github" || segment == ".vs" ||
            segment == "tests" || segment == "test" || segment == "out" || segment == "cache" ||
            segment == "vcpkg_installed" || segment == ".tools" ||
            ((segment == "build" || segment == "builds") && start == 0U))
        {
            return true;
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1U;
    }
    return false;
}

/// @brief File名または拡張子が配布禁止対象か返す
[[nodiscard]] bool has_forbidden_file_kind(std::string_view a_path)
{
    const std::string lower = ascii_lower(a_path);
    constexpr std::array forbiddenExtensions = {
        std::string_view(".pdb"),  std::string_view(".ilk"), std::string_view(".obj"), std::string_view(".idb"),
        std::string_view(".user"), std::string_view(".suo"), std::string_view(".pfx"), std::string_view(".pem"),
        std::string_view(".key"),  std::string_view(".cer"), std::string_view(".exe"), std::string_view(".dll"),
        std::string_view(".lib"),  std::string_view(".exp"),
    };
    if (std::ranges::any_of(forbiddenExtensions,
                            [&lower](std::string_view a_extension) noexcept { return lower.ends_with(a_extension); }))
    {
        return true;
    }
    return lower.ends_with("cuegameproduct") || lower.ends_with("cuegameproduct.exe");
}

/// @brief Manifest Inventoryの長さ付きCanonical Fieldを追加する
void append_inventory_field(std::string &a_output, std::string_view a_name, std::string_view a_value)
{
    a_output.append(a_name);
    a_output.push_back('=');
    a_output.append(std::to_string(a_value.size()));
    a_output.push_back(':');
    a_output.append(a_value);
    a_output.push_back('\n');
}

/// @brief 生成Tool Roleに対応する固定TargetとBundle Path
struct ExpectedTool final
{
    cue::distribution::DistributionFileRole role;
    std::string_view target;
    std::string_view path;
};

constexpr std::array k_expectedTools = {
    ExpectedTool{cue::distribution::DistributionFileRole::Bootstrap, "CueEngineBootstrap",
                 "Bin/CueEngineBootstrap.exe"},
    ExpectedTool{cue::distribution::DistributionFileRole::ProjectHub, "CueProjectHubTool", "Bin/CueProjectHubTool.exe"},
    ExpectedTool{cue::distribution::DistributionFileRole::Editor, "CueEditorTool", "Bin/CueEditorTool.exe"},
    ExpectedTool{cue::distribution::DistributionFileRole::RuntimeHost, "CueRuntimeHost", "Bin/CueRuntimeHost.exe"},
    ExpectedTool{cue::distribution::DistributionFileRole::Installer, "CueEngineInstallerTool",
                 "Bin/CueEngineInstallerTool.exe"},
    ExpectedTool{cue::distribution::DistributionFileRole::InstallWorker, "CueEngineInstallWorker",
                 "Bin/CueEngineInstallWorker.exe"},
};

/// @brief Roleに対応する期待Tool契約を返す
[[nodiscard]] const ExpectedTool *find_expected_tool(cue::distribution::DistributionFileRole a_role) noexcept
{
    const auto iterator = std::ranges::find_if(k_expectedTools, [a_role](const ExpectedTool &a_tool) noexcept
                                               { return a_tool.role == a_role; });
    return iterator == k_expectedTools.end() ? nullptr : &*iterator;
}
} // namespace

namespace cue::distribution
{
Result<DistributionFileRole> classify_distribution_source_path(std::string_view a_relativePath,
                                                               const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_canonical_distribution_path(a_relativePath))
        {
            return Result<DistributionFileRole>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidPayloadPath, "Distribution source path is not canonical"));
        }
        if (has_forbidden_segment(a_relativePath) || has_forbidden_file_kind(a_relativePath))
        {
            return Result<DistributionFileRole>::failure(make_distribution_error(
                a_assertContext, DistributionError::ForbiddenPayload, "Distribution source path is forbidden"));
        }
        if (a_relativePath == "CMakeLists.txt" || a_relativePath == "CMakePresets.json" ||
            is_under(a_relativePath, "CMake") || a_relativePath.ends_with("/CMakeLists.txt") ||
            a_relativePath.ends_with(".cmake"))
        {
            return Result<DistributionFileRole>::success(DistributionFileRole::CMake);
        }
        if (a_relativePath == "Tools/Dependencies/RestoreVcpkg.ps1")
        {
            return Result<DistributionFileRole>::success(DistributionFileRole::Script);
        }
        if (a_relativePath == "ThirdParty/vcpkg.json" || a_relativePath == "ThirdParty/vcpkg-configuration.json" ||
            a_relativePath == "ThirdParty/vcpkg-tool.json")
        {
            return Result<DistributionFileRole>::success(DistributionFileRole::DependencyDefinition);
        }
        if (a_relativePath == "ThirdParty/THIRD_PARTY_NOTICES.md")
        {
            return Result<DistributionFileRole>::success(DistributionFileRole::ThirdPartyNotice);
        }
        if (is_under(a_relativePath, "ThirdParty/Licenses"))
        {
            return Result<DistributionFileRole>::success(DistributionFileRole::ThirdPartyLicense);
        }
        if (a_relativePath == "LICENSE.txt")
        {
            return Result<DistributionFileRole>::success(DistributionFileRole::License);
        }
        if (a_relativePath == "README.md" || is_under(a_relativePath, "Engine/Documents"))
        {
            return Result<DistributionFileRole>::success(DistributionFileRole::Document);
        }
        if (is_under(a_relativePath, "Templates"))
        {
            return Result<DistributionFileRole>::success(DistributionFileRole::Template);
        }
        if (is_under(a_relativePath, "Engine/Source"))
        {
            if (a_relativePath.ends_with(".hlsl") || a_relativePath.ends_with(".hlsli"))
            {
                return Result<DistributionFileRole>::success(DistributionFileRole::Hlsl);
            }
            return Result<DistributionFileRole>::success(DistributionFileRole::EngineSource);
        }
        return Result<DistributionFileRole>::failure(make_distribution_error(
            a_assertContext, DistributionError::PayloadNotAllowlisted, "Distribution source path is not allowlisted"));
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

Result<void> validate_repository_evidence(const RepositoryStateEvidence &a_started,
                                          const RepositoryStateEvidence &a_completed,
                                          const AssertContext &a_assertContext) noexcept
{
    if (!a_started.clean || !a_completed.clean)
    {
        return Result<void>::failure(make_distribution_error(a_assertContext, DistributionError::DirtyRepository,
                                                             "Distribution Publisher requires a clean Repository"));
    }
    const auto valid = [](const RepositoryStateEvidence &a_evidence) noexcept
    {
        return is_canonical_git_revision(a_evidence.headRevision) &&
               is_canonical_git_revision(a_evidence.indexTreeId) && is_canonical_sha256(a_evidence.trackedStateHash) &&
               is_canonical_sha256(a_evidence.untrackedStateHash);
    };
    if (!valid(a_started) || !valid(a_completed))
    {
        return Result<void>::failure(make_distribution_error(a_assertContext, DistributionError::InvalidIdentity,
                                                             "Repository evidence identity is invalid"));
    }
    if (!(a_started == a_completed))
    {
        return Result<void>::failure(make_distribution_error(a_assertContext, DistributionError::RepositoryChanged,
                                                             "Repository changed while Inventory was generated"));
    }
    return Result<void>::success();
}

Result<DistributionPublisherInventory> make_distribution_publisher_inventory(
    std::span<const SourceBlobEvidence> a_sources, std::span<const GeneratedToolEvidence> a_tools,
    std::string_view a_fixedRevision, const PublisherBuildIdentity &a_publisherBuildIdentity,
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_sources.empty() || a_tools.size() != k_expectedTools.size() ||
            !is_canonical_git_revision(a_fixedRevision) ||
            !is_valid_publisher_build_identity(a_publisherBuildIdentity) ||
            a_publisherBuildIdentity.builtFromRevision != a_fixedRevision)
        {
            return Result<DistributionPublisherInventory>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidIdentity, "Distribution Publisher identity is invalid"));
        }

        struct SourceRecord final
        {
            DistributionFileEntry entry;
            std::string gitBlobId;
        };
        std::vector<SourceRecord> sourceRecords;
        sourceRecords.reserve(a_sources.size());
        for (const SourceBlobEvidence &source : a_sources)
        {
            Result<DistributionFileRole> role = classify_distribution_source_path(source.relativePath, a_assertContext);
            if (!role)
            {
                return Result<DistributionPublisherInventory>::failure(std::move(*role.try_error()));
            }
            if (!is_canonical_git_revision(source.gitBlobId) || !is_canonical_sha256(source.commitSha256) ||
                !is_canonical_sha256(source.stagedSha256) || source.commitByteSize != source.stagedByteSize ||
                source.commitSha256 != source.stagedSha256)
            {
                return Result<DistributionPublisherInventory>::failure(
                    make_distribution_error(a_assertContext, DistributionError::SourceBlobMismatch,
                                            "Staged source does not match its fixed Commit Blob"));
            }
            sourceRecords.push_back({DistributionFileEntry{*role.try_value(), source.relativePath,
                                                           source.stagedByteSize, source.stagedSha256},
                                     source.gitBlobId});
        }
        std::ranges::sort(sourceRecords, [](const SourceRecord &a_left, const SourceRecord &a_right) noexcept
                          { return a_left.entry.relativePath < a_right.entry.relativePath; });
        std::vector<std::string> sourcePathKeys;
        sourcePathKeys.reserve(sourceRecords.size());
        for (const SourceRecord &source : sourceRecords)
        {
            sourcePathKeys.push_back(ascii_lower(source.entry.relativePath));
        }
        std::ranges::sort(sourcePathKeys);
        if (std::adjacent_find(sourcePathKeys.begin(), sourcePathKeys.end()) != sourcePathKeys.end())
        {
            return Result<DistributionPublisherInventory>::failure(make_distribution_error(
                a_assertContext, DistributionError::DuplicatePayload, "Distribution source path is duplicated"));
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
        for (const DistributionFileRole requiredRole : requiredSourceRoles)
        {
            if (std::ranges::none_of(sourceRecords, [requiredRole](const SourceRecord &a_source) noexcept
                                     { return a_source.entry.role == requiredRole; }))
            {
                return Result<DistributionPublisherInventory>::failure(
                    make_distribution_error(a_assertContext, DistributionError::MissingRequiredPayload,
                                            "Required source-managed Distribution payload is missing"));
            }
        }

        std::string inventoryCanonical;
        inventoryCanonical.reserve(sourceRecords.size() * 256U);
        for (const SourceRecord &source : sourceRecords)
        {
            append_inventory_field(inventoryCanonical, "role", distribution_file_role_name(source.entry.role));
            append_inventory_field(inventoryCanonical, "path", source.entry.relativePath);
            append_inventory_field(inventoryCanonical, "size", std::to_string(source.entry.byteSize));
            append_inventory_field(inventoryCanonical, "sha256", source.entry.sha256);
            append_inventory_field(inventoryCanonical, "gitBlobId", source.gitBlobId);
        }

        std::array<bool, k_expectedTools.size()> foundTools{};
        std::vector<DistributionFileEntry> files;
        files.reserve(sourceRecords.size() + a_tools.size());
        for (SourceRecord &source : sourceRecords)
        {
            files.push_back(std::move(source.entry));
        }
        for (const GeneratedToolEvidence &tool : a_tools)
        {
            const ExpectedTool *expected = find_expected_tool(tool.role);
            if (expected == nullptr)
            {
                return Result<DistributionPublisherInventory>::failure(make_distribution_error(
                    a_assertContext, DistributionError::InvalidGeneratedTool, "Generated Tool role is invalid"));
            }
            const std::size_t expectedIndex = static_cast<std::size_t>(expected - k_expectedTools.data());
            if (foundTools[expectedIndex] || tool.targetName != expected->target ||
                tool.relativePath != expected->path || tool.configuration != "Release" ||
                tool.builtFromRevision != a_fixedRevision ||
                !(tool.publisherBuildIdentity == a_publisherBuildIdentity) || tool.byteSize == 0U ||
                tool.peArchitecture != DistributionArchitecture::X64 || !is_canonical_sha256(tool.sha256) ||
                !is_canonical_distribution_path(tool.relativePath))
            {
                return Result<DistributionPublisherInventory>::failure(make_distribution_error(
                    a_assertContext, DistributionError::InvalidGeneratedTool, "Generated Tool evidence is invalid"));
            }
            foundTools[expectedIndex] = true;
            files.push_back({tool.role, tool.relativePath, tool.byteSize, tool.sha256});
        }
        if (std::ranges::find(foundTools, false) != foundTools.end())
        {
            return Result<DistributionPublisherInventory>::failure(make_distribution_error(
                a_assertContext, DistributionError::MissingRequiredPayload, "Required generated Tool is missing"));
        }
        std::ranges::sort(files, [](const DistributionFileEntry &a_left, const DistributionFileEntry &a_right) noexcept
                          { return a_left.relativePath < a_right.relativePath; });
        if (std::adjacent_find(files.begin(), files.end(),
                               [](const DistributionFileEntry &a_left, const DistributionFileEntry &a_right) noexcept
                               { return a_left.relativePath == a_right.relativePath; }) != files.end())
        {
            return Result<DistributionPublisherInventory>::failure(make_distribution_error(
                a_assertContext, DistributionError::DuplicatePayload, "Distribution Inventory path is duplicated"));
        }

        DistributionPublisherInventory inventory;
        inventory.sourceInventoryHash = distribution_private::sha256_text(inventoryCanonical);
        inventory.files = std::move(files);
        return Result<DistributionPublisherInventory>::success(std::move(inventory));
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
