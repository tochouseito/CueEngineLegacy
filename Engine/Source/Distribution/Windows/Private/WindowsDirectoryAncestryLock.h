#pragma once

#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::distribution::windows_detail
{
/// @brief Process Image解決中のDirectory祖先置換をNative Handle群で拒否する
class WindowsDirectoryAncestryLock final
{
  public:
    WindowsDirectoryAncestryLock() = delete;
    WindowsDirectoryAncestryLock(const WindowsDirectoryAncestryLock &) = delete;
    WindowsDirectoryAncestryLock &operator=(const WindowsDirectoryAncestryLock &) = delete;
    /// @brief 祖先Directory Handle群の一意所有権を移動する
    WindowsDirectoryAncestryLock(WindowsDirectoryAncestryLock &&a_other) noexcept;
    /// @brief 現在の祖先Lockを解放して所有権を移動する
    WindowsDirectoryAncestryLock &operator=(WindowsDirectoryAncestryLock &&a_other) noexcept;
    /// @brief 全祖先Directory Handleを閉じてRename禁止を解除する
    ~WindowsDirectoryAncestryLock() noexcept;

    /// @brief Volume Root直下から対象Directoryまでを非Reparse Handleで固定する
    [[nodiscard]] static Result<WindowsDirectoryAncestryLock> acquire(
        const std::filesystem::path &a_directory, const AssertContext &a_assertContext) noexcept;

    /// @brief 所有する全祖先Directory Handleを明示的に閉じる
    void release() noexcept;
    /// @brief 所有する全祖先Directory Handleを閉じずに呼出元へ移動する
    [[nodiscard]] std::vector<std::uintptr_t> take_handles() noexcept;

  private:
    /// @brief 検証済みNative Handle値の一意所有権を取得する
    explicit WindowsDirectoryAncestryLock(std::vector<std::uintptr_t> &&a_handles) noexcept;

    std::vector<std::uintptr_t> m_handles;
};
} // namespace cue::distribution::windows_detail
