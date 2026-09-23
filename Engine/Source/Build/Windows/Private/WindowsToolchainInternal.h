#pragma once

#include <Cue/Build/Toolchain.h>

#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::detail
{
/// @brief 指定ExecutableをGit for Windows候補としてIdentityとVersionまで検査する
[[nodiscard]] BuildToolCandidate probe_git_for_windows(std::string_view a_path, std::string_view a_installationRoot,
                                                       const AssertContext &a_assertContext) noexcept;
} // namespace cue::detail
