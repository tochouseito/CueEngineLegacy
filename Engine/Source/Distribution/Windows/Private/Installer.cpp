#include <Cue/Distribution/Windows/Installer.h>

#include <Cue/Distribution/Error.h>
#include <Cue/Distribution/InstallState.h>
#include <Cue/Distribution/Manifest.h>
#include <Cue/Distribution/Publisher.h>
#include <Cue/Foundation/Assert.h>

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_maximumManifestBytes = 16U * 1024U * 1024U;
constexpr std::uint64_t k_maximumPayloadBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t k_maximumInstallEntries = 4096U;
constexpr std::wstring_view k_manifestName = L"CueEngineDistribution.json";
constexpr std::wstring_view k_payloadMarkerName = L"CueEnginePayload.complete.json";
constexpr std::wstring_view k_probeMarkerName = L"CueEngineProbe.complete.json";
constexpr std::wstring_view k_workerMarkerName = L"CueEngineInstallWorker.complete.json";
constexpr DWORD k_probeTimeoutMilliseconds = 120000U;
constexpr DWORD k_probePollMilliseconds = 25U;
constexpr DWORD k_workerGateTimeoutMilliseconds = 120000U;
constexpr std::size_t k_maximumProbeDiagnosticBytes = 4096U;
constexpr std::size_t k_maximumProbeDrainBytesPerPoll = 64U * 1024U;

/// @brief Native Handleを一意所有して全経路でCloseする
class HandleOwner final
{
  public:
    /// @brief 無効Handleから空Ownerを構築する
    HandleOwner() noexcept = default;
    /// @brief Native Handleの一意所有権を取得する
    explicit HandleOwner(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    HandleOwner(const HandleOwner &) = delete;
    HandleOwner &operator=(const HandleOwner &) = delete;
    /// @brief Native Handle所有権を移動する
    HandleOwner(HandleOwner &&a_other) noexcept : m_handle(std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE))
    {
    }
    /// @brief 既存Handleを解放してNative Handle所有権を移動する
    HandleOwner &operator=(HandleOwner &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset();
            m_handle = std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    /// @brief 所有Native Handleを閉じる
    ~HandleOwner() noexcept
    {
        reset();
    }
    /// @brief 所有Handleを返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }
    /// @brief 有効Handleを所有しているか返す
    [[nodiscard]] bool valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }
    /// @brief CloseせずNative Handle所有権を解放する
    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(m_handle, INVALID_HANDLE_VALUE);
    }
    /// @brief 所有Handleを閉じて無効化する
    void reset() noexcept
    {
        if (valid())
        {
            static_cast<void>(CloseHandle(m_handle));
        }
        m_handle = INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

/// @brief Install Root全体のWriter所有権をProcess間で保持する
class ControlLease final
{
  public:
    ControlLease() = delete;
    ControlLease(const ControlLease &) = delete;
    ControlLease &operator=(const ControlLease &) = delete;
    /// @brief Lock済みHandleの所有権を移動する
    ControlLease(ControlLease &&a_other) noexcept : m_handle(std::move(a_other.m_handle))
    {
    }
    /// @brief 現在のLeaseを閉じて排他Open Handleの所有権を移動する
    ControlLease &operator=(ControlLease &&a_other) noexcept
    {
        if (this != &a_other)
        {
            m_handle = std::move(a_other.m_handle);
        }
        return *this;
    }
    /// @brief Lock済みHandleを所有する
    explicit ControlLease(HandleOwner a_handle) noexcept : m_handle(std::move(a_handle))
    {
    }
    /// @brief 排他Open Handleを閉じてLeaseを解放する
    ~ControlLease() noexcept = default;
    /// @brief Probeへ継承するLock File Handleを返す
    [[nodiscard]] HANDLE handle() const noexcept
    {
        return m_handle.get();
    }
    /// @brief 排他Control Leaseを明示的に解放する
    void release() noexcept
    {
        m_handle.reset();
    }

  private:
    HandleOwner m_handle;
};

/// @brief 検証済みBundle ManifestとDigestを一つのSnapshotとして保持する
struct BundleSnapshot final
{
    cue::distribution::DistributionManifest manifest;
    std::string manifestBytes;
    std::string manifestDigest;
    cue::distribution::DistributionFileEntry worker;
    std::string workerId;
};

/// @brief Registry Fileの読取状態をValid／Missing／Corruptへ分離する
struct RegistrySnapshot final
{
    std::optional<cue::distribution::InstalledVersionsRegistry> registry;
    std::vector<std::byte> bytes;
    bool wasMissing = false;
};

/// @brief Registry EntryとVersion／Worker Evidenceを同じ検証Snapshotとして保持する
struct InstalledVersionSnapshot final
{
    cue::distribution::InstalledVersionsRegistry registry;
    cue::distribution::InstalledVersionEntry entry;
    BundleSnapshot bundle;
};

/// @brief Allocation失敗をDistribution Fatalへ変換する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Cue.Distribution.Windows Installer allocation failed");
}

/// @brief 予期しない例外をDistribution Fatalへ変換する
[[noreturn]] void terminate_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Cue.Distribution.Windows Installer unexpected exception");
}

/// @brief Install Service失敗を安定Domain Errorへ変換する
[[nodiscard]] cue::Error install_error(const cue::AssertContext &a_assertContext,
                                       cue::distribution::DistributionError a_code, std::string_view a_summary) noexcept
{
    return cue::distribution::make_distribution_error(a_assertContext, a_code, a_summary);
}

/// @brief UTF-8文字列をWin32 UTF-16へ厳密変換する
[[nodiscard]] std::optional<std::wstring> to_wide(std::string_view a_value)
{
    if (a_value.empty())
    {
        return std::wstring{};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_value.data(),
                                         static_cast<int>(a_value.size()), nullptr, 0);
    if (size <= 0)
    {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_value.data(), static_cast<int>(a_value.size()),
                            result.data(), size) != size)
    {
        return std::nullopt;
    }
    return result;
}

/// @brief UTF-16文字列をUTF-8へ厳密変換する
[[nodiscard]] std::optional<std::string> to_utf8(std::wstring_view a_value)
{
    if (a_value.empty())
    {
        return std::string{};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_value.data(),
                                         static_cast<int>(a_value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_value.data(), static_cast<int>(a_value.size()),
                            result.data(), size, nullptr, nullptr) != size)
    {
        return std::nullopt;
    }
    return result;
}

/// @brief Absolute PathをWin32比較用の正規形へ変換する
[[nodiscard]] std::optional<std::filesystem::path> normalize_absolute(std::string_view a_value)
{
    if (a_value.empty() || a_value.find('\0') != std::string_view::npos)
    {
        return std::nullopt;
    }
    auto wide = to_wide(a_value);
    if (!wide || wide->empty() || wide->find(L'\0') != std::wstring::npos)
    {
        return std::nullopt;
    }
    std::filesystem::path path(*wide);
    if (!path.is_absolute())
    {
        return std::nullopt;
    }
    return path.lexically_normal();
}

/// @brief Absolute PathをWin32 Extended-length Pathへ変換する
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

/// @brief Standard LibraryのFilesystem操作へ渡すExtended-length Pathを返す
[[nodiscard]] std::filesystem::path extended_filesystem_path(const std::filesystem::path &a_path)
{
    return std::filesystem::path(win32_path(a_path));
}

/// @brief Native File Handleが参照する正規DOS Pathを取得する
[[nodiscard]] std::optional<std::wstring> handle_path(HANDLE a_handle)
{
    const DWORD required = GetFinalPathNameByHandleW(a_handle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0U)
    {
        return std::nullopt;
    }
    std::wstring result(required, L'\0');
    const DWORD written =
        GetFinalPathNameByHandleW(a_handle, result.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0U || written >= required)
    {
        return std::nullopt;
    }
    result.resize(written);
    return result;
}

/// @brief UUID v4をlowercase canonical文字列として生成する
[[nodiscard]] std::optional<std::string> make_uuid()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
    {
        return std::nullopt;
    }
    wchar_t text[39]{};
    if (StringFromGUID2(guid, text, static_cast<int>(std::size(text))) != 39)
    {
        return std::nullopt;
    }
    std::string result;
    result.reserve(36U);
    for (std::size_t index = 1U; index < 37U; ++index)
    {
        wchar_t value = text[index];
        if (value >= L'A' && value <= L'F')
        {
            value = static_cast<wchar_t>(value + (L'a' - L'A'));
        }
        result.push_back(static_cast<char>(value));
    }
    return result;
}

/// @brief Journal Atomic ReplaceがPublish前に残したCanonical一時File名か返す
[[nodiscard]] bool is_journal_atomic_temporary(const std::filesystem::path &a_path)
{
    const auto name = to_utf8(a_path.filename().native());
    constexpr std::string_view separator = ".json.tmp-";
    if (!name || name->size() != 36U + separator.size() + 36U ||
        std::string_view(*name).substr(36U, separator.size()) != separator)
    {
        return false;
    }
    return cue::distribution::is_canonical_bundle_id(std::string_view(*name).substr(0U, 36U)) &&
           cue::distribution::is_canonical_bundle_id(std::string_view(*name).substr(36U + separator.size()));
}

/// @brief Probe Marker Atomic ReplaceがPublish前に残したCanonical一時File名か返す
[[nodiscard]] bool is_probe_marker_atomic_temporary(const std::filesystem::path &a_path)
{
    const auto name = to_utf8(a_path.filename().native());
    constexpr std::string_view prefix = "CueEngineProbe.complete.json.tmp-";
    return name && name->size() == prefix.size() + 36U && name->starts_with(prefix) &&
           cue::distribution::is_canonical_bundle_id(std::string_view(*name).substr(prefix.size()));
}

/// @brief Path EntryがReparse Pointでない既存Directoryか返す
[[nodiscard]] bool is_plain_directory(const std::filesystem::path &a_path) noexcept
{
    const DWORD attributes = GetFileAttributesW(win32_path(a_path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

/// @brief Path EntryがReparse Pointでない既存Regular Fileか返す
[[nodiscard]] bool is_plain_file(const std::filesystem::path &a_path) noexcept
{
    const DWORD attributes = GetFileAttributesW(win32_path(a_path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

/// @brief VersionPublished再開前にCanonicalなProbe Marker一時Fileだけを回収する
[[nodiscard]] cue::Result<void> cleanup_probe_marker_temporaries(const std::filesystem::path &a_versionRoot,
                                                                 const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::vector<std::filesystem::path> temporaries;
        std::size_t entryCount = 0U;
        for (const std::filesystem::directory_entry &entry :
             std::filesystem::directory_iterator(extended_filesystem_path(a_versionRoot)))
        {
            ++entryCount;
            if (entryCount > k_maximumInstallEntries)
            {
                return cue::Result<void>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::ResourceLimitExceeded,
                                  "Published Version entry count exceeds its limit"));
            }
            if (is_plain_file(entry.path()) && is_probe_marker_atomic_temporary(entry.path()))
            {
                temporaries.push_back(entry.path());
            }
        }
        for (const std::filesystem::path &temporary : temporaries)
        {
            if (DeleteFileW(win32_path(temporary).c_str()) == FALSE && GetLastError() != ERROR_FILE_NOT_FOUND)
            {
                return cue::Result<void>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                                  "Abandoned Install Probe Marker temporary file could not be removed"));
            }
        }
        return cue::Result<void>::success();
    }
    catch (...)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Published Version entries could not be enumerated"));
    }
}

/// @brief Install Rootまでの既存DirectoryをHandleで辿りReparse Point経由の書込を拒否する
[[nodiscard]] bool has_plain_existing_ancestry(const std::filesystem::path &a_path) noexcept
{
    std::filesystem::path current = a_path.root_path();
    if (current.empty())
    {
        return false;
    }
    for (const std::filesystem::path &component : a_path.relative_path())
    {
        current /= component;
        const DWORD attributes = GetFileAttributesW(win32_path(current).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        HandleOwner handle(CreateFileW(win32_path(current).c_str(), FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        FILE_ATTRIBUTE_TAG_INFO information{};
        if (!handle.valid() ||
            GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &information, sizeof(information)) ==
                FALSE ||
            (information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        {
            return false;
        }
    }
    return true;
}

/// @brief PathがLocal Fixed Volume上にあるか返す
[[nodiscard]] bool is_local_fixed_path(const std::filesystem::path &a_path) noexcept
{
    std::filesystem::path probe = a_path;
    while (!probe.empty() && GetFileAttributesW(win32_path(probe).c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        probe = probe.parent_path();
    }
    if (probe.empty())
    {
        return false;
    }
    std::array<wchar_t, MAX_PATH + 1U> volume{};
    return GetVolumePathNameW(win32_path(probe).c_str(), volume.data(), static_cast<DWORD>(volume.size())) != FALSE &&
           GetDriveTypeW(volume.data()) == DRIVE_FIXED;
}

/// @brief Installer管理Directoryを作成しReparse Pointへ到達しないことを検証する
[[nodiscard]] cue::Result<void> ensure_managed_directories(const std::filesystem::path &a_installRoot,
                                                           const cue::AssertContext &a_assertContext) noexcept
{
    constexpr std::array<std::wstring_view, 12U> directories = {
        L"State",
        L"Versions",
        L"Operations",
        L"Operations/Journals",
        L"Operations/Staging",
        L"Operations/WorkerStaging",
        L"Operations/Workers",
        L"Operations/ExecutionLeases",
        L"Operations/Evidence",
        L"Operations/Quarantine",
        L"Operations/Quarantine/Journals",
        L"Operations/Quarantine/Versions",
    };
    for (std::wstring_view relative : directories)
    {
        std::filesystem::path path = a_installRoot / relative;
        const DWORD attributes = GetFileAttributesW(win32_path(path).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !is_plain_directory(path))
        {
            return cue::Result<void>::failure(install_error(a_assertContext,
                                                            cue::distribution::DistributionError::InstallConflict,
                                                            "Install managed path is not a plain directory"));
        }
        std::error_code error;
        std::filesystem::create_directories(extended_filesystem_path(path), error);
        if (error || !is_plain_directory(path))
        {
            return cue::Result<void>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                              "Install managed directory could not be created"));
        }
    }
    return cue::Result<void>::success();
}

/// @brief FileにInternet／Restricted ZoneのMark-of-the-Webがないか返す
[[nodiscard]] bool has_no_mark_of_the_web(const std::filesystem::path &a_path) noexcept
{
    std::wstring stream = win32_path(a_path);
    stream.append(L":Zone.Identifier");
    HandleOwner handle(CreateFileW(stream.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid())
    {
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    }
    std::array<char, 4096U> bytes{};
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle.get(), &size) == FALSE || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > bytes.size())
    {
        return false;
    }
    DWORD read = 0U;
    if (ReadFile(handle.get(), bytes.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) == FALSE ||
        read != static_cast<DWORD>(size.QuadPart))
    {
        return false;
    }
    const std::string_view content(bytes.data(), read);
    return content.find("ZoneId=3") == std::string_view::npos && content.find("ZoneId=4") == std::string_view::npos;
}

/// @brief Regular Fileを上限付きで全読込する
[[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const std::filesystem::path &a_path,
                                                            std::uint64_t a_maximumBytes,
                                                            const cue::AssertContext &a_assertContext) noexcept
{
    HandleOwner handle(CreateFileW(win32_path(a_path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.valid())
    {
        return cue::Result<std::vector<std::byte>>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Install file could not be opened"));
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle.get(), &size) == FALSE || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > a_maximumBytes)
    {
        return cue::Result<std::vector<std::byte>>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::ResourceLimitExceeded,
                          "Install file size exceeds its limit"));
    }
    try
    {
        std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0U;
        while (offset < bytes.size())
        {
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024U * 1024U));
            DWORD read = 0U;
            if (ReadFile(handle.get(), bytes.data() + offset, request, &read, nullptr) == FALSE || read == 0U)
            {
                return cue::Result<std::vector<std::byte>>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                                  "Install file could not be read completely"));
            }
            offset += read;
        }
        return cue::Result<std::vector<std::byte>>::success(std::move(bytes));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

/// @brief Byte列をSHA-256へ変換する
[[nodiscard]] cue::Result<std::string> hash_bytes(std::span<const std::byte> a_bytes,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    return cue::distribution::compute_distribution_sha256(a_bytes, a_assertContext);
}

/// @brief File全体を読んでSHA-256を返す
[[nodiscard]] cue::Result<std::string> hash_file(const std::filesystem::path &a_path, std::uint64_t a_maximumBytes,
                                                 const cue::AssertContext &a_assertContext) noexcept
{
    auto bytes = read_file(a_path, a_maximumBytes, a_assertContext);
    if (!bytes)
    {
        return cue::Result<std::string>::failure(std::move(*bytes.try_error()));
    }
    return hash_bytes(*bytes.try_value(), a_assertContext);
}

/// @brief CopyまたはPublish済みManifestが検証時Snapshotと同じDigestか確認する
[[nodiscard]] cue::Result<void> validate_manifest_digest(const std::filesystem::path &a_root,
                                                         const BundleSnapshot &a_bundle,
                                                         const cue::AssertContext &a_assertContext) noexcept
{
    auto digest = hash_file(a_root / k_manifestName, k_maximumManifestBytes, a_assertContext);
    if (!digest || *digest.try_value() != a_bundle.manifestDigest)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::BundleValidationFailed,
                                                        "Bundle Manifest changed after validation"));
    }
    return cue::Result<void>::success();
}

/// @brief Byte列をTemporary Fileへ耐久書込みしてDestinationをAtomic Replaceする
[[nodiscard]] cue::Result<void> write_atomic(const std::filesystem::path &a_path, std::span<const std::byte> a_bytes,
                                             const cue::AssertContext &a_assertContext) noexcept
{
    const DWORD destinationAttributes = GetFileAttributesW(win32_path(a_path).c_str());
    if (destinationAttributes != INVALID_FILE_ATTRIBUTES &&
        ((destinationAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
         (destinationAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U))
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::InstallConflict,
                                                        "Atomic write destination is not a plain file"));
    }
    auto id = make_uuid();
    if (!id)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Atomic write operation ID could not be generated"));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(extended_filesystem_path(a_path.parent_path()), directoryError);
    if (directoryError)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Atomic write parent directory could not be created"));
    }
    std::filesystem::path temporary = a_path;
    temporary += L".tmp-";
    temporary += to_wide(*id).value_or(L"invalid");
    HandleOwner handle(CreateFileW(win32_path(temporary).c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!handle.valid())
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Atomic write temporary file could not be created"));
    }
    std::size_t offset = 0U;
    while (offset < a_bytes.size())
    {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(a_bytes.size() - offset, 1024U * 1024U));
        DWORD written = 0U;
        if (WriteFile(handle.get(), a_bytes.data() + offset, request, &written, nullptr) == FALSE || written != request)
        {
            handle.reset();
            static_cast<void>(DeleteFileW(win32_path(temporary).c_str()));
            return cue::Result<void>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                              "Atomic write temporary file could not be written completely"));
        }
        offset += written;
    }
    if (FlushFileBuffers(handle.get()) == FALSE)
    {
        handle.reset();
        static_cast<void>(DeleteFileW(win32_path(temporary).c_str()));
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Atomic write temporary file could not be flushed"));
    }
    handle.reset();
    if (MoveFileExW(win32_path(temporary).c_str(), win32_path(a_path).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        static_cast<void>(DeleteFileW(win32_path(temporary).c_str()));
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Atomic write publish failed"));
    }
    return cue::Result<void>::success();
}

/// @brief Canonical JSON文字列をAtomic Fileへ耐久書込みする
[[nodiscard]] cue::Result<void> write_atomic_text(const std::filesystem::path &a_path, std::string_view a_bytes,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    const auto *begin = reinterpret_cast<const std::byte *>(a_bytes.data());
    return write_atomic(a_path, std::span(begin, a_bytes.size()), a_assertContext);
}

/// @brief Native Handleが指定Plain Fileを直接参照するか検証する
[[nodiscard]] bool handle_matches_plain_file(HANDLE a_handle, const std::filesystem::path &a_path) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO attributeInfo{};
    const auto finalPath = handle_path(a_handle);
    const std::wstring expected = win32_path(a_path);
    return GetFileInformationByHandleEx(a_handle, FileAttributeTagInfo, &attributeInfo, sizeof(attributeInfo)) !=
               FALSE &&
           (attributeInfo.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U &&
           finalPath && _wcsicmp(finalPath->c_str(), expected.c_str()) == 0;
}

/// @brief Versions直下へ入力可能なCanonical Directory名か返す
[[nodiscard]] bool is_version_directory(std::string_view a_value) noexcept
{
    if (a_value.size() < 42U || a_value.front() != 'v')
    {
        return false;
    }
    const std::size_t separator = a_value.find("--", 1U);
    if (separator == std::string_view::npos)
    {
        return false;
    }
    const std::string_view version = a_value.substr(1U, separator - 1U);
    const std::string_view bundleId = a_value.substr(separator + 2U);
    return cue::distribution::is_canonical_engine_version(version) &&
           cue::distribution::is_canonical_bundle_id(bundleId);
}

/// @brief Fileを作成しないProcess間排他Control Leaseを取得する
[[nodiscard]] cue::Result<ControlLease> acquire_control_lease(const std::filesystem::path &a_installRoot,
                                                              const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path operations = a_installRoot / L"Operations";
    std::error_code error;
    std::filesystem::create_directories(extended_filesystem_path(operations), error);
    if (error || !is_plain_directory(operations))
    {
        return cue::Result<ControlLease>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Install Operations directory could not be created"));
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    const std::filesystem::path lockPath = operations / L"CueEngine.control.lock";
    HandleOwner handle(
        CreateFileW(win32_path(lockPath).c_str(), GENERIC_READ | GENERIC_WRITE, 0U, &security, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid())
    {
        const DWORD errorCode = GetLastError();
        return cue::Result<ControlLease>::failure(
            install_error(a_assertContext,
                          errorCode == ERROR_SHARING_VIOLATION || errorCode == ERROR_LOCK_VIOLATION
                              ? cue::distribution::DistributionError::InstallConflict
                              : cue::distribution::DistributionError::PlatformOperationFailed,
                          errorCode == ERROR_SHARING_VIOLATION || errorCode == ERROR_LOCK_VIOLATION
                              ? "Another Install operation owns the Control Lease"
                              : "Install Control Lease file could not be opened"));
    }
    if (!handle_matches_plain_file(handle.get(), lockPath))
    {
        return cue::Result<ControlLease>::failure(install_error(a_assertContext,
                                                                cue::distribution::DistributionError::InstallConflict,
                                                                "Install Control Lease path is not a plain file"));
    }
    return cue::Result<ControlLease>::success(ControlLease(std::move(handle)));
}

/// @brief 起動検証中だけInstall Writerを拒否する共有Control Leaseを取得する
[[nodiscard]] cue::Result<ControlLease> acquire_shared_control_lease(const std::filesystem::path &a_installRoot,
                                                                     const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path lockPath = a_installRoot / L"Operations" / L"CueEngine.control.lock";
    HandleOwner handle(CreateFileW(win32_path(lockPath).c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid())
    {
        return cue::Result<ControlLease>::failure(install_error(
            a_assertContext,
            GetLastError() == ERROR_SHARING_VIOLATION ? cue::distribution::DistributionError::InstallConflict
                                                      : cue::distribution::DistributionError::PlatformOperationFailed,
            "Install Control Lease could not be acquired for launch validation"));
    }
    if (!handle_matches_plain_file(handle.get(), lockPath))
    {
        return cue::Result<ControlLease>::failure(install_error(a_assertContext,
                                                                cue::distribution::DistributionError::InstallConflict,
                                                                "Install Control Lease path is not a plain file"));
    }
    return cue::Result<ControlLease>::success(ControlLease(std::move(handle)));
}

/// @brief Version固有Execution Lease File Pathを返す
[[nodiscard]] std::filesystem::path execution_lease_path(const std::filesystem::path &a_installRoot,
                                                         std::string_view a_directoryName)
{
    return a_installRoot / L"Operations" / L"ExecutionLeases" / (to_wide(a_directoryName).value_or(L"") + L".lock");
}

/// @brief 排他Control Lease下でVersion固有Execution Lease FileをPlain Fileとして用意する
[[nodiscard]] cue::Result<void> ensure_execution_lease_file(const std::filesystem::path &a_installRoot,
                                                            std::string_view a_directoryName,
                                                            const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path path = execution_lease_path(a_installRoot, a_directoryName);
    HandleOwner handle(CreateFileW(win32_path(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid() || !handle_matches_plain_file(handle.get(), path))
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Version Execution Lease file could not be prepared"));
    }
    return cue::Result<void>::success();
}

/// @brief Child Processへ継承する共有Version Execution Leaseを取得する
[[nodiscard]] cue::Result<HandleOwner> acquire_shared_execution_lease(
    const std::filesystem::path &a_installRoot, std::string_view a_directoryName,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto ensured = ensure_execution_lease_file(a_installRoot, a_directoryName, a_assertContext);
    if (!ensured)
    {
        return cue::Result<HandleOwner>::failure(std::move(*ensured.try_error()));
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const std::filesystem::path path = execution_lease_path(a_installRoot, a_directoryName);
    HandleOwner handle(CreateFileW(win32_path(path).c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid() || !handle_matches_plain_file(handle.get(), path))
    {
        return cue::Result<HandleOwner>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Shared Version Execution Lease could not be acquired"));
    }
    return cue::Result<HandleOwner>::success(std::move(handle));
}

/// @brief 新規起動を止めた状態で使用中Versionの終了を待ち排他Execution Leaseを取得する
[[nodiscard]] cue::Result<HandleOwner> acquire_exclusive_execution_lease(
    const std::filesystem::path &a_installRoot, std::string_view a_directoryName,
    const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path path = execution_lease_path(a_installRoot, a_directoryName);
    const ULONGLONG startedAt = GetTickCount64();
    while (true)
    {
        HandleOwner handle(CreateFileW(win32_path(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0U, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (handle.valid())
        {
            if (!handle_matches_plain_file(handle.get(), path))
            {
                return cue::Result<HandleOwner>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                                  "Version Execution Lease path is not a plain file"));
            }
            return cue::Result<HandleOwner>::success(std::move(handle));
        }
        const DWORD code = GetLastError();
        const bool isBusy = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
        if (!isBusy || GetTickCount64() - startedAt >= k_workerGateTimeoutMilliseconds)
        {
            return cue::Result<HandleOwner>::failure(
                install_error(a_assertContext,
                              isBusy ? cue::distribution::DistributionError::InstalledVersionBusy
                                     : cue::distribution::DistributionError::PlatformOperationFailed,
                              isBusy ? "Installed Version remained in use until the Uninstall timeout"
                                     : "Exclusive Version Execution Lease could not be acquired"));
        }
        Sleep(k_probePollMilliseconds);
    }
}

/// @brief File先頭からWindows x64 PE Machineを検証する
[[nodiscard]] bool is_x64_pe(std::span<const std::byte> a_bytes) noexcept
{
    if (a_bytes.size() < 64U || std::to_integer<unsigned char>(a_bytes[0]) != 'M' ||
        std::to_integer<unsigned char>(a_bytes[1]) != 'Z')
    {
        return false;
    }
    /// @brief PE Header内のlittle-endian 16-bit値を読み取る
    const auto read_u16 = [&a_bytes](std::size_t a_offset) noexcept
    {
        return static_cast<std::uint16_t>(std::to_integer<unsigned char>(a_bytes[a_offset])) |
               static_cast<std::uint16_t>(std::to_integer<unsigned char>(a_bytes[a_offset + 1U]) << 8U);
    };
    /// @brief PE Header内のlittle-endian 32-bit値を読み取る
    const auto read_u32 = [&a_bytes](std::size_t a_offset) noexcept
    {
        return static_cast<std::uint32_t>(std::to_integer<unsigned char>(a_bytes[a_offset])) |
               (static_cast<std::uint32_t>(std::to_integer<unsigned char>(a_bytes[a_offset + 1U])) << 8U) |
               (static_cast<std::uint32_t>(std::to_integer<unsigned char>(a_bytes[a_offset + 2U])) << 16U) |
               (static_cast<std::uint32_t>(std::to_integer<unsigned char>(a_bytes[a_offset + 3U])) << 24U);
    };
    const std::uint32_t offset = read_u32(0x3cU);
    return offset <= a_bytes.size() - 6U && read_u32(offset) == 0x00004550U && read_u16(offset + 4U) == 0x8664U;
}

/// @brief Manifest InventoryのFile Byte、Digest、PE、MOTWを全件検証する
[[nodiscard]] cue::Result<void> validate_inventory(const std::filesystem::path &a_root,
                                                   const cue::distribution::DistributionManifest &a_manifest,
                                                   bool a_allowInstallMarkers,
                                                   const cue::AssertContext &a_assertContext) noexcept
{
    std::set<std::string> expected;
    expected.insert("CueEngineDistribution.json");
    for (const cue::distribution::DistributionFileEntry &file : a_manifest.files)
    {
        const std::filesystem::path path = a_root / to_wide(file.relativePath).value_or(L"");
        if (!is_plain_file(path))
        {
            cue::Error error =
                install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                              "Bundle Inventory contains a missing or reparse file");
            error.add_context(a_assertContext.fatal_handler(), file.relativePath);
            return cue::Result<void>::failure(std::move(error));
        }
        if (!has_no_mark_of_the_web(path))
        {
            cue::Error error =
                install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                              "Bundle Inventory contains a remote-marked file");
            error.add_context(a_assertContext.fatal_handler(), file.relativePath);
            return cue::Result<void>::failure(std::move(error));
        }
        auto bytes = read_file(path, k_maximumPayloadBytes, a_assertContext);
        if (!bytes || bytes.try_value()->size() != file.byteSize)
        {
            return cue::Result<void>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                              "Bundle Inventory file size does not match"));
        }
        auto digest = hash_bytes(*bytes.try_value(), a_assertContext);
        if (!digest || *digest.try_value() != file.sha256)
        {
            return cue::Result<void>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                              "Bundle Inventory file digest does not match"));
        }
        const bool isTool = file.role == cue::distribution::DistributionFileRole::Bootstrap ||
                            file.role == cue::distribution::DistributionFileRole::ProjectHub ||
                            file.role == cue::distribution::DistributionFileRole::Editor ||
                            file.role == cue::distribution::DistributionFileRole::RuntimeHost ||
                            file.role == cue::distribution::DistributionFileRole::Installer ||
                            file.role == cue::distribution::DistributionFileRole::InstallWorker;
        if (isTool && !is_x64_pe(*bytes.try_value()))
        {
            return cue::Result<void>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                              "Bundle Tool is not a Windows x64 PE"));
        }
        expected.insert(file.relativePath);
    }
    try
    {
        const std::filesystem::path traversalRoot = extended_filesystem_path(a_root);
        for (const std::filesystem::directory_entry &entry :
             std::filesystem::recursive_directory_iterator(traversalRoot))
        {
            const DWORD attributes = GetFileAttributesW(win32_path(entry.path()).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            {
                return cue::Result<void>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                                  "Bundle contains an unsupported reparse entry"));
            }
            if (entry.is_directory())
            {
                continue;
            }
            if (!entry.is_regular_file())
            {
                return cue::Result<void>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                                  "Bundle contains an unsupported entry"));
            }
            const std::filesystem::path relative = std::filesystem::relative(entry.path(), traversalRoot);
            auto utf8 = to_utf8(relative.generic_wstring());
            if (!utf8)
            {
                return cue::Result<void>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                                  "Bundle relative path is not valid UTF-8"));
            }
            if (a_allowInstallMarkers &&
                (*utf8 == "CueEnginePayload.complete.json" || *utf8 == "CueEngineProbe.complete.json"))
            {
                continue;
            }
            if (!expected.erase(*utf8))
            {
                return cue::Result<void>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                                  "Bundle contains an unregistered file"));
            }
        }
    }
    catch (...)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Bundle Inventory enumeration failed"));
    }
    if (!expected.empty())
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::BundleValidationFailed,
                                                        "Bundle Inventory is incomplete"));
    }
    return cue::Result<void>::success();
}

/// @brief Bundle Manifestと全Payloadを読取専用で検証する
[[nodiscard]] cue::Result<BundleSnapshot> validate_bundle(const std::filesystem::path &a_bundleRoot,
                                                          bool a_allowInstallMarkers,
                                                          const cue::AssertContext &a_assertContext) noexcept
{
    if (!is_plain_directory(a_bundleRoot) || !is_local_fixed_path(a_bundleRoot))
    {
        return cue::Result<BundleSnapshot>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                          "Bundle Root must be a plain directory on a local fixed drive"));
    }
    const std::filesystem::path manifestPath = a_bundleRoot / k_manifestName;
    if (!is_plain_file(manifestPath) || !has_no_mark_of_the_web(manifestPath))
    {
        return cue::Result<BundleSnapshot>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                          "Bundle Manifest is missing or remote-marked"));
    }
    auto bytes = read_file(manifestPath, k_maximumManifestBytes, a_assertContext);
    if (!bytes)
    {
        return cue::Result<BundleSnapshot>::failure(std::move(*bytes.try_error()));
    }
    const std::string manifestBytes(reinterpret_cast<const char *>(bytes.try_value()->data()),
                                    bytes.try_value()->size());
    auto manifest = cue::distribution::read_distribution_manifest(manifestBytes, a_assertContext);
    if (!manifest)
    {
        return cue::Result<BundleSnapshot>::failure(std::move(*manifest.try_error()));
    }
    auto validated = validate_inventory(a_bundleRoot, *manifest.try_value(), a_allowInstallMarkers, a_assertContext);
    if (!validated)
    {
        return cue::Result<BundleSnapshot>::failure(std::move(*validated.try_error()));
    }
    auto manifestDigest = hash_bytes(*bytes.try_value(), a_assertContext);
    auto workerId = cue::distribution::make_install_worker_id(*manifest.try_value(), a_assertContext);
    if (!manifestDigest || !workerId)
    {
        return cue::Result<BundleSnapshot>::failure(manifestDigest ? std::move(*workerId.try_error())
                                                                   : std::move(*manifestDigest.try_error()));
    }
    /// @brief 検証済みManifestからInstall Worker Inventoryを取得する
    const auto worker = std::ranges::find_if(
        manifest.try_value()->files, [](const cue::distribution::DistributionFileEntry &a_file) noexcept
        { return a_file.role == cue::distribution::DistributionFileRole::InstallWorker; });
    if (worker == manifest.try_value()->files.end())
    {
        return cue::Result<BundleSnapshot>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::MissingRequiredPayload,
                          "Bundle Install Worker is missing"));
    }
    BundleSnapshot result{std::move(*manifest.try_value()), manifestBytes, std::move(*manifestDigest.try_value()),
                          *worker, std::move(*workerId.try_value())};
    return cue::Result<BundleSnapshot>::success(std::move(result));
}

/// @brief Copy完了Fileの内容を耐久化して後続Markerより先に永続化する
[[nodiscard]] cue::Result<void> flush_copied_file(const std::filesystem::path &a_path,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    const std::wstring path = win32_path(a_path);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Copied Install file attributes could not be read"));
    }
    const bool restoreReadOnly = (attributes & FILE_ATTRIBUTE_READONLY) != 0U;
    if (restoreReadOnly && SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) == FALSE)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Copied Install file could not be made writable for flush"));
    }
    bool flushed = false;
    {
        HandleOwner handle(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
        flushed = handle.valid() && FlushFileBuffers(handle.get()) != FALSE;
    }
    if (restoreReadOnly && SetFileAttributesW(path.c_str(), attributes) == FALSE)
    {
        return cue::Result<void>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Copied Install file read-only attribute could not be restored"));
    }
    if (!flushed)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Copied Install file could not be flushed"));
    }
    return cue::Result<void>::success();
}

/// @brief Manifest InventoryだけをOperation固有Version Stagingへ複製する
[[nodiscard]] cue::Result<void> copy_bundle(const std::filesystem::path &a_source,
                                            const std::filesystem::path &a_destination, const BundleSnapshot &a_bundle,
                                            const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code error;
    std::filesystem::create_directories(extended_filesystem_path(a_destination), error);
    if (error)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Version Staging could not be created"));
    }
    std::vector<std::string> paths{"CueEngineDistribution.json"};
    paths.reserve(a_bundle.manifest.files.size() + 1U);
    for (const auto &file : a_bundle.manifest.files)
    {
        paths.push_back(file.relativePath);
    }
    for (const std::string &relative : paths)
    {
        const std::filesystem::path relativePath = to_wide(relative).value_or(L"");
        const std::filesystem::path destination = a_destination / relativePath;
        std::filesystem::create_directories(extended_filesystem_path(destination.parent_path()), error);
        if (error ||
            CopyFileW(win32_path(a_source / relativePath).c_str(), win32_path(destination).c_str(), TRUE) == FALSE)
        {
            return cue::Result<void>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                              "Bundle Payload could not be copied to Version Staging"));
        }
        auto flushed = flush_copied_file(destination, a_assertContext);
        if (!flushed)
        {
            return flushed;
        }
    }
    auto inventory = validate_inventory(a_destination, a_bundle.manifest, false, a_assertContext);
    if (!inventory)
    {
        return inventory;
    }
    return validate_manifest_digest(a_destination, a_bundle, a_assertContext);
}

/// @brief Directoryを同一Volume上の未使用DestinationへAtomic Publishする
[[nodiscard]] cue::Result<void> publish_directory(const std::filesystem::path &a_source,
                                                  const std::filesystem::path &a_destination,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    if (MoveFileExW(win32_path(a_source).c_str(), win32_path(a_destination).c_str(), MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PublishConflict,
                                                        "Install directory Atomic Publish failed"));
    }
    return cue::Result<void>::success();
}

/// @brief Canonical Marker Fileを読んでTextを返す
[[nodiscard]] cue::Result<std::string> read_text(const std::filesystem::path &a_path,
                                                 const cue::AssertContext &a_assertContext) noexcept
{
    auto bytes = read_file(a_path, k_maximumManifestBytes, a_assertContext);
    if (!bytes)
    {
        return cue::Result<std::string>::failure(std::move(*bytes.try_error()));
    }
    try
    {
        return cue::Result<std::string>::success(
            std::string(reinterpret_cast<const char *>(bytes.try_value()->data()), bytes.try_value()->size()));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

/// @brief JournalをCanonical JSONとしてAtomic Publishする
[[nodiscard]] cue::Result<void> write_journal(const std::filesystem::path &a_installRoot,
                                              const cue::distribution::InstallOperationJournal &a_journal,
                                              const cue::AssertContext &a_assertContext) noexcept
{
    auto bytes = cue::distribution::write_install_operation_journal(a_journal, a_assertContext);
    if (!bytes)
    {
        return cue::Result<void>::failure(std::move(*bytes.try_error()));
    }
    const std::filesystem::path path =
        a_installRoot / L"Operations" / L"Journals" / (to_wide(a_journal.operationId).value_or(L"") + L".json");
    return write_atomic_text(path, *bytes.try_value(), a_assertContext);
}

/// @brief Journal Stageを隣接遷移だけ許可して耐久更新する
[[nodiscard]] cue::Result<void> advance_journal(const std::filesystem::path &a_installRoot,
                                                cue::distribution::InstallOperationJournal &a_journal,
                                                cue::distribution::InstallOperationStage a_next,
                                                const cue::AssertContext &a_assertContext) noexcept
{
    if (!cue::distribution::is_valid_install_stage_transition(a_journal.kind, a_journal.stage, a_next))
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::InvalidInstallState,
                                                        "Install Journal stage transition is invalid"));
    }
    a_journal.stage = a_next;
    return write_journal(a_installRoot, a_journal, a_assertContext);
}

/// @brief Journal FileをCanonical値とDigestへ変換する
[[nodiscard]] cue::Result<std::pair<cue::distribution::InstallOperationJournal, std::string>> read_journal(
    const std::filesystem::path &a_path, const cue::AssertContext &a_assertContext) noexcept
{
    auto text = read_text(a_path, a_assertContext);
    if (!text)
    {
        return cue::Result<std::pair<cue::distribution::InstallOperationJournal, std::string>>::failure(
            std::move(*text.try_error()));
    }
    auto journal = cue::distribution::read_install_operation_journal(*text.try_value(), a_assertContext);
    if (!journal)
    {
        return cue::Result<std::pair<cue::distribution::InstallOperationJournal, std::string>>::failure(
            std::move(*journal.try_error()));
    }
    const auto *begin = reinterpret_cast<const std::byte *>(text.try_value()->data());
    auto digest = hash_bytes(std::span(begin, text.try_value()->size()), a_assertContext);
    if (!digest)
    {
        return cue::Result<std::pair<cue::distribution::InstallOperationJournal, std::string>>::failure(
            std::move(*digest.try_error()));
    }
    return cue::Result<std::pair<cue::distribution::InstallOperationJournal, std::string>>::success(
        std::pair(std::move(*journal.try_value()), std::move(*digest.try_value())));
}

/// @brief InstalledVersions.jsonをMissing／Valid／Corruptへ分類する
[[nodiscard]] cue::Result<RegistrySnapshot> read_registry(const std::filesystem::path &a_installRoot,
                                                          const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path path = a_installRoot / L"State" / L"InstalledVersions.json";
    if (GetFileAttributesW(win32_path(path).c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        RegistrySnapshot result;
        result.wasMissing = true;
        return cue::Result<RegistrySnapshot>::success(std::move(result));
    }
    if (!is_plain_file(path))
    {
        return cue::Result<RegistrySnapshot>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                          "Installed Versions Registry path is not a plain file"));
    }
    auto bytes = read_file(path, k_maximumManifestBytes, a_assertContext);
    if (!bytes)
    {
        return cue::Result<RegistrySnapshot>::failure(std::move(*bytes.try_error()));
    }
    const std::string text(reinterpret_cast<const char *>(bytes.try_value()->data()), bytes.try_value()->size());
    auto registry = cue::distribution::read_installed_versions_registry(text, a_assertContext);
    RegistrySnapshot result;
    result.bytes = std::move(*bytes.try_value());
    if (registry)
    {
        result.registry = std::move(*registry.try_value());
    }
    constexpr std::string_view knownSchemaPrefix = "{\"schemaVersion\":1,";
    const bool identifiesKnownSchema = text == "{\"schemaVersion\":1" || text.starts_with(knownSchemaPrefix);
    if (!registry && text.find("\"schemaVersion\"") != std::string::npos &&
        (!identifiesKnownSchema || text.ends_with("}\n")))
    {
        return cue::Result<RegistrySnapshot>::failure(std::move(*registry.try_error()));
    }
    return cue::Result<RegistrySnapshot>::success(std::move(result));
}

/// @brief RegistryをCanonical JSONとしてAtomic Replaceする
[[nodiscard]] cue::Result<void> write_registry(const std::filesystem::path &a_installRoot,
                                               const cue::distribution::InstalledVersionsRegistry &a_registry,
                                               const cue::AssertContext &a_assertContext) noexcept
{
    auto bytes = cue::distribution::write_installed_versions_registry(a_registry, a_assertContext);
    if (!bytes)
    {
        return cue::Result<void>::failure(std::move(*bytes.try_error()));
    }
    return write_atomic_text(a_installRoot / L"State" / L"InstalledVersions.json", *bytes.try_value(), a_assertContext);
}

/// @brief Payload Markerを生成してStagingへ最後に耐久書込みする
[[nodiscard]] cue::Result<std::string> write_payload_marker(const std::filesystem::path &a_versionRoot,
                                                            const BundleSnapshot &a_bundle,
                                                            std::string_view a_directoryName,
                                                            const cue::AssertContext &a_assertContext) noexcept
{
    cue::distribution::PayloadCompleteMarker marker{std::string(a_directoryName), a_bundle.manifest.bundleId,
                                                    a_bundle.manifestDigest};
    auto bytes = cue::distribution::write_payload_complete_marker(marker, a_assertContext);
    if (!bytes)
    {
        return cue::Result<std::string>::failure(std::move(*bytes.try_error()));
    }
    auto written = write_atomic_text(a_versionRoot / k_payloadMarkerName, *bytes.try_value(), a_assertContext);
    if (!written)
    {
        return cue::Result<std::string>::failure(std::move(*written.try_error()));
    }
    const auto *begin = reinterpret_cast<const std::byte *>(bytes.try_value()->data());
    return hash_bytes(std::span(begin, bytes.try_value()->size()), a_assertContext);
}

/// @brief Payload MarkerとVersion Payloadを再検証してMarker Digestを返す
[[nodiscard]] cue::Result<std::string> validate_published_payload(const std::filesystem::path &a_versionRoot,
                                                                  const BundleSnapshot &a_bundle,
                                                                  std::string_view a_directoryName,
                                                                  const cue::AssertContext &a_assertContext) noexcept
{
    auto manifest = validate_manifest_digest(a_versionRoot, a_bundle, a_assertContext);
    if (!manifest)
    {
        return cue::Result<std::string>::failure(std::move(*manifest.try_error()));
    }
    auto validated = validate_inventory(a_versionRoot, a_bundle.manifest, true, a_assertContext);
    if (!validated)
    {
        return cue::Result<std::string>::failure(std::move(*validated.try_error()));
    }
    auto text = read_text(a_versionRoot / k_payloadMarkerName, a_assertContext);
    if (!text)
    {
        return cue::Result<std::string>::failure(std::move(*text.try_error()));
    }
    auto marker = cue::distribution::read_payload_complete_marker(*text.try_value(), a_assertContext);
    if (!marker || marker.try_value()->directoryName != a_directoryName ||
        marker.try_value()->bundleId != a_bundle.manifest.bundleId ||
        marker.try_value()->manifestDigest != a_bundle.manifestDigest)
    {
        return cue::Result<std::string>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                          "Published Version Payload marker does not match"));
    }
    const auto *begin = reinterpret_cast<const std::byte *>(text.try_value()->data());
    return hash_bytes(std::span(begin, text.try_value()->size()), a_assertContext);
}

/// @brief Probe Markerを耐久書込みしてDigestを返す
[[nodiscard]] cue::Result<std::string> write_probe_marker(const std::filesystem::path &a_versionRoot,
                                                          const BundleSnapshot &a_bundle,
                                                          std::string_view a_directoryName,
                                                          std::string_view a_operationId,
                                                          const cue::AssertContext &a_assertContext) noexcept
{
    cue::distribution::InstallProbeMarker marker{std::string(a_operationId), std::string(a_directoryName),
                                                 a_bundle.manifest.bundleId, a_bundle.manifestDigest};
    auto bytes = cue::distribution::write_install_probe_marker(marker, a_assertContext);
    if (!bytes)
    {
        return cue::Result<std::string>::failure(std::move(*bytes.try_error()));
    }
    auto written = write_atomic_text(a_versionRoot / k_probeMarkerName, *bytes.try_value(), a_assertContext);
    if (!written)
    {
        return cue::Result<std::string>::failure(std::move(*written.try_error()));
    }
    const auto *begin = reinterpret_cast<const std::byte *>(bytes.try_value()->data());
    return hash_bytes(std::span(begin, bytes.try_value()->size()), a_assertContext);
}

/// @brief Probe Markerを再検証してDigestを返す
[[nodiscard]] cue::Result<std::string> validate_probe_marker(const std::filesystem::path &a_versionRoot,
                                                             const BundleSnapshot &a_bundle,
                                                             std::string_view a_directoryName,
                                                             const cue::AssertContext &a_assertContext) noexcept
{
    auto text = read_text(a_versionRoot / k_probeMarkerName, a_assertContext);
    if (!text)
    {
        return cue::Result<std::string>::failure(std::move(*text.try_error()));
    }
    auto marker = cue::distribution::read_install_probe_marker(*text.try_value(), a_assertContext);
    if (!marker || marker.try_value()->directoryName != a_directoryName ||
        marker.try_value()->bundleId != a_bundle.manifest.bundleId ||
        marker.try_value()->manifestDigest != a_bundle.manifestDigest)
    {
        return cue::Result<std::string>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                          "Install Probe marker does not match the Version"));
    }
    const auto *begin = reinterpret_cast<const std::byte *>(text.try_value()->data());
    return hash_bytes(std::span(begin, text.try_value()->size()), a_assertContext);
}

/// @brief Windows Command Line ArgumentをCreateProcess規則でQuoteする
[[nodiscard]] std::wstring quote_argument(std::wstring_view a_argument)
{
    std::wstring output(1U, L'"');
    std::size_t backslashes = 0U;
    for (wchar_t character : a_argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'"')
        {
            output.append(backslashes * 2U + 1U, L'\\');
            output.push_back(L'"');
            backslashes = 0U;
            continue;
        }
        output.append(backslashes, L'\\');
        backslashes = 0U;
        output.push_back(character);
    }
    output.append(backslashes * 2U, L'\\');
    output.push_back(L'"');
    return output;
}

/// @brief 現在Processが読込済みのModule Directoryを返す
[[nodiscard]] std::optional<std::wstring> loaded_module_directory(std::wstring_view a_moduleName)
{
    const std::wstring moduleName(a_moduleName);
    const HMODULE module = GetModuleHandleW(moduleName.c_str());
    if (module == nullptr)
    {
        return std::nullopt;
    }
    std::array<wchar_t, 32768U> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size())
    {
        return std::nullopt;
    }
    return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path().native();
}

/// @brief 大文字小文字を区別せず未登録のDirectoryだけを追加する
void append_unique_directory(std::vector<std::wstring> &a_directories, std::wstring a_directory)
{
    const auto duplicate = std::ranges::find_if(
        a_directories,
        [&a_directory](const std::wstring &a_existing) noexcept
        {
            return CompareStringOrdinal(a_existing.data(), static_cast<int>(a_existing.size()), a_directory.data(),
                                        static_cast<int>(a_directory.size()), TRUE) == CSTR_EQUAL;
        });
    if (duplicate == a_directories.end())
    {
        a_directories.push_back(std::move(a_directory));
    }
}

/// @brief Probeへ渡すSystemRootと読込済みRuntimeだけのUnicode Environment Blockを構築する
[[nodiscard]] std::optional<std::vector<wchar_t>> make_probe_environment()
{
    const DWORD required = GetEnvironmentVariableW(L"SystemRoot", nullptr, 0U);
    if (required == 0U)
    {
        return std::nullopt;
    }
    std::wstring systemRoot(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"SystemRoot", systemRoot.data(), required);
    if (written == 0U || written >= required)
    {
        return std::nullopt;
    }
    systemRoot.resize(written);

    std::vector<std::wstring> runtimeDirectories;
    constexpr std::array<std::wstring_view, 10U> runtimeModules = {
        L"msvcp140.dll",        L"msvcp140d.dll", L"vcruntime140.dll", L"vcruntime140d.dll", L"vcruntime140_1.dll",
        L"vcruntime140_1d.dll", L"ucrtbase.dll",  L"ucrtbased.dll",    L"concrt140.dll",     L"concrt140d.dll",
    };
    for (std::wstring_view module : runtimeModules)
    {
        auto directory = loaded_module_directory(module);
        if (directory)
        {
            append_unique_directory(runtimeDirectories, std::move(*directory));
        }
    }
    std::array<wchar_t, 32768U> systemDirectory{};
    const UINT systemDirectoryLength =
        GetSystemDirectoryW(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
    if (systemDirectoryLength == 0U || systemDirectoryLength >= systemDirectory.size())
    {
        return std::nullopt;
    }
    append_unique_directory(runtimeDirectories,
                            std::wstring(systemDirectory.data(), static_cast<std::size_t>(systemDirectoryLength)));

    std::wstring runtimePath;
    for (const std::wstring &directory : runtimeDirectories)
    {
        if (!runtimePath.empty())
        {
            runtimePath.push_back(L';');
        }
        runtimePath.append(directory);
    }
    std::vector<wchar_t> environment;
    environment.reserve(17U + runtimePath.size() + systemRoot.size() + 3U);
    constexpr std::wstring_view pathPrefix = L"Path=";
    environment.insert(environment.end(), pathPrefix.begin(), pathPrefix.end());
    environment.insert(environment.end(), runtimePath.begin(), runtimePath.end());
    environment.push_back(L'\0');
    constexpr std::wstring_view systemRootPrefix = L"SystemRoot=";
    environment.insert(environment.end(), systemRootPrefix.begin(), systemRootPrefix.end());
    environment.insert(environment.end(), systemRoot.begin(), systemRoot.end());
    environment.push_back(L'\0');
    environment.push_back(L'\0');
    return environment;
}

/// @brief Probe標準出力をProcess待機中に排出し、失敗診断を上限付きで保持する
[[nodiscard]] cue::Result<void> drain_probe_diagnostics(HANDLE a_pipe, std::string &a_diagnostics,
                                                        const cue::AssertContext &a_assertContext) noexcept
{
    std::array<char, 512U> buffer{};
    std::size_t drainedBytes = 0U;
    while (drainedBytes < k_maximumProbeDrainBytesPerPoll)
    {
        DWORD availableBytes = 0U;
        if (PeekNamedPipe(a_pipe, nullptr, 0U, nullptr, &availableBytes, nullptr) == FALSE)
        {
            if (GetLastError() == ERROR_BROKEN_PIPE)
            {
                return cue::Result<void>::success();
            }
            return cue::Result<void>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                              "Install Probe diagnostic pipe could not be inspected"));
        }
        if (availableBytes == 0U)
        {
            return cue::Result<void>::success();
        }
        const DWORD requestedBytes = static_cast<DWORD>(
            std::min<std::size_t>({buffer.size(), availableBytes, k_maximumProbeDrainBytesPerPoll - drainedBytes}));
        DWORD readBytes = 0U;
        if (ReadFile(a_pipe, buffer.data(), requestedBytes, &readBytes, nullptr) == FALSE)
        {
            if (GetLastError() == ERROR_BROKEN_PIPE)
            {
                return cue::Result<void>::success();
            }
            return cue::Result<void>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                              "Install Probe diagnostic pipe could not be read"));
        }
        if (readBytes == 0U)
        {
            return cue::Result<void>::success();
        }
        const std::size_t retainedBytes =
            std::min<std::size_t>(readBytes, k_maximumProbeDiagnosticBytes - a_diagnostics.size());
        a_diagnostics.append(buffer.data(), retainedBytes);
        drainedBytes += readBytes;
    }
    return cue::Result<void>::success();
}

/// @brief 専用Install Probe ProcessへControl Lease Handleと診断出力先だけを継承して実行する
[[nodiscard]] cue::Result<void> run_probe_process(const std::filesystem::path &a_installRoot,
                                                  const std::filesystem::path &a_versionRoot,
                                                  std::string_view a_directoryName, std::string_view a_operationId,
                                                  std::string_view a_manifestDigest, HANDLE a_leaseHandle,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path executable = a_versionRoot / L"Bin" / L"CueEngineInstallerTool.exe";
    const std::wstring executableNative = win32_path(executable);
    const auto installUtf8 = to_utf8(a_installRoot.native());
    if (!installUtf8)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Root could not be encoded for Probe"));
    }
    std::wstring command = quote_argument(executableNative);
    const std::array<std::string, 11U> arguments = {
        "--install-probe",
        "--lease-handle",
        std::to_string(reinterpret_cast<std::uintptr_t>(a_leaseHandle)),
        "--install-root",
        *installUtf8,
        "--version-directory",
        std::string(a_directoryName),
        "--operation-id",
        std::string(a_operationId),
        "--manifest-digest",
        std::string(a_manifestDigest),
    };
    for (const std::string &argument : arguments)
    {
        command.push_back(L' ');
        command.append(quote_argument(to_wide(argument).value_or(L"")));
    }

    SECURITY_ATTRIBUTES pipeSecurity{};
    pipeSecurity.nLength = sizeof(pipeSecurity);
    pipeSecurity.bInheritHandle = TRUE;
    HANDLE diagnosticReadRaw = INVALID_HANDLE_VALUE;
    HANDLE diagnosticWriteRaw = INVALID_HANDLE_VALUE;
    if (CreatePipe(&diagnosticReadRaw, &diagnosticWriteRaw, &pipeSecurity, 0U) == FALSE)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Probe diagnostic pipe could not be created"));
    }
    HandleOwner diagnosticRead(diagnosticReadRaw);
    HandleOwner diagnosticWrite(diagnosticWriteRaw);
    if (SetHandleInformation(diagnosticRead.get(), HANDLE_FLAG_INHERIT, 0U) == FALSE)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Probe diagnostic read handle could not be isolated"));
    }
    HandleOwner nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &pipeSecurity,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nullInput.valid())
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Probe null input handle could not be created"));
    }

    SIZE_T attributeBytes = 0U;
    static_cast<void>(InitializeProcThreadAttributeList(nullptr, 1U, 0U, &attributeBytes));
    if (attributeBytes == 0U)
    {
        return cue::Result<void>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Install Probe inheritance allowlist size could not be determined"));
    }
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (InitializeProcThreadAttributeList(attributes, 1U, 0U, &attributeBytes) == FALSE)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Probe inheritance allowlist could not be configured"));
    }
    std::array<HANDLE, 3U> inheritedHandles = {a_leaseHandle, diagnosticWrite.get(), nullInput.get()};
    if (UpdateProcThreadAttribute(attributes, 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles.data(),
                                  sizeof(inheritedHandles), nullptr, nullptr) == FALSE)
    {
        DeleteProcThreadAttributeList(attributes);
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Probe inheritance handle could not be configured"));
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput.get();
    startup.StartupInfo.hStdOutput = diagnosticWrite.get();
    startup.StartupInfo.hStdError = diagnosticWrite.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    auto environment = make_probe_environment();
    if (!environment)
    {
        DeleteProcThreadAttributeList(attributes);
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Probe environment could not be constructed"));
    }
    const BOOL created = CreateProcessW(executableNative.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                        environment->data(), nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    if (created == FALSE)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Probe process could not be created"));
    }
    diagnosticWrite.reset();
    HandleOwner processHandle(process.hProcess);
    HandleOwner threadHandle(process.hThread);
    std::string diagnostics;
    const ULONGLONG startedAt = GetTickCount64();
    DWORD wait = WAIT_TIMEOUT;
    while (wait == WAIT_TIMEOUT && GetTickCount64() - startedAt < k_probeTimeoutMilliseconds)
    {
        auto drained = drain_probe_diagnostics(diagnosticRead.get(), diagnostics, a_assertContext);
        if (!drained)
        {
            static_cast<void>(TerminateProcess(processHandle.get(), 91U));
            static_cast<void>(WaitForSingleObject(processHandle.get(), INFINITE));
            return drained;
        }
        wait = WaitForSingleObject(processHandle.get(), k_probePollMilliseconds);
    }
    if (wait != WAIT_OBJECT_0)
    {
        static_cast<void>(TerminateProcess(processHandle.get(), 90U));
        static_cast<void>(WaitForSingleObject(processHandle.get(), INFINITE));
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Probe process timed out"));
    }
    auto drained = drain_probe_diagnostics(diagnosticRead.get(), diagnostics, a_assertContext);
    if (!drained)
    {
        return drained;
    }
    DWORD exitCode = 0U;
    if (GetExitCodeProcess(processHandle.get(), &exitCode) == FALSE || exitCode != 0U)
    {
        cue::Error error = install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                                         "Install Probe process rejected the Version");
        error.add_context(a_assertContext.fatal_handler(), "Exit code: " + std::to_string(exitCode));
        if (!diagnostics.empty())
        {
            error.add_context(a_assertContext.fatal_handler(), "Probe diagnostics: " + diagnostics);
        }
        return cue::Result<void>::failure(std::move(error));
    }
    return cue::Result<void>::success();
}

/// @brief Install Worker DirectoryのExecutableとMarkerを再検証する
[[nodiscard]] cue::Result<std::pair<std::string, std::string>> validate_worker(
    const std::filesystem::path &a_workerRoot, const BundleSnapshot &a_bundle,
    const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path executable = a_workerRoot / L"CueEngineInstallWorker.exe";
    const std::filesystem::path markerPath = a_workerRoot / k_workerMarkerName;
    if (!is_plain_directory(a_workerRoot) || !is_plain_file(executable) || !is_plain_file(markerPath))
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                          "Published Install Worker evidence is not plain"));
    }
    auto executableBytes = read_file(executable, k_maximumPayloadBytes, a_assertContext);
    if (!executableBytes || executableBytes.try_value()->size() != a_bundle.worker.byteSize ||
        !is_x64_pe(*executableBytes.try_value()))
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                          "Published Install Worker executable is invalid"));
    }
    auto executableDigest = hash_bytes(*executableBytes.try_value(), a_assertContext);
    if (!executableDigest || *executableDigest.try_value() != a_bundle.worker.sha256)
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                          "Published Install Worker digest does not match"));
    }
    auto markerText = read_text(markerPath, a_assertContext);
    if (!markerText)
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(std::move(*markerText.try_error()));
    }
    auto marker = cue::distribution::read_install_worker_marker(*markerText.try_value(), a_assertContext);
    auto publisherDigest = cue::distribution::make_publisher_build_identity_digest(
        a_bundle.manifest.publisherBuildIdentity, a_assertContext);
    if (!marker || !publisherDigest || marker.try_value()->workerId != a_bundle.workerId ||
        marker.try_value()->bundleId != a_bundle.manifest.bundleId ||
        marker.try_value()->engineSourceRevision != a_bundle.manifest.engineSourceRevision ||
        marker.try_value()->publisherBuildIdentityDigest != *publisherDigest.try_value() ||
        marker.try_value()->executable != a_bundle.worker)
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                          "Published Install Worker marker does not match"));
    }
    const auto *begin = reinterpret_cast<const std::byte *>(markerText.try_value()->data());
    auto markerDigest = hash_bytes(std::span(begin, markerText.try_value()->size()), a_assertContext);
    if (!markerDigest)
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(std::move(*markerDigest.try_error()));
    }
    return cue::Result<std::pair<std::string, std::string>>::success(
        std::pair(std::move(*executableDigest.try_value()), std::move(*markerDigest.try_value())));
}

/// @brief Payload Cleanup後もJournalと外部Workerの固定Identityを再検証する
[[nodiscard]] cue::Result<void> validate_worker_for_journal(const std::filesystem::path &a_installRoot,
                                                            const cue::distribution::InstallOperationJournal &a_journal,
                                                            const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path workerRoot =
        a_installRoot / L"Operations" / L"Workers" / to_wide(a_journal.workerId).value_or(L"");
    const std::filesystem::path executable = workerRoot / L"CueEngineInstallWorker.exe";
    const std::filesystem::path markerPath = workerRoot / k_workerMarkerName;
    if (!is_plain_directory(workerRoot) || !is_plain_file(executable) || !is_plain_file(markerPath))
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::BundleValidationFailed,
                                                        "Uninstall Worker evidence is not plain"));
    }
    auto markerText = read_text(markerPath, a_assertContext);
    if (!markerText)
    {
        return cue::Result<void>::failure(std::move(*markerText.try_error()));
    }
    auto marker = cue::distribution::read_install_worker_marker(*markerText.try_value(), a_assertContext);
    if (!marker || marker.try_value()->workerId != a_journal.workerId ||
        marker.try_value()->bundleId != a_journal.target->bundleId)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::BundleValidationFailed,
                                                        "Uninstall Worker marker does not match its Journal"));
    }
    auto executableBytes = read_file(executable, k_maximumPayloadBytes, a_assertContext);
    if (!executableBytes || executableBytes.try_value()->size() != marker.try_value()->executable.byteSize ||
        !is_x64_pe(*executableBytes.try_value()))
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::BundleValidationFailed,
                                                        "Uninstall Worker executable is invalid"));
    }
    auto executableDigest = hash_bytes(*executableBytes.try_value(), a_assertContext);
    if (!executableDigest || *executableDigest.try_value() != marker.try_value()->executable.sha256)
    {
        return cue::Result<void>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                          "Uninstall Worker executable digest does not match its marker"));
    }
    return cue::Result<void>::success();
}

/// @brief ManifestのInstall WorkerをOperation Stagingで検証してVersion外へAtomic Publishする
[[nodiscard]] cue::Result<std::pair<std::string, std::string>> publish_worker(
    const std::filesystem::path &a_installRoot, const std::filesystem::path &a_versionRoot,
    const BundleSnapshot &a_bundle, std::string_view a_operationId, const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path finalRoot =
        a_installRoot / L"Operations" / L"Workers" / to_wide(a_bundle.workerId).value_or(L"");
    if (is_plain_directory(finalRoot))
    {
        return validate_worker(finalRoot, a_bundle, a_assertContext);
    }
    const std::filesystem::path stagingParent =
        a_installRoot / L"Operations" / L"WorkerStaging" / to_wide(a_operationId).value_or(L"");
    const std::filesystem::path stagingRoot = stagingParent / to_wide(a_bundle.workerId).value_or(L"");
    std::error_code error;
    std::filesystem::remove_all(extended_filesystem_path(stagingParent), error);
    error.clear();
    std::filesystem::create_directories(extended_filesystem_path(stagingRoot), error);
    const std::filesystem::path stagedExecutable = stagingRoot / L"CueEngineInstallWorker.exe";
    if (error || CopyFileW(win32_path(a_versionRoot / to_wide(a_bundle.worker.relativePath).value_or(L"")).c_str(),
                           win32_path(stagedExecutable).c_str(), TRUE) == FALSE)
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Install Worker Staging could not be created"));
    }
    auto executableFlushed = flush_copied_file(stagedExecutable, a_assertContext);
    if (!executableFlushed)
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(std::move(*executableFlushed.try_error()));
    }
    auto publisherDigest = cue::distribution::make_publisher_build_identity_digest(
        a_bundle.manifest.publisherBuildIdentity, a_assertContext);
    if (!publisherDigest)
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(std::move(*publisherDigest.try_error()));
    }
    cue::distribution::InstallWorkerMarker marker{a_bundle.workerId,
                                                  a_bundle.manifest.bundleId,
                                                  a_bundle.manifest.engineSourceRevision,
                                                  *publisherDigest.try_value(),
                                                  a_bundle.worker,
                                                  cue::distribution::DistributionArchitecture::X64};
    auto markerBytes = cue::distribution::write_install_worker_marker(marker, a_assertContext);
    if (!markerBytes)
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(std::move(*markerBytes.try_error()));
    }
    auto markerWritten = write_atomic_text(stagingRoot / k_workerMarkerName, *markerBytes.try_value(), a_assertContext);
    if (!markerWritten)
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(std::move(*markerWritten.try_error()));
    }
    auto validated = validate_worker(stagingRoot, a_bundle, a_assertContext);
    if (!validated)
    {
        return validated;
    }
    std::filesystem::create_directories(extended_filesystem_path(finalRoot.parent_path()), error);
    if (error)
    {
        return cue::Result<std::pair<std::string, std::string>>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Install Worker parent could not be created"));
    }
    auto published = publish_directory(stagingRoot, finalRoot, a_assertContext);
    if (!published)
    {
        if (is_plain_directory(finalRoot))
        {
            return validate_worker(finalRoot, a_bundle, a_assertContext);
        }
        return cue::Result<std::pair<std::string, std::string>>::failure(std::move(*published.try_error()));
    }
    std::filesystem::remove_all(extended_filesystem_path(stagingParent), error);
    return validate_worker(finalRoot, a_bundle, a_assertContext);
}

/// @brief Version、Payload／Probe Marker、WorkerからRegistry Entryを再構築する
[[nodiscard]] cue::Result<cue::distribution::InstalledVersionEntry> build_version_entry(
    const std::filesystem::path &a_installRoot, const std::filesystem::path &a_versionRoot,
    const BundleSnapshot &a_bundle, std::string_view a_directoryName,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto payloadDigest = validate_published_payload(a_versionRoot, a_bundle, a_directoryName, a_assertContext);
    auto probeDigest = validate_probe_marker(a_versionRoot, a_bundle, a_directoryName, a_assertContext);
    auto worker = validate_worker(a_installRoot / L"Operations" / L"Workers" / to_wide(a_bundle.workerId).value_or(L""),
                                  a_bundle, a_assertContext);
    if (!payloadDigest || !probeDigest || !worker)
    {
        if (!payloadDigest)
            return cue::Result<cue::distribution::InstalledVersionEntry>::failure(
                std::move(*payloadDigest.try_error()));
        if (!probeDigest)
            return cue::Result<cue::distribution::InstalledVersionEntry>::failure(std::move(*probeDigest.try_error()));
        return cue::Result<cue::distribution::InstalledVersionEntry>::failure(std::move(*worker.try_error()));
    }
    cue::distribution::InstalledVersionEntry entry{
        std::string(a_directoryName),
        a_bundle.manifest.engineVersion,
        a_bundle.manifest.bundleId,
        a_bundle.manifestDigest,
        std::move(*payloadDigest.try_value()),
        std::move(*probeDigest.try_value()),
        a_bundle.workerId,
        std::move(worker.try_value()->first),
        std::move(worker.try_value()->second),
        cue::distribution::InstalledVersionState::Selectable,
    };
    return cue::Result<cue::distribution::InstalledVersionEntry>::success(std::move(entry));
}

/// @brief Registry EntryとVersion／Worker Evidenceを一つの再検証Snapshotとして読み込む
[[nodiscard]] cue::Result<InstalledVersionSnapshot> read_installed_version(
    const std::filesystem::path &a_installRoot, std::string_view a_directoryName, bool a_allowPendingRemoval,
    const cue::AssertContext &a_assertContext) noexcept
{
    using namespace cue::distribution;
    if (!is_version_directory(a_directoryName))
    {
        return cue::Result<InstalledVersionSnapshot>::failure(
            install_error(a_assertContext, DistributionError::InvalidInstallState,
                          "Installed Version directory identity is invalid"));
    }
    auto registrySnapshot = read_registry(a_installRoot, a_assertContext);
    if (!registrySnapshot || !registrySnapshot.try_value()->registry)
    {
        return cue::Result<InstalledVersionSnapshot>::failure(
            registrySnapshot ? install_error(a_assertContext, DistributionError::InvalidInstallState,
                                             "Installed Versions Registry is unavailable")
                             : std::move(*registrySnapshot.try_error()));
    }
    InstalledVersionsRegistry registry = std::move(*registrySnapshot.try_value()->registry);
    const auto found = std::ranges::find_if(registry.versions, [a_directoryName](const InstalledVersionEntry &a_entry)
                                            { return a_entry.directoryName == a_directoryName; });
    if (found == registry.versions.end() ||
        (!a_allowPendingRemoval && found->state != InstalledVersionState::Selectable))
    {
        return cue::Result<InstalledVersionSnapshot>::failure(
            install_error(a_assertContext, DistributionError::InvalidInstallState,
                          "Installed Version is not selectable in the Registry"));
    }
    const InstalledVersionEntry entry = *found;
    const std::filesystem::path versionRoot = a_installRoot / L"Versions" / to_wide(a_directoryName).value_or(L"");
    auto bundle = validate_bundle(versionRoot, true, a_assertContext);
    if (!bundle)
    {
        return cue::Result<InstalledVersionSnapshot>::failure(std::move(*bundle.try_error()));
    }
    auto rebuilt =
        build_version_entry(a_installRoot, versionRoot, *bundle.try_value(), a_directoryName, a_assertContext);
    if (!rebuilt)
    {
        return cue::Result<InstalledVersionSnapshot>::failure(std::move(*rebuilt.try_error()));
    }
    rebuilt.try_value()->state = entry.state;
    if (*rebuilt.try_value() != entry)
    {
        return cue::Result<InstalledVersionSnapshot>::failure(
            install_error(a_assertContext, DistributionError::InstallConflict,
                          "Installed Version Evidence does not match the Registry"));
    }
    InstalledVersionSnapshot snapshot{std::move(registry), entry, std::move(*bundle.try_value())};
    return cue::Result<InstalledVersionSnapshot>::success(std::move(snapshot));
}

/// @brief Recovery Source EvidenceがPublish直前にも一致するか返す
[[nodiscard]] bool revalidate_source_evidence(const std::filesystem::path &a_installRoot,
                                              const cue::distribution::RegistrySourceEvidence &a_evidence,
                                              const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path registryPath = a_installRoot / L"State" / L"InstalledVersions.json";
    if (GetFileAttributesW(win32_path(registryPath).c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }
    if (a_evidence.wasMissing)
    {
        return true;
    }
    const std::filesystem::path evidencePath =
        a_installRoot / L"Operations" / L"Evidence" / to_wide(a_evidence.evidenceName).value_or(L"");
    if (!is_plain_file(evidencePath))
    {
        return false;
    }
    auto bytes = read_file(evidencePath, k_maximumManifestBytes, a_assertContext);
    if (!bytes || bytes.try_value()->size() != a_evidence.byteSize)
    {
        return false;
    }
    auto digest = hash_bytes(*bytes.try_value(), a_assertContext);
    return digest && *digest.try_value() == a_evidence.sha256;
}

/// @brief Recovery Journal耐久化後にSource RegistryをEvidenceへ冪等退避する
[[nodiscard]] cue::Result<void> ensure_source_evidence(const std::filesystem::path &a_installRoot,
                                                       const cue::distribution::RegistrySourceEvidence &a_evidence,
                                                       const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path registryPath = a_installRoot / L"State" / L"InstalledVersions.json";
    const bool registryExists = GetFileAttributesW(win32_path(registryPath).c_str()) != INVALID_FILE_ATTRIBUTES;
    if (a_evidence.wasMissing)
    {
        return !registryExists ? cue::Result<void>::success()
                               : cue::Result<void>::failure(install_error(
                                     a_assertContext, cue::distribution::DistributionError::InstallConflict,
                                     "Registry appeared after missing Recovery Evidence was recorded"));
    }
    const std::filesystem::path evidencePath =
        a_installRoot / L"Operations" / L"Evidence" / to_wide(a_evidence.evidenceName).value_or(L"");
    if (is_plain_file(evidencePath))
    {
        if (registryExists || !revalidate_source_evidence(a_installRoot, a_evidence, a_assertContext))
        {
            return cue::Result<void>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                              "Registry Recovery Evidence conflicts with the current Registry"));
        }
        return cue::Result<void>::success();
    }
    if (!is_plain_file(registryPath))
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::InstallConflict,
                                                        "Registry Recovery source is missing or not a plain file"));
    }
    auto bytes = read_file(registryPath, k_maximumManifestBytes, a_assertContext);
    if (!bytes || bytes.try_value()->size() != a_evidence.byteSize)
    {
        return cue::Result<void>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                          "Registry Recovery source size changed before Evidence publish"));
    }
    auto digest = hash_bytes(*bytes.try_value(), a_assertContext);
    if (!digest || *digest.try_value() != a_evidence.sha256)
    {
        return cue::Result<void>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                          "Registry Recovery source digest changed before Evidence publish"));
    }
    if (MoveFileExW(win32_path(registryPath).c_str(), win32_path(evidencePath).c_str(), MOVEFILE_WRITE_THROUGH) ==
            FALSE ||
        !revalidate_source_evidence(a_installRoot, a_evidence, a_assertContext))
    {
        return cue::Result<void>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Corrupt Registry could not be published as Recovery Evidence"));
    }
    return cue::Result<void>::success();
}

/// @brief 非互換または破損Journalを耐久Evidenceへ隔離して以後の自動Recoveryを停止する
[[nodiscard]] cue::Result<void> quarantine_invalid_journal(const std::filesystem::path &a_installRoot,
                                                           const std::filesystem::path &a_journalPath,
                                                           const cue::AssertContext &a_assertContext) noexcept
{
    auto quarantineId = make_uuid();
    if (!quarantineId)
    {
        return cue::Result<void>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Invalid Install Journal quarantine ID could not be generated"));
    }
    const std::filesystem::path quarantineRoot = a_installRoot / L"Operations" / L"Quarantine" / L"Journals";
    std::error_code error;
    std::filesystem::create_directories(extended_filesystem_path(quarantineRoot), error);
    if (error || !is_plain_directory(quarantineRoot))
    {
        return cue::Result<void>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Invalid Install Journal quarantine directory could not be created"));
    }
    const std::filesystem::path destination =
        quarantineRoot / (a_journalPath.filename().native() + L"-" + to_wide(*quarantineId).value_or(L"invalid"));
    if (MoveFileExW(win32_path(a_journalPath).c_str(), win32_path(destination).c_str(), MOVEFILE_WRITE_THROUGH) ==
        FALSE)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Invalid Install Journal could not be quarantined"));
    }
    return cue::Result<void>::success();
}

/// @brief 全Journalを検証してRecovery除外対象と既存Recoveryを収集する
[[nodiscard]] cue::Result<std::vector<std::pair<cue::distribution::InstallOperationJournal, std::string>>>
read_all_journals(const std::filesystem::path &a_installRoot, const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<std::pair<cue::distribution::InstallOperationJournal, std::string>> result;
    const std::filesystem::path root = a_installRoot / L"Operations" / L"Journals";
    const std::filesystem::path quarantineRoot = a_installRoot / L"Operations" / L"Quarantine" / L"Journals";
    std::error_code error;
    std::filesystem::create_directories(extended_filesystem_path(root), error);
    if (error)
    {
        return cue::Result<decltype(result)>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Install Journal directory could not be created"));
    }
    try
    {
        if (std::filesystem::directory_iterator(extended_filesystem_path(quarantineRoot)) !=
            std::filesystem::directory_iterator{})
        {
            return cue::Result<decltype(result)>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::InstallRecoveryBlocked,
                              "Quarantined Install Journal requires explicit repair"));
        }
        std::vector<std::filesystem::path> journalPaths;
        for (const std::filesystem::directory_entry &entry :
             std::filesystem::directory_iterator(extended_filesystem_path(root)))
        {
            if (journalPaths.size() >= k_maximumInstallEntries)
            {
                return cue::Result<decltype(result)>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::ResourceLimitExceeded,
                                  "Install Journal count exceeds its limit"));
            }
            journalPaths.push_back(entry.path());
        }
        for (const std::filesystem::path &journalPath : journalPaths)
        {
            if (is_plain_file(journalPath) && is_journal_atomic_temporary(journalPath))
            {
                if (DeleteFileW(win32_path(journalPath).c_str()) == FALSE && GetLastError() != ERROR_FILE_NOT_FOUND)
                {
                    return cue::Result<decltype(result)>::failure(
                        install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                                      "Abandoned Install Journal temporary file could not be removed"));
                }
                continue;
            }
            if (!is_plain_file(journalPath) || journalPath.extension() != L".json")
            {
                return cue::Result<decltype(result)>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::InstallRecoveryBlocked,
                                  "Install Journal directory contains an unknown entry"));
            }
            auto journal = read_journal(journalPath, a_assertContext);
            if (!journal)
            {
                auto quarantined = quarantine_invalid_journal(a_installRoot, journalPath, a_assertContext);
                if (!quarantined)
                {
                    return cue::Result<decltype(result)>::failure(std::move(*quarantined.try_error()));
                }
                return cue::Result<decltype(result)>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::InstallRecoveryBlocked,
                                  "Invalid Install Journal was quarantined and requires explicit repair"));
            }
            const std::filesystem::path expectedName =
                to_wide(journal.try_value()->first.operationId).value_or(L"") + L".json";
            if (journalPath.filename() != expectedName)
            {
                return cue::Result<decltype(result)>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::InstallRecoveryBlocked,
                                  "Install Journal file name does not match its Operation ID"));
            }
            result.push_back(std::move(*journal.try_value()));
        }
    }
    catch (...)
    {
        return cue::Result<decltype(result)>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Install Journal enumeration failed"));
    }
    /// @brief Journal列挙順をOperation IDで決定的に固定する
    std::ranges::sort(result, {}, [](const auto &a_value) -> const std::string & { return a_value.first.operationId; });
    return cue::Result<decltype(result)>::success(std::move(result));
}

/// @brief 完成Version DirectoryをRecovery候補として全Evidence再検証する
[[nodiscard]] std::optional<cue::distribution::InstalledVersionEntry> recover_version_candidate(
    const std::filesystem::path &a_installRoot, const std::filesystem::directory_entry &a_entry,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto directoryName = to_utf8(a_entry.path().filename().native());
    if (!directoryName)
    {
        return std::nullopt;
    }
    auto bundle = validate_bundle(a_entry.path(), true, a_assertContext);
    if (!bundle)
    {
        return std::nullopt;
    }
    auto version =
        build_version_entry(a_installRoot, a_entry.path(), *bundle.try_value(), *directoryName, a_assertContext);
    return version ? std::optional(std::move(*version.try_value())) : std::nullopt;
}

/// @brief Recovery Journalが記録したBlocked JournalとVersion候補を現行Evidenceへ再照合する
[[nodiscard]] cue::Result<void> revalidate_recovery_inputs(const std::filesystem::path &a_installRoot,
                                                           const cue::distribution::InstallOperationJournal &a_recovery,
                                                           const cue::AssertContext &a_assertContext) noexcept
{
    using namespace cue::distribution;
    auto journals = read_all_journals(a_installRoot, a_assertContext);
    if (!journals)
    {
        return cue::Result<void>::failure(std::move(*journals.try_error()));
    }
    std::size_t currentBlockedCount = 0U;
    for (const auto &current : *journals.try_value())
    {
        if (current.first.operationId == a_recovery.operationId)
        {
            continue;
        }
        if (current.first.kind == InstallOperationKind::RegistryRecovery || !current.first.target)
        {
            return cue::Result<void>::failure(
                install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                              "Registry Recovery encountered a new or malformed Journal"));
        }
        /// @brief 現行Journalに対応する耐久Blocked Evidenceを検索する
        const auto blocked = std::ranges::find_if(a_recovery.blockedOperations,
                                                  [&current](const BlockedInstallOperation &a_operation) noexcept
                                                  { return a_operation.operationId == current.first.operationId; });
        if (blocked == a_recovery.blockedOperations.end() || blocked->kind != current.first.kind ||
            blocked->stage != current.first.stage || blocked->directoryName != current.first.target->directoryName ||
            blocked->journalDigest != current.second)
        {
            return cue::Result<void>::failure(
                install_error(a_assertContext, DistributionError::InstallConflict,
                              "Registry Recovery blocked Journal changed before publish"));
        }
        ++currentBlockedCount;
    }
    if (currentBlockedCount != a_recovery.blockedOperations.size())
    {
        return cue::Result<void>::failure(
            install_error(a_assertContext, DistributionError::InstallConflict,
                          "Registry Recovery blocked Journal disappeared before publish"));
    }
    for (const RegistryRecoveryCandidate &candidate : a_recovery.candidates)
    {
        const std::filesystem::path path =
            a_installRoot / L"Versions" / to_wide(candidate.version.directoryName).value_or(L"");
        std::error_code error;
        const std::filesystem::directory_entry entry(path, error);
        if (error)
        {
            return cue::Result<void>::failure(install_error(a_assertContext, DistributionError::InstallConflict,
                                                            "Registry Recovery candidate disappeared before publish"));
        }
        auto recovered = recover_version_candidate(a_installRoot, entry, a_assertContext);
        if (!recovered || *recovered != candidate.version)
        {
            return cue::Result<void>::failure(
                install_error(a_assertContext, DistributionError::InstallConflict,
                              "Registry Recovery candidate evidence changed before publish"));
        }
    }
    return cue::Result<void>::success();
}

/// @brief Recovery Journal候補から期待するRevision 1 Registryを構築する
[[nodiscard]] cue::distribution::InstalledVersionsRegistry make_recovered_registry(
    const cue::distribution::InstallOperationJournal &a_journal)
{
    cue::distribution::InstalledVersionsRegistry rebuilt;
    rebuilt.generationId = a_journal.operationId;
    rebuilt.revision = 1U;
    rebuilt.versions.reserve(a_journal.candidates.size());
    for (const cue::distribution::RegistryRecoveryCandidate &candidate : a_journal.candidates)
    {
        rebuilt.versions.push_back(candidate.version);
    }
    return rebuilt;
}

/// @brief Missing／Corrupt RegistryをJournal証拠からv1へ再構築する
[[nodiscard]] cue::Result<cue::distribution::InstalledVersionsRegistry> recover_registry(
    const std::filesystem::path &a_installRoot, RegistrySnapshot a_source, std::string_view a_workerId,
    const cue::AssertContext &a_assertContext) noexcept
{
    using namespace cue::distribution;
    auto journals = read_all_journals(a_installRoot, a_assertContext);
    if (!journals)
    {
        return cue::Result<InstalledVersionsRegistry>::failure(std::move(*journals.try_error()));
    }
    InstallOperationJournal *recovery = nullptr;
    for (auto &journal : *journals.try_value())
    {
        if (journal.first.kind == InstallOperationKind::RegistryRecovery)
        {
            if (recovery != nullptr)
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                                  "Multiple Registry Recovery Journals exist"));
            }
            recovery = &journal.first;
        }
    }
    if (recovery == nullptr || recovery->stage == InstallOperationStage::Prepared)
    {
        std::size_t incompleteOperationCount = 0U;
        for (const auto &current : *journals.try_value())
        {
            if (recovery != nullptr && current.first.operationId == recovery->operationId)
            {
                continue;
            }
            if (current.first.kind == InstallOperationKind::Rollback)
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                                  "Registry Recovery cannot infer an incomplete Rollback selection"));
            }
            if (current.first.kind == InstallOperationKind::RegistryRecovery || !current.first.target)
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                                  "Registry Recovery encountered a conflicting Journal"));
            }
            if (++incompleteOperationCount > 1U)
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                                  "Multiple incomplete Install operations require explicit repair"));
            }
        }
    }
    InstallOperationJournal journal;
    if (recovery != nullptr)
    {
        if (recovery->workerId != a_workerId)
        {
            return cue::Result<InstalledVersionsRegistry>::failure(
                install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                              "Registry Recovery Journal belongs to another Worker identity"));
        }
        journal = *recovery;
    }
    else
    {
        auto operationId = make_uuid();
        if (!operationId)
        {
            return cue::Result<InstalledVersionsRegistry>::failure(
                install_error(a_assertContext, DistributionError::PlatformOperationFailed,
                              "Registry Recovery operation ID could not be generated"));
        }
        journal.operationId = std::move(*operationId);
        journal.kind = InstallOperationKind::RegistryRecovery;
        journal.stage = InstallOperationStage::Prepared;
        journal.workerId = std::string(a_workerId);
        RegistrySourceEvidence evidence;
        evidence.wasMissing = a_source.wasMissing;
        if (!a_source.wasMissing)
        {
            auto evidenceId = make_uuid();
            auto digest = hash_bytes(a_source.bytes, a_assertContext);
            if (!evidenceId || !digest)
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    digest ? install_error(a_assertContext, DistributionError::PlatformOperationFailed,
                                           "Registry Evidence ID could not be generated")
                           : std::move(*digest.try_error()));
            }
            evidence.wasMissing = false;
            evidence.evidenceName = "InstalledVersions.corrupt-" + *evidenceId + ".json";
            evidence.byteSize = a_source.bytes.size();
            evidence.sha256 = std::move(*digest.try_value());
        }
        journal.sourceRegistryEvidence = std::move(evidence);
        auto written = write_journal(a_installRoot, journal, a_assertContext);
        if (!written)
        {
            return cue::Result<InstalledVersionsRegistry>::failure(std::move(*written.try_error()));
        }
    }
    auto evidence = ensure_source_evidence(a_installRoot, *journal.sourceRegistryEvidence, a_assertContext);
    if (!evidence)
    {
        return cue::Result<InstalledVersionsRegistry>::failure(std::move(*evidence.try_error()));
    }

    if (journal.stage == InstallOperationStage::Prepared)
    {
        std::set<std::string> blockedDirectories;
        for (const auto &current : *journals.try_value())
        {
            if (current.first.operationId == journal.operationId)
            {
                continue;
            }
            if (current.first.kind == InstallOperationKind::Rollback)
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                                  "Registry Recovery cannot infer an incomplete Rollback selection"));
            }
            if (current.first.kind == InstallOperationKind::RegistryRecovery || !current.first.target)
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                                  "Registry Recovery encountered a conflicting Journal"));
            }
            if (blockedDirectories.contains(current.first.target->directoryName))
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                                  "Multiple incomplete Journals target the same Version"));
            }
            InstallOperationJournal reconciled = current.first;
            reconciled.expectedRegistry = ExpectedRegistry{journal.operationId, 1U};
            if (reconciled != current.first)
            {
                auto written = write_journal(a_installRoot, reconciled, a_assertContext);
                if (!written)
                {
                    return cue::Result<InstalledVersionsRegistry>::failure(std::move(*written.try_error()));
                }
            }
            const std::filesystem::path reconciledPath = a_installRoot / L"Operations" / L"Journals" /
                                                         (to_wide(reconciled.operationId).value_or(L"") + L".json");
            auto durable = read_journal(reconciledPath, a_assertContext);
            if (!durable || durable.try_value()->first != reconciled)
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    durable ? install_error(a_assertContext, DistributionError::InstallConflict,
                                            "Reconciled Install Journal could not be revalidated")
                            : std::move(*durable.try_error()));
            }
            journal.blockedOperations.push_back({reconciled.operationId, reconciled.kind,
                                                 reconciled.target->directoryName, reconciled.stage,
                                                 std::move(durable.try_value()->second)});
            blockedDirectories.insert(current.first.target->directoryName);
        }
        for (const BlockedInstallOperation &blocked : journal.blockedOperations)
        {
            if (blocked.stage == InstallOperationStage::RegistryPublished)
            {
                blockedDirectories.erase(blocked.directoryName);
            }
        }
        const std::filesystem::path versions = a_installRoot / L"Versions";
        std::error_code error;
        std::filesystem::create_directories(extended_filesystem_path(versions), error);
        if (error)
        {
            return cue::Result<InstalledVersionsRegistry>::failure(
                install_error(a_assertContext, DistributionError::PlatformOperationFailed,
                              "Versions directory could not be created for Registry Recovery"));
        }
        std::filesystem::directory_iterator iterator(extended_filesystem_path(versions), error);
        const std::filesystem::directory_iterator end;
        std::size_t versionCount = 0U;
        for (; !error && iterator != end; iterator.increment(error))
        {
            if (++versionCount > k_maximumInstallEntries)
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    install_error(a_assertContext, DistributionError::ResourceLimitExceeded,
                                  "Installed Version count exceeds its Recovery limit"));
            }
            const std::filesystem::directory_entry &entry = *iterator;
            auto name = to_utf8(entry.path().filename().native());
            std::error_code typeError;
            if (!entry.is_directory(typeError) || typeError || !name || blockedDirectories.contains(*name))
            {
                continue;
            }
            auto candidate = recover_version_candidate(a_installRoot, entry, a_assertContext);
            if (candidate)
            {
                journal.candidates.push_back({std::move(*candidate)});
            }
        }
        if (error)
        {
            return cue::Result<InstalledVersionsRegistry>::failure(
                install_error(a_assertContext, DistributionError::PlatformOperationFailed,
                              "Versions directory enumeration failed during Registry Recovery"));
        }
        /// @brief Recovery候補をVersion Directory名で決定的に固定する
        std::ranges::sort(journal.candidates, {},
                          [](const RegistryRecoveryCandidate &a_candidate) -> const std::string &
                          { return a_candidate.version.directoryName; });
        for (const BlockedInstallOperation &blocked : journal.blockedOperations)
        {
            if (blocked.stage != InstallOperationStage::RegistryPublished)
            {
                continue;
            }
            const auto candidate = std::ranges::find_if(
                journal.candidates, [&blocked](const RegistryRecoveryCandidate &a_candidate) noexcept
                { return a_candidate.version.directoryName == blocked.directoryName; });
            if (candidate == journal.candidates.end())
            {
                return cue::Result<InstalledVersionsRegistry>::failure(
                    install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                                  "Registry-published Version evidence could not be recovered"));
            }
        }
        auto advanced =
            advance_journal(a_installRoot, journal, InstallOperationStage::CandidatesValidated, a_assertContext);
        if (!advanced)
        {
            return cue::Result<InstalledVersionsRegistry>::failure(std::move(*advanced.try_error()));
        }
    }
    if (journal.stage == InstallOperationStage::CandidatesValidated)
    {
        if (!revalidate_source_evidence(a_installRoot, *journal.sourceRegistryEvidence, a_assertContext))
        {
            return cue::Result<InstalledVersionsRegistry>::failure(
                install_error(a_assertContext, DistributionError::InstallConflict,
                              "Registry Recovery source evidence changed before publish"));
        }
        auto revalidated = revalidate_recovery_inputs(a_installRoot, journal, a_assertContext);
        if (!revalidated)
        {
            return cue::Result<InstalledVersionsRegistry>::failure(std::move(*revalidated.try_error()));
        }
        InstalledVersionsRegistry rebuilt = make_recovered_registry(journal);
        auto published = write_registry(a_installRoot, rebuilt, a_assertContext);
        if (!published)
        {
            return cue::Result<InstalledVersionsRegistry>::failure(std::move(*published.try_error()));
        }
        auto advanced =
            advance_journal(a_installRoot, journal, InstallOperationStage::RegistryPublished, a_assertContext);
        if (!advanced)
        {
            return cue::Result<InstalledVersionsRegistry>::failure(std::move(*advanced.try_error()));
        }
    }
    auto current = read_registry(a_installRoot, a_assertContext);
    if (!current || !current.try_value()->registry ||
        *current.try_value()->registry != make_recovered_registry(journal))
    {
        return cue::Result<InstalledVersionsRegistry>::failure(
            install_error(a_assertContext, DistributionError::InstallRecoveryBlocked,
                          "Published Registry Recovery result could not be revalidated"));
    }
    auto revalidated = revalidate_recovery_inputs(a_installRoot, journal, a_assertContext);
    if (!revalidated)
    {
        return cue::Result<InstalledVersionsRegistry>::failure(std::move(*revalidated.try_error()));
    }
    const std::filesystem::path journalPath =
        a_installRoot / L"Operations" / L"Journals" / (to_wide(journal.operationId).value_or(L"") + L".json");
    if (DeleteFileW(win32_path(journalPath).c_str()) == FALSE && GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        return cue::Result<InstalledVersionsRegistry>::failure(
            install_error(a_assertContext, DistributionError::PlatformOperationFailed,
                          "Completed Registry Recovery Journal could not be removed"));
    }
    return cue::Result<InstalledVersionsRegistry>::success(std::move(*current.try_value()->registry));
}

/// @brief Valid Registryを返し、不在または破損時は明示Recoveryを完了する
[[nodiscard]] cue::Result<cue::distribution::InstalledVersionsRegistry> ensure_registry(
    const std::filesystem::path &a_installRoot, std::string_view a_workerId,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto snapshot = read_registry(a_installRoot, a_assertContext);
    if (!snapshot)
    {
        return cue::Result<cue::distribution::InstalledVersionsRegistry>::failure(std::move(*snapshot.try_error()));
    }
    if (snapshot.try_value()->registry)
    {
        auto journals = read_all_journals(a_installRoot, a_assertContext);
        if (!journals)
        {
            return cue::Result<cue::distribution::InstalledVersionsRegistry>::failure(std::move(*journals.try_error()));
        }
        cue::distribution::InstallOperationJournal *recovery = nullptr;
        for (auto &current : *journals.try_value())
        {
            if (current.first.kind != cue::distribution::InstallOperationKind::RegistryRecovery)
            {
                continue;
            }
            if (recovery != nullptr)
            {
                return cue::Result<cue::distribution::InstalledVersionsRegistry>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::InstallRecoveryBlocked,
                                  "Multiple Registry Recovery Journals exist beside a published Registry"));
            }
            recovery = &current.first;
        }
        if (recovery == nullptr)
        {
            return cue::Result<cue::distribution::InstalledVersionsRegistry>::success(
                std::move(*snapshot.try_value()->registry));
        }
        const cue::distribution::InstalledVersionsRegistry expected = make_recovered_registry(*recovery);
        if (*snapshot.try_value()->registry != expected ||
            (recovery->stage != cue::distribution::InstallOperationStage::CandidatesValidated &&
             recovery->stage != cue::distribution::InstallOperationStage::RegistryPublished))
        {
            return cue::Result<cue::distribution::InstalledVersionsRegistry>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                              "Published Registry conflicts with its incomplete Recovery Journal"));
        }
        auto revalidated = revalidate_recovery_inputs(a_installRoot, *recovery, a_assertContext);
        if (!revalidated)
        {
            return cue::Result<cue::distribution::InstalledVersionsRegistry>::failure(
                std::move(*revalidated.try_error()));
        }
        if (recovery->stage == cue::distribution::InstallOperationStage::CandidatesValidated)
        {
            auto advanced = advance_journal(
                a_installRoot, *recovery, cue::distribution::InstallOperationStage::RegistryPublished, a_assertContext);
            if (!advanced)
            {
                return cue::Result<cue::distribution::InstalledVersionsRegistry>::failure(
                    std::move(*advanced.try_error()));
            }
        }
        const std::filesystem::path journalPath =
            a_installRoot / L"Operations" / L"Journals" / (to_wide(recovery->operationId).value_or(L"") + L".json");
        if (DeleteFileW(win32_path(journalPath).c_str()) == FALSE && GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            return cue::Result<cue::distribution::InstalledVersionsRegistry>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                              "Completed Registry Recovery Journal could not be removed"));
        }
        return cue::Result<cue::distribution::InstalledVersionsRegistry>::success(
            std::move(*snapshot.try_value()->registry));
    }
    return recover_registry(a_installRoot, std::move(*snapshot.try_value()), a_workerId, a_assertContext);
}

/// @brief 同じVersion Identityの未完了Install／Update Journalを検索する
[[nodiscard]] cue::Result<std::optional<cue::distribution::InstallOperationJournal>> find_resumable_journal(
    const std::filesystem::path &a_installRoot, std::string_view a_directoryName, const BundleSnapshot &a_bundle,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto journals = read_all_journals(a_installRoot, a_assertContext);
    if (!journals)
    {
        return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::failure(
            std::move(*journals.try_error()));
    }
    std::optional<cue::distribution::InstallOperationJournal> result;
    for (const auto &item : *journals.try_value())
    {
        const auto &journal = item.first;
        if ((journal.kind == cue::distribution::InstallOperationKind::Install ||
             journal.kind == cue::distribution::InstallOperationKind::Update) &&
            journal.target && journal.target->directoryName == a_directoryName)
        {
            if (result || journal.target->bundleId != a_bundle.manifest.bundleId ||
                journal.target->manifestDigest != a_bundle.manifestDigest || journal.workerId != a_bundle.workerId)
            {
                return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                                  "Incomplete Install Journal conflicts with the requested Bundle"));
            }
            result = journal;
        }
        else if (journal.kind != cue::distribution::InstallOperationKind::RegistryRecovery)
        {
            return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::failure(
                install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                              "Another incomplete Install operation requires recovery"));
        }
    }
    return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::success(std::move(result));
}

/// @brief RollbackまたはUninstallの同一Version未完了Journalだけを検索する
[[nodiscard]] cue::Result<std::optional<cue::distribution::InstallOperationJournal>> find_version_operation_journal(
    const std::filesystem::path &a_installRoot, cue::distribution::InstallOperationKind a_kind,
    const cue::distribution::InstalledVersionEntry &a_entry, std::string_view a_workerId,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto journals = read_all_journals(a_installRoot, a_assertContext);
    if (!journals)
    {
        return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::failure(
            std::move(*journals.try_error()));
    }
    std::optional<cue::distribution::InstallOperationJournal> result;
    for (const auto &item : *journals.try_value())
    {
        const auto &journal = item.first;
        if (journal.kind == a_kind && journal.target && journal.target->directoryName == a_entry.directoryName)
        {
            if (result || journal.target->bundleId != a_entry.bundleId ||
                journal.target->manifestDigest != a_entry.manifestDigest || journal.workerId != a_workerId)
            {
                return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                                  "Incomplete Version operation Journal identity does not match"));
            }
            result = journal;
            continue;
        }
        return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                          "Another incomplete Install operation requires recovery"));
    }
    return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::success(std::move(result));
}

/// @brief 指定Versionの未完了Uninstall Journalだけを読取る
[[nodiscard]] cue::Result<std::optional<cue::distribution::InstallOperationJournal>> find_uninstall_journal(
    const std::filesystem::path &a_installRoot, std::string_view a_directoryName,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto journals = read_all_journals(a_installRoot, a_assertContext);
    if (!journals)
    {
        return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::failure(
            std::move(*journals.try_error()));
    }
    std::optional<cue::distribution::InstallOperationJournal> result;
    std::size_t recoveryCount = 0U;
    for (const auto &item : *journals.try_value())
    {
        const auto &journal = item.first;
        if (journal.kind == cue::distribution::InstallOperationKind::RegistryRecovery)
        {
            if (++recoveryCount > 1U)
            {
                return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::failure(
                    install_error(a_assertContext, cue::distribution::DistributionError::InstallRecoveryBlocked,
                                  "Multiple Registry Recovery Journals exist"));
            }
            continue;
        }
        if (journal.kind == cue::distribution::InstallOperationKind::Uninstall && journal.target &&
            journal.target->directoryName == a_directoryName && !result)
        {
            result = journal;
            continue;
        }
        return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                          "Another incomplete Install operation requires recovery"));
    }
    return cue::Result<std::optional<cue::distribution::InstallOperationJournal>>::success(std::move(result));
}

/// @brief Operation IDとWorker IDが一致する単一Uninstall Journalを読取る
[[nodiscard]] cue::Result<cue::distribution::InstallOperationJournal> read_worker_uninstall_journal(
    const std::filesystem::path &a_installRoot, std::string_view a_operationId, std::string_view a_workerId,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto journals = read_all_journals(a_installRoot, a_assertContext);
    if (!journals)
    {
        return cue::Result<cue::distribution::InstallOperationJournal>::failure(std::move(*journals.try_error()));
    }
    std::optional<cue::distribution::InstallOperationJournal> result;
    std::size_t recoveryCount = 0U;
    for (const auto &item : *journals.try_value())
    {
        const cue::distribution::InstallOperationJournal &journal = item.first;
        if (journal.kind == cue::distribution::InstallOperationKind::RegistryRecovery && ++recoveryCount <= 1U)
        {
            continue;
        }
        if (!result && journal.kind == cue::distribution::InstallOperationKind::Uninstall && journal.target &&
            journal.operationId == a_operationId && journal.workerId == a_workerId)
        {
            result = journal;
            continue;
        }
        return cue::Result<cue::distribution::InstallOperationJournal>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                          "Uninstall Worker requires exactly one matching Journal"));
    }
    if (!result)
    {
        return cue::Result<cue::distribution::InstallOperationJournal>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                          "Uninstall Worker Journal is missing"));
    }
    return cue::Result<cue::distribution::InstallOperationJournal>::success(std::move(*result));
}

/// @brief RegistryへVersion EntryをExpected Generation／Revision照合付きでPublishする
[[nodiscard]] cue::Result<std::uint64_t> publish_registry_entry(
    const std::filesystem::path &a_installRoot, const cue::distribution::InstallOperationJournal &a_journal,
    const cue::distribution::InstalledVersionEntry &a_entry, const cue::AssertContext &a_assertContext) noexcept
{
    auto snapshot = read_registry(a_installRoot, a_assertContext);
    if (!snapshot || !snapshot.try_value()->registry)
    {
        return cue::Result<std::uint64_t>::failure(
            snapshot ? install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                                     "Installed Versions Registry disappeared during Install")
                     : std::move(*snapshot.try_error()));
    }
    auto registry = std::move(*snapshot.try_value()->registry);
    /// @brief 同じImmutable Version Directoryの既存Entryを検索する
    const auto existing = std::ranges::find_if(registry.versions, [&a_entry](const auto &a_version) noexcept
                                               { return a_version.directoryName == a_entry.directoryName; });
    if (existing != registry.versions.end())
    {
        if (*existing == a_entry && registry.generationId == a_journal.expectedRegistry->generationId &&
            registry.revision == a_journal.expectedRegistry->revision + 1U)
        {
            return cue::Result<std::uint64_t>::success(static_cast<std::uint64_t>(registry.revision));
        }
        return cue::Result<std::uint64_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                          "Installed Version identity collides with an existing Registry entry"));
    }
    if (registry.generationId != a_journal.expectedRegistry->generationId ||
        registry.revision != a_journal.expectedRegistry->revision)
    {
        return cue::Result<std::uint64_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                          "Installed Versions Registry generation or revision changed"));
    }
    registry.versions.push_back(a_entry);
    std::ranges::sort(registry.versions, {}, &cue::distribution::InstalledVersionEntry::directoryName);
    if (registry.selectedVersion.empty())
    {
        registry.selectedVersion = a_entry.directoryName;
    }
    ++registry.revision;
    auto written = write_registry(a_installRoot, registry, a_assertContext);
    if (!written)
    {
        return cue::Result<std::uint64_t>::failure(std::move(*written.try_error()));
    }
    return cue::Result<std::uint64_t>::success(static_cast<std::uint64_t>(registry.revision));
}

/// @brief Version EntryがRegistryと現行Evidenceで一致するか検証する
[[nodiscard]] cue::Result<std::uint64_t> validate_registry_entry(
    const std::filesystem::path &a_installRoot, const cue::distribution::InstalledVersionEntry &a_entry,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto snapshot = read_registry(a_installRoot, a_assertContext);
    if (!snapshot || !snapshot.try_value()->registry)
    {
        return cue::Result<std::uint64_t>::failure(
            snapshot ? install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                                     "Installed Versions Registry is unavailable")
                     : std::move(*snapshot.try_error()));
    }
    const auto &registry = *snapshot.try_value()->registry;
    /// @brief 再検証対象と同じVersion DirectoryのRegistry Entryを検索する
    const auto iterator = std::ranges::find_if(registry.versions, [&a_entry](const auto &a_version) noexcept
                                               { return a_version.directoryName == a_entry.directoryName; });
    if (iterator == registry.versions.end() || *iterator != a_entry)
    {
        return cue::Result<std::uint64_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::InstallConflict,
                          "Installed Version Registry entry could not be revalidated"));
    }
    return cue::Result<std::uint64_t>::success(static_cast<std::uint64_t>(registry.revision));
}

/// @brief Install Operation固有StagingとJournalを最終共有状態検証後に削除する
[[nodiscard]] cue::Result<void> cleanup_operation(const std::filesystem::path &a_installRoot,
                                                  std::string_view a_operationId,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code error;
    const std::filesystem::path staging =
        a_installRoot / L"Operations" / L"Staging" / to_wide(a_operationId).value_or(L"");
    const std::filesystem::path workerStaging =
        a_installRoot / L"Operations" / L"WorkerStaging" / to_wide(a_operationId).value_or(L"");
    std::filesystem::remove_all(extended_filesystem_path(staging), error);
    if (error)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Version Staging cleanup failed"));
    }
    std::filesystem::remove_all(extended_filesystem_path(workerStaging), error);
    if (error)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Install Worker Staging cleanup failed"));
    }
    const std::filesystem::path journal =
        a_installRoot / L"Operations" / L"Journals" / (to_wide(a_operationId).value_or(L"") + L".json");
    if (DeleteFileW(win32_path(journal).c_str()) == FALSE && GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        return cue::Result<void>::failure(install_error(a_assertContext,
                                                        cue::distribution::DistributionError::PlatformOperationFailed,
                                                        "Completed Install Journal cleanup failed"));
    }
    return cue::Result<void>::success();
}

/// @brief Rollback Journalを選択Publishから最終再検証まで冪等再開する
[[nodiscard]] cue::Result<cue::distribution::WindowsRollbackOutcome> execute_rollback(
    const std::filesystem::path &a_installRoot, cue::distribution::InstallOperationJournal a_journal,
    const cue::AssertContext &a_assertContext) noexcept
{
    using namespace cue::distribution;
    const std::string &directoryName = a_journal.target->directoryName;
    if (a_journal.expectedRegistry->revision == std::numeric_limits<std::uint64_t>::max())
    {
        return cue::Result<WindowsRollbackOutcome>::failure(
            install_error(a_assertContext, DistributionError::ResourceLimitExceeded,
                          "Installed Versions Registry revision cannot advance for Rollback"));
    }
    if (a_journal.stage == InstallOperationStage::Prepared)
    {
        auto snapshot = read_installed_version(a_installRoot, directoryName, false, a_assertContext);
        if (!snapshot || snapshot.try_value()->entry.bundleId != a_journal.target->bundleId ||
            snapshot.try_value()->entry.manifestDigest != a_journal.target->manifestDigest ||
            snapshot.try_value()->entry.workerId != a_journal.workerId)
        {
            return cue::Result<WindowsRollbackOutcome>::failure(
                snapshot ? install_error(a_assertContext, DistributionError::InstallConflict,
                                         "Rollback target Evidence changed")
                         : std::move(*snapshot.try_error()));
        }
        InstalledVersionsRegistry registry = std::move(snapshot.try_value()->registry);
        if (registry.generationId == a_journal.expectedRegistry->generationId &&
            registry.revision == a_journal.expectedRegistry->revision && registry.selectedVersion != directoryName)
        {
            registry.selectedVersion = directoryName;
            ++registry.revision;
            auto published = write_registry(a_installRoot, registry, a_assertContext);
            if (!published)
            {
                return cue::Result<WindowsRollbackOutcome>::failure(std::move(*published.try_error()));
            }
        }
        auto selected = read_installed_version(a_installRoot, directoryName, false, a_assertContext);
        if (!selected || selected.try_value()->registry.generationId != a_journal.expectedRegistry->generationId ||
            selected.try_value()->registry.revision != a_journal.expectedRegistry->revision + 1U ||
            selected.try_value()->registry.selectedVersion != directoryName)
        {
            return cue::Result<WindowsRollbackOutcome>::failure(
                selected ? install_error(a_assertContext, DistributionError::InstallConflict,
                                         "Rollback selection Publish could not be revalidated")
                         : std::move(*selected.try_error()));
        }
        auto advanced =
            advance_journal(a_installRoot, a_journal, InstallOperationStage::SelectionPublished, a_assertContext);
        if (!advanced)
        {
            return cue::Result<WindowsRollbackOutcome>::failure(std::move(*advanced.try_error()));
        }
    }
    auto selected = read_installed_version(a_installRoot, directoryName, false, a_assertContext);
    if (!selected || selected.try_value()->registry.generationId != a_journal.expectedRegistry->generationId ||
        selected.try_value()->registry.revision != a_journal.expectedRegistry->revision + 1U ||
        selected.try_value()->registry.selectedVersion != directoryName ||
        selected.try_value()->entry.bundleId != a_journal.target->bundleId ||
        selected.try_value()->entry.manifestDigest != a_journal.target->manifestDigest ||
        selected.try_value()->entry.workerId != a_journal.workerId)
    {
        return cue::Result<WindowsRollbackOutcome>::failure(
            selected ? install_error(a_assertContext, DistributionError::InstallConflict,
                                     "Completed Rollback state could not be revalidated")
                     : std::move(*selected.try_error()));
    }
    const std::uint64_t registryRevision = selected.try_value()->registry.revision;
    auto cleaned = cleanup_operation(a_installRoot, a_journal.operationId, a_assertContext);
    if (!cleaned)
    {
        return cue::Result<WindowsRollbackOutcome>::failure(std::move(*cleaned.try_error()));
    }
    WindowsRollbackOutcome outcome{a_journal.operationId, directoryName, registryRevision, false};
    return cue::Result<WindowsRollbackOutcome>::success(std::move(outcome));
}

/// @brief Uninstall完了時に許可する通常またはRegistry Recovery後のRevisionか返す
[[nodiscard]] bool is_completed_uninstall_revision(std::uint64_t a_revision, std::uint64_t a_expectedRevision) noexcept
{
    const bool recoveredRevision = a_revision == a_expectedRevision;
    const bool canAdvance = a_expectedRevision <= std::numeric_limits<std::uint64_t>::max() - 2U;
    return recoveredRevision || (canAdvance && a_revision == a_expectedRevision + 2U);
}

/// @brief Uninstall Journal対象VersionのQuarantine PathをOperation Identityから導出する
[[nodiscard]] std::filesystem::path uninstall_quarantine_path(
    const std::filesystem::path &a_installRoot, const cue::distribution::InstallOperationJournal &a_journal)
{
    return a_installRoot / L"Operations" / L"Quarantine" / L"Versions" /
           (to_wide(a_journal.target->directoryName).value_or(L"") + L"--" +
            to_wide(a_journal.operationId).value_or(L""));
}

/// @brief Uninstall Journal IdentityとPayload／Worker Evidenceの一致を検証する
[[nodiscard]] cue::Result<std::pair<BundleSnapshot, cue::distribution::InstalledVersionEntry>>
validate_uninstall_payload(const std::filesystem::path &a_installRoot, const std::filesystem::path &a_payloadRoot,
                           const cue::distribution::InstallOperationJournal &a_journal,
                           const cue::AssertContext &a_assertContext) noexcept
{
    using namespace cue::distribution;
    auto bundle = validate_bundle(a_payloadRoot, true, a_assertContext);
    if (!bundle)
    {
        return cue::Result<std::pair<BundleSnapshot, InstalledVersionEntry>>::failure(std::move(*bundle.try_error()));
    }
    auto entry = build_version_entry(a_installRoot, a_payloadRoot, *bundle.try_value(), a_journal.target->directoryName,
                                     a_assertContext);
    if (!entry)
    {
        return cue::Result<std::pair<BundleSnapshot, InstalledVersionEntry>>::failure(std::move(*entry.try_error()));
    }
    if (entry.try_value()->bundleId != a_journal.target->bundleId ||
        entry.try_value()->manifestDigest != a_journal.target->manifestDigest ||
        entry.try_value()->workerId != a_journal.workerId)
    {
        return cue::Result<std::pair<BundleSnapshot, InstalledVersionEntry>>::failure(
            install_error(a_assertContext, DistributionError::InstallConflict,
                          "Uninstall target Evidence does not match its Journal"));
    }
    return cue::Result<std::pair<BundleSnapshot, InstalledVersionEntry>>::success(
        std::pair(std::move(*bundle.try_value()), std::move(*entry.try_value())));
}

/// @brief Selectable VersionをPendingRemovalへ変更して新規起動を停止する
[[nodiscard]] cue::Result<void> publish_removal_block(const std::filesystem::path &a_installRoot,
                                                      const cue::distribution::InstallOperationJournal &a_journal,
                                                      const cue::AssertContext &a_assertContext) noexcept
{
    using namespace cue::distribution;
    auto snapshot = read_registry(a_installRoot, a_assertContext);
    if (!snapshot || !snapshot.try_value()->registry)
    {
        return cue::Result<void>::failure(
            snapshot ? install_error(a_assertContext, DistributionError::InstallConflict,
                                     "Installed Versions Registry is unavailable during Uninstall")
                     : std::move(*snapshot.try_error()));
    }
    InstalledVersionsRegistry registry = std::move(*snapshot.try_value()->registry);
    const auto target = std::ranges::find_if(registry.versions, [&a_journal](const InstalledVersionEntry &a_entry)
                                             { return a_entry.directoryName == a_journal.target->directoryName; });
    if (target == registry.versions.end())
    {
        if (registry.generationId != a_journal.expectedRegistry->generationId ||
            registry.revision != a_journal.expectedRegistry->revision)
        {
            return cue::Result<void>::failure(install_error(a_assertContext, DistributionError::InstallConflict,
                                                            "Recovered Registry does not match the Uninstall Journal"));
        }
        return cue::Result<void>::success();
    }
    if (target->bundleId != a_journal.target->bundleId || target->manifestDigest != a_journal.target->manifestDigest ||
        target->workerId != a_journal.workerId)
    {
        return cue::Result<void>::failure(install_error(a_assertContext, DistributionError::InstallConflict,
                                                        "Uninstall Registry Entry identity changed"));
    }
    if (target->state == InstalledVersionState::Selectable)
    {
        if (a_journal.expectedRegistry->revision > std::numeric_limits<std::uint64_t>::max() - 2U)
        {
            return cue::Result<void>::failure(
                install_error(a_assertContext, DistributionError::ResourceLimitExceeded,
                              "Installed Versions Registry revision cannot advance for Uninstall"));
        }
        if (registry.generationId != a_journal.expectedRegistry->generationId ||
            registry.revision != a_journal.expectedRegistry->revision)
        {
            return cue::Result<void>::failure(install_error(a_assertContext, DistributionError::InstallConflict,
                                                            "Installed Versions Registry changed before Uninstall"));
        }
        const auto replacement =
            std::ranges::find_if(registry.versions,
                                 [&a_journal](const InstalledVersionEntry &a_entry)
                                 {
                                     return a_entry.directoryName != a_journal.target->directoryName &&
                                            a_entry.state == InstalledVersionState::Selectable;
                                 });
        if (replacement == registry.versions.end())
        {
            return cue::Result<void>::failure(install_error(a_assertContext,
                                                            DistributionError::InstalledVersionProtected,
                                                            "The last selectable Installed Version cannot be removed"));
        }
        target->state = InstalledVersionState::PendingRemoval;
        if (registry.selectedVersion == target->directoryName)
        {
            registry.selectedVersion = replacement->directoryName;
        }
        ++registry.revision;
        auto published = write_registry(a_installRoot, registry, a_assertContext);
        if (!published)
        {
            return cue::Result<void>::failure(std::move(*published.try_error()));
        }
    }
    auto durable = read_registry(a_installRoot, a_assertContext);
    if (!durable || !durable.try_value()->registry)
    {
        return cue::Result<void>::failure(durable ? install_error(a_assertContext, DistributionError::InstallConflict,
                                                                  "PendingRemoval Registry Publish is unavailable")
                                                  : std::move(*durable.try_error()));
    }
    const InstalledVersionsRegistry &published = *durable.try_value()->registry;
    const auto blocked = std::ranges::find_if(published.versions, [&a_journal](const InstalledVersionEntry &a_entry)
                                              { return a_entry.directoryName == a_journal.target->directoryName; });
    if (published.generationId != a_journal.expectedRegistry->generationId ||
        published.revision != a_journal.expectedRegistry->revision + 1U || blocked == published.versions.end() ||
        blocked->state != InstalledVersionState::PendingRemoval ||
        published.selectedVersion == a_journal.target->directoryName)
    {
        return cue::Result<void>::failure(install_error(a_assertContext, DistributionError::InstallConflict,
                                                        "PendingRemoval Registry Publish could not be revalidated"));
    }
    return cue::Result<void>::success();
}

/// @brief Quarantine済みVersion EntryをRegistryからAtomic削除する
[[nodiscard]] cue::Result<void> publish_registry_removal(const std::filesystem::path &a_installRoot,
                                                         const cue::distribution::InstallOperationJournal &a_journal,
                                                         const cue::AssertContext &a_assertContext) noexcept
{
    using namespace cue::distribution;
    auto snapshot = read_registry(a_installRoot, a_assertContext);
    if (!snapshot || !snapshot.try_value()->registry)
    {
        return cue::Result<void>::failure(
            snapshot ? install_error(a_assertContext, DistributionError::InstallConflict,
                                     "Installed Versions Registry is unavailable during removal Publish")
                     : std::move(*snapshot.try_error()));
    }
    InstalledVersionsRegistry registry = std::move(*snapshot.try_value()->registry);
    auto target = std::ranges::find_if(registry.versions, [&a_journal](const InstalledVersionEntry &a_entry)
                                       { return a_entry.directoryName == a_journal.target->directoryName; });
    if (target != registry.versions.end())
    {
        if (target->state != InstalledVersionState::PendingRemoval ||
            registry.generationId != a_journal.expectedRegistry->generationId ||
            registry.revision != a_journal.expectedRegistry->revision + 1U ||
            target->bundleId != a_journal.target->bundleId ||
            target->manifestDigest != a_journal.target->manifestDigest || target->workerId != a_journal.workerId)
        {
            return cue::Result<void>::failure(install_error(a_assertContext, DistributionError::InstallConflict,
                                                            "PendingRemoval Registry Entry changed before removal"));
        }
        registry.versions.erase(target);
        ++registry.revision;
        auto published = write_registry(a_installRoot, registry, a_assertContext);
        if (!published)
        {
            return cue::Result<void>::failure(std::move(*published.try_error()));
        }
    }
    auto durable = read_registry(a_installRoot, a_assertContext);
    if (!durable || !durable.try_value()->registry)
    {
        return cue::Result<void>::failure(durable ? install_error(a_assertContext, DistributionError::InstallConflict,
                                                                  "Registry removal Publish is unavailable")
                                                  : std::move(*durable.try_error()));
    }
    const InstalledVersionsRegistry &published = *durable.try_value()->registry;
    const bool targetAbsent =
        std::ranges::none_of(published.versions, [&a_journal](const InstalledVersionEntry &a_entry)
                             { return a_entry.directoryName == a_journal.target->directoryName; });
    if (published.generationId != a_journal.expectedRegistry->generationId || !targetAbsent ||
        !is_completed_uninstall_revision(published.revision, a_journal.expectedRegistry->revision))
    {
        return cue::Result<void>::failure(install_error(a_assertContext, DistributionError::InstallConflict,
                                                        "Registry removal Publish could not be revalidated"));
    }
    return cue::Result<void>::success();
}

/// @brief Uninstall JournalをPendingRemoval、Quarantine、Registry削除の順で冪等再開する
[[nodiscard]] cue::Result<void> execute_uninstall(const std::filesystem::path &a_installRoot,
                                                  cue::distribution::InstallOperationJournal a_journal,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    using namespace cue::distribution;
    const std::filesystem::path versionRoot =
        a_installRoot / L"Versions" / to_wide(a_journal.target->directoryName).value_or(L"");
    const std::filesystem::path quarantine = uninstall_quarantine_path(a_installRoot, a_journal);
    if (a_journal.stage == InstallOperationStage::Prepared)
    {
        auto evidence = validate_uninstall_payload(a_installRoot, versionRoot, a_journal, a_assertContext);
        if (!evidence)
        {
            return cue::Result<void>::failure(std::move(*evidence.try_error()));
        }
        auto blocked = publish_removal_block(a_installRoot, a_journal, a_assertContext);
        if (!blocked)
        {
            return cue::Result<void>::failure(std::move(*blocked.try_error()));
        }
        auto advanced =
            advance_journal(a_installRoot, a_journal, InstallOperationStage::RemovalBlocked, a_assertContext);
        if (!advanced)
        {
            return cue::Result<void>::failure(std::move(*advanced.try_error()));
        }
    }
    if (a_journal.stage == InstallOperationStage::RemovalBlocked)
    {
        const bool hasVersion = is_plain_directory(versionRoot);
        const bool hasQuarantine = is_plain_directory(quarantine);
        if (hasVersion == hasQuarantine)
        {
            return cue::Result<void>::failure(install_error(a_assertContext, DistributionError::InstallConflict,
                                                            "Uninstall Version and Quarantine state is ambiguous"));
        }
        if (hasVersion)
        {
            auto evidence = validate_uninstall_payload(a_installRoot, versionRoot, a_journal, a_assertContext);
            if (!evidence)
            {
                return cue::Result<void>::failure(std::move(*evidence.try_error()));
            }
            if (MoveFileExW(win32_path(versionRoot).c_str(), win32_path(quarantine).c_str(), MOVEFILE_WRITE_THROUGH) ==
                FALSE)
            {
                return cue::Result<void>::failure(
                    install_error(a_assertContext, DistributionError::PlatformOperationFailed,
                                  "Installed Version could not be moved to Uninstall Quarantine"));
            }
        }
        auto quarantined = validate_uninstall_payload(a_installRoot, quarantine, a_journal, a_assertContext);
        if (!quarantined)
        {
            return cue::Result<void>::failure(std::move(*quarantined.try_error()));
        }
        auto advanced =
            advance_journal(a_installRoot, a_journal, InstallOperationStage::VersionQuarantined, a_assertContext);
        if (!advanced)
        {
            return cue::Result<void>::failure(std::move(*advanced.try_error()));
        }
    }
    if (a_journal.stage == InstallOperationStage::VersionQuarantined)
    {
        auto quarantined = validate_uninstall_payload(a_installRoot, quarantine, a_journal, a_assertContext);
        if (!quarantined)
        {
            return cue::Result<void>::failure(std::move(*quarantined.try_error()));
        }
        auto removed = publish_registry_removal(a_installRoot, a_journal, a_assertContext);
        if (!removed)
        {
            return cue::Result<void>::failure(std::move(*removed.try_error()));
        }
        auto advanced =
            advance_journal(a_installRoot, a_journal, InstallOperationStage::RegistryEntryRemoved, a_assertContext);
        if (!advanced)
        {
            return cue::Result<void>::failure(std::move(*advanced.try_error()));
        }
    }
    auto finalRegistry = read_registry(a_installRoot, a_assertContext);
    if (!finalRegistry || !finalRegistry.try_value()->registry)
    {
        return cue::Result<void>::failure(finalRegistry
                                              ? install_error(a_assertContext, DistributionError::InstallConflict,
                                                              "Completed Uninstall Registry is unavailable")
                                              : std::move(*finalRegistry.try_error()));
    }
    const InstalledVersionsRegistry &registry = *finalRegistry.try_value()->registry;
    const bool targetAbsent =
        std::ranges::none_of(registry.versions, [&a_journal](const InstalledVersionEntry &a_entry)
                             { return a_entry.directoryName == a_journal.target->directoryName; });
    if (registry.generationId != a_journal.expectedRegistry->generationId || !targetAbsent ||
        !is_completed_uninstall_revision(registry.revision, a_journal.expectedRegistry->revision))
    {
        return cue::Result<void>::failure(install_error(a_assertContext, DistributionError::InstallConflict,
                                                        "Completed Uninstall Registry could not be revalidated"));
    }
    if (is_plain_directory(quarantine))
    {
        auto quarantined = validate_uninstall_payload(a_installRoot, quarantine, a_journal, a_assertContext);
        if (!quarantined)
        {
            return cue::Result<void>::failure(std::move(*quarantined.try_error()));
        }
        std::error_code error;
        std::filesystem::remove_all(extended_filesystem_path(quarantine), error);
        if (error)
        {
            return cue::Result<void>::failure(install_error(a_assertContext, DistributionError::PlatformOperationFailed,
                                                            "Uninstall Quarantine cleanup failed"));
        }
    }
    auto cleaned = cleanup_operation(a_installRoot, a_journal.operationId, a_assertContext);
    if (!cleaned)
    {
        return cleaned;
    }
    return cue::Result<void>::success();
}

/// @brief 現在Process Imageが検証済み外部Worker Pathから起動されたか確認する
[[nodiscard]] bool is_current_worker_executable(const std::filesystem::path &a_expected) noexcept
{
    std::array<wchar_t, 32768U> currentPath{};
    const DWORD length = GetModuleFileNameW(nullptr, currentPath.data(), static_cast<DWORD>(currentPath.size()));
    if (length == 0U || length >= currentPath.size())
    {
        return false;
    }
    const std::filesystem::path current(std::wstring_view(currentPath.data(), length));
    HandleOwner expectedHandle(CreateFileW(win32_path(a_expected).c_str(), FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    HandleOwner currentHandle(CreateFileW(win32_path(current).c_str(), FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const auto expectedFinal = expectedHandle.valid() ? handle_path(expectedHandle.get()) : std::nullopt;
    const auto currentFinal = currentHandle.valid() ? handle_path(currentHandle.get()) : std::nullopt;
    return expectedFinal && currentFinal && _wcsicmp(expectedFinal->c_str(), currentFinal->c_str()) == 0;
}

/// @brief Version外WorkerへGate Handleだけを継承してUninstall再開を委譲する
[[nodiscard]] cue::Result<std::uint32_t> launch_uninstall_worker(
    const std::filesystem::path &a_installRoot, const cue::distribution::InstallOperationJournal &a_journal,
    ControlLease &a_controlLease, const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path executable = a_installRoot / L"Operations" / L"Workers" /
                                             to_wide(a_journal.workerId).value_or(L"") / L"CueEngineInstallWorker.exe";
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HandleOwner gate(CreateEventW(&security, TRUE, FALSE, nullptr));
    if (!gate.valid())
    {
        return cue::Result<std::uint32_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Uninstall Worker release gate could not be created"));
    }
    const auto installUtf8 = to_utf8(a_installRoot.native());
    if (!installUtf8)
    {
        return cue::Result<std::uint32_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Install Root could not be encoded for Uninstall Worker"));
    }
    std::wstring command = quote_argument(win32_path(executable));
    const std::array<std::string, 9U> arguments = {
        "--uninstall-worker",  "--gate-handle", std::to_string(reinterpret_cast<std::uintptr_t>(gate.get())),
        "--install-root",      *installUtf8,    "--operation-id",
        a_journal.operationId, "--worker-id",   a_journal.workerId,
    };
    for (const std::string &argument : arguments)
    {
        command.push_back(L' ');
        command.append(quote_argument(to_wide(argument).value_or(L"")));
    }
    SIZE_T attributeBytes = 0U;
    static_cast<void>(InitializeProcThreadAttributeList(nullptr, 1U, 0U, &attributeBytes));
    if (attributeBytes == 0U)
    {
        return cue::Result<std::uint32_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Uninstall Worker inheritance allowlist size could not be determined"));
    }
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (InitializeProcThreadAttributeList(attributes, 1U, 0U, &attributeBytes) == FALSE)
    {
        return cue::Result<std::uint32_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Uninstall Worker inheritance allowlist could not be configured"));
    }
    HANDLE inheritedHandle = gate.get();
    if (UpdateProcThreadAttribute(attributes, 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inheritedHandle,
                                  sizeof(inheritedHandle), nullptr, nullptr) == FALSE)
    {
        DeleteProcThreadAttributeList(attributes);
        return cue::Result<std::uint32_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Uninstall Worker inheritance Handle could not be configured"));
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    auto environment = make_probe_environment();
    if (!environment)
    {
        DeleteProcThreadAttributeList(attributes);
        return cue::Result<std::uint32_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Uninstall Worker environment could not be constructed"));
    }
    const BOOL created = CreateProcessW(win32_path(executable).c_str(), command.data(), nullptr, nullptr, TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                        environment->data(), nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    if (created == FALSE)
    {
        return cue::Result<std::uint32_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Uninstall Worker process could not be created"));
    }
    HandleOwner processHandle(process.hProcess);
    HandleOwner threadHandle(process.hThread);
    const std::uint32_t processId = process.dwProcessId;
    a_controlLease.release();
    if (SetEvent(gate.get()) == FALSE)
    {
        static_cast<void>(TerminateProcess(processHandle.get(), 90U));
        static_cast<void>(WaitForSingleObject(processHandle.get(), INFINITE));
        return cue::Result<std::uint32_t>::failure(
            install_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                          "Uninstall Worker release gate could not be signaled"));
    }
    return cue::Result<std::uint32_t>::success(std::uint32_t{processId});
}

/// @brief Install／Update Journalを現在Stageの直後から冪等再開する
[[nodiscard]] cue::Result<cue::distribution::WindowsInstallOutcome> execute_install(
    const std::filesystem::path &a_bundleRoot, const std::filesystem::path &a_installRoot,
    const BundleSnapshot &a_bundle, ControlLease &a_lease, cue::distribution::InstallOperationJournal a_journal,
    const cue::AssertContext &a_assertContext) noexcept
{
    using namespace cue::distribution;
    using cue::Result;
    const std::string &directoryName = a_journal.target->directoryName;
    const std::filesystem::path versionRoot = a_installRoot / L"Versions" / to_wide(directoryName).value_or(L"");
    const std::filesystem::path operationRoot =
        a_installRoot / L"Operations" / L"Staging" / to_wide(a_journal.operationId).value_or(L"");
    const std::filesystem::path stagingRoot = operationRoot / L"Version";

    if (a_journal.stage == InstallOperationStage::Prepared)
    {
        bool reuseStaging = false;
        if (is_plain_directory(stagingRoot))
        {
            auto existing = validate_published_payload(stagingRoot, a_bundle, directoryName, a_assertContext);
            reuseStaging = existing.has_value();
        }
        if (!reuseStaging)
        {
            std::error_code error;
            std::filesystem::remove_all(extended_filesystem_path(operationRoot), error);
            if (error)
            {
                return Result<WindowsInstallOutcome>::failure(
                    install_error(a_assertContext, DistributionError::PlatformOperationFailed,
                                  "Abandoned Version Staging could not be recovered"));
            }
            auto copied = copy_bundle(a_bundleRoot, stagingRoot, a_bundle, a_assertContext);
            if (!copied)
            {
                return Result<WindowsInstallOutcome>::failure(std::move(*copied.try_error()));
            }
            auto marker = write_payload_marker(stagingRoot, a_bundle, directoryName, a_assertContext);
            if (!marker)
            {
                return Result<WindowsInstallOutcome>::failure(std::move(*marker.try_error()));
            }
        }
        auto staged = validate_published_payload(stagingRoot, a_bundle, directoryName, a_assertContext);
        if (!staged)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*staged.try_error()));
        }
        auto advanced =
            advance_journal(a_installRoot, a_journal, InstallOperationStage::PayloadStaged, a_assertContext);
        if (!advanced)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*advanced.try_error()));
        }
    }
    if (a_journal.stage == InstallOperationStage::PayloadStaged)
    {
        if (!is_plain_directory(versionRoot))
        {
            std::error_code error;
            std::filesystem::create_directories(extended_filesystem_path(versionRoot.parent_path()), error);
            if (error)
            {
                return Result<WindowsInstallOutcome>::failure(install_error(a_assertContext,
                                                                            DistributionError::PlatformOperationFailed,
                                                                            "Versions directory could not be created"));
            }
            auto published = publish_directory(stagingRoot, versionRoot, a_assertContext);
            if (!published)
            {
                return Result<WindowsInstallOutcome>::failure(std::move(*published.try_error()));
            }
        }
        auto validated = validate_published_payload(versionRoot, a_bundle, directoryName, a_assertContext);
        if (!validated)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*validated.try_error()));
        }
        auto advanced =
            advance_journal(a_installRoot, a_journal, InstallOperationStage::VersionPublished, a_assertContext);
        if (!advanced)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*advanced.try_error()));
        }
    }
    if (a_journal.stage == InstallOperationStage::VersionPublished)
    {
        if (is_plain_directory(versionRoot))
        {
            auto probeTemporariesCleaned = cleanup_probe_marker_temporaries(versionRoot, a_assertContext);
            if (!probeTemporariesCleaned)
            {
                return Result<WindowsInstallOutcome>::failure(std::move(*probeTemporariesCleaned.try_error()));
            }
        }
        auto probe = run_probe_process(a_installRoot, versionRoot, directoryName, a_journal.operationId,
                                       a_bundle.manifestDigest, a_lease.handle(), a_assertContext);
        if (!probe)
        {
            const std::filesystem::path quarantine =
                a_installRoot / L"Operations" / L"Quarantine" /
                (to_wide(directoryName).value_or(L"") + L"-" + to_wide(a_journal.operationId).value_or(L""));
            const DWORD quarantineAttributes = GetFileAttributesW(win32_path(quarantine).c_str());
            bool quarantined = is_plain_directory(quarantine);
            if (is_plain_directory(versionRoot))
            {
                std::error_code error;
                if (quarantineAttributes != INVALID_FILE_ATTRIBUTES)
                {
                    return Result<WindowsInstallOutcome>::failure(
                        install_error(a_assertContext, DistributionError::InstallConflict,
                                      "Install Probe quarantine destination already exists"));
                }
                std::filesystem::create_directories(extended_filesystem_path(quarantine.parent_path()), error);
                quarantined = !error && MoveFileExW(win32_path(versionRoot).c_str(), win32_path(quarantine).c_str(),
                                                    MOVEFILE_WRITE_THROUGH) != FALSE;
            }
            if (quarantined)
            {
                auto validatedQuarantine =
                    validate_published_payload(quarantine, a_bundle, directoryName, a_assertContext);
                if (!validatedQuarantine)
                {
                    return Result<WindowsInstallOutcome>::failure(std::move(*validatedQuarantine.try_error()));
                }
                auto cleaned = cleanup_operation(a_installRoot, a_journal.operationId, a_assertContext);
                if (!cleaned)
                {
                    return Result<WindowsInstallOutcome>::failure(std::move(*cleaned.try_error()));
                }
            }
            return Result<WindowsInstallOutcome>::failure(std::move(*probe.try_error()));
        }
        auto marker = write_probe_marker(versionRoot, a_bundle, directoryName, a_journal.operationId, a_assertContext);
        if (!marker)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*marker.try_error()));
        }
        auto advanced =
            advance_journal(a_installRoot, a_journal, InstallOperationStage::ProbeSucceeded, a_assertContext);
        if (!advanced)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*advanced.try_error()));
        }
    }
    if (a_journal.stage == InstallOperationStage::ProbeSucceeded)
    {
        auto worker = publish_worker(a_installRoot, versionRoot, a_bundle, a_journal.operationId, a_assertContext);
        if (!worker)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*worker.try_error()));
        }
        auto advanced =
            advance_journal(a_installRoot, a_journal, InstallOperationStage::WorkerPublished, a_assertContext);
        if (!advanced)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*advanced.try_error()));
        }
    }
    auto entry = build_version_entry(a_installRoot, versionRoot, a_bundle, directoryName, a_assertContext);
    if (!entry)
    {
        return Result<WindowsInstallOutcome>::failure(std::move(*entry.try_error()));
    }
    std::uint64_t registryRevision = 0U;
    if (a_journal.stage == InstallOperationStage::WorkerPublished)
    {
        auto published = publish_registry_entry(a_installRoot, a_journal, *entry.try_value(), a_assertContext);
        if (!published)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*published.try_error()));
        }
        registryRevision = *published.try_value();
        auto advanced =
            advance_journal(a_installRoot, a_journal, InstallOperationStage::RegistryPublished, a_assertContext);
        if (!advanced)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*advanced.try_error()));
        }
    }
    auto validatedRegistry = validate_registry_entry(a_installRoot, *entry.try_value(), a_assertContext);
    if (!validatedRegistry)
    {
        return Result<WindowsInstallOutcome>::failure(std::move(*validatedRegistry.try_error()));
    }
    registryRevision = *validatedRegistry.try_value();
    auto cleaned = cleanup_operation(a_installRoot, a_journal.operationId, a_assertContext);
    if (!cleaned)
    {
        return Result<WindowsInstallOutcome>::failure(std::move(*cleaned.try_error()));
    }
    WindowsInstallOutcome outcome{a_journal.operationId, directoryName, a_bundle.workerId, registryRevision, false};
    return Result<WindowsInstallOutcome>::success(std::move(outcome));
}
} // namespace

namespace cue::distribution
{
WindowsInstalledVersionExecutionLease::WindowsInstalledVersionExecutionLease(std::uintptr_t a_handle,
                                                                             std::string a_installRoot,
                                                                             std::string a_versionDirectory) noexcept
    : m_handle(a_handle), m_installRoot(std::move(a_installRoot)), m_versionDirectory(std::move(a_versionDirectory))
{
}

WindowsInstalledVersionExecutionLease::WindowsInstalledVersionExecutionLease(
    WindowsInstalledVersionExecutionLease &&a_other) noexcept
    : m_handle(std::exchange(a_other.m_handle, 0U)), m_installRoot(std::move(a_other.m_installRoot)),
      m_versionDirectory(std::move(a_other.m_versionDirectory))
{
}

WindowsInstalledVersionExecutionLease &WindowsInstalledVersionExecutionLease::operator=(
    WindowsInstalledVersionExecutionLease &&a_other) noexcept
{
    if (this != &a_other)
    {
        if (m_handle != 0U)
        {
            static_cast<void>(CloseHandle(reinterpret_cast<HANDLE>(m_handle)));
        }
        m_handle = std::exchange(a_other.m_handle, 0U);
        m_installRoot = std::move(a_other.m_installRoot);
        m_versionDirectory = std::move(a_other.m_versionDirectory);
    }
    return *this;
}

WindowsInstalledVersionExecutionLease::~WindowsInstalledVersionExecutionLease() noexcept
{
    if (m_handle != 0U)
    {
        static_cast<void>(CloseHandle(reinterpret_cast<HANDLE>(m_handle)));
    }
}

std::uintptr_t WindowsInstalledVersionExecutionLease::native_handle() const noexcept
{
    return m_handle;
}

const std::string &WindowsInstalledVersionExecutionLease::install_root() const noexcept
{
    return m_installRoot;
}

const std::string &WindowsInstalledVersionExecutionLease::version_directory() const noexcept
{
    return m_versionDirectory;
}

Result<WindowsInstallOutcome> install_windows_source_sdk(const WindowsInstallRequest &a_request,
                                                         const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!a_request.allowUnsignedLocal)
        {
            return Result<WindowsInstallOutcome>::failure(
                make_distribution_error(a_assertContext, DistributionError::BundleValidationFailed,
                                        "Unsigned Local Developer Bundle requires explicit allowUnsignedLocal"));
        }
        auto bundleRoot = normalize_absolute(a_request.bundleRoot);
        auto installRoot = normalize_absolute(a_request.installRoot);
        if (!bundleRoot || !installRoot || !is_local_fixed_path(*bundleRoot) ||
            !has_plain_existing_ancestry(*bundleRoot) || !is_local_fixed_path(*installRoot) ||
            !has_plain_existing_ancestry(*installRoot))
        {
            return Result<WindowsInstallOutcome>::failure(make_distribution_error(
                a_assertContext, DistributionError::BundleValidationFailed,
                "Bundle Root and Install Root must be absolute local paths without reparse ancestors"));
        }
        auto bundle = validate_bundle(*bundleRoot, false, a_assertContext);
        if (!bundle)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*bundle.try_error()));
        }
        std::error_code error;
        std::filesystem::create_directories(extended_filesystem_path(*installRoot), error);
        if (error || !is_plain_directory(*installRoot) || !has_plain_existing_ancestry(*installRoot))
        {
            return Result<WindowsInstallOutcome>::failure(
                make_distribution_error(a_assertContext, DistributionError::PlatformOperationFailed,
                                        "Install Root could not be created as a plain directory"));
        }
        auto lease = acquire_control_lease(*installRoot, a_assertContext);
        if (!lease)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*lease.try_error()));
        }
        auto directories = ensure_managed_directories(*installRoot, a_assertContext);
        if (!directories)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*directories.try_error()));
        }
        auto registry = ensure_registry(*installRoot, bundle.try_value()->workerId, a_assertContext);
        if (!registry)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*registry.try_error()));
        }
        auto directory = make_distribution_version_directory(bundle.try_value()->manifest.engineVersion,
                                                             bundle.try_value()->manifest.bundleId, a_assertContext);
        if (!directory)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*directory.try_error()));
        }
        auto resumable =
            find_resumable_journal(*installRoot, *directory.try_value(), *bundle.try_value(), a_assertContext);
        if (!resumable)
        {
            return Result<WindowsInstallOutcome>::failure(std::move(*resumable.try_error()));
        }
        /// @brief 同じVersion Identityが既に導入済みか検索する
        const auto existing = std::ranges::find_if(registry.try_value()->versions,
                                                   [&directory](const InstalledVersionEntry &a_entry) noexcept
                                                   { return a_entry.directoryName == *directory.try_value(); });
        if (existing != registry.try_value()->versions.end())
        {
            auto rebuilt = build_version_entry(
                *installRoot, *installRoot / L"Versions" / to_wide(*directory.try_value()).value_or(L""),
                *bundle.try_value(), *directory.try_value(), a_assertContext);
            if (!rebuilt || *rebuilt.try_value() != *existing)
            {
                return Result<WindowsInstallOutcome>::failure(
                    make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                            "Existing Installed Version does not match the requested Bundle"));
            }
            if (*resumable.try_value())
            {
                auto resumed = execute_install(*bundleRoot, *installRoot, *bundle.try_value(), *lease.try_value(),
                                               std::move(**resumable.try_value()), a_assertContext);
                if (!resumed)
                {
                    return resumed;
                }
                resumed.try_value()->wasAlreadyInstalled = true;
                return resumed;
            }
            WindowsInstallOutcome outcome{"", *directory.try_value(), bundle.try_value()->workerId,
                                          registry.try_value()->revision, true};
            return Result<WindowsInstallOutcome>::success(std::move(outcome));
        }
        InstallOperationJournal journal;
        if (*resumable.try_value())
        {
            journal = std::move(**resumable.try_value());
        }
        else
        {
            auto operationId = make_uuid();
            if (!operationId)
            {
                return Result<WindowsInstallOutcome>::failure(
                    make_distribution_error(a_assertContext, DistributionError::PlatformOperationFailed,
                                            "Install operation ID could not be generated"));
            }
            journal.operationId = std::move(*operationId);
            journal.kind = a_request.isUpdate ? InstallOperationKind::Update : InstallOperationKind::Install;
            journal.stage = InstallOperationStage::Prepared;
            journal.workerId = bundle.try_value()->workerId;
            journal.expectedRegistry =
                ExpectedRegistry{registry.try_value()->generationId, registry.try_value()->revision};
            journal.target = InstallOperationTarget{*directory.try_value(), bundle.try_value()->manifest.bundleId,
                                                    bundle.try_value()->manifestDigest};
            auto written = write_journal(*installRoot, journal, a_assertContext);
            if (!written)
            {
                return Result<WindowsInstallOutcome>::failure(std::move(*written.try_error()));
            }
        }
        return execute_install(*bundleRoot, *installRoot, *bundle.try_value(), *lease.try_value(), std::move(journal),
                               a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<void> run_windows_install_probe(const WindowsInstallProbeRequest &a_request,
                                       const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_request.leaseHandle == 0U || !is_canonical_bundle_id(a_request.operationId) ||
            !is_canonical_sha256(a_request.manifestDigest))
        {
            return Result<void>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidInstallState, "Install Probe request identity is invalid"));
        }
        auto installRoot = normalize_absolute(a_request.installRoot);
        if (!installRoot || !is_plain_directory(*installRoot))
        {
            return Result<void>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidInstallState, "Install Probe root is invalid"));
        }
        HANDLE leaseHandle = reinterpret_cast<HANDLE>(a_request.leaseHandle);
        DWORD handleFlags = 0U;
        FILE_STANDARD_INFO information{};
        if (GetHandleInformation(leaseHandle, &handleFlags) == FALSE || (handleFlags & HANDLE_FLAG_INHERIT) == 0U)
        {
            return Result<void>::failure(
                make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                        "Install Probe did not inherit the Install Root Control Lease handle"));
        }
        if (GetFileInformationByHandleEx(leaseHandle, FileStandardInfo, &information, sizeof(information)) == FALSE ||
            information.Directory != FALSE)
        {
            return Result<void>::failure(make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                                                 "Install Probe Control Lease handle is not a file"));
        }
        const std::filesystem::path operations = *installRoot / L"Operations";
        HandleOwner operationsHandle(CreateFileW(win32_path(operations).c_str(), FILE_READ_ATTRIBUTES,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                 OPEN_EXISTING,
                                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!operationsHandle.valid())
        {
            return Result<void>::failure(make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                                                 "Install Probe Operations directory is invalid"));
        }
        FILE_ATTRIBUTE_TAG_INFO operationsInformation{};
        auto expectedDirectory = handle_path(operationsHandle.get());
        if (GetFileInformationByHandleEx(operationsHandle.get(), FileAttributeTagInfo, &operationsInformation,
                                         sizeof(operationsInformation)) == FALSE ||
            (operationsInformation.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (operationsInformation.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U || !expectedDirectory)
        {
            return Result<void>::failure(make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                                                 "Install Probe Operations directory is invalid"));
        }
        expectedDirectory->push_back(L'\\');
        expectedDirectory->append(L"CueEngine.control.lock");
        auto inheritedPath = handle_path(leaseHandle);
        if (!inheritedPath || CompareStringOrdinal(inheritedPath->data(), static_cast<int>(inheritedPath->size()),
                                                   expectedDirectory->data(),
                                                   static_cast<int>(expectedDirectory->size()), TRUE) != CSTR_EQUAL)
        {
            return Result<void>::failure(
                make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                        "Install Probe Control Lease path does not match the Install Root"));
        }
        if (SetHandleInformation(leaseHandle, HANDLE_FLAG_INHERIT, 0U) == FALSE)
        {
            return Result<void>::failure(
                make_distribution_error(a_assertContext, DistributionError::PlatformOperationFailed,
                                        "Install Probe could not limit Control Lease inheritance"));
        }
        const std::filesystem::path journalPath =
            *installRoot / L"Operations" / L"Journals" / (to_wide(a_request.operationId).value_or(L"") + L".json");
        auto journal = read_journal(journalPath, a_assertContext);
        if (!journal || !journal.try_value()->first.target ||
            journal.try_value()->first.stage != InstallOperationStage::VersionPublished ||
            journal.try_value()->first.target->directoryName != a_request.versionDirectory ||
            journal.try_value()->first.target->manifestDigest != a_request.manifestDigest)
        {
            return Result<void>::failure(
                make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                        "Install Probe Journal does not match the inherited operation"));
        }
        const std::filesystem::path versionRoot =
            *installRoot / L"Versions" / to_wide(a_request.versionDirectory).value_or(L"");
        auto bundle = validate_bundle(versionRoot, true, a_assertContext);
        if (!bundle || bundle.try_value()->manifestDigest != a_request.manifestDigest)
        {
            return Result<void>::failure(
                bundle ? make_distribution_error(a_assertContext, DistributionError::BundleValidationFailed,
                                                 "Install Probe Manifest digest does not match")
                       : std::move(*bundle.try_error()));
        }
        auto payload =
            validate_published_payload(versionRoot, *bundle.try_value(), a_request.versionDirectory, a_assertContext);
        if (!payload)
        {
            return Result<void>::failure(std::move(*payload.try_error()));
        }
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<WindowsRollbackOutcome> rollback_windows_installed_version(const WindowsInstalledVersionRequest &a_request,
                                                                  const AssertContext &a_assertContext) noexcept
{
    try
    {
        auto installRoot = normalize_absolute(a_request.installRoot);
        if (!installRoot || !is_local_fixed_path(*installRoot) || !is_plain_directory(*installRoot) ||
            !has_plain_existing_ancestry(*installRoot) || !is_version_directory(a_request.versionDirectory))
        {
            return Result<WindowsRollbackOutcome>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidInstallState, "Rollback request paths are invalid"));
        }
        auto controlLease = acquire_control_lease(*installRoot, a_assertContext);
        if (!controlLease)
        {
            return Result<WindowsRollbackOutcome>::failure(std::move(*controlLease.try_error()));
        }
        auto directories = ensure_managed_directories(*installRoot, a_assertContext);
        if (!directories)
        {
            return Result<WindowsRollbackOutcome>::failure(std::move(*directories.try_error()));
        }
        const std::filesystem::path versionRoot =
            *installRoot / L"Versions" / to_wide(a_request.versionDirectory).value_or(L"");
        auto bundle = validate_bundle(versionRoot, true, a_assertContext);
        if (!bundle)
        {
            return Result<WindowsRollbackOutcome>::failure(std::move(*bundle.try_error()));
        }
        auto registry = ensure_registry(*installRoot, bundle.try_value()->workerId, a_assertContext);
        if (!registry)
        {
            return Result<WindowsRollbackOutcome>::failure(std::move(*registry.try_error()));
        }
        auto snapshot = read_installed_version(*installRoot, a_request.versionDirectory, false, a_assertContext);
        if (!snapshot)
        {
            return Result<WindowsRollbackOutcome>::failure(std::move(*snapshot.try_error()));
        }
        auto resumable =
            find_version_operation_journal(*installRoot, InstallOperationKind::Rollback, snapshot.try_value()->entry,
                                           snapshot.try_value()->bundle.workerId, a_assertContext);
        if (!resumable)
        {
            return Result<WindowsRollbackOutcome>::failure(std::move(*resumable.try_error()));
        }
        if (!resumable.try_value()->has_value() &&
            snapshot.try_value()->registry.selectedVersion == a_request.versionDirectory)
        {
            WindowsRollbackOutcome outcome{"", a_request.versionDirectory, snapshot.try_value()->registry.revision,
                                           true};
            return Result<WindowsRollbackOutcome>::success(std::move(outcome));
        }
        InstallOperationJournal journal;
        if (resumable.try_value()->has_value())
        {
            journal = std::move(**resumable.try_value());
        }
        else
        {
            auto operationId = make_uuid();
            if (!operationId)
            {
                return Result<WindowsRollbackOutcome>::failure(
                    make_distribution_error(a_assertContext, DistributionError::PlatformOperationFailed,
                                            "Rollback operation ID could not be generated"));
            }
            journal.operationId = std::move(*operationId);
            journal.kind = InstallOperationKind::Rollback;
            journal.stage = InstallOperationStage::Prepared;
            journal.workerId = snapshot.try_value()->entry.workerId;
            journal.expectedRegistry =
                ExpectedRegistry{snapshot.try_value()->registry.generationId, snapshot.try_value()->registry.revision};
            journal.target =
                InstallOperationTarget{snapshot.try_value()->entry.directoryName, snapshot.try_value()->entry.bundleId,
                                       snapshot.try_value()->entry.manifestDigest};
            auto written = write_journal(*installRoot, journal, a_assertContext);
            if (!written)
            {
                return Result<WindowsRollbackOutcome>::failure(std::move(*written.try_error()));
            }
        }
        return execute_rollback(*installRoot, std::move(journal), a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<WindowsInstalledVersionExecutionLease> acquire_windows_installed_version_execution_lease(
    const WindowsInstalledVersionRequest &a_request, const AssertContext &a_assertContext) noexcept
{
    try
    {
        auto installRoot = normalize_absolute(a_request.installRoot);
        if (!installRoot || !is_local_fixed_path(*installRoot) || !is_plain_directory(*installRoot) ||
            !has_plain_existing_ancestry(*installRoot) || !is_version_directory(a_request.versionDirectory))
        {
            return Result<WindowsInstalledVersionExecutionLease>::failure(
                make_distribution_error(a_assertContext, DistributionError::InvalidInstallState,
                                        "Installed Version launch request is invalid"));
        }
        auto controlLease = acquire_shared_control_lease(*installRoot, a_assertContext);
        if (!controlLease)
        {
            return Result<WindowsInstalledVersionExecutionLease>::failure(std::move(*controlLease.try_error()));
        }
        const std::filesystem::path journalsRoot = *installRoot / L"Operations" / L"Journals";
        if (!is_plain_directory(journalsRoot))
        {
            return Result<WindowsInstalledVersionExecutionLease>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidInstallState, "Install Journal directory is unavailable"));
        }
        std::error_code journalError;
        const std::filesystem::directory_iterator journal(extended_filesystem_path(journalsRoot), journalError);
        if (journalError || journal != std::filesystem::directory_iterator{})
        {
            return Result<WindowsInstalledVersionExecutionLease>::failure(
                make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                        "An incomplete Install operation blocks Version launch"));
        }
        auto snapshot = read_installed_version(*installRoot, a_request.versionDirectory, false, a_assertContext);
        if (!snapshot || snapshot.try_value()->registry.selectedVersion != a_request.versionDirectory)
        {
            return Result<WindowsInstalledVersionExecutionLease>::failure(
                snapshot ? make_distribution_error(a_assertContext, DistributionError::InvalidInstallState,
                                                   "Requested Version is not the selected Installed Version")
                         : std::move(*snapshot.try_error()));
        }
        const InstalledVersionSnapshot expected = *snapshot.try_value();
        auto executionLease = acquire_shared_execution_lease(*installRoot, a_request.versionDirectory, a_assertContext);
        if (!executionLease)
        {
            return Result<WindowsInstalledVersionExecutionLease>::failure(std::move(*executionLease.try_error()));
        }
        auto revalidated = read_installed_version(*installRoot, a_request.versionDirectory, false, a_assertContext);
        if (!revalidated || revalidated.try_value()->registry != expected.registry ||
            revalidated.try_value()->entry != expected.entry ||
            revalidated.try_value()->registry.selectedVersion != a_request.versionDirectory)
        {
            return Result<WindowsInstalledVersionExecutionLease>::failure(
                revalidated ? make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                                      "Installed Version changed while acquiring its Execution Lease")
                            : std::move(*revalidated.try_error()));
        }
        auto encodedRoot = to_utf8(installRoot->native());
        if (!encodedRoot)
        {
            return Result<WindowsInstalledVersionExecutionLease>::failure(
                make_distribution_error(a_assertContext, DistributionError::PlatformOperationFailed,
                                        "Install Root could not be encoded for its Execution Lease"));
        }
        WindowsInstalledVersionExecutionLease lease(
            reinterpret_cast<std::uintptr_t>(executionLease.try_value()->release()), std::move(*encodedRoot),
            a_request.versionDirectory);
        return Result<WindowsInstalledVersionExecutionLease>::success(std::move(lease));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<WindowsUninstallOutcome> uninstall_windows_installed_version(const WindowsInstalledVersionRequest &a_request,
                                                                    const AssertContext &a_assertContext) noexcept
{
    try
    {
        auto installRoot = normalize_absolute(a_request.installRoot);
        if (!installRoot || !is_local_fixed_path(*installRoot) || !is_plain_directory(*installRoot) ||
            !has_plain_existing_ancestry(*installRoot) || !is_version_directory(a_request.versionDirectory))
        {
            return Result<WindowsUninstallOutcome>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidInstallState, "Uninstall request paths are invalid"));
        }
        auto controlLease = acquire_control_lease(*installRoot, a_assertContext);
        if (!controlLease)
        {
            return Result<WindowsUninstallOutcome>::failure(std::move(*controlLease.try_error()));
        }
        auto directories = ensure_managed_directories(*installRoot, a_assertContext);
        if (!directories)
        {
            return Result<WindowsUninstallOutcome>::failure(std::move(*directories.try_error()));
        }
        auto resumable = find_uninstall_journal(*installRoot, a_request.versionDirectory, a_assertContext);
        if (!resumable)
        {
            return Result<WindowsUninstallOutcome>::failure(std::move(*resumable.try_error()));
        }
        InstallOperationJournal journal;
        if (resumable.try_value()->has_value())
        {
            journal = **resumable.try_value();
            const std::filesystem::path versionRoot =
                *installRoot / L"Versions" / to_wide(a_request.versionDirectory).value_or(L"");
            const std::filesystem::path quarantine = uninstall_quarantine_path(*installRoot, journal);
            const bool hasVersion = is_plain_directory(versionRoot);
            const bool hasQuarantine = is_plain_directory(quarantine);
            if (hasVersion || hasQuarantine)
            {
                if (hasVersion && hasQuarantine)
                {
                    return Result<WindowsUninstallOutcome>::failure(
                        make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                                "Uninstall Version and Quarantine state is ambiguous"));
                }
                const std::filesystem::path &payloadRoot = hasVersion ? versionRoot : quarantine;
                auto evidence = validate_uninstall_payload(*installRoot, payloadRoot, journal, a_assertContext);
                if (!evidence)
                {
                    return Result<WindowsUninstallOutcome>::failure(std::move(*evidence.try_error()));
                }
            }
            else if (journal.stage != InstallOperationStage::RegistryEntryRemoved)
            {
                return Result<WindowsUninstallOutcome>::failure(
                    make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                            "Uninstall Payload is missing before its final cleanup Stage"));
            }
            auto registry = ensure_registry(*installRoot, journal.workerId, a_assertContext);
            if (!registry)
            {
                return Result<WindowsUninstallOutcome>::failure(std::move(*registry.try_error()));
            }
            auto reconciled = find_uninstall_journal(*installRoot, a_request.versionDirectory, a_assertContext);
            if (!reconciled || !reconciled.try_value()->has_value())
            {
                return Result<WindowsUninstallOutcome>::failure(
                    reconciled ? make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                                         "Uninstall Journal disappeared during Registry recovery")
                               : std::move(*reconciled.try_error()));
            }
            journal = std::move(**reconciled.try_value());
        }
        else
        {
            const std::filesystem::path versionRoot =
                *installRoot / L"Versions" / to_wide(a_request.versionDirectory).value_or(L"");
            auto versionBundle = validate_bundle(versionRoot, true, a_assertContext);
            if (!versionBundle)
            {
                return Result<WindowsUninstallOutcome>::failure(std::move(*versionBundle.try_error()));
            }
            auto registry = ensure_registry(*installRoot, versionBundle.try_value()->workerId, a_assertContext);
            if (!registry)
            {
                return Result<WindowsUninstallOutcome>::failure(std::move(*registry.try_error()));
            }
            auto snapshot = read_installed_version(*installRoot, a_request.versionDirectory, false, a_assertContext);
            if (!snapshot)
            {
                return Result<WindowsUninstallOutcome>::failure(std::move(*snapshot.try_error()));
            }
            const InstalledVersionEntry entry = snapshot.try_value()->entry;
            const std::size_t selectableCount = static_cast<std::size_t>(std::ranges::count_if(
                snapshot.try_value()->registry.versions, [](const InstalledVersionEntry &a_version)
                { return a_version.state == InstalledVersionState::Selectable; }));
            if (selectableCount <= 1U)
            {
                return Result<WindowsUninstallOutcome>::failure(
                    make_distribution_error(a_assertContext, DistributionError::InstalledVersionProtected,
                                            "The last selectable Installed Version cannot be removed"));
            }
            auto operationId = make_uuid();
            if (!operationId)
            {
                return Result<WindowsUninstallOutcome>::failure(
                    make_distribution_error(a_assertContext, DistributionError::PlatformOperationFailed,
                                            "Uninstall operation ID could not be generated"));
            }
            journal.operationId = std::move(*operationId);
            journal.kind = InstallOperationKind::Uninstall;
            journal.stage = InstallOperationStage::Prepared;
            journal.workerId = entry.workerId;
            journal.expectedRegistry =
                ExpectedRegistry{snapshot.try_value()->registry.generationId, snapshot.try_value()->registry.revision};
            journal.target = InstallOperationTarget{entry.directoryName, entry.bundleId, entry.manifestDigest};
            auto written = write_journal(*installRoot, journal, a_assertContext);
            if (!written)
            {
                return Result<WindowsUninstallOutcome>::failure(std::move(*written.try_error()));
            }
        }
        auto worker = validate_worker_for_journal(*installRoot, journal, a_assertContext);
        if (!worker)
        {
            return Result<WindowsUninstallOutcome>::failure(std::move(*worker.try_error()));
        }
        auto leaseFile = ensure_execution_lease_file(*installRoot, a_request.versionDirectory, a_assertContext);
        if (!leaseFile)
        {
            return Result<WindowsUninstallOutcome>::failure(std::move(*leaseFile.try_error()));
        }
        auto processId = launch_uninstall_worker(*installRoot, journal, *controlLease.try_value(), a_assertContext);
        if (!processId)
        {
            return Result<WindowsUninstallOutcome>::failure(std::move(*processId.try_error()));
        }
        WindowsUninstallOutcome outcome{journal.operationId, a_request.versionDirectory, journal.workerId,
                                        *processId.try_value()};
        return Result<WindowsUninstallOutcome>::success(std::move(outcome));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<void> run_windows_uninstall_worker(const WindowsUninstallWorkerRequest &a_request,
                                          const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_request.gateHandle == 0U || !is_canonical_bundle_id(a_request.operationId) ||
            !is_canonical_sha256(a_request.workerId))
        {
            return Result<void>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidInstallState, "Uninstall Worker request is invalid"));
        }
        HandleOwner gate(reinterpret_cast<HANDLE>(a_request.gateHandle));
        DWORD gateFlags = 0U;
        if (GetHandleInformation(gate.get(), &gateFlags) == FALSE || (gateFlags & HANDLE_FLAG_INHERIT) == 0U ||
            WaitForSingleObject(gate.get(), k_workerGateTimeoutMilliseconds) != WAIT_OBJECT_0 ||
            SetHandleInformation(gate.get(), HANDLE_FLAG_INHERIT, 0U) == FALSE)
        {
            return Result<void>::failure(
                make_distribution_error(a_assertContext, DistributionError::InvalidInstallState,
                                        "Uninstall Worker release gate is invalid or was not signaled"));
        }
        auto installRoot = normalize_absolute(a_request.installRoot);
        if (!installRoot || !is_local_fixed_path(*installRoot) || !is_plain_directory(*installRoot) ||
            !has_plain_existing_ancestry(*installRoot))
        {
            return Result<void>::failure(make_distribution_error(
                a_assertContext, DistributionError::InvalidInstallState, "Uninstall Worker Install Root is invalid"));
        }
        auto controlLease = acquire_control_lease(*installRoot, a_assertContext);
        if (!controlLease)
        {
            return Result<void>::failure(std::move(*controlLease.try_error()));
        }
        auto directories = ensure_managed_directories(*installRoot, a_assertContext);
        if (!directories)
        {
            return Result<void>::failure(std::move(*directories.try_error()));
        }
        auto journal =
            read_worker_uninstall_journal(*installRoot, a_request.operationId, a_request.workerId, a_assertContext);
        if (!journal)
        {
            return Result<void>::failure(std::move(*journal.try_error()));
        }
        auto registry = ensure_registry(*installRoot, a_request.workerId, a_assertContext);
        if (!registry)
        {
            return Result<void>::failure(std::move(*registry.try_error()));
        }
        journal =
            read_worker_uninstall_journal(*installRoot, a_request.operationId, a_request.workerId, a_assertContext);
        if (!journal)
        {
            return Result<void>::failure(std::move(*journal.try_error()));
        }
        const std::filesystem::path versionRoot =
            *installRoot / L"Versions" / to_wide(journal.try_value()->target->directoryName).value_or(L"");
        const std::filesystem::path quarantine = uninstall_quarantine_path(*installRoot, *journal.try_value());
        const bool hasVersion = is_plain_directory(versionRoot);
        const bool hasQuarantine = is_plain_directory(quarantine);
        if (hasVersion || hasQuarantine)
        {
            if (hasVersion && hasQuarantine)
            {
                return Result<void>::failure(
                    make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                            "Uninstall Worker found ambiguous Version and Quarantine state"));
            }
            const std::filesystem::path &payloadRoot = hasVersion ? versionRoot : quarantine;
            auto evidence =
                validate_uninstall_payload(*installRoot, payloadRoot, *journal.try_value(), a_assertContext);
            if (!evidence)
            {
                return Result<void>::failure(std::move(*evidence.try_error()));
            }
        }
        else if (journal.try_value()->stage != InstallOperationStage::RegistryEntryRemoved)
        {
            return Result<void>::failure(
                make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                        "Uninstall Worker Payload is missing before its final cleanup Stage"));
        }
        auto worker = validate_worker_for_journal(*installRoot, *journal.try_value(), a_assertContext);
        if (!worker)
        {
            return Result<void>::failure(std::move(*worker.try_error()));
        }
        const std::filesystem::path expectedWorker = *installRoot / L"Operations" / L"Workers" /
                                                     to_wide(a_request.workerId).value_or(L"") /
                                                     L"CueEngineInstallWorker.exe";
        if (!is_current_worker_executable(expectedWorker))
        {
            return Result<void>::failure(
                make_distribution_error(a_assertContext, DistributionError::InstallConflict,
                                        "Uninstall Worker is not running from its verified external path"));
        }
        auto executionLease = acquire_exclusive_execution_lease(
            *installRoot, journal.try_value()->target->directoryName, a_assertContext);
        if (!executionLease)
        {
            return Result<void>::failure(std::move(*executionLease.try_error()));
        }
        const std::string directoryName = journal.try_value()->target->directoryName;
        auto uninstalled = execute_uninstall(*installRoot, std::move(*journal.try_value()), a_assertContext);
        if (!uninstalled)
        {
            return uninstalled;
        }
        executionLease.try_value()->reset();
        static_cast<void>(DeleteFileW(win32_path(execution_lease_path(*installRoot, directoryName)).c_str()));
        return Result<void>::success();
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}
} // namespace cue::distribution
