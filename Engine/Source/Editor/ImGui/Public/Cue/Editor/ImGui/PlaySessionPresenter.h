#pragma once

#include <Cue/Editor/ImGui/SessionLog.h>
#include <Cue/EditorCore/EditorDocument.h>
#include <Cue/EditorCore/EditorPlaySessionController.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
class Error;
class Logger;
} // namespace cue

namespace cue::editor
{
/// @brief Play ToolbarからEditor Play Controllerへ送る意味Command
enum class EditorPlaySessionCommand : std::uint8_t
{
    Play,
    Stop
};

/// @brief Play中のEditor終了確認へ適用するUser判断
enum class EditorPlayShutdownDecision : std::uint8_t
{
    StopAndClose,
    Cancel
};

/// @brief Editor Play Controllerの値SnapshotをToolbar、状態、Consoleへ変換するImGui Adapter
///
/// RuntimeWorldやRuntime Entityを参照せず、ControllerのOwner Threadだけで全操作を実行する
class PlaySessionPresenter final
{
  public:
    /// @brief FactoryだけがPresenter Constructorへ渡せる生成権限
    class ConstructionKey final
    {
      public:
        /// @brief Factory内部で生成権限を値として複製する
        ConstructionKey(const ConstructionKey &) noexcept = default;
        /// @brief Factory内部で生成権限を値として複製代入する
        ConstructionKey &operator=(const ConstructionKey &) noexcept = default;
        /// @brief Factory内部で生成権限を値として移動する
        ConstructionKey(ConstructionKey &&) noexcept = default;
        /// @brief Factory内部で生成権限を値として移動代入する
        ConstructionKey &operator=(ConstructionKey &&) noexcept = default;
        /// @brief bit_castによる権限生成を防ぐnon-trivialな破棄を行う
        ~ConstructionKey() noexcept
        {
        }

      private:
        friend class PlaySessionPresenter;

        /// @brief PlaySessionPresenter Factoryだけに生成権限を発行する
        ConstructionKey() noexcept = default;
    };

    /// @brief 注入参照とSession限定Log購読を所有して初期Presentation Stateを作る
    PlaySessionPresenter(ConstructionKey, editor_core::EditorPlaySessionController &a_controller, Logger &a_logger,
                         EditorSessionLogRouter &a_logRouter, const AssertContext &a_assertContext) noexcept;
    /// @brief Presenter Stateと購読Tokenの複製を禁止する
    PlaySessionPresenter(const PlaySessionPresenter &) = delete;
    /// @brief Presenter Stateと購読Tokenの複製代入を禁止する
    PlaySessionPresenter &operator=(const PlaySessionPresenter &) = delete;
    /// @brief ImGui Session中のAddress安定性を保つためMove構築を禁止する
    PlaySessionPresenter(PlaySessionPresenter &&) = delete;
    /// @brief ImGui Session中のAddress安定性を保つためMove代入を禁止する
    PlaySessionPresenter &operator=(PlaySessionPresenter &&) = delete;
    /// @brief Play停止とSession Log購読解除が完了済みであることを確認してPresentation Stateを破棄する
    ~PlaySessionPresenter() noexcept;

    /// @brief Play ControllerとProcess Log RouterをSession限定購読を作るPresentation Ownerへ束ねる
    [[nodiscard]] static std::unique_ptr<PlaySessionPresenter> create(
        editor_core::EditorPlaySessionController &a_controller, Logger &a_logger, EditorSessionLogRouter &a_logRouter,
        const AssertContext &a_assertContext) noexcept;

    /// @brief 次のPlay対象となるActive Editor Document Identityだけを更新する
    void set_active_document(std::optional<editor_core::EditorDocumentId> a_documentId) noexcept;
    /// @brief ImGui FrameのPlay／Stop ShortcutをRuntime更新前に意味Commandへ変換する
    void process_shortcuts() noexcept;
    /// @brief Running Sessionを一Frame進め、StopRequestedなら安全な停止を完了する
    void advance_runtime(InputCapture a_capture = {}) noexcept;
    /// @brief Play／Stop Toolbar、Session状態、Error、Console、終了確認を描画する
    void draw() noexcept;

    /// @brief Commandが現在有効な場合だけControllerへ送信する
    /// @return Commandを受理した場合にtrueを返す
    [[nodiscard]] bool submit(EditorPlaySessionCommand a_command) noexcept;
    /// @brief Play中なら終了確認を開始し、停止済みなら即時終了可能を返す
    [[nodiscard]] bool begin_editor_shutdown() noexcept;
    /// @brief 終了確認判断を適用し、安全に停止できた場合だけ終了可能を返す
    [[nodiscard]] bool respond_to_editor_shutdown(EditorPlayShutdownDecision a_decision) noexcept;
    /// @brief UIで確定した終了可能通知を一度だけ返す
    [[nodiscard]] bool take_shutdown_ready() noexcept;

    /// @brief Active DocumentとSession StateからPlay操作が有効か返す
    [[nodiscard]] bool can_play() const noexcept;
    /// @brief RunningまたはCleanupFailed StateでStop操作が有効か返す
    [[nodiscard]] bool can_stop() const noexcept;
    /// @brief Editor終了確認がUser判断待ちか返す
    [[nodiscard]] bool is_shutdown_confirmation_pending() const noexcept;
    /// @brief Pointer非保持の現在Session状態Snapshotを返す
    [[nodiscard]] editor_core::EditorPlaySessionSnapshot state_snapshot() const noexcept;
    /// @brief 最後のPlay操作に対応する日本語Messageを返す
    [[nodiscard]] std::string_view message() const noexcept;
    /// @brief 現在Messageが回復可能なRuntime失敗を表すか返す
    [[nodiscard]] bool has_error_message() const noexcept;

  private:
    /// @brief 次のPlay開始直前にSession限定Log購読を作成する
    void begin_log_capture() noexcept;
    /// @brief Session停止後にLog値をPresentationへ複製して購読を同期解除する
    void end_log_capture() noexcept;
    /// @brief Active SessionへStop要求を送り、必要ならCleanup再試行を行う
    [[nodiscard]] bool stop_immediately() noexcept;
    /// @brief Runtime Errorを日本語UIと開発者向けProcess Logへ同時反映する
    void report_runtime_error(Error a_error, std::string_view a_operation) noexcept;
    /// @brief 正常操作の短い日本語MessageをUIとProcess Logへ反映する
    void report_status(std::string_view a_status, std::string_view a_logMessage) noexcept;
    /// @brief Toolbarと現在Session Identityを描画する
    void draw_toolbar() noexcept;
    /// @brief Filter、Clear、Copyを持つSession Log Consoleを描画する
    void draw_console();
    /// @brief Play中のEditor終了確認とCancel経路を描画する
    void draw_shutdown_confirmation() noexcept;

    editor_core::EditorPlaySessionController *m_controller;
    Logger *m_logger;
    EditorSessionLogRouter *m_logRouter;
    const AssertContext *m_assertContext;
    std::unique_ptr<EditorSessionLogSubscription> m_logSubscription;
    std::vector<EditorSessionLogEntry> m_retainedLogEntries;
    std::optional<editor_core::EditorDocumentId> m_activeDocumentId;
    std::array<char, 128> m_filter{};
    std::string m_message;
    bool m_shouldFocusWindow = false;
    bool m_hasError = false;
    bool m_openShutdownConfirmation = false;
    bool m_isShutdownConfirmationPending = false;
    bool m_isShutdownReady = false;
};
} // namespace cue::editor
