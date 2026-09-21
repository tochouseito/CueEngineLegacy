#include <Cue/GameModule/GameModuleAbi.h>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>

#if !defined(CUE_RUNTIME_PACKAGE_STATIC_PROBE)
/// @brief Dynamic Game ModuleのManifest登録済みApp-local依存を実Loadさせる
extern "C" __declspec(dllimport) int cue_runtime_package_dependency_probe() noexcept;
#endif

namespace
{
constexpr char k_probeModeEnvironment[] = "CUE_RUNTIME_PACKAGE_PROBE_MODE";
constexpr char k_createModuleFailureMessage[] = "Probe Project Scope initialization was rejected";

/// @brief Process単位のTest Modeが要求値と一致するか返す
[[nodiscard]] bool is_probe_mode(std::string_view a_expected) noexcept
{
    std::array<char, 64U> value{};
    const DWORD length = GetEnvironmentVariableA(
        k_probeModeEnvironment, value.data(), static_cast<DWORD>(value.size()));
    return length == a_expected.size() && length < value.size() &&
           std::string_view(value.data(), static_cast<std::size_t>(length)) == a_expected;
}

#if CUE_TEST_BUILD_CONFIGURATION == 1
constexpr std::uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_DEBUG;
#elif CUE_TEST_BUILD_CONFIGURATION == 2
constexpr std::uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_DEVELOPMENT;
#elif CUE_TEST_BUILD_CONFIGURATION == 3
constexpr std::uint32_t k_configuration = CUE_GAME_MODULE_CONFIGURATION_RELEASE;
#else
#error CUE_TEST_BUILD_CONFIGURATION must identify a supported configuration
#endif

constexpr CueGameUuidV1 k_projectId = {
    sizeof(CueGameUuidV1),
    CUE_GAME_MODULE_STRUCTURE_VERSION_1,
    {0x41U, 0x23U, 0x45U, 0x67U, 0x89U, 0xabU, 0x4cU, 0xdeU, 0x8fU, 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xabU, 0xcdU}};

struct ModuleState final
{
    bool isAlive = true;
};

struct SystemState final
{
    bool isStarted = false;
    bool wasUpdated = false;
};

/// @brief Test Module所有のProject Scopeを生成する
CueGameModuleResult CUE_GAME_MODULE_CALL create_module(
    CueGameModuleHandle *a_module, CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    if (a_module == nullptr || *a_module != nullptr)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
#if !defined(CUE_RUNTIME_PACKAGE_STATIC_PROBE)
    if (cue_runtime_package_dependency_probe() != 42)
    {
        return CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED;
    }
#endif
    if (is_probe_mode("create-module-failure"))
    {
        if (a_diagnostic != nullptr && a_diagnostic->structSize >= sizeof(CueGameModuleDiagnosticV1) &&
            a_diagnostic->version == CUE_GAME_MODULE_STRUCTURE_VERSION_1)
        {
            a_diagnostic->code = CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED;
            a_diagnostic->reserved = 0U;
            a_diagnostic->message = {
                sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                k_createModuleFailureMessage, sizeof(k_createModuleFailureMessage) - 1U};
        }
        return CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED;
    }
    *a_module = new (std::nothrow) ModuleState{};
    return *a_module == nullptr ? CUE_GAME_MODULE_RESULT_OUT_OF_MEMORY : CUE_GAME_MODULE_RESULT_SUCCESS;
}

/// @brief Test Module所有のProject Scopeを破棄する
void CUE_GAME_MODULE_CALL destroy_module(CueGameModuleHandle a_module) noexcept
{
    delete static_cast<ModuleState *>(a_module);
}

/// @brief 空のSchema登録Stageが正しいSinkで呼ばれたことを検証する
CueGameModuleResult CUE_GAME_MODULE_CALL register_schemas(
    CueGameModuleHandle a_module, const CueGameRegistrationSinkV1 *a_sink,
    CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    if (a_module == nullptr || a_sink == nullptr || a_sink->structSize < sizeof(CueGameRegistrationSinkV1) ||
        a_sink->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 || a_sink->registerSchema == nullptr)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    if (is_probe_mode("ignored-schema-sink-failure"))
    {
        static_cast<void>(a_sink->registerSchema(a_sink->context, nullptr, a_diagnostic));
    }
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

/// @brief 空のComponent登録Stageが正しいSinkで呼ばれたことを検証する
CueGameModuleResult CUE_GAME_MODULE_CALL register_components(
    CueGameModuleHandle a_module, const CueGameRegistrationSinkV1 *a_sink,
    CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    if (a_module == nullptr || a_sink == nullptr || a_sink->structSize < sizeof(CueGameRegistrationSinkV1) ||
        a_sink->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 || a_sink->registerComponent == nullptr)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    if (is_probe_mode("ignored-component-sink-failure"))
    {
        static_cast<void>(a_sink->registerComponent(a_sink->context, nullptr, a_diagnostic));
    }
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

/// @brief Test SystemのOwner Stateを生成する
CueGameModuleResult CUE_GAME_MODULE_CALL create_system(
    CueGameModuleHandle a_module, CueGameSystemState *a_state, CueGameModuleDiagnosticV1 *) noexcept
{
    if (a_module == nullptr || a_state == nullptr || *a_state != nullptr)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    *a_state = new (std::nothrow) SystemState{};
    return *a_state == nullptr ? CUE_GAME_MODULE_RESULT_OUT_OF_MEMORY : CUE_GAME_MODULE_RESULT_SUCCESS;
}

/// @brief Test SystemのOwner Stateを破棄する
void CUE_GAME_MODULE_CALL destroy_system(CueGameSystemState a_state) noexcept
{
    delete static_cast<SystemState *>(a_state);
}

/// @brief Test Systemを未開始状態から開始する
CueGameModuleResult CUE_GAME_MODULE_CALL start_system(
    CueGameSystemState a_state, CueGameModuleDiagnosticV1 *) noexcept
{
    auto *state = static_cast<SystemState *>(a_state);
    if (state == nullptr || state->isStarted)
    {
        return CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED;
    }
    if (is_probe_mode("system-start-failure"))
    {
        return CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED;
    }
    state->isStarted = true;
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

/// @brief Test Systemが開始後に一つ以上のFrameを受け取ったことを記録する
CueGameModuleResult CUE_GAME_MODULE_CALL update_system(
    CueGameSystemState a_state, const CueGameSystemUpdateV1 *a_update,
    CueGameModuleDiagnosticV1 *) noexcept
{
    auto *state = static_cast<SystemState *>(a_state);
    if (state == nullptr || !state->isStarted || a_update == nullptr ||
        a_update->structSize < sizeof(CueGameSystemUpdateV1) ||
        a_update->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1)
    {
        return CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED;
    }
    state->wasUpdated = true;
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

/// @brief Test Systemを一Frame以上の更新後に停止する
CueGameModuleResult CUE_GAME_MODULE_CALL stop_system(
    CueGameSystemState a_state, CueGameModuleDiagnosticV1 *) noexcept
{
    auto *state = static_cast<SystemState *>(a_state);
    if (state == nullptr || !state->isStarted || !state->wasUpdated)
    {
        return CUE_GAME_MODULE_RESULT_LIFECYCLE_FAILED;
    }
    state->isStarted = false;
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}

constexpr char k_systemIdText[] = "runtime-package-probe";
constexpr char k_invalidSystemIdText[] = "\xc3\x28";
constexpr CueGameSystemDescriptorV1 k_system = {
    sizeof(CueGameSystemDescriptorV1),
    CUE_GAME_MODULE_STRUCTURE_VERSION_1,
    {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, k_systemIdText, sizeof(k_systemIdText) - 1U},
    CUE_GAME_MODULE_SYSTEM_PHASE_UPDATE,
    0,
    nullptr,
    0U,
    &create_system,
    &destroy_system,
    &start_system,
    &update_system,
    &stop_system};

/// @brief 一つのLifecycle検証SystemをHost Sinkへ登録する
CueGameModuleResult CUE_GAME_MODULE_CALL register_systems(
    CueGameModuleHandle a_module, const CueGameRegistrationSinkV1 *a_sink,
    CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    if (a_module == nullptr || a_sink == nullptr || a_sink->structSize < sizeof(CueGameRegistrationSinkV1) ||
        a_sink->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 || a_sink->registerSystem == nullptr)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    if (is_probe_mode("invalid-system-id"))
    {
        CueGameSystemDescriptorV1 invalidSystem = k_system;
        invalidSystem.stableId = {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                  k_invalidSystemIdText, sizeof(k_invalidSystemIdText) - 1U};
        return a_sink->registerSystem(a_sink->context, &invalidSystem, a_diagnostic);
    }
    if (is_probe_mode("ignored-system-sink-failure"))
    {
        CueGameSystemDescriptorV1 invalidSystem = k_system;
        invalidSystem.stableId = {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                  k_invalidSystemIdText, sizeof(k_invalidSystemIdText) - 1U};
        static_cast<void>(a_sink->registerSystem(a_sink->context, &invalidSystem, a_diagnostic));
    }
    if (is_probe_mode("system-start-failure"))
    {
        CueGameSystemDescriptorV1 failingSystem = k_system;
        failingSystem.phase = CUE_GAME_MODULE_SYSTEM_PHASE_POST_UPDATE;
        failingSystem.order = 2000;
        return a_sink->registerSystem(a_sink->context, &failingSystem, a_diagnostic);
    }
    return a_sink->registerSystem(a_sink->context, &k_system, a_diagnostic);
}

constexpr CueGameModuleApiV1 k_api = {
    sizeof(CueGameModuleApiV1),
    CUE_GAME_MODULE_STRUCTURE_VERSION_1,
    CUE_GAME_MODULE_ABI_VERSION_1,
    k_configuration,
    CUE_GAME_MODULE_ARCHITECTURE_X64,
    0U,
    k_projectId,
    &create_module,
    &register_schemas,
    &register_components,
    &register_systems,
    &destroy_module,
    {0U, 0U, 0U, 0U}};

constexpr CueGameModuleApiV1 k_apiWithReservedTail = {
    sizeof(CueGameModuleApiV1),
    CUE_GAME_MODULE_STRUCTURE_VERSION_1,
    CUE_GAME_MODULE_ABI_VERSION_1,
    k_configuration,
    CUE_GAME_MODULE_ARCHITECTURE_X64,
    0U,
    k_projectId,
    &create_module,
    &register_schemas,
    &register_components,
    &register_systems,
    &destroy_module,
    {0U, 0U, 0U, 1U}};
} // namespace

/// @brief Test用Game Moduleの固定ABI Tableを返す
CUE_GAME_MODULE_EXTERN_C CUE_GAME_MODULE_EXPORT CueGameModuleResult CUE_GAME_MODULE_CALL
cue_game_module_query(uint32_t a_requestedAbiVersion, CueGameModuleQueryOutputV1 *a_output,
                      CueGameModuleDiagnosticV1 *) CUE_GAME_MODULE_NOEXCEPT
{
    if (a_requestedAbiVersion != CUE_GAME_MODULE_ABI_VERSION_1 || a_output == nullptr ||
        a_output->structSize != sizeof(CueGameModuleQueryOutputV1) ||
        a_output->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1)
    {
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    a_output->api = is_probe_mode("reserved-api-tail") ? &k_apiWithReservedTail : &k_api;
    if (is_probe_mode("query-output-size"))
    {
        a_output->structSize = 0U;
    }
    if (is_probe_mode("query-output-version"))
    {
        a_output->version = CUE_GAME_MODULE_STRUCTURE_VERSION_1 + 1U;
    }
    if (is_probe_mode("reserved-query-output"))
    {
        a_output->reserved[1] = 1U;
    }
    return CUE_GAME_MODULE_RESULT_SUCCESS;
}
