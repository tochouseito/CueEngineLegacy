#include <Cue/RuntimeHost/GameModuleQueryProvider.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/GameCore/Clock.h>
#include <Cue/Renderer/RendererSchema.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Runtime/RuntimeSchema.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef CUE_RUNTIME_BUILD_CONFIGURATION
#error CUE_RUNTIME_BUILD_CONFIGURATION must identify the RuntimeHost build configuration
#endif

namespace
{
constexpr std::size_t k_maximumRuntimeSystems = 256U;
constexpr std::uint64_t k_maximumAbiStringBytes = 64U * 1024U;

/// @brief 共通Game Module接続処理中の回復不能例外をFatal境界へ渡す
[[noreturn]] void terminate_game_module_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Unexpected exception escaped Game Module connection");
    std::abort();
}

/// @brief UTF-8 Byte列がUnicode Scalar Valueの正規Encodingだけを含むか返す
[[nodiscard]] bool is_valid_utf8(std::string_view a_text) noexcept
{
    std::size_t offset = 0U;
    while (offset < a_text.size())
    {
        const auto first = static_cast<unsigned char>(a_text[offset]);
        if (first <= 0x7fU)
        {
            ++offset;
            continue;
        }
        std::size_t length = 0U;
        std::uint32_t value = 0U;
        std::uint32_t minimum = 0U;
        if (first >= 0xc2U && first <= 0xdfU)
        {
            length = 2U;
            value = first & 0x1fU;
            minimum = 0x80U;
        }
        else if (first >= 0xe0U && first <= 0xefU)
        {
            length = 3U;
            value = first & 0x0fU;
            minimum = 0x800U;
        }
        else if (first >= 0xf0U && first <= 0xf4U)
        {
            length = 4U;
            value = first & 0x07U;
            minimum = 0x10000U;
        }
        else
        {
            return false;
        }
        if (offset + length > a_text.size())
        {
            return false;
        }
        for (std::size_t index = 1U; index < length; ++index)
        {
            const auto continuation = static_cast<unsigned char>(a_text[offset + index]);
            if ((continuation & 0xc0U) != 0x80U)
            {
                return false;
            }
            value = (value << 6U) | (continuation & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU))
        {
            return false;
        }
        offset += length;
    }
    return true;
}

/// @brief 現在ConfigurationをGame Module ABI値へ変換する
[[nodiscard]] constexpr std::uint32_t host_module_configuration() noexcept
{
#if CUE_RUNTIME_BUILD_CONFIGURATION == 1
    return CUE_GAME_MODULE_CONFIGURATION_DEBUG;
#elif CUE_RUNTIME_BUILD_CONFIGURATION == 2
    return CUE_GAME_MODULE_CONFIGURATION_DEVELOPMENT;
#elif CUE_RUNTIME_BUILD_CONFIGURATION == 3
    return CUE_GAME_MODULE_CONFIGURATION_RELEASE;
#else
#error Unsupported CUE_RUNTIME_BUILD_CONFIGURATION value
#endif
}

/// @brief ABI UUIDをlowercase canonical Textへ変換する
[[nodiscard]] std::string uuid_text(const CueGameUuidV1 &a_uuid)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(36U);
    for (std::size_t index = 0U; index < 16U; ++index)
    {
        if (index == 4U || index == 6U || index == 8U || index == 10U)
        {
            output.push_back('-');
        }
        output.push_back(digits[(a_uuid.bytes[index] >> 4U) & 0x0fU]);
        output.push_back(digits[a_uuid.bytes[index] & 0x0fU]);
    }
    return output;
}

/// @brief Module Diagnosticへ呼出中有効な固定Messageを設定する
void set_module_diagnostic(CueGameModuleDiagnosticV1 *a_diagnostic, CueGameModuleResult a_code,
                           const char *a_message) noexcept
{
    if (a_diagnostic == nullptr || a_diagnostic->structSize < sizeof(CueGameModuleDiagnosticV1) ||
        a_diagnostic->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1)
    {
        return;
    }
    a_diagnostic->code = a_code;
    a_diagnostic->reserved = 0U;
    a_diagnostic->message = {
        sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, a_message,
        a_message == nullptr ? 0U : static_cast<std::uint64_t>(std::char_traits<char>::length(a_message))};
}

/// @brief Module登録中に収集する一System Definition
struct PendingSystem final
{
    cue::game_core::RuntimeSystemDescriptor descriptor;
    CueGameSystemCreateV1 createState;
    CueGameSystemDestroyV1 destroyState;
    CueGameSystemStartV1 start;
    CueGameSystemUpdateCallbackV1 update;
    CueGameSystemStopV1 stop;
};

enum class RegistrationStage : std::uint8_t
{
    Schemas,
    Components,
    Systems
};

/// @brief Game Module ABI SinkへOwner Thread限定の登録状態を渡す
struct RegistrationContext final
{
    RegistrationStage stage = RegistrationStage::Schemas;
    CueGameModuleResult sinkFailure = CUE_GAME_MODULE_RESULT_SUCCESS;
    std::vector<PendingSystem> systems;
};

/// @brief 登録Stage内で最初に発生したSink失敗を外側Callback結果とは独立して保持する
void latch_registration_failure(RegistrationContext *a_context, CueGameModuleResult a_result) noexcept
{
    if (a_context != nullptr && a_result != CUE_GAME_MODULE_RESULT_SUCCESS &&
        a_context->sinkFailure == CUE_GAME_MODULE_RESULT_SUCCESS)
    {
        a_context->sinkFailure = a_result;
    }
}

/// @brief Sink失敗をStageへ記録しModule Diagnosticと同じ結果を返す
[[nodiscard]] CueGameModuleResult fail_registration(
    RegistrationContext *a_context, CueGameModuleDiagnosticV1 *a_diagnostic,
    CueGameModuleResult a_result, const char *a_message) noexcept
{
    latch_registration_failure(a_context, a_result);
    set_module_diagnostic(a_diagnostic, a_result, a_message);
    return a_result;
}

/// @brief M16未対応のGame Schema登録をFail-closedに拒否する
CueGameModuleResult CUE_GAME_MODULE_CALL reject_schema_registration(
    void *a_context, const CueGameSchemaDescriptorV1 *, CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    auto *context = static_cast<RegistrationContext *>(a_context);
    if (context == nullptr || context->stage != RegistrationStage::Schemas)
    {
        return fail_registration(context, a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                                 "Schema registration occurred outside its stage");
    }
    return fail_registration(context, a_diagnostic, CUE_GAME_MODULE_RESULT_REGISTRATION_FAILED,
                             "RuntimeHost v1 does not yet accept custom Schema registration");
}

/// @brief M16未対応のGame Component登録をFail-closedに拒否する
CueGameModuleResult CUE_GAME_MODULE_CALL reject_component_registration(
    void *a_context, const CueGameComponentDescriptorV1 *, CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    auto *context = static_cast<RegistrationContext *>(a_context);
    if (context == nullptr || context->stage != RegistrationStage::Components)
    {
        return fail_registration(context, a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                                 "Component registration occurred outside its stage");
    }
    return fail_registration(context, a_diagnostic, CUE_GAME_MODULE_RESULT_REGISTRATION_FAILED,
                             "RuntimeHost v1 does not yet accept custom Component registration");
}

/// @brief ABI UTF-8 Viewを所有文字列へ検証Copyする
[[nodiscard]] bool copy_utf8_view(const CueGameUtf8ViewV1 &a_view, std::string &a_output)
{
    if (a_view.structSize < sizeof(CueGameUtf8ViewV1) || a_view.version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 ||
        a_view.size == 0U || a_view.size > k_maximumAbiStringBytes || a_view.data == nullptr)
    {
        return false;
    }
    const std::string_view text(a_view.data, static_cast<std::size_t>(a_view.size));
    if (!is_valid_utf8(text))
    {
        return false;
    }
    a_output.assign(text);
    return true;
}

/// @brief Game Module System DescriptorとCallbackをHost所有定義へCopy登録する
CueGameModuleResult CUE_GAME_MODULE_CALL register_system(
    void *a_context, const CueGameSystemDescriptorV1 *a_descriptor,
    CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    auto *context = static_cast<RegistrationContext *>(a_context);
    if (context == nullptr || context->stage != RegistrationStage::Systems || a_descriptor == nullptr ||
        a_descriptor->structSize < sizeof(CueGameSystemDescriptorV1) ||
        a_descriptor->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 ||
        a_descriptor->dependencyCount > k_maximumRuntimeSystems ||
        (a_descriptor->dependencyCount != 0U && a_descriptor->dependencies == nullptr) ||
        a_descriptor->createState == nullptr || a_descriptor->destroyState == nullptr || a_descriptor->start == nullptr ||
        a_descriptor->update == nullptr || a_descriptor->stop == nullptr ||
        context->systems.size() >= k_maximumRuntimeSystems)
    {
        return fail_registration(context, a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                                 "Runtime System descriptor is invalid");
    }
    try
    {
        PendingSystem pending{{}, a_descriptor->createState, a_descriptor->destroyState, a_descriptor->start,
                              a_descriptor->update, a_descriptor->stop};
        if (!copy_utf8_view(a_descriptor->stableId, pending.descriptor.id))
        {
            return fail_registration(context, a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                                     "Runtime System stable ID is invalid");
        }
        switch (a_descriptor->phase)
        {
        case CUE_GAME_MODULE_SYSTEM_PHASE_PRE_UPDATE:
            pending.descriptor.phase = cue::game_core::RuntimeUpdatePhase::PreUpdate;
            break;
        case CUE_GAME_MODULE_SYSTEM_PHASE_UPDATE:
            pending.descriptor.phase = cue::game_core::RuntimeUpdatePhase::Update;
            break;
        case CUE_GAME_MODULE_SYSTEM_PHASE_POST_UPDATE:
            pending.descriptor.phase = cue::game_core::RuntimeUpdatePhase::PostUpdate;
            break;
        default:
            return fail_registration(context, a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                                     "Runtime System phase is invalid");
        }
        pending.descriptor.order = a_descriptor->order;
        for (std::size_t index = 0U; index < a_descriptor->dependencyCount; ++index)
        {
            std::string dependency;
            if (!copy_utf8_view(a_descriptor->dependencies[index], dependency))
            {
                return fail_registration(context, a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                                         "Runtime System dependency is invalid");
            }
            pending.descriptor.dependencies.push_back(std::move(dependency));
        }
        context->systems.push_back(std::move(pending));
        set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_SUCCESS, nullptr);
        return CUE_GAME_MODULE_RESULT_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        return fail_registration(context, a_diagnostic, CUE_GAME_MODULE_RESULT_OUT_OF_MEMORY,
                                 "Runtime System registration allocation failed");
    }
    catch (...)
    {
        return fail_registration(context, a_diagnostic, CUE_GAME_MODULE_RESULT_REGISTRATION_FAILED,
                                 "Runtime System registration failed unexpectedly");
    }
}

/// @brief Game Module登録Callbackの失敗をRuntime Errorへ変換する
[[nodiscard]] cue::Result<void> call_registration(
    CueGameModuleRegisterV1 a_callback, CueGameModuleHandle a_module, RegistrationContext &a_context,
    RegistrationStage a_stage, const cue::AssertContext &a_assertContext) noexcept
{
    a_context.stage = a_stage;
    a_context.sinkFailure = CUE_GAME_MODULE_RESULT_SUCCESS;
    CueGameRegistrationSinkV1 sink{sizeof(CueGameRegistrationSinkV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                   &a_context, &reject_schema_registration, &reject_component_registration,
                                   &register_system, {0U, 0U, 0U, 0U}};
    CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                         0U, 0U,
                                         {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr, 0U}};
    const CueGameModuleResult result = a_callback(a_module, &sink, &diagnostic);
    if (result != CUE_GAME_MODULE_RESULT_SUCCESS || a_context.sinkFailure != CUE_GAME_MODULE_RESULT_SUCCESS)
    {
        return cue::Result<void>::failure(cue::runtime::make_runtime_error(
            a_assertContext, cue::runtime::RuntimeError::InvalidApplicationConfiguration,
            "Game Module registration failed"));
    }
    return cue::Result<void>::success();
}

/// @brief Module借用DiagnosticをCode Lease解放前に所有ErrorへCopyする
[[nodiscard]] cue::Error make_module_callback_error(
    const cue::AssertContext &a_assertContext, CueGameModuleResult a_result,
    const CueGameModuleDiagnosticV1 &a_diagnostic, std::string_view a_summary)
{
    std::string summary(a_summary);
    summary.append(" (module result ");
    summary.append(std::to_string(a_result));
    summary.push_back(')');
    if (a_diagnostic.structSize >= sizeof(CueGameModuleDiagnosticV1) &&
        a_diagnostic.version == CUE_GAME_MODULE_STRUCTURE_VERSION_1)
    {
        summary.append(" (diagnostic code ");
        summary.append(std::to_string(a_diagnostic.code));
        summary.push_back(')');
        std::string message;
        if (copy_utf8_view(a_diagnostic.message, message))
        {
            summary.append(": ");
            summary.append(message);
        }
    }
    return cue::runtime::make_runtime_error(
        a_assertContext, cue::runtime::RuntimeError::InvalidApplicationConfiguration, summary);
}

/// @brief Runtime System失敗を外部診断Ownerへ依存しない所有Errorへ変換する
[[nodiscard]] cue::Error make_game_system_error(cue::runtime::RuntimeError a_code,
                                                std::string_view a_summary) noexcept
{
    static cue::AbortFatalHandler emergencyHandler;
    cue::ErrorCode code =
        cue::ErrorCode::create(emergencyHandler, "Cue.Runtime", static_cast<std::int64_t>(a_code));
    return cue::Error::create(emergencyHandler, std::move(code), a_summary);
}

/// @brief Provider方式に依存しないGame Module System State Adapter
class GameModuleRuntimeSystem final : public cue::game_core::RuntimeSystem
{
  public:
    GameModuleRuntimeSystem(std::shared_ptr<cue::runtime_host::GameModuleConnection> a_connection,
                            CueGameSystemState a_state, PendingSystem a_definition) noexcept
        : m_connection(std::move(a_connection)), m_state(a_state), m_definition(std::move(a_definition))
    {
    }

    ~GameModuleRuntimeSystem() noexcept override
    {
        if (m_state != nullptr)
        {
            m_definition.destroyState(m_state);
        }
    }

    [[nodiscard]] cue::Result<void> start(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                             0U, 0U,
                                             {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr,
                                              0U}};
        if (m_definition.start(m_state, &diagnostic) != CUE_GAME_MODULE_RESULT_SUCCESS)
        {
            return cue::Result<void>::failure(make_game_system_error(
                cue::runtime::RuntimeError::ApplicationSessionStartFailed, "Game Module System start failed"));
        }
        return cue::Result<void>::success();
    }

    [[nodiscard]] cue::Result<void> update(
        const cue::game_core::RuntimeSystemUpdateContext &a_context) noexcept override
    {
        const cue::game_core::FrameTiming &timing = a_context.timing.timing();
        CueGameSystemUpdateV1 update{sizeof(CueGameSystemUpdateV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                     timing.frameIndex, timing.simulationDeltaNanoseconds,
                                     timing.simulationTimeNanoseconds};
        CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                             0U, 0U,
                                             {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr,
                                              0U}};
        if (m_definition.update(m_state, &update, &diagnostic) != CUE_GAME_MODULE_RESULT_SUCCESS)
        {
            return cue::Result<void>::failure(make_game_system_error(
                cue::runtime::RuntimeError::ApplicationSessionUpdateFailed, "Game Module System update failed"));
        }
        return cue::Result<void>::success();
    }

    [[nodiscard]] cue::Result<void> stop(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                             0U, 0U,
                                             {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr,
                                              0U}};
        if (m_definition.stop(m_state, &diagnostic) != CUE_GAME_MODULE_RESULT_SUCCESS)
        {
            return cue::Result<void>::failure(make_game_system_error(
                cue::runtime::RuntimeError::ApplicationSessionCleanupFailed, "Game Module System stop failed"));
        }
        return cue::Result<void>::success();
    }

  private:
    std::shared_ptr<cue::runtime_host::GameModuleConnection> m_connection;
    CueGameSystemState m_state;
    PendingSystem m_definition;
};

/// @brief Pending System群からSession所有登録を構築し、途中失敗を逆順Rollbackする
[[nodiscard]] cue::Result<std::vector<cue::runtime::RuntimeSystemRegistration>> create_systems(
    const std::shared_ptr<cue::runtime_host::GameModuleConnection> &a_connection,
    CueGameModuleHandle a_module, std::vector<PendingSystem> a_pending,
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::vector<cue::runtime::RuntimeSystemRegistration> systems;
        systems.reserve(a_pending.size());
        for (PendingSystem &pending : a_pending)
        {
            CueGameSystemState state = nullptr;
            CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1),
                                                 CUE_GAME_MODULE_STRUCTURE_VERSION_1, 0U, 0U,
                                                 {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                                  nullptr, 0U}};
            const CueGameModuleResult result = pending.createState(a_module, &state, &diagnostic);
            if (result != CUE_GAME_MODULE_RESULT_SUCCESS || state == nullptr)
            {
                cue::Error error = make_module_callback_error(
                    a_assertContext, result, diagnostic, "Game Module System state creation failed");
                if (state != nullptr)
                {
                    pending.destroyState(state);
                }
                while (!systems.empty())
                {
                    systems.pop_back();
                }
                return cue::Result<std::vector<cue::runtime::RuntimeSystemRegistration>>::failure(
                    std::move(error));
            }
            cue::game_core::RuntimeSystemDescriptor descriptor = pending.descriptor;
            auto system = std::make_unique<GameModuleRuntimeSystem>(a_connection, state, std::move(pending));
            systems.push_back({std::move(descriptor), std::move(system)});
        }
        return cue::Result<std::vector<cue::runtime::RuntimeSystemRegistration>>::success(std::move(systems));
    }
    catch (...)
    {
        terminate_game_module_exception(a_assertContext);
    }
}
} // namespace

namespace cue::runtime_host
{
class GameModuleConnectionFactory final
{
  public:
    [[nodiscard]] static std::shared_ptr<GameModuleConnection> create(
        std::shared_ptr<GameModuleCodeLifetime> a_codeLifetime, const CueGameModuleApiV1 &a_api,
        CueGameModuleHandle a_module)
    {
        return std::shared_ptr<GameModuleConnection>(
            new GameModuleConnection(std::move(a_codeLifetime), a_api, a_module));
    }
};

GameModuleConnection::GameModuleConnection(std::shared_ptr<GameModuleCodeLifetime> a_codeLifetime,
                                           const CueGameModuleApiV1 &a_api,
                                           CueGameModuleHandle a_module) noexcept
    : m_codeLifetime(std::move(a_codeLifetime)), m_api(&a_api), m_module(a_module)
{
}

GameModuleConnection::~GameModuleConnection() noexcept
{
    if (m_module != nullptr)
    {
        m_api->destroyModule(m_module);
    }
}

PreparedGameModule::PreparedGameModule(std::shared_ptr<GameModuleConnection> a_connection,
                                       std::unique_ptr<schema::SchemaRegistryIdentitySource> a_schemaIdentitySource,
                                       std::unique_ptr<schema::SchemaRegistry> a_schemaRegistry,
                                       std::vector<runtime::RuntimeSystemRegistration> a_systems) noexcept
    : m_connection(std::move(a_connection)), m_schemaIdentitySource(std::move(a_schemaIdentitySource)),
      m_schemaRegistry(std::move(a_schemaRegistry)), m_systems(std::move(a_systems))
{
}

PreparedGameModule::~PreparedGameModule() noexcept
{
    while (!m_systems.empty())
    {
        m_systems.pop_back();
    }
}

PreparedGameModule &PreparedGameModule::operator=(PreparedGameModule &&a_other) noexcept
{
    if (this == &a_other)
    {
        return *this;
    }
    while (!m_systems.empty())
    {
        m_systems.pop_back();
    }
    m_schemaRegistry.reset();
    m_schemaIdentitySource.reset();
    m_connection.reset();
    m_connection = std::move(a_other.m_connection);
    m_schemaIdentitySource = std::move(a_other.m_schemaIdentitySource);
    m_schemaRegistry = std::move(a_other.m_schemaRegistry);
    m_systems = std::move(a_other.m_systems);
    return *this;
}

const schema::SchemaRegistry &PreparedGameModule::schema_registry() const noexcept
{
    return *m_schemaRegistry;
}

std::vector<runtime::RuntimeSystemRegistration> &PreparedGameModule::systems() noexcept
{
    return m_systems;
}

Result<PreparedGameModule> connect_game_module(
    GameModuleQueryProvider &a_provider, std::string_view a_expectedProjectId,
    std::unique_ptr<schema::SchemaRegistryIdentitySource> a_identitySource,
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!a_identitySource)
        {
            return Result<PreparedGameModule>::failure(runtime::make_runtime_error(
                a_assertContext, runtime::RuntimeError::InvalidApplicationConfiguration,
                "Game Module connection is missing its Schema Identity Source"));
        }
        Result<ResolvedGameModuleQuery> resolved = a_provider.resolve(a_assertContext);
        if (!resolved)
        {
            return Result<PreparedGameModule>::failure(std::move(*resolved.try_error()));
        }
        if (resolved.try_value()->query == nullptr || !resolved.try_value()->codeLifetime)
        {
            Error error = runtime::make_runtime_error(
                a_assertContext, runtime::RuntimeError::InvalidApplicationConfiguration,
                "Game Module Query Provider returned an incomplete code lease");
            a_identitySource.reset();
            return Result<PreparedGameModule>::failure(std::move(error));
        }
        CueGameModuleQueryOutputV1 queryOutput{sizeof(CueGameModuleQueryOutputV1),
                                               CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr, {0U, 0U}};
        CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1),
                                             CUE_GAME_MODULE_STRUCTURE_VERSION_1, 0U, 0U,
                                             {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                              nullptr, 0U}};
        if (resolved.try_value()->query(CUE_GAME_MODULE_ABI_VERSION_1, &queryOutput, &diagnostic) !=
                CUE_GAME_MODULE_RESULT_SUCCESS ||
            queryOutput.structSize != sizeof(CueGameModuleQueryOutputV1) ||
            queryOutput.version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 || queryOutput.api == nullptr ||
            queryOutput.reserved[0] != 0U || queryOutput.reserved[1] != 0U)
        {
            Error error = a_provider.make_query_contract_error(
                a_assertContext, "Game Module rejected the RuntimeHost ABI");
            a_identitySource.reset();
            return Result<PreparedGameModule>::failure(std::move(error));
        }
        const CueGameModuleApiV1 &api = *queryOutput.api;
        if (api.structSize < sizeof(CueGameModuleApiV1) || api.version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 ||
            api.abiVersion != CUE_GAME_MODULE_ABI_VERSION_1 || api.configuration != host_module_configuration() ||
            api.architecture != CUE_GAME_MODULE_ARCHITECTURE_X64 || api.reserved != 0U ||
            api.reservedTail[0] != 0U || api.reservedTail[1] != 0U || api.reservedTail[2] != 0U ||
            api.reservedTail[3] != 0U || api.projectId.structSize < sizeof(CueGameUuidV1) ||
            api.projectId.version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 ||
            uuid_text(api.projectId) != a_expectedProjectId || api.createModule == nullptr ||
            api.registerSchemas == nullptr || api.registerComponents == nullptr || api.registerSystems == nullptr ||
            api.destroyModule == nullptr)
        {
            Error error = a_provider.make_query_contract_error(
                a_assertContext, "Game Module API identity or lifecycle is incompatible");
            a_identitySource.reset();
            return Result<PreparedGameModule>::failure(std::move(error));
        }
        CueGameModuleHandle moduleHandle = nullptr;
        const CueGameModuleResult createModuleResult = api.createModule(&moduleHandle, &diagnostic);
        if (createModuleResult != CUE_GAME_MODULE_RESULT_SUCCESS || moduleHandle == nullptr)
        {
            Error error = make_module_callback_error(
                a_assertContext, createModuleResult, diagnostic, "Game Module Project Scope creation failed");
            a_identitySource.reset();
            if (moduleHandle != nullptr)
            {
                api.destroyModule(moduleHandle);
            }
            return Result<PreparedGameModule>::failure(std::move(error));
        }
        std::shared_ptr<GameModuleConnection> connection = GameModuleConnectionFactory::create(
            std::move(resolved.try_value()->codeLifetime), api, moduleHandle);
        PreparedGameModule prepared(
            std::move(connection), std::move(a_identitySource), nullptr, {});
        RegistrationContext registration;
        Result<void> schemas = call_registration(api.registerSchemas, moduleHandle, registration,
                                                 RegistrationStage::Schemas, a_assertContext);
        Result<void> components = schemas ? call_registration(api.registerComponents, moduleHandle, registration,
                                                              RegistrationStage::Components, a_assertContext)
                                         : Result<void>::failure(std::move(*schemas.try_error()));
        Result<void> registeredSystems =
            components ? call_registration(api.registerSystems, moduleHandle, registration,
                                           RegistrationStage::Systems, a_assertContext)
                       : Result<void>::failure(std::move(*components.try_error()));
        if (!registeredSystems)
        {
            return Result<PreparedGameModule>::failure(std::move(*registeredSystems.try_error()));
        }
        schema::SchemaRegistryBuilder builder(*prepared.m_schemaIdentitySource, a_assertContext);
        Result<void> coreSchemas = runtime::add_runtime_schema_types(builder, a_assertContext);
        if (!coreSchemas)
        {
            return Result<PreparedGameModule>::failure(std::move(*coreSchemas.try_error()));
        }
        Result<void> rendererSchemas = renderer::add_renderer_schema_types(builder, a_assertContext);
        if (!rendererSchemas)
        {
            return Result<PreparedGameModule>::failure(std::move(*rendererSchemas.try_error()));
        }
        Result<std::unique_ptr<schema::SchemaRegistry>> registry = builder.seal();
        if (!registry)
        {
            return Result<PreparedGameModule>::failure(std::move(*registry.try_error()));
        }
        prepared.m_schemaRegistry = std::move(*registry.try_value());
        Result<std::vector<runtime::RuntimeSystemRegistration>> systems =
            create_systems(prepared.m_connection, moduleHandle, std::move(registration.systems), a_assertContext);
        if (!systems)
        {
            return Result<PreparedGameModule>::failure(std::move(*systems.try_error()));
        }
        prepared.m_systems = std::move(*systems.try_value());
        return Result<PreparedGameModule>::success(std::move(prepared));
    }
    catch (...)
    {
        terminate_game_module_exception(a_assertContext);
    }
}
} // namespace cue::runtime_host
