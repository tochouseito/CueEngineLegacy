#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/GameCore/CommandBuffer.h>
#include <Cue/GameCore/World.h>
#include <Cue/Input/FrameInputSnapshot.h>
#include <Cue/Renderer/RendererSchema.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Runtime/RuntimeSchema.h>
#include <Cue/RuntimeHost/GameModuleQueryProvider.h>
#include <Cue/RuntimeHost/StaticRuntimePackage.h>
#include <Cue/Schema/Registry.h>

#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef CUE_TEST_BUILD_CONFIGURATION
#error CUE_TEST_BUILD_CONFIGURATION must identify the test configuration
#endif

namespace
{
constexpr std::string_view k_projectId = "41234567-89ab-4cde-8f01-23456789abcd";
constexpr CueGameUuidV1 k_projectUuid = {
    sizeof(CueGameUuidV1),
    CUE_GAME_MODULE_STRUCTURE_VERSION_1,
    {0x41U, 0x23U, 0x45U, 0x67U, 0x89U, 0xabU, 0x4cU, 0xdeU,
     0x8fU, 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xabU, 0xcdU}};

#if CUE_TEST_BUILD_CONFIGURATION == 1
constexpr std::uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_DEBUG;
#elif CUE_TEST_BUILD_CONFIGURATION == 2
constexpr std::uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_DEVELOPMENT;
#elif CUE_TEST_BUILD_CONFIGURATION == 3
constexpr std::uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_RELEASE;
#else
#error Unsupported CUE_TEST_BUILD_CONFIGURATION value
#endif

std::vector<std::string> *g_timeline = nullptr;
bool g_failSecondState = false;
bool g_failModuleCreation = false;

constexpr std::string_view k_moduleFailureMessage = "Module diagnostic before destroy";
constexpr std::string_view k_systemFailureMessage = "System diagnostic before rollback";

void record(std::string a_event)
{
    g_timeline->push_back(std::move(a_event));
}

struct ModuleState final
{
    char diagnostic[64]{};
};

struct SystemState final
{
    char id;
    char diagnostic[64]{};
};

void set_diagnostic(CueGameModuleDiagnosticV1 *a_diagnostic, char *a_storage,
                    std::string_view a_message) noexcept
{
    std::char_traits<char>::copy(a_storage, a_message.data(), a_message.size());
    a_storage[a_message.size()] = '\0';
    if (a_diagnostic != nullptr)
    {
        a_diagnostic->code = CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED;
        a_diagnostic->message = {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                 a_storage, a_message.size()};
    }
}

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(77);
    }

    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(78);
    }
};

class TestClock final : public cue::game_core::MonotonicClock
{
  public:
    [[nodiscard]] cue::Result<cue::game_core::MonotonicClockSample> sample(
        const cue::AssertContext &) noexcept override
    {
        cue::game_core::MonotonicClockSample sample{m_value};
        m_value += 16'000'000;
        return cue::Result<cue::game_core::MonotonicClockSample>::success(std::move(sample));
    }

  private:
    std::int64_t m_value = 0;
};

void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::_Exit(2);
    }
}

template <typename T> [[nodiscard]] T take_value(cue::Result<T> &&a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

CueGameModuleResult CUE_GAME_MODULE_CALL create_module(
    CueGameModuleHandle *a_module, CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    record("createModule");
    if (a_module == nullptr || *a_module != nullptr)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    auto *module = new (std::nothrow) ModuleState{};
    *a_module = module;
    if (module == nullptr)
    {
        return CUE_GAME_MODULE_RESULT_OUT_OF_MEMORY;
    }
    if (g_failModuleCreation)
    {
        set_diagnostic(a_diagnostic, module->diagnostic, k_moduleFailureMessage);
        return CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED;
    }
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

void CUE_GAME_MODULE_CALL destroy_module(CueGameModuleHandle a_module) noexcept
{
    record("destroyModule");
    delete static_cast<ModuleState *>(a_module);
}

CueGameModuleResult CUE_GAME_MODULE_CALL register_schemas(
    CueGameModuleHandle, const CueGameRegistrationSinkV1 *, CueGameModuleDiagnosticV1 *) noexcept
{
    record("registerSchemas");
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

CueGameModuleResult CUE_GAME_MODULE_CALL register_components(
    CueGameModuleHandle, const CueGameRegistrationSinkV1 *, CueGameModuleDiagnosticV1 *) noexcept
{
    record("registerComponents");
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

template <char Id> CueGameModuleResult CUE_GAME_MODULE_CALL create_system(
    CueGameModuleHandle, CueGameSystemState *a_state, CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    record(std::string("createState.") + Id);
    if (a_state == nullptr || *a_state != nullptr)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    *a_state = new (std::nothrow) SystemState{Id, {}};
    if (*a_state == nullptr)
    {
        return CUE_GAME_MODULE_RESULT_OUT_OF_MEMORY;
    }
    if constexpr (Id == 'B')
    {
        if (g_failSecondState)
        {
            auto *state = static_cast<SystemState *>(*a_state);
            set_diagnostic(a_diagnostic, state->diagnostic, k_systemFailureMessage);
            return CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED;
        }
    }
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

void CUE_GAME_MODULE_CALL destroy_system(CueGameSystemState a_state) noexcept
{
    auto *state = static_cast<SystemState *>(a_state);
    record(std::string("destroyState.") + state->id);
    delete state;
}

CueGameModuleResult CUE_GAME_MODULE_CALL start_system(
    CueGameSystemState a_state, CueGameModuleDiagnosticV1 *) noexcept
{
    auto *state = static_cast<SystemState *>(a_state);
    record(std::string("start.") + state->id);
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

CueGameModuleResult CUE_GAME_MODULE_CALL update_system(
    CueGameSystemState a_state, const CueGameSystemUpdateV1 *, CueGameModuleDiagnosticV1 *) noexcept
{
    auto *state = static_cast<SystemState *>(a_state);
    record(std::string("update.") + state->id);
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

CueGameModuleResult CUE_GAME_MODULE_CALL stop_system(
    CueGameSystemState a_state, CueGameModuleDiagnosticV1 *) noexcept
{
    auto *state = static_cast<SystemState *>(a_state);
    record(std::string("stop.") + state->id);
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

constexpr char k_systemAId[] = "system-a";
constexpr char k_systemBId[] = "system-b";
constexpr CueGameSystemDescriptorV1 k_systemA = {
    sizeof(CueGameSystemDescriptorV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
    {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, k_systemAId, sizeof(k_systemAId) - 1U},
    CUE_GAME_MODULE_SYSTEM_PHASE_UPDATE, 0, nullptr, 0U, &create_system<'A'>, &destroy_system,
    &start_system, &update_system, &stop_system};
constexpr CueGameSystemDescriptorV1 k_systemB = {
    sizeof(CueGameSystemDescriptorV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
    {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, k_systemBId, sizeof(k_systemBId) - 1U},
    CUE_GAME_MODULE_SYSTEM_PHASE_UPDATE, 1, nullptr, 0U, &create_system<'B'>, &destroy_system,
    &start_system, &update_system, &stop_system};

CueGameModuleResult CUE_GAME_MODULE_CALL register_systems(
    CueGameModuleHandle, const CueGameRegistrationSinkV1 *a_sink,
    CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    record("registerSystems");
    if (a_sink == nullptr || a_sink->registerSystem == nullptr)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    CueGameModuleResult first = a_sink->registerSystem(a_sink->context, &k_systemA, a_diagnostic);
    return first == CUE_GAME_MODULE_RESULT_SUCCESS
               ? a_sink->registerSystem(a_sink->context, &k_systemB, a_diagnostic)
               : first;
}

constexpr CueGameModuleApiV1 k_api = {
    sizeof(CueGameModuleApiV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, CUE_GAME_MODULE_ABI_VERSION_1,
    k_configuration, CUE_GAME_MODULE_ARCHITECTURE_X64, 0U, k_projectUuid, &create_module,
    &register_schemas, &register_components, &register_systems, &destroy_module, {0U, 0U, 0U, 0U}};

CueGameModuleResult CUE_GAME_MODULE_CALL query_module(
    std::uint32_t a_version, CueGameModuleQueryOutputV1 *a_output,
    CueGameModuleDiagnosticV1 *) noexcept
{
    record("query");
    if (a_version != CUE_GAME_MODULE_ABI_VERSION_1 || a_output == nullptr)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    a_output->api = &k_api;
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

CueGameModuleResult CUE_GAME_MODULE_CALL reject_query_module(
    std::uint32_t, CueGameModuleQueryOutputV1 *, CueGameModuleDiagnosticV1 *) noexcept
{
    record("queryRejected");
    return CUE_GAME_MODULE_RESULT_UNSUPPORTED_ABI;
}

class TrackingCodeLifetime final : public cue::runtime_host::GameModuleCodeLifetime
{
  public:
    ~TrackingCodeLifetime() noexcept override
    {
        record("unload");
    }
};

class TrackingQueryProvider final : public cue::runtime_host::GameModuleQueryProvider
{
  public:
    explicit TrackingQueryProvider(
        cue::runtime_host::GameModuleQueryFunction a_query = &query_module) noexcept
        : m_query(a_query)
    {
    }

    [[nodiscard]] cue::Result<cue::runtime_host::ResolvedGameModuleQuery> resolve(
        const cue::AssertContext &) noexcept override
    {
        record("resolveDynamic");
        return cue::Result<cue::runtime_host::ResolvedGameModuleQuery>::success(
            {m_query, std::make_shared<TrackingCodeLifetime>()});
    }

    [[nodiscard]] cue::Error make_query_contract_error(
        const cue::AssertContext &a_assertContext, std::string_view a_summary) const noexcept override
    {
        record("classifyQueryContract");
        return cue::runtime::make_runtime_error(
            a_assertContext, cue::runtime::RuntimeError::InvalidApplicationConfiguration, a_summary);
    }

  private:
    cue::runtime_host::GameModuleQueryFunction m_query;
};

void run_lifecycle(cue::runtime_host::GameModuleQueryProvider &a_provider,
                   const cue::AssertContext &a_assertContext)
{
    cue::runtime_host::PreparedGameModule prepared = take_value(cue::runtime_host::connect_game_module(
        a_provider, k_projectId, std::make_unique<cue::schema::SchemaRegistryIdentitySource>(),
        a_assertContext));
    const cue::schema::SchemaRegistry &registry = prepared.schema_registry();
    const cue::renderer::RendererSchemaTypeIds rendererIds =
        take_value(cue::renderer::make_renderer_schema_type_ids(a_assertContext));
    require(registry.find(rendererIds.camera, a_assertContext).has_value());
    require(registry.find(rendererIds.mesh, a_assertContext).has_value());
    std::vector<cue::runtime::RuntimeSystemRegistration> &systems = prepared.systems();
    cue::game_core::WorldIdentitySource worldIdentitySource;
    std::unique_ptr<cue::game_core::World> world =
        take_value(cue::game_core::World::create(worldIdentitySource, registry, a_assertContext));
    {
        cue::game_core::StructuralCommandBuffer commands(*world);
        cue::game_core::RuntimeSystemContext systemContext{*world, commands};
        for (cue::runtime::RuntimeSystemRegistration &system : systems)
        {
            require(system.system->start(systemContext).has_value());
        }
        TestClock source;
        cue::game_core::GameClock clock =
            take_value(cue::game_core::GameClock::create(source, 100'000'000, a_assertContext));
        require(clock.reset().has_value());
        cue::game_core::UpdateContext timing = take_value(clock.advance_frame());
        const cue::FrameInputSnapshot input;
        cue::game_core::RuntimeSystemUpdateContext updateContext{*world, commands, timing, input};
        for (cue::runtime::RuntimeSystemRegistration &system : systems)
        {
            require(system.system->update(updateContext).has_value());
        }
        for (auto system = systems.rbegin(); system != systems.rend(); ++system)
        {
            require(system->system->stop(systemContext).has_value());
        }
    }
    world.reset();
}

/// @brief CoreとRendererの重複Stable Type IDをSeal前に拒否する
void test_renderer_schema_duplicate(const cue::AssertContext &a_assertContext)
{
    cue::schema::SchemaRegistryIdentitySource identitySource;
    cue::schema::SchemaRegistryBuilder builder(identitySource, a_assertContext);
    require(cue::runtime::add_runtime_schema_types(builder, a_assertContext).has_value());
    require(cue::renderer::add_renderer_schema_types(builder, a_assertContext).has_value());
    require(!cue::renderer::add_renderer_schema_types(builder, a_assertContext).has_value());
}

void test_static_provider(const cue::AssertContext &a_assertContext)
{
    std::vector<std::string> timeline;
    g_timeline = &timeline;
    auto provider = take_value(cue::runtime_host::create_static_game_module_query_provider(
        &query_module, a_assertContext));
    run_lifecycle(*provider, a_assertContext);
    const std::vector<std::string> expected = {
        "query", "createModule", "registerSchemas", "registerComponents", "registerSystems",
        "createState.A", "createState.B", "start.A", "start.B", "update.A", "update.B",
        "stop.B", "stop.A", "destroyState.B", "destroyState.A", "destroyModule"};
    require(timeline == expected);
}

/// @brief 未実装のPublisher署名検証をRuntime起動境界が常に拒否することを検証する
void test_static_runtime_trust_policy(const cue::AssertContext &a_assertContext)
{
    std::vector<std::string> timeline;
    g_timeline = &timeline;
    cue::Result<cue::runtime_host::RuntimeHostStartup> signedPackage =
        cue::runtime_host::load_static_runtime_package(
            &query_module, k_projectId, cue::runtime_host::StaticRuntimeTrustMode::PublisherSigned,
            std::string(64U, 'a'), a_assertContext);
    cue::Result<cue::runtime_host::RuntimeHostStartup> inconsistentUnsignedPackage =
        cue::runtime_host::load_static_runtime_package(
            &query_module, k_projectId, cue::runtime_host::StaticRuntimeTrustMode::UnsignedLocal,
            std::string(64U, 'a'), a_assertContext);
    cue::Result<cue::runtime_host::RuntimeHostStartup> unknownTrustMode =
        cue::runtime_host::load_static_runtime_package(
            &query_module, k_projectId, static_cast<cue::runtime_host::StaticRuntimeTrustMode>(0xffU), {},
            a_assertContext);
    require(!signedPackage.has_value() && !inconsistentUnsignedPackage.has_value() &&
            !unknownTrustMode.has_value());
    require(signedPackage.try_error()->summary().find("Only UnsignedLocal") != std::string_view::npos);
    require(inconsistentUnsignedPackage.try_error()->summary().find("Only UnsignedLocal") != std::string_view::npos);
    require(unknownTrustMode.try_error()->summary().find("Only UnsignedLocal") != std::string_view::npos);
    require(timeline.empty());
}

void test_dynamic_provider_and_rollback(const cue::AssertContext &a_assertContext)
{
    std::vector<std::string> timeline;
    g_timeline = &timeline;
    TrackingQueryProvider provider;
    run_lifecycle(provider, a_assertContext);
    const std::vector<std::string> expected = {
        "resolveDynamic", "query", "createModule", "registerSchemas", "registerComponents", "registerSystems",
        "createState.A", "createState.B", "start.A", "start.B", "update.A", "update.B",
        "stop.B", "stop.A", "destroyState.B", "destroyState.A", "destroyModule", "unload"};
    require(timeline == expected);

    timeline.clear();
    g_failSecondState = true;
    cue::Result<cue::runtime_host::PreparedGameModule> failed = cue::runtime_host::connect_game_module(
        provider, k_projectId, std::make_unique<cue::schema::SchemaRegistryIdentitySource>(),
        a_assertContext);
    g_failSecondState = false;
    require(!failed.has_value());
    require(failed.try_error()->summary().find(k_systemFailureMessage) != std::string_view::npos);
    const std::vector<std::string> rollback = {
        "resolveDynamic", "query", "createModule", "registerSchemas", "registerComponents", "registerSystems",
        "createState.A", "createState.B", "destroyState.B", "destroyState.A", "destroyModule", "unload"};
    require(timeline == rollback);

    timeline.clear();
    TrackingQueryProvider rejectingProvider(&reject_query_module);
    cue::Result<cue::runtime_host::PreparedGameModule> rejected = cue::runtime_host::connect_game_module(
        rejectingProvider, k_projectId, std::make_unique<cue::schema::SchemaRegistryIdentitySource>(),
        a_assertContext);
    require(!rejected.has_value());
    require(rejected.try_error()->code().domain() == "Cue.Runtime");
    const std::vector<std::string> rejectedTimeline = {
        "resolveDynamic", "queryRejected", "classifyQueryContract", "unload"};
    require(timeline == rejectedTimeline);

    timeline.clear();
    g_failModuleCreation = true;
    cue::Result<cue::runtime_host::PreparedGameModule> moduleCreationFailure =
        cue::runtime_host::connect_game_module(
            provider, k_projectId, std::make_unique<cue::schema::SchemaRegistryIdentitySource>(),
            a_assertContext);
    g_failModuleCreation = false;
    require(!moduleCreationFailure.has_value());
    require(moduleCreationFailure.try_error()->summary().find(k_moduleFailureMessage) != std::string_view::npos);
    const std::vector<std::string> moduleCreationFailureTimeline = {
        "resolveDynamic", "query", "createModule", "destroyModule", "unload"};
    require(timeline == moduleCreationFailureTimeline);
}
} // namespace

int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    test_static_provider(assertContext);
    test_renderer_schema_duplicate(assertContext);
    test_static_runtime_trust_policy(assertContext);
    test_dynamic_provider_and_rollback(assertContext);
    return 0;
}
