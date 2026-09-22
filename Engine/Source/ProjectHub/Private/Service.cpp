#include <Cue/ProjectHub/Service.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/ProjectHub/Error.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace
{
[[nodiscard]] bool is_durability_unknown(const cue::Error &a_error) noexcept
{
    return a_error.root_code().domain() == "Cue.IO" &&
           a_error.root_code().value() == static_cast<std::int64_t>(cue::IoError::DurabilityUnknown);
}

[[nodiscard]] cue::project_hub::ProjectEntryState map_locator_state(cue::ProjectLocatorState a_state) noexcept
{
    switch (a_state)
    {
    case cue::ProjectLocatorState::Available:
        return cue::project_hub::ProjectEntryState::Available;
    case cue::ProjectLocatorState::Missing:
        return cue::project_hub::ProjectEntryState::Missing;
    case cue::ProjectLocatorState::Moved:
        return cue::project_hub::ProjectEntryState::Moved;
    }
    return cue::project_hub::ProjectEntryState::Broken;
}

/// @brief Engine VersionをProject一覧用のcanonical文字列へ変換する
[[nodiscard]] std::string format_engine_version(const cue::EngineVersion &a_version)
{
    std::string formatted = std::to_string(a_version.major);
    formatted.push_back('.');
    formatted.append(std::to_string(a_version.minor));
    formatted.push_back('.');
    formatted.append(std::to_string(a_version.patch));
    return formatted;
}
} // namespace

namespace cue::project_hub
{
EditorLaunchRequest::EditorLaunchRequest(std::string &&a_projectDescriptorLocator, std::string &&a_expectedProjectId,
                                         std::string &&a_engineCompatibilityId,
                                         std::optional<std::string> &&a_initialSceneLocator,
                                         std::optional<std::string> &&a_expectedInitialSceneAssetId,
                                         std::optional<InstalledEngineLaunchIdentity>
                                             &&a_installedEngineIdentity) noexcept
    : m_projectDescriptorLocator(std::move(a_projectDescriptorLocator)),
      m_expectedProjectId(std::move(a_expectedProjectId)), m_engineCompatibilityId(std::move(a_engineCompatibilityId)),
      m_initialSceneLocator(std::move(a_initialSceneLocator)),
      m_expectedInitialSceneAssetId(std::move(a_expectedInitialSceneAssetId)),
      m_installedEngineIdentity(std::move(a_installedEngineIdentity))
{
}

std::uint32_t EditorLaunchRequest::protocol_version() const noexcept
{
    return k_editorLaunchProtocolVersion;
}

std::string_view EditorLaunchRequest::project_descriptor_locator() const noexcept
{
    return m_projectDescriptorLocator;
}

std::string_view EditorLaunchRequest::expected_project_id() const noexcept
{
    return m_expectedProjectId;
}

std::string_view EditorLaunchRequest::engine_compatibility_id() const noexcept
{
    return m_engineCompatibilityId;
}

const std::optional<std::string> &EditorLaunchRequest::initial_scene_locator() const noexcept
{
    return m_initialSceneLocator;
}

const std::optional<std::string> &EditorLaunchRequest::expected_initial_scene_asset_id() const noexcept
{
    return m_expectedInitialSceneAssetId;
}

const std::optional<InstalledEngineLaunchIdentity> &EditorLaunchRequest::installed_engine_identity() const noexcept
{
    return m_installedEngineIdentity;
}

ProjectHubService::ProjectHubService(ConstructionKey, FilesystemRoot &a_workspaceFilesystem,
                                     ProjectHubPlatform &a_platform, ProjectHubConfiguration &&a_configuration,
                                     RecentProjectRegistry &&a_registry, const AssertContext &a_assertContext) noexcept
    : m_workspaceFilesystem(&a_workspaceFilesystem), m_platform(&a_platform), m_assertContext(&a_assertContext),
      m_configuration(std::move(a_configuration)), m_registry(std::move(a_registry))
{
}

Result<std::unique_ptr<ProjectHubService>> ProjectHubService::create(FilesystemRoot &a_workspaceFilesystem,
                                                                     ProjectHubPlatform &a_platform,
                                                                     ProjectHubConfiguration &&a_configuration,
                                                                     const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_configuration.supportedProjectFormatVersion == 0U ||
            (a_configuration.blankProjectCompatibility.maximumExclusive.has_value() &&
             *a_configuration.blankProjectCompatibility.maximumExclusive <=
                 a_configuration.blankProjectCompatibility.minimum))
        {
            return Result<std::unique_ptr<ProjectHubService>>::failure(make_project_hub_error(
                a_assertContext, ProjectHubError::InvalidConfiguration, "Project Hub configuration is invalid"));
        }
        std::size_t selectedEngineCount = 0U;
        for (std::size_t index = 0U; index < a_configuration.installedEngineVersions.size(); ++index)
        {
            const InstalledEngineVersionView &version = a_configuration.installedEngineVersions[index];
            if (version.directoryName.empty() || version.displayName.empty() || version.bundleId.empty() ||
                version.manifestDigest.empty())
            {
                return Result<std::unique_ptr<ProjectHubService>>::failure(make_project_hub_error(
                    a_assertContext, ProjectHubError::InvalidConfiguration,
                    "Installed Engine Version configuration is incomplete"));
            }
            const bool duplicate = std::ranges::any_of(
                a_configuration.installedEngineVersions.begin(),
                a_configuration.installedEngineVersions.begin() + static_cast<std::ptrdiff_t>(index),
                [&version](const InstalledEngineVersionView &a_candidate)
                { return a_candidate.directoryName == version.directoryName; });
            if (duplicate)
            {
                return Result<std::unique_ptr<ProjectHubService>>::failure(make_project_hub_error(
                    a_assertContext, ProjectHubError::InvalidConfiguration,
                    "Installed Engine Version configuration contains duplicate identities"));
            }
            if (version.isSelected)
            {
                ++selectedEngineCount;
                if (version.engineVersion != a_configuration.currentEngineVersion)
                {
                    return Result<std::unique_ptr<ProjectHubService>>::failure(make_project_hub_error(
                        a_assertContext, ProjectHubError::InvalidConfiguration,
                        "Selected Installed Engine Version differs from the active compatibility version"));
                }
            }
        }
        if (!a_configuration.installedEngineVersions.empty() && selectedEngineCount != 1U)
        {
            return Result<std::unique_ptr<ProjectHubService>>::failure(make_project_hub_error(
                a_assertContext, ProjectHubError::InvalidConfiguration,
                "Installed Engine Version configuration must select exactly one version"));
        }
        auto registry = load_recent_project_registry(a_workspaceFilesystem, a_assertContext);
        if (!registry)
        {
            return Result<std::unique_ptr<ProjectHubService>>::failure(reclassify_project_hub_error(
                a_assertContext, ProjectHubError::PersistenceFailure, "Project Hub registry could not be loaded",
                std::move(*registry.try_error())));
        }
        std::unique_ptr<ProjectHubService> service(
            new ProjectHubService(ConstructionKey{}, a_workspaceFilesystem, a_platform, std::move(a_configuration),
                                  std::move(*registry.try_value()), a_assertContext));
        service->m_templates.push_back(ProjectTemplateView{std::string(k_blank3dTemplateId), "Blank 3D",
                                                           service->m_configuration.blankProjectCompatibility});
        auto refreshed = service->refresh();
        if (!refreshed)
        {
            return Result<std::unique_ptr<ProjectHubService>>::failure(std::move(*refreshed.try_error()));
        }
        return Result<std::unique_ptr<ProjectHubService>>::success(std::move(service));
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Cue.ProjectHub service construction failed");
    }
    std::terminate();
}

std::span<const ProjectTemplateView> ProjectHubService::templates() const noexcept
{
    return m_templates;
}

std::span<const ProjectRowView> ProjectHubService::projects() const noexcept
{
    return m_projects;
}

std::span<const InstalledEngineVersionView> ProjectHubService::installed_engine_versions() const noexcept
{
    return m_configuration.installedEngineVersions;
}

Result<void> ProjectHubService::select_installed_engine_version(std::string_view a_versionDirectory) noexcept
{
    try
    {
        const auto selected = std::ranges::find_if(
            m_configuration.installedEngineVersions,
            [a_versionDirectory](const InstalledEngineVersionView &a_version)
            { return a_version.directoryName == a_versionDirectory; });
        if (selected == m_configuration.installedEngineVersions.end() || !selected->isAvailable)
        {
            return Result<void>::failure(make_project_hub_error(
                *m_assertContext, ProjectHubError::ProjectUnsupported,
                "Selected Installed Engine Version is unavailable"));
        }
        if (selected->isSelected)
        {
            return Result<void>::success();
        }
        const EngineVersion previousEngineVersion = m_configuration.currentEngineVersion;
        std::vector<InstalledEngineVersionView> previousVersions = m_configuration.installedEngineVersions;
        const EngineVersion selectedEngineVersion = selected->engineVersion;
        for (InstalledEngineVersionView &version : m_configuration.installedEngineVersions)
        {
            version.isSelected = version.directoryName == a_versionDirectory;
        }
        m_configuration.currentEngineVersion = selectedEngineVersion;
        Result<void> refreshed = refresh();
        if (!refreshed)
        {
            m_configuration.currentEngineVersion = previousEngineVersion;
            m_configuration.installedEngineVersions = std::move(previousVersions);
        }
        return refreshed;
    }
    catch (...)
    {
        m_assertContext->fatal_handler().terminate("Cue.ProjectHub Installed Engine Version selection failed");
    }
    std::terminate();
}

Result<void> ProjectHubService::replace_installed_engine_versions(
    std::vector<InstalledEngineVersionView> a_versions) noexcept
{
    try
    {
        std::size_t selectedCount = 0U;
        std::optional<EngineVersion> selectedEngineVersion;
        for (std::size_t index = 0U; index < a_versions.size(); ++index)
        {
            const InstalledEngineVersionView &version = a_versions[index];
            const bool duplicate = std::ranges::any_of(
                a_versions.begin(), a_versions.begin() + static_cast<std::ptrdiff_t>(index),
                [&version](const InstalledEngineVersionView &a_candidate)
                { return a_candidate.directoryName == version.directoryName; });
            if (version.directoryName.empty() || version.displayName.empty() || version.bundleId.empty() ||
                version.manifestDigest.empty() || duplicate)
            {
                return Result<void>::failure(make_project_hub_error(
                    *m_assertContext, ProjectHubError::InvalidConfiguration,
                    "Installed Engine Version refresh contains an invalid identity"));
            }
            if (version.isSelected)
            {
                ++selectedCount;
                selectedEngineVersion = version.engineVersion;
            }
        }
        if (!a_versions.empty() && selectedCount != 1U)
        {
            return Result<void>::failure(make_project_hub_error(
                *m_assertContext, ProjectHubError::InvalidConfiguration,
                "Installed Engine Version refresh must select exactly one version"));
        }
        const EngineVersion previousEngineVersion = m_configuration.currentEngineVersion;
        std::vector<InstalledEngineVersionView> previousVersions =
            std::move(m_configuration.installedEngineVersions);
        m_configuration.installedEngineVersions = std::move(a_versions);
        if (selectedEngineVersion)
        {
            m_configuration.currentEngineVersion = *selectedEngineVersion;
        }
        Result<void> refreshed = refresh();
        if (!refreshed)
        {
            m_configuration.currentEngineVersion = previousEngineVersion;
            m_configuration.installedEngineVersions = std::move(previousVersions);
        }
        return refreshed;
    }
    catch (...)
    {
        m_assertContext->fatal_handler().terminate("Cue.ProjectHub Installed Engine Version refresh failed");
    }
    std::terminate();
}

ProjectCreationOutcome::ProjectCreationOutcome(std::string &&a_projectLocator, bool a_isRecentRegistered,
                                               std::optional<Error> &&a_creationDurabilityError,
                                               std::optional<Error> &&a_recentPersistenceError) noexcept
    : m_projectLocator(std::move(a_projectLocator)), m_isRecentRegistered(a_isRecentRegistered),
      m_creationDurabilityError(std::move(a_creationDurabilityError)),
      m_recentPersistenceError(std::move(a_recentPersistenceError))
{
}

bool ProjectCreationOutcome::is_recent_registered() const noexcept
{
    return m_isRecentRegistered;
}

std::string_view ProjectCreationOutcome::project_locator() const noexcept
{
    return m_projectLocator;
}

const Error *ProjectCreationOutcome::try_creation_durability_error() const noexcept
{
    return m_creationDurabilityError ? &*m_creationDurabilityError : nullptr;
}

const Error *ProjectCreationOutcome::try_recent_persistence_error() const noexcept
{
    return m_recentPersistenceError ? &*m_recentPersistenceError : nullptr;
}

Result<void> ProjectHubService::refresh() noexcept
{
    auto candidateResult = clone_registry();
    if (!candidateResult)
    {
        return Result<void>::failure(std::move(*candidateResult.try_error()));
    }
    RecentProjectRegistry candidate = std::move(*candidateResult.try_value());
    auto prepared = prepare_registry_snapshot(candidate);
    if (!prepared)
    {
        return Result<void>::failure(std::move(*prepared.try_error()));
    }
    if (prepared.try_value()->registryChanged)
    {
        auto saved = save_recent_project_registry(*m_workspaceFilesystem, candidate, *m_assertContext);
        if (!saved)
        {
            const bool wasPublished = is_durability_unknown(*saved.try_error());
            Error primary = reclassify_project_hub_error(*m_assertContext, ProjectHubError::PersistenceFailure,
                                                         "Project Hub registry state could not be saved",
                                                         std::move(*saved.try_error()));
            if (wasPublished)
            {
                m_registry = std::move(candidate);
                m_projects = std::move(prepared.try_value()->projects);
            }
            return Result<void>::failure(std::move(primary));
        }
        m_registry = std::move(candidate);
    }
    m_projects = std::move(prepared.try_value()->projects);
    return Result<void>::success();
}

Result<ProjectHubService::PreparedRegistrySnapshot> ProjectHubService::prepare_registry_snapshot(
    RecentProjectRegistry &a_registry) noexcept
{
    try
    {
        std::vector<ProjectRowView> projects;
        projects.reserve(a_registry.entries().size());
        bool registryChanged = false;
        for (const RecentProject &entry : a_registry.entries())
        {
            ProjectRowView row{std::string(entry.project_id().text()),
                               std::string(entry.locator()),
                               std::string(entry.locator()),
                               entry.last_opened_milliseconds(),
                               format_engine_version(m_configuration.currentEngineVersion),
                               std::nullopt,
                               std::nullopt,
                               entry.is_pinned(),
                               map_locator_state(entry.locator_state()),
                               ProjectEntryProblem::None,
                               ProjectCompatibilityStatus::Unknown,
                               false,
                               false,
                               std::nullopt,
                               {}};

            auto root = m_platform->open_root(entry.locator());
            if (!root)
            {
                row.state = ProjectEntryState::Broken;
                row.problem = ProjectEntryProblem::LocatorAccessFailed;
                projects.push_back(std::move(row));
                continue;
            }
            if (*root.try_value() == nullptr)
            {
                row.state = ProjectEntryState::Missing;
                if (entry.locator_state() != ProjectLocatorState::Missing)
                {
                    auto marked = a_registry.mark_project_missing(entry.project_id(), *m_assertContext);
                    if (!marked)
                    {
                        return Result<PreparedRegistrySnapshot>::failure(std::move(*marked.try_error()));
                    }
                    registryChanged = true;
                }
                projects.push_back(std::move(row));
                continue;
            }

            auto storage = m_platform->inspect_project_storage(entry.locator());
            if (storage)
            {
                row.latestWriteMilliseconds = storage.try_value()->latestWriteMilliseconds;
                row.byteSize = storage.try_value()->byteSize;
            }

            auto descriptor = load_project_descriptor(**root.try_value(), *m_assertContext);
            if (!descriptor)
            {
                row.state = ProjectEntryState::Broken;
                row.problem = ProjectEntryProblem::DescriptorInvalid;
                projects.push_back(std::move(row));
                continue;
            }
            if (descriptor.try_value()->project_id() != entry.project_id())
            {
                row.state = ProjectEntryState::Broken;
                row.problem = ProjectEntryProblem::IdentityMismatch;
                projects.push_back(std::move(row));
                continue;
            }

            row.displayName = std::string(descriptor.try_value()->display_name());
            row.engineCompatibility = descriptor.try_value()->engine_compatibility();
            if (entry.locator_state() == ProjectLocatorState::Missing)
            {
                auto marked = a_registry.mark_project_available(entry.project_id(), *m_assertContext);
                if (!marked)
                {
                    return Result<PreparedRegistrySnapshot>::failure(std::move(*marked.try_error()));
                }
                row.state = ProjectEntryState::Available;
                registryChanged = true;
            }
            auto compatibility = evaluate_project_compatibility(
                descriptor.try_value()->schema_version(), m_configuration.supportedProjectFormatVersion,
                descriptor.try_value()->engine_compatibility(), m_configuration.currentEngineVersion,
                m_configuration.capabilityProfile, m_configuration.capabilitySnapshot, *m_assertContext);
            if (!compatibility)
            {
                row.state = ProjectEntryState::Broken;
                row.problem = ProjectEntryProblem::CompatibilityInvalid;
                projects.push_back(std::move(row));
                continue;
            }
            row.compatibilityStatus = compatibility.try_value()->status();
            row.canOpen = compatibility.try_value()->can_open();
            row.canMigrate = descriptor.try_value()->schema_version() < k_currentProjectDescriptorSchemaVersion &&
                             m_configuration.supportedProjectFormatVersion == k_currentProjectDescriptorSchemaVersion;
            row.compatibilityReasons.assign(compatibility.try_value()->reasons().begin(),
                                            compatibility.try_value()->reasons().end());
            projects.push_back(std::move(row));
        }
        return Result<PreparedRegistrySnapshot>::success(
            PreparedRegistrySnapshot{std::move(projects), registryChanged});
    }
    catch (...)
    {
        m_assertContext->fatal_handler().terminate("Cue.ProjectHub registry snapshot preparation failed");
    }
    std::terminate();
}

Result<ProjectCreationOutcome> ProjectHubService::create_blank_project(std::string_view a_parentLocator,
                                                                       std::string_view a_projectName,
                                                                       std::string_view a_templateId,
                                                                       std::uint64_t a_openedMilliseconds) noexcept
{
    if (a_templateId != k_blank3dTemplateId)
    {
        return Result<ProjectCreationOutcome>::failure(make_project_hub_error(
            *m_assertContext, ProjectHubError::InvalidTemplate, "Project template is not registered"));
    }
    auto parentLocator = m_platform->normalize_project_locator(a_parentLocator);
    if (!parentLocator)
    {
        return Result<ProjectCreationOutcome>::failure(
            reclassify_project_hub_error(*m_assertContext, ProjectHubError::InvalidLocator,
                                         "Project parent locator is invalid", std::move(*parentLocator.try_error())));
    }
    auto parentRoot = m_platform->create_or_open_root(*parentLocator.try_value());
    if (!parentRoot)
    {
        return Result<ProjectCreationOutcome>::failure(reclassify_project_hub_error(
            *m_assertContext, ProjectHubError::InvalidLocator, "Project parent locator could not be opened",
            std::move(*parentRoot.try_error())));
    }
    if (*parentRoot.try_value() == nullptr)
    {
        return Result<ProjectCreationOutcome>::failure(make_project_hub_error(
            *m_assertContext, ProjectHubError::ProjectMissing, "Project parent locator could not be created"));
    }
    auto projectLocator = m_platform->compose_project_locator(*parentLocator.try_value(), a_projectName);
    if (!projectLocator)
    {
        return Result<ProjectCreationOutcome>::failure(reclassify_project_hub_error(
            *m_assertContext, ProjectHubError::InvalidLocator, "Project locator could not be composed",
            std::move(*projectLocator.try_error())));
    }
    auto projectId = m_platform->next_project_id();
    if (!projectId)
    {
        return Result<ProjectCreationOutcome>::failure(std::move(*projectId.try_error()));
    }
    auto sceneAssetId = m_platform->next_scene_asset_id();
    if (!sceneAssetId)
    {
        return Result<ProjectCreationOutcome>::failure(std::move(*sceneAssetId.try_error()));
    }
    auto descriptor = generate_blank_project(
        **parentRoot.try_value(), a_projectName, a_projectName, *projectId.try_value(), *sceneAssetId.try_value(),
        BlankProjectTemplate{m_configuration.blankProjectCompatibility}, *m_assertContext);
    std::optional<Error> creationDurabilityError;
    if (!descriptor)
    {
        if (!is_durability_unknown(*descriptor.try_error()))
        {
            return Result<ProjectCreationOutcome>::failure(std::move(*descriptor.try_error()));
        }
        creationDurabilityError.emplace(std::move(*descriptor.try_error()));
        auto publishedRoot = m_platform->open_root(*projectLocator.try_value());
        if (!publishedRoot)
        {
            creationDurabilityError->append_secondary_diagnostics(*m_assertContext, *publishedRoot.try_error(),
                                                                  "Published project re-open failed", "Revalidation");
        }
        else if (*publishedRoot.try_value() == nullptr)
        {
            Error revalidation = make_project_hub_error(*m_assertContext, ProjectHubError::ProjectMissing,
                                                        "Published project was not found during revalidation");
            creationDurabilityError->append_secondary_diagnostics(
                *m_assertContext, revalidation, "Published project re-open returned no root", "Revalidation");
        }
        else
        {
            auto publishedDescriptor = load_project_descriptor(**publishedRoot.try_value(), *m_assertContext);
            if (!publishedDescriptor)
            {
                creationDurabilityError->append_secondary_diagnostics(
                    *m_assertContext, *publishedDescriptor.try_error(), "Published project descriptor is invalid",
                    "Revalidation");
            }
            else if (publishedDescriptor.try_value()->project_id() != *projectId.try_value())
            {
                Error revalidation =
                    make_project_hub_error(*m_assertContext, ProjectHubError::ProjectIdentityMismatch,
                                           "Published project identity differs from the generated identity");
                creationDurabilityError->append_secondary_diagnostics(
                    *m_assertContext, revalidation, "Published project identity verification failed", "Revalidation");
            }
            else
            {
                descriptor = Result<ProjectDescriptor>::success(std::move(*publishedDescriptor.try_value()));
            }
        }
        if (!descriptor)
        {
            return Result<ProjectCreationOutcome>::failure(std::move(*creationDurabilityError));
        }
    }
    auto makeOutcome = [&projectLocator, &creationDurabilityError](bool a_isRecentRegistered, Error &&a_error)
    {
        std::optional<Error> recentPersistenceError(std::in_place, std::move(a_error));
        return Result<ProjectCreationOutcome>::success(
            ProjectCreationOutcome(std::string(*projectLocator.try_value()), a_isRecentRegistered,
                                   std::move(creationDurabilityError), std::move(recentPersistenceError)));
    };
    auto candidate = clone_registry();
    if (!candidate)
    {
        return makeOutcome(false, std::move(*candidate.try_error()));
    }
    auto registered = candidate.try_value()->register_project(*descriptor.try_value(), *projectLocator.try_value(),
                                                              a_openedMilliseconds, *m_assertContext);
    if (!registered)
    {
        return makeOutcome(false, std::move(*registered.try_error()));
    }
    auto committed = commit_registry(std::move(*candidate.try_value()));
    if (!committed)
    {
        const bool isRegistered = is_durability_unknown(*committed.try_error());
        return makeOutcome(isRegistered, std::move(*committed.try_error()));
    }
    std::optional<Error> recentPersistenceError;
    return Result<ProjectCreationOutcome>::success(ProjectCreationOutcome(std::string(*projectLocator.try_value()),
                                                                          true, std::move(creationDurabilityError),
                                                                          std::move(recentPersistenceError)));
}

Result<void> ProjectHubService::register_project(std::string_view a_locator, std::uint64_t a_openedMilliseconds,
                                                 bool a_confirmMovedProject) noexcept
{
    auto locator = m_platform->normalize_project_locator(a_locator);
    if (!locator)
    {
        return Result<void>::failure(reclassify_project_hub_error(*m_assertContext, ProjectHubError::InvalidLocator,
                                                                  "Project locator is invalid",
                                                                  std::move(*locator.try_error())));
    }
    auto root = m_platform->open_root(*locator.try_value());
    if (!root)
    {
        return Result<void>::failure(reclassify_project_hub_error(*m_assertContext, ProjectHubError::ProjectBroken,
                                                                  "Project locator could not be opened",
                                                                  std::move(*root.try_error())));
    }
    if (*root.try_value() == nullptr)
    {
        return Result<void>::failure(make_project_hub_error(*m_assertContext, ProjectHubError::ProjectMissing,
                                                            "Project locator does not exist"));
    }
    auto descriptor = load_project_descriptor(**root.try_value(), *m_assertContext);
    if (!descriptor)
    {
        return Result<void>::failure(reclassify_project_hub_error(*m_assertContext, ProjectHubError::ProjectBroken,
                                                                  "Project descriptor could not be loaded",
                                                                  std::move(*descriptor.try_error())));
    }
    auto candidate = clone_registry();
    if (!candidate)
    {
        return Result<void>::failure(std::move(*candidate.try_error()));
    }
    Result<void> registered =
        a_confirmMovedProject
            ? candidate.try_value()->reassociate_project(*descriptor.try_value(), *locator.try_value(),
                                                         a_openedMilliseconds, *m_assertContext)
            : candidate.try_value()->register_project(*descriptor.try_value(), *locator.try_value(),
                                                      a_openedMilliseconds, *m_assertContext);
    if (!registered)
    {
        return registered;
    }
    return commit_registry(std::move(*candidate.try_value()));
}

Result<EditorLaunchRequest> ProjectHubService::open_project(
    std::string_view a_projectId, std::uint64_t a_openedMilliseconds,
    std::optional<std::string_view> a_initialSceneLocator) noexcept
{
    try
    {
        auto projectId = parse_project_id(a_projectId);
        if (!projectId)
        {
            return Result<EditorLaunchRequest>::failure(std::move(*projectId.try_error()));
        }
        const auto found = std::find_if(m_registry.entries().begin(), m_registry.entries().end(),
                                        [&projectId](const RecentProject &a_entry)
                                        { return a_entry.project_id() == *projectId.try_value(); });
        if (found == m_registry.entries().end())
        {
            return Result<EditorLaunchRequest>::failure(make_project_hub_error(
                *m_assertContext, ProjectHubError::ProjectNotFound, "Project is not in the Recent registry"));
        }
        auto root = m_platform->open_root(found->locator());
        if (!root)
        {
            Error primary =
                reclassify_project_hub_error(*m_assertContext, ProjectHubError::ProjectBroken,
                                             "Project locator could not be opened", std::move(*root.try_error()));
            refresh_after_open_failure(primary);
            return Result<EditorLaunchRequest>::failure(std::move(primary));
        }
        if (*root.try_value() == nullptr)
        {
            auto candidate = clone_registry();
            if (!candidate)
            {
                return Result<EditorLaunchRequest>::failure(std::move(*candidate.try_error()));
            }
            auto marked = candidate.try_value()->mark_project_missing(*projectId.try_value(), *m_assertContext);
            if (!marked)
            {
                return Result<EditorLaunchRequest>::failure(std::move(*marked.try_error()));
            }
            auto committed = commit_registry(std::move(*candidate.try_value()));
            if (!committed)
            {
                return Result<EditorLaunchRequest>::failure(std::move(*committed.try_error()));
            }
            return Result<EditorLaunchRequest>::failure(make_project_hub_error(
                *m_assertContext, ProjectHubError::ProjectMissing, "Project locator does not exist"));
        }
        auto descriptor = load_project_descriptor(**root.try_value(), *m_assertContext);
        if (!descriptor)
        {
            Error primary = reclassify_project_hub_error(*m_assertContext, ProjectHubError::ProjectBroken,
                                                         "Project descriptor could not be loaded",
                                                         std::move(*descriptor.try_error()));
            refresh_after_open_failure(primary);
            return Result<EditorLaunchRequest>::failure(std::move(primary));
        }
        if (descriptor.try_value()->project_id() != *projectId.try_value())
        {
            Error primary = make_project_hub_error(*m_assertContext, ProjectHubError::ProjectIdentityMismatch,
                                                   "Project descriptor identity differs from the Recent registry");
            refresh_after_open_failure(primary);
            return Result<EditorLaunchRequest>::failure(std::move(primary));
        }
        auto compatibility = evaluate_project_compatibility(
            descriptor.try_value()->schema_version(), m_configuration.supportedProjectFormatVersion,
            descriptor.try_value()->engine_compatibility(), m_configuration.currentEngineVersion,
            m_configuration.capabilityProfile, m_configuration.capabilitySnapshot, *m_assertContext);
        if (!compatibility)
        {
            Error primary = std::move(*compatibility.try_error());
            refresh_after_open_failure(primary);
            return Result<EditorLaunchRequest>::failure(std::move(primary));
        }
        if (!compatibility.try_value()->can_open())
        {
            Error primary = make_project_hub_error(*m_assertContext, ProjectHubError::ProjectUnsupported,
                                                   "Project cannot be opened by this engine");
            refresh_after_open_failure(primary);
            return Result<EditorLaunchRequest>::failure(std::move(primary));
        }
        std::optional<InstalledEngineLaunchIdentity> installedEngineIdentity;
        if (!m_configuration.installedEngineVersions.empty())
        {
            const auto selectedEngine = std::ranges::find_if(
                m_configuration.installedEngineVersions,
                [](const InstalledEngineVersionView &a_version) { return a_version.isSelected; });
            if (selectedEngine == m_configuration.installedEngineVersions.end() || !selectedEngine->isAvailable)
            {
                return Result<EditorLaunchRequest>::failure(make_project_hub_error(
                    *m_assertContext, ProjectHubError::ProjectUnsupported,
                    "Selected Installed Engine Version cannot launch the Editor"));
            }
            installedEngineIdentity = InstalledEngineLaunchIdentity{
                selectedEngine->directoryName, selectedEngine->bundleId, selectedEngine->manifestDigest};
        }
        std::optional<std::string> sceneLocator;
        std::optional<std::string> expectedSceneAssetId;
        if (a_initialSceneLocator.has_value())
        {
            auto parsedSceneLocator = RelativePath::parse(*a_initialSceneLocator, *m_assertContext);
            if (!parsedSceneLocator)
            {
                return Result<EditorLaunchRequest>::failure(reclassify_project_hub_error(
                    *m_assertContext, ProjectHubError::InvalidSceneLocator, "Initial scene locator is invalid",
                    std::move(*parsedSceneLocator.try_error())));
            }
            sceneLocator = std::string(parsedSceneLocator.try_value()->text());
        }
        else if (descriptor.try_value()->default_scene().has_value())
        {
            sceneLocator = std::string(descriptor.try_value()->default_scene()->source_locator().text());
            expectedSceneAssetId = std::string(descriptor.try_value()->default_scene()->scene_asset_id());
        }
        auto descriptorLocator = m_platform->compose_descriptor_locator(found->locator());
        if (!descriptorLocator)
        {
            return Result<EditorLaunchRequest>::failure(reclassify_project_hub_error(
                *m_assertContext, ProjectHubError::InvalidLocator, "Project descriptor locator could not be composed",
                std::move(*descriptorLocator.try_error())));
        }
        std::string expectedProjectId(descriptor.try_value()->project_id().text());
        std::string compatibilityId =
            make_engine_compatibility_id(descriptor.try_value()->engine_compatibility(), *m_assertContext);
        std::string projectLocator(found->locator());
        auto candidate = clone_registry();
        if (!candidate)
        {
            return Result<EditorLaunchRequest>::failure(std::move(*candidate.try_error()));
        }
        auto registered = candidate.try_value()->register_project(*descriptor.try_value(), projectLocator,
                                                                  a_openedMilliseconds, *m_assertContext);
        if (!registered)
        {
            return Result<EditorLaunchRequest>::failure(std::move(*registered.try_error()));
        }
        auto committed = commit_registry(std::move(*candidate.try_value()));
        if (!committed)
        {
            return Result<EditorLaunchRequest>::failure(std::move(*committed.try_error()));
        }
        return Result<EditorLaunchRequest>::success(
            EditorLaunchRequest(std::move(*descriptorLocator.try_value()), std::move(expectedProjectId),
                                std::move(compatibilityId), std::move(sceneLocator), std::move(expectedSceneAssetId),
                                std::move(installedEngineIdentity)));
    }
    catch (...)
    {
        m_assertContext->fatal_handler().terminate("Cue.ProjectHub open request generation failed");
    }
    std::terminate();
}

Result<ProjectDescriptorMigrationOutcome> ProjectHubService::migrate_project(std::string_view a_projectId) noexcept
{
    try
    {
        auto projectId = parse_project_id(a_projectId);
        if (!projectId)
        {
            return Result<ProjectDescriptorMigrationOutcome>::failure(std::move(*projectId.try_error()));
        }
        const auto entry = std::find_if(m_registry.entries().begin(), m_registry.entries().end(),
                                        [&projectId](const RecentProject &a_candidate)
                                        { return a_candidate.project_id() == *projectId.try_value(); });
        if (entry == m_registry.entries().end())
        {
            return Result<ProjectDescriptorMigrationOutcome>::failure(make_project_hub_error(
                *m_assertContext, ProjectHubError::ProjectNotFound, "Project is not in the Recent registry"));
        }
        auto root = m_platform->open_root(entry->locator());
        if (!root)
        {
            return Result<ProjectDescriptorMigrationOutcome>::failure(
                reclassify_project_hub_error(*m_assertContext, ProjectHubError::ProjectBroken,
                                             "Project locator could not be opened", std::move(*root.try_error())));
        }
        if (*root.try_value() == nullptr)
        {
            return Result<ProjectDescriptorMigrationOutcome>::failure(make_project_hub_error(
                *m_assertContext, ProjectHubError::ProjectMissing, "Project locator does not exist"));
        }
        auto source = load_project_descriptor(**root.try_value(), *m_assertContext);
        if (!source)
        {
            return Result<ProjectDescriptorMigrationOutcome>::failure(
                reclassify_project_hub_error(*m_assertContext, ProjectHubError::ProjectBroken,
                                             "Project descriptor could not be loaded", std::move(*source.try_error())));
        }
        if (source.try_value()->project_id() != *projectId.try_value())
        {
            return Result<ProjectDescriptorMigrationOutcome>::failure(
                make_project_hub_error(*m_assertContext, ProjectHubError::ProjectIdentityMismatch,
                                       "Project descriptor identity differs from the Recent registry"));
        }
        if (source.try_value()->schema_version() >= k_currentProjectDescriptorSchemaVersion ||
            m_configuration.supportedProjectFormatVersion != k_currentProjectDescriptorSchemaVersion)
        {
            return Result<ProjectDescriptorMigrationOutcome>::failure(
                make_project_hub_error(*m_assertContext, ProjectHubError::ProjectUnsupported,
                                       "Project does not have an explicitly supported descriptor migration"));
        }

        auto migrated = migrate_project_descriptor(**root.try_value(), *m_assertContext);
        if (!migrated)
        {
            return Result<ProjectDescriptorMigrationOutcome>::failure(std::move(*migrated.try_error()));
        }
        const ProjectDescriptor &descriptor = migrated.try_value()->descriptor();
        auto compatibility = evaluate_project_compatibility(
            descriptor.schema_version(), m_configuration.supportedProjectFormatVersion,
            descriptor.engine_compatibility(), m_configuration.currentEngineVersion, m_configuration.capabilityProfile,
            m_configuration.capabilitySnapshot, *m_assertContext);
        if (!compatibility)
        {
            return Result<ProjectDescriptorMigrationOutcome>::failure(std::move(*compatibility.try_error()));
        }
        const auto row = std::find_if(m_projects.begin(), m_projects.end(), [&a_projectId](const ProjectRowView &a_row)
                                      { return a_row.projectId == a_projectId; });
        if (row == m_projects.end())
        {
            return Result<ProjectDescriptorMigrationOutcome>::failure(make_project_hub_error(
                *m_assertContext, ProjectHubError::ProjectNotFound, "Project view disappeared during migration"));
        }
        row->displayName = std::string(descriptor.display_name());
        row->state = ProjectEntryState::Available;
        row->problem = ProjectEntryProblem::None;
        row->compatibilityStatus = compatibility.try_value()->status();
        row->canOpen = compatibility.try_value()->can_open();
        row->canMigrate = false;
        row->engineCompatibility = descriptor.engine_compatibility();
        row->compatibilityReasons.assign(compatibility.try_value()->reasons().begin(),
                                         compatibility.try_value()->reasons().end());
        return std::move(migrated);
    }
    catch (...)
    {
        m_assertContext->fatal_handler().terminate("Cue.ProjectHub descriptor migration failed");
    }
    std::terminate();
}

Result<void> ProjectHubService::set_project_pinned(std::string_view a_projectId, bool a_isPinned) noexcept
{
    auto projectId = parse_project_id(a_projectId);
    if (!projectId)
    {
        return Result<void>::failure(std::move(*projectId.try_error()));
    }
    auto candidate = clone_registry();
    if (!candidate)
    {
        return Result<void>::failure(std::move(*candidate.try_error()));
    }
    auto changed = candidate.try_value()->set_project_pinned(*projectId.try_value(), a_isPinned, *m_assertContext);
    return changed ? commit_registry(std::move(*candidate.try_value())) : std::move(changed);
}

Result<void> ProjectHubService::move_pinned_project(std::string_view a_projectId, std::size_t a_targetIndex) noexcept
{
    auto projectId = parse_project_id(a_projectId);
    if (!projectId)
    {
        return Result<void>::failure(std::move(*projectId.try_error()));
    }
    auto candidate = clone_registry();
    if (!candidate)
    {
        return Result<void>::failure(std::move(*candidate.try_error()));
    }
    auto changed = candidate.try_value()->move_pinned_project(*projectId.try_value(), a_targetIndex, *m_assertContext);
    return changed ? commit_registry(std::move(*candidate.try_value())) : std::move(changed);
}

Result<void> ProjectHubService::open_project_folder(std::string_view a_projectId) noexcept
{
    auto projectId = parse_project_id(a_projectId);
    if (!projectId)
    {
        return Result<void>::failure(std::move(*projectId.try_error()));
    }
    const auto row = std::find_if(m_projects.begin(), m_projects.end(), [&projectId](const ProjectRowView &a_row)
                                  { return a_row.projectId == projectId.try_value()->text(); });
    if (row == m_projects.end())
    {
        return Result<void>::failure(
            make_project_hub_error(*m_assertContext, ProjectHubError::ProjectNotFound, "Project is not registered"));
    }
    auto root = m_platform->open_root(row->locator);
    if (!root)
    {
        return Result<void>::failure(reclassify_project_hub_error(*m_assertContext, ProjectHubError::InvalidLocator,
                                                                  "Project folder could not be opened",
                                                                  std::move(*root.try_error())));
    }
    if (*root.try_value() == nullptr)
    {
        return Result<void>::failure(
            make_project_hub_error(*m_assertContext, ProjectHubError::ProjectMissing, "Project folder does not exist"));
    }
    return m_platform->open_project_folder(row->locator);
}

Result<void> ProjectHubService::remove_project(std::string_view a_projectId) noexcept
{
    auto projectId = parse_project_id(a_projectId);
    if (!projectId)
    {
        return Result<void>::failure(std::move(*projectId.try_error()));
    }
    auto candidate = clone_registry();
    if (!candidate)
    {
        return Result<void>::failure(std::move(*candidate.try_error()));
    }
    auto removed = candidate.try_value()->remove_project(*projectId.try_value(), *m_assertContext);
    return removed ? commit_registry(std::move(*candidate.try_value())) : std::move(removed);
}

Result<RecentProjectRegistry> ProjectHubService::clone_registry() const noexcept
{
    auto serialized = serialize_recent_project_registry(m_registry, *m_assertContext);
    if (!serialized)
    {
        return Result<RecentProjectRegistry>::failure(std::move(*serialized.try_error()));
    }
    return parse_recent_project_registry(*serialized.try_value(), *m_assertContext);
}

void ProjectHubService::refresh_after_open_failure(Error &a_primary) noexcept
{
    auto refreshed = refresh();
    if (!refreshed)
    {
        if (is_durability_unknown(*refreshed.try_error()))
        {
            Error uncertain = reclassify_project_hub_error(
                *m_assertContext, ProjectHubError::OpenRejectedViewDurabilityUnknown,
                "Project Hub view was published after open rejection but durability could not be confirmed",
                std::move(a_primary));
            uncertain.append_secondary_diagnostics(
                *m_assertContext, *refreshed.try_error(),
                "Project Hub view refresh was published but durability could not be confirmed", "View Refresh");
            a_primary = std::move(uncertain);
            return;
        }
        a_primary.append_secondary_diagnostics(*m_assertContext, *refreshed.try_error(),
                                               "Project Hub view refresh failed after open rejection", "Refresh");
    }
}

Result<void> ProjectHubService::commit_registry(RecentProjectRegistry &&a_registry) noexcept
{
    auto prepared = prepare_registry_snapshot(a_registry);
    if (!prepared)
    {
        return Result<void>::failure(std::move(*prepared.try_error()));
    }
    auto saved = save_recent_project_registry(*m_workspaceFilesystem, a_registry, *m_assertContext);
    if (!saved)
    {
        const bool wasPublished = is_durability_unknown(*saved.try_error());
        Error primary =
            reclassify_project_hub_error(*m_assertContext, ProjectHubError::PersistenceFailure,
                                         wasPublished ? "Project Hub registry durability could not be confirmed"
                                                      : "Project Hub registry could not be saved",
                                         std::move(*saved.try_error()));
        if (wasPublished)
        {
            m_registry = std::move(a_registry);
            m_projects = std::move(prepared.try_value()->projects);
        }
        return Result<void>::failure(std::move(primary));
    }
    m_registry = std::move(a_registry);
    m_projects = std::move(prepared.try_value()->projects);
    return Result<void>::success();
}

Result<ProjectId> ProjectHubService::parse_project_id(std::string_view a_projectId) const noexcept
{
    return ProjectId::parse(a_projectId, *m_assertContext);
}
} // namespace cue::project_hub
