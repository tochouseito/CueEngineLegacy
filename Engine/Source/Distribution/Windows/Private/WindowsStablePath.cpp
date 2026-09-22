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

/// @brief Process Image検証失敗をDistribution Errorへ変換する
[[nodiscard]] cue::Error process_image_error(const cue::AssertContext &a_assertContext) noexcept
{
    return cue::distribution::make_distribution_error(
        a_assertContext, cue::distribution::DistributionError::InstallConflict,
        "Suspended Worker process image does not match the verified executable");
}

/// @brief 二つのFile Handleが同じVolume上の同一File IDを指すか確認する
[[nodiscard]] bool has_same_file_identity(HANDLE a_left, HANDLE a_right) noexcept
{
    BY_HANDLE_FILE_INFORMATION left{};
    BY_HANDLE_FILE_INFORMATION right{};
    return GetFileInformationByHandle(a_left, &left) != FALSE && GetFileInformationByHandle(a_right, &right) != FALSE &&
           left.dwVolumeSerialNumber == right.dwVolumeSerialNumber && left.nFileIndexHigh == right.nFileIndexHigh &&
           left.nFileIndexLow == right.nFileIndexLow;
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

Result<void> verify_suspended_process_image(std::uintptr_t a_expectedFileHandle, std::uintptr_t a_processHandle,
                                            const AssertContext &a_assertContext) noexcept
{
    try
    {
        const HANDLE expected = reinterpret_cast<HANDLE>(a_expectedFileHandle);
        const HANDLE process = reinterpret_cast<HANDLE>(a_processHandle);
        if (expected == nullptr || expected == INVALID_HANDLE_VALUE || process == nullptr ||
            process == INVALID_HANDLE_VALUE)
        {
            return Result<void>::failure(process_image_error(a_assertContext));
        }

        std::wstring nativePath(32768U, L'\0');
        DWORD length = static_cast<DWORD>(nativePath.size());
        if (QueryFullProcessImageNameW(process, PROCESS_NAME_NATIVE, nativePath.data(), &length) == FALSE ||
            length == 0U || length >= nativePath.size())
        {
            return Result<void>::failure(process_image_error(a_assertContext));
        }
        nativePath.resize(length);
        if (!nativePath.starts_with(L"\\Device\\"))
        {
            return Result<void>::failure(process_image_error(a_assertContext));
        }

        const std::wstring globalRootPath = L"\\\\?\\GLOBALROOT" + nativePath;
        const HANDLE mappedImage = CreateFileW(globalRootPath.c_str(), FILE_READ_ATTRIBUTES,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mappedImage == INVALID_HANDLE_VALUE)
        {
            return Result<void>::failure(process_image_error(a_assertContext));
        }
        const bool matches = has_same_file_identity(expected, mappedImage);
        static_cast<void>(CloseHandle(mappedImage));
        return matches ? Result<void>::success() : Result<void>::failure(process_image_error(a_assertContext));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Windows process image verification failed unexpectedly");
        std::abort();
    }
}
} // namespace cue::distribution::windows_detail
