#pragma once

#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <filesystem>

namespace cue
{
class AssertContext;
}

namespace cue::distribution::windows_detail
{
/// @brief 検証済みFile HandleからSUBST割当てを解決済みの正規DOS Pathを取得する
[[nodiscard]] Result<std::filesystem::path> resolve_stable_dos_path(
    std::uintptr_t a_handle, const AssertContext &a_assertContext) noexcept;
} // namespace cue::distribution::windows_detail
