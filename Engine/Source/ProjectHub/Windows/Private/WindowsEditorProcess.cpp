#include <Cue/ProjectHub/Windows/WindowsProjectHubPlatform.h>

#include <Cue/Distribution/Windows/Installer.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/IO/Error.h>
#include <Cue/ProjectHub/Error.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>

namespace
{
constexpr std::size_t k_maxCommandLineLength = 32767;

/// @brief Allocation失敗をEditor Process起動境界からFatal終端する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("Windows Editor process command construction failed");
    std::terminate();
}

/// @brief UTF-8引数をNULなしのStrict UTF-16へ変換する
[[nodiscard]] cue::Result<std::wstring> to_utf16(std::string_view a_text, const cue::AssertContext &a_context) noexcept
{
    std::wstring converted;
    const cue::WindowsUtfConversionResult result =
        cue::convert_utf8_to_windows_utf16(a_text, converted, a_context.fatal_handler());
    if (result.status != cue::WindowsUtfConversionStatus::Success || converted.find(L'\0') != std::wstring::npos)
    {
        return cue::Result<std::wstring>::failure(
            cue::project_hub::make_project_hub_error(a_context, cue::project_hub::ProjectHubError::EditorLaunchFailed,
                                                     "Editor process argument is not valid UTF-8"));
    }
    return cue::Result<std::wstring>::success(std::move(converted));
}

/// @brief Windows C Runtime規則で一つのCommand Line引数をQuoteする
void append_argument(std::wstring &a_commandLine, std::wstring_view a_argument)
{
    if (!a_commandLine.empty())
    {
        a_commandLine.push_back(L' ');
    }
    a_commandLine.push_back(L'"');
    std::size_t slashCount = 0;
    for (wchar_t character : a_argument)
    {
        if (character == L'\\')
        {
            ++slashCount;
            continue;
        }
        if (character == L'"')
        {
            a_commandLine.append(slashCount * 2 + 1, L'\\');
            a_commandLine.push_back(L'"');
            slashCount = 0;
            continue;
        }
        a_commandLine.append(slashCount, L'\\');
        slashCount = 0;
        a_commandLine.push_back(character);
    }
    a_commandLine.append(slashCount * 2, L'\\');
    a_commandLine.push_back(L'"');
}

/// @brief UTF-8値を変換してCommand Lineへ一引数として追加する
[[nodiscard]] cue::Result<void> append_utf8_argument(std::wstring &a_commandLine, std::string_view a_argument,
                                                     const cue::AssertContext &a_context) noexcept
{
    cue::Result<std::wstring> converted = to_utf16(a_argument, a_context);
    if (!converted)
    {
        return cue::Result<void>::failure(std::move(*converted.try_error()));
    }
    try
    {
        append_argument(a_commandLine, *converted.try_value());
    }
    catch (...)
    {
        terminate_allocation(a_context);
    }
    return cue::Result<void>::success();
}

/// @brief Editor Process起動のWin32失敗をProject Hub分類へ変換する
[[nodiscard]] cue::Error make_launch_error(const cue::AssertContext &a_context, DWORD a_nativeCode) noexcept
{
    cue::Error cause = cue::make_io_error(a_context, cue::IoError::IoFailure, "Editor process creation failed",
                                          static_cast<std::int64_t>(a_nativeCode));
    return cue::project_hub::reclassify_project_hub_error(a_context,
                                                          cue::project_hub::ProjectHubError::EditorLaunchFailed,
                                                          "Editor process could not be launched", std::move(cause));
}

/// @brief Editor Process監視のWin32失敗をProject Hub分類へ変換する
[[nodiscard]] cue::Error make_monitor_error(const cue::AssertContext &a_context, std::string_view a_summary,
                                            std::string_view a_nativeDomain, std::int64_t a_nativeCode) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_context.fatal_handler(), a_nativeDomain, a_nativeCode);
    cue::Error cause = cue::Error::create(a_context.fatal_handler(), std::move(code), a_summary);
    return cue::project_hub::reclassify_project_hub_error(a_context,
                                                          cue::project_hub::ProjectHubError::EditorProcessFailed,
                                                          "Editor process did not complete normally", std::move(cause));
}

/// @brief ProcessとPrimary Thread Handleを一意所有して非待機終了監視を提供する
class WindowsEditorProcessImpl final : public cue::project_hub::WindowsEditorProcess
{
  public:
    WindowsEditorProcessImpl(HANDLE a_process, HANDLE a_thread, const cue::AssertContext &a_context) noexcept
        : m_process(a_process), m_thread(a_thread), m_assertContext(&a_context)
    {
    }

    WindowsEditorProcessImpl(const WindowsEditorProcessImpl &) = delete;
    WindowsEditorProcessImpl &operator=(const WindowsEditorProcessImpl &) = delete;

    ~WindowsEditorProcessImpl() noexcept override
    {
        if (m_thread != nullptr)
        {
            CloseHandle(m_thread);
        }
        if (m_process != nullptr)
        {
            CloseHandle(m_process);
        }
    }

    [[nodiscard]] cue::Result<bool> poll() noexcept override
    {
        const DWORD waitResult = WaitForSingleObject(m_process, 0);
        if (waitResult == WAIT_TIMEOUT)
        {
            return cue::Result<bool>::success(true);
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            const DWORD nativeCode = waitResult == WAIT_FAILED ? GetLastError() : waitResult;
            return cue::Result<bool>::failure(
                make_monitor_error(*m_assertContext, "Editor process wait failed", "Win32", nativeCode));
        }

        DWORD exitCode = 0;
        if (!GetExitCodeProcess(m_process, &exitCode))
        {
            return cue::Result<bool>::failure(
                make_monitor_error(*m_assertContext, "Editor process exit code failed", "Win32", GetLastError()));
        }
        if (exitCode != 0)
        {
            return cue::Result<bool>::failure(make_monitor_error(*m_assertContext, "Editor process exited with failure",
                                                                 "Editor.ExitCode", exitCode));
        }
        return cue::Result<bool>::success(false);
    }

  private:
    HANDLE m_process;
    HANDLE m_thread;
    const cue::AssertContext *m_assertContext;
};
} // namespace

namespace cue::project_hub
{
Result<std::unique_ptr<WindowsEditorProcess>> launch_windows_editor_process(
    std::string_view a_editorExecutableLocator, const EditorLaunchRequest &a_request,
    const AssertContext &a_assertContext,
    distribution::WindowsInstalledVersionExecutionLease *a_executionLease) noexcept
{
    Result<std::wstring> executable = to_utf16(a_editorExecutableLocator, a_assertContext);
    if (!executable || executable.try_value()->empty())
    {
        return executable
                   ? Result<std::unique_ptr<WindowsEditorProcess>>::failure(make_project_hub_error(
                         a_assertContext, ProjectHubError::EditorLaunchFailed, "Editor executable locator is empty"))
                   : Result<std::unique_ptr<WindowsEditorProcess>>::failure(std::move(*executable.try_error()));
    }

    std::wstring commandLine;
    try
    {
        commandLine.reserve(1024);
        append_argument(commandLine, *executable.try_value());
        append_argument(commandLine, L"--protocol-version");
        append_argument(commandLine, std::to_wstring(a_request.protocol_version()));
        append_argument(commandLine, L"--project-descriptor");
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    Result<void> descriptor =
        append_utf8_argument(commandLine, a_request.project_descriptor_locator(), a_assertContext);
    if (!descriptor)
    {
        return Result<std::unique_ptr<WindowsEditorProcess>>::failure(std::move(*descriptor.try_error()));
    }
    try
    {
        append_argument(commandLine, L"--expected-project-id");
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    Result<void> projectId = append_utf8_argument(commandLine, a_request.expected_project_id(), a_assertContext);
    if (!projectId)
    {
        return Result<std::unique_ptr<WindowsEditorProcess>>::failure(std::move(*projectId.try_error()));
    }
    try
    {
        append_argument(commandLine, L"--engine-compatibility-id");
    }
    catch (...)
    {
        terminate_allocation(a_assertContext);
    }
    Result<void> compatibility =
        append_utf8_argument(commandLine, a_request.engine_compatibility_id(), a_assertContext);
    if (!compatibility)
    {
        return Result<std::unique_ptr<WindowsEditorProcess>>::failure(std::move(*compatibility.try_error()));
    }
    if (a_request.initial_scene_locator().has_value())
    {
        try
        {
            append_argument(commandLine, L"--initial-scene");
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
        Result<void> scene = append_utf8_argument(commandLine, *a_request.initial_scene_locator(), a_assertContext);
        if (!scene)
        {
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(std::move(*scene.try_error()));
        }
    }
    if (a_request.expected_initial_scene_asset_id().has_value())
    {
        try
        {
            append_argument(commandLine, L"--expected-initial-scene-asset-id");
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
        Result<void> sceneAssetId =
            append_utf8_argument(commandLine, *a_request.expected_initial_scene_asset_id(), a_assertContext);
        if (!sceneAssetId)
        {
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(std::move(*sceneAssetId.try_error()));
        }
    }
    if (a_executionLease != nullptr)
    {
        if (a_editorExecutableLocator != a_executionLease->editor_executable() ||
            a_executionLease->native_handle() == 0U)
        {
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(make_project_hub_error(
                a_assertContext, ProjectHubError::EditorLaunchFailed,
                "Installed Editor executable does not match its validated Execution Lease"));
        }
        try
        {
            append_argument(commandLine, L"--engine-install-root");
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
        Result<void> installRoot =
            append_utf8_argument(commandLine, a_executionLease->install_root(), a_assertContext);
        if (!installRoot)
        {
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(std::move(*installRoot.try_error()));
        }
        try
        {
            append_argument(commandLine, L"--engine-version-directory");
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
        Result<void> versionDirectory =
            append_utf8_argument(commandLine, a_executionLease->version_directory(), a_assertContext);
        if (!versionDirectory)
        {
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(std::move(*versionDirectory.try_error()));
        }
        try
        {
            append_argument(commandLine, L"--engine-bundle-id");
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
        Result<void> bundleId = append_utf8_argument(commandLine, a_executionLease->bundle_id(), a_assertContext);
        if (!bundleId)
        {
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(std::move(*bundleId.try_error()));
        }
        try
        {
            append_argument(commandLine, L"--engine-manifest-digest");
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
        Result<void> manifestDigest =
            append_utf8_argument(commandLine, a_executionLease->manifest_digest(), a_assertContext);
        if (!manifestDigest)
        {
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(std::move(*manifestDigest.try_error()));
        }
        try
        {
            append_argument(commandLine, L"--engine-execution-lease-handle");
            append_argument(commandLine, std::to_wstring(a_executionLease->native_handle()));
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
    }
    if (commandLine.size() >= k_maxCommandLineLength)
    {
        return Result<std::unique_ptr<WindowsEditorProcess>>::failure(
            make_project_hub_error(a_assertContext, ProjectHubError::EditorLaunchFailed,
                                   "Editor process command line exceeds the Windows limit"));
    }

    PROCESS_INFORMATION process{};
    BOOL created = FALSE;
    if (a_executionLease == nullptr)
    {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        created = CreateProcessW(executable.try_value()->c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                                 nullptr, nullptr, &startup, &process);
    }
    else
    {
        SIZE_T attributeBytes = 0U;
        static_cast<void>(InitializeProcThreadAttributeList(nullptr, 1U, 0U, &attributeBytes));
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || attributeBytes == 0U)
        {
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(
                make_launch_error(a_assertContext, GetLastError()));
        }
        std::vector<std::byte> attributeStorage;
        try
        {
            attributeStorage.resize(attributeBytes);
        }
        catch (...)
        {
            terminate_allocation(a_assertContext);
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1U, 0U, &attributeBytes) == FALSE)
        {
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(
                make_launch_error(a_assertContext, GetLastError()));
        }
        HANDLE inheritedHandle = reinterpret_cast<HANDLE>(a_executionLease->native_handle());
        if (UpdateProcThreadAttribute(startup.lpAttributeList, 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      &inheritedHandle, sizeof(inheritedHandle), nullptr, nullptr) == FALSE)
        {
            const DWORD error = GetLastError();
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(make_launch_error(a_assertContext, error));
        }
        created = CreateProcessW(executable.try_value()->c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
        const DWORD error = created == FALSE ? GetLastError() : ERROR_SUCCESS;
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (created == FALSE)
        {
            return Result<std::unique_ptr<WindowsEditorProcess>>::failure(make_launch_error(a_assertContext, error));
        }
    }
    if (!created)
    {
        return Result<std::unique_ptr<WindowsEditorProcess>>::failure(
            make_launch_error(a_assertContext, GetLastError()));
    }
    try
    {
        std::unique_ptr<WindowsEditorProcess> owner =
            std::make_unique<WindowsEditorProcessImpl>(process.hProcess, process.hThread, a_assertContext);
        return Result<std::unique_ptr<WindowsEditorProcess>>::success(std::move(owner));
    }
    catch (...)
    {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        terminate_allocation(a_assertContext);
    }
}
} // namespace cue::project_hub
