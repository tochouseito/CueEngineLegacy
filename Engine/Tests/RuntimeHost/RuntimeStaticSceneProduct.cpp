#include <Cue/Foundation/Result.h>
#include <Cue/GameModule/GameModuleAbi.h>
#include <Cue/RuntimeHost/RuntimeHostProcess.h>
#include <Cue/RuntimeHost/StaticRuntimePackage.h>

#include <string_view>

namespace
{
constexpr std::string_view k_projectId = "41234567-89ab-4cde-8f01-23456789abcd";

/// @brief Test Package相対Runtime Dataと静的Game Moduleから起動入力を構築する
[[nodiscard]] cue::Result<cue::runtime_host::RuntimeHostStartup> make_startup(
    const cue::AssertContext &a_assertContext) noexcept
{
    return cue::runtime_host::load_static_runtime_package(
        &cue_game_module_query, k_projectId, cue::runtime_host::StaticRuntimeTrustMode::UnsignedLocal, {},
        a_assertContext);
}
} // namespace

/// @brief Release Static Productと共通RuntimeHost Scene画素Gateを接続する
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    const cue::runtime_host::RuntimeHostProcessDescriptor descriptor = {&make_startup, true};
    return cue::runtime_host::run_runtime_host_process(a_argumentCount, a_arguments, descriptor);
}
