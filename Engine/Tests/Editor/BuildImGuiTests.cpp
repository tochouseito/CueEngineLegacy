#include <Cue/Editor/ImGui/BuildPresenter.h>
#include <Cue/Editor/ImGui/PackagePresenter.h>

#include <Cue/EditorCore/EditorIntent.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Project/Generator.h>
#include <Cue/Scene/ComponentData.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Scene/Serialization.h>
#include <Cue/Schema/Registry.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>

namespace
{
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 1U};
constexpr std::string_view k_packageProjectId = "00000000-0000-4000-8000-000000000941";
constexpr std::string_view k_reloadedPackageProjectId = "00000000-0000-4000-8000-000000000942";
constexpr std::string_view k_packageSceneId = "10000000-0000-4000-8000-000000000941";

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief 想定外の引数なしFatal終了を固有Exit Codeで検出する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief 想定外のMessage付きFatal終了を固有Exit Codeで検出する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

/// @brief Presenter TestのScene編集へ重複しないUUID Version 4候補を返す
class TestSceneIdentitySource final : public cue::scene::SceneIdentitySource
{
  public:
    /// @brief Counterを埋め込んだRFC 4122 UUID Version 4候補を返す
    [[nodiscard]] cue::scene::IdentityBytes next_identity() noexcept override
    {
        cue::scene::IdentityBytes bytes{};
        for (std::size_t index = 0U; index < sizeof(m_next); ++index)
        {
            bytes[bytes.size() - 1U - index] = static_cast<std::uint8_t>((m_next >> (index * 8U)) & 0xFFU);
        }
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
        ++m_next;
        return bytes;
    }

  private:
    std::uint64_t m_next = 0x100U;
};

/// @brief Test前提違反をSource Line由来のExit Codeで即時報告する
void require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::_Exit(static_cast<int>((a_location.line() % 200U) + 20U));
    }
}

enum class RunnerMode : std::uint8_t
{
    BlockUntilCancelled,
    Succeed,
    Fail
};

struct RunnerState final
{
    std::atomic<RunnerMode> mode = RunnerMode::BlockUntilCancelled;
    std::atomic<std::uint64_t> sequence = 0U;
};

class ControlledRunner final : public cue::ChildProcessRunner
{
  public:
    /// @brief Testが共有するProcess状態を借用して制御可能Runnerを構築する
    explicit ControlledRunner(RunnerState &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief 指定Modeに従う成功、失敗、Cancel結果と順序付きLogを返す
    [[nodiscard]] cue::Result<cue::ChildProcessResult> run(
        const cue::ChildProcessRequest &, const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        while (m_state->mode.load(std::memory_order_acquire) == RunnerMode::BlockUntilCancelled &&
               !a_cancellation.is_cancel_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const std::uint64_t sequence = m_state->sequence.fetch_add(1U, std::memory_order_relaxed);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::cancelled(
                {{sequence, cue::ChildProcessStream::StandardError, "cancelled\n"}}));
        }
        const std::uint32_t exitCode = m_state->mode.load(std::memory_order_acquire) == RunnerMode::Fail ? 2U : 0U;
        return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::exited(
            exitCode, {{sequence, cue::ChildProcessStream::StandardOutput, "build##log\n" + std::string("\xC3", 1U)}}));
    }

  private:
    RunnerState *m_state;
};

class TestPublisher final : public cue::BuildArtifactPublisher
{
  public:
    class TestLease final : public cue::BuildWorkspaceLease
    {
      public:
        /// @brief 検証用Leaseの生存期間だけを表す
        TestLease() noexcept = default;
        /// @brief Native Resourceを持たない検証用Leaseを破棄する
        ~TestLease() override = default;
    };

    /// @brief Assert境界を借用して検証用Publisherを構築する
    explicit TestPublisher(const cue::AssertContext &a_assertContext) noexcept : m_assertContext(&a_assertContext)
    {
    }

    /// @brief 取消前ならBuild Workerへ検証用Exclusive Leaseを返す
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>> acquire_build_lease(
        const cue::BuildPlan &, const cue::ChildProcessCancellation &a_cancellation,
        cue::BuildArtifactLockDeadline) noexcept override
    {
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(std::nullopt);
        }
        return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(
            std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>(std::make_unique<TestLease>()));
    }

    /// @brief Cancel前だけ必須Fileを含む決定的な検証用Artifact Inventoryを返す
    [[nodiscard]] cue::Result<std::optional<cue::BuildArtifactInventory>> publish(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation,
        std::unique_ptr<cue::BuildWorkspaceLease> a_buildLease, cue::BuildArtifactLockDeadline) noexcept override
    {
        static_cast<void>(a_buildLease);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
        }
        auto inventory =
            cue::BuildArtifactInventory::create(a_plan, "71234567-89ab-4cde-8f01-23456789abcd",
                                                {{"CueGameModule.dll", 256U, std::string(64U, 'a')},
                                                 {"CueGameModule.metadata.json", 64U, std::string(64U, 'b')}},
                                                *m_assertContext);
        if (!inventory)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(std::move(*inventory.try_error()));
        }
        return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(
            std::optional<cue::BuildArtifactInventory>(std::move(*inventory.try_value())));
    }

  private:
    const cue::AssertContext *m_assertContext;
};

/// @brief Package RetryがBuild失敗するTestで未使用のArtifact Read境界を満たす
class TestArtifactReader final : public cue::BuildArtifactReader
{
  public:
    /// @brief 未使用のRead要求へCancel相当の空Leaseを返す
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>> acquire_current_read_lease(
        const cue::BuildArtifactInventory &, const cue::BuildArtifactReadCancellation &,
        cue::BuildArtifactLockDeadline) noexcept override
    {
        return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::success(std::nullopt);
    }
};

/// @brief Package Presenter Test用の一時Rootを所有する
class TestDirectory final
{
  public:
    /// @brief Process固有時刻を含む一時Rootを作成する
    TestDirectory()
        : m_path(std::filesystem::temp_directory_path() /
                 ("CuePackagePresenterTests-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(m_path);
    }

    TestDirectory(const TestDirectory &) = delete;
    TestDirectory &operator=(const TestDirectory &) = delete;

    /// @brief Test所有Rootだけを除去する
    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    /// @brief 一時RootのAbsolute Pathを返す
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

class TestOperationIdSource final : public cue::editor::BuildOperationIdSource
{
  public:
    /// @brief 決定的なOperation ID列の所有権を取得してSourceを構築する
    explicit TestOperationIdSource(std::vector<std::string> a_ids) noexcept : m_ids(std::move(a_ids))
    {
    }

    /// @brief 次の検証用Operation IDを一度だけ返す
    [[nodiscard]] cue::Result<std::string> next_operation_id() noexcept override
    {
        require(m_index < m_ids.size());
        std::string operationId = m_ids[m_index++];
        return cue::Result<std::string>::success(std::move(operationId));
    }

  private:
    std::vector<std::string> m_ids;
    std::size_t m_index = 0U;
};

/// @brief Process起動を行わないTest用のAbsolute Runner設定を返す
[[nodiscard]] cue::CMakeRunnerSettings make_settings()
{
    return {"C:/Tools/cmake.exe", "C:/CueEngine", {}, std::chrono::seconds(5), std::chrono::seconds(5), "14.51.36231"};
}

/// @brief Package Presenter Test用Project Descriptorを作る
[[nodiscard]] cue::ProjectDescriptor make_package_descriptor(const cue::AssertContext &a_assertContext) noexcept
{
    auto projectId = cue::ProjectId::parse(k_packageProjectId, a_assertContext);
    require(projectId.has_value());
    auto descriptor = cue::create_blank_project_descriptor(
        std::move(*projectId.try_value()), "Package Presenter Test",
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}}, k_packageSceneId,
        a_assertContext);
    require(descriptor.has_value());
    return std::move(*descriptor.try_value());
}

/// @brief Package Retryを開始できる最小Runtime Dataを作る
[[nodiscard]] cue::package::MinimalRuntimeDataPublication make_runtime_data(
    const cue::AssertContext &a_assertContext) noexcept
{
    cue::ProjectDescriptor descriptor = make_package_descriptor(a_assertContext);
    auto sceneId = cue::scene::SceneAssetId::parse(k_packageSceneId, a_assertContext);
    require(sceneId.has_value());
    cue::scene::SceneDocument scene =
        cue::scene::SceneDocument::create(std::move(*sceneId.try_value()), a_assertContext);
    auto snapshot = cue::scene::create_scene_snapshot(scene, a_assertContext);
    require(snapshot.has_value());
    auto runtimeData = cue::package::publish_minimal_runtime_data(descriptor, *snapshot.try_value(), a_assertContext);
    require(runtimeData.has_value());
    return std::move(*runtimeData.try_value());
}

/// @brief Presenterの一Frame分のShortcut処理と描画を実行する
void draw_frame(cue::editor::BuildPresenter &a_presenter) noexcept
{
    ImGui::NewFrame();
    a_presenter.process_shortcuts();
    a_presenter.draw();
    ImGui::Render();
}

/// @brief F6のPressとReleaseをFrameへ送りBuild Shortcutを再現する
void press_build_shortcut(cue::editor::BuildPresenter &a_presenter) noexcept
{
    ImGuiIO &input = ImGui::GetIO();
    ImGui::NewFrame();
    ImGui::SetNextWindowFocus();
    a_presenter.draw();
    ImGui::Render();
    input.AddKeyEvent(ImGuiKey_F6, true);
    draw_frame(a_presenter);
    input.AddKeyEvent(ImGuiKey_F6, false);
    draw_frame(a_presenter);
}

/// @brief Configuration、Cancel、Retry、履歴再表示、Keyboard、終了確認をHeadless検証する
void test_build_workflow(const cue::AssertContext &a_assertContext)
{
    RunnerState runnerState;
    auto serviceResult =
        cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(runnerState),
                                      std::make_unique<TestPublisher>(a_assertContext), a_assertContext);
    require(serviceResult.has_value());
    std::unique_ptr<cue::GameBuildService> service = std::move(*serviceResult.try_value());
    auto operationIds = std::make_unique<TestOperationIdSource>(
        std::vector<std::string>{"", "01234567-89ab-4cde-8f01-23456789abcd", "11234567-89ab-4cde-8f01-23456789abcd",
                                 "21234567-89ab-4cde-8f01-23456789abcd", "31234567-89ab-4cde-8f01-23456789abcd",
                                 "41234567-89ab-4cde-8f01-23456789abcd", "51234567-89ab-4cde-8f01-23456789abcd"});
    std::unique_ptr<cue::editor::BuildPresenter> presenter =
        cue::editor::BuildPresenter::create(*service, std::filesystem::current_path().generic_string(),
                                            k_workspaceCompatibility, std::move(operationIds), a_assertContext);

    require(presenter->can_start());
    require(!presenter->submit(cue::editor::EditorBuildCommand::Start));
    require(presenter->has_error_message());
    require(presenter->message().find("Cue.Build.Plan") != std::string_view::npos);
    require(presenter->set_configuration(cue::BuildConfiguration::Development));
    require(presenter->submit(cue::editor::EditorBuildCommand::Start));
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Running);
    require(!presenter->can_start());
    require(!presenter->set_configuration(cue::BuildConfiguration::Release));
    require(!presenter->begin_editor_shutdown());
    require(presenter->is_shutdown_confirmation_pending());
    require(!presenter->respond_to_editor_shutdown(cue::editor::EditorBuildShutdownDecision::KeepEditorOpen));
    require(!presenter->is_shutdown_confirmation_pending());
    require(presenter->submit(cue::editor::EditorBuildCommand::Cancel));
    require(service->wait_for_completion().has_value());
    presenter->refresh();
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Cancelled);
    require(presenter->message().find("キャンセル") != std::string_view::npos);

    runnerState.mode.store(RunnerMode::Succeed, std::memory_order_release);
    require(presenter->submit(cue::editor::EditorBuildCommand::Retry));
    require(service->wait_for_completion().has_value());
    presenter->refresh();
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Succeeded);
    require(presenter->current_snapshot().profile->configuration() == cue::BuildConfiguration::Development);
    require(presenter->current_snapshot().artifact.has_value());
    require(presenter->displayed_log_count() == 2U);

    const std::string successfulOperationId = presenter->current_snapshot().operationId;
    runnerState.mode.store(RunnerMode::Fail, std::memory_order_release);
    require(presenter->submit(cue::editor::EditorBuildCommand::Start));
    require(service->wait_for_completion().has_value());
    presenter->refresh();
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Failed);
    require(presenter->current_snapshot().latestSuccessfulArtifact.has_value());
    require(presenter->saved_operations().size() == 2U);
    require(presenter->select_operation(successfulOperationId));
    require(presenter->displayed_snapshot().state == cue::GameBuildOperationState::Succeeded);
    require(presenter->displayed_log_count() == 2U);
    for (const cue::BuildLogSnapshot &log : presenter->displayed_snapshot().logs)
    {
        require(log.operationId == successfulOperationId);
    }

    runnerState.mode.store(RunnerMode::BlockUntilCancelled, std::memory_order_release);
    require(presenter->submit(cue::editor::EditorBuildCommand::Start));
    require(!presenter->begin_editor_shutdown());
    draw_frame(*presenter);
    require(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));
    runnerState.mode.store(RunnerMode::Succeed, std::memory_order_release);
    require(service->wait_for_completion().has_value());
    require(presenter->respond_to_editor_shutdown(cue::editor::EditorBuildShutdownDecision::CancelBuildAndClose));
    require(!presenter->take_shutdown_ready());
    require(presenter->begin_editor_shutdown());
    require(!presenter->is_shutdown_confirmation_pending());
    draw_frame(*presenter);
    require(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));
    require(!presenter->take_shutdown_ready());

    runnerState.mode.store(RunnerMode::BlockUntilCancelled, std::memory_order_release);
    press_build_shortcut(*presenter);
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Running);
    require(!presenter->begin_editor_shutdown());
    const bool firstImmediatelyReady =
        presenter->respond_to_editor_shutdown(cue::editor::EditorBuildShutdownDecision::CancelBuildAndClose);
    if (!firstImmediatelyReady)
    {
        require(service->wait_for_completion().has_value());
        presenter->refresh();
    }
    draw_frame(*presenter);

    runnerState.mode.store(RunnerMode::BlockUntilCancelled, std::memory_order_release);
    press_build_shortcut(*presenter);
    require(presenter->current_snapshot().state == cue::GameBuildOperationState::Running);
    require(!presenter->begin_editor_shutdown());
    require(!presenter->respond_to_editor_shutdown(cue::editor::EditorBuildShutdownDecision::KeepEditorOpen));
    require(!presenter->take_shutdown_ready());
    require(!presenter->begin_editor_shutdown());
    const bool immediatelyReady =
        presenter->respond_to_editor_shutdown(cue::editor::EditorBuildShutdownDecision::CancelBuildAndClose);
    if (!immediatelyReady)
    {
        require(service->wait_for_completion().has_value());
        presenter->refresh();
        require(presenter->take_shutdown_ready());
    }
    require(!presenter->take_shutdown_ready());
    presenter.reset();
}

/// @brief Package Retry入力とPresenter固有診断のFrame間保持を検証する
void test_package_retry_and_diagnostic(const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    const std::filesystem::path projectRoot = directory.path() / "Project";
    const std::filesystem::path engineRoot = directory.path() / "Engine";
    std::filesystem::create_directories(engineRoot);

    auto parentFilesystem = cue::create_windows_filesystem_root(directory.path().generic_string(), a_assertContext);
    auto projectId = cue::ProjectId::parse(k_packageProjectId, a_assertContext);
    require(parentFilesystem.has_value() && projectId.has_value());
    auto generated = cue::generate_blank_project(
        **parentFilesystem.try_value(), "Project", "Package Presenter Test", *projectId.try_value(), k_packageSceneId,
        {cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}}}, a_assertContext);
    require(generated.has_value());

    RunnerState runnerState;
    runnerState.mode.store(RunnerMode::Fail, std::memory_order_release);
    auto build = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(runnerState),
                                               std::make_unique<TestPublisher>(a_assertContext), a_assertContext);
    auto projectFilesystem = cue::create_windows_filesystem_root(projectRoot.generic_string(), a_assertContext);
    auto engineFilesystem = cue::create_windows_filesystem_root(engineRoot.generic_string(), a_assertContext);
    require(build.has_value() && projectFilesystem.has_value() && engineFilesystem.has_value());
    RunnerState runState;
    auto workflow = cue::package::GamePackageWorkflowService::create(
        std::move(*build.try_value()), std::make_unique<TestArtifactReader>(),
        std::move(*projectFilesystem.try_value()), std::move(*engineFilesystem.try_value()),
        std::make_unique<ControlledRunner>(runState), projectRoot.generic_string(), {},
        cue::package::RuntimeHostBuildSource::EngineBinaryRoot, a_assertContext);
    require(workflow.has_value());
    std::unique_ptr<cue::package::GamePackageWorkflowService> service = std::move(*workflow.try_value());

    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    require(profile.has_value());
    cue::BuildRequest request{projectRoot.generic_string(), std::move(*profile.try_value()),
                              "61234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    require(service
                ->start(std::move(request), cue::CMakeConfigureMode::Required, {1U, 0U, 0U},
                        std::string(k_packageProjectId), make_runtime_data(a_assertContext))
                .has_value());
    require(service->wait_for_package().has_value());
    require(service->snapshot().state == cue::package::PackageWorkflowState::Failed);

    auto sourceAssets =
        cue::create_windows_filesystem_root((projectRoot / "Assets" / "Source").generic_string(), a_assertContext);
    auto savedRoot = cue::create_windows_filesystem_root((projectRoot / "Saved").generic_string(), a_assertContext);
    auto descriptorRoot = cue::create_windows_filesystem_root(projectRoot.generic_string(), a_assertContext);
    cue::schema::SchemaRegistryIdentitySource schemaIdentitySource;
    cue::schema::SchemaRegistryBuilder schemaBuilder(schemaIdentitySource, a_assertContext);
    auto schemaRegistry = schemaBuilder.seal();
    require(sourceAssets.has_value() && savedRoot.has_value() && descriptorRoot.has_value() &&
            schemaRegistry.has_value());
    auto valueRegistry =
        cue::scene::ComponentValueSchemaRegistry::create({}, **schemaRegistry.try_value(), a_assertContext);
    require(valueRegistry.has_value());
    cue::scene::SceneMigrationRegistry sceneMigrations;
    cue::scene::ComponentMigrationRegistry componentMigrations;
    cue::editor_core::ScenePersistenceServices persistence(
        **descriptorRoot.try_value(), **sourceAssets.try_value(), **savedRoot.try_value(), **schemaRegistry.try_value(),
        *valueRegistry.try_value(), sceneMigrations, componentMigrations);
    std::unique_ptr<cue::editor_core::EditorController> controller =
        cue::editor_core::EditorController::create(std::move(*generated.try_value()), persistence, a_assertContext);
    auto operationIds = std::make_unique<TestOperationIdSource>(
        std::vector<std::string>{"71234567-89ab-4cde-8f01-23456789abcd", "81234567-89ab-4cde-8f01-23456789abcd",
                                 "91234567-89ab-4cde-8f01-23456789abcd", "a1234567-89ab-4cde-8f01-23456789abcd",
                                 "b1234567-89ab-4cde-8f01-23456789abcd"});
    std::unique_ptr<cue::editor::PackagePresenter> presenter =
        cue::editor::PackagePresenter::create(*service, *controller, projectRoot.generic_string(),
                                              k_workspaceCompatibility, std::move(operationIds), a_assertContext);

    auto reloadedProjectId = cue::ProjectId::parse(k_reloadedPackageProjectId, a_assertContext);
    require(reloadedProjectId.has_value());
    auto reloadedDescriptor = cue::create_blank_project_descriptor(
        std::move(*reloadedProjectId.try_value()), "Reloaded Package Presenter Test",
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}}, k_packageSceneId,
        a_assertContext);
    require(reloadedDescriptor.has_value());
    require(cue::save_project_descriptor(**descriptorRoot.try_value(), *reloadedDescriptor.try_value(), a_assertContext)
                .has_value());

    require(presenter->can_retry());
    require(!presenter->submit(cue::editor::EditorPackageCommand::Retry));
    require(presenter->has_error_message());
    require(presenter->message().find("Cue.EditorCore/15") != std::string_view::npos);
    require(controller->session().project_descriptor().project_id().text() == k_packageProjectId);
    require(controller->session().project_descriptor().engine_compatibility() ==
            cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}});

    auto compatibilityProjectId = cue::ProjectId::parse(k_packageProjectId, a_assertContext);
    require(compatibilityProjectId.has_value());
    auto compatibilityChangedDescriptor = cue::create_blank_project_descriptor(
        std::move(*compatibilityProjectId.try_value()), "Reloaded Package Presenter Test",
        cue::EngineCompatibility{cue::EngineVersion{1U, 1U, 0U}, cue::EngineVersion{2U, 1U, 0U}}, k_packageSceneId,
        a_assertContext);
    require(compatibilityChangedDescriptor.has_value());
    require(cue::save_project_descriptor(**descriptorRoot.try_value(), *compatibilityChangedDescriptor.try_value(),
                                         a_assertContext)
                .has_value());

    require(!presenter->submit(cue::editor::EditorPackageCommand::Retry));
    require(presenter->has_error_message());
    require(presenter->message().find("Cue.EditorCore/15") != std::string_view::npos);
    require(controller->session().project_descriptor().project_id().text() == k_packageProjectId);
    require(controller->session().project_descriptor().engine_compatibility() ==
            cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}});

    auto restoredProjectId = cue::ProjectId::parse(k_packageProjectId, a_assertContext);
    require(restoredProjectId.has_value());
    auto restoredDescriptor = cue::create_blank_project_descriptor(
        std::move(*restoredProjectId.try_value()), "Reloaded Package Presenter Test",
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}}, k_packageSceneId,
        a_assertContext);
    require(restoredDescriptor.has_value());
    require(cue::save_project_descriptor(**descriptorRoot.try_value(), *restoredDescriptor.try_value(), a_assertContext)
                .has_value());

    require(presenter->submit(cue::editor::EditorPackageCommand::Retry));
    require(service->wait_for_package().has_value());
    presenter->refresh();
    require(presenter->current_snapshot().state == cue::package::PackageWorkflowState::Failed);
    require(presenter->current_snapshot().build.operationId == "91234567-89ab-4cde-8f01-23456789abcd");

    require(presenter->submit(cue::editor::EditorPackageCommand::StartShipping));
    require(presenter->message().find("Monolithic Shipping Product") != std::string_view::npos);
    require(service->wait_for_package().has_value());
    presenter->refresh();
    require(presenter->current_snapshot().state == cue::package::PackageWorkflowState::Failed);
    require(presenter->message().starts_with("Buildに失敗したためPackageは開始していません") ||
            presenter->message().starts_with("Packageの検証または公開に失敗しました"));
    require(presenter->current_snapshot().build.operationId == "a1234567-89ab-4cde-8f01-23456789abcd");
    require(presenter->current_snapshot().build.profile.has_value());
    require(presenter->current_snapshot().build.profile->target() == cue::BuildTarget::ShippingProduct);
    require(presenter->current_snapshot().build.profile->configuration() == cue::BuildConfiguration::Release);
    require(presenter->current_snapshot().build.profile->minimum_trust_mode() == cue::ShippingTrustMode::UnsignedLocal);

    auto locator = cue::RelativePath::parse("Scenes/Default.cuescene", a_assertContext);
    require(locator.has_value());
    auto documentId = controller->open_document_from_storage(std::move(*locator.try_value()));
    require(documentId.has_value());
    TestSceneIdentitySource identitySource;
    require(controller
                ->execute_intent(*documentId.try_value(), cue::editor_core::AddObjectIntent{std::nullopt, "Dirty"},
                                 identitySource, {})
                .has_value());
    require(!presenter->submit(cue::editor::EditorPackageCommand::Retry));
    const std::string localDiagnostic(presenter->message());
    require(presenter->has_error_message());
    presenter->refresh();
    require(presenter->has_error_message());
    require(presenter->message() == localDiagnostic);
}
} // namespace

/// @brief Game Build ImGuiの操作、履歴、Artifact、終了時Cancel契約を検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    require(ImGui::CreateContext() != nullptr);
    ImGuiIO &input = ImGui::GetIO();
    input.IniFilename = nullptr;
    input.DisplaySize = ImVec2(1280.0F, 720.0F);
    input.DeltaTime = 1.0F / 60.0F;
    static_cast<void>(input.Fonts->Build());

    test_build_workflow(assertContext);
    test_package_retry_and_diagnostic(assertContext);
    ImGui::DestroyContext();
    return 0;
}
