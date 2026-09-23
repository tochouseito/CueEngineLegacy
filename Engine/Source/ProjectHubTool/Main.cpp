#include "Resources/CueProjectHubToolResource.h"

#include <Cue/Distribution/Windows/Installer.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Foundation/NumberParsing.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Platform/FileDialog.h>
#include <Cue/Platform/Windows/WindowsFileDialog.h>
#include <Cue/Project/Compatibility.h>
#include <Cue/ProjectHub/Error.h>
#include <Cue/ProjectHub/ImGui/ProjectHubPresenter.h>
#include <Cue/ProjectHub/Windows/WindowsProjectHubPlatform.h>
#include <Cue/ToolHost/WindowsD3D12/ToolHost.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <Windows.h>
#include <imgui.h>

namespace
{
constexpr int k_initializationFailure = 1;
constexpr int k_toolHostFailure = 2;

/// @brief Project Hub実行Fileの配置からDeveloper／Installed起動境界を保持する
struct ProjectHubExecutableEnvironment final
{
    std::string developerEditorExecutable;
    std::optional<std::string> installRoot;
};

[[nodiscard]] cue::Result<std::vector<cue::project_hub::InstalledEngineVersionView>> make_installed_engine_views(
    std::string_view a_installRoot, const cue::AssertContext &a_assertContext) noexcept;

/// @brief Canonical major.minor.patch文字列をProject互換性Versionへ変換する
[[nodiscard]] std::optional<cue::EngineVersion> parse_engine_version(std::string_view a_value) noexcept
{
    const std::size_t firstSeparator = a_value.find('.');
    const std::size_t secondSeparator =
        firstSeparator == std::string_view::npos ? firstSeparator : a_value.find('.', firstSeparator + 1U);
    if (firstSeparator == std::string_view::npos || secondSeparator == std::string_view::npos ||
        a_value.find('.', secondSeparator + 1U) != std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto major = cue::parse_unsigned_decimal<std::uint32_t>(a_value.substr(0U, firstSeparator));
    const auto minor = cue::parse_unsigned_decimal<std::uint32_t>(
        a_value.substr(firstSeparator + 1U, secondSeparator - firstSeparator - 1U));
    const auto patch = cue::parse_unsigned_decimal<std::uint32_t>(a_value.substr(secondSeparator + 1U));
    if (!major || !minor || !patch)
    {
        return std::nullopt;
    }
    return cue::EngineVersion{*major, *minor, *patch};
}

/// @brief Project Hub初期化ErrorをTool Host上でUserへ通知する
class InitializationFailureClient final : public cue::tool_host::ToolHostClient
{
  public:
    /// @brief UIで表示する初期化ErrorをClientが一意所有する
    explicit InitializationFailureClient(cue::Error &&a_error) noexcept : m_error(std::move(a_error))
    {
    }

    /// @brief Move-only Errorの一意所有を保つためCopy構築を禁止する
    InitializationFailureClient(const InitializationFailureClient &) = delete;
    /// @brief Move-only Errorの一意所有を保つためCopy代入を禁止する
    InitializationFailureClient &operator=(const InitializationFailureClient &) = delete;
    /// @brief 所有Errorを規定の順序で破棄する
    ~InitializationFailureClient() override = default;

    /// @brief Recoverableな初期化失敗と回復操作をProject Hub Windowへ描画する
    void draw_frame() noexcept override
    {
        ImGui::SetNextWindowSize(ImVec2(760.0F, 320.0F), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Project Hub Initialization Error"))
        {
            ImGui::TextUnformatted("Project Hubを初期化できませんでした。");
            ImGui::Spacing();
            const std::string_view summary = m_error.summary();
            ImGui::TextUnformatted(summary.data(), summary.data() + summary.size());
            const std::string_view domain = m_error.code().domain();
            ImGui::Text("Domain: %.*s", static_cast<int>(domain.size()), domain.data());
            ImGui::Text("Code: %lld", static_cast<long long>(m_error.code().value()));
            ImGui::Spacing();
            ImGui::TextWrapped(
                "WorkspaceのPath、アクセス権、同名のFileまたはReparse Pointを確認してから再起動してください。");
            ImGui::Spacing();
            if (ImGui::Button("終了") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_shouldClose = true;
            }
        }
        ImGui::End();
    }

    /// @brief Native Window終了要求を初期化失敗UIの終了状態へ反映する
    void request_close() noexcept override
    {
        m_shouldClose = true;
    }

    /// @brief Userが終了操作を選択したか返す
    [[nodiscard]] bool should_close() const noexcept override
    {
        return m_shouldClose;
    }

    /// @brief UI Session終了後に診断Logへ渡す初期化Errorを返す
    [[nodiscard]] cue::Error take_error() noexcept
    {
        return std::move(m_error);
    }

  private:
    cue::Error m_error;
    bool m_shouldClose = false;
};

/// @brief Project Hub本体を構築できないErrorを最小Tool Hostで表示して終了Codeへ変換する
[[nodiscard]] int show_initialization_failure(cue::Error &&a_error, cue::Logger &a_logger,
                                              const cue::AssertContext &a_assertContext) noexcept
{
    InitializationFailureClient client(std::move(a_error));
    const cue::tool_host::ToolHostDescriptor descriptor{
        "CueEngine Project Hub", {960U, 480U}, 0U, static_cast<std::uint16_t>(IDI_CUE_PROJECT_HUB_TOOL)};
    cue::Result<void> hosted = cue::tool_host::run_windows_d3d12_tool_host(descriptor, client, a_assertContext);
    cue::Error initialization = client.take_error();
    if (!hosted)
    {
        hosted.try_error()->append_secondary_diagnostics(a_assertContext, initialization,
                                                         "Project Hub initialization also failed", "Initialization");
        static_cast<void>(
            a_logger.log(cue::LogLevel::Error, "Project Hub initialization UI failed", std::move(*hosted.try_error())));
        return k_toolHostFailure;
    }
    static_cast<void>(
        a_logger.log(cue::LogLevel::Error, "Project Hub initialization failed", std::move(initialization)));
    return k_initializationFailure;
}

/// @brief Folder Dialog の選択結果を反映する Project Hub 入力欄を区別する
enum class FolderSelectionTarget
{
    CreationDestination,
    ExistingProject,
    EngineBundle
};

/// @brief Workerで完了したInstalled Engine操作をUI Threadへ一度だけ引き渡す
struct InstalledEngineOperationCompletion final
{
    cue::project_hub::InstalledEngineOperationKind kind =
        cue::project_hub::InstalledEngineOperationKind::RollbackVersion;
    std::optional<cue::Error> failure;
    std::optional<std::vector<cue::project_hub::InstalledEngineVersionView>> versions;
    std::optional<cue::Error> inspectionFailure;
};

/// @brief Project Hub PresenterをTool Host CallbackとEditor Process Adapterへ接続する
class ProjectHubToolClient final : public cue::tool_host::ToolHostClient
{
  public:
    /// @brief PresenterとDeveloper／Installed実行環境をClient全寿命へ関連付ける
    ProjectHubToolClient(cue::project_hub::ProjectHubPresenter &a_presenter,
                         cue::project_hub::ProjectHubService &a_service,
                         ProjectHubExecutableEnvironment &&a_environment,
                         const cue::AssertContext &a_assertContext) noexcept
        : m_presenter(&a_presenter), m_service(&a_service), m_assertContext(&a_assertContext),
          m_editorExecutableLocator(std::move(a_environment.developerEditorExecutable)),
          m_installRoot(std::move(a_environment.installRoot)),
          m_fileDialogService(cue::create_windows_file_dialog_service(a_assertContext))
    {
    }

    /// @brief Presentation Callback Stateの複製を禁止する
    ProjectHubToolClient(const ProjectHubToolClient &) = delete;
    /// @brief Presentation Callback Stateの複製を禁止する
    ProjectHubToolClient &operator=(const ProjectHubToolClient &) = delete;
    /// @brief 実行中WorkerをJoinしてからPresentationと診断Contextの非所有参照を破棄する
    ~ProjectHubToolClient() override = default;

    /// @brief Project Hub画面を描画し、生成されたLaunch RequestをWindows Adapterへ渡す
    void draw_frame() noexcept override
    {
        poll_installed_engine_operation();
        if (m_editorProcess != nullptr)
        {
            cue::Result<bool> processState = m_editorProcess->poll();
            if (!processState)
            {
                m_presenter->report_editor_launch_failure(*processState.try_error());
                m_editorProcess.reset();
            }
            else if (!*processState.try_value())
            {
                m_presenter->report_editor_process_completed();
                m_editorProcess.reset();
            }
        }

        m_presenter->draw(m_editorProcess == nullptr && !m_installedEngineOperationRunning, m_installRoot.has_value(),
                          m_installedEngineOperationRunning);
        const std::optional<std::string_view> browseRequest = m_presenter->take_destination_browse_request();
        if (browseRequest.has_value())
        {
            browse_folder(*browseRequest, FolderSelectionTarget::CreationDestination);
        }
        const std::optional<std::string_view> registrationBrowseRequest =
            m_presenter->take_registration_browse_request();
        if (registrationBrowseRequest.has_value())
        {
            browse_folder(*registrationBrowseRequest, FolderSelectionTarget::ExistingProject);
        }
        if (m_presenter->take_engine_bundle_browse_request())
        {
            browse_folder({}, FolderSelectionTarget::EngineBundle);
        }
        std::optional<cue::project_hub::InstalledEngineOperationRequest> engineOperation =
            m_presenter->take_installed_engine_operation_request();
        if (engineOperation)
        {
            start_installed_engine_operation(std::move(*engineOperation));
        }
        std::optional<cue::project_hub::EditorLaunchRequest> request = m_presenter->take_editor_launch_request();
        if (!request.has_value())
        {
            return;
        }
        cue::Result<std::unique_ptr<cue::project_hub::WindowsEditorProcess>> launched = launch_editor(*request);
        if (!launched)
        {
            m_presenter->report_editor_launch_failure(*launched.try_error());
            return;
        }
        m_editorProcess = std::move(*launched.try_value());
    }

    /// @brief Native File Dialog Ownerとして使用するWindowをHost実行中だけ保持する
    void window_ready(cue::Window &a_window) noexcept override
    {
        m_window = &a_window;
    }

    /// @brief Native Window終了要求をProject Hub Presenterへ渡す
    void request_close() noexcept override
    {
        m_closeRequested = true;
    }

    /// @brief EscapeまたはWindow終了だけをTool Host終了要求として返す
    [[nodiscard]] bool should_close() const noexcept override
    {
        return !m_installedEngineOperationRunning && (m_closeRequested || m_presenter->is_exit_requested());
    }

  private:
    /// @brief Installed Engine操作Kindに対応する完了Messageを返す
    [[nodiscard]] static std::string_view installed_engine_operation_success_message(
        cue::project_hub::InstalledEngineOperationKind a_kind) noexcept
    {
        switch (a_kind)
        {
        case cue::project_hub::InstalledEngineOperationKind::InstallUnsignedLocalBundle:
            return "Local Developer BundleをInstallしました。";
        case cue::project_hub::InstalledEngineOperationKind::RollbackVersion:
            return "Installed Engine Versionを選択しました。";
        case cue::project_hub::InstalledEngineOperationKind::UninstallVersion:
            return "Uninstallを外部Workerへ委譲しました。実行中VersionはHub終了後に削除されます。";
        }
        return "Installed Engine操作が完了しました。";
    }

    /// @brief Worker完了状態をUI Threadへ回収し、Registry再検査とMessage更新を行う
    void poll_installed_engine_operation() noexcept
    {
        std::optional<InstalledEngineOperationCompletion> completion;
        {
            std::lock_guard lock(m_installedEngineOperationMutex);
            if (!m_installedEngineOperationCompletion)
            {
                return;
            }
            completion.emplace(std::move(*m_installedEngineOperationCompletion));
            m_installedEngineOperationCompletion.reset();
        }
        if (m_installedEngineOperationWorker.joinable())
        {
            m_installedEngineOperationWorker.join();
        }
        m_installedEngineOperationRunning = false;
        bool refreshed = false;
        if (completion->versions)
        {
            cue::Result<void> replaced =
                m_service->replace_installed_engine_versions(std::move(*completion->versions));
            refreshed = replaced.has_value();
            if (!replaced && !completion->failure)
            {
                m_presenter->report_installed_engine_operation_failure(*replaced.try_error());
            }
        }
        else if (completion->inspectionFailure && !completion->failure)
        {
            m_presenter->report_installed_engine_operation_failure(*completion->inspectionFailure);
        }
        if (completion->failure)
        {
            m_presenter->report_installed_engine_operation_failure(*completion->failure);
            return;
        }
        if (!refreshed)
        {
            return;
        }
        m_presenter->report_installed_engine_operation_completed(
            installed_engine_operation_success_message(completion->kind));
    }

    /// @brief Presenterから受けたInstalled Engine操作を描画Thread外のWorkerで実行する
    void start_installed_engine_operation(cue::project_hub::InstalledEngineOperationRequest a_request) noexcept
    {
        if (m_installedEngineOperationRunning)
        {
            return;
        }
        if (!m_installRoot)
        {
            cue::Error error = cue::project_hub::make_project_hub_error(
                *m_assertContext, cue::project_hub::ProjectHubError::InvalidConfiguration,
                "Installed Engine management root is unavailable");
            m_presenter->report_installed_engine_operation_failure(error);
            return;
        }
        try
        {
            const std::string installRoot = *m_installRoot;
            m_installedEngineOperationRunning = true;
            m_presenter->report_installed_engine_operation_started();
            m_installedEngineOperationWorker = std::jthread(
                [this, request = std::move(a_request), installRoot]() mutable noexcept
                {
                    std::optional<cue::Error> failure;
                    if (request.kind == cue::project_hub::InstalledEngineOperationKind::InstallUnsignedLocalBundle)
                    {
                        cue::distribution::WindowsInstallRequest installRequest{request.target, installRoot, false,
                                                                                 true};
                        auto installed =
                            cue::distribution::install_windows_source_sdk(installRequest, *m_assertContext);
                        if (!installed)
                        {
                            failure.emplace(std::move(*installed.try_error()));
                        }
                    }
                    else
                    {
                        cue::distribution::WindowsInstalledVersionRequest versionRequest{installRoot, request.target};
                        if (request.kind == cue::project_hub::InstalledEngineOperationKind::RollbackVersion)
                        {
                            auto selected = cue::distribution::rollback_windows_installed_version(versionRequest,
                                                                                                    *m_assertContext);
                            if (!selected)
                            {
                                failure.emplace(std::move(*selected.try_error()));
                            }
                        }
                        else
                        {
                            auto uninstalled = cue::distribution::uninstall_windows_installed_version(
                                versionRequest, *m_assertContext);
                            if (!uninstalled)
                            {
                                failure.emplace(std::move(*uninstalled.try_error()));
                            }
                        }
                    }
                    std::optional<std::vector<cue::project_hub::InstalledEngineVersionView>> versions;
                    std::optional<cue::Error> inspectionFailure;
                    auto inspectedVersions = make_installed_engine_views(installRoot, *m_assertContext);
                    if (inspectedVersions)
                    {
                        versions.emplace(std::move(*inspectedVersions.try_value()));
                    }
                    else
                    {
                        inspectionFailure.emplace(std::move(*inspectedVersions.try_error()));
                    }
                    std::lock_guard lock(m_installedEngineOperationMutex);
                    m_installedEngineOperationCompletion.emplace(InstalledEngineOperationCompletion{
                        request.kind, std::move(failure), std::move(versions), std::move(inspectionFailure)});
                });
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate("Installed Engine operation worker could not be started");
        }
    }

    /// @brief Installed Versions Registryを再検査してServiceの互換Viewへ反映する
    [[nodiscard]] bool refresh_installed_engine_versions(bool a_reportFailure) noexcept
    {
        if (!m_installRoot)
        {
            return false;
        }
        auto versions = make_installed_engine_views(*m_installRoot, *m_assertContext);
        if (!versions)
        {
            if (a_reportFailure)
            {
                m_presenter->report_installed_engine_operation_failure(*versions.try_error());
            }
            return false;
        }
        cue::Result<void> replaced =
            m_service->replace_installed_engine_versions(std::move(*versions.try_value()));
        if (!replaced && a_reportFailure)
        {
            m_presenter->report_installed_engine_operation_failure(*replaced.try_error());
        }
        return replaced.has_value();
    }

    /// @brief Developer隣接Editorまたは再検証済みInstalled VersionからEditor Processを起動する
    [[nodiscard]] cue::Result<std::unique_ptr<cue::project_hub::WindowsEditorProcess>> launch_editor(
        const cue::project_hub::EditorLaunchRequest &a_request) noexcept
    {
        const std::optional<cue::project_hub::InstalledEngineLaunchIdentity> &installedIdentity =
            a_request.installed_engine_identity();
        if (!installedIdentity)
        {
            return cue::project_hub::launch_windows_editor_process(m_editorExecutableLocator, a_request,
                                                                    *m_assertContext);
        }
        if (!m_installRoot)
        {
            return cue::Result<std::unique_ptr<cue::project_hub::WindowsEditorProcess>>::failure(
                cue::project_hub::make_project_hub_error(*m_assertContext,
                                                         cue::project_hub::ProjectHubError::EditorLaunchFailed,
                                                         "Installed Engine launch root is unavailable"));
        }
        cue::distribution::WindowsInstalledVersionRequest versionRequest{
            *m_installRoot, installedIdentity->versionDirectory};
        auto selected = cue::distribution::rollback_windows_installed_version(versionRequest, *m_assertContext);
        if (!selected)
        {
            return cue::Result<std::unique_ptr<cue::project_hub::WindowsEditorProcess>>::failure(
                std::move(*selected.try_error()));
        }
        auto lease =
            cue::distribution::acquire_windows_installed_version_execution_lease(versionRequest, *m_assertContext);
        if (!lease)
        {
            return cue::Result<std::unique_ptr<cue::project_hub::WindowsEditorProcess>>::failure(
                std::move(*lease.try_error()));
        }
        if (lease.try_value()->bundle_id() != installedIdentity->bundleId ||
            lease.try_value()->manifest_digest() != installedIdentity->manifestDigest)
        {
            return cue::Result<std::unique_ptr<cue::project_hub::WindowsEditorProcess>>::failure(
                cue::project_hub::make_project_hub_error(*m_assertContext,
                                                         cue::project_hub::ProjectHubError::EditorLaunchFailed,
                                                         "Installed Engine identity changed after Hub inspection"));
        }
        return cue::project_hub::launch_windows_editor_process(lease.try_value()->editor_executable(), a_request,
                                                                *m_assertContext, lease.try_value());
    }

    /// @brief Project Folder 選択 Intent を既存 Windows Folder Dialog へ同期接続する
    void browse_folder(std::string_view a_initialLocation, FolderSelectionTarget a_target) noexcept
    {
        if (m_window == nullptr)
        {
            return;
        }
        cue::Result<cue::FileDialogOwnerToken> owner =
            cue::create_windows_file_dialog_owner(*m_window, *m_assertContext);
        if (!owner)
        {
            m_presenter->report_folder_browse_failure(*owner.try_error());
            return;
        }
        try
        {
            cue::FileDialogRequest request(cue::FileDialogKind::SelectFolder, {}, {}, std::string(a_initialLocation),
                                           std::move(*owner.try_value()));
            cue::Result<cue::FileDialogResult> selected = m_fileDialogService->show(request);
            if (!selected)
            {
                m_presenter->report_folder_browse_failure(*selected.try_error());
                return;
            }
            if (selected.try_value()->outcome() == cue::FileDialogOutcome::Selected)
            {
                const std::optional<std::string_view> selectedPath = selected.try_value()->selected_path();
                if (selectedPath.has_value())
                {
                    if (a_target == FolderSelectionTarget::ExistingProject)
                    {
                        m_presenter->apply_registration_selection(*selectedPath);
                    }
                    else if (a_target == FolderSelectionTarget::EngineBundle)
                    {
                        m_presenter->apply_engine_bundle_selection(*selectedPath);
                    }
                    else
                    {
                        m_presenter->apply_destination_selection(*selectedPath);
                    }
                }
            }
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate("Project Hub folder dialog request allocation failed");
        }
    }

    cue::project_hub::ProjectHubPresenter *m_presenter;
    cue::project_hub::ProjectHubService *m_service;
    const cue::AssertContext *m_assertContext;
    std::string m_editorExecutableLocator;
    std::optional<std::string> m_installRoot;
    std::unique_ptr<cue::FileDialogService> m_fileDialogService;
    std::unique_ptr<cue::project_hub::WindowsEditorProcess> m_editorProcess;
    cue::Window *m_window = nullptr;
    bool m_closeRequested = false;
    bool m_installedEngineOperationRunning = false;
    std::mutex m_installedEngineOperationMutex;
    std::optional<InstalledEngineOperationCompletion> m_installedEngineOperationCompletion;
    std::jthread m_installedEngineOperationWorker;
};

/// @brief 実行中Project Hubの配置をDeveloper隣接EditorまたはInstalled Version Rootとして検証する
[[nodiscard]] cue::Result<ProjectHubExecutableEnvironment> inspect_executable_environment(
    const cue::AssertContext &a_assertContext) noexcept
{
    std::wstring modulePath(32768, L'\0');
    const DWORD capacity = static_cast<DWORD>(modulePath.size());
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), capacity);
    if (length == 0 || length >= capacity)
    {
        const DWORD nativeCode = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.ProjectHubTool", 1);
        cue::NativeError native =
            cue::NativeError::create(a_assertContext.fatal_handler(), "Win32", static_cast<std::int64_t>(nativeCode));
        return cue::Result<ProjectHubExecutableEnvironment>::failure(cue::Error::create(
            a_assertContext.fatal_handler(), std::move(code), "Project Hub executable path could not be read",
            std::move(native)));
    }
    modulePath.resize(length);
    const std::filesystem::path executablePath(modulePath);
    const std::filesystem::path executableDirectory = executablePath.parent_path();
    if (executableDirectory.empty())
    {
        cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.ProjectHubTool", 2);
        return cue::Result<ProjectHubExecutableEnvironment>::failure(cue::Error::create(
            a_assertContext.fatal_handler(), std::move(code), "Project Hub executable directory is invalid"));
    }
    const std::filesystem::path versionRoot = executableDirectory.parent_path();
    const std::filesystem::path versionsRoot = versionRoot.parent_path();
    const bool isInstalledLayout = _wcsicmp(executableDirectory.filename().c_str(), L"Bin") == 0 &&
                                   _wcsicmp(versionsRoot.filename().c_str(), L"Versions") == 0 &&
                                   !versionsRoot.parent_path().empty();
    const std::filesystem::path selectedPath =
        isInstalledLayout ? versionsRoot.parent_path() : executableDirectory / L"CueEditorTool.exe";
    std::string convertedPath;
    const cue::WindowsUtfConversionResult conversion = cue::convert_windows_utf16_to_utf8(
        selectedPath.native(), convertedPath, a_assertContext.fatal_handler());
    if (conversion.status != cue::WindowsUtfConversionStatus::Success)
    {
        cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.ProjectHubTool", 3);
        cue::NativeError native =
            cue::NativeError::create(a_assertContext.fatal_handler(), "Win32", conversion.nativeCode);
        return cue::Result<ProjectHubExecutableEnvironment>::failure(cue::Error::create(
            a_assertContext.fatal_handler(), std::move(code), "Project Hub executable path conversion failed",
            std::move(native)));
    }
    ProjectHubExecutableEnvironment environment;
    if (isInstalledLayout)
    {
        environment.installRoot = std::move(convertedPath);
    }
    else
    {
        environment.developerEditorExecutable = std::move(convertedPath);
    }
    return cue::Result<ProjectHubExecutableEnvironment>::success(std::move(environment));
}

/// @brief Installed Versions Registry検査結果をUI非依存Project Hub Viewへ変換する
[[nodiscard]] cue::Result<std::vector<cue::project_hub::InstalledEngineVersionView>>
make_installed_engine_views(std::string_view a_installRoot, const cue::AssertContext &a_assertContext) noexcept
{
    auto inspected = cue::distribution::inspect_windows_installed_versions(a_installRoot, a_assertContext);
    if (!inspected)
    {
        return cue::Result<std::vector<cue::project_hub::InstalledEngineVersionView>>::failure(
            std::move(*inspected.try_error()));
    }
    try
    {
        std::vector<cue::project_hub::InstalledEngineVersionView> versions;
        versions.reserve(inspected.try_value()->versions.size());
        for (const cue::distribution::WindowsInstalledVersionInspection &version : inspected.try_value()->versions)
        {
            const std::optional<cue::EngineVersion> parsedVersion = parse_engine_version(version.engineVersion);
            if (!parsedVersion)
            {
                cue::ErrorCode code =
                    cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.ProjectHubTool", 5);
                return cue::Result<std::vector<cue::project_hub::InstalledEngineVersionView>>::failure(
                    cue::Error::create(a_assertContext.fatal_handler(), std::move(code),
                                       "Installed Engine Version is not canonical"));
            }
            std::string displayName = version.engineVersion;
            if (!version.isAvailable)
            {
                displayName.append(" (利用不可)");
            }
            versions.push_back(cue::project_hub::InstalledEngineVersionView{
                version.directoryName, std::move(displayName), *parsedVersion, version.bundleId,
                version.manifestDigest, version.diagnostic, version.isAvailable, version.isSelected});
        }
        return cue::Result<std::vector<cue::project_hub::InstalledEngineVersionView>>::success(std::move(versions));
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Installed Engine Version View allocation failed");
    }
    std::terminate();
}

/// @brief M12 Project Hubが生成・受理するProject VersionとCapability条件を構築する
[[nodiscard]] cue::Result<cue::project_hub::ProjectHubConfiguration> make_configuration(
    std::vector<cue::project_hub::InstalledEngineVersionView> a_installedEngineVersions,
    const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::ProjectCapabilityProfile> profile = cue::ProjectCapabilityProfile::create({}, a_assertContext);
    if (!profile)
    {
        return cue::Result<cue::project_hub::ProjectHubConfiguration>::failure(std::move(*profile.try_error()));
    }
    cue::Result<cue::ProjectCapabilitySnapshot> snapshot = cue::ProjectCapabilitySnapshot::create({}, a_assertContext);
    if (!snapshot)
    {
        return cue::Result<cue::project_hub::ProjectHubConfiguration>::failure(std::move(*snapshot.try_error()));
    }
    cue::EngineVersion currentVersion{1U, 0U, 0U};
    if (!a_installedEngineVersions.empty())
    {
        const auto selected = std::ranges::find_if(
            a_installedEngineVersions,
            [](const cue::project_hub::InstalledEngineVersionView &a_version) { return a_version.isSelected; });
        if (selected == a_installedEngineVersions.end())
        {
            cue::ErrorCode code =
                cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.ProjectHubTool", 4);
            return cue::Result<cue::project_hub::ProjectHubConfiguration>::failure(cue::Error::create(
                a_assertContext.fatal_handler(), std::move(code), "Installed Engine Registry has no selected Version"));
        }
        currentVersion = selected->engineVersion;
    }
    cue::project_hub::ProjectHubConfiguration configuration{
        cue::k_currentProjectDescriptorSchemaVersion,
        currentVersion,
        std::move(*profile.try_value()),
        std::move(*snapshot.try_value()),
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}},
        std::move(a_installedEngineVersions)};
    return cue::Result<cue::project_hub::ProjectHubConfiguration>::success(std::move(configuration));
}

/// @brief Project Hub ToolのDependency Graphを構築してUI Sessionを実行する
[[nodiscard]] int run(cue::Logger &a_logger, const cue::AssertContext &a_assertContext)
{
    cue::Result<cue::RelativePath> workspacePath = cue::RelativePath::parse("CueEngine/Workspace", a_assertContext);
    if (!workspacePath)
    {
        return show_initialization_failure(std::move(*workspacePath.try_error()), a_logger, a_assertContext);
    }
    cue::Result<std::unique_ptr<cue::FilesystemRoot>> workspace = cue::create_windows_known_folder_filesystem_root(
        cue::WindowsKnownFolder::LocalApplicationData, *workspacePath.try_value(),
        cue::WindowsRootOpenMode::CreateOrOpen, a_assertContext);
    if (!workspace)
    {
        return show_initialization_failure(std::move(*workspace.try_error()), a_logger, a_assertContext);
    }
    auto executableEnvironment = inspect_executable_environment(a_assertContext);
    if (!executableEnvironment)
    {
        return show_initialization_failure(std::move(*executableEnvironment.try_error()), a_logger, a_assertContext);
    }
    std::vector<cue::project_hub::InstalledEngineVersionView> installedEngineVersions;
    if (executableEnvironment.try_value()->installRoot)
    {
        auto inspected = make_installed_engine_views(*executableEnvironment.try_value()->installRoot, a_assertContext);
        if (!inspected)
        {
            return show_initialization_failure(std::move(*inspected.try_error()), a_logger, a_assertContext);
        }
        installedEngineVersions = std::move(*inspected.try_value());
    }
    cue::Result<std::unique_ptr<cue::project_hub::ProjectHubPlatform>> platform =
        cue::project_hub::create_windows_project_hub_platform(a_assertContext);
    cue::Result<cue::project_hub::ProjectHubConfiguration> configuration =
        make_configuration(std::move(installedEngineVersions), a_assertContext);
    if (!platform || !configuration)
    {
        cue::Error error = !platform ? std::move(*platform.try_error()) : std::move(*configuration.try_error());
        static_cast<void>(a_logger.log(cue::LogLevel::Error, "Project Hub configuration failed", std::move(error)));
        return k_initializationFailure;
    }
    cue::Result<std::unique_ptr<cue::project_hub::ProjectHubService>> service =
        cue::project_hub::ProjectHubService::create(**workspace.try_value(), **platform.try_value(),
                                                    std::move(*configuration.try_value()), a_assertContext);
    if (!service)
    {
        return show_initialization_failure(std::move(*service.try_error()), a_logger, a_assertContext);
    }
    cue::Result<std::unique_ptr<cue::project_hub::ProjectHubPresenter>> presenter =
        cue::project_hub::ProjectHubPresenter::create(**service.try_value(), a_assertContext);
    if (!presenter)
    {
        static_cast<void>(
            a_logger.log(cue::LogLevel::Error, "Project Hub presentation initialization failed",
                         std::move(*presenter.try_error())));
        return k_initializationFailure;
    }

    ProjectHubToolClient client(**presenter.try_value(), **service.try_value(),
                                std::move(*executableEnvironment.try_value()), a_assertContext);
    const cue::tool_host::ToolHostDescriptor descriptor{
        "CueEngine Project Hub", {1280U, 720U}, 0U, static_cast<std::uint16_t>(IDI_CUE_PROJECT_HUB_TOOL)};
    cue::Result<void> hosted = cue::tool_host::run_windows_d3d12_tool_host(descriptor, client, a_assertContext);
    if (!hosted)
    {
        static_cast<void>(
            a_logger.log(cue::LogLevel::Error, "Project Hub Tool Host failed", std::move(*hosted.try_error())));
        return k_toolHostFailure;
    }
    return 0;
}
} // namespace

/// @brief Project Hub Toolの診断寿命を最外側で所有してUI Sessionの終了Codeを返す
int wmain()
{
    cue::AbortFatalHandler fatalHandler;
    try
    {
        std::vector<std::unique_ptr<cue::LogSink>> sinks;
        sinks.push_back(std::make_unique<cue::ConsoleLogSink>());
        cue::Logger logger(fatalHandler, std::move(sinks));
        cue::AssertContext assertContext(logger, fatalHandler);
        return run(logger, assertContext);
    }
    catch (...)
    {
        fatalHandler.terminate("Project Hub Tool allocation failed");
    }
}
