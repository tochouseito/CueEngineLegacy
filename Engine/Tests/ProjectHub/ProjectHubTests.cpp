#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/ProjectHub/Error.h>
#include <Cue/ProjectHub/Service.h>

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    [[noreturn]] void terminate() noexcept override
    {
        std::terminate();
    }

    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::terminate();
    }
};

[[nodiscard]] std::string to_utf8(const std::filesystem::path &a_path)
{
    const std::u8string text = a_path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

class TestDirectory final
{
  public:
    TestDirectory()
    {
        m_path = std::filesystem::temp_directory_path() /
                 ("CueProjectHubTests-" + std::to_string(static_cast<unsigned long>(GetCurrentProcessId())));
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
        std::filesystem::create_directories(m_path / "Workspace", error);
        m_valid = !error;
    }

    TestDirectory(const TestDirectory &) = delete;
    TestDirectory &operator=(const TestDirectory &) = delete;

    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return m_valid;
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
    bool m_valid = false;
};

class FailingWorkspaceFilesystem final : public cue::FilesystemRoot
{
  public:
    explicit FailingWorkspaceFilesystem(std::unique_ptr<cue::FilesystemRoot> a_inner,
                                        const cue::AssertContext &a_assertContext) noexcept
        : m_inner(std::move(a_inner)), m_assertContext(&a_assertContext)
    {
    }

    void set_write_failure(bool a_shouldFail) noexcept
    {
        m_shouldFail = a_shouldFail;
    }

    void set_write_durability_unknown(bool a_shouldFail) noexcept
    {
        m_writeDurabilityUnknown = a_shouldFail;
    }

    void set_publish_durability_unknown(bool a_shouldFail) noexcept
    {
        m_publishDurabilityUnknown = a_shouldFail;
    }

    void reset_write_count() noexcept
    {
        m_writeCount = 0U;
    }

    [[nodiscard]] std::size_t write_count() const noexcept
    {
        return m_writeCount;
    }

    /// @brief Wrapped Rootの比較専用Identityを転送する
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> root_identity() const noexcept override
    {
        return m_inner->root_identity();
    }

    [[nodiscard]] cue::Result<cue::EntryType> query_entry(const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->query_entry(a_path);
    }

    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &a_path,
                                                                std::size_t a_maxBytes) noexcept override
    {
        return m_inner->read_file(a_path, a_maxBytes);
    }

    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->create_directories(a_path);
    }

    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &a_path,
                                                      std::span<const std::byte> a_bytes) noexcept override
    {
        ++m_writeCount;
        if (m_shouldFail)
        {
            return cue::Result<void>::failure(cue::project_hub::make_project_hub_error(
                *m_assertContext, cue::project_hub::ProjectHubError::PersistenceFailure,
                "Test workspace write failure"));
        }
        auto written = m_inner->write_file_atomic(a_path, a_bytes);
        if (!written)
        {
            return written;
        }
        if (m_writeDurabilityUnknown)
        {
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                                                 "Test workspace durability is unknown"));
        }
        return cue::Result<void>::success();
    }

    [[nodiscard]] cue::Result<void> write_recovery_backup_atomic(
        const cue::RelativePath &a_destination, std::span<const std::byte> a_bytes,
        const cue::AssertContext &a_assertContext) noexcept override
    {
        return m_inner->write_recovery_backup_atomic(a_destination, a_bytes, a_assertContext);
    }

    [[nodiscard]] cue::Result<cue::FileWriteLease> acquire_file_write_lease(
        const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->acquire_file_write_lease(a_path);
    }

    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(cue::FileWriteLease &a_lease,
                                                                   const cue::RelativePath &a_path,
                                                                   cue::FileFingerprint a_expected,
                                                                   std::size_t a_maximumExpectedBytes,
                                                                   std::span<const std::byte> a_bytes) noexcept override
    {
        return m_inner->write_file_atomic_if_unchanged(a_lease, a_path, a_expected, a_maximumExpectedBytes, a_bytes);
    }

    [[nodiscard]] cue::Result<void> remove_file(const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->remove_file(a_path);
    }

    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(
        const cue::RelativePath &a_destination) noexcept override
    {
        return m_inner->create_staging_area(a_destination);
    }

    [[nodiscard]] cue::Result<void> publish_staging_area(
        cue::StagingArea &&a_staging, const cue::RelativePath &a_destination,
        const cue::StagingPublishAuthorization *a_authorization = nullptr) noexcept override
    {
        auto published = m_inner->publish_staging_area(std::move(a_staging), a_destination, a_authorization);
        if (!published)
        {
            return published;
        }
        if (m_publishDurabilityUnknown)
        {
            m_publishDurabilityUnknown = false;
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                                                 "Test project publish durability is unknown"));
        }
        return cue::Result<void>::success();
    }

    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&a_staging) noexcept override
    {
        return m_inner->rollback_staging_area(std::move(a_staging));
    }

  private:
    std::unique_ptr<cue::FilesystemRoot> m_inner;
    const cue::AssertContext *m_assertContext;
    bool m_shouldFail = false;
    bool m_writeDurabilityUnknown = false;
    bool m_publishDurabilityUnknown = false;
    std::size_t m_writeCount = 0U;
};

class TestProjectHubPlatform final : public cue::project_hub::ProjectHubPlatform
{
  public:
    explicit TestProjectHubPlatform(const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext)
    {
    }

    void set_next_project_publish_durability_unknown() noexcept
    {
        m_nextProjectPublishDurabilityUnknown = true;
    }

    /// @brief 次の Project 生成を Staging 内 Descriptor 書込みの DurabilityUnknown で失敗させる
    void set_next_project_write_durability_unknown() noexcept
    {
        m_nextProjectWriteDurabilityUnknown = true;
    }

    void set_failed_locator(std::string_view a_locator)
    {
        m_failedLocator = std::string(a_locator);
    }

    void clear_failed_locator() noexcept
    {
        m_failedLocator.reset();
    }

    /// @brief 指定Locatorの次回Openだけ成功させ、その直後の再検証ではMissingを返す
    void set_missing_after_next_open(std::string_view a_locator)
    {
        m_missingAfterNextOpenLocator = std::string(a_locator);
        m_matchingOpenCount = 0U;
    }

    [[nodiscard]] cue::Result<std::string> normalize_project_locator(std::string_view a_locator) noexcept override
    {
        try
        {
            if (a_locator.empty())
            {
                return cue::Result<std::string>::failure(cue::project_hub::make_project_hub_error(
                    *m_assertContext, cue::project_hub::ProjectHubError::InvalidLocator, "Locator is empty"));
            }
            std::filesystem::path path{std::string(a_locator)};
            path = std::filesystem::absolute(path).lexically_normal();
            std::string normalized = to_utf8(path);
            return cue::Result<std::string>::success(std::move(normalized));
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate("Test locator normalization failed");
        }
        std::terminate();
    }

    [[nodiscard]] cue::Result<std::string> compose_project_locator(std::string_view a_parentLocator,
                                                                   std::string_view a_projectName) noexcept override
    {
        try
        {
            std::filesystem::path path{std::string(a_parentLocator)};
            path /= std::string(a_projectName);
            std::string composed = to_utf8(path.lexically_normal());
            return cue::Result<std::string>::success(std::move(composed));
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate("Test project locator composition failed");
        }
        std::terminate();
    }

    [[nodiscard]] cue::Result<std::string> compose_descriptor_locator(
        std::string_view a_projectLocator) noexcept override
    {
        try
        {
            std::filesystem::path path{std::string(a_projectLocator)};
            path /= "CueProject.json";
            std::string composed = to_utf8(path.lexically_normal());
            return cue::Result<std::string>::success(std::move(composed));
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate("Test descriptor locator composition failed");
        }
        std::terminate();
    }

    [[nodiscard]] cue::Result<std::unique_ptr<cue::FilesystemRoot>> open_root(
        std::string_view a_locator) noexcept override
    {
        try
        {
            if (m_failedLocator.has_value() && *m_failedLocator == a_locator)
            {
                return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::failure(cue::make_io_error(
                    *m_assertContext, cue::IoError::PermissionDenied, "Test locator access failure"));
            }
            if (m_missingAfterNextOpenLocator.has_value() && *m_missingAfterNextOpenLocator == a_locator)
            {
                if (m_matchingOpenCount == 1U)
                {
                    m_missingAfterNextOpenLocator.reset();
                    m_matchingOpenCount = 0U;
                    std::unique_ptr<cue::FilesystemRoot> missing;
                    return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::success(std::move(missing));
                }
                ++m_matchingOpenCount;
            }
            const std::filesystem::path path{std::string(a_locator)};
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            if (error)
            {
                return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::failure(
                    cue::project_hub::make_project_hub_error(*m_assertContext,
                                                             cue::project_hub::ProjectHubError::InvalidLocator,
                                                             "Locator existence could not be queried"));
            }
            if (!exists)
            {
                std::unique_ptr<cue::FilesystemRoot> missing;
                return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::success(std::move(missing));
            }
            auto root = cue::create_windows_filesystem_root(to_utf8(path), *m_assertContext);
            if (!root || (!m_nextProjectPublishDurabilityUnknown && !m_nextProjectWriteDurabilityUnknown))
            {
                return root;
            }
            auto injected =
                std::make_unique<FailingWorkspaceFilesystem>(std::move(*root.try_value()), *m_assertContext);
            if (m_nextProjectPublishDurabilityUnknown)
            {
                m_nextProjectPublishDurabilityUnknown = false;
                injected->set_publish_durability_unknown(true);
            }
            if (m_nextProjectWriteDurabilityUnknown)
            {
                m_nextProjectWriteDurabilityUnknown = false;
                injected->set_write_durability_unknown(true);
            }
            std::unique_ptr<cue::FilesystemRoot> result(std::move(injected));
            return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::success(std::move(result));
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate("Test project root open failed");
        }
        std::terminate();
    }

    [[nodiscard]] cue::Result<std::unique_ptr<cue::FilesystemRoot>> create_or_open_root(
        std::string_view a_locator) noexcept override
    {
        try
        {
            std::error_code error;
            static_cast<void>(
                std::filesystem::create_directories(std::filesystem::path(std::string(a_locator)), error));
            if (error)
            {
                return cue::Result<std::unique_ptr<cue::FilesystemRoot>>::failure(
                    cue::project_hub::make_project_hub_error(*m_assertContext,
                                                             cue::project_hub::ProjectHubError::InvalidLocator,
                                                             "Test project root creation failed"));
            }
            return open_root(a_locator);
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate("Test project root creation failed");
        }
        std::terminate();
    }

    [[nodiscard]] cue::Result<cue::project_hub::ProjectStorageMetadata> inspect_project_storage(
        std::string_view) noexcept override
    {
        return cue::Result<cue::project_hub::ProjectStorageMetadata>::success({1234U, 5678U});
    }

    [[nodiscard]] cue::Result<void> open_project_folder(std::string_view a_locator) noexcept override
    {
        try
        {
            m_lastOpenedFolder = std::string(a_locator);
            return cue::Result<void>::success();
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate("Test Project folder open failed");
        }
        std::terminate();
    }

    [[nodiscard]] std::string_view last_opened_folder() const noexcept
    {
        return m_lastOpenedFolder;
    }

    [[nodiscard]] cue::Result<cue::ProjectId> next_project_id() noexcept override
    {
        constexpr std::string_view ids[] = {
            "00000000-0000-4000-8000-000000000001", "00000000-0000-4000-8000-000000000002",
            "00000000-0000-4000-8000-000000000003", "00000000-0000-4000-8000-000000000004",
            "00000000-0000-4000-8000-000000000005", "00000000-0000-4000-8000-000000000006"};
        if (m_nextIdentity >= std::size(ids))
        {
            return cue::Result<cue::ProjectId>::failure(cue::project_hub::make_project_hub_error(
                *m_assertContext, cue::project_hub::ProjectHubError::InvalidConfiguration,
                "Test ProjectId source is exhausted"));
        }
        return cue::ProjectId::parse(ids[m_nextIdentity++], *m_assertContext);
    }

    [[nodiscard]] cue::Result<std::string> next_scene_asset_id() noexcept override
    {
        constexpr std::string_view ids[] = {
            "10000000-0000-4000-8000-000000000001", "10000000-0000-4000-8000-000000000002",
            "10000000-0000-4000-8000-000000000003", "10000000-0000-4000-8000-000000000004",
            "10000000-0000-4000-8000-000000000005", "10000000-0000-4000-8000-000000000006"};
        if (m_nextSceneIdentity >= std::size(ids))
        {
            return cue::Result<std::string>::failure(cue::project_hub::make_project_hub_error(
                *m_assertContext, cue::project_hub::ProjectHubError::InvalidConfiguration,
                "Test SceneAssetId source is exhausted"));
        }
        return cue::Result<std::string>::success(std::string(ids[m_nextSceneIdentity++]));
    }

  private:
    const cue::AssertContext *m_assertContext;
    std::size_t m_nextIdentity = 0U;
    std::size_t m_nextSceneIdentity = 0U;
    bool m_nextProjectPublishDurabilityUnknown = false;
    bool m_nextProjectWriteDurabilityUnknown = false;
    std::optional<std::string> m_failedLocator;
    std::optional<std::string> m_missingAfterNextOpenLocator;
    std::size_t m_matchingOpenCount = 0U;
    std::string m_lastOpenedFolder;
};

[[nodiscard]] cue::Result<cue::project_hub::ProjectHubConfiguration> make_configuration(
    const cue::AssertContext &a_assertContext) noexcept
{
    auto profile = cue::ProjectCapabilityProfile::create({}, a_assertContext);
    auto snapshot = cue::ProjectCapabilitySnapshot::create({}, a_assertContext);
    if (!profile)
    {
        return cue::Result<cue::project_hub::ProjectHubConfiguration>::failure(std::move(*profile.try_error()));
    }
    if (!snapshot)
    {
        return cue::Result<cue::project_hub::ProjectHubConfiguration>::failure(std::move(*snapshot.try_error()));
    }
    cue::project_hub::ProjectHubConfiguration configuration{
        cue::k_currentProjectDescriptorSchemaVersion, cue::EngineVersion{1U, 0U, 0U}, std::move(*profile.try_value()),
        std::move(*snapshot.try_value()),
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}},
        {cue::project_hub::InstalledEngineVersionView{"v1.0.0--11111111-1111-4111-8111-111111111111",
                                                      "1.0.0", cue::EngineVersion{1U, 0U, 0U}, "bundle-one",
                                                      "manifest-one", {}, true, true},
         cue::project_hub::InstalledEngineVersionView{"v1.1.0--22222222-2222-4222-8222-222222222222",
                                                      "1.1.0", cue::EngineVersion{1U, 1U, 0U}, "bundle-two",
                                                      "manifest-two", {}, true, false}}};
    return cue::Result<cue::project_hub::ProjectHubConfiguration>::success(std::move(configuration));
}

[[nodiscard]] const cue::project_hub::ProjectRowView *find_project(
    std::span<const cue::project_hub::ProjectRowView> a_projects, std::string_view a_projectId) noexcept
{
    for (const cue::project_hub::ProjectRowView &project : a_projects)
    {
        if (project.projectId == a_projectId)
        {
            return &project;
        }
    }
    return nullptr;
}

[[nodiscard]] bool overwrite_descriptor(const std::filesystem::path &a_path, std::string_view a_text)
{
    std::ofstream stream(a_path, std::ios::binary | std::ios::trunc);
    stream.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
    return stream.good();
}

[[nodiscard]] bool test_project_hub_workflow(const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    if (!directory.valid())
    {
        return false;
    }
    auto workspace = cue::create_windows_filesystem_root(to_utf8(directory.path() / "Workspace"), a_assertContext);
    auto configuration = make_configuration(a_assertContext);
    TestProjectHubPlatform platform(a_assertContext);
    if (!workspace || !configuration)
    {
        return false;
    }
    FailingWorkspaceFilesystem workspaceFilesystem(std::move(*workspace.try_value()), a_assertContext);
    auto service = cue::project_hub::ProjectHubService::create(workspaceFilesystem, platform,
                                                               std::move(*configuration.try_value()), a_assertContext);
    if (!service || service.try_value()->get()->templates().size() != 1U ||
        service.try_value()->get()->templates()[0].id != cue::project_hub::k_blank3dTemplateId ||
        service.try_value()->get()->installed_engine_versions().size() != 2U ||
        !service.try_value()->get()->installed_engine_versions()[0].isSelected ||
        !service.try_value()->get()->projects().empty())
    {
        return false;
    }

    const std::string projectsLocator = to_utf8(directory.path() / "Projects");
    if (service.try_value()->get()->create_blank_project(projectsLocator, "Invalid", "unknown-template", 0U) ||
        !service.try_value()->get()->create_blank_project(projectsLocator, "Alpha",
                                                          cue::project_hub::k_blank3dTemplateId, 100U) ||
        !service.try_value()->get()->create_blank_project(projectsLocator, "Bravo",
                                                          cue::project_hub::k_blank3dTemplateId, 200U) ||
        !service.try_value()->get()->create_blank_project(projectsLocator, "Charlie",
                                                          cue::project_hub::k_blank3dTemplateId, 300U))
    {
        return false;
    }
    constexpr std::string_view alphaId = "00000000-0000-4000-8000-000000000001";
    constexpr std::string_view bravoId = "00000000-0000-4000-8000-000000000002";
    constexpr std::string_view charlieId = "00000000-0000-4000-8000-000000000003";
    constexpr std::string_view deltaId = "00000000-0000-4000-8000-000000000004";
    constexpr std::string_view echoId = "00000000-0000-4000-8000-000000000005";
    const auto *alpha = find_project(service.try_value()->get()->projects(), alphaId);
    if (alpha == nullptr || alpha->displayName != "Alpha" ||
        alpha->state != cue::project_hub::ProjectEntryState::Available || !alpha->canOpen || alpha->canMigrate ||
        alpha->compatibilityStatus != cue::ProjectCompatibilityStatus::Compatible ||
        !alpha->engineCompatibility.has_value() ||
        alpha->engineCompatibility->minimum != cue::EngineVersion{1U, 0U, 0U} || alpha->editorVersion != "1.0.0" ||
        alpha->latestWriteMilliseconds != 1234U || alpha->byteSize != 5678U)
    {
        return false;
    }
    const std::string alphaLocator = alpha->locator;
    if (!service.try_value()->get()->open_project_folder(alphaId) || platform.last_opened_folder() != alphaLocator ||
        service.try_value()->get()->open_project_folder("00000000-0000-4000-8000-000000000099"))
    {
        return false;
    }

    constexpr std::string_view legacyBravoDescriptor =
        "{\"schemaVersion\":1,\"projectId\":\"00000000-0000-4000-8000-000000000002\","
        "\"displayName\":\"Bravo Project\",\"engineCompatibility\":{\"minimum\":\"1.0.0\","
        "\"maximumExclusive\":\"2.0.0\"},\"roots\":{\"sourceAssets\":\"Assets/Source\","
        "\"runtimeAssets\":\"Assets/Runtime\",\"generated\":\"Generated\",\"saved\":\"Saved\"},"
        "\"defaultScene\":null,\"requiredCapabilities\":[],\"extensions\":{}}";
    if (!overwrite_descriptor(directory.path() / "Projects" / "Bravo" / "CueProject.json", legacyBravoDescriptor) ||
        !service.try_value()->get()->refresh())
    {
        return false;
    }
    const auto *legacyBravo = find_project(service.try_value()->get()->projects(), bravoId);
    if (legacyBravo == nullptr || legacyBravo->canOpen || !legacyBravo->canMigrate ||
        legacyBravo->compatibilityStatus != cue::ProjectCompatibilityStatus::Unsupported)
    {
        return false;
    }
    auto migratedBravo = service.try_value()->get()->migrate_project(bravoId);
    const auto *currentBravo = find_project(service.try_value()->get()->projects(), bravoId);
    if (!migratedBravo || migratedBravo.try_value()->status() != cue::ProjectDescriptorMigrationStatus::Committed ||
        migratedBravo.try_value()->descriptor().default_scene().has_value() || currentBravo == nullptr ||
        !currentBravo->canOpen || currentBravo->canMigrate)
    {
        return false;
    }

    if (!service.try_value()->get()->set_project_pinned(alphaId, true) ||
        !service.try_value()->get()->set_project_pinned(charlieId, true) ||
        !service.try_value()->get()->move_pinned_project(charlieId, 0U) ||
        service.try_value()->get()->projects()[0].projectId != charlieId)
    {
        return false;
    }

    std::error_code error;
    std::filesystem::rename(directory.path() / "Projects" / "Alpha",
                            directory.path() / "Projects" / "AlphaTemporarilyMissing", error);
    workspaceFilesystem.reset_write_count();
    if (error || !service.try_value()->get()->set_project_pinned(charlieId, false) ||
        workspaceFilesystem.write_count() != 1U)
    {
        return false;
    }
    const auto *temporarilyMissingAlpha = find_project(service.try_value()->get()->projects(), alphaId);
    const auto *unpinnedCharlie = find_project(service.try_value()->get()->projects(), charlieId);
    if (temporarilyMissingAlpha == nullptr ||
        temporarilyMissingAlpha->state != cue::project_hub::ProjectEntryState::Missing || unpinnedCharlie == nullptr ||
        unpinnedCharlie->isPinned)
    {
        return false;
    }
    std::filesystem::rename(directory.path() / "Projects" / "AlphaTemporarilyMissing",
                            directory.path() / "Projects" / "Alpha", error);
    if (error || !service.try_value()->get()->refresh())
    {
        return false;
    }

    auto invalidLaunch =
        service.try_value()->get()->open_project(alphaId, 350U, std::string_view("../Outside.cuescene"));
    const auto *alphaAfterInvalidLaunch = find_project(service.try_value()->get()->projects(), alphaId);
    if (invalidLaunch || alphaAfterInvalidLaunch == nullptr || alphaAfterInvalidLaunch->lastOpenedMilliseconds != 100U)
    {
        return false;
    }
    if (!service.try_value()->get()->select_installed_engine_version(
            "v1.1.0--22222222-2222-4222-8222-222222222222") ||
        service.try_value()->get()->select_installed_engine_version("missing-version") ||
        service.try_value()->get()->installed_engine_versions()[0].isSelected ||
        !service.try_value()->get()->installed_engine_versions()[1].isSelected)
    {
        return false;
    }
    std::vector<cue::project_hub::InstalledEngineVersionView> refreshedVersions(
        service.try_value()->get()->installed_engine_versions().begin(),
        service.try_value()->get()->installed_engine_versions().end());
    refreshedVersions[0].isAvailable = false;
    refreshedVersions[0].diagnostic = "Installed Version payload is corrupt";
    std::vector<cue::project_hub::InstalledEngineVersionView> duplicateVersions = refreshedVersions;
    duplicateVersions[0].directoryName = duplicateVersions[1].directoryName;
    if (service.try_value()->get()->replace_installed_engine_versions(std::move(duplicateVersions)) ||
        !service.try_value()->get()->replace_installed_engine_versions(std::move(refreshedVersions)) ||
        service.try_value()->get()->installed_engine_versions()[0].isAvailable ||
        service.try_value()->get()->installed_engine_versions()[0].diagnostic.empty() ||
        !service.try_value()->get()->installed_engine_versions()[1].isSelected)
    {
        return false;
    }
    auto launch = service.try_value()->get()->open_project(alphaId, 400U, std::string_view("Scenes/Start.cuescene"));
    if (!launch || launch.try_value()->protocol_version() != cue::project_hub::k_editorLaunchProtocolVersion ||
        launch.try_value()->expected_project_id() != alphaId ||
        launch.try_value()->engine_compatibility_id() != "cue-engine:[1.0.0,2.0.0)" ||
        !launch.try_value()->initial_scene_locator().has_value() ||
        launch.try_value()->expected_initial_scene_asset_id().has_value() ||
        !launch.try_value()->installed_engine_identity().has_value() ||
        launch.try_value()->installed_engine_identity()->versionDirectory !=
            "v1.1.0--22222222-2222-4222-8222-222222222222" ||
        launch.try_value()->installed_engine_identity()->bundleId != "bundle-two" ||
        launch.try_value()->installed_engine_identity()->manifestDigest != "manifest-two" ||
        launch.try_value()->project_descriptor_locator().find("CueProject.json") == std::string_view::npos)
    {
        return false;
    }
    auto defaultLaunch = service.try_value()->get()->open_project(alphaId, 410U);
    if (!defaultLaunch || !defaultLaunch.try_value()->initial_scene_locator().has_value() ||
        *defaultLaunch.try_value()->initial_scene_locator() != "Scenes/Default.cuescene" ||
        !defaultLaunch.try_value()->expected_initial_scene_asset_id().has_value() ||
        *defaultLaunch.try_value()->expected_initial_scene_asset_id() != "10000000-0000-4000-8000-000000000001" ||
        !std::filesystem::is_regular_file(directory.path() / "Projects" / "Alpha" / "Assets" / "Source" / "Scenes" /
                                          "Default.cuescene"))
    {
        return false;
    }

    platform.set_failed_locator(to_utf8(directory.path() / "Projects" / "Alpha"));
    auto inaccessibleLaunch = service.try_value()->get()->open_project(alphaId, 425U);
    const auto *inaccessibleAlpha = find_project(service.try_value()->get()->projects(), alphaId);
    platform.clear_failed_locator();
    if (inaccessibleLaunch || inaccessibleAlpha == nullptr ||
        inaccessibleAlpha->state != cue::project_hub::ProjectEntryState::Broken ||
        inaccessibleAlpha->problem != cue::project_hub::ProjectEntryProblem::LocatorAccessFailed ||
        !service.try_value()->get()->refresh())
    {
        return false;
    }

    std::filesystem::rename(directory.path() / "Projects" / "Bravo", directory.path() / "Projects" / "BravoMoved",
                            error);
    if (error)
    {
        return false;
    }
    auto missingLaunch = service.try_value()->get()->open_project(bravoId, 450U);
    if (missingLaunch)
    {
        return false;
    }
    const auto *missingBravo = find_project(service.try_value()->get()->projects(), bravoId);
    if (missingBravo == nullptr || missingBravo->state != cue::project_hub::ProjectEntryState::Missing ||
        !service.try_value()->get()->register_project(to_utf8(directory.path() / "Projects" / "BravoMoved"), 500U,
                                                      true))
    {
        return false;
    }
    const auto *movedBravo = find_project(service.try_value()->get()->projects(), bravoId);
    if (movedBravo == nullptr || movedBravo->state != cue::project_hub::ProjectEntryState::Moved)
    {
        return false;
    }

    if (!overwrite_descriptor(directory.path() / "Projects" / "Charlie" / "CueProject.json", "{broken"))
    {
        return false;
    }
    platform.set_missing_after_next_open(to_utf8(directory.path() / "Projects" / "Charlie"));
    workspaceFilesystem.set_write_durability_unknown(true);
    auto brokenLaunch = service.try_value()->get()->open_project(charlieId, 525U);
    workspaceFilesystem.set_write_durability_unknown(false);
    const bool preservedOpenRejection =
        !brokenLaunch && !brokenLaunch.try_error()->causes().empty() &&
        brokenLaunch.try_error()->causes().front().code().domain() == "Cue.ProjectHub" &&
        brokenLaunch.try_error()->causes().front().code().value() ==
            static_cast<std::int64_t>(cue::project_hub::ProjectHubError::ProjectBroken);
    if (brokenLaunch || brokenLaunch.try_error()->code().domain() != "Cue.ProjectHub" ||
        brokenLaunch.try_error()->code().value() !=
            static_cast<std::int64_t>(cue::project_hub::ProjectHubError::OpenRejectedViewDurabilityUnknown) ||
        !preservedOpenRejection)
    {
        return false;
    }
    const auto *brokenCharlie = find_project(service.try_value()->get()->projects(), charlieId);
    if (brokenCharlie == nullptr || brokenCharlie->state != cue::project_hub::ProjectEntryState::Missing)
    {
        return false;
    }

    workspaceFilesystem.set_write_durability_unknown(true);
    auto uncertainPin = service.try_value()->get()->set_project_pinned(charlieId, true);
    workspaceFilesystem.set_write_durability_unknown(false);
    const auto *durabilitySynchronizedCharlie = find_project(service.try_value()->get()->projects(), charlieId);
    if (uncertainPin || uncertainPin.try_error()->root_code().domain() != "Cue.IO" ||
        uncertainPin.try_error()->root_code().value() != static_cast<std::int64_t>(cue::IoError::DurabilityUnknown) ||
        durabilitySynchronizedCharlie == nullptr || !durabilitySynchronizedCharlie->isPinned)
    {
        return false;
    }

    workspaceFilesystem.set_write_failure(true);
    auto partiallyCreated = service.try_value()->get()->create_blank_project(
        projectsLocator, "Delta", cue::project_hub::k_blank3dTemplateId, 550U);
    workspaceFilesystem.set_write_failure(false);
    if (!partiallyCreated || partiallyCreated.try_value()->is_recent_registered() ||
        partiallyCreated.try_value()->try_recent_persistence_error() == nullptr ||
        !std::filesystem::exists(directory.path() / "Projects" / "Delta" / "CueProject.json") ||
        find_project(service.try_value()->get()->projects(), deltaId) != nullptr ||
        !service.try_value()->get()->register_project(partiallyCreated.try_value()->project_locator(), 550U, false) ||
        find_project(service.try_value()->get()->projects(), deltaId) == nullptr ||
        !service.try_value()->get()->remove_project(deltaId))
    {
        return false;
    }

    platform.set_next_project_publish_durability_unknown();
    auto uncertainCreation = service.try_value()->get()->create_blank_project(
        projectsLocator, "Echo", cue::project_hub::k_blank3dTemplateId, 575U);
    if (!uncertainCreation || !uncertainCreation.try_value()->is_recent_registered() ||
        uncertainCreation.try_value()->try_creation_durability_error() == nullptr ||
        uncertainCreation.try_value()->try_recent_persistence_error() != nullptr ||
        uncertainCreation.try_value()->try_creation_durability_error()->root_code().domain() != "Cue.IO" ||
        uncertainCreation.try_value()->try_creation_durability_error()->root_code().value() !=
            static_cast<std::int64_t>(cue::IoError::DurabilityUnknown) ||
        find_project(service.try_value()->get()->projects(), echoId) == nullptr ||
        !service.try_value()->get()->remove_project(echoId))
    {
        return false;
    }

    platform.set_next_project_write_durability_unknown();
    auto unpublishedCreation = service.try_value()->get()->create_blank_project(
        projectsLocator, "Foxtrot", cue::project_hub::k_blank3dTemplateId, 600U);
    if (unpublishedCreation || unpublishedCreation.try_error()->root_code().domain() != "Cue.IO" ||
        unpublishedCreation.try_error()->root_code().value() !=
            static_cast<std::int64_t>(cue::IoError::DurabilityUnknown) ||
        std::filesystem::exists(directory.path() / "Projects" / "Foxtrot"))
    {
        return false;
    }

    workspaceFilesystem.set_write_failure(true);
    auto failedRemoval = service.try_value()->get()->remove_project(alphaId);
    workspaceFilesystem.set_write_failure(false);
    if (failedRemoval || find_project(service.try_value()->get()->projects(), alphaId) == nullptr)
    {
        return false;
    }
    if (!service.try_value()->get()->remove_project(alphaId) ||
        find_project(service.try_value()->get()->projects(), alphaId) != nullptr ||
        !std::filesystem::exists(directory.path() / "Projects" / "Alpha" / "CueProject.json"))
    {
        return false;
    }

    auto restartedConfiguration = make_configuration(a_assertContext);
    if (!restartedConfiguration)
    {
        return false;
    }
    auto restarted = cue::project_hub::ProjectHubService::create(
        workspaceFilesystem, platform, std::move(*restartedConfiguration.try_value()), a_assertContext);
    return restarted && restarted.try_value()->get()->projects().size() == 2U &&
           find_project(restarted.try_value()->get()->projects(), bravoId) != nullptr &&
           find_project(restarted.try_value()->get()->projects(), charlieId) != nullptr &&
           find_project(restarted.try_value()->get()->projects(), charlieId)->isPinned;
}
} // namespace

int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_project_hub_workflow(assertContext) ? 0 : 1;
}
