#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/RuntimeHost/RuntimeHostStartup.h>
#include <Cue/Scene/Instantiation.h>

#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::runtime_host
{
/// @brief Package Trust検証後に共通使用するRuntime Scene Readerを単体検証可能にする
[[nodiscard]] Result<scene::SceneSnapshot> parse_runtime_scene_data(
    std::string_view a_bytes, std::string_view a_expectedSceneId,
    const AssertContext &a_assertContext) noexcept;
/// @brief Executable親DirectoryのModular Packageを検証しDynamic Query Provider経由で起動入力を構築する
[[nodiscard]] Result<RuntimeHostStartup> load_runtime_package(
    const AssertContext &a_assertContext) noexcept;
} // namespace cue::runtime_host
