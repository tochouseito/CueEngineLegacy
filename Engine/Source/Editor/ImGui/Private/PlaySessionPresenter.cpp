#include <Cue/Editor/ImGui/PlaySessionPresenter.h>

#include <Cue/EditorCore/Error.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Runtime/Error.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <utility>

#include <imgui.h>

namespace
{
constexpr std::size_t k_maximumRetainedLogEntries = 4096U;
constexpr float k_runtimeWindowWidthRatio = 0.4F;
constexpr float k_runtimeWindowMinimumWidth = 360.0F;
constexpr float k_runtimeWindowMaximumWidth = 640.0F;
constexpr float k_runtimeWindowHeight = 360.0F;
constexpr float k_runtimeWindowTop = 60.0F;
constexpr float k_runtimeWindowRightMargin = 20.0F;

/// @brief Runtime WindowをFiles初期位置と分離し表示領域内へ収めるSizeを返す
[[nodiscard]] ImVec2 runtime_window_size(ImVec2 a_displaySize) noexcept
{
    const float preferredWidth = std::clamp(a_displaySize.x * k_runtimeWindowWidthRatio,
                                            k_runtimeWindowMinimumWidth, k_runtimeWindowMaximumWidth);
    const float availableWidth = std::max(1.0F, a_displaySize.x - (k_runtimeWindowRightMargin * 2.0F));
    const float availableHeight = std::max(1.0F, a_displaySize.y - k_runtimeWindowTop);
    return ImVec2(std::min(preferredWidth, availableWidth), std::min(k_runtimeWindowHeight, availableHeight));
}

/// @brief Runtime Windowを右上へ配置しEditor Shell背面への完全重複を避ける位置を返す
[[nodiscard]] ImVec2 runtime_window_position(ImVec2 a_displaySize, ImVec2 a_windowSize) noexcept
{
    return ImVec2(std::max(0.0F, a_displaySize.x - a_windowSize.x - k_runtimeWindowRightMargin),
                  std::min(k_runtimeWindowTop, std::max(0.0F, a_displaySize.y - a_windowSize.y)));
}

/// @brief Session Stateを利用者向けの短い日本語へ変換する
[[nodiscard]] const char *state_label(cue::editor_core::EditorPlaySessionState a_state) noexcept
{
    switch (a_state)
    {
    case cue::editor_core::EditorPlaySessionState::Idle:
        return "未実行";
    case cue::editor_core::EditorPlaySessionState::Running:
        return "実行中";
    case cue::editor_core::EditorPlaySessionState::StopRequested:
        return "停止処理中";
    case cue::editor_core::EditorPlaySessionState::CleanupFailed:
        return "停止の再試行待ち";
    case cue::editor_core::EditorPlaySessionState::Stopped:
        return "停止済み";
    }
    return "不明";
}

/// @brief Log LevelをConsole行の固定Prefixへ変換する
[[nodiscard]] const char *level_label(cue::LogLevel a_level) noexcept
{
    switch (a_level)
    {
    case cue::LogLevel::Trace:
        return "Trace";
    case cue::LogLevel::Debug:
        return "Debug";
    case cue::LogLevel::Info:
        return "Info";
    case cue::LogLevel::Warning:
        return "Warning";
    case cue::LogLevel::Error:
        return "Error";
    case cue::LogLevel::Fatal:
        return "Fatal";
    }
    return "Unknown";
}

/// @brief Log Levelに応じたConsole文字色を返す
[[nodiscard]] ImVec4 level_color(cue::LogLevel a_level) noexcept
{
    switch (a_level)
    {
    case cue::LogLevel::Warning:
        return ImVec4(1.0F, 0.8F, 0.3F, 1.0F);
    case cue::LogLevel::Error:
    case cue::LogLevel::Fatal:
        return ImVec4(1.0F, 0.35F, 0.35F, 1.0F);
    case cue::LogLevel::Trace:
    case cue::LogLevel::Debug:
        return ImVec4(0.65F, 0.7F, 0.75F, 1.0F);
    case cue::LogLevel::Info:
        return ImVec4(0.85F, 0.88F, 0.92F, 1.0F);
    }
    return ImVec4(0.85F, 0.88F, 0.92F, 1.0F);
}

/// @brief ASCII文字だけをLocale非依存の小文字へ変換する
[[nodiscard]] char ascii_lower(char a_value) noexcept
{
    const unsigned char value = static_cast<unsigned char>(a_value);
    if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z'))
    {
        return static_cast<char>(value + static_cast<unsigned char>('a' - 'A'));
    }
    return static_cast<char>(value);
}

/// @brief 二つのASCII文字をLocale非依存でCase-insensitive比較する
[[nodiscard]] bool ascii_equal(char a_left, char a_right) noexcept
{
    return ascii_lower(a_left) == ascii_lower(a_right);
}

/// @brief UTF-8 Byte列を壊さずASCII部分だけCase-insensitive比較する
[[nodiscard]] bool matches_filter(std::string_view a_text, std::string_view a_filter) noexcept
{
    if (a_filter.empty())
    {
        return true;
    }
    return std::search(a_text.begin(), a_text.end(), a_filter.begin(), a_filter.end(), ascii_equal) != a_text.end();
}

/// @brief Stable Error CategoryをRuntime英語文言に依存しない日本語UIへ変換する
[[nodiscard]] std::string_view localized_error(const cue::Error &a_error) noexcept
{
    const cue::ErrorCode &code = a_error.root_code();
    if (code.domain() == "Cue.EditorCore")
    {
        switch (static_cast<cue::editor_core::EditorCoreError>(code.value()))
        {
        case cue::editor_core::EditorCoreError::DocumentNotFound:
            return "Play対象のSceneが見つかりません。";
        case cue::editor_core::EditorCoreError::InvalidPlayConfiguration:
            return "Runtimeの構成が無効です。";
        case cue::editor_core::EditorCoreError::InvalidPlayState:
            return "現在の実行状態ではその操作を行えません。";
        case cue::editor_core::EditorCoreError::PlayGenerationExhausted:
            return "Play Sessionの上限に達しました。Editorを再起動してください。";
        default:
            break;
        }
    }
    if (code.domain() == "Cue.Runtime")
    {
        switch (static_cast<cue::runtime::RuntimeError>(code.value()))
        {
        case cue::runtime::RuntimeError::ApplicationSessionCleanupFailed:
        case cue::runtime::RuntimeError::RuntimeWorldShutdownFailed:
            return "Runtimeの停止処理を完了できませんでした。再試行してください。";
        case cue::runtime::RuntimeError::ApplicationSessionUpdateFailed:
            return "ゲームの更新に失敗しました。停止処理へ移行します。";
        default:
            return "ゲームの起動に失敗しました。詳細はConsoleを確認してください。";
        }
    }
    return "ゲームの起動または停止に失敗しました。詳細はConsoleを確認してください。";
}

/// @brief ImGui描画中の予期しない例外を既存Fatal境界へ渡す
[[noreturn]] void terminate_presenter_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Editor Play Session presentation failed unexpectedly");
    std::abort();
}
} // namespace

namespace cue::editor
{
std::unique_ptr<PlaySessionPresenter> PlaySessionPresenter::create(
    editor_core::EditorPlaySessionController &a_controller, Logger &a_logger, EditorSessionLogRouter &a_logRouter,
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        return std::make_unique<PlaySessionPresenter>(ConstructionKey{}, a_controller, a_logger, a_logRouter,
                                                      a_assertContext);
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Editor Play Session Presenter allocation failed");
    }
    std::terminate();
}

PlaySessionPresenter::PlaySessionPresenter(ConstructionKey, editor_core::EditorPlaySessionController &a_controller,
                                           Logger &a_logger, EditorSessionLogRouter &a_logRouter,
                                           const AssertContext &a_assertContext) noexcept
    : m_controller(&a_controller), m_logger(&a_logger), m_logRouter(&a_logRouter), m_assertContext(&a_assertContext)
{
}

PlaySessionPresenter::~PlaySessionPresenter() noexcept
{
    const editor_core::EditorPlaySessionState state = m_controller->state_snapshot().state;
    if (m_logSubscription != nullptr ||
        (state != editor_core::EditorPlaySessionState::Idle && state != editor_core::EditorPlaySessionState::Stopped))
    {
        m_assertContext->fatal_handler().terminate(
            "Editor Play Session Presenter destruction requires stopped Runtime and detached Log capture");
    }
}

void PlaySessionPresenter::set_active_document(std::optional<editor_core::EditorDocumentId> a_documentId) noexcept
{
    if (m_activeDocumentId != a_documentId)
    {
        m_shouldFocusWindow = a_documentId.has_value();
    }
    m_activeDocumentId = a_documentId;
}

void PlaySessionPresenter::process_shortcuts() noexcept
{
    try
    {
        const bool canUseKeyboard =
            !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        if (canUseKeyboard && ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F5, ImGuiInputFlags_RouteGlobal))
        {
            static_cast<void>(submit(EditorPlaySessionCommand::Stop));
        }
        else if (canUseKeyboard && ImGui::Shortcut(ImGuiKey_F5, ImGuiInputFlags_RouteGlobal))
        {
            static_cast<void>(submit(EditorPlaySessionCommand::Play));
        }
    }
    catch (...)
    {
        terminate_presenter_exception(*m_assertContext);
    }
}

void PlaySessionPresenter::advance_runtime(InputCapture a_capture) noexcept
{
    const editor_core::EditorPlaySessionSnapshot snapshot = m_controller->state_snapshot();
    if (snapshot.state == editor_core::EditorPlaySessionState::Running)
    {
        Result<void> advanced = m_controller->advance_frame(a_capture);
        if (!advanced)
        {
            report_runtime_error(std::move(*advanced.try_error()), "Editor Play frame update failed");
        }
        return;
    }
    if (snapshot.state == editor_core::EditorPlaySessionState::StopRequested)
    {
        static_cast<void>(stop_immediately());
    }
}

void PlaySessionPresenter::draw() noexcept
{
    try
    {
        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        const ImVec2 windowSize = runtime_window_size(displaySize);
        ImGui::SetNextWindowPos(runtime_window_position(displaySize, windowSize), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(windowSize, ImGuiCond_FirstUseEver);
        if (m_shouldFocusWindow)
        {
            ImGui::SetNextWindowFocus();
            m_shouldFocusWindow = false;
        }
        if (ImGui::Begin("Runtime"))
        {
            draw_toolbar();
            ImGui::Separator();
            draw_console();
        }
        ImGui::End();
        draw_shutdown_confirmation();
    }
    catch (...)
    {
        terminate_presenter_exception(*m_assertContext);
    }
}

bool PlaySessionPresenter::submit(EditorPlaySessionCommand a_command) noexcept
{
    if (a_command == EditorPlaySessionCommand::Play)
    {
        if (!can_play())
        {
            return false;
        }
        begin_log_capture();
        Result<void> started = m_controller->start(*m_activeDocumentId);
        const editor_core::EditorPlaySessionSnapshot snapshot = m_controller->state_snapshot();
        if (snapshot.generation != 0U)
        {
            m_logSubscription->set_session_generation(snapshot.generation);
        }
        if (!started)
        {
            report_runtime_error(std::move(*started.try_error()), "Editor Play start failed");
            if (snapshot.state == editor_core::EditorPlaySessionState::Idle ||
                snapshot.state == editor_core::EditorPlaySessionState::Stopped)
            {
                end_log_capture();
            }
            return true;
        }
        report_status("ゲームを開始しました。", "Editor Play started");
        return true;
    }

    if (!can_stop())
    {
        return false;
    }
    if (m_controller->state_snapshot().state == editor_core::EditorPlaySessionState::CleanupFailed)
    {
        static_cast<void>(stop_immediately());
        return true;
    }
    Result<void> requested = m_controller->request_stop();
    if (!requested)
    {
        report_runtime_error(std::move(*requested.try_error()), "Editor Play stop request failed");
        return true;
    }
    report_status("ゲームの停止処理を開始しています。", "Editor Play stop requested");
    return true;
}

bool PlaySessionPresenter::begin_editor_shutdown() noexcept
{
    const editor_core::EditorPlaySessionState state = m_controller->state_snapshot().state;
    if (state == editor_core::EditorPlaySessionState::Idle || state == editor_core::EditorPlaySessionState::Stopped)
    {
        if (m_logSubscription != nullptr)
        {
            end_log_capture();
        }
        return true;
    }
    if (!m_isShutdownConfirmationPending)
    {
        m_isShutdownConfirmationPending = true;
        m_openShutdownConfirmation = true;
    }
    return false;
}

bool PlaySessionPresenter::respond_to_editor_shutdown(EditorPlayShutdownDecision a_decision) noexcept
{
    if (!m_isShutdownConfirmationPending)
    {
        return false;
    }
    if (a_decision == EditorPlayShutdownDecision::Cancel)
    {
        m_isShutdownConfirmationPending = false;
        report_status("Editorの終了をキャンセルしました。", "Editor shutdown cancelled during Play");
        return false;
    }

    const bool isStopped = stop_immediately();
    if (isStopped)
    {
        m_isShutdownConfirmationPending = false;
    }
    return isStopped;
}

bool PlaySessionPresenter::take_shutdown_ready() noexcept
{
    const bool isReady = m_isShutdownReady;
    m_isShutdownReady = false;
    return isReady;
}

bool PlaySessionPresenter::can_play() const noexcept
{
    const editor_core::EditorPlaySessionState state = m_controller->state_snapshot().state;
    return m_activeDocumentId.has_value() && (state == editor_core::EditorPlaySessionState::Idle ||
                                              state == editor_core::EditorPlaySessionState::Stopped);
}

bool PlaySessionPresenter::can_stop() const noexcept
{
    const editor_core::EditorPlaySessionState state = m_controller->state_snapshot().state;
    return state == editor_core::EditorPlaySessionState::Running ||
           state == editor_core::EditorPlaySessionState::CleanupFailed;
}

bool PlaySessionPresenter::is_shutdown_confirmation_pending() const noexcept
{
    return m_isShutdownConfirmationPending;
}

editor_core::EditorPlaySessionSnapshot PlaySessionPresenter::state_snapshot() const noexcept
{
    return m_controller->state_snapshot();
}

std::string_view PlaySessionPresenter::message() const noexcept
{
    return m_message;
}

bool PlaySessionPresenter::has_error_message() const noexcept
{
    return m_hasError;
}

void PlaySessionPresenter::begin_log_capture() noexcept
{
    if (m_logSubscription != nullptr)
    {
        m_assertContext->fatal_handler().terminate("Editor Play Log capture is already active");
    }
    Result<std::unique_ptr<EditorSessionLogSubscription>> subscription = m_logRouter->subscribe(*m_assertContext);
    if (!subscription)
    {
        report_fatal(*m_logger, m_assertContext->fatal_handler(), "Editor Play Log capture could not start",
                     std::move(*subscription.try_error()));
    }
    m_logSubscription = std::move(*subscription.try_value());
}

void PlaySessionPresenter::end_log_capture() noexcept
{
    if (m_logSubscription == nullptr)
    {
        return;
    }
    try
    {
        std::vector<EditorSessionLogEntry> completed = m_logSubscription->snapshot({});
        if (completed.size() >= k_maximumRetainedLogEntries)
        {
            m_retainedLogEntries.assign(std::make_move_iterator(completed.end() - k_maximumRetainedLogEntries),
                                        std::make_move_iterator(completed.end()));
            m_logSubscription.reset();
            return;
        }
        const std::size_t combinedSize = m_retainedLogEntries.size() + completed.size();
        if (combinedSize > k_maximumRetainedLogEntries)
        {
            const std::size_t removeCount = combinedSize - k_maximumRetainedLogEntries;
            m_retainedLogEntries.erase(m_retainedLogEntries.begin(), m_retainedLogEntries.begin() + removeCount);
        }
        m_retainedLogEntries.insert(m_retainedLogEntries.end(), std::make_move_iterator(completed.begin()),
                                    std::make_move_iterator(completed.end()));
        m_logSubscription.reset();
    }
    catch (...)
    {
        terminate_presenter_exception(*m_assertContext);
    }
}

bool PlaySessionPresenter::stop_immediately() noexcept
{
    editor_core::EditorPlaySessionState state = m_controller->state_snapshot().state;
    if (state == editor_core::EditorPlaySessionState::Idle || state == editor_core::EditorPlaySessionState::Stopped)
    {
        end_log_capture();
        return true;
    }
    if (state == editor_core::EditorPlaySessionState::Running)
    {
        Result<void> requested = m_controller->request_stop();
        if (!requested)
        {
            report_runtime_error(std::move(*requested.try_error()), "Editor Play shutdown stop request failed");
            return false;
        }
        state = m_controller->state_snapshot().state;
    }
    if (state == editor_core::EditorPlaySessionState::StopRequested ||
        state == editor_core::EditorPlaySessionState::CleanupFailed)
    {
        Result<void> stopped = m_controller->stop();
        const bool isStopped = m_controller->state_snapshot().state == editor_core::EditorPlaySessionState::Stopped;
        if (!stopped)
        {
            report_runtime_error(std::move(*stopped.try_error()), "Editor Play stop failed");
            if (isStopped)
            {
                end_log_capture();
            }
            return isStopped;
        }
        report_status("ゲームを停止しました。", "Editor Play stopped");
        end_log_capture();
        return true;
    }
    return false;
}

void PlaySessionPresenter::report_runtime_error(Error a_error, std::string_view a_operation) noexcept
{
    try
    {
        m_message.assign(localized_error(a_error));
        m_hasError = true;
        static_cast<void>(m_logger->log(LogLevel::Error, a_operation, std::move(a_error)));
    }
    catch (...)
    {
        terminate_presenter_exception(*m_assertContext);
    }
}

void PlaySessionPresenter::report_status(std::string_view a_status, std::string_view a_logMessage) noexcept
{
    try
    {
        m_message.assign(a_status);
        m_hasError = false;
        static_cast<void>(m_logger->log(LogLevel::Info, a_logMessage));
    }
    catch (...)
    {
        terminate_presenter_exception(*m_assertContext);
    }
}

void PlaySessionPresenter::draw_toolbar() noexcept
{
    ImGui::BeginDisabled(!can_play());
    if (ImGui::Button("Play (F5)"))
    {
        static_cast<void>(submit(EditorPlaySessionCommand::Play));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_stop());
    const bool isCleanupFailed =
        m_controller->state_snapshot().state == editor_core::EditorPlaySessionState::CleanupFailed;
    if (ImGui::Button(isCleanupFailed ? "停止を再試行" : "Stop (Shift+F5)"))
    {
        static_cast<void>(submit(EditorPlaySessionCommand::Stop));
    }
    ImGui::EndDisabled();

    const editor_core::EditorPlaySessionSnapshot snapshot = m_controller->state_snapshot();
    ImGui::Text("状態: %s", state_label(snapshot.state));
    if (snapshot.generation != 0U)
    {
        ImGui::Text("Session: %llu  World: %llu  Frame: %llu", static_cast<unsigned long long>(snapshot.generation),
                    static_cast<unsigned long long>(snapshot.worldId),
                    static_cast<unsigned long long>(snapshot.frameCount));
    }
    if (!m_message.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              m_hasError ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F) : ImVec4(0.45F, 0.9F, 0.55F, 1.0F));
        ImGui::TextWrapped("%s", m_message.c_str());
        ImGui::PopStyleColor();
    }
}

void PlaySessionPresenter::draw_console()
{
    ImGui::InputTextWithHint("##RuntimeLogFilter", "Console Filter", m_filter.data(), m_filter.size());
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        m_retainedLogEntries.clear();
        if (m_logSubscription != nullptr)
        {
            m_logSubscription->clear();
        }
    }
    std::vector<EditorSessionLogEntry> entries;
    entries.reserve(m_retainedLogEntries.size());
    for (const EditorSessionLogEntry &entry : m_retainedLogEntries)
    {
        if (matches_filter(entry.message, m_filter.data()))
        {
            entries.push_back(entry);
        }
    }
    if (m_logSubscription != nullptr)
    {
        std::vector<EditorSessionLogEntry> active = m_logSubscription->snapshot(m_filter.data());
        entries.insert(entries.end(), std::make_move_iterator(active.begin()), std::make_move_iterator(active.end()));
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy"))
    {
        std::string copied;
        for (const EditorSessionLogEntry &entry : entries)
        {
            if (entry.sessionGeneration != 0U)
            {
                copied.append("[Session ");
                copied.append(std::to_string(entry.sessionGeneration));
                copied.append("] ");
            }
            copied.push_back('[');
            copied.append(level_label(entry.level));
            copied.append("] ");
            copied.append(entry.message);
            copied.push_back('\n');
        }
        ImGui::SetClipboardText(copied.c_str());
    }

    if (ImGui::BeginChild("RuntimeConsoleEntries", ImVec2(0.0F, 220.0F), true))
    {
        for (const EditorSessionLogEntry &entry : entries)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, level_color(entry.level));
            if (entry.sessionGeneration == 0U)
            {
                ImGui::Text("[%s]", level_label(entry.level));
            }
            else
            {
                ImGui::Text("[Session %llu] [%s]", static_cast<unsigned long long>(entry.sessionGeneration),
                            level_label(entry.level));
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted(entry.message.data(), entry.message.data() + entry.message.size());
        }
    }
    ImGui::EndChild();
}

void PlaySessionPresenter::draw_shutdown_confirmation() noexcept
{
    if (m_openShutdownConfirmation)
    {
        ImGui::OpenPopup("Play中のEditor終了");
        m_openShutdownConfirmation = false;
    }
    if (!ImGui::BeginPopupModal("Play中のEditor終了", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    ImGui::TextUnformatted("ゲームを停止してEditorを終了しますか？");
    if (m_controller->state_snapshot().state == editor_core::EditorPlaySessionState::CleanupFailed)
    {
        ImGui::TextUnformatted("停止処理が未完了です。再試行できます。");
    }
    if (ImGui::Button("停止して終了"))
    {
        if (respond_to_editor_shutdown(EditorPlayShutdownDecision::StopAndClose))
        {
            m_isShutdownReady = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル"))
    {
        static_cast<void>(respond_to_editor_shutdown(EditorPlayShutdownDecision::Cancel));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
} // namespace cue::editor
