#include "Resources/CueEditorToolResource.h"

#include <Cue/Build/DiagnosticBundle.h>
#include <Cue/Build/Windows/WindowsArtifactPublisher.h>
#include <Cue/Build/Windows/WindowsToolchain.h>
#include <Cue/Distribution/Windows/Installer.h>
#include <Cue/Editor/ImGui/BuildPresenter.h>
#include <Cue/Editor/ImGui/DebugView.h>
#include <Cue/Editor/ImGui/EditorDockspace.h>
#include <Cue/Editor/ImGui/EditorPresenter.h>
#include <Cue/Editor/ImGui/FilesPresenter.h>
#include <Cue/Editor/ImGui/GameView.h>
#include <Cue/Editor/ImGui/PackagePresenter.h>
#include <Cue/Editor/ImGui/PlayInputRouting.h>
#include <Cue/Editor/ImGui/PlaySessionPresenter.h>
#include <Cue/Editor/ImGui/SessionLog.h>
#include <Cue/Editor/Windows/EditorSession.h>
#include <Cue/EditorCore/EditorIntent.h>
#include <Cue/EditorCore/EditorPlaySessionController.h>
#include <Cue/EditorCore/Error.h>
#include <Cue/EditorCore/SceneCommand.h>
#include <Cue/EngineAssets/BuiltInAssetCatalog.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Foundation/NumberParsing.h>
#include <Cue/Foundation/Windows/UtfConversion.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/RuntimeSystem.h>
#include <Cue/GameCore/World.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Input/InputEventQueue.h>
#include <Cue/Package/Workflow.h>
#include <Cue/Platform/Windows/WindowsProcess.h>
#include <Cue/Project/Compatibility.h>
#include <Cue/Renderer/RenderExtraction.h>
#include <Cue/Renderer/RendererRuntimeSystem.h>
#include <Cue/Renderer/RendererSchema.h>
#include <Cue/Runtime/RuntimeSchema.h>
#include <Cue/Runtime/RuntimeSystemFactory.h>
#include <Cue/Scene/Error.h>
#include <Cue/Scene/Serialization.h>
#include <Cue/Schema/Registry.h>
#include <Cue/ToolHost/WindowsD3D12/ToolHost.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <Windows.h>
#include <bcrypt.h>
#include <imgui.h>

namespace
{
constexpr int k_invalidArguments = 64;
constexpr int k_sessionInitializationFailed = 1;
constexpr int k_toolHostFailed = 2;
constexpr int k_processTestFailed = 3;
constexpr std::uint64_t k_firstEditorPlayGeneration = 1U;
constexpr std::int64_t k_maximumEditorPlayDeltaNanoseconds = 100'000'000;
constexpr std::size_t k_processTestPlayCycleCount = 12U;
constexpr std::uint32_t k_engineBuildPolicyVersion = 2U;

/// @brief EnumWindows中に対象Executableが所有する可視Windowを記録する
struct VisibleProcessWindowProbe final
{
    const std::filesystem::path *expectedExecutable = nullptr;
    bool found = false;
};

/// @brief 可視Top-level Windowの所有Processが対象Executableか照合する
BOOL CALLBACK observe_visible_process_window(HWND a_window, LPARAM a_context) noexcept
{
    auto *probe = reinterpret_cast<VisibleProcessWindowProbe *>(a_context);
    if (probe == nullptr || probe->expectedExecutable == nullptr || IsWindowVisible(a_window) == FALSE)
    {
        return TRUE;
    }
    DWORD processId = 0U;
    static_cast<void>(GetWindowThreadProcessId(a_window, &processId));
    if (processId == 0U)
    {
        return TRUE;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr)
    {
        return TRUE;
    }
    std::array<wchar_t, 32768U> executablePath{};
    DWORD executablePathSize = static_cast<DWORD>(executablePath.size());
    const BOOL queried = QueryFullProcessImageNameW(process, 0U, executablePath.data(), &executablePathSize);
    CloseHandle(process);
    if (queried == FALSE)
    {
        return TRUE;
    }
    std::error_code error;
    probe->found = std::filesystem::equivalent(
        *probe->expectedExecutable, std::filesystem::path(std::wstring_view(executablePath.data(), executablePathSize)),
        error);
    return probe->found ? FALSE : TRUE;
}

/// @brief UTF-8 Absolute Executableが所有する可視Top-level Windowを観測したか返す
[[nodiscard]] bool has_visible_process_window(std::string_view a_executable)
{
    const std::filesystem::path executable(
        std::u8string_view(reinterpret_cast<const char8_t *>(a_executable.data()), a_executable.size()));
    VisibleProcessWindowProbe probe{&executable, false};
    static_cast<void>(EnumWindows(observe_visible_process_window, reinterpret_cast<LPARAM>(&probe)));
    return probe.found;
}

/// @brief Child Processの全Capture Byte列に指定Markerが含まれるか返す
[[nodiscard]] bool process_output_contains(const std::vector<cue::ChildProcessOutputChunk> &a_output,
                                           std::string_view a_marker)
{
    std::string captured;
    for (const cue::ChildProcessOutputChunk &chunk : a_output)
    {
        captured.append(chunk.bytes);
    }
    return captured.find(a_marker) != std::string::npos;
}

/// @brief Windows System RNGからBuild Operation用UUID Version 4を発行する
class WindowsBuildOperationIdSource final : public cue::editor::BuildOperationIdSource
{
  public:
    /// @brief Fatal境界をOperation ID Source全寿命へ借用する
    explicit WindowsBuildOperationIdSource(const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext)
    {
    }
    /// @brief 借用Fatal境界だけを解放する
    ~WindowsBuildOperationIdSource() override = default;

    /// @brief BCrypt System RNGからlowercase UUID Version 4を返す
    [[nodiscard]] cue::Result<std::string> next_operation_id() noexcept override
    {
        std::array<std::uint8_t, 16U> bytes{};
        const NTSTATUS status =
            BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0)
        {
            cue::ErrorCode code = cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.EditorTool.Build", 1);
            cue::NativeError native = cue::NativeError::create(m_assertContext->fatal_handler(), "NTSTATUS", status);
            return cue::Result<std::string>::failure(
                cue::Error::create(m_assertContext->fatal_handler(), std::move(code),
                                   "Build operation identity generation failed", std::move(native)));
        }
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
        constexpr std::string_view digits = "0123456789abcdef";
        std::array<char, 36U> text{};
        std::size_t output = 0U;
        for (std::size_t index = 0U; index < bytes.size(); ++index)
        {
            if (index == 4U || index == 6U || index == 8U || index == 10U)
            {
                text[output++] = '-';
            }
            text[output++] = digits[bytes[index] >> 4U];
            text[output++] = digits[bytes[index] & 0x0fU];
        }
        try
        {
            return cue::Result<std::string>::success(std::string(text.data(), text.size()));
        }
        catch (...)
        {
            m_assertContext->fatal_handler().terminate("Build operation identity allocation failed");
            std::abort();
        }
    }

  private:
    const cue::AssertContext *m_assertContext;
};

/// @brief 検証済みToolchain Reportから指定Kindの選択Toolを返す
[[nodiscard]] const cue::BuildToolCandidate *find_tool(const cue::BuildEnvironmentReport &a_report,
                                                       cue::BuildToolKind a_kind) noexcept
{
    const auto found =
        std::find_if(a_report.selectedTools.begin(), a_report.selectedTools.end(),
                     [a_kind](const cue::BuildToolCandidate &a_tool) noexcept { return a_tool.kind == a_kind; });
    return found == a_report.selectedTools.end() ? nullptr : &*found;
}

/// @brief Compiler Installation Root末尾からMSBuildへ固定するminor Toolset Versionを返す
[[nodiscard]] std::optional<std::string> msvc_toolset_version(std::string_view a_installationRoot,
                                                              const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        while (!a_installationRoot.empty() && (a_installationRoot.back() == '/' || a_installationRoot.back() == '\\'))
        {
            a_installationRoot.remove_suffix(1U);
        }
        const std::size_t separator = a_installationRoot.find_last_of("/\\");
        const std::string_view version =
            separator == std::string_view::npos ? a_installationRoot : a_installationRoot.substr(separator + 1U);
        return version.empty() ? std::nullopt : std::optional<std::string>(version);
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("MSVC toolset version allocation failed");
        std::abort();
    }
}

/// @brief Windows DirectoryをChild ProcessのSYSTEMROOT値としてUTF-8で返す
[[nodiscard]] std::optional<std::string> windows_directory(const cue::AssertContext &a_assertContext) noexcept
{
    std::array<wchar_t, MAX_PATH> path{};
    const UINT length = GetWindowsDirectoryW(path.data(), static_cast<UINT>(path.size()));
    if (length == 0U || length >= path.size())
    {
        return std::nullopt;
    }
    std::string converted;
    const cue::WindowsUtfConversionResult result = cue::convert_windows_utf16_to_utf8(
        std::wstring_view(path.data(), length), converted, a_assertContext.fatal_handler());
    return result.status == cue::WindowsUtfConversionStatus::Success ? std::optional<std::string>(std::move(converted))
                                                                     : std::nullopt;
}

/// @brief 指定Windows Environment値をChild Process Allowlist用UTF-8へ変換する
[[nodiscard]] std::optional<std::string> windows_environment_value(const wchar_t *a_name,
                                                                   const cue::AssertContext &a_assertContext) noexcept
{
    const DWORD required = GetEnvironmentVariableW(a_name, nullptr, 0U);
    if (required == 0U || required > 32767U)
    {
        return std::nullopt;
    }
    try
    {
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(a_name, value.data(), required);
        if (written == 0U || written >= required)
        {
            return std::nullopt;
        }
        value.resize(written);
        std::string converted;
        const cue::WindowsUtfConversionResult result =
            cue::convert_windows_utf16_to_utf8(value, converted, a_assertContext.fatal_handler());
        return result.status == cue::WindowsUtfConversionStatus::Success
                   ? std::optional<std::string>(std::move(converted))
                   : std::nullopt;
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Build environment allowlist allocation failed");
        std::abort();
    }
}

/// @brief Editor ToolのCommand Line値と重複検査状態を保持する
struct EditorToolOptions final
{
    cue::editor::WindowsEditorLaunchParameters parameters{};
    cue::distribution::WindowsInheritedVersionExecutionLeaseRequest installedEngineRequest{};
    std::optional<std::string> processTestAction;
    std::uint64_t maximumFrameCount = 0U;
    bool hasProtocolVersion = false;
    bool hasProjectDescriptor = false;
    bool hasExpectedProjectId = false;
    bool hasCompatibilityId = false;
    bool hasInitialScene = false;
    bool hasExpectedInitialSceneAssetId = false;
    bool hasEngineInstallRoot = false;
    bool hasEngineVersionDirectory = false;
    bool hasEngineBundleId = false;
    bool hasEngineManifestDigest = false;
    bool hasEngineExecutionLeaseHandle = false;
    bool hasMaximumFrameCount = false;
    bool hasProcessTestAction = false;
};

/// @brief Canonical major.minor.patch Engine Version文字列を所有Valueへ変換する
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

/// @brief Document終了後に継続するSceneまたはProject操作
enum class PendingTransition : std::uint8_t
{
    None,
    NewScene,
    OpenScene,
    SaveSceneAs,
    ReloadScene,
    CloseProject,
};

/// @brief Process TestのRuntime生成、入力観測、停止済み所有数をProcess Scopeで保持する
struct PlayWorkflowProbe final
{
    std::size_t createdSystemCount = 0U;
    std::size_t destroyedSystemCount = 0U;
    std::size_t startedSystemCount = 0U;
    std::size_t updateCount = 0U;
    std::size_t inputUpdateCount = 0U;
    std::size_t stoppedSystemCount = 0U;
    std::size_t activeSystemCount = 0U;
    std::size_t injectedStartFailureCount = 0U;
    bool shouldFailNextStart = false;
};

/// @brief Process Test中のRuntime CallbackとPortable Input順を外部Probeへ記録する
class PlayWorkflowSystem final : public cue::game_core::RuntimeSystem
{
  public:
    /// @brief Session-local SystemをProcess Scope Probeへ関連付ける
    PlayWorkflowSystem(PlayWorkflowProbe &a_probe, const cue::AssertContext &a_assertContext) noexcept
        : m_probe(&a_probe), m_assertContext(&a_assertContext)
    {
    }

    /// @brief System Instanceの破棄をProcess Scope Probeへ記録する
    ~PlayWorkflowSystem() noexcept override
    {
        ++m_probe->destroyedSystemCount;
    }

    /// @brief 指定時だけ副作用なしのStart失敗を注入し成功Session数を記録する
    [[nodiscard]] cue::Result<void> start(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        if (m_probe->shouldFailNextStart)
        {
            m_probe->shouldFailNextStart = false;
            ++m_probe->injectedStartFailureCount;
            cue::ErrorCode code =
                cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.EditorTool.PlayWorkflowTest", 1);
            return cue::Result<void>::failure(cue::Error::create(m_assertContext->fatal_handler(), std::move(code),
                                                                 "Injected process workflow system start failure"));
        }
        m_isStarted = true;
        ++m_probe->startedSystemCount;
        ++m_probe->activeSystemCount;
        return cue::Result<void>::success();
    }

    /// @brief Input SnapshotがUpdate前にKeyDownを反映した回数を記録する
    [[nodiscard]] cue::Result<void> update(
        const cue::game_core::RuntimeSystemUpdateContext &a_context) noexcept override
    {
        ++m_probe->updateCount;
        if (a_context.input.was_key_pressed(cue::InputKey::A))
        {
            ++m_probe->inputUpdateCount;
        }
        return cue::Result<void>::success();
    }

    /// @brief 成功Start済みSystemだけを一度停止して残留数を減らす
    [[nodiscard]] cue::Result<void> stop(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        if (m_isStarted)
        {
            m_isStarted = false;
            ++m_probe->stoppedSystemCount;
            --m_probe->activeSystemCount;
        }
        return cue::Result<void>::success();
    }

  private:
    PlayWorkflowProbe *m_probe;
    const cue::AssertContext *m_assertContext;
    bool m_isStarted = false;
};

/// @brief 各Play Sessionへ独立したProcess Test Systemを生成する
class PlayWorkflowSystemFactory final : public cue::runtime::RuntimeSystemFactory
{
  public:
    /// @brief Project Scope FactoryをProcess Scope Probeへ関連付ける
    explicit PlayWorkflowSystemFactory(PlayWorkflowProbe &a_probe) noexcept : m_probe(&a_probe)
    {
    }

    /// @brief Callback順を観測するSession-local Systemと固定Descriptorを返す
    [[nodiscard]] cue::Result<cue::runtime::RuntimeSystemRegistration> create_system(
        const cue::AssertContext &a_assertContext) const noexcept override
    {
        ++m_probe->createdSystemCount;
        cue::runtime::RuntimeSystemRegistration registration{
            {"Editor.Play.ProcessWorkflow", cue::game_core::RuntimeUpdatePhase::Update, 0, {}},
            std::make_unique<PlayWorkflowSystem>(*m_probe, a_assertContext)};
        return cue::Result<cue::runtime::RuntimeSystemRegistration>::success(std::move(registration));
    }

  private:
    PlayWorkflowProbe *m_probe;
};

/// @brief Play前後で維持すべきAuthoring Documentの所有値Snapshotを保持する
struct PlayWorkflowDocumentState final
{
    std::string serializedScene;
    std::vector<std::string> selection;
    std::optional<std::string> primarySelection;
    std::uint64_t currentState = 0U;
    std::uint64_t savedState = 0U;
    std::size_t historyEntryCount = 0U;
    bool isDirty = false;
};

/// @brief Editor Tool失敗をProcess Rootの安定Domainへ分類する
[[nodiscard]] cue::Error make_tool_error(const cue::AssertContext &a_context, std::int64_t a_code,
                                         std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_context.fatal_handler(), "Cue.EditorTool", a_code);
    return cue::Error::create(a_context.fatal_handler(), std::move(code), a_summary);
}

/// @brief Authoring SceneとEditor固有状態をPlay前後比較用の所有値へ複製する
[[nodiscard]] cue::Result<PlayWorkflowDocumentState> capture_play_workflow_document_state(
    const cue::editor_core::EditorDocument &a_document, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        cue::Result<std::string> serialized =
            cue::scene::serialize_scene_document(a_document.scene_document(), a_assertContext);
        if (!serialized)
        {
            return cue::Result<PlayWorkflowDocumentState>::failure(std::move(*serialized.try_error()));
        }
        PlayWorkflowDocumentState state;
        state.serializedScene = std::move(*serialized.try_value());
        state.currentState = a_document.current_state_id().value();
        state.savedState = a_document.saved_state_id().value();
        state.historyEntryCount = a_document.history_entry_count();
        state.isDirty = a_document.is_dirty();
        state.selection.reserve(a_document.selection().size());
        for (const cue::scene::ObjectId &objectId : a_document.selection())
        {
            const cue::scene::IdentityText identity = objectId.canonical_text();
            state.selection.emplace_back(identity.data(), identity.size());
        }
        if (const cue::scene::ObjectId *primary = a_document.try_primary_selection(); primary != nullptr)
        {
            const cue::scene::IdentityText identity = primary->canonical_text();
            state.primarySelection.emplace(identity.data(), identity.size());
        }
        return cue::Result<PlayWorkflowDocumentState>::success(std::move(state));
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Editor Play workflow state capture failed unexpectedly");
        std::abort();
    }
}

/// @brief 現在DocumentがPlay前に取得したAuthoring状態と一致するか検証する
[[nodiscard]] bool matches_play_workflow_document_state(const cue::editor_core::EditorDocument &a_document,
                                                        const PlayWorkflowDocumentState &a_expected,
                                                        const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<PlayWorkflowDocumentState> current = capture_play_workflow_document_state(a_document, a_assertContext);
    if (!current)
    {
        return false;
    }
    return current.try_value()->serializedScene == a_expected.serializedScene &&
           current.try_value()->selection == a_expected.selection &&
           current.try_value()->primarySelection == a_expected.primarySelection &&
           current.try_value()->currentState == a_expected.currentState &&
           current.try_value()->savedState == a_expected.savedState &&
           current.try_value()->historyEntryCount == a_expected.historyEntryCount &&
           current.try_value()->isDirty == a_expected.isDirty;
}

/// @brief UI Composition中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_tool_exception(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("Editor Tool operation failed unexpectedly");
    std::abort();
}

/// @brief Windows Command Line値をStrict UTF-8へ変換する
[[nodiscard]] cue::Result<std::string> convert_argument(std::wstring_view a_value,
                                                        const cue::AssertContext &a_context) noexcept
{
    std::string converted;
    const cue::WindowsUtfConversionResult conversion =
        cue::convert_windows_utf16_to_utf8(a_value, converted, a_context.fatal_handler());
    if (conversion.status != cue::WindowsUtfConversionStatus::Success)
    {
        cue::ErrorCode code = cue::ErrorCode::create(a_context.fatal_handler(), "Cue.EditorTool", k_invalidArguments);
        cue::NativeError nativeError =
            cue::NativeError::create(a_context.fatal_handler(), "Win32", conversion.nativeCode);
        cue::Error error = cue::Error::create(a_context.fatal_handler(), std::move(code),
                                              "Editor option is not valid UTF-16", std::move(nativeError));
        return cue::Result<std::string>::failure(std::move(error));
    }

    return cue::Result<std::string>::success(std::move(converted));
}

/// @brief Process Test Actionから要求するGame Build Configurationを復元する
[[nodiscard]] std::optional<cue::BuildConfiguration> process_test_build_configuration(
    std::string_view a_action) noexcept
{
    if (a_action == "build-workflow-debug")
    {
        return cue::BuildConfiguration::Debug;
    }
    if (a_action == "build-workflow-development")
    {
        return cue::BuildConfiguration::Development;
    }
    if (a_action == "build-workflow-release")
    {
        return cue::BuildConfiguration::Release;
    }
    return std::nullopt;
}

/// @brief Process Test Actionから要求するPackage Workflow Configurationを復元する
[[nodiscard]] std::optional<cue::BuildConfiguration> process_test_package_configuration(
    std::string_view a_action) noexcept
{
    if (a_action == "package-workflow-debug")
    {
        return cue::BuildConfiguration::Debug;
    }
    if (a_action == "package-workflow-development")
    {
        return cue::BuildConfiguration::Development;
    }
    if (a_action == "package-workflow-release")
    {
        return cue::BuildConfiguration::Release;
    }
    return std::nullopt;
}

/// @brief Process Test ActionがRelease Monolithic Shipping Workflowを要求するか返す
[[nodiscard]] bool is_process_test_shipping_workflow(std::string_view a_action) noexcept
{
    return a_action == "shipping-workflow-release";
}

/// @brief Editor起動Contractの必須値、重複、未知Optionを検証する
[[nodiscard]] cue::Result<EditorToolOptions> parse_options(int a_argumentCount, wchar_t **a_arguments,
                                                           const cue::AssertContext &a_context) noexcept
{
    try
    {
        EditorToolOptions options;
        for (int index = 1; index < a_argumentCount; ++index)
        {
            const std::wstring_view option = a_arguments[index];
            if (index + 1 >= a_argumentCount)
            {
                return cue::Result<EditorToolOptions>::failure(
                    make_tool_error(a_context, k_invalidArguments, "Editor option is missing its value"));
            }
            const std::wstring_view value = a_arguments[++index];
            if (option == L"--protocol-version")
            {
                const std::optional<std::uint32_t> protocol = cue::parse_unsigned_decimal<std::uint32_t>(value);
                if (options.hasProtocolVersion || !protocol.has_value())
                {
                    return cue::Result<EditorToolOptions>::failure(make_tool_error(
                        a_context, k_invalidArguments, "Editor protocol version is duplicated or invalid"));
                }
                options.parameters.protocolVersion = *protocol;
                options.hasProtocolVersion = true;
            }
            else if (option == L"--project-descriptor")
            {
                if (options.hasProjectDescriptor)
                {
                    return cue::Result<EditorToolOptions>::failure(
                        make_tool_error(a_context, k_invalidArguments, "Project descriptor option is duplicated"));
                }
                cue::Result<std::string> converted = convert_argument(value, a_context);
                if (!converted)
                {
                    return cue::Result<EditorToolOptions>::failure(std::move(*converted.try_error()));
                }
                options.parameters.projectDescriptorLocator = std::move(*converted.try_value());
                options.hasProjectDescriptor = true;
            }
            else if (option == L"--expected-project-id")
            {
                if (options.hasExpectedProjectId)
                {
                    return cue::Result<EditorToolOptions>::failure(
                        make_tool_error(a_context, k_invalidArguments, "Expected ProjectId option is duplicated"));
                }
                cue::Result<std::string> converted = convert_argument(value, a_context);
                if (!converted)
                {
                    return cue::Result<EditorToolOptions>::failure(std::move(*converted.try_error()));
                }
                options.parameters.expectedProjectId = std::move(*converted.try_value());
                options.hasExpectedProjectId = true;
            }
            else if (option == L"--engine-compatibility-id")
            {
                if (options.hasCompatibilityId)
                {
                    return cue::Result<EditorToolOptions>::failure(
                        make_tool_error(a_context, k_invalidArguments, "Engine compatibility option is duplicated"));
                }
                cue::Result<std::string> converted = convert_argument(value, a_context);
                if (!converted)
                {
                    return cue::Result<EditorToolOptions>::failure(std::move(*converted.try_error()));
                }
                options.parameters.engineCompatibilityId = std::move(*converted.try_value());
                options.hasCompatibilityId = true;
            }
            else if (option == L"--initial-scene")
            {
                if (options.hasInitialScene)
                {
                    return cue::Result<EditorToolOptions>::failure(
                        make_tool_error(a_context, k_invalidArguments, "Initial scene option is duplicated"));
                }
                cue::Result<std::string> converted = convert_argument(value, a_context);
                if (!converted)
                {
                    return cue::Result<EditorToolOptions>::failure(std::move(*converted.try_error()));
                }
                options.parameters.initialSceneLocator = std::move(*converted.try_value());
                options.hasInitialScene = true;
            }
            else if (option == L"--expected-initial-scene-asset-id")
            {
                if (options.hasExpectedInitialSceneAssetId)
                {
                    return cue::Result<EditorToolOptions>::failure(make_tool_error(
                        a_context, k_invalidArguments, "Expected initial SceneAssetId option is duplicated"));
                }
                cue::Result<std::string> converted = convert_argument(value, a_context);
                if (!converted)
                {
                    return cue::Result<EditorToolOptions>::failure(std::move(*converted.try_error()));
                }
                options.parameters.expectedInitialSceneAssetId = std::move(*converted.try_value());
                options.hasExpectedInitialSceneAssetId = true;
            }
            else if (option == L"--engine-install-root")
            {
                if (options.hasEngineInstallRoot)
                {
                    return cue::Result<EditorToolOptions>::failure(
                        make_tool_error(a_context, k_invalidArguments, "Engine install root is duplicated"));
                }
                cue::Result<std::string> converted = convert_argument(value, a_context);
                if (!converted)
                {
                    return cue::Result<EditorToolOptions>::failure(std::move(*converted.try_error()));
                }
                options.installedEngineRequest.installRoot = std::move(*converted.try_value());
                options.hasEngineInstallRoot = true;
            }
            else if (option == L"--engine-version-directory")
            {
                if (options.hasEngineVersionDirectory)
                {
                    return cue::Result<EditorToolOptions>::failure(
                        make_tool_error(a_context, k_invalidArguments, "Engine version directory is duplicated"));
                }
                cue::Result<std::string> converted = convert_argument(value, a_context);
                if (!converted)
                {
                    return cue::Result<EditorToolOptions>::failure(std::move(*converted.try_error()));
                }
                options.installedEngineRequest.versionDirectory = std::move(*converted.try_value());
                options.hasEngineVersionDirectory = true;
            }
            else if (option == L"--engine-bundle-id")
            {
                if (options.hasEngineBundleId)
                {
                    return cue::Result<EditorToolOptions>::failure(
                        make_tool_error(a_context, k_invalidArguments, "Engine bundle identity is duplicated"));
                }
                cue::Result<std::string> converted = convert_argument(value, a_context);
                if (!converted)
                {
                    return cue::Result<EditorToolOptions>::failure(std::move(*converted.try_error()));
                }
                options.installedEngineRequest.bundleId = std::move(*converted.try_value());
                options.hasEngineBundleId = true;
            }
            else if (option == L"--engine-manifest-digest")
            {
                if (options.hasEngineManifestDigest)
                {
                    return cue::Result<EditorToolOptions>::failure(
                        make_tool_error(a_context, k_invalidArguments, "Engine manifest digest is duplicated"));
                }
                cue::Result<std::string> converted = convert_argument(value, a_context);
                if (!converted)
                {
                    return cue::Result<EditorToolOptions>::failure(std::move(*converted.try_error()));
                }
                options.installedEngineRequest.manifestDigest = std::move(*converted.try_value());
                options.hasEngineManifestDigest = true;
            }
            else if (option == L"--engine-execution-lease-handle")
            {
                const auto handleValue = cue::parse_unsigned_decimal<std::uintptr_t>(value);
                if (options.hasEngineExecutionLeaseHandle || !handleValue || *handleValue == 0U)
                {
                    return cue::Result<EditorToolOptions>::failure(make_tool_error(
                        a_context, k_invalidArguments, "Engine Execution Lease handle is duplicated or invalid"));
                }
                options.installedEngineRequest.inheritedLeaseHandle = *handleValue;
                options.hasEngineExecutionLeaseHandle = true;
            }
            else if (option == L"--maximum-frame-count")
            {
                const std::optional<std::uint64_t> frameCount = cue::parse_unsigned_decimal<std::uint64_t>(value);
                if (options.hasMaximumFrameCount || !frameCount.has_value())
                {
                    return cue::Result<EditorToolOptions>::failure(
                        make_tool_error(a_context, k_invalidArguments, "Maximum frame count is duplicated or invalid"));
                }
                options.maximumFrameCount = *frameCount;
                options.hasMaximumFrameCount = true;
            }
            else if (option == L"--process-test-action")
            {
                if (options.hasProcessTestAction)
                {
                    return cue::Result<EditorToolOptions>::failure(
                        make_tool_error(a_context, k_invalidArguments, "Process test action is duplicated"));
                }
                cue::Result<std::string> converted = convert_argument(value, a_context);
                if (!converted)
                {
                    return cue::Result<EditorToolOptions>::failure(std::move(*converted.try_error()));
                }
                options.processTestAction = std::move(*converted.try_value());
                options.hasProcessTestAction = true;
            }
            else
            {
                return cue::Result<EditorToolOptions>::failure(
                    make_tool_error(a_context, k_invalidArguments, "Editor option is not recognized"));
            }
        }
        if (!options.hasProtocolVersion || !options.hasProjectDescriptor || !options.hasExpectedProjectId ||
            !options.hasCompatibilityId)
        {
            return cue::Result<EditorToolOptions>::failure(
                make_tool_error(a_context, k_invalidArguments, "Required Editor launch options are missing"));
        }
        if (options.hasExpectedInitialSceneAssetId && !options.hasInitialScene)
        {
            return cue::Result<EditorToolOptions>::failure(make_tool_error(
                a_context, k_invalidArguments, "Expected initial SceneAssetId requires an initial scene locator"));
        }
        const std::size_t installedOptionCount = static_cast<std::size_t>(options.hasEngineInstallRoot) +
                                                 static_cast<std::size_t>(options.hasEngineVersionDirectory) +
                                                 static_cast<std::size_t>(options.hasEngineBundleId) +
                                                 static_cast<std::size_t>(options.hasEngineManifestDigest) +
                                                 static_cast<std::size_t>(options.hasEngineExecutionLeaseHandle);
        if (installedOptionCount != 0U && installedOptionCount != 5U)
        {
            return cue::Result<EditorToolOptions>::failure(make_tool_error(
                a_context, k_invalidArguments, "Installed Engine launch options must be supplied as one contract"));
        }
        if (options.hasProcessTestAction &&
            (!options.hasInitialScene || !options.hasMaximumFrameCount ||
             (*options.processTestAction != "autosave-recovery" && *options.processTestAction != "autosave-new-scene" &&
              *options.processTestAction != "edit-close-save" && *options.processTestAction != "files-workflow" &&
              *options.processTestAction != "play-repeated-workflow" &&
              !process_test_build_configuration(*options.processTestAction).has_value() &&
              !process_test_package_configuration(*options.processTestAction).has_value() &&
              !is_process_test_shipping_workflow(*options.processTestAction))))
        {
            return cue::Result<EditorToolOptions>::failure(make_tool_error(
                a_context, k_invalidArguments,
                "Process test action requires an initial scene, a maximum frame count, and a recognized value"));
        }
        return cue::Result<EditorToolOptions>::success(std::move(options));
    }
    catch (...)
    {
        terminate_tool_exception(a_context);
    }
}

/// @brief 現在BuildがM12で対応するProject互換性入力を生成する
[[nodiscard]] cue::Result<cue::editor::WindowsEditorEngineConfiguration> make_engine_configuration(
    cue::EngineVersion a_engineVersion, const cue::AssertContext &a_context) noexcept
{
    cue::Result<cue::ProjectCapabilityProfile> profile = cue::ProjectCapabilityProfile::create({}, a_context);
    if (!profile)
    {
        return cue::Result<cue::editor::WindowsEditorEngineConfiguration>::failure(std::move(*profile.try_error()));
    }
    cue::Result<cue::ProjectCapabilitySnapshot> snapshot = cue::ProjectCapabilitySnapshot::create({}, a_context);
    if (!snapshot)
    {
        return cue::Result<cue::editor::WindowsEditorEngineConfiguration>::failure(std::move(*snapshot.try_error()));
    }
    return cue::Result<cue::editor::WindowsEditorEngineConfiguration>::success(
        {cue::k_currentProjectDescriptorSchemaVersion, a_engineVersion, std::move(*profile.try_value()),
         std::move(*snapshot.try_value())});
}

/// @brief Project-only ShellとActive EditorPresenterをFile Workflowへ接続する
class EditorToolClient final : public cue::tool_host::ToolHostClient
{
  public:
    /// @brief Sessionと診断ContextをClient全寿命へ関連付ける
    EditorToolClient(cue::editor::WindowsEditorSession &a_session, cue::Logger &a_logger,
                     cue::editor::EditorSessionLogRouter &a_logRouter,
                     std::span<const cue::runtime::RuntimeSystemFactory *const> a_systemFactories,
                     const cue::distribution::WindowsInstalledVersionExecutionLease *a_installedEngineLease,
                     const cue::AssertContext &a_assertContext) noexcept
        : m_session(&a_session), m_assertContext(&a_assertContext), m_rendererSystemFactory(m_runtimeRenderSnapshot)
    {
        try
        {
            cue::schema::SchemaRegistryBuilder schemaBuilder(m_runtimeSchemaIdentitySource, a_assertContext);
            cue::Result<void> addedRuntimeSchema =
                cue::runtime::add_runtime_schema_types(schemaBuilder, a_assertContext);
            if (!addedRuntimeSchema)
            {
                cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                                  "Editor Runtime Schema initialization failed",
                                  std::move(*addedRuntimeSchema.try_error()));
            }
            cue::Result<void> addedRendererSchema =
                cue::renderer::add_renderer_schema_types(schemaBuilder, a_assertContext);
            if (!addedRendererSchema)
            {
                cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                                  "Editor Renderer Schema initialization failed",
                                  std::move(*addedRendererSchema.try_error()));
            }
            cue::Result<std::unique_ptr<cue::schema::SchemaRegistry>> runtimeSchema = schemaBuilder.seal();
            if (!runtimeSchema)
            {
                cue::report_fatal(a_logger, a_assertContext.fatal_handler(), "Editor Runtime Schema sealing failed",
                                  std::move(*runtimeSchema.try_error()));
            }
            m_runtimeSchema = std::move(*runtimeSchema.try_value());

            cue::Result<cue::runtime::RuntimeSchemaTypeIds> typeIds =
                cue::runtime::make_runtime_schema_type_ids(a_assertContext);
            if (!typeIds)
            {
                cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                                  "Editor Runtime Schema identity initialization failed",
                                  std::move(*typeIds.try_error()));
            }
            std::vector<const cue::runtime::RuntimeSystemFactory *> runtimeSystemFactories;
            runtimeSystemFactories.reserve(a_systemFactories.size() + 1U);
            runtimeSystemFactories.push_back(&m_rendererSystemFactory);
            runtimeSystemFactories.insert(runtimeSystemFactories.end(), a_systemFactories.begin(),
                                          a_systemFactories.end());
            cue::Result<std::unique_ptr<cue::editor_core::EditorPlaySessionController>> playController =
                cue::editor_core::EditorPlaySessionController::create(
                    a_session.controller().session(), m_worldIdentitySource, m_clock, *m_runtimeSchema,
                    runtimeSystemFactories, std::move(typeIds.try_value()->transform),
                    std::move(typeIds.try_value()->sceneObjectState), k_firstEditorPlayGeneration,
                    k_maximumEditorPlayDeltaNanoseconds, a_assertContext);
            if (!playController)
            {
                cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                                  "Editor Play Session Controller initialization failed",
                                  std::move(*playController.try_error()));
            }
            m_playController = std::move(*playController.try_value());

            cue::Result<cue::renderer::RendererSchemaTypeIds> rendererTypeIds =
                cue::renderer::make_renderer_schema_type_ids(a_assertContext);
            if (!rendererTypeIds)
            {
                cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                                  "Editor Renderer Schema identity initialization failed",
                                  std::move(*rendererTypeIds.try_error()));
            }
            m_rendererTypeIds.emplace(std::move(*rendererTypeIds.try_value()));
            cue::Result<cue::renderer::DebugCamera> debugCamera =
                cue::renderer::DebugCamera::create_default(a_assertContext.fatal_handler());
            if (!debugCamera)
            {
                cue::report_fatal(a_logger, a_assertContext.fatal_handler(),
                                  "Editor Debug Camera initialization failed", std::move(*debugCamera.try_error()));
            }
            m_debugCamera.emplace(std::move(*debugCamera.try_value()));

            m_playPresenter =
                cue::editor::PlaySessionPresenter::create(*m_playController, a_logger, a_logRouter, a_assertContext);
            m_filesPresenter =
                std::make_unique<cue::editor::FilesPresenter>(a_session.files_workspace(), a_assertContext);
            initialize_build_workflow(a_logger, a_installedEngineLease);
            refresh_recovery_candidates();
            rebuild_presenter();
        }
        catch (...)
        {
            terminate_tool_exception(a_assertContext);
        }
    }

    EditorToolClient(const EditorToolClient &) = delete;
    EditorToolClient &operator=(const EditorToolClient &) = delete;
    /// @brief Runtimeを停止してからPresenter、購読Token、ControllerをSessionより先に破棄する
    ~EditorToolClient() override
    {
        if (m_packagePresenter != nullptr &&
            (m_packagePresenter->current_snapshot().state == cue::package::PackageWorkflowState::Building ||
             m_packagePresenter->current_snapshot().state == cue::package::PackageWorkflowState::Packaging ||
             m_packagePresenter->current_snapshot().state == cue::package::PackageWorkflowState::Running))
        {
            static_cast<void>(m_packagePresenter->begin_editor_shutdown());
            const bool immediatelyReady = m_packagePresenter->respond_to_editor_shutdown(
                cue::editor::EditorPackageShutdownDecision::StopAndClose);
            if (!immediatelyReady)
            {
                const cue::package::PackageWorkflowState state = m_packagePresenter->current_snapshot().state;
                cue::Result<void> completed = state == cue::package::PackageWorkflowState::Running
                                                  ? m_packageService->wait_for_run_completion()
                                                  : m_packageService->wait_for_package();
                m_packagePresenter->refresh();
                if (!completed || !m_packagePresenter->take_shutdown_ready())
                {
                    m_assertContext->fatal_handler().terminate(
                        "Editor Tool destruction requires completed Package Workflow cleanup");
                }
            }
        }
        if (m_buildPresenter != nullptr &&
            m_buildPresenter->current_snapshot().state == cue::GameBuildOperationState::Running)
        {
            const bool alreadyReady = m_buildPresenter->begin_editor_shutdown();
            if (!alreadyReady)
            {
                const bool immediatelyReady = m_buildPresenter->respond_to_editor_shutdown(
                    cue::editor::EditorBuildShutdownDecision::CancelBuildAndClose);
                if (!immediatelyReady)
                {
                    cue::Result<void> completed = m_buildService->wait_for_completion();
                    m_buildPresenter->refresh();
                    if (!completed || !m_buildPresenter->take_shutdown_ready())
                    {
                        m_assertContext->fatal_handler().terminate(
                            "Editor Tool destruction requires completed Build cleanup");
                    }
                }
            }
        }
        const cue::editor_core::EditorPlaySessionState state = m_playPresenter->state_snapshot().state;
        if (state != cue::editor_core::EditorPlaySessionState::Idle &&
            state != cue::editor_core::EditorPlaySessionState::Stopped)
        {
            static_cast<void>(m_playPresenter->begin_editor_shutdown());
            if (!m_playPresenter->respond_to_editor_shutdown(cue::editor::EditorPlayShutdownDecision::StopAndClose))
            {
                m_assertContext->fatal_handler().terminate(
                    "Editor Tool destruction requires completed Play Session cleanup");
            }
        }
    }

    /// @brief 最初のFrame前にEditor専用ImGui Dockingを有効化する
    void window_ready(cue::Window &) noexcept override
    {
        cue::editor::enable_editor_docking();
    }

    /// @brief Tool HostのPortable入力を現在FrameのViewport Routing用に所有Copyする
    void input_frame(cue::tool_host::ToolHostInputFrameView a_input) noexcept override
    {
        m_inputSnapshot = a_input.snapshot;
        m_uiInputCapture = a_input.uiCapture;
        m_inputPlayGeneration = m_playController->state_snapshot().generation;
        m_inputEventCount = (std::min)(a_input.events.size(), m_inputEvents.size());
        std::copy_n(a_input.events.begin(), m_inputEventCount, m_inputEvents.begin());
        if (a_input.events.size() > m_inputEvents.size())
        {
            m_inputEvents[0] = {cue::InputEventType::DeviceReset};
            m_inputEventCount = 1U;
        }
    }

    /// @brief Game Viewが前Frameで計測した描画領域をTool Host要求へ変換する
    [[nodiscard]] cue::tool_host::ToolHostRenderSurfaceRequests render_surface_requests() const noexcept override
    {
        return {{{m_gameViewRequest.width, m_gameViewRequest.height, m_gameViewRequest.isVisible},
                 {m_debugViewRequest.width, m_debugViewRequest.height, m_debugViewRequest.isVisible}}};
    }

    /// @brief Tool Hostが所有する非所有Texture Viewを現在FrameのGame／Debug Viewへ関連付ける
    void render_surfaces_ready(cue::tool_host::ToolHostRenderSurfaceViews a_surfaces) noexcept override
    {
        const cue::tool_host::ToolHostRenderSurfaceView game =
            a_surfaces[static_cast<std::size_t>(cue::tool_host::ToolHostRenderSurfaceSlot::GameView)];
        const cue::tool_host::ToolHostRenderSurfaceView debug =
            a_surfaces[static_cast<std::size_t>(cue::tool_host::ToolHostRenderSurfaceSlot::DebugView)];
        m_gameViewSurface = {game.textureId, game.width, game.height};
        m_debugViewSurface = {debug.textureId, debug.width, debug.height};
    }

    /// @brief 現在のEdit／Play SnapshotへMain CameraとEditor専用DebugCameraを関連付ける
    [[nodiscard]] cue::tool_host::ToolHostRenderFrameView render_frame_view() const noexcept override
    {
        const cue::editor_core::EditorPlaySessionState playState = m_playPresenter->state_snapshot().state;
        const cue::renderer::RenderSnapshot &snapshot =
            playState == cue::editor_core::EditorPlaySessionState::Running ||
                    playState == cue::editor_core::EditorPlaySessionState::StopRequested
                ? m_runtimeRenderSnapshot.snapshot()
                : m_authoringRenderSnapshot;
        return {&snapshot, snapshot.try_main_camera(), m_debugCamera ? &m_debugCamera->camera() : nullptr};
    }

    /// @brief 実Editor Compositionから一構成の生成Project BuildとArtifact公開を検証する
    [[nodiscard]] cue::Result<void> run_build_workflow_process_test(cue::BuildConfiguration a_configuration) noexcept
    {
        try
        {
            /// @brief Build Workflow不変条件違反をProcess Test用Errorへ変換する
            const auto fail = [this](std::string_view a_summary) noexcept
            { return cue::Result<void>::failure(make_tool_error(*m_assertContext, k_processTestFailed, a_summary)); };

            /// @brief Process Test用File全体を比較可能なByte列として読む
            const auto read_file = [](const std::filesystem::path &a_path)
            {
                std::ifstream input(a_path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            };

            /// @brief Process Test専用Sourceを完全なByte列として置換する
            const auto write_source = [](const std::filesystem::path &a_path, std::string_view a_bytes) noexcept
            {
                std::ofstream output(a_path, std::ios::binary | std::ios::trunc);
                output.write(a_bytes.data(), static_cast<std::streamsize>(a_bytes.size()));
                return output.good();
            };

            if (m_buildPresenter == nullptr || m_buildService == nullptr || !m_buildWorkspaceCompatibility)
            {
                return fail("Build workflow is unavailable in the Editor composition");
            }
            if (!m_buildPresenter->set_configuration(a_configuration) || !m_buildPresenter->set_force_configure(true) ||
                !m_buildPresenter->submit(cue::editor::EditorBuildCommand::Start))
            {
                return fail("Editor Build command could not be submitted");
            }
            cue::Result<void> completed = m_buildService->wait_for_completion();
            m_buildPresenter->refresh();
            const cue::BuildOperationSnapshot initialSnapshot = m_buildPresenter->current_snapshot();
            if (!completed)
            {
                return cue::Result<void>::failure(std::move(*completed.try_error()));
            }
            if (initialSnapshot.state != cue::GameBuildOperationState::Succeeded)
            {
                for (const cue::BuildLogSnapshot &log : initialSnapshot.logs)
                {
                    static_cast<void>(std::fwrite(log.bytes.data(), sizeof(char), log.bytes.size(), stderr));
                }
                for (const cue::BuildDiagnosticSnapshot &diagnostic : initialSnapshot.diagnostics)
                {
                    std::fprintf(stderr, "Build diagnostic: %s/%lld %s\n", diagnostic.domain.c_str(),
                                 static_cast<long long>(diagnostic.code), diagnostic.summary.c_str());
                    for (const std::string &context : diagnostic.contexts)
                    {
                        std::fprintf(stderr, "  Context: %s\n", context.c_str());
                    }
                    if (diagnostic.nativeError)
                    {
                        std::fprintf(stderr, "  Native: %s/%lld\n", diagnostic.nativeError->domain.c_str(),
                                     static_cast<long long>(diagnostic.nativeError->code));
                    }
                }
                static_cast<void>(std::fflush(stderr));
                if (!initialSnapshot.diagnostics.empty())
                {
                    return fail(initialSnapshot.diagnostics.front().summary);
                }
                if (!initialSnapshot.logs.empty())
                {
                    return fail(initialSnapshot.logs.back().bytes);
                }
                return fail("Editor Build did not complete successfully");
            }
            if (!initialSnapshot.artifact || initialSnapshot.artifact->configuration() != a_configuration ||
                initialSnapshot.stages.size() != 2U)
            {
                return fail("Editor Build did not publish the requested Game Module artifact");
            }
            bool hasDll = false;
            bool hasMetadata = false;
            bool hasPdb = false;
            for (const cue::BuildArtifactFile &file : initialSnapshot.artifact->files())
            {
                hasDll = hasDll || file.relativePath == "CueGameModule.dll";
                hasMetadata = hasMetadata || file.relativePath == "CueGameModule.metadata.json";
                hasPdb = hasPdb || file.relativePath == "CueGameModule.pdb";
            }
            if (!hasDll || !hasMetadata || (a_configuration != cue::BuildConfiguration::Release && !hasPdb))
            {
                return fail("Editor Build Artifact inventory is incomplete");
            }

            const std::string_view configuration =
                a_configuration == cue::BuildConfiguration::Debug
                    ? "Debug"
                    : (a_configuration == cue::BuildConfiguration::Development ? "Development" : "Release");
            const std::string_view projectLocator = m_session->project_locator();
            const std::filesystem::path projectRoot(
                std::u8string_view(reinterpret_cast<const char8_t *>(projectLocator.data()), projectLocator.size()));
            const std::filesystem::path currentPath =
                projectRoot / "Generated" / "Artifacts" / "GameModule" / configuration / "modular" / "Current.json";
            std::ifstream currentStream(currentPath, std::ios::binary);
            const std::string current{std::istreambuf_iterator<char>(currentStream), std::istreambuf_iterator<char>()};
            if (!currentStream.is_open() || currentStream.bad() ||
                current.find(initialSnapshot.operationId) == std::string::npos ||
                current.find("CueGameModule.dll") == std::string::npos ||
                current.find("CueGameModule.metadata.json") == std::string::npos ||
                (hasPdb && current.find("CueGameModule.pdb") == std::string::npos))
            {
                return fail("Editor Build Current manifest does not identify the published artifact");
            }
            currentStream.close();

            const std::filesystem::path gameSource = projectRoot / "Source" / "Game" / "GameModule.cpp";
            std::ifstream sourceStream(gameSource, std::ios::binary);
            const std::string source{std::istreambuf_iterator<char>(sourceStream), std::istreambuf_iterator<char>()};
            if (!sourceStream.is_open() || sourceStream.bad() || source.empty() ||
                !write_source(gameSource, source + "\n#error CUE_EDITOR_PROCESS_EXPECTED_BUILD_FAILURE\n"))
            {
                return fail("Editor Build process test could not prepare its isolated failure input");
            }
            const bool configuredForReuse = m_buildPresenter->set_force_configure(false);
            const bool failureSubmitted =
                configuredForReuse && m_buildPresenter->submit(cue::editor::EditorBuildCommand::Start);
            cue::Result<void> failureCompleted =
                failureSubmitted ? m_buildService->wait_for_completion() : cue::Result<void>::success();
            m_buildPresenter->refresh();
            const cue::BuildOperationSnapshot failedSnapshot = m_buildPresenter->current_snapshot();
            const bool sourceRestored = write_source(gameSource, source);
            if (!sourceRestored)
            {
                return fail("Editor Build process test could not restore its isolated Source input");
            }
            if (!failureSubmitted || !failureCompleted ||
                failedSnapshot.state != cue::GameBuildOperationState::Failed ||
                !failedSnapshot.latestSuccessfulArtifact || failedSnapshot.artifact ||
                read_file(currentPath) != current)
            {
                return fail("Editor Build failure did not preserve the previous successful artifact");
            }
            if (!failedSnapshot.profile)
            {
                return fail("Editor Build failure did not retain its Build Profile");
            }
            cue::BuildRequest failedRequest{std::string(projectLocator), *failedSnapshot.profile,
                                            failedSnapshot.operationId, *m_buildWorkspaceCompatibility};
            cue::Result<cue::BuildPlan> failedPlan = cue::create_build_plan(failedRequest, *m_assertContext);
            if (!failedPlan)
            {
                return cue::Result<void>::failure(std::move(*failedPlan.try_error()));
            }
            cue::BuildDiagnosticBundleInput diagnosticInput{
                failedSnapshot,
                cue::make_build_diagnostic_plan_snapshot(*failedPlan.try_value(), *m_assertContext),
                std::nullopt,
                {}};
            cue::BuildDiagnosticBundleLimits diagnosticLimits;
            cue::Result<cue::BuildDiagnosticBundle> diagnostic =
                cue::create_build_diagnostic_bundle(diagnosticInput, diagnosticLimits, *m_assertContext);
            const std::filesystem::path diagnosticParent = projectRoot / "Saved" / "Build" / "DiagnosticBundles";
            const std::filesystem::path diagnosticPath = diagnosticParent / failedSnapshot.operationId;
            std::error_code filesystemError;
            std::filesystem::create_directories(diagnosticParent, filesystemError);
            cue::Result<std::string> diagnosticLocator = convert_argument(diagnosticPath.native(), *m_assertContext);
            if (!diagnostic || filesystemError || !diagnosticLocator)
            {
                return fail("Editor Build diagnostic bundle could not be prepared");
            }
            cue::Result<void> diagnosticWritten = cue::write_build_diagnostic_bundle_directory(
                *diagnostic.try_value(), *diagnosticLocator.try_value(), *m_assertContext);
            if (!diagnosticWritten)
            {
                return cue::Result<void>::failure(std::move(*diagnosticWritten.try_error()));
            }
            cue::Result<cue::BuildDiagnosticBundle> diagnosticRead = cue::read_build_diagnostic_bundle_directory(
                *diagnosticLocator.try_value(), diagnosticLimits, *m_assertContext);
            if (!diagnosticRead || diagnosticRead.try_value()->operation_id() != failedSnapshot.operationId ||
                diagnosticRead.try_value()->state() != cue::GameBuildOperationState::Failed ||
                diagnosticRead.try_value()->files().size() != diagnostic.try_value()->files().size())
            {
                return fail("Editor Build diagnostic bundle did not survive its write and read workflow");
            }

            if (!m_buildPresenter->submit(cue::editor::EditorBuildCommand::Retry))
            {
                return fail("Editor Build retry command could not be submitted");
            }
            completed = m_buildService->wait_for_completion();
            m_buildPresenter->refresh();
            const cue::BuildOperationSnapshot &retrySnapshot = m_buildPresenter->current_snapshot();
            if (!completed)
            {
                return cue::Result<void>::failure(std::move(*completed.try_error()));
            }
            if (retrySnapshot.state != cue::GameBuildOperationState::Succeeded)
            {
                if (!retrySnapshot.diagnostics.empty())
                {
                    return fail(retrySnapshot.diagnostics.front().summary);
                }
                if (!retrySnapshot.logs.empty())
                {
                    return fail(retrySnapshot.logs.back().bytes);
                }
                return fail("Editor Build retry did not succeed");
            }
            if (retrySnapshot.operationId == failedSnapshot.operationId)
            {
                return fail("Editor Build retry reused the failed Operation identity");
            }
            if (!retrySnapshot.artifact || retrySnapshot.artifact->configuration() != a_configuration)
            {
                return fail("Editor Build retry did not publish the requested configuration");
            }
            if (read_file(currentPath).find(retrySnapshot.operationId) == std::string::npos)
            {
                return fail("Editor Build retry Current manifest does not identify the new artifact");
            }
            return cue::Result<void>::success();
        }
        catch (...)
        {
            terminate_tool_exception(*m_assertContext);
        }
    }

    /// @brief 実Editor CompositionからBuild、Package、Standalone起動をHeadless検証する
    [[nodiscard]] cue::Result<void> run_package_workflow_process_test(cue::BuildConfiguration a_configuration) noexcept
    {
        try
        {
            /// @brief Package Workflow不変条件違反をProcess Test用Errorへ変換する
            const auto fail = [this](std::string_view a_summary) noexcept
            { return cue::Result<void>::failure(make_tool_error(*m_assertContext, k_processTestFailed, a_summary)); };

            if (m_packagePresenter == nullptr || m_packageService == nullptr)
            {
                return fail("Package workflow is unavailable in the Editor composition");
            }
            m_packagePresenter->set_active_document(m_session->active_document_id());
            if (!m_packagePresenter->set_configuration(a_configuration) ||
                !m_packagePresenter->set_force_configure(true))
            {
                return fail("Editor Package configuration could not be applied");
            }
            if (!m_packagePresenter->submit(cue::editor::EditorPackageCommand::Start))
            {
                return fail(m_packagePresenter->message().empty() ? "Editor Package command could not be submitted"
                                                                  : m_packagePresenter->message());
            }
            cue::Result<void> completed = m_packageService->wait_for_package();
            m_packagePresenter->refresh();
            const cue::package::PackageWorkflowSnapshot &published = m_packagePresenter->current_snapshot();
            if (!completed)
            {
                if (!published.build.diagnostics.empty())
                {
                    return fail(published.build.diagnostics.back().summary);
                }
                if (!published.build.logs.empty())
                {
                    return fail(published.build.logs.back().bytes);
                }
                if (!published.message.empty())
                {
                    return fail(published.message);
                }
                return cue::Result<void>::failure(std::move(*completed.try_error()));
            }
            if (published.state != cue::package::PackageWorkflowState::PackageReady || !published.package ||
                published.package->manifest.configuration != a_configuration ||
                published.package->manifest.fileCount < 5U || published.package->artifactId.empty() ||
                published.package->destination.empty() || published.package->executable.empty())
            {
                return fail(published.message.empty() ? "Editor Package did not complete successfully"
                                                      : published.message);
            }
            cue::Result<void> started = m_packageService->run(cue::package::PackageRunMode::SmokeTest);
            if (!started)
            {
                return cue::Result<void>::failure(std::move(*started.try_error()));
            }
            completed = m_packageService->wait_for_run_completion();
            m_packagePresenter->refresh();
            const cue::package::PackageWorkflowSnapshot &ran = m_packagePresenter->current_snapshot();
            if (!completed)
            {
                return cue::Result<void>::failure(std::move(*completed.try_error()));
            }
            if (ran.state != cue::package::PackageWorkflowState::RunSucceeded ||
                ran.activeStage != cue::package::PackageWorkflowStage::None)
            {
                return fail(ran.message.empty() ? "Packaged Runtime did not exit successfully" : ran.message);
            }
            return cue::Result<void>::success();
        }
        catch (...)
        {
            terminate_tool_exception(*m_assertContext);
        }
    }

    /// @brief 実Editor CompositionからRelease Shipping ProductのBuild、Package、起動をHeadless検証する
    [[nodiscard]] cue::Result<void> run_shipping_workflow_process_test() noexcept
    {
        try
        {
            /// @brief Shipping Workflow不変条件違反をProcess Test用Errorへ変換する
            const auto fail = [this](std::string_view a_summary) noexcept
            { return cue::Result<void>::failure(make_tool_error(*m_assertContext, k_processTestFailed, a_summary)); };

            if (m_packagePresenter == nullptr || m_packageService == nullptr)
            {
                return fail("Shipping workflow is unavailable in the Editor composition");
            }
            m_packagePresenter->set_active_document(m_session->active_document_id());
            if (!m_packagePresenter->set_configuration(cue::BuildConfiguration::Release) ||
                !m_packagePresenter->set_force_configure(true))
            {
                return fail("Editor Shipping configuration could not be applied");
            }
            if (!m_packagePresenter->submit(cue::editor::EditorPackageCommand::StartShipping))
            {
                return fail(m_packagePresenter->message().empty() ? "Editor Shipping command could not be submitted"
                                                                  : m_packagePresenter->message());
            }
            cue::Result<void> completed = m_packageService->wait_for_package();
            m_packagePresenter->refresh();
            const cue::package::PackageWorkflowSnapshot &published = m_packagePresenter->current_snapshot();
            if (!completed)
            {
                return cue::Result<void>::failure(std::move(*completed.try_error()));
            }
            if (published.state != cue::package::PackageWorkflowState::PackageReady || !published.package ||
                published.package->manifest.configuration != cue::BuildConfiguration::Release ||
                published.package->manifest.executionModel != cue::package::PackageExecutionModel::Monolithic ||
                published.package->manifest.trustMode != cue::ShippingTrustMode::UnsignedLocal ||
                published.package->manifest.publicDistributionReady || published.package->manifest.fileCount != 3U ||
                published.package->artifactId.empty() || published.package->destination.empty() ||
                published.recoveryStagingLocator.has_value() ||
                !published.package->destination.starts_with("Generated/Packages/Shipping/Release/") ||
                !published.package->executable.ends_with("/CueGameProduct.exe"))
            {
                if (!published.build.diagnostics.empty())
                {
                    return fail(published.build.diagnostics.back().summary);
                }
                if (!published.build.logs.empty())
                {
                    return fail(published.build.logs.back().bytes);
                }
                return fail(published.message.empty() ? "Editor Shipping Package did not complete successfully"
                                                      : published.message);
            }
            const std::string productExecutable = published.package->executable;
            cue::Result<void> started = m_packageService->run(cue::package::PackageRunMode::SmokeTest);
            if (!started)
            {
                return cue::Result<void>::failure(std::move(*started.try_error()));
            }
            completed = m_packageService->wait_for_run_completion();
            m_packagePresenter->refresh();
            const cue::package::PackageWorkflowSnapshot &ran = m_packagePresenter->current_snapshot();
            if (!completed)
            {
                return cue::Result<void>::failure(std::move(*completed.try_error()));
            }
            if (ran.state != cue::package::PackageWorkflowState::RunSucceeded ||
                ran.activeStage != cue::package::PackageWorkflowStage::None || ran.recoveryStagingLocator.has_value())
            {
                return fail(ran.message.empty() ? "Packaged Shipping Product did not exit successfully" : ran.message);
            }

            started = m_packageService->run(cue::package::PackageRunMode::Interactive);
            if (!started)
            {
                return cue::Result<void>::failure(std::move(*started.try_error()));
            }
            const auto startupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            bool observedStartedProduct = false;
            while (std::chrono::steady_clock::now() < startupDeadline)
            {
                const cue::package::PackageWorkflowSnapshot snapshot = m_packageService->snapshot();
                if (snapshot.state != cue::package::PackageWorkflowState::Running)
                {
                    return fail(snapshot.message.empty() ? "Packaged Shipping Product exited before Stop verification"
                                                         : snapshot.message);
                }
                if (has_visible_process_window(productExecutable))
                {
                    observedStartedProduct = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!observedStartedProduct)
            {
                return fail("Packaged Shipping Product startup was not observed before timeout");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const auto stopStartedAt = std::chrono::steady_clock::now();
            cue::Result<void> stopped = m_packageService->stop();
            if (!stopped)
            {
                return cue::Result<void>::failure(std::move(*stopped.try_error()));
            }
            completed = m_packageService->wait_for_run_completion();
            const auto stopElapsed = std::chrono::steady_clock::now() - stopStartedAt;
            m_packagePresenter->refresh();
            const cue::package::PackageWorkflowSnapshot &stoppedSnapshot = m_packagePresenter->current_snapshot();
            if (!completed || stoppedSnapshot.state != cue::package::PackageWorkflowState::PackageReady ||
                stoppedSnapshot.activeStage != cue::package::PackageWorkflowStage::None ||
                stoppedSnapshot.recoveryStagingLocator.has_value() || stopElapsed >= std::chrono::seconds(5) ||
                !process_output_contains(stoppedSnapshot.runOutput, "D3D12 Render Loop shutdown completed"))
            {
                return completed
                           ? fail(stoppedSnapshot.message.empty() ? "Packaged Shipping Product did not stop cleanly"
                                                                  : stoppedSnapshot.message)
                           : cue::Result<void>::failure(std::move(*completed.try_error()));
            }
            return cue::Result<void>::success();
        }
        catch (...)
        {
            terminate_tool_exception(*m_assertContext);
        }
    }

    /// @brief 実Editor Process内で反復Play、失敗復旧、終了時Cleanupの統合契約を検証する
    [[nodiscard]] cue::Result<void> run_play_workflow_process_test(PlayWorkflowProbe &a_probe) noexcept
    {
        try
        {
            /// @brief Workflow不変条件違反をProcess Test用Errorへ変換する
            const auto fail = [this](std::string_view a_summary) noexcept
            { return cue::Result<void>::failure(make_tool_error(*m_assertContext, k_processTestFailed, a_summary)); };

            if (!m_session->active_document_id().has_value())
            {
                return fail("Play workflow requires an active scene");
            }
            const cue::editor_core::EditorDocumentId documentId = *m_session->active_document_id();
            const cue::editor_core::EditorDocument *document =
                m_session->controller().session().find_document(documentId);
            if (document == nullptr || document->scene_document().objects().empty())
            {
                return fail("Play workflow requires an authoring object");
            }
            const cue::scene::ObjectId objectId = document->scene_document().objects().front().id();
            cue::editor_core::RenameObjectIntent renameBefore{objectId, "Process Play Source"};
            cue::Result<void> renamedBefore = m_session->controller().execute_intent(
                documentId, std::move(renameBefore), m_session->identity_source(), {});
            const std::array selection{objectId};
            cue::Result<void> selected = m_session->controller().set_selection(documentId, selection, &objectId);
            if (!renamedBefore || !selected)
            {
                return fail("Play workflow could not prepare dirty authoring state and selection");
            }
            document = m_session->controller().session().find_document(documentId);
            if (document == nullptr)
            {
                return fail("Play workflow lost the prepared Editor document");
            }
            cue::Result<PlayWorkflowDocumentState> captured =
                capture_play_workflow_document_state(*document, *m_assertContext);
            if (!captured)
            {
                return cue::Result<void>::failure(std::move(*captured.try_error()));
            }
            PlayWorkflowDocumentState expected = std::move(*captured.try_value());
            std::vector<std::uint64_t> worldIds;
            worldIds.reserve(k_processTestPlayCycleCount);

            for (std::size_t cycle = 0U; cycle < k_processTestPlayCycleCount; ++cycle)
            {
                cue::Result<void> started = m_playController->start(documentId);
                const cue::editor_core::EditorPlaySessionSnapshot running = m_playController->state_snapshot();
                if (!started || running.state != cue::editor_core::EditorPlaySessionState::Running ||
                    running.worldId == 0U || a_probe.activeSystemCount != 1U ||
                    std::find(worldIds.begin(), worldIds.end(), running.worldId) != worldIds.end())
                {
                    return fail("Repeated Play did not create one independent Runtime session");
                }
                worldIds.push_back(running.worldId);

                const std::size_t updateCount = a_probe.updateCount;
                const std::size_t inputUpdateCount = a_probe.inputUpdateCount;
                cue::Result<bool> pushed =
                    m_playController->push_input_event({cue::InputEventType::KeyDown, cue::InputKey::A});
                cue::Result<void> advanced = m_playController->advance_frame({});
                if (!pushed || !*pushed.try_value() || !advanced || a_probe.updateCount != updateCount + 1U ||
                    a_probe.inputUpdateCount != inputUpdateCount + 1U)
                {
                    return fail("Runtime Update did not observe the queued Input in the same frame");
                }
                cue::Result<void> requested = m_playController->request_stop();
                cue::Result<void> stopped = m_playController->stop();
                const cue::editor_core::EditorPlaySessionSnapshot stoppedState = m_playController->state_snapshot();
                document = m_session->controller().session().find_document(documentId);
                if (!requested || !stopped || stoppedState.state != cue::editor_core::EditorPlaySessionState::Stopped ||
                    a_probe.activeSystemCount != 0U || a_probe.createdSystemCount != a_probe.destroyedSystemCount ||
                    document == nullptr || !matches_play_workflow_document_state(*document, expected, *m_assertContext))
                {
                    return fail("Stop left Runtime ownership or changed the Authoring document");
                }

                if (cycle == 0U)
                {
                    cue::editor_core::RenameObjectIntent renameAfter{objectId, "Process Play Re-edited"};
                    cue::Result<void> renamedAfter = m_session->controller().execute_intent(
                        documentId, std::move(renameAfter), m_session->identity_source(), {});
                    document = m_session->controller().session().find_document(documentId);
                    if (!renamedAfter || document == nullptr)
                    {
                        return fail("Authoring edit after Stop failed");
                    }
                    captured = capture_play_workflow_document_state(*document, *m_assertContext);
                    if (!captured)
                    {
                        return cue::Result<void>::failure(std::move(*captured.try_error()));
                    }
                    expected = std::move(*captured.try_value());
                }
            }

            cue::Result<cue::runtime::RuntimeSchemaTypeIds> runtimeTypeIds =
                cue::runtime::make_runtime_schema_type_ids(*m_assertContext);
            cue::Result<cue::scene::ComponentInstanceId> opaqueComponentId =
                cue::scene::ComponentInstanceId::parse("00000000-0000-4000-8000-000000000216", *m_assertContext);
            cue::Result<cue::schema::SchemaVersion> futureVersion =
                cue::schema::SchemaVersion::create(2U, *m_assertContext);
            if (!runtimeTypeIds || !opaqueComponentId || !futureVersion)
            {
                return fail("Scene load failure input could not be constructed");
            }
            const cue::scene::ComponentInstanceId retainedOpaqueComponentId = *opaqueComponentId.try_value();
            cue::Result<cue::scene::OpaqueComponentData> opaqueComponent = cue::scene::OpaqueComponentData::create(
                std::move(*opaqueComponentId.try_value()), runtimeTypeIds.try_value()->transform,
                std::move(*futureVersion.try_value()), "{\"future\":true}", *m_runtimeSchema, *m_assertContext);
            if (!opaqueComponent)
            {
                return cue::Result<void>::failure(std::move(*opaqueComponent.try_error()));
            }
            const cue::scene::SceneAssetId sceneAssetId = document->scene_document().scene_asset_id();
            cue::Result<cue::editor_core::DocumentStateId> addedOpaque =
                m_session->controller().execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, sceneAssetId,
                    cue::editor_core::AddComponentCommand{
                        objectId, cue::scene::SceneComponent::opaque(std::move(*opaqueComponent.try_value()))}});
            document = m_session->controller().session().find_document(documentId);
            if (!addedOpaque || document == nullptr)
            {
                return fail("Scene load failure component could not be added to the Authoring document");
            }
            captured = capture_play_workflow_document_state(*document, *m_assertContext);
            if (!captured)
            {
                return cue::Result<void>::failure(std::move(*captured.try_error()));
            }
            const PlayWorkflowDocumentState unsupportedSceneState = std::move(*captured.try_value());
            const std::size_t createdBeforeLoadFailure = a_probe.createdSystemCount;
            const std::size_t destroyedBeforeLoadFailure = a_probe.destroyedSystemCount;
            cue::Result<void> failedSceneLoad = m_playController->start(documentId);
            const cue::editor_core::EditorPlaySessionSnapshot failedLoadState = m_playController->state_snapshot();
            document = m_session->controller().session().find_document(documentId);
            if (failedSceneLoad || failedLoadState.state != cue::editor_core::EditorPlaySessionState::Stopped ||
                !failedLoadState.hasFailure ||
                failedSceneLoad.try_error()->root_code().value() !=
                    static_cast<std::int64_t>(cue::scene::SceneError::UnsupportedRuntimeComponent) ||
                a_probe.createdSystemCount != createdBeforeLoadFailure + 1U ||
                a_probe.destroyedSystemCount != destroyedBeforeLoadFailure + 1U || a_probe.activeSystemCount != 0U ||
                document == nullptr ||
                !matches_play_workflow_document_state(*document, unsupportedSceneState, *m_assertContext))
            {
                return fail("Scene instantiation failure did not roll back without changing Editor state");
            }
            cue::Result<cue::editor_core::DocumentStateId> removedOpaque =
                m_session->controller().execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, sceneAssetId,
                    cue::editor_core::RemoveComponentCommand{objectId, retainedOpaqueComponentId}});
            document = m_session->controller().session().find_document(documentId);
            if (!removedOpaque || document == nullptr)
            {
                return fail("Authoring recovery edit after Scene load failure failed");
            }
            captured = capture_play_workflow_document_state(*document, *m_assertContext);
            if (!captured || captured.try_value()->serializedScene != expected.serializedScene ||
                captured.try_value()->selection != expected.selection ||
                captured.try_value()->primarySelection != expected.primarySelection)
            {
                return fail("Scene load failure recovery did not restore Authoring contents and selection");
            }
            expected = std::move(*captured.try_value());

            a_probe.shouldFailNextStart = true;
            cue::Result<void> failedStart = m_playController->start(documentId);
            const cue::editor_core::EditorPlaySessionSnapshot failedState = m_playController->state_snapshot();
            document = m_session->controller().session().find_document(documentId);
            if (failedStart || failedState.state != cue::editor_core::EditorPlaySessionState::Stopped ||
                !failedState.hasFailure || a_probe.injectedStartFailureCount != 1U || a_probe.activeSystemCount != 0U ||
                a_probe.createdSystemCount != a_probe.destroyedSystemCount || document == nullptr ||
                !matches_play_workflow_document_state(*document, expected, *m_assertContext) ||
                !m_playController->stop())
            {
                return fail("System Start failure did not roll back without changing Editor state");
            }

            const std::size_t recoveryUpdateCount = a_probe.updateCount;
            const std::size_t recoveryInputCount = a_probe.inputUpdateCount;
            cue::Result<void> recoveredStart = m_playController->start(documentId);
            cue::Result<bool> recoveredInput =
                m_playController->push_input_event({cue::InputEventType::KeyDown, cue::InputKey::A});
            cue::Result<void> recoveredUpdate = m_playController->advance_frame({});
            cue::Result<void> recoveredStop = m_playController->stop();
            if (!recoveredStart || !recoveredInput || !*recoveredInput.try_value() || !recoveredUpdate ||
                !recoveredStop || a_probe.updateCount != recoveryUpdateCount + 1U ||
                a_probe.inputUpdateCount != recoveryInputCount + 1U || a_probe.activeSystemCount != 0U ||
                a_probe.createdSystemCount != a_probe.destroyedSystemCount)
            {
                return fail("Play could not recover after the injected Start failure");
            }
            document = m_session->controller().session().find_document(documentId);
            if (document == nullptr || !matches_play_workflow_document_state(*document, expected, *m_assertContext))
            {
                return fail("Recovered Play changed the Authoring document");
            }

            cue::Result<void> finalStart = m_playController->start(documentId);
            if (!finalStart || a_probe.activeSystemCount != 1U ||
                a_probe.createdSystemCount != a_probe.destroyedSystemCount + 1U)
            {
                return fail("Editor close cleanup test could not leave one active child session");
            }
            return cue::Result<void>::success();
        }
        catch (...)
        {
            terminate_tool_exception(*m_assertContext);
        }
    }

    /// @brief Project-onlyまたはActive Scene UIを描画しFrame末尾でWorkflowを進める
    void draw_frame() noexcept override
    {
        try
        {
            m_playPresenter->set_active_document(m_session->active_document_id());
            m_playPresenter->process_shortcuts();
            if (m_buildPresenter != nullptr)
            {
                m_buildPresenter->process_shortcuts();
            }
            if (m_playPresenter->take_shutdown_ready())
            {
                request_project_close();
            }
            if (m_presenter != nullptr)
            {
                m_presenter->draw();
                std::optional<cue::editor::EditorWorkflowRequest> request = m_presenter->take_workflow_request();
                if (request.has_value())
                {
                    handle_workflow_request(*request);
                }
            }
            else
            {
                draw_project_shell();
            }
            cue::editor::dock_editor_window_on_first_use();
            m_filesPresenter->draw();
            cue::editor::dock_editor_window_on_first_use();
            m_playPresenter->draw();
            m_gameViewRequest = cue::editor::draw_game_view(m_gameViewSurface);
            m_debugViewRequest = cue::editor::draw_debug_view(m_debugViewSurface);
            update_debug_camera();
            refresh_render_snapshot();
            if (m_buildPresenter != nullptr)
            {
                cue::editor::dock_editor_window_on_first_use();
                m_buildPresenter->draw();
                if (m_buildPresenter->take_shutdown_ready())
                {
                    request_project_close();
                }
            }
            else if (!m_buildUnavailableMessage.empty())
            {
                cue::editor::dock_editor_window_on_first_use();
                if (ImGui::Begin("Game Build"))
                {
                    ImGui::TextWrapped("%s", m_buildUnavailableMessage.c_str());
                }
                ImGui::End();
            }
            if (m_packagePresenter != nullptr)
            {
                m_packagePresenter->set_active_document(m_session->active_document_id());
                cue::editor::dock_editor_window_on_first_use();
                m_packagePresenter->draw();
                if (m_packagePresenter->take_shutdown_ready())
                {
                    request_project_close();
                }
            }
            draw_locator_dialog();
            draw_close_dialog();
            draw_overwrite_dialog();
            draw_uncertain_save_dialog();
            route_and_advance_runtime();
            autosave_recovery_if_needed();
        }
        catch (...)
        {
            terminate_tool_exception(*m_assertContext);
        }
    }

    /// @brief Native Window終了要求をDirty Close状態遷移へ変換する
    void request_close() noexcept override
    {
        if (!m_shouldClose && m_pendingTransition == PendingTransition::None)
        {
            request_project_close();
        }
    }

    /// @brief Project Session終了が確認済みならTool Host終了を許可する
    [[nodiscard]] bool should_close() const noexcept override
    {
        return m_shouldClose;
    }

  private:
    /// @brief 現在のWindows ToolchainをBuild Service、Artifact Publisher、ImGui Presenterへ接続する
    void initialize_build_workflow(
        cue::Logger &a_logger,
        const cue::distribution::WindowsInstalledVersionExecutionLease *a_installedEngineLease) noexcept
    {
        cue::BuildEnvironmentReport environment;
        if (a_installedEngineLease != nullptr)
        {
            cue::BuildEnvironmentInventory inventory =
                cue::discover_current_windows_build_environment(*m_assertContext);
            inventory.engineSourceRoot = a_installedEngineLease->engine_source_root();
            inventory.engineSourceAvailable = true;
            inventory.engineBinaryRoot.clear();
            inventory.engineBinaryAvailable = false;
            cue::BuildEnvironmentRequirements requirements =
                cue::current_windows_build_requirements(*m_assertContext);
            requirements.requiresEngineBinary = false;
            environment = cue::validate_build_environment(inventory, requirements, *m_assertContext);
        }
        else
        {
            environment = cue::validate_current_windows_build_environment(*m_assertContext);
        }
        if (environment.support != cue::BuildEnvironmentSupport::Supported)
        {
            m_buildUnavailableMessage = "Game Buildは現在のToolchain構成では利用できません。";
            if (!environment.diagnostics.empty())
            {
                m_buildUnavailableMessage.append(" ");
                m_buildUnavailableMessage.append(environment.diagnostics.front().summary);
            }
            return;
        }
        const cue::BuildToolCandidate *cmake = find_tool(environment, cue::BuildToolKind::CMake);
        const cue::BuildToolCandidate *compiler = find_tool(environment, cue::BuildToolKind::MsvcCompiler);
        const std::optional<std::string> toolsetVersion =
            compiler != nullptr ? msvc_toolset_version(compiler->installationRoot, *m_assertContext) : std::nullopt;
        const std::optional<std::string> systemRoot = windows_directory(*m_assertContext);
        if (cmake == nullptr || compiler == nullptr || !compiler->version || !toolsetVersion || !systemRoot)
        {
            m_buildUnavailableMessage = "Game Buildに必要なToolchain情報を確定できませんでした。";
            return;
        }
        struct EnvironmentVariable final
        {
            const wchar_t *nativeName;
            const char *name;
        };
        constexpr std::array environmentVariables = {
            EnvironmentVariable{L"windir", "windir"},
            EnvironmentVariable{L"SystemDrive", "SystemDrive"},
            EnvironmentVariable{L"ProgramFiles", "ProgramFiles"},
            EnvironmentVariable{L"ProgramFiles(x86)", "ProgramFiles(x86)"},
            EnvironmentVariable{L"ProgramData", "ProgramData"},
            EnvironmentVariable{L"CommonProgramFiles", "CommonProgramFiles"},
            EnvironmentVariable{L"CommonProgramFiles(x86)", "CommonProgramFiles(x86)"},
            EnvironmentVariable{L"TEMP", "TEMP"},
            EnvironmentVariable{L"TMP", "TMP"},
            EnvironmentVariable{L"ComSpec", "ComSpec"},
            EnvironmentVariable{L"OS", "OS"},
            EnvironmentVariable{L"NUMBER_OF_PROCESSORS", "NUMBER_OF_PROCESSORS"},
            EnvironmentVariable{L"PROCESSOR_ARCHITECTURE", "PROCESSOR_ARCHITECTURE"},
            EnvironmentVariable{L"PROCESSOR_IDENTIFIER", "PROCESSOR_IDENTIFIER"},
            EnvironmentVariable{L"PROCESSOR_LEVEL", "PROCESSOR_LEVEL"},
            EnvironmentVariable{L"PROCESSOR_REVISION", "PROCESSOR_REVISION"}};
        std::vector<cue::ChildProcessEnvironmentEntry> environmentAllowlist;
        environmentAllowlist.reserve(environmentVariables.size() + 1U);
        environmentAllowlist.push_back({"SYSTEMROOT", *systemRoot});
        for (const EnvironmentVariable &variable : environmentVariables)
        {
            const std::optional<std::string> value = windows_environment_value(variable.nativeName, *m_assertContext);
            if (!value)
            {
                m_buildUnavailableMessage = "Game Buildに必要なWindows Environmentを確定できませんでした。";
                return;
            }
            environmentAllowlist.push_back({variable.name, *value});
        }
        const std::vector<cue::ChildProcessEnvironmentEntry> runEnvironment = environmentAllowlist;
        cue::Result<std::unique_ptr<cue::ChildProcessRunner>> processRunner =
            cue::create_windows_child_process_runner(*m_assertContext);
        cue::Result<std::unique_ptr<cue::ChildProcessRunner>> packageBuildProcessRunner =
            cue::create_windows_child_process_runner(*m_assertContext);
        if (!processRunner || !packageBuildProcessRunner)
        {
            cue::report_fatal(a_logger, m_assertContext->fatal_handler(), "Build Process Runner initialization failed",
                              processRunner ? std::move(*packageBuildProcessRunner.try_error())
                                            : std::move(*processRunner.try_error()));
        }
        auto createArtifactPublisher = [&]() noexcept
        {
            if (a_installedEngineLease == nullptr)
            {
                return cue::create_windows_build_artifact_publisher(
                    std::string(m_session->project_locator()),
                    m_session->controller().session().project_descriptor(), *m_assertContext);
            }
            cue::WindowsInstalledEngineSourceProvenance provenance{
                a_installedEngineLease->engine_source_root(), a_installedEngineLease->engine_source_revision(),
                a_installedEngineLease->source_inventory_hash(),
                a_installedEngineLease->publisher_build_identity_digest()};
            return cue::create_windows_build_artifact_publisher(
                std::string(m_session->project_locator()),
                m_session->controller().session().project_descriptor(), std::move(provenance), *m_assertContext);
        };
        cue::Result<std::unique_ptr<cue::BuildArtifactPublisher>> artifactPublisher = createArtifactPublisher();
        cue::Result<std::unique_ptr<cue::BuildArtifactPublisher>> packageArtifactPublisher = createArtifactPublisher();
        cue::Result<std::unique_ptr<cue::BuildArtifactReader>> packageArtifactReader =
            cue::create_windows_build_artifact_reader(std::string(m_session->project_locator()),
                                                      m_session->controller().session().project_descriptor(),
                                                      *m_assertContext);
        if (!artifactPublisher || !packageArtifactPublisher || !packageArtifactReader)
        {
            cue::Error error = !artifactPublisher
                                   ? std::move(*artifactPublisher.try_error())
                                   : (!packageArtifactPublisher ? std::move(*packageArtifactPublisher.try_error())
                                                                : std::move(*packageArtifactReader.try_error()));
            cue::report_fatal(a_logger, m_assertContext->fatal_handler(), "Build Artifact access initialization failed",
                              std::move(error));
        }
        cue::CMakeRunnerSettings runnerSettings{
            cmake->nativePath,       environment.engineSourceRoot, std::move(environmentAllowlist),
            std::chrono::minutes(5), std::chrono::minutes(30),     *toolsetVersion};
        runnerSettings.buildsRuntimeHost = a_installedEngineLease != nullptr;
        cue::Result<std::unique_ptr<cue::GameBuildService>> service =
            cue::GameBuildService::create(runnerSettings, std::move(*processRunner.try_value()),
                                          std::move(*artifactPublisher.try_value()), *m_assertContext);
        cue::Result<std::unique_ptr<cue::GameBuildService>> packageBuildService =
            cue::GameBuildService::create(std::move(runnerSettings), std::move(*packageBuildProcessRunner.try_value()),
                                          std::move(*packageArtifactPublisher.try_value()), *m_assertContext);
        if (!service || !packageBuildService)
        {
            cue::report_fatal(a_logger, m_assertContext->fatal_handler(), "Game Build Service initialization failed",
                              service ? std::move(*packageBuildService.try_error()) : std::move(*service.try_error()));
        }
        m_buildService = std::move(*service.try_value());
        cue::BuildWorkspaceCompatibility compatibility{cue::BuildGenerator::VisualStudio2026,
                                                       cue::BuildArchitecture::X64, *compiler->version,
                                                       k_engineBuildPolicyVersion};
        m_buildWorkspaceCompatibility = compatibility;
        m_buildPresenter = cue::editor::BuildPresenter::create(
            *m_buildService, std::string(m_session->project_locator()), compatibility,
            std::make_unique<WindowsBuildOperationIdSource>(*m_assertContext), *m_assertContext);
        cue::Result<std::unique_ptr<cue::FilesystemRoot>> projectFilesystem =
            cue::create_windows_filesystem_root(m_session->project_locator(), *m_assertContext);
        const std::string_view engineBinaryRoot = a_installedEngineLease != nullptr
                                                      ? m_session->project_locator()
                                                      : std::string_view(environment.engineBinaryRoot);
        cue::Result<std::unique_ptr<cue::FilesystemRoot>> engineBinaryFilesystem =
            cue::create_windows_filesystem_root(engineBinaryRoot, *m_assertContext);
        cue::Result<std::unique_ptr<cue::ChildProcessRunner>> runProcessRunner =
            cue::create_windows_child_process_runner(*m_assertContext);
        if (!projectFilesystem || !engineBinaryFilesystem || !runProcessRunner)
        {
            cue::Error error = !projectFilesystem
                                   ? std::move(*projectFilesystem.try_error())
                                   : (!engineBinaryFilesystem ? std::move(*engineBinaryFilesystem.try_error())
                                                              : std::move(*runProcessRunner.try_error()));
            cue::report_fatal(a_logger, m_assertContext->fatal_handler(),
                              "Package Workflow dependency initialization failed", std::move(error));
        }
        cue::Result<std::unique_ptr<cue::package::GamePackageWorkflowService>> packageService =
            cue::package::GamePackageWorkflowService::create(
                std::move(*packageBuildService.try_value()), std::move(*packageArtifactReader.try_value()),
                std::move(*projectFilesystem.try_value()), std::move(*engineBinaryFilesystem.try_value()),
                std::move(*runProcessRunner.try_value()), std::string(m_session->project_locator()), runEnvironment,
                a_installedEngineLease != nullptr ? cue::package::RuntimeHostBuildSource::ProjectBuildTree
                                                  : cue::package::RuntimeHostBuildSource::EngineBinaryRoot,
                *m_assertContext);
        if (!packageService)
        {
            cue::report_fatal(a_logger, m_assertContext->fatal_handler(),
                              "Package Workflow Service initialization failed", std::move(*packageService.try_error()));
        }
        m_packageService = std::move(*packageService.try_value());
        m_packagePresenter = cue::editor::PackagePresenter::create(
            *m_packageService, m_session->controller(), std::string(m_session->project_locator()), compatibility,
            std::make_unique<WindowsBuildOperationIdSource>(*m_assertContext), *m_assertContext);
        m_packagePresenter->set_active_document(m_session->active_document_id());
    }

    /// @brief Active Document Identityに対応するPresenterを再生成する
    void rebuild_presenter() noexcept
    {
        m_lastRecoveryDocumentId.reset();
        m_lastRecoveryStateValue.reset();
        if (!m_session->active_document_id().has_value())
        {
            m_presenter.reset();
            return;
        }
        cue::Result<cue::scene::ComponentInstanceId> cameraTemplateId =
            cue::scene::ComponentInstanceId::parse("70000000-0000-4000-8000-000000000101", *m_assertContext);
        cue::Result<cue::scene::ComponentInstanceId> meshTemplateId =
            cue::scene::ComponentInstanceId::parse("70000000-0000-4000-8000-000000000102", *m_assertContext);
        if (!cameraTemplateId || !meshTemplateId)
        {
            m_assertContext->fatal_handler().terminate("Editor Renderer component template identity is invalid");
        }
        cue::Result<cue::scene::SceneComponent> cameraTemplate = cue::renderer::make_camera_component(
            std::move(*cameraTemplateId.try_value()), false, m_session->schema_registry(),
            m_session->value_schema_registry(), *m_assertContext);
        cue::Result<cue::scene::SceneComponent> meshTemplate = cue::renderer::make_cube_mesh_component(
            std::move(*meshTemplateId.try_value()), m_session->schema_registry(), m_session->value_schema_registry(),
            *m_assertContext);
        if (!cameraTemplate || !meshTemplate)
        {
            m_assertContext->fatal_handler().terminate("Editor Renderer component template creation failed");
        }
        std::vector<cue::editor_core::EditorComponentTemplate> componentTemplates;
        componentTemplates.push_back({"Camera", std::move(*cameraTemplate.try_value())});
        componentTemplates.push_back({"Mesh (Built-in Cube)", std::move(*meshTemplate.try_value())});

        const std::span<const cue::engine_assets::BuiltInMeshDescriptor> meshCatalog =
            cue::engine_assets::built_in_mesh_catalog();
        cue::Result<void> validatedCatalog =
            cue::engine_assets::validate_builtin_mesh_catalog(meshCatalog, *m_assertContext);
        if (!validatedCatalog)
        {
            m_assertContext->fatal_handler().terminate("Editor Built-in Mesh Catalog validation failed");
        }
        std::vector<cue::editor_core::EditorPrimitiveTemplate> primitiveTemplates;
        primitiveTemplates.reserve(meshCatalog.size());
        for (const cue::engine_assets::BuiltInMeshDescriptor &descriptor : meshCatalog)
        {
            cue::Result<cue::scene::ComponentInstanceId> primitiveTemplateId =
                cue::scene::ComponentInstanceId::generate(m_session->identity_source(), *m_assertContext);
            if (!primitiveTemplateId)
            {
                m_assertContext->fatal_handler().terminate("Editor Primitive template identity generation failed");
            }
            cue::Result<cue::scene::SceneComponent> primitivePrototype = cue::renderer::make_builtin_mesh_component(
                std::move(*primitiveTemplateId.try_value()), descriptor.assetId, m_session->schema_registry(),
                m_session->value_schema_registry(), *m_assertContext);
            if (!primitivePrototype)
            {
                m_assertContext->fatal_handler().terminate("Editor Primitive template creation failed");
            }
            const bool isEnabled = cue::renderer::is_render_mesh_supported(descriptor.assetId);
            primitiveTemplates.push_back(cue::editor_core::EditorPrimitiveTemplate{
                std::string(descriptor.displayName), std::string(descriptor.assetId),
                std::move(*primitivePrototype.try_value()),
                isEnabled ? std::string{} : "GameView／DebugViewはこのPrimitiveに未対応です。", isEnabled});
        }
        m_presenter = cue::editor::EditorPresenter::create(m_session->controller(), *m_session->active_document_id(),
                                                           m_session->identity_source(), m_session->schema_registry(),
                                                           std::move(componentTemplates), std::move(primitiveTemplates),
                                                           *m_assertContext);
    }

    /// @brief Editor UI確定後にPortable EventをPlay SessionへFIFO順に配送して一Frame進める
    void route_and_advance_runtime() noexcept
    {
        const cue::editor_core::EditorPlaySessionSnapshot playState = m_playPresenter->state_snapshot();
        if (playState.state != cue::editor_core::EditorPlaySessionState::Running)
        {
            m_inputEventCount = 0U;
            m_playPresenter->advance_runtime();
            return;
        }

        const cue::editor::PlayInputRoutingContext context{m_uiInputCapture,
                                                           ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId),
                                                           ImGui::GetIO().WantTextInput,
                                                           m_gameViewRequest.isWindowFocused,
                                                           m_gameViewRequest.isViewportHovered,
                                                           m_gameViewRequest.isViewportActive,
                                                           m_debugViewRequest.isViewportHovered,
                                                           m_debugViewRequest.isViewportActive};
        const cue::editor::PlayInputRoutingResult routing = cue::editor::route_play_input_events(
            std::span<cue::InputEvent>(m_inputEvents.data(), m_inputEventCount), context);
        if (playState.generation == m_inputPlayGeneration)
        {
            for (std::size_t index = 0U; index < routing.eventCount; ++index)
            {
                cue::Result<bool> pushed = m_playController->push_input_event(m_inputEvents[index]);
                if (!pushed)
                {
                    report_error(*pushed.try_error());
                    break;
                }
            }
        }
        m_inputEventCount = 0U;
        m_playPresenter->advance_runtime(routing.capture);
    }

    /// @brief Debug Viewが占有したMouse入力だけをEditor専用Cameraへ適用する
    void update_debug_camera() noexcept
    {
        if (!m_debugCamera.has_value())
        {
            m_debugCameraInput.reset();
            return;
        }

        const cue::editor::DebugViewCameraMotion inputMotion =
            m_debugCameraInput.update(m_inputSnapshot, m_debugViewRequest);
        const cue::renderer::DebugCameraMotion motion{inputMotion.yawDeltaRadians, inputMotion.pitchDeltaRadians,
                                                      inputMotion.rightTranslation, inputMotion.upTranslation,
                                                      inputMotion.forwardTranslation};

        if (motion.yawDeltaRadians == 0.0F && motion.pitchDeltaRadians == 0.0F && motion.rightTranslation == 0.0F &&
            motion.upTranslation == 0.0F && motion.forwardTranslation == 0.0F)
        {
            return;
        }
        cue::Result<void> moved = m_debugCamera->apply_motion(m_assertContext->fatal_handler(), motion);
        if (!moved)
        {
            m_debugCameraInput.reset();
            report_error(*moved.try_error());
        }
    }

    /// @brief Edit中はAuthoring Scene、Play中はRuntime SystemのSnapshotを描画入力へ選択する
    void refresh_render_snapshot() noexcept
    {
        const cue::editor_core::EditorPlaySessionState playState = m_playPresenter->state_snapshot().state;
        if (playState == cue::editor_core::EditorPlaySessionState::Running ||
            playState == cue::editor_core::EditorPlaySessionState::StopRequested)
        {
            return;
        }
        const cue::editor_core::EditorDocument *document = active_document();
        if (document == nullptr || !m_rendererTypeIds.has_value())
        {
            m_authoringRenderSnapshot = cue::renderer::RenderSnapshot{};
            return;
        }
        cue::Result<cue::renderer::RenderSnapshot> snapshot = cue::renderer::extract_render_snapshot(
            document->scene_document(), *m_rendererTypeIds, ++m_authoringSnapshotGeneration, *m_assertContext);
        if (!snapshot)
        {
            m_authoringRenderSnapshot = cue::renderer::RenderSnapshot{};
            return;
        }
        m_authoringRenderSnapshot = std::move(*snapshot.try_value());
    }

    /// @brief 各Persistent Stateを一度だけRecoveryへAtomic保存する
    void autosave_recovery_if_needed() noexcept
    {
        const cue::editor_core::EditorDocument *document = active_document();
        if (document == nullptr)
        {
            m_lastRecoveryDocumentId.reset();
            m_lastRecoveryStateValue.reset();
            return;
        }
        if ((!document->is_dirty() && document->has_saved_destination()) ||
            document->persistence_state() != cue::editor_core::DocumentPersistenceState::Idle)
        {
            return;
        }
        const std::uint64_t documentId = document->id().value();
        const std::uint64_t stateValue = document->current_state_id().value();
        if (m_lastRecoveryDocumentId == documentId && m_lastRecoveryStateValue == stateValue)
        {
            return;
        }
        m_lastRecoveryDocumentId = documentId;
        m_lastRecoveryStateValue = stateValue;
        cue::Result<void> saved = m_session->autosave_active_scene_recovery();
        if (!saved)
        {
            report_error(*saved.try_error());
        }
    }

    /// @brief Project-only状態からScene作成、Open、Recovery、終了操作を描画する
    void draw_project_shell() noexcept
    {
        std::optional<PendingTransition> transition;
        if (cue::editor::begin_editor_dockspace_host())
        {
            if (ImGui::BeginMenuBar())
            {
                if (ImGui::BeginMenu("ファイル"))
                {
                    if (ImGui::MenuItem("新しいScene", "Ctrl+N"))
                    {
                        transition = PendingTransition::NewScene;
                    }
                    if (ImGui::MenuItem("Sceneを開く", "Ctrl+O"))
                    {
                        transition = PendingTransition::OpenScene;
                    }
                    if (ImGui::MenuItem("終了"))
                    {
                        transition = PendingTransition::CloseProject;
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }
            if (!m_message.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      m_hasError ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F) : ImVec4(0.45F, 0.9F, 0.55F, 1.0F));
                ImGui::TextWrapped("%s", m_message.c_str());
                ImGui::PopStyleColor();
            }
        }
        cue::editor::end_editor_dockspace_host();

        cue::editor::dock_editor_window_on_first_use();
        if (ImGui::Begin("Project"))
        {
            ImGui::Text("Project: %s", m_session->project_locator().data());
            ImGui::TextUnformatted("Sceneを作成するか、Source Assets内のSceneを開いてください。");
            if (ImGui::Button("新しいScene"))
            {
                transition = PendingTransition::NewScene;
            }
            ImGui::SameLine();
            if (ImGui::Button("Sceneを開く"))
            {
                transition = PendingTransition::OpenScene;
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Recovery");
            if (ImGui::Button("Recovery候補を更新"))
            {
                refresh_recovery_candidates();
            }
            for (std::size_t index = 0; index < m_recoveryCandidates.size(); ++index)
            {
                const cue::editor_core::RecoveryCandidateInspection &candidate = m_recoveryCandidates[index];
                ImGui::PushID(static_cast<int>(index));
                ImGui::TextUnformatted(candidate.scene_id().c_str());
                if (candidate.try_metadata() != nullptr)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("開く"))
                    {
                        cue::Result<cue::editor_core::EditorDocumentId> opened =
                            m_session->open_recovery_scene(candidate.scene_id());
                        if (!opened)
                        {
                            report_error(*opened.try_error());
                        }
                        else
                        {
                            m_recoveryCandidates.clear();
                            rebuild_presenter();
                        }
                    }
                }
                else if (candidate.try_error() != nullptr)
                {
                    ImGui::SameLine();
                    ImGui::TextUnformatted("破損または未対応");
                }
                ImGui::PopID();
            }
        }
        ImGui::End();
        const bool canUseShortcut =
            !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        if (!transition.has_value() && canUseShortcut && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N))
        {
            transition = PendingTransition::NewScene;
        }
        if (!transition.has_value() && canUseShortcut && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
        {
            transition = PendingTransition::OpenScene;
        }
        if (transition.has_value())
        {
            if (*transition == PendingTransition::CloseProject)
            {
                request_project_close();
            }
            else
            {
                begin_transition(*transition);
            }
        }
    }

    /// @brief PresenterのFile Menu要求をSession Workflowへ変換する
    void handle_workflow_request(cue::editor::EditorWorkflowRequest a_request) noexcept
    {
        switch (a_request)
        {
        case cue::editor::EditorWorkflowRequest::NewScene:
            begin_transition(PendingTransition::NewScene);
            break;
        case cue::editor::EditorWorkflowRequest::OpenScene:
            begin_transition(PendingTransition::OpenScene);
            break;
        case cue::editor::EditorWorkflowRequest::SaveScene:
            static_cast<void>(save_scene());
            break;
        case cue::editor::EditorWorkflowRequest::SaveSceneAs:
            begin_transition(PendingTransition::SaveSceneAs);
            break;
        case cue::editor::EditorWorkflowRequest::ReloadScene:
            begin_transition(PendingTransition::ReloadScene);
            break;
        case cue::editor::EditorWorkflowRequest::CloseProject:
            request_project_close();
            break;
        }
    }

    /// @brief Play、Package Workflow、Buildの実行中確認を順に適用し停止済みの場合だけProject Closeへ進む
    void request_project_close() noexcept
    {
        if (m_pendingTransition != PendingTransition::None || m_shouldClose)
        {
            return;
        }
        if (!m_playPresenter->begin_editor_shutdown())
        {
            return;
        }
        if (m_packagePresenter != nullptr && !m_packagePresenter->begin_editor_shutdown())
        {
            return;
        }
        if (m_buildPresenter != nullptr && !m_buildPresenter->begin_editor_shutdown())
        {
            return;
        }
        begin_transition(PendingTransition::CloseProject);
    }

    /// @brief Scene切替入力またはProject終了前のClose判断を開始する
    void begin_transition(PendingTransition a_transition) noexcept
    {
        if (m_pendingTransition != PendingTransition::None || m_shouldClose)
        {
            return;
        }
        if (a_transition == PendingTransition::NewScene || a_transition == PendingTransition::OpenScene ||
            a_transition == PendingTransition::SaveSceneAs)
        {
            m_locatorMode = a_transition;
            m_sceneLocator.fill('\0');
            const cue::editor_core::EditorDocument *document = active_document();
            const std::string_view initial =
                a_transition == PendingTransition::NewScene
                    ? "Scenes/NewScene.cuescene"
                    : (a_transition == PendingTransition::SaveSceneAs && document != nullptr
                           ? document->scene_locator().text()
                           : "Scenes/Main.cuescene");
            std::copy_n(initial.begin(), std::min(initial.size(), m_sceneLocator.size() - 1U), m_sceneLocator.begin());
            m_openLocatorDialog = true;
            return;
        }
        m_pendingTransition = a_transition;
        if (a_transition == PendingTransition::ReloadScene)
        {
            const cue::editor_core::EditorDocument *document = active_document();
            if (document == nullptr)
            {
                m_pendingTransition = PendingTransition::None;
                return;
            }
            if (document->requires_close_decision())
            {
                m_openCloseDialog = true;
                return;
            }
            perform_reload();
            return;
        }
        cue::Result<cue::editor_core::DocumentCloseState> state = m_session->request_close();
        if (!state)
        {
            report_error(*state.try_error());
            m_pendingTransition = PendingTransition::None;
            return;
        }
        if (*state.try_value() == cue::editor_core::DocumentCloseState::Closed)
        {
            rebuild_presenter();
            complete_transition();
        }
        else
        {
            m_openCloseDialog = true;
        }
    }

    /// @brief 準備済みSceneへの切替またはProject終了を確定する
    void complete_transition() noexcept
    {
        const PendingTransition transition = m_pendingTransition;
        m_pendingTransition = PendingTransition::None;
        m_openCloseDialog = false;
        if (transition == PendingTransition::CloseProject)
        {
            m_shouldClose = true;
            return;
        }
        if (transition == PendingTransition::NewScene || transition == PendingTransition::OpenScene)
        {
            rebuild_presenter();
            report_status(transition == PendingTransition::NewScene ? "新しいSceneを作成しました。"
                                                                    : "Sceneを開きました。");
        }
        else if (transition == PendingTransition::SaveSceneAs)
        {
            m_locatorMode = PendingTransition::None;
            rebuild_presenter();
            report_status("Sceneを別名で保存しました。");
        }
    }

    /// @brief Active Documentを失わないController Reloadを実行する
    void perform_reload() noexcept
    {
        cue::Result<cue::editor_core::DocumentStateId> reloaded = m_session->reload_active_scene();
        m_pendingTransition = PendingTransition::None;
        m_openCloseDialog = false;
        if (!reloaded)
        {
            report_error(*reloaded.try_error());
            return;
        }
        rebuild_presenter();
        report_status("Sceneを再読込しました。");
    }

    /// @brief Scene Locator入力を検証してNew、Open、Save Asへ渡す
    void draw_locator_dialog() noexcept
    {
        if (m_openLocatorDialog)
        {
            ImGui::OpenPopup("Scene Locator");
            m_openLocatorDialog = false;
        }
        if (!ImGui::BeginPopupModal("Scene Locator", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::TextUnformatted("Source Assets Rootからの相対Pathを入力してください。");
        ImGui::InputText("##SceneLocator", m_sceneLocator.data(), m_sceneLocator.size());
        const char *confirmLabel = m_locatorMode == PendingTransition::NewScene
                                       ? "作成"
                                       : (m_locatorMode == PendingTransition::SaveSceneAs ? "保存" : "開く");
        if (ImGui::Button(confirmLabel))
        {
            cue::Result<cue::RelativePath> locator = cue::RelativePath::parse(m_sceneLocator.data(), *m_assertContext);
            if (!locator)
            {
                report_error(*locator.try_error());
            }
            else
            {
                const PendingTransition locatorMode = m_locatorMode;
                if (locatorMode == PendingTransition::SaveSceneAs)
                {
                    m_pendingTransition = PendingTransition::SaveSceneAs;
                    if (save_scene_as(std::move(*locator.try_value()), false))
                    {
                        m_locatorMode = PendingTransition::None;
                        ImGui::CloseCurrentPopup();
                        complete_transition();
                    }
                    else if (m_openUncertainSaveDialog || m_openOverwriteDialog)
                    {
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        m_pendingTransition = PendingTransition::None;
                    }
                    ImGui::EndPopup();
                    return;
                }
                cue::Result<cue::editor_core::EditorDocumentId> prepared =
                    locatorMode == PendingTransition::NewScene
                        ? m_session->prepare_new_scene(std::move(*locator.try_value()))
                        : m_session->prepare_open_scene(std::move(*locator.try_value()));
                if (!prepared)
                {
                    report_error(*prepared.try_error());
                }
                else
                {
                    m_locatorMode = PendingTransition::None;
                    ImGui::CloseCurrentPopup();
                    m_pendingTransition = locatorMode;
                    cue::Result<cue::editor_core::DocumentCloseState> state =
                        m_session->request_activate_prepared_scene();
                    if (!state)
                    {
                        const cue::Error activationError = std::move(*state.try_error());
                        static_cast<void>(m_session->discard_prepared_scene());
                        m_pendingTransition = PendingTransition::None;
                        report_error(activationError);
                    }
                    else if (*state.try_value() == cue::editor_core::DocumentCloseState::Closed)
                    {
                        complete_transition();
                    }
                    else
                    {
                        m_openCloseDialog = true;
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル"))
        {
            m_locatorMode = PendingTransition::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /// @brief Dirty SceneのSave、Discard、Cancel判断を切替種別へ適用する
    void draw_close_dialog() noexcept
    {
        if (m_openCloseDialog)
        {
            ImGui::OpenPopup("未保存の変更");
            m_openCloseDialog = false;
        }
        if (!ImGui::BeginPopupModal("未保存の変更", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::TextUnformatted("Sceneに未保存の変更があります。");
        if (ImGui::Button("保存"))
        {
            bool canSave = true;
            if (m_pendingTransition != PendingTransition::ReloadScene)
            {
                cue::Result<cue::editor_core::DocumentCloseState> state =
                    m_session->respond_to_close(cue::editor_core::CloseDecision::Save);
                if (!state)
                {
                    report_error(*state.try_error());
                    canSave = false;
                }
            }
            if (canSave && save_scene())
            {
                ImGui::CloseCurrentPopup();
                continue_transition_after_save();
            }
            else if (m_openUncertainSaveDialog || m_openOverwriteDialog)
            {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("破棄"))
        {
            if (m_pendingTransition == PendingTransition::ReloadScene)
            {
                ImGui::CloseCurrentPopup();
                perform_reload();
            }
            else
            {
                cue::Result<cue::editor_core::DocumentCloseState> state =
                    m_session->respond_to_close(cue::editor_core::CloseDecision::Discard);
                if (!state)
                {
                    report_error(*state.try_error());
                }
                else
                {
                    ImGui::CloseCurrentPopup();
                    complete_transition();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル"))
        {
            if (m_pendingTransition == PendingTransition::ReloadScene)
            {
                m_pendingTransition = PendingTransition::None;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                cue::Result<cue::editor_core::DocumentCloseState> state =
                    m_session->respond_to_close(cue::editor_core::CloseDecision::Cancel);
                if (!state)
                {
                    report_error(*state.try_error());
                }
                else
                {
                    if (m_session->has_prepared_scene())
                    {
                        cue::Result<void> discarded = m_session->discard_prepared_scene();
                        if (!discarded)
                        {
                            report_error(*discarded.try_error());
                            ImGui::EndPopup();
                            return;
                        }
                    }
                    m_pendingTransition = PendingTransition::None;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndPopup();
    }

    /// @brief 確定Save後に保留中のReload、Scene切替、Project終了を継続する
    void continue_transition_after_save() noexcept
    {
        if (m_pendingTransition == PendingTransition::SaveSceneAs)
        {
            complete_transition();
            return;
        }
        if (m_pendingTransition == PendingTransition::ReloadScene)
        {
            perform_reload();
            return;
        }
        if (m_pendingTransition == PendingTransition::NewScene || m_pendingTransition == PendingTransition::OpenScene)
        {
            if (!m_session->has_prepared_scene())
            {
                complete_transition();
                return;
            }
            if (!restore_open_close_state_after_save())
            {
                return;
            }
            cue::Result<cue::editor_core::DocumentCloseState> state = m_session->request_activate_prepared_scene();
            if (!state)
            {
                report_error(*state.try_error());
            }
            else if (*state.try_value() == cue::editor_core::DocumentCloseState::Closed)
            {
                complete_transition();
            }
            else
            {
                m_openCloseDialog = true;
            }
            return;
        }
        if (m_pendingTransition == PendingTransition::CloseProject)
        {
            if (!restore_open_close_state_after_save())
            {
                return;
            }
            cue::Result<cue::editor_core::DocumentCloseState> state = m_session->request_close();
            if (!state)
            {
                report_error(*state.try_error());
            }
            else if (*state.try_value() == cue::editor_core::DocumentCloseState::Closed)
            {
                complete_transition();
            }
            else
            {
                m_openCloseDialog = true;
            }
        }
    }

    /// @brief Save失敗後の判断待ちをOpenへ戻してCloseを再評価可能にする
    [[nodiscard]] bool restore_open_close_state_after_save() noexcept
    {
        const cue::editor_core::EditorDocument *document = active_document();
        if (document == nullptr || document->close_state() != cue::editor_core::DocumentCloseState::AwaitingDecision)
        {
            return true;
        }
        cue::Result<cue::editor_core::DocumentCloseState> reopened =
            m_session->respond_to_close(cue::editor_core::CloseDecision::Cancel);
        if (!reopened)
        {
            report_error(*reopened.try_error());
            return false;
        }
        return true;
    }

    /// @brief Active Sceneの確定Saveだけを成功として返す
    [[nodiscard]] bool save_scene(bool a_allowExistingDestination = false) noexcept
    {
        cue::Result<cue::scene::SceneSaveOutcome> saved =
            a_allowExistingDestination ? m_session->save_active_scene_overwriting_existing_destination()
                                       : m_session->save_active_scene();
        if (!saved)
        {
            const bool destinationConflict =
                !a_allowExistingDestination && is_new_destination_conflict(*saved.try_error());
            report_error(*saved.try_error());
            m_openUncertainSaveDialog = active_save_is_uncertain();
            m_openOverwriteDialog = destinationConflict && !m_openUncertainSaveDialog;
            return false;
        }
        if (saved.try_value()->status() != cue::scene::SceneSaveStatus::Committed)
        {
            if (saved.try_value()->try_error() != nullptr)
            {
                report_error(*saved.try_value()->try_error());
            }
            m_openUncertainSaveDialog = active_save_is_uncertain();
            return false;
        }
        report_status("Sceneを保存しました。");
        return true;
    }

    /// @brief Active Sceneを別Locatorへ保存し競合とSave Uncertainの判断画面へ接続する
    [[nodiscard]] bool save_scene_as(cue::RelativePath a_locator, bool a_allowExistingDestination) noexcept
    {
        cue::Result<cue::scene::SceneSaveOutcome> saved =
            a_allowExistingDestination ? m_session->save_active_scene_as_overwriting(std::move(a_locator))
                                       : m_session->save_active_scene_as_new(std::move(a_locator));
        if (!saved)
        {
            const bool destinationConflict = !a_allowExistingDestination && is_destination_conflict(*saved.try_error());
            report_error(*saved.try_error());
            m_openUncertainSaveDialog = active_save_is_uncertain();
            m_openOverwriteDialog = destinationConflict && !m_openUncertainSaveDialog;
            return false;
        }
        if (saved.try_value()->status() != cue::scene::SceneSaveStatus::Committed)
        {
            if (saved.try_value()->try_error() != nullptr)
            {
                report_error(*saved.try_value()->try_error());
            }
            m_openUncertainSaveDialog = active_save_is_uncertain();
            return false;
        }
        return true;
    }

    /// @brief 未保存Documentの初回Destination競合か判定する
    [[nodiscard]] bool is_new_destination_conflict(const cue::Error &a_error) const noexcept
    {
        const cue::editor_core::EditorDocument *document = active_document();
        return document != nullptr && !document->has_saved_destination() && is_destination_conflict(a_error);
    }

    /// @brief 保存先Entryの存在または外部変更競合か判定する
    [[nodiscard]] static bool is_destination_conflict(const cue::Error &a_error) noexcept
    {
        return a_error.root_code().domain() == "Cue.EditorCore" &&
               a_error.root_code().value() ==
                   static_cast<std::int64_t>(cue::editor_core::EditorCoreError::ExternalConflict);
    }

    /// @brief Active SceneがSave Uncertain判断待ちか確認する
    [[nodiscard]] bool active_save_is_uncertain() const noexcept
    {
        const cue::editor_core::EditorDocument *document = active_document();
        return document != nullptr &&
               document->persistence_state() == cue::editor_core::DocumentPersistenceState::SaveUncertain;
    }

    /// @brief 初回保存先の既存File置換をUserへ明示確認する
    void draw_overwrite_dialog() noexcept
    {
        if (m_openOverwriteDialog)
        {
            ImGui::OpenPopup("既存Sceneの上書き");
            m_openOverwriteDialog = false;
        }
        if (!ImGui::BeginPopupModal("既存Sceneの上書き", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::TextUnformatted("保存先に既存Fileがあります。");
        ImGui::TextUnformatted("内容を置換して保存しますか？");
        if (ImGui::Button("上書きして保存"))
        {
            bool saved = false;
            if (m_locatorMode == PendingTransition::SaveSceneAs)
            {
                cue::Result<cue::RelativePath> locator =
                    cue::RelativePath::parse(m_sceneLocator.data(), *m_assertContext);
                if (!locator)
                {
                    report_error(*locator.try_error());
                }
                else
                {
                    saved = save_scene_as(std::move(*locator.try_value()), true);
                }
            }
            else
            {
                saved = save_scene(true);
            }
            if (saved)
            {
                ImGui::CloseCurrentPopup();
                continue_transition_after_save();
            }
            else if (m_openUncertainSaveDialog)
            {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル"))
        {
            ImGui::CloseCurrentPopup();
            if (m_pendingTransition == PendingTransition::SaveSceneAs)
            {
                m_pendingTransition = PendingTransition::None;
                m_locatorMode = PendingTransition::None;
            }
            else if (m_pendingTransition != PendingTransition::None)
            {
                m_openCloseDialog = true;
            }
        }
        ImGui::EndPopup();
    }

    /// @brief Save Uncertainを再試行するか記録だけを破棄する判断を描画する
    void draw_uncertain_save_dialog() noexcept
    {
        if (m_openUncertainSaveDialog)
        {
            ImGui::OpenPopup("保存結果を確認");
            m_openUncertainSaveDialog = false;
        }
        if (!ImGui::BeginPopupModal("保存結果を確認", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::TextUnformatted("保存先への発行結果を確定できませんでした。");
        ImGui::TextUnformatted("再検証するか、不確定記録だけを破棄してください。");
        if (ImGui::Button("再検証 / 再試行"))
        {
            cue::Result<cue::scene::SceneSaveStatus> retried = m_session->retry_uncertain_save_active_scene();
            if (!retried)
            {
                report_error(*retried.try_error());
            }
            else if (*retried.try_value() == cue::scene::SceneSaveStatus::Committed)
            {
                ImGui::CloseCurrentPopup();
                report_status("Sceneの保存結果を確認しました。");
                continue_transition_after_save();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("不確定記録を破棄"))
        {
            cue::Result<void> discarded = m_session->discard_uncertain_save_active_scene();
            if (!discarded)
            {
                report_error(*discarded.try_error());
            }
            else
            {
                ImGui::CloseCurrentPopup();
                report_status("保存の不確定記録を破棄しました。Sceneは未保存のままです。");
                if (m_pendingTransition == PendingTransition::SaveSceneAs)
                {
                    m_pendingTransition = PendingTransition::None;
                    m_locatorMode = PendingTransition::None;
                }
                else if (m_pendingTransition != PendingTransition::None)
                {
                    m_openCloseDialog = true;
                }
            }
        }
        ImGui::EndPopup();
    }

    /// @brief Active DocumentをController Sessionから取得する
    [[nodiscard]] const cue::editor_core::EditorDocument *active_document() const noexcept
    {
        if (!m_session->active_document_id().has_value())
        {
            return nullptr;
        }
        return m_session->controller().session().find_document(*m_session->active_document_id());
    }

    /// @brief Recovery Registryを再列挙してProject-only Snapshotへ反映する
    void refresh_recovery_candidates() noexcept
    {
        cue::Result<std::vector<cue::editor_core::RecoveryCandidateInspection>> candidates =
            m_session->list_recovery_candidates();
        if (!candidates)
        {
            report_error(*candidates.try_error());
            return;
        }
        m_recoveryCandidates = std::move(*candidates.try_value());
    }

    /// @brief Workflow ErrorをActive PresenterまたはProject Shellへ表示する
    void report_error(const cue::Error &a_error) noexcept
    {
        if (m_presenter != nullptr)
        {
            m_presenter->report_workflow_error(a_error);
            return;
        }
        try
        {
            m_message = std::string(a_error.summary());
            m_hasError = true;
        }
        catch (...)
        {
            terminate_tool_exception(*m_assertContext);
        }
    }

    /// @brief Workflow成功をActive PresenterまたはProject Shellへ表示する
    void report_status(std::string_view a_status) noexcept
    {
        if (m_presenter != nullptr)
        {
            m_presenter->report_workflow_status(a_status);
            return;
        }
        try
        {
            m_message.assign(a_status);
            m_hasError = false;
        }
        catch (...)
        {
            terminate_tool_exception(*m_assertContext);
        }
    }

    cue::editor::WindowsEditorSession *m_session;
    const cue::AssertContext *m_assertContext;
    cue::schema::SchemaRegistryIdentitySource m_runtimeSchemaIdentitySource;
    cue::game_core::WorldIdentitySource m_worldIdentitySource;
    cue::game_core::SteadyMonotonicClock m_clock;
    std::unique_ptr<cue::schema::SchemaRegistry> m_runtimeSchema;
    cue::renderer::RenderSnapshotStore m_runtimeRenderSnapshot;
    cue::renderer::RendererRuntimeSystemFactory m_rendererSystemFactory;
    std::optional<cue::renderer::RendererSchemaTypeIds> m_rendererTypeIds;
    std::optional<cue::renderer::DebugCamera> m_debugCamera;
    cue::renderer::RenderSnapshot m_authoringRenderSnapshot;
    std::uint64_t m_authoringSnapshotGeneration = 0U;
    std::unique_ptr<cue::editor_core::EditorPlaySessionController> m_playController;
    std::unique_ptr<cue::editor::PlaySessionPresenter> m_playPresenter;
    std::unique_ptr<cue::GameBuildService> m_buildService;
    std::unique_ptr<cue::editor::BuildPresenter> m_buildPresenter;
    std::optional<cue::BuildWorkspaceCompatibility> m_buildWorkspaceCompatibility;
    std::unique_ptr<cue::package::GamePackageWorkflowService> m_packageService;
    std::unique_ptr<cue::editor::PackagePresenter> m_packagePresenter;
    std::unique_ptr<cue::editor::EditorPresenter> m_presenter;
    std::unique_ptr<cue::editor::FilesPresenter> m_filesPresenter;
    cue::editor::GameViewRequest m_gameViewRequest;
    cue::editor::GameViewSurface m_gameViewSurface;
    cue::editor::DebugViewRequest m_debugViewRequest;
    cue::editor::DebugViewSurface m_debugViewSurface;
    cue::editor::DebugViewCameraInput m_debugCameraInput;
    cue::FrameInputSnapshot m_inputSnapshot;
    std::array<cue::InputEvent, cue::InputEventQueue::k_capacity> m_inputEvents = {};
    std::size_t m_inputEventCount = 0U;
    cue::InputCapture m_uiInputCapture;
    std::uint64_t m_inputPlayGeneration = 0U;
    std::vector<cue::editor_core::RecoveryCandidateInspection> m_recoveryCandidates;
    std::array<char, 512> m_sceneLocator{};
    std::string m_message;
    std::string m_buildUnavailableMessage;
    std::optional<std::uint64_t> m_lastRecoveryDocumentId;
    std::optional<std::uint64_t> m_lastRecoveryStateValue;
    PendingTransition m_pendingTransition = PendingTransition::None;
    PendingTransition m_locatorMode = PendingTransition::None;
    bool m_openLocatorDialog = false;
    bool m_openCloseDialog = false;
    bool m_openOverwriteDialog = false;
    bool m_openUncertainSaveDialog = false;
    bool m_shouldClose = false;
    bool m_hasError = false;
};

/// @brief 起動失敗をLoggerへ記録し対応するProcess Exit Codeを返す
[[nodiscard]] int report_error(cue::Logger &a_logger, std::string_view a_summary, cue::Error a_error,
                               int a_exitCode) noexcept
{
    static_cast<void>(a_logger.log(cue::LogLevel::Error, a_summary, std::move(a_error)));
    static_cast<void>(a_logger.flush());
    return a_exitCode;
}

/// @brief Process Test専用Actionを実Editor Session内で実行する
[[nodiscard]] cue::Result<void> apply_process_test_action(const std::optional<std::string> &a_action,
                                                          cue::editor::WindowsEditorSession &a_session,
                                                          const cue::AssertContext &a_assertContext) noexcept
{
    if (!a_action.has_value())
    {
        return cue::Result<void>::success();
    }
    if (!a_session.active_document_id().has_value())
    {
        return cue::Result<void>::failure(
            make_tool_error(a_assertContext, k_processTestFailed, "Process test action requires an active scene"));
    }
    if (*a_action == "play-repeated-workflow" || process_test_build_configuration(*a_action).has_value() ||
        process_test_package_configuration(*a_action).has_value() || is_process_test_shipping_workflow(*a_action))
    {
        return cue::Result<void>::success();
    }
    if (*a_action == "files-workflow")
    {
        cue::editor::FilesPresenter presenter(a_session.files_workspace(), a_assertContext);
        /// @brief 一つのFiles操作をPresentation Adapter経由で同期実行する
        const auto submit =
            [&presenter](cue::editor::FilesIntentKind a_kind, std::string a_source, std::string a_destination) noexcept
        { return presenter.submit({a_kind, std::move(a_source), std::move(a_destination)}).has_value(); };
        if (!submit(cue::editor::FilesIntentKind::CreateFolder, {}, "FilesWorkflow") ||
            !submit(cue::editor::FilesIntentKind::CreateEmptyFile, {}, "Draft.txt") ||
            !submit(cue::editor::FilesIntentKind::Rename, "Draft.txt", "Renamed.txt") ||
            !submit(cue::editor::FilesIntentKind::Move, "Renamed.txt", "FilesWorkflow/Moved.txt") ||
            !submit(cue::editor::FilesIntentKind::Copy, "FilesWorkflow/Moved.txt", "Copy.txt") ||
            !submit(cue::editor::FilesIntentKind::Delete, "Copy.txt", {}))
        {
            return cue::Result<void>::failure(
                make_tool_error(a_assertContext, k_processTestFailed, "Files process workflow operation failed"));
        }
        const std::span<const cue::project_files::RecoveryEntry> recovery =
            a_session.files_workspace().view_model().recovery_entries();
        if (recovery.empty())
        {
            return cue::Result<void>::failure(make_tool_error(
                a_assertContext, k_processTestFailed, "Files process workflow did not publish a recovery entry"));
        }
        const std::string operationId = recovery.front().operationId;
        if (!submit(cue::editor::FilesIntentKind::Restore, operationId, {}))
        {
            return cue::Result<void>::failure(
                make_tool_error(a_assertContext, k_processTestFailed, "Files process workflow restore failed"));
        }
        return cue::Result<void>::success();
    }
    if (*a_action == "autosave-new-scene")
    {
        cue::Result<cue::editor_core::DocumentCloseState> closed = a_session.request_close();
        if (!closed || *closed.try_value() != cue::editor_core::DocumentCloseState::Closed)
        {
            return cue::Result<void>::failure(
                closed ? make_tool_error(a_assertContext, k_processTestFailed,
                                         "Process test could not close the clean initial scene")
                       : std::move(*closed.try_error()));
        }
        cue::Result<cue::RelativePath> locator =
            cue::RelativePath::parse("Scenes/Child-Unedited.cuescene", a_assertContext);
        if (!locator)
        {
            return cue::Result<void>::failure(std::move(*locator.try_error()));
        }
        cue::Result<cue::editor_core::EditorDocumentId> created =
            a_session.create_scene(std::move(*locator.try_value()));
        if (!created)
        {
            return cue::Result<void>::failure(std::move(*created.try_error()));
        }
        const cue::editor_core::EditorDocument *document =
            a_session.controller().session().find_document(*created.try_value());
        if (document == nullptr || document->is_dirty() || document->has_saved_destination())
        {
            return cue::Result<void>::failure(make_tool_error(
                a_assertContext, k_processTestFailed, "Process test new scene did not remain clean and unsaved"));
        }
        return cue::Result<void>::success();
    }
    cue::editor_core::AddObjectIntent addObject{
        std::nullopt, *a_action == "autosave-recovery" ? "Child Process Recovery" : "Child Process Saved"};
    cue::Result<void> edited = a_session.controller().execute_intent(
        *a_session.active_document_id(), std::move(addObject), a_session.identity_source(), {});
    if (!edited)
    {
        return cue::Result<void>::failure(std::move(*edited.try_error()));
    }
    if (*a_action == "autosave-recovery")
    {
        return cue::Result<void>::success();
    }

    cue::Result<cue::editor_core::DocumentCloseState> state = a_session.request_close();
    if (!state)
    {
        return cue::Result<void>::failure(std::move(*state.try_error()));
    }
    if (*state.try_value() != cue::editor_core::DocumentCloseState::AwaitingDecision)
    {
        return cue::Result<void>::failure(make_tool_error(a_assertContext, k_processTestFailed,
                                                          "Process test edit did not require a close decision"));
    }
    state = a_session.respond_to_close(cue::editor_core::CloseDecision::Save);
    if (!state)
    {
        return cue::Result<void>::failure(std::move(*state.try_error()));
    }
    cue::Result<cue::scene::SceneSaveOutcome> saved = a_session.save_active_scene();
    if (!saved)
    {
        return cue::Result<void>::failure(std::move(*saved.try_error()));
    }
    if (saved.try_value()->status() != cue::scene::SceneSaveStatus::Committed ||
        a_session.active_document_id().has_value())
    {
        return cue::Result<void>::failure(make_tool_error(
            a_assertContext, k_processTestFailed, "Process test close save did not commit and close the scene"));
    }
    return cue::Result<void>::success();
}

/// @brief Editor SessionとTool Hostを寿命順に構築してUI Loopを実行する
[[nodiscard]] int run(EditorToolOptions a_options, cue::Logger &a_logger,
                      cue::editor::EditorSessionLogRouter &a_logRouter,
                      const cue::AssertContext &a_assertContext) noexcept
{
    cue::EngineVersion engineVersion{1U, 0U, 0U};
    std::optional<cue::distribution::WindowsInstalledVersionExecutionLease> installedEngineLease;
    if (a_options.hasEngineInstallRoot)
    {
        auto adoptedLease = cue::distribution::adopt_windows_inherited_version_execution_lease(
            a_options.installedEngineRequest, a_assertContext);
        if (!adoptedLease)
        {
            return report_error(a_logger, "Installed Engine lease adoption failed",
                                std::move(*adoptedLease.try_error()), k_sessionInitializationFailed);
        }
        const std::optional<cue::EngineVersion> installedVersion =
            parse_engine_version(adoptedLease.try_value()->engine_version());
        if (!installedVersion)
        {
            return report_error(a_logger, "Installed Engine version is invalid",
                                make_tool_error(a_assertContext, k_sessionInitializationFailed,
                                                "Validated Installed Engine Version is not canonical"),
                                k_sessionInitializationFailed);
        }
        engineVersion = *installedVersion;
        installedEngineLease.emplace(std::move(*adoptedLease.try_value()));
    }
    cue::Result<cue::editor::WindowsEditorEngineConfiguration> configuration =
        make_engine_configuration(engineVersion, a_assertContext);
    if (!configuration)
    {
        return report_error(a_logger, "Editor engine configuration failed", std::move(*configuration.try_error()),
                            k_sessionInitializationFailed);
    }
    cue::Result<std::unique_ptr<cue::editor::WindowsEditorSession>> session = cue::editor::WindowsEditorSession::create(
        std::move(a_options.parameters), std::move(*configuration.try_value()), a_assertContext);
    if (!session)
    {
        return report_error(a_logger, "Editor project session failed", std::move(*session.try_error()),
                            k_sessionInitializationFailed);
    }
    cue::Result<void> processTest =
        apply_process_test_action(a_options.processTestAction, **session.try_value(), a_assertContext);
    if (!processTest)
    {
        return report_error(a_logger, "Editor process test action failed", std::move(*processTest.try_error()),
                            k_processTestFailed);
    }
    const bool isPlayWorkflowTest =
        a_options.processTestAction.has_value() && *a_options.processTestAction == "play-repeated-workflow";
    const std::optional<cue::BuildConfiguration> buildWorkflowConfiguration =
        a_options.processTestAction ? process_test_build_configuration(*a_options.processTestAction) : std::nullopt;
    const std::optional<cue::BuildConfiguration> packageWorkflowConfiguration =
        a_options.processTestAction ? process_test_package_configuration(*a_options.processTestAction) : std::nullopt;
    const bool isShippingWorkflowTest =
        a_options.processTestAction.has_value() && is_process_test_shipping_workflow(*a_options.processTestAction);
    PlayWorkflowProbe playWorkflowProbe;
    PlayWorkflowSystemFactory playWorkflowFactory(playWorkflowProbe);
    const std::array<const cue::runtime::RuntimeSystemFactory *, 1U> playWorkflowFactories{&playWorkflowFactory};
    std::span<const cue::runtime::RuntimeSystemFactory *const> systemFactories;
    if (isPlayWorkflowTest)
    {
        systemFactories = playWorkflowFactories;
    }
    {
        EditorToolClient client(**session.try_value(), a_logger, a_logRouter, systemFactories,
                                installedEngineLease ? &*installedEngineLease : nullptr, a_assertContext);
        if (buildWorkflowConfiguration)
        {
            cue::Result<void> workflow = client.run_build_workflow_process_test(*buildWorkflowConfiguration);
            if (!workflow)
            {
                return report_error(a_logger, "Editor Build process workflow failed", std::move(*workflow.try_error()),
                                    k_processTestFailed);
            }
        }
        if (packageWorkflowConfiguration)
        {
            cue::Result<void> workflow = client.run_package_workflow_process_test(*packageWorkflowConfiguration);
            if (!workflow)
            {
                return report_error(a_logger, "Editor Package process workflow failed",
                                    std::move(*workflow.try_error()), k_processTestFailed);
            }
        }
        if (isShippingWorkflowTest)
        {
            cue::Result<void> workflow = client.run_shipping_workflow_process_test();
            if (!workflow)
            {
                return report_error(a_logger, "Editor Shipping process workflow failed",
                                    std::move(*workflow.try_error()), k_processTestFailed);
            }
        }
        if (isPlayWorkflowTest)
        {
            cue::Result<void> workflow = client.run_play_workflow_process_test(playWorkflowProbe);
            if (!workflow)
            {
                return report_error(a_logger, "Editor Play process workflow failed", std::move(*workflow.try_error()),
                                    k_processTestFailed);
            }
        }
        const cue::tool_host::ToolHostDescriptor descriptor{"CueEngine Editor",
                                                            {1440U, 900U},
                                                            a_options.maximumFrameCount,
                                                            static_cast<std::uint16_t>(IDI_CUE_EDITOR_TOOL)};
        cue::Result<void> hosted = cue::tool_host::run_windows_d3d12_tool_host(descriptor, client, a_assertContext);
        if (!hosted)
        {
            return report_error(a_logger, "Editor Tool Host failed", std::move(*hosted.try_error()), k_toolHostFailed);
        }
    }
    if (isPlayWorkflowTest && (playWorkflowProbe.activeSystemCount != 0U ||
                               playWorkflowProbe.startedSystemCount != playWorkflowProbe.stoppedSystemCount ||
                               playWorkflowProbe.createdSystemCount != playWorkflowProbe.destroyedSystemCount))
    {
        return report_error(a_logger, "Editor close left a child Play session",
                            make_tool_error(a_assertContext, k_processTestFailed,
                                            "Editor destruction did not release every started Runtime system"),
                            k_processTestFailed);
    }
    return 0;
}
} // namespace

/// @brief Project Hub起動値をEditor Sessionへ変換してProcess Exit Codeを返す
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    cue::AbortFatalHandler fatalHandler;
    try
    {
        std::vector<std::unique_ptr<cue::LogSink>> sinks;
        sinks.push_back(std::make_unique<cue::ConsoleLogSink>());
        std::unique_ptr<cue::editor::EditorSessionLogRouter> logRouter =
            std::make_unique<cue::editor::EditorSessionLogRouter>();
        cue::editor::EditorSessionLogRouter *logRouterReference = logRouter.get();
        sinks.push_back(std::move(logRouter));
        cue::Logger logger(fatalHandler, std::move(sinks));
        cue::AssertContext assertContext(logger, fatalHandler);
        cue::Result<EditorToolOptions> options = parse_options(a_argumentCount, a_arguments, assertContext);
        if (!options)
        {
            std::fputws(L"Usage: CueEditorTool --protocol-version <version> --project-descriptor <absolute path> "
                        L"--expected-project-id <uuid> --engine-compatibility-id <id> [--initial-scene <path>] "
                        L"[--engine-install-root <absolute path> --engine-version-directory <name> "
                        L"--engine-bundle-id <uuid> --engine-manifest-digest <sha256> "
                        L"--engine-execution-lease-handle <handle>] "
                        L"[--maximum-frame-count <count>]\n",
                        stderr);
            return report_error(logger, "Editor command line is invalid", std::move(*options.try_error()),
                                k_invalidArguments);
        }
        return run(std::move(*options.try_value()), logger, *logRouterReference, assertContext);
    }
    catch (...)
    {
        fatalHandler.terminate("Editor Tool allocation failed");
    }
}
