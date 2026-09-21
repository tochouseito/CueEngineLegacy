#include <Cue/RuntimeHost/RuntimeHostApplication.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/World.h>
#include <Cue/Input/Windows/WindowsInputMessageSink.h>
#include <Cue/Platform/Windows/WindowsMessageSink.h>
#include <Cue/Renderer/RendererRuntimeSystem.h>
#include <Cue/Renderer/RendererSchema.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Runtime/RuntimeSchema.h>
#include <Cue/RuntimeHost/GameModuleQueryProvider.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Schema/Registry.h>

#include <exception>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_startupSceneAssetId = "70000000-0000-4000-8000-000000000001";
constexpr std::uint64_t k_sessionGeneration = 1U;
constexpr std::int64_t k_maxDeltaNanoseconds = 100'000'000;

/// @brief Standalone RuntimeのCore／Renderer Typeを持つ不変Registryを生成する
[[nodiscard]] cue::Result<std::unique_ptr<cue::schema::SchemaRegistry>> make_schema_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    cue::Result<void> added = cue::runtime::add_runtime_schema_types(builder, a_assertContext);
    if (!added)
    {
        return cue::Result<std::unique_ptr<cue::schema::SchemaRegistry>>::failure(std::move(*added.try_error()));
    }
    cue::Result<void> rendererAdded = cue::renderer::add_renderer_schema_types(builder, a_assertContext);
    if (!rendererAdded)
    {
        return cue::Result<std::unique_ptr<cue::schema::SchemaRegistry>>::failure(
            std::move(*rendererAdded.try_error()));
    }
    return builder.seal();
}

/// @brief M16のPackage発見まで使用する固定空Sceneを独立Snapshotへ変換する
[[nodiscard]] cue::Result<cue::scene::SceneSnapshot> make_startup_scene(
    const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<cue::scene::SceneAssetId> sceneId =
        cue::scene::SceneAssetId::parse(k_startupSceneAssetId, a_assertContext);
    if (!sceneId)
    {
        return cue::Result<cue::scene::SceneSnapshot>::failure(std::move(*sceneId.try_error()));
    }
    cue::scene::SceneDocument document =
        cue::scene::SceneDocument::create(std::move(*sceneId.try_value()), a_assertContext);
    return cue::scene::create_scene_snapshot(document, a_assertContext);
}
} // namespace

namespace cue::runtime_host
{
class RuntimeHostApplication::State final
{
  public:
    /// @brief Windowと診断Contextを非所有参照として固定した空のHost Compositionを構築する
    State(Window &a_window, const AssertContext &a_assertContext) noexcept
        : window(&a_window), assertContext(&a_assertContext)
    {
    }

    Window *window;
    const AssertContext *assertContext;
    game_core::WorldIdentitySource worldIdentitySource;
    game_core::SteadyMonotonicClock clock;
    std::shared_ptr<GameModuleConnection> gameModule;
    std::unique_ptr<schema::SchemaRegistryIdentitySource> schemaIdentitySource;
    std::unique_ptr<schema::SchemaRegistry> schemaRegistry;
    renderer::RenderSnapshotStore renderSnapshotStore;
    std::unique_ptr<runtime::RuntimeApplicationSession> session;
    std::unique_ptr<WindowsInputMessageSink> inputSink;
    bool isInputSinkAttached = false;
};

Result<std::unique_ptr<RuntimeHostApplication>> RuntimeHostApplication::start_fixed_smoke(
    Window &a_window, const AssertContext &a_assertContext) noexcept
{
    auto identitySource = std::make_unique<schema::SchemaRegistryIdentitySource>();
    Result<std::unique_ptr<schema::SchemaRegistry>> registry =
        make_schema_registry(*identitySource, a_assertContext);
    if (!registry)
    {
        return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(*registry.try_error()));
    }
    Result<scene::SceneSnapshot> snapshot = make_startup_scene(a_assertContext);
    if (!snapshot)
    {
        return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(*snapshot.try_error()));
    }
    return start(a_window,
                 RuntimeHostStartup(nullptr, std::move(identitySource), std::move(*registry.try_value()),
                                    std::move(*snapshot.try_value()), {}),
                 a_assertContext);
}

Result<std::unique_ptr<RuntimeHostApplication>> RuntimeHostApplication::start(
    Window &a_window, RuntimeHostStartup a_startup, const AssertContext &a_assertContext) noexcept
{
    try
    {
        std::unique_ptr<State> state = std::make_unique<State>(a_window, a_assertContext);
        std::vector<runtime::RuntimeSystemRegistration> systems = a_startup.take_systems();
        state->gameModule = a_startup.take_game_module();
        state->schemaIdentitySource = a_startup.take_schema_identity_source();
        state->schemaRegistry = a_startup.take_schema_registry();
        scene::SceneSnapshot startupSnapshot = a_startup.take_startup_scene();
        if (!state->schemaIdentitySource || !state->schemaRegistry)
        {
            return Result<std::unique_ptr<RuntimeHostApplication>>::failure(runtime::make_runtime_error(
                a_assertContext, runtime::RuntimeError::InvalidApplicationConfiguration,
                "Runtime Host startup is missing its Schema Registry"));
        }

        Result<std::unique_ptr<runtime::RuntimeApplicationSession>> session =
            runtime::RuntimeApplicationSession::create(k_sessionGeneration, state->clock, k_maxDeltaNanoseconds,
                                                       a_assertContext);
        if (!session)
        {
            return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(*session.try_error()));
        }
        state->session = std::move(*session.try_value());
        renderer::RendererRuntimeSystemFactory rendererFactory(state->renderSnapshotStore);
        Result<runtime::RuntimeSystemRegistration> rendererSystem = rendererFactory.create_system(a_assertContext);
        if (!rendererSystem)
        {
            return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(*rendererSystem.try_error()));
        }
        Result<void> rendererRegistered = state->session->register_system(
            std::move(rendererSystem.try_value()->descriptor), std::move(rendererSystem.try_value()->system),
            std::move(rendererSystem.try_value()->componentBuilderFactories));
        if (!rendererRegistered)
        {
            return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(*rendererRegistered.try_error()));
        }
        for (runtime::RuntimeSystemRegistration &system : systems)
        {
            Result<void> registered = state->session->register_system(
                std::move(system.descriptor), std::move(system.system), std::move(system.componentBuilderFactories));
            if (!registered)
            {
                return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(*registered.try_error()));
            }
        }
        state->inputSink = std::make_unique<WindowsInputMessageSink>(state->session->input_events());
        std::unique_ptr<RuntimeHostApplication> application =
            std::make_unique<RuntimeHostApplication>(ConstructionKey{}, std::move(state));

        Result<void> attached =
            attach_windows_message_sink(a_window, *application->m_state->inputSink, a_assertContext);
        if (!attached)
        {
            Result<void> stopped = application->m_state->session->stop();
            if (!stopped)
            {
                report_fatal(a_assertContext.logger(), a_assertContext.fatal_handler(),
                             "Runtime Host could not cleanup an unattached Runtime Application",
                             std::move(*stopped.try_error()));
            }
            return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(*attached.try_error()));
        }
        application->m_state->isInputSinkAttached = true;

        Result<runtime::RuntimeSchemaTypeIds> schemaTypeIds = runtime::make_runtime_schema_type_ids(a_assertContext);
        if (!schemaTypeIds)
        {
            report_fatal(a_assertContext.logger(), a_assertContext.fatal_handler(),
                         "Runtime Host fixed Schema Type parsing failed after attaching Input",
                         std::move(*schemaTypeIds.try_error()));
        }

        Result<void> started = application->m_state->session->start(
            startupSnapshot, application->m_state->worldIdentitySource, *application->m_state->schemaRegistry,
            std::move(schemaTypeIds.try_value()->transform), std::move(schemaTypeIds.try_value()->sceneObjectState));
        if (!started)
        {
            if (application->m_state->session->state() == runtime::RuntimeApplicationSessionState::CleanupFailed)
            {
                Result<void> cleanup = application->m_state->session->stop();
                if (!cleanup)
                {
                    report_fatal(a_assertContext.logger(), a_assertContext.fatal_handler(),
                                 "Runtime Host could not cleanup a failed Runtime Application start",
                                 std::move(*cleanup.try_error()));
                }
            }

            Result<std::optional<Error>> retained = application->m_state->session->take_failure();
            Error primary = retained && retained.try_value()->has_value() ? std::move(retained.try_value()->value())
                                                                          : std::move(*started.try_error());
            Result<void> detached =
                detach_windows_message_sink(a_window, *application->m_state->inputSink, a_assertContext);
            if (!detached)
            {
                report_fatal(a_assertContext.logger(), a_assertContext.fatal_handler(),
                             "Runtime Host could not detach Input after failed Runtime Application start",
                             std::move(*detached.try_error()));
            }
            application->m_state->isInputSinkAttached = false;
            return Result<std::unique_ptr<RuntimeHostApplication>>::failure(std::move(primary));
        }

        return Result<std::unique_ptr<RuntimeHostApplication>>::success(std::move(application));
    }
    catch (const std::bad_alloc &)
    {
        a_assertContext.fatal_handler().terminate("Runtime Host application composition allocation failed");
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate(
            "Runtime Host application composition caught an unexpected exception");
    }

    std::terminate();
}

RuntimeHostApplication::RuntimeHostApplication(ConstructionKey, std::unique_ptr<State> a_state) noexcept
    : m_state(std::move(a_state))
{
}

RuntimeHostApplication::~RuntimeHostApplication() noexcept
{
    const bool isComplete = is_cleanup_complete();
    CUE_ASSERT(*m_state->assertContext, isComplete,
               "Runtime Host application destruction requires stopped Runtime and detached Input Sink");
    if (!isComplete)
    {
        m_state->assertContext->fatal_handler().terminate(
            "Runtime Host application destruction requires stopped Runtime and detached Input Sink");
    }
}

Result<void> RuntimeHostApplication::advance_frame() noexcept
{
    return m_state->session->advance_frame({});
}

Result<void> RuntimeHostApplication::stop(runtime::RuntimeApplicationStopReason a_reason) noexcept
{
    if (is_cleanup_complete())
    {
        return Result<void>::success();
    }

    if (m_state->session->state() == runtime::RuntimeApplicationSessionState::Running)
    {
        Result<void> requested = m_state->session->request_stop(a_reason);
        if (!requested)
        {
            return requested;
        }
    }

    Result<void> stopped = m_state->session->stop();
    std::optional<Error> completedStopFailure;
    if (!stopped && m_state->session->state() != runtime::RuntimeApplicationSessionState::Stopped)
    {
        return stopped;
    }
    if (!stopped)
    {
        completedStopFailure.emplace(std::move(*stopped.try_error()));
    }

    Result<void> detached = detach_windows_message_sink(*m_state->window, *m_state->inputSink, *m_state->assertContext);
    Result<std::optional<Error>> retained = m_state->session->take_failure();
    if (!detached)
    {
        if (retained && retained.try_value()->has_value())
        {
            Error primary = std::move(retained.try_value()->value());
            primary.append_secondary_diagnostics(*m_state->assertContext, *detached.try_error(),
                                                 "Runtime Host Input Sink detach failed after Runtime stopped",
                                                 "Input sink detach");
            return Result<void>::failure(std::move(primary));
        }
        return detached;
    }
    m_state->isInputSinkAttached = false;

    if (!retained)
    {
        return Result<void>::failure(std::move(*retained.try_error()));
    }
    if (retained.try_value()->has_value())
    {
        return Result<void>::failure(std::move(retained.try_value()->value()));
    }
    if (completedStopFailure.has_value())
    {
        return Result<void>::failure(std::move(completedStopFailure.value()));
    }
    return Result<void>::success();
}

bool RuntimeHostApplication::is_cleanup_complete() const noexcept
{
    return !m_state->isInputSinkAttached &&
           m_state->session->state() == runtime::RuntimeApplicationSessionState::Stopped;
}

std::uint64_t RuntimeHostApplication::generation() const noexcept
{
    return m_state->session->generation();
}

std::uint64_t RuntimeHostApplication::world_id() const noexcept
{
    return m_state->session->world_id();
}

std::uint64_t RuntimeHostApplication::frame_count() const noexcept
{
    return m_state->session->next_frame_index();
}

const renderer::RenderSnapshot &RuntimeHostApplication::render_snapshot() const noexcept
{
    return m_state->renderSnapshotStore.snapshot();
}

runtime::RuntimeApplicationStopReason RuntimeHostApplication::stop_reason() const noexcept
{
    return m_state->session->stop_reason();
}
} // namespace cue::runtime_host
