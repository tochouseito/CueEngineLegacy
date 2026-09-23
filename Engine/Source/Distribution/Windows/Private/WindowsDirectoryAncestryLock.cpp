#include "WindowsDirectoryAncestryLock.h"

#include <Cue/Distribution/Error.h>
#include <Cue/Foundation/Assert.h>

#include <Windows.h>

#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace
{
/// @brief Filesystem PathをWindows Extended-length Pathへ変換する
[[nodiscard]] std::wstring win32_path(const std::filesystem::path &a_path)
{
    std::filesystem::path preferred = a_path;
    preferred.make_preferred();
    const std::wstring path = preferred.native();
    if (path.starts_with(L"\\\\?\\"))
    {
        return path;
    }
    if (path.starts_with(L"\\\\"))
    {
        return L"\\\\?\\UNC\\" + path.substr(2U);
    }
    return L"\\\\?\\" + path;
}

/// @brief 祖先Directory固定失敗をDistribution Errorへ変換する
[[nodiscard]] cue::Error ancestry_error(const cue::AssertContext &a_assertContext,
                                        cue::distribution::DistributionError a_code,
                                        std::string_view a_summary) noexcept
{
    return cue::distribution::make_distribution_error(a_assertContext, a_code, a_summary);
}

/// @brief Allocation失敗をDistribution Fatalへ変換する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Windows Directory ancestry lock allocation failed");
    std::abort();
}
} // namespace

namespace cue::distribution::windows_detail
{
WindowsDirectoryAncestryLock::WindowsDirectoryAncestryLock(std::vector<std::uintptr_t> &&a_handles) noexcept
    : m_handles(std::move(a_handles))
{
}

WindowsDirectoryAncestryLock::WindowsDirectoryAncestryLock(WindowsDirectoryAncestryLock &&a_other) noexcept
    : m_handles(std::move(a_other.m_handles))
{
    a_other.m_handles.clear();
}

WindowsDirectoryAncestryLock &WindowsDirectoryAncestryLock::operator=(WindowsDirectoryAncestryLock &&a_other) noexcept
{
    if (this != &a_other)
    {
        release();
        m_handles = std::move(a_other.m_handles);
        a_other.m_handles.clear();
    }
    return *this;
}

WindowsDirectoryAncestryLock::~WindowsDirectoryAncestryLock() noexcept
{
    release();
}

Result<WindowsDirectoryAncestryLock> WindowsDirectoryAncestryLock::acquire(
    const std::filesystem::path &a_directory, const AssertContext &a_assertContext) noexcept
{
    try
    {
        const std::filesystem::path directory = a_directory.lexically_normal();
        std::filesystem::path current = directory.root_path();
        if (!directory.is_absolute() || current.empty() || directory.relative_path().empty())
        {
            return Result<WindowsDirectoryAncestryLock>::failure(
                ancestry_error(a_assertContext, DistributionError::InvalidInstallState,
                               "Worker ancestry lock requires an absolute directory path"));
        }

        std::vector<std::uintptr_t> handles;
        handles.reserve(16U);
        for (const std::filesystem::path &component : directory.relative_path())
        {
            current /= component;
            const HANDLE handle = CreateFileW(
                win32_path(current).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                const DWORD nativeError = GetLastError();
                WindowsDirectoryAncestryLock owner(std::move(handles));
                return Result<WindowsDirectoryAncestryLock>::failure(
                    ancestry_error(a_assertContext,
                                   nativeError == ERROR_SHARING_VIOLATION
                                       ? DistributionError::InstallConflict
                                       : DistributionError::PlatformOperationFailed,
                                   "Worker directory ancestry could not be locked for process launch"));
            }

            FILE_ATTRIBUTE_TAG_INFO information{};
            if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &information, sizeof(information)) ==
                    FALSE ||
                (information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
                (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            {
                static_cast<void>(CloseHandle(handle));
                WindowsDirectoryAncestryLock owner(std::move(handles));
                return Result<WindowsDirectoryAncestryLock>::failure(
                    ancestry_error(a_assertContext, DistributionError::InstallConflict,
                                   "Worker directory ancestry contains an invalid or reparse entry"));
            }
            try
            {
                handles.push_back(reinterpret_cast<std::uintptr_t>(handle));
            }
            catch (...)
            {
                static_cast<void>(CloseHandle(handle));
                throw;
            }
        }
        return Result<WindowsDirectoryAncestryLock>::success(WindowsDirectoryAncestryLock(std::move(handles)));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Windows Directory ancestry lock failed unexpectedly");
        std::abort();
    }
}

void WindowsDirectoryAncestryLock::release() noexcept
{
    for (const std::uintptr_t handle : m_handles)
    {
        static_cast<void>(CloseHandle(reinterpret_cast<HANDLE>(handle)));
    }
    m_handles.clear();
}

std::vector<std::uintptr_t> WindowsDirectoryAncestryLock::take_handles() noexcept
{
    std::vector<std::uintptr_t> handles = std::move(m_handles);
    m_handles.clear();
    return handles;
}
} // namespace cue::distribution::windows_detail
