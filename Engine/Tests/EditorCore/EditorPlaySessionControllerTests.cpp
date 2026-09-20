#include <Cue/EditorCore/EditorController.h>
#include <Cue/EditorCore/EditorPlaySessionController.h>
#include <Cue/EditorCore/Error.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/RuntimeSystem.h>
#include <Cue/GameCore/World.h>
#include <Cue/Input/FrameInputSnapshot.h>
#include <Cue/Math/Transform.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Runtime/RuntimeSystemFactory.h>
#include <Cue/Scene/Serialization.h>
#include <Cue/Schema/Descriptor.h>
#include <Cue/Schema/Registry.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_transformTypeId = "50000000-0000-4000-8000-000000000005";
constexpr std::string_view k_sceneObjectStateTypeId = "10000000-0000-4000-8000-000000000001";
constexpr std::string_view k_sceneAssetId = "70000000-0000-4000-8000-000000000214";
constexpr std::string_view k_objectId = "80000000-0000-4000-8000-000000000214";

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の通常FatalをProcess失敗へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(77);
    }

    /// @brief Test中のEmergency FatalをProcess失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(78);
    }
};

class TestClock final : public cue::game_core::MonotonicClock
{
  public:
    /// @brief Testごとに独立した単調時刻を0から生成する
    TestClock() noexcept = default;

    /// @brief 次のSampleだけを回復可能Errorにする
    void fail_next_sample() noexcept
    {
        m_shouldFail = true;
    }

    /// @brief 固定16msずつ進み指定時だけStart Rollback用失敗を返す
    [[nodiscard]] cue::Result<cue::game_core::MonotonicClockSample> sample(
        const cue::AssertContext &a_assertContext) noexcept override
    {
        if (m_shouldFail)
        {
            m_shouldFail = false;
            cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.EditorCore.PlayTest", 1);
            return cue::Result<cue::game_core::MonotonicClockSample>::failure(
                cue::Error::create(a_assertContext.fatal_handler(), std::move(code), "Injected Play clock failure"));
        }

        cue::game_core::MonotonicClockSample sample{m_nanoseconds};
        m_nanoseconds += 16'000'000;
        return cue::Result<cue::game_core::MonotonicClockSample>::success(std::move(sample));
    }

  private:
    std::int64_t m_nanoseconds = 0;
    bool m_shouldFail = false;
};

struct StopRetryProbe final
{
    std::size_t stopCount = 0U;
    std::size_t failuresRemaining = 1U;
};

class RetryStopSystem final : public cue::game_core::RuntimeSystem
{
  public:
    /// @brief 再Cleanup回数を外部Probeへ記録するTest Systemを生成する
    RetryStopSystem(StopRetryProbe &a_probe, const cue::AssertContext &a_assertContext) noexcept
        : m_probe(&a_probe), m_assertContext(&a_assertContext)
    {
    }

    /// @brief 副作用なしでSystem Startを完了する
    [[nodiscard]] cue::Result<void> start(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief Frame中はTest Probeを変更せず成功する
    [[nodiscard]] cue::Result<void> update(const cue::game_core::RuntimeSystemUpdateContext &) noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief 最初のCleanupだけ失敗し次の呼出しで完了する
    [[nodiscard]] cue::Result<void> stop(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        ++m_probe->stopCount;
        if (m_probe->failuresRemaining == 0U)
        {
            return cue::Result<void>::success();
        }

        --m_probe->failuresRemaining;
        cue::ErrorCode code = cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.EditorCore.PlayTest", 2);
        return cue::Result<void>::failure(
            cue::Error::create(m_assertContext->fatal_handler(), std::move(code), "Injected Play stop failure"));
    }

  private:
    StopRetryProbe *m_probe;
    const cue::AssertContext *m_assertContext;
};

class RetryStopSystemFactory final : public cue::runtime::RuntimeSystemFactory
{
  public:
    /// @brief 各Playへ独立Systemを生成するTest FactoryをProject Scope Probeへ結び付ける
    explicit RetryStopSystemFactory(StopRetryProbe &a_probe) noexcept : m_probe(&a_probe)
    {
    }

    /// @brief 一つの再Cleanup検証Systemと不変Descriptorを生成する
    [[nodiscard]] cue::Result<cue::runtime::RuntimeSystemRegistration> create_system(
        const cue::AssertContext &a_assertContext) const noexcept override
    {
        cue::runtime::RuntimeSystemRegistration registration{
            {"Editor.Play.StopRetry", cue::game_core::RuntimeUpdatePhase::Update, 0, {}},
            std::make_unique<RetryStopSystem>(*m_probe, a_assertContext)};
        return cue::Result<cue::runtime::RuntimeSystemRegistration>::success(std::move(registration));
    }

  private:
    StopRetryProbe *m_probe;
};

struct InputFrameProbe final
{
    std::array<bool, 3U> keyDown = {};
    std::array<bool, 3U> keyReleased = {};
    std::array<bool, 3U> keyboardCaptured = {};
    std::size_t frameCount = 0U;
};

class InputProbeSystem final : public cue::game_core::RuntimeSystem
{
  public:
    /// @brief Sessionを跨ぐ入力状態を観測するTest Systemを生成する
    explicit InputProbeSystem(InputFrameProbe &a_probe) noexcept : m_probe(&a_probe)
    {
    }

    /// @brief 入力観測前に副作用なしで開始する
    [[nodiscard]] cue::Result<void> start(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief Runtime Systemが実際に受け取ったFrame Inputを値で記録する
    [[nodiscard]] cue::Result<void> update(
        const cue::game_core::RuntimeSystemUpdateContext &a_context) noexcept override
    {
        if (m_probe->frameCount < m_probe->keyDown.size())
        {
            const std::size_t index = m_probe->frameCount;
            m_probe->keyDown[index] = a_context.input.is_key_down(cue::InputKey::A);
            m_probe->keyReleased[index] = a_context.input.was_key_released(cue::InputKey::A);
            m_probe->keyboardCaptured[index] = a_context.input.is_keyboard_captured();
        }
        ++m_probe->frameCount;
        return cue::Result<void>::success();
    }

    /// @brief 入力観測後に副作用なしで停止する
    [[nodiscard]] cue::Result<void> stop(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        return cue::Result<void>::success();
    }

  private:
    InputFrameProbe *m_probe;
};

class InputProbeSystemFactory final : public cue::runtime::RuntimeSystemFactory
{
  public:
    /// @brief Play世代ごとに独立Systemを生成するProbeを保持する
    explicit InputProbeSystemFactory(InputFrameProbe &a_probe) noexcept : m_probe(&a_probe)
    {
    }

    /// @brief Input観測SystemをRuntimeへ登録する
    [[nodiscard]] cue::Result<cue::runtime::RuntimeSystemRegistration> create_system(
        const cue::AssertContext &) const noexcept override
    {
        cue::runtime::RuntimeSystemRegistration registration{
            {"Editor.Play.InputProbe", cue::game_core::RuntimeUpdatePhase::Update, 0, {}},
            std::make_unique<InputProbeSystem>(*m_probe)};
        return cue::Result<cue::runtime::RuntimeSystemRegistration>::success(std::move(registration));
    }

  private:
    InputFrameProbe *m_probe;
};

/// @brief 条件が偽ならEditor Play Session Testを失敗終了する
void require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::fprintf(stderr, "EditorPlaySessionControllerTests failed at line %u\n", a_location.line());
        std::_Exit(2);
    }
}

/// @brief Resultが失敗ならEditor Play Session Testを失敗終了する
template <typename T> void require(const cue::Result<T> &a_result) noexcept
{
    require(a_result.has_value());
}

/// @brief 成功Resultから所有Valueを取り出す
template <typename T> [[nodiscard]] T take_value(cue::Result<T> &&a_result) noexcept
{
    require(a_result);
    return std::move(*a_result.try_value());
}

/// @brief Canonical UUIDからTest用TypeIdを生成する
[[nodiscard]] cue::schema::TypeId make_type_id(std::string_view a_text,
                                               const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::TypeId::parse(a_text, a_assertContext));
}

/// @brief Fieldを持たないTest用Type Descriptorを生成する
[[nodiscard]] cue::schema::TypeDescriptor make_type_descriptor(std::string_view a_typeId, std::string_view a_name,
                                                               const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::schema::FieldDescriptor> fields;
    std::vector<cue::schema::FieldId> reserved;
    return take_value(
        cue::schema::create_type_descriptor(make_type_id(a_typeId, a_assertContext), a_name,
                                            take_value(cue::schema::SchemaVersion::create(1U, a_assertContext)),
                                            std::move(fields), std::move(reserved), a_assertContext));
}

/// @brief TransformとSceneObjectStateを登録したSeal済みRegistryを生成する
[[nodiscard]] std::unique_ptr<cue::schema::SchemaRegistry> make_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    require(builder.add_type(make_type_descriptor(k_transformTypeId, "Cue.Core.Transform", a_assertContext)));
    require(builder.add_type(
        make_type_descriptor(k_sceneObjectStateTypeId, "Cue.Scene.SceneObjectState", a_assertContext)));
    return take_value(builder.seal());
}

/// @brief 固定IdentityからEditor Play Test用Project Descriptorを生成する
[[nodiscard]] cue::ProjectDescriptor make_project_descriptor(const cue::AssertContext &a_assertContext) noexcept
{
    cue::ProjectId projectId =
        take_value(cue::ProjectId::parse("00000000-0000-4000-8000-000000000214", a_assertContext));
    return take_value(
        cue::create_blank_project_descriptor(std::move(projectId), "Editor Play Test",
                                             cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, std::nullopt},
                                             "00000000-0000-4000-8000-000000000099", a_assertContext));
}

/// @brief 一Objectを持つ検証済みAuthoring Sceneを生成する
[[nodiscard]] cue::scene::SceneDocument make_scene_document(const cue::AssertContext &a_assertContext) noexcept
{
    cue::scene::SceneAssetId sceneId = take_value(cue::scene::SceneAssetId::parse(k_sceneAssetId, a_assertContext));
    cue::scene::SceneDocument document = cue::scene::SceneDocument::create(std::move(sceneId), a_assertContext);
    cue::scene::ObjectId objectId = take_value(cue::scene::ObjectId::parse(k_objectId, a_assertContext));
    require(document.add_object(objectId, "Play Source", true, std::nullopt, cue::math::Transform{}));
    return document;
}

/// @brief 指定WorkspaceとProcess Scope依存からTest用Play Controllerを生成する
[[nodiscard]] std::unique_ptr<cue::editor_core::EditorPlaySessionController> make_play_controller(
    const cue::editor_core::ProjectWorkspaceSession &a_workspaceSession,
    cue::game_core::WorldIdentitySource &a_worldIdentitySource, TestClock &a_clock,
    const cue::schema::SchemaRegistry &a_schemaRegistry, std::uint64_t a_firstGeneration,
    const cue::AssertContext &a_assertContext,
    std::span<const cue::runtime::RuntimeSystemFactory *const> a_systemFactories = {}) noexcept
{
    return take_value(cue::editor_core::EditorPlaySessionController::create(
        a_workspaceSession, a_worldIdentitySource, a_clock, a_schemaRegistry, a_systemFactories,
        make_type_id(k_transformTypeId, a_assertContext), make_type_id(k_sceneObjectStateTypeId, a_assertContext),
        a_firstGeneration, 100'000'000, a_assertContext));
}

/// @brief Capture解放とPlay再開始後のSession-local Input初期化をSystem入力で検証する
void test_input_capture_and_replay(const cue::schema::SchemaRegistry &a_schemaRegistry,
                                   cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                                   const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto editor = cue::editor_core::EditorController::create(make_project_descriptor(a_assertContext), a_assertContext);
    cue::RelativePath locator = take_value(cue::RelativePath::parse("Scenes/InputReplay.cuescene", a_assertContext));
    const cue::editor_core::EditorDocumentId documentId =
        take_value(editor->open_document(make_scene_document(a_assertContext), std::move(locator), true));
    InputFrameProbe probe;
    InputProbeSystemFactory factory(probe);
    const std::array<const cue::runtime::RuntimeSystemFactory *, 1U> factories{&factory};
    auto play = make_play_controller(editor->session(), a_worldIdentitySource, clock, a_schemaRegistry, 500U,
                                     a_assertContext, factories);

    require(play->start(documentId));
    require(take_value(play->push_input_event({cue::InputEventType::KeyDown, cue::InputKey::A})));
    require(play->advance_frame({}));
    require(probe.frameCount == 1U && probe.keyDown[0] && !probe.keyboardCaptured[0]);
    require(play->advance_frame({true, false}));
    require(probe.frameCount == 2U && !probe.keyDown[1] && probe.keyReleased[1] && probe.keyboardCaptured[1]);
    require(play->stop());

    require(play->start(documentId));
    require(play->advance_frame({}));
    require(probe.frameCount == 3U && !probe.keyDown[2] && !probe.keyReleased[2] && !probe.keyboardCaptured[2]);
    require(play->stop());
}

/// @brief Dirty Authoring状態を変えず12回のPlay／Stopで独立Worldを生成できることを検証する
void test_dirty_document_repeated_play(const cue::schema::SchemaRegistry &a_schemaRegistry,
                                       cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                                       const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto editor = cue::editor_core::EditorController::create(make_project_descriptor(a_assertContext), a_assertContext);
    cue::scene::SceneDocument scene = make_scene_document(a_assertContext);
    cue::scene::SceneAssetId sceneId = scene.scene_asset_id();
    cue::scene::ObjectId objectId = take_value(cue::scene::ObjectId::parse(k_objectId, a_assertContext));
    cue::RelativePath locator = take_value(cue::RelativePath::parse("Scenes/DirtyPlay.cuescene", a_assertContext));
    cue::editor_core::EditorDocumentId documentId =
        take_value(editor->open_document(std::move(scene), std::move(locator), true));
    require(editor
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, sceneId, cue::editor_core::RenameObjectCommand{objectId, "Dirty Play Source"}})
                .has_value());
    const std::array selection{objectId};
    require(editor->set_selection(documentId, selection, &objectId));

    const cue::editor_core::EditorDocument *document = editor->session().find_document(documentId);
    require(document != nullptr && document->is_dirty());
    const cue::editor_core::DocumentStateId currentState = document->current_state_id();
    const cue::editor_core::DocumentStateId savedState = document->saved_state_id();
    const std::size_t historyEntries = document->history_entry_count();
    const std::string authoringBefore =
        take_value(cue::scene::serialize_scene_document(document->scene_document(), a_assertContext));

    auto play =
        make_play_controller(editor->session(), a_worldIdentitySource, clock, a_schemaRegistry, 100U, a_assertContext);
    require(play->state_snapshot().state == cue::editor_core::EditorPlaySessionState::Idle);
    cue::Result<void> idleFrame = play->advance_frame({});
    require(!idleFrame && idleFrame.try_error()->code().value() ==
                              static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidPlayState));
    cue::Result<void> idleStopRequest = play->request_stop();
    require(!idleStopRequest && idleStopRequest.try_error()->code().value() ==
                                    static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidPlayState));
    std::vector<std::uint64_t> worldIds;
    for (std::uint64_t cycle = 0U; cycle < 12U; ++cycle)
    {
        require(play->start(documentId));
        cue::editor_core::EditorPlaySessionSnapshot running = play->state_snapshot();
        require(running.state == cue::editor_core::EditorPlaySessionState::Running);
        require(running.documentId.has_value() && running.documentId.value() == documentId);
        require(running.generation == 100U + cycle && running.worldId != 0U && running.frameCount == 0U);
        require(!running.hasFailure);
        for (std::uint64_t existing : worldIds)
        {
            require(existing != running.worldId);
        }
        worldIds.push_back(running.worldId);

        cue::Result<void> duplicateStart = play->start(documentId);
        require(!duplicateStart && duplicateStart.try_error()->code().value() ==
                                       static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidPlayState));
        require(play->state_snapshot().generation == running.generation);

        require(take_value(play->push_input_event({cue::InputEventType::KeyDown, cue::InputKey::A})));
        require(play->advance_frame({false, false}));
        require(play->state_snapshot().frameCount == 1U);
        require(play->request_stop());
        require(play->state_snapshot().state == cue::editor_core::EditorPlaySessionState::StopRequested);
        require(play->stop());
        require(play->stop());

        cue::editor_core::EditorPlaySessionSnapshot stopped = play->state_snapshot();
        require(stopped.state == cue::editor_core::EditorPlaySessionState::Stopped);
        require(stopped.generation == 100U + cycle && stopped.frameCount == 1U && !stopped.hasFailure);
        document = editor->session().find_document(documentId);
        require(document != nullptr && document->is_dirty());
        require(document->current_state_id() == currentState && document->saved_state_id() == savedState);
        require(document->history_entry_count() == historyEntries);
        require(document->selection().size() == 1U && document->selection().front() == objectId);
        require(take_value(cue::scene::serialize_scene_document(document->scene_document(), a_assertContext)) ==
                authoringBefore);
    }

    cue::Result<bool> stoppedInput = play->push_input_event({cue::InputEventType::KeyUp, cue::InputKey::A});
    require(!stoppedInput && stoppedInput.try_error()->code().value() ==
                                 static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidPlayState));
}

/// @brief Runtime Start Rollback完了後に新しいGenerationで再Playできることを検証する
void test_start_failure_replay(const cue::schema::SchemaRegistry &a_schemaRegistry,
                               cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                               const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto editor = cue::editor_core::EditorController::create(make_project_descriptor(a_assertContext), a_assertContext);
    cue::RelativePath locator =
        take_value(cue::RelativePath::parse("Scenes/ReplayAfterFailure.cuescene", a_assertContext));
    cue::editor_core::EditorDocumentId documentId =
        take_value(editor->open_document(make_scene_document(a_assertContext), std::move(locator), true));
    auto play =
        make_play_controller(editor->session(), a_worldIdentitySource, clock, a_schemaRegistry, 200U, a_assertContext);

    cue::Result<void> missing = play->start(cue::editor_core::EditorDocumentId(999U));
    require(!missing && missing.try_error()->code().value() ==
                            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::DocumentNotFound));
    require(play->state_snapshot().state == cue::editor_core::EditorPlaySessionState::Idle);

    clock.fail_next_sample();
    cue::Result<void> failed = play->start(documentId);
    require(!failed && failed.try_error()->root_code().domain() == "Cue.EditorCore.PlayTest");
    cue::editor_core::EditorPlaySessionSnapshot rolledBack = play->state_snapshot();
    require(rolledBack.state == cue::editor_core::EditorPlaySessionState::Stopped);
    require(rolledBack.generation == 200U && rolledBack.worldId != 0U && rolledBack.hasFailure);
    require(play->stop());

    require(play->start(documentId));
    require(play->state_snapshot().generation == 201U);
    require(play->advance_frame({}));
    require(play->stop());
    require(play->state_snapshot().state == cue::editor_core::EditorPlaySessionState::Stopped);
}

/// @brief Snapshot作成後にSource DocumentをCloseしてもRuntimeが独立して停止できることを検証する
void test_document_close_isolation(const cue::schema::SchemaRegistry &a_schemaRegistry,
                                   cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                                   const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto editor = cue::editor_core::EditorController::create(make_project_descriptor(a_assertContext), a_assertContext);
    cue::RelativePath locator =
        take_value(cue::RelativePath::parse("Scenes/CloseDuringPlay.cuescene", a_assertContext));
    cue::editor_core::EditorDocumentId documentId =
        take_value(editor->open_document(make_scene_document(a_assertContext), std::move(locator), true));
    auto play =
        make_play_controller(editor->session(), a_worldIdentitySource, clock, a_schemaRegistry, 300U, a_assertContext);

    require(play->start(documentId));
    require(take_value(editor->request_close(documentId)) == cue::editor_core::DocumentCloseState::Closed);
    require(editor->session().find_document(documentId) == nullptr);
    require(play->advance_frame({}));
    require(play->stop());
    cue::editor_core::EditorPlaySessionSnapshot stopped = play->state_snapshot();
    require(stopped.state == cue::editor_core::EditorPlaySessionState::Stopped);
    require(stopped.documentId.has_value() && stopped.documentId.value() == documentId);
}

/// @brief CleanupFailedでSession所有を保持し再Stop後に新しいPlayへ進めることを検証する
void test_cleanup_failure_retry(const cue::schema::SchemaRegistry &a_schemaRegistry,
                                cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                                const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto editor = cue::editor_core::EditorController::create(make_project_descriptor(a_assertContext), a_assertContext);
    cue::RelativePath locator = take_value(cue::RelativePath::parse("Scenes/CleanupRetry.cuescene", a_assertContext));
    cue::editor_core::EditorDocumentId documentId =
        take_value(editor->open_document(make_scene_document(a_assertContext), std::move(locator), true));
    StopRetryProbe probe;
    RetryStopSystemFactory factory(probe);
    const std::array<const cue::runtime::RuntimeSystemFactory *, 1U> factories{&factory};
    auto play = make_play_controller(editor->session(), a_worldIdentitySource, clock, a_schemaRegistry, 400U,
                                     a_assertContext, factories);

    require(play->start(documentId));
    require(play->request_stop());
    cue::Result<void> firstStop = play->stop();
    require(!firstStop && firstStop.try_error()->code().value() ==
                              static_cast<std::int64_t>(cue::runtime::RuntimeError::ApplicationSessionCleanupFailed));
    require(probe.stopCount == 1U);
    require(play->state_snapshot().state == cue::editor_core::EditorPlaySessionState::CleanupFailed);
    require(play->state_snapshot().hasFailure);

    cue::Result<void> blockedStart = play->start(documentId);
    require(!blockedStart && blockedStart.try_error()->code().value() ==
                                 static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidPlayState));
    cue::Result<void> completedCleanup = play->stop();
    require(!completedCleanup && completedCleanup.try_error()->root_code().value() == 2);
    require(probe.stopCount == 2U);
    require(play->state_snapshot().state == cue::editor_core::EditorPlaySessionState::Stopped);
    require(play->stop());

    require(play->start(documentId));
    require(play->state_snapshot().generation == 401U);
    require(play->stop());
    require(probe.stopCount == 3U);
}

/// @brief 最大GenerationのSession終了後にOverflowを検出して新しいWorldを作らないことを検証する
void test_generation_exhaustion(const cue::schema::SchemaRegistry &a_schemaRegistry,
                                cue::game_core::WorldIdentitySource &a_worldIdentitySource,
                                const cue::AssertContext &a_assertContext) noexcept
{
    TestClock clock;
    auto editor = cue::editor_core::EditorController::create(make_project_descriptor(a_assertContext), a_assertContext);
    cue::RelativePath locator = take_value(cue::RelativePath::parse("Scenes/Generation.cuescene", a_assertContext));
    cue::editor_core::EditorDocumentId documentId =
        take_value(editor->open_document(make_scene_document(a_assertContext), std::move(locator), true));
    auto play = make_play_controller(editor->session(), a_worldIdentitySource, clock, a_schemaRegistry,
                                     std::numeric_limits<std::uint64_t>::max(), a_assertContext);

    require(play->start(documentId));
    require(play->state_snapshot().generation == std::numeric_limits<std::uint64_t>::max());
    require(play->stop());
    cue::Result<void> exhausted = play->start(documentId);
    require(!exhausted && exhausted.try_error()->code().value() ==
                              static_cast<std::int64_t>(cue::editor_core::EditorCoreError::PlayGenerationExhausted));
}
} // namespace

/// @brief Editor Play ControllerのDocument分離、Rollback、反復Session契約をUIなしで検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource schemaIdentitySource;
    std::unique_ptr<cue::schema::SchemaRegistry> registry = make_registry(schemaIdentitySource, assertContext);
    cue::game_core::WorldIdentitySource worldIdentitySource;

    test_dirty_document_repeated_play(*registry, worldIdentitySource, assertContext);
    test_input_capture_and_replay(*registry, worldIdentitySource, assertContext);
    test_start_failure_replay(*registry, worldIdentitySource, assertContext);
    test_document_close_isolation(*registry, worldIdentitySource, assertContext);
    test_cleanup_failure_retry(*registry, worldIdentitySource, assertContext);
    test_generation_exhaustion(*registry, worldIdentitySource, assertContext);
    return 0;
}
