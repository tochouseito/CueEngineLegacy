#pragma once

#include <Cue/ProjectHub/Service.h>

#include <memory>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::distribution
{
class WindowsInstalledVersionExecutionLease;
}

namespace cue::project_hub
{
/// @brief 起動済みEditor Processの終了状態をNative Handleなしで監視する境界
class WindowsEditorProcess
{
  public:
    WindowsEditorProcess(const WindowsEditorProcess &) = delete;
    WindowsEditorProcess &operator=(const WindowsEditorProcess &) = delete;

    /// @brief Process監視に使用するWindows Resourceを解放する
    virtual ~WindowsEditorProcess() noexcept = default;

    /// @brief 実行中ならtrue、正常終了済みならfalse、監視失敗または異常終了ならErrorを返す
    [[nodiscard]] virtual Result<bool> poll() noexcept = 0;

  protected:
    WindowsEditorProcess() noexcept = default;
};

/// @brief Windows File APIとUUID生成をProject Hub Platform境界へ接続する
[[nodiscard]] Result<std::unique_ptr<ProjectHubPlatform>> create_windows_project_hub_platform(
    const AssertContext &a_assertContext) noexcept;

/// @brief Editor Launch Requestを独立Windows Processへ変換し、監視可能な一意所有者を返す
[[nodiscard]] Result<std::unique_ptr<WindowsEditorProcess>> launch_windows_editor_process(
    std::string_view a_editorExecutableLocator, const EditorLaunchRequest &a_request,
    const AssertContext &a_assertContext,
    distribution::WindowsInstalledVersionExecutionLease *a_executionLease = nullptr) noexcept;
} // namespace cue::project_hub
