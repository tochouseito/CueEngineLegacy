#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Project/Compatibility.h>
#include <Cue/ProjectHub/ImGui/ProjectHubPresenter.h>
#include <Cue/ProjectHub/Windows/WindowsProjectHubPlatform.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <Windows.h>
#include <imgui.h>

static_assert(!std::is_copy_constructible_v<cue::project_hub::ProjectHubPresenter>);
static_assert(!std::is_move_constructible_v<cue::project_hub::ProjectHubPresenter>);

namespace
{
/// @brief Test中の回復不能状態を固定Exit Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(75);
    }

    /// @brief Message付きFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

/// @brief Project Hub Presenter Test専用の一時Workspaceを所有する
class TestDirectory final
{
  public:
    /// @brief ProcessとTick値から一意な一時Directoryを作成する
    TestDirectory()
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0 || length >= temporary.size())
        {
            return;
        }
        m_path = temporary.data();
        m_path += L"CueProjectHubImGuiTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64());
        m_isCreated = CreateDirectoryW(m_path.c_str(), nullptr) != FALSE;
    }

    /// @brief 一時Directory所有権の複製を禁止する
    TestDirectory(const TestDirectory &) = delete;
    /// @brief 一時Directory所有権の複製を禁止する
    TestDirectory &operator=(const TestDirectory &) = delete;

    /// @brief Testが作成した空の一時Directoryだけを削除する
    ~TestDirectory()
    {
        if (m_isCreated)
        {
            DeleteFileW((m_path + L"\\RecentProjects.json").c_str());
            DeleteFileW((m_path + L"\\RecentProjects.json.tmp").c_str());
            RemoveDirectoryW(m_path.c_str());
        }
    }

    /// @brief 一時Directoryの作成結果を返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_isCreated;
    }

    /// @brief 一時DirectoryのUTF-16 Locatorを返す
    [[nodiscard]] std::wstring_view path() const noexcept
    {
        return m_path;
    }

  private:
    std::wstring m_path;
    bool m_isCreated = false;
};

/// @brief Test用Loggerを追加Sinkなしで生成する
[[nodiscard]] std::unique_ptr<cue::Logger> create_logger(TestFatalHandler &a_handler)
{
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    return std::make_unique<cue::Logger>(a_handler, std::move(sinks));
}

/// @brief UTF-16 Test LocatorをProject Hub入力用UTF-8へ変換する
[[nodiscard]] std::string to_utf8(std::wstring_view a_text, cue::AssertContext &a_context)
{
    std::string converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_windows_utf16_to_utf8(a_text, converted, a_context.fatal_handler());
    return result.status == cue::WindowsUtfConversionStatus::Success ? std::move(converted) : std::string();
}

/// @brief Presenter Test用の最小Project Hub設定を生成する
[[nodiscard]] cue::Result<cue::project_hub::ProjectHubConfiguration> make_configuration(
    const cue::AssertContext &a_context) noexcept
{
    cue::Result<cue::ProjectCapabilityProfile> profile = cue::ProjectCapabilityProfile::create({}, a_context);
    cue::Result<cue::ProjectCapabilitySnapshot> snapshot = cue::ProjectCapabilitySnapshot::create({}, a_context);
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
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}}};
    return cue::Result<cue::project_hub::ProjectHubConfiguration>::success(std::move(configuration));
}

/// @brief 注入済みInput Eventで一つのHeadless ImGui Frameを実行する
void draw_frame(cue::project_hub::ProjectHubPresenter &a_presenter) noexcept
{
    ImGui::NewFrame();
    a_presenter.draw();
    ImGui::Render();
}

/// @brief EscapeがModalを先にCancelし、次のEscapeだけがTool終了になることを検証する
[[nodiscard]] int test_keyboard_and_cancel(cue::AssertContext &a_context)
{
    TestDirectory directory;
    const std::string locator = to_utf8(directory.path(), a_context);
    cue::Result<std::unique_ptr<cue::FilesystemRoot>> workspace =
        cue::create_windows_filesystem_root(locator, a_context);
    cue::Result<std::unique_ptr<cue::project_hub::ProjectHubPlatform>> platform =
        cue::project_hub::create_windows_project_hub_platform(a_context);
    cue::Result<cue::project_hub::ProjectHubConfiguration> configuration = make_configuration(a_context);
    if (!directory.is_created())
    {
        return 10;
    }
    if (locator.empty())
    {
        return 11;
    }
    if (!workspace)
    {
        return 12;
    }
    if (!platform)
    {
        return 13;
    }
    if (!configuration)
    {
        return 14;
    }
    cue::Result<std::unique_ptr<cue::project_hub::ProjectHubService>> service =
        cue::project_hub::ProjectHubService::create(**workspace.try_value(), **platform.try_value(),
                                                    std::move(*configuration.try_value()), a_context);
    if (!service)
    {
        return static_cast<int>(20 + service.try_error()->root_code().value());
    }
    cue::Result<std::unique_ptr<cue::project_hub::ProjectHubPresenter>> presenter =
        cue::project_hub::ProjectHubPresenter::create(**service.try_value(), a_context);
    if (!presenter || ImGui::CreateContext() == nullptr)
    {
        return 3;
    }
    presenter.try_value()->get()->apply_engine_bundle_selection("C:/LocalBundle");
    std::optional<cue::project_hub::InstalledEngineOperationRequest> installRequest =
        presenter.try_value()->get()->take_installed_engine_operation_request();
    if (!installRequest ||
        installRequest->kind != cue::project_hub::InstalledEngineOperationKind::InstallUnsignedLocalBundle ||
        installRequest->target != "C:/LocalBundle" ||
        presenter.try_value()->get()->take_installed_engine_operation_request())
    {
        ImGui::DestroyContext();
        return 15;
    }

    ImGuiIO &input = ImGui::GetIO();
    input.IniFilename = nullptr;
    input.DisplaySize = ImVec2(1280.0F, 720.0F);
    input.DeltaTime = 1.0F / 60.0F;
    static_cast<void>(input.Fonts->Build());
    draw_frame(**presenter.try_value());

    input.AddKeyEvent(ImGuiMod_Ctrl, true);
    input.AddKeyEvent(ImGuiKey_N, true);
    draw_frame(**presenter.try_value());
    const bool didOpenModal = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    input.AddKeyEvent(ImGuiKey_N, false);
    input.AddKeyEvent(ImGuiMod_Ctrl, false);
    draw_frame(**presenter.try_value());

    input.AddKeyEvent(ImGuiMod_Ctrl, true);
    input.AddKeyEvent(ImGuiKey_O, true);
    draw_frame(**presenter.try_value());
    input.AddKeyEvent(ImGuiKey_O, false);
    input.AddKeyEvent(ImGuiMod_Ctrl, false);
    draw_frame(**presenter.try_value());

    input.AddKeyEvent(ImGuiKey_Escape, true);
    draw_frame(**presenter.try_value());
    const bool modalAbsorbedEscape = !presenter.try_value()->get()->is_exit_requested();
    const bool modalClosedWithoutQueuedShortcut = !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    input.AddKeyEvent(ImGuiKey_Escape, false);
    draw_frame(**presenter.try_value());
    input.AddKeyEvent(ImGuiKey_Escape, true);
    draw_frame(**presenter.try_value());
    const bool toolAcceptedEscape = presenter.try_value()->get()->is_exit_requested();

    ImGui::DestroyContext();
    if (!didOpenModal)
    {
        return 6;
    }
    if (!modalAbsorbedEscape)
    {
        return 4;
    }
    if (!modalClosedWithoutQueuedShortcut)
    {
        return 7;
    }
    return toolAcceptedEscape ? 0 : 5;
}
} // namespace

/// @brief Project Hub ImGui Keyboard操作とCancel優先順位をProcess単位で検証する
int main()
{
    TestFatalHandler handler;
    std::unique_ptr<cue::Logger> logger = create_logger(handler);
    cue::AssertContext context(*logger, handler);
    return test_keyboard_and_cancel(context);
}
