#include "WindowsStablePath.h"

#include <Cue/Distribution/Error.h>
#include <Cue/Foundation/Assert.h>

#include <Windows.h>

#include <cstdlib>
#include <new>
#include <string>

namespace
{
/// @brief Stable Path解決失敗をDistribution Errorへ変換する
[[nodiscard]] cue::Error stable_path_error(const cue::AssertContext &a_assertContext) noexcept
{
    return cue::distribution::make_distribution_error(
        a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
        "Worker executable canonical DOS path could not be resolved");
}

/// @brief Allocation失敗をDistribution Fatalへ変換する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Windows stable path allocation failed");
    std::abort();
}
} // namespace

namespace cue::distribution::windows_detail
{
Result<std::filesystem::path> resolve_stable_dos_path(
    std::uintptr_t a_handle, const AssertContext &a_assertContext) noexcept
{
    try
    {
        const HANDLE handle = reinterpret_cast<HANDLE>(a_handle);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        {
            return Result<std::filesystem::path>::failure(stable_path_error(a_assertContext));
        }

        constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
        const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0U, flags);
        if (required == 0U)
        {
            return Result<std::filesystem::path>::failure(stable_path_error(a_assertContext));
        }

        std::wstring path(required, L'\0');
        const DWORD written = GetFinalPathNameByHandleW(handle, path.data(), required, flags);
        if (written == 0U || written >= required)
        {
            return Result<std::filesystem::path>::failure(stable_path_error(a_assertContext));
        }
        path.resize(written);
        if (!path.starts_with(L"\\\\?\\"))
        {
            return Result<std::filesystem::path>::failure(stable_path_error(a_assertContext));
        }
        return Result<std::filesystem::path>::success(std::filesystem::path(std::move(path)));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Windows stable path resolution failed unexpectedly");
        std::abort();
    }
}
} // namespace cue::distribution::windows_detail
