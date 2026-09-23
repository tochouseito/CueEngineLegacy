#pragma once

#include <Cue/Foundation/Result.h>

namespace cue
{
class AssertContext;
}

namespace cue::distribution
{
struct MinimumToolchain;

/// @brief Developer Source SDK導入前にHost Toolchainの存在と最低Versionを読取り専用で検証する
[[nodiscard]] Result<void> validate_windows_developer_prerequisites(
    const MinimumToolchain &a_minimumToolchain, const AssertContext &a_assertContext) noexcept;
} // namespace cue::distribution
