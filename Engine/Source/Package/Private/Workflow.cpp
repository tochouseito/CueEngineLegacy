#include <Cue/Package/Workflow.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Package/Error.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace
{
constexpr std::size_t k_maximumRuntimeOutputBytes = 4U * 1024U * 1024U;

enum class WorkflowError : std::int64_t
{
    MissingDependency = 1,
    InvalidInput,
    OperationAlreadyRunning,
    NoRetryableOperation,
    NoPublishedPackage,
    NoActiveOperation,
    OwnerThreadViolation,
    ArtifactMismatch,
    PackagePublicationFailed,
    RuntimeProcessFailed
};

/// @brief 回復不能なWorkflow内部例外をFatalHandlerへ通知してProcessを停止する
[[noreturn]] void terminate_workflow_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Game package workflow failed unexpectedly");
    std::abort();
}

/// @brief Workflow固有の回復可能Errorを一貫したDomainで構築する
[[nodiscard]] cue::Error make_workflow_error(const cue::AssertContext &a_assertContext, WorkflowError a_code,
                                             std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Package.Workflow",
                                                 static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Run前検証からProcess終了までPackage TreeのWrite／Deleteを拒否する所有Guard
class PackageRunGuard final
{
#if defined(_WIN32)
    /// @brief 非同期Directory変更通知のBufferとNative Handleを一体所有する
    struct DirectoryChangeMonitor final
    {
        /// @brief 未開始のDirectory変更監視を構築する
        DirectoryChangeMonitor() noexcept = default;
        DirectoryChangeMonitor(const DirectoryChangeMonitor &) = delete;
        DirectoryChangeMonitor &operator=(const DirectoryChangeMonitor &) = delete;

        /// @brief Pending通知と監視Threadを停止してNative Handleを閉じる
        ~DirectoryChangeMonitor() noexcept
        {
            stop_and_wait();
            if (directory != INVALID_HANDLE_VALUE)
            {
                CloseHandle(directory);
            }
            if (event != nullptr)
            {
                CloseHandle(event);
            }
        }

        /// @brief 変更または監視異常を即時Process Cancellationへ接続するThreadを開始する
        [[nodiscard]] bool arm(std::shared_ptr<cue::ChildProcessCancellation> a_cancellation) noexcept
        {
            if (!pending.load(std::memory_order_acquire) || watcher.joinable())
            {
                return false;
            }
            try
            {
                watcher = std::thread(
                    [this, cancellation = std::move(a_cancellation)]()
                    {
                        const DWORD waitResult = WaitForSingleObject(event, INFINITE);
                        DWORD transferred = 0U;
                        const BOOL completed = waitResult == WAIT_OBJECT_0
                                                   ? GetOverlappedResult(directory, &overlapped, &transferred, FALSE)
                                                   : FALSE;
                        const DWORD error = completed == FALSE ? GetLastError() : ERROR_SUCCESS;
                        pending.store(false, std::memory_order_release);
                        const bool expectedShutdownCancellation = completed == FALSE &&
                                                                  error == ERROR_OPERATION_ABORTED &&
                                                                  shutdownRequested.load(std::memory_order_acquire);
                        if (!expectedShutdownCancellation)
                        {
                            changed.store(true, std::memory_order_release);
                            cancellation->request_cancel();
                        }
                    });
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        /// @brief Pending通知を取消し監視Threadの完了を待つ
        void stop_and_wait() noexcept
        {
            shutdownRequested.store(true, std::memory_order_release);
            if (directory != INVALID_HANDLE_VALUE && pending.load(std::memory_order_acquire))
            {
                static_cast<void>(CancelIoEx(directory, &overlapped));
            }
            if (watcher.joinable())
            {
                watcher.join();
            }
            else if (directory != INVALID_HANDLE_VALUE && pending.exchange(false, std::memory_order_acq_rel))
            {
                DWORD transferred = 0U;
                static_cast<void>(GetOverlappedResult(directory, &overlapped, &transferred, TRUE));
            }
        }

        HANDLE directory = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED overlapped{};
        alignas(DWORD) std::array<std::byte, 4096U> buffer{};
        std::atomic<bool> pending = false;
        std::atomic<bool> changed = false;
        std::atomic<bool> shutdownRequested = false;
        std::thread watcher;
    };
#endif

  public:
    /// @brief 無効Guardを構築する
    PackageRunGuard() noexcept = default;
    /// @brief Guardの共有所有を禁止する
    PackageRunGuard(const PackageRunGuard &) = delete;
    /// @brief Guardの共有所有を禁止する
    PackageRunGuard &operator=(const PackageRunGuard &) = delete;
    /// @brief 全Native Handleの所有権を移動する
    PackageRunGuard(PackageRunGuard &&a_other) noexcept
#if defined(_WIN32)
        : m_directoryChangeMonitor(std::move(a_other.m_directoryChangeMonitor)), m_handles(std::move(a_other.m_handles))
#else
        : m_handles(std::move(a_other.m_handles))
#endif
    {
        a_other.m_handles.clear();
    }
    /// @brief 既存Guardを解放して全Native Handleの所有権を移動する
    PackageRunGuard &operator=(PackageRunGuard &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset();
#if defined(_WIN32)
            m_directoryChangeMonitor = std::move(a_other.m_directoryChangeMonitor);
#endif
            m_handles = std::move(a_other.m_handles);
            a_other.m_handles.clear();
        }
        return *this;
    }
    /// @brief Package Treeの置換Guardを解放する
    ~PackageRunGuard() noexcept
    {
        reset();
    }

#if defined(_WIN32)
    /// @brief Package Root配下の名前またはMetadata変更を一回検知する監視を開始する
    [[nodiscard]] bool start_directory_change_monitor(const std::filesystem::path &a_root)
    {
        std::unique_ptr<DirectoryChangeMonitor> monitor = std::make_unique<DirectoryChangeMonitor>();
        monitor->directory =
            CreateFileW(a_root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED, nullptr);
        if (monitor->directory == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        monitor->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (monitor->event == nullptr)
        {
            return false;
        }
        monitor->overlapped.hEvent = monitor->event;
        constexpr DWORD changes = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                  FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                                  FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
                                  FILE_NOTIFY_CHANGE_SECURITY;
        if (ReadDirectoryChangesW(monitor->directory, monitor->buffer.data(),
                                  static_cast<DWORD>(monitor->buffer.size()), TRUE, changes, nullptr,
                                  &monitor->overlapped, nullptr) == FALSE)
        {
            return false;
        }
        monitor->pending.store(true, std::memory_order_release);
        m_directoryChangeMonitor = std::move(monitor);
        return true;
    }

    /// @brief Package Tree変更を実行中Processの即時Cancellationへ接続する
    [[nodiscard]] bool arm_directory_change_cancellation(
        std::shared_ptr<cue::ChildProcessCancellation> a_cancellation) noexcept
    {
        return m_directoryChangeMonitor && m_directoryChangeMonitor->arm(std::move(a_cancellation));
    }

    /// @brief 検証対象Native Handleの所有権を追加する
    void add_handle(HANDLE a_handle)
    {
        m_handles.push_back(a_handle);
    }
#else
    /// @brief 非WindowsではNative変更監視が不要なため実行継続を許可する
    [[nodiscard]] bool arm_directory_change_cancellation(
        std::shared_ptr<cue::ChildProcessCancellation> a_cancellation) noexcept
    {
        static_cast<void>(a_cancellation);
        return true;
    }
#endif

    /// @brief 監視を停止しPackage Tree変更または監視異常が発生したかFail-closedで返す
    [[nodiscard]] bool finish_and_has_directory_change() noexcept
    {
#if defined(_WIN32)
        if (!m_directoryChangeMonitor)
        {
            return true;
        }
        m_directoryChangeMonitor->stop_and_wait();
        return m_directoryChangeMonitor->changed.load(std::memory_order_acquire);
#else
        return false;
#endif
    }

  private:
    /// @brief 所有中の全Native Handleを逆順に閉じる
    void reset() noexcept
    {
#if defined(_WIN32)
        m_directoryChangeMonitor.reset();
        for (auto handle = m_handles.rbegin(); handle != m_handles.rend(); ++handle)
        {
            if (*handle != nullptr && *handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(*handle);
            }
        }
        m_handles.clear();
#endif
    }

#if defined(_WIN32)
    std::unique_ptr<DirectoryChangeMonitor> m_directoryChangeMonitor;
    std::vector<HANDLE> m_handles;
#else
    std::vector<std::byte> m_handles;
#endif
};

/// @brief UTF-8 PathをWindows filesystem Pathへ変換する
[[nodiscard]] std::filesystem::path native_path(std::string_view a_path)
{
#if defined(_WIN32)
    std::u8string pathBytes;
    pathBytes.reserve(a_path.size());
    for (const char byte : a_path)
    {
        pathBytes.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path(std::move(pathBytes));
#else
    return std::filesystem::path(a_path);
#endif
}

#if defined(_WIN32)
/// @brief Absolute Windows PathをProcess Manifest非依存のExtended-length形式へ変換する
[[nodiscard]] std::filesystem::path extended_native_path(const std::filesystem::path &a_path)
{
    std::wstring native = a_path.native();
    std::replace(native.begin(), native.end(), L'/', L'\\');
    if (native.starts_with(L"\\\\?\\"))
    {
        return std::filesystem::path(std::move(native));
    }
    if (native.starts_with(L"\\\\"))
    {
        return std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2U));
    }
    return std::filesystem::path(L"\\\\?\\" + native);
}

/// @brief Absolute Directory Chainを非Reparse Handleで固定する
[[nodiscard]] bool lock_directory_chain(const std::filesystem::path &a_directory, PackageRunGuard &a_guard,
                                        std::vector<std::wstring> &a_lockedDirectories)
{
    if (!a_directory.is_absolute())
    {
        return false;
    }
    std::filesystem::path current = a_directory.root_path();
    for (const std::filesystem::path &segment : a_directory.relative_path())
    {
        current /= segment;
        const std::wstring currentText = current.native();
        const bool alreadyLocked = std::ranges::any_of(
            a_lockedDirectories,
            [&currentText](const std::wstring &a_locked)
            {
                return a_locked.size() == currentText.size() &&
                       CompareStringOrdinal(a_locked.data(), static_cast<int>(a_locked.size()), currentText.data(),
                                            static_cast<int>(currentText.size()), TRUE) == CSTR_EQUAL;
            });
        if (alreadyLocked)
        {
            continue;
        }
        HANDLE handle =
            CreateFileW(extended_native_path(current).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        {
            CloseHandle(handle);
            return false;
        }
        a_guard.add_handle(handle);
        a_lockedDirectories.push_back(currentText);
    }
    return !a_lockedDirectories.empty();
}

/// @brief Package Fileを非Reparse Handleで固定する
[[nodiscard]] bool lock_regular_file(const std::filesystem::path &a_path, PackageRunGuard &a_guard)
{
    HANDLE handle =
        CreateFileW(extended_native_path(a_path).c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U || GetFileType(handle) != FILE_TYPE_DISK)
    {
        CloseHandle(handle);
        return false;
    }
    a_guard.add_handle(handle);
    return true;
}
#endif

/// @brief Package Root、Manifest、全Payloadを固定しWrite／Delete共有を拒否する
[[nodiscard]] cue::Result<PackageRunGuard> acquire_package_run_guard(std::string_view a_packageRoot,
                                                                     const cue::package::PackageManifest &a_manifest,
                                                                     const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        PackageRunGuard guard;
#if defined(_WIN32)
        const std::filesystem::path root = native_path(a_packageRoot);
        std::vector<std::wstring> lockedDirectories;
        if (!lock_directory_chain(root, guard, lockedDirectories) ||
            !guard.start_directory_change_monitor(extended_native_path(root)) ||
            !lock_regular_file(root / L"CuePackage.json", guard))
        {
            return cue::Result<PackageRunGuard>::failure(
                make_workflow_error(a_assertContext, WorkflowError::ArtifactMismatch,
                                    "Published Package root or manifest could not be locked for validated execution"));
        }
        for (const cue::package::PackageFileEntry &entry : a_manifest.files())
        {
            const std::filesystem::path relative = native_path(entry.relative_path());
            if (!lock_directory_chain(root / relative.parent_path(), guard, lockedDirectories) ||
                !lock_regular_file(root / relative, guard))
            {
                return cue::Result<PackageRunGuard>::failure(
                    make_workflow_error(a_assertContext, WorkflowError::ArtifactMismatch,
                                        "Published Package file could not be locked for validated execution"));
            }
        }
#else
        static_cast<void>(a_packageRoot);
        static_cast<void>(a_manifest);
#endif
        return cue::Result<PackageRunGuard>::success(std::move(guard));
    }
    catch (...)
    {
        terminate_workflow_exception(a_assertContext);
    }
}

/// @brief Build ConfigurationをCMakeおよびPackage Pathと同じ固定名へ変換する
[[nodiscard]] std::string_view configuration_name(cue::BuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case cue::BuildConfiguration::Debug:
        return "Debug";
    case cue::BuildConfiguration::Development:
        return "Development";
    case cue::BuildConfiguration::Release:
        return "Release";
    }
    return {};
}

/// @brief ASCII文字列を同一Byte列へ変換して所有する
[[nodiscard]] std::vector<std::byte> copy_bytes(std::string_view a_text)
{
    const std::span<const char> characters(a_text.data(), a_text.size());
    const std::span<const std::byte> raw = std::as_bytes(characters);
    return std::vector<std::byte>(raw.begin(), raw.end());
}

/// @brief 二つのRoot相対Path要素をslash一個で連結する
[[nodiscard]] std::string join_relative(std::string_view a_left, std::string_view a_right)
{
    std::string path(a_left);
    if (!path.empty() && path.back() != '/')
    {
        path.push_back('/');
    }
    path.append(a_right);
    return path;
}

/// @brief UTF-8 Absolute Rootと正規化済みRelative Pathを表示・Process用Locatorへ連結する
[[nodiscard]] std::string join_absolute(std::string_view a_root, std::string_view a_relative)
{
    std::string path(a_root);
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
    {
        path.push_back('/');
    }
    path.append(a_relative);
    return path;
}

/// @brief Package相対Pathから最後のFile Nameを借用する
[[nodiscard]] std::string_view package_file_name(std::string_view a_relativePath) noexcept
{
    const std::size_t separator = a_relativePath.find_last_of('/');
    return separator == std::string_view::npos ? a_relativePath : a_relativePath.substr(separator + 1U);
}

/// @brief 同じProject Rootから導出されたAbsolute LocatorをRoot相対Locatorへ変換する
[[nodiscard]] std::optional<std::string> make_project_relative(std::string_view a_projectRoot,
                                                               std::string_view a_absoluteLocator)
{
    std::string root(a_projectRoot);
    std::string locator(a_absoluteLocator);
    std::replace(root.begin(), root.end(), '\\', '/');
    std::replace(locator.begin(), locator.end(), '\\', '/');
    while (root.size() > 3U && root.back() == '/')
    {
        root.pop_back();
    }
    root.push_back('/');
    if (!locator.starts_with(root) || locator.size() == root.size())
    {
        return std::nullopt;
    }
    return locator.substr(root.size());
}

/// @brief Package File InventoryがRole、Path、Size、Hashまで完全一致するか判定する
[[nodiscard]] bool package_files_match(std::span<const cue::package::PackageFileEntry> a_expected,
                                       std::span<const cue::package::PackageFileEntry> a_actual) noexcept
{
    return a_expected.size() == a_actual.size() &&
           std::equal(
               a_expected.begin(), a_expected.end(), a_actual.begin(),
               [](const cue::package::PackageFileEntry &a_left, const cue::package::PackageFileEntry &a_right) noexcept
               {
                   return a_left.role() == a_right.role() && a_left.relative_path() == a_right.relative_path() &&
                          a_left.byte_size() == a_right.byte_size() && a_left.sha256() == a_right.sha256();
               });
}

/// @brief 再読込Manifestが公開完了時Snapshotと全Identity・Inventoryで一致するか判定する
[[nodiscard]] bool package_manifest_matches_summary(const cue::package::PackageManifest &a_manifest,
                                                    const cue::package::PackageManifestSummary &a_expected) noexcept
{
    return a_manifest.project_id() == a_expected.projectId && a_manifest.engine_version() == a_expected.engineVersion &&
           a_manifest.configuration() == a_expected.configuration &&
           a_manifest.execution_model() == a_expected.executionModel &&
           a_manifest.startup_scene_asset_id() == a_expected.startupSceneAssetId &&
           a_manifest.application_executable() == a_expected.applicationExecutable &&
           a_manifest.trust_mode() == a_expected.trustMode &&
           a_manifest.publisher_key_id() == a_expected.publisherKeyId &&
           a_manifest.files().size() == a_expected.fileCount &&
           package_files_match(a_expected.files, a_manifest.files());
}

/// @brief 日本語の失敗段階とError Domain、Code、SummaryをUI向け一行Messageへ平坦化する
[[nodiscard]] std::string error_message(std::string_view a_context, const cue::Error &a_error)
{
    std::string message(a_context);
    message.append(": ");
    message.append(a_error.root_code().domain());
    message.push_back('/');
    message.append(std::to_string(a_error.root_code().value()));
    message.push_back(' ');
    message.append(a_error.summary());
    return message;
}

/// @brief Build失敗状態をPackage Workflow終端状態へ変換する
[[nodiscard]] cue::package::PackageWorkflowState build_terminal_state(cue::GameBuildOperationState a_state) noexcept
{
    return a_state == cue::GameBuildOperationState::Cancelled ? cue::package::PackageWorkflowState::Cancelled
                                                              : cue::package::PackageWorkflowState::Failed;
}

/// @brief Package化対象PEの宣言Sizeを全体読込前にRuntimeHostと同じ上限で検証する
[[nodiscard]] cue::Result<void> validate_runtime_pe_memory_contract(std::span<const cue::BuildArtifactFile> a_files,
                                                                    const cue::AssertContext &a_assertContext) noexcept
{
    std::uint64_t totalBytes = 0U;
    for (const cue::BuildArtifactFile &file : a_files)
    {
        if (file.relativePath == "CueGameModule.pdb" || file.relativePath == "CueGameModule.metadata.json" ||
            file.relativePath == "CueRuntimeHost.exe")
        {
            continue;
        }
        if (file.byteSize > cue::package::k_maximumRuntimePeImageBytes ||
            totalBytes > cue::package::k_maximumRuntimePeInventoryBytes - file.byteSize)
        {
            return cue::Result<void>::failure(cue::package::make_package_error(
                a_assertContext, cue::package::PackageError::PackageManifestResourceLimitExceeded,
                "Runtime PE image inventory exceeds the RuntimeHost memory contract"));
        }
        totalBytes += file.byteSize;
    }
    return cue::Result<void>::success();
}
} // namespace

namespace cue::package
{
struct GamePackageWorkflowService::Impl final
{
    struct PackageInputs final
    {
        EngineVersion engineVersion;
        std::string projectId;
        MinimalRuntimeDataPublication runtimeData;
    };

    /// @brief 検証済み依存とRoot Locatorの所有権をWorkflow実装へ移す
    Impl(std::unique_ptr<GameBuildService> a_buildService, std::unique_ptr<BuildArtifactReader> a_artifactReader,
         std::unique_ptr<FilesystemRoot> a_projectFilesystem, std::unique_ptr<FilesystemRoot> a_engineBinaryFilesystem,
         std::unique_ptr<ChildProcessRunner> a_runProcessRunner, std::string a_projectRoot,
         std::vector<ChildProcessEnvironmentEntry> a_runEnvironment,
         RuntimeHostBuildSource a_runtimeHostBuildSource, const AssertContext &a_assertContext) noexcept
        : buildService(std::move(a_buildService)), artifactReader(std::move(a_artifactReader)),
          projectFilesystem(std::move(a_projectFilesystem)),
          engineBinaryFilesystem(std::move(a_engineBinaryFilesystem)), runProcessRunner(std::move(a_runProcessRunner)),
          projectRoot(std::move(a_projectRoot)), runEnvironment(std::move(a_runEnvironment)),
          runtimeHostBuildSource(a_runtimeHostBuildSource), assertContext(&a_assertContext),
          ownerThread(std::this_thread::get_id())
    {
    }

    /// @brief 呼出ThreadがService生成Threadと一致するか返す
    [[nodiscard]] bool is_owner_thread() const noexcept
    {
        return std::this_thread::get_id() == ownerThread;
    }

    /// @brief 保持中のPackage Stagingを同じFilesystem InstanceでRollbackする
    [[nodiscard]] Result<void> retry_staging_recovery() noexcept
    {
        if (!recoveryStaging)
        {
            return Result<void>::success();
        }
        Result<void> rollback = projectFilesystem->rollback_staging_area(std::move(*recoveryStaging));
        if (rollback)
        {
            recoveryStaging.reset();
        }
        return rollback;
    }

    /// @brief 入力Rootから上限付きByte列を読みPackage Payloadへ変換する
    [[nodiscard]] Result<PackageFilePayload> read_payload(FilesystemRoot &a_filesystem, std::string a_sourcePath,
                                                          PackageFileRole a_role, std::string a_packagePath,
                                                          std::uint64_t a_maximumBytes) noexcept
    {
        Result<RelativePath> source = RelativePath::parse(a_sourcePath, *assertContext);
        if (!source || a_maximumBytes > std::numeric_limits<std::size_t>::max())
        {
            return Result<PackageFilePayload>::failure(
                source ? make_workflow_error(*assertContext, WorkflowError::InvalidInput,
                                             "Package input file exceeds the addressable size")
                       : std::move(*source.try_error()));
        }
        Result<std::vector<std::byte>> bytes =
            a_filesystem.read_file(*source.try_value(), static_cast<std::size_t>(a_maximumBytes));
        if (!bytes)
        {
            return Result<PackageFilePayload>::failure(std::move(*bytes.try_error()));
        }
        return PackageFilePayload::create(a_role, std::move(a_packagePath), std::move(*bytes.try_value()),
                                          *assertContext);
    }

    /// @brief Publisher ReportをWorkflow Snapshotまたは保持可能な失敗診断へ変換する
    [[nodiscard]] Result<PublishedRuntimePackageSnapshot> complete_publication(
        PackagePublishReport a_report, const BuildArtifactInventory &a_artifact, std::string_view a_operationId,
        std::string_view a_executableName, std::optional<PackagePublishDiagnosticSnapshot> &a_diagnostic) noexcept
    {
        if (!a_report.succeeded())
        {
            a_diagnostic = PackagePublishDiagnosticSnapshot{
                a_report.stage, a_report.outcome, std::move(a_report.destination), std::move(a_report.manifest)};
            Error primary = a_report.error
                                ? std::move(*a_report.error)
                                : make_workflow_error(*assertContext, WorkflowError::PackagePublicationFailed,
                                                      "Package publication failed without a diagnostic");
            if (a_report.recoveryStaging)
            {
                recoveryStaging = std::move(a_report.recoveryStaging);
                Result<void> rollback = retry_staging_recovery();
                if (!rollback)
                {
                    primary.append_secondary_diagnostics(*assertContext, *rollback.try_error(),
                                                         "Package staging recovery retry failed", "Rollback");
                }
            }
            return Result<PublishedRuntimePackageSnapshot>::failure(std::move(primary));
        }
        const std::string packageRoot = join_absolute(projectRoot, a_report.destination);
        return Result<PublishedRuntimePackageSnapshot>::success(
            {std::string(a_operationId), std::string(a_artifact.artifact_id()), std::move(a_report.destination),
             join_absolute(packageRoot, a_executableName), std::move(a_report.manifest)});
    }

    /// @brief SnapshotのPackageをManifestと完全Inventoryへ再照合し固定Executable名を返す
    [[nodiscard]] Result<PackageRunGuard> validate_package_for_run(
        const PublishedRuntimePackageSnapshot &a_package) noexcept
    {
        Result<RelativePath> manifestPath =
            RelativePath::parse(join_relative(a_package.destination, "CuePackage.json"), *assertContext);
        if (!manifestPath)
        {
            return Result<PackageRunGuard>::failure(std::move(*manifestPath.try_error()));
        }
        Result<std::vector<std::byte>> manifestBytes =
            projectFilesystem->read_file(*manifestPath.try_value(), k_maximumPackageManifestBytes);
        if (!manifestBytes)
        {
            return Result<PackageRunGuard>::failure(std::move(*manifestBytes.try_error()));
        }
        const std::string_view manifestText(reinterpret_cast<const char *>(manifestBytes.try_value()->data()),
                                            manifestBytes.try_value()->size());
        Result<PackageManifest> manifest = parse_package_manifest(manifestText, *assertContext);
        if (!manifest)
        {
            return Result<PackageRunGuard>::failure(std::move(*manifest.try_error()));
        }
        const PackageManifestSummary &expected = a_package.manifest;
        if (!package_manifest_matches_summary(*manifest.try_value(), expected))
        {
            return Result<PackageRunGuard>::failure(
                make_workflow_error(*assertContext, WorkflowError::ArtifactMismatch,
                                    "Published Package Manifest differs from the completed workflow snapshot"));
        }
        const std::string executableName = manifest.try_value()->execution_model() == PackageExecutionModel::Monolithic
                                               ? std::string(*manifest.try_value()->application_executable())
                                               : std::string("CueRuntimeHost.exe");
        const std::string packageRoot = join_absolute(projectRoot, a_package.destination);
        if (a_package.executable != join_absolute(packageRoot, executableName))
        {
            return Result<PackageRunGuard>::failure(
                make_workflow_error(*assertContext, WorkflowError::ArtifactMismatch,
                                    "Published Package executable is outside the validated Package root"));
        }
        Result<PackageRunGuard> packageGuard =
            acquire_package_run_guard(packageRoot, *manifest.try_value(), *assertContext);
        if (!packageGuard)
        {
            return packageGuard;
        }
        manifestBytes = projectFilesystem->read_file(*manifestPath.try_value(), k_maximumPackageManifestBytes);
        if (!manifestBytes)
        {
            return Result<PackageRunGuard>::failure(std::move(*manifestBytes.try_error()));
        }
        const std::string_view lockedManifestText(reinterpret_cast<const char *>(manifestBytes.try_value()->data()),
                                                  manifestBytes.try_value()->size());
        Result<PackageManifest> lockedManifest = parse_package_manifest(lockedManifestText, *assertContext);
        if (!lockedManifest)
        {
            return Result<PackageRunGuard>::failure(std::move(*lockedManifest.try_error()));
        }
        if (!package_manifest_matches_summary(*lockedManifest.try_value(), expected))
        {
            return Result<PackageRunGuard>::failure(
                make_workflow_error(*assertContext, WorkflowError::ArtifactMismatch,
                                    "Locked Package Manifest differs from the completed workflow snapshot"));
        }
        Result<void> verified = verify_package_manifest_files(packageRoot, *lockedManifest.try_value(), *assertContext);
        if (!verified)
        {
            return Result<PackageRunGuard>::failure(std::move(*verified.try_error()));
        }
        return packageGuard;
    }

    /// @brief Build ArtifactとRuntime Dataから不変Packageを一度だけ公開する
    [[nodiscard]] Result<PublishedRuntimePackageSnapshot> publish_package(
        const BuildArtifactInventory &a_artifact, const PackageInputs &a_inputs, std::string_view a_operationId,
        const PackageCancellation &a_cancellation,
        std::optional<PackagePublishDiagnosticSnapshot> &a_diagnostic) noexcept
    {
        try
        {
            const std::string_view configuration = configuration_name(a_artifact.configuration());
            if (configuration.empty())
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(make_workflow_error(
                    *assertContext, WorkflowError::InvalidInput, "Package Build Configuration is invalid"));
            }
            if (a_artifact.profile().target() == BuildTarget::ShippingProduct)
            {
                if (a_artifact.configuration() != BuildConfiguration::Release ||
                    a_inputs.projectId != a_inputs.runtimeData.project_id())
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(
                        make_workflow_error(*assertContext, WorkflowError::ArtifactMismatch,
                                            "Shipping Package requires Release and matching Project Runtime Data"));
                }
                const std::string packageParent = "Generated/Packages/Shipping/Release";
                Result<RelativePath> parent = RelativePath::parse(packageParent, *assertContext);
                Result<RelativePath> destination =
                    RelativePath::parse(join_relative(packageParent, a_operationId), *assertContext);
                if (!parent || !destination)
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(parent ? std::move(*destination.try_error())
                                                                                   : std::move(*parent.try_error()));
                }
                Result<void> directory = projectFilesystem->create_directories(*parent.try_value());
                if (!directory)
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*directory.try_error()));
                }
                Result<PackagePublishReport> published = publish_monolithic_runtime_package(
                    *projectFilesystem, projectRoot, *artifactReader, a_artifact, a_inputs.engineVersion,
                    a_inputs.runtimeData, *destination.try_value(), a_cancellation, *assertContext);
                if (!published)
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*published.try_error()));
                }
                return complete_publication(std::move(*published.try_value()), a_artifact, a_operationId,
                                            "CueGameProduct.exe", a_diagnostic);
            }
            Result<void> runtimePeMemoryContract =
                validate_runtime_pe_memory_contract(a_artifact.files(), *assertContext);
            if (!runtimePeMemoryContract)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(
                    std::move(*runtimePeMemoryContract.try_error()));
            }

            std::vector<PackageFilePayload> payloads;
            payloads.reserve(a_artifact.files().size() + 3U);
            const std::optional<std::string> artifactDirectory =
                make_project_relative(projectRoot, a_artifact.version_directory());
            if (!artifactDirectory)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(
                    make_workflow_error(*assertContext, WorkflowError::ArtifactMismatch,
                                        "Build artifact directory is outside the package project root"));
            }

            Result<std::optional<std::unique_ptr<BuildArtifactReadLease>>> acquired =
                artifactReader->acquire_current_read_lease(a_artifact, a_cancellation, std::nullopt);
            if (!acquired)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*acquired.try_error()));
            }
            if (!acquired.try_value()->has_value())
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(
                    make_workflow_error(*assertContext, WorkflowError::PackagePublicationFailed,
                                        "Package publication was cancelled while waiting for the artifact read lease"));
            }
            std::unique_ptr<BuildArtifactReadLease> artifactReadLease = std::move(**acquired.try_value());

            if (runtimeHostBuildSource == RuntimeHostBuildSource::PublishedBuildArtifact)
            {
                const auto runtimeHostFile =
                    std::find_if(a_artifact.files().begin(), a_artifact.files().end(),
                                 [](const BuildArtifactFile &a_file) noexcept
                                 { return a_file.relativePath == "CueRuntimeHost.exe"; });
                if (runtimeHostFile == a_artifact.files().end())
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(make_workflow_error(
                        *assertContext, WorkflowError::ArtifactMismatch,
                        "Published Build Artifact does not contain RuntimeHost"));
                }
                Result<PackageFilePayload> runtimeHost = read_payload(
                    *projectFilesystem, join_relative(*artifactDirectory, runtimeHostFile->relativePath),
                    PackageFileRole::RuntimeHost, "CueRuntimeHost.exe", k_maximumRuntimePeImageBytes);
                if (!runtimeHost)
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*runtimeHost.try_error()));
                }
                if (runtimeHost.try_value()->entry().byte_size() != runtimeHostFile->byteSize ||
                    runtimeHost.try_value()->entry().sha256() != runtimeHostFile->contentHash)
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(make_workflow_error(
                        *assertContext, WorkflowError::ArtifactMismatch,
                        "RuntimeHost bytes differ from the published inventory"));
                }
                payloads.push_back(std::move(*runtimeHost.try_value()));
            }
            else
            {
                Result<PackageFilePayload> runtimeHost = read_payload(
                    *engineBinaryFilesystem,
                    join_relative("bin", join_relative(configuration, "CueRuntimeHost.exe")),
                    PackageFileRole::RuntimeHost, "CueRuntimeHost.exe", k_maximumRuntimePeImageBytes);
                if (!runtimeHost)
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*runtimeHost.try_error()));
                }
                payloads.push_back(std::move(*runtimeHost.try_value()));
            }

            for (const BuildArtifactFile &file : a_artifact.files())
            {
                if (a_cancellation.is_cancel_requested())
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(make_workflow_error(
                        *assertContext, WorkflowError::PackagePublicationFailed, "Package publication was cancelled"));
                }
                if (file.relativePath == "CueGameModule.pdb" || file.relativePath == "CueRuntimeHost.exe")
                {
                    continue;
                }
                PackageFileRole role = PackageFileRole::RuntimeDependency;
                std::string packagePath = join_relative("Runtime", file.relativePath);
                if (file.relativePath == "CueGameModule.dll")
                {
                    role = PackageFileRole::GameModule;
                    packagePath = "Game/CueGameModule.dll";
                }
                else if (file.relativePath == "CueGameModule.metadata.json")
                {
                    role = PackageFileRole::GameModuleMetadata;
                    packagePath = "Game/CueGameModule.metadata.json";
                }
                Result<PackageFilePayload> payload =
                    read_payload(*projectFilesystem, join_relative(*artifactDirectory, file.relativePath), role,
                                 std::move(packagePath), file.byteSize);
                if (!payload)
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*payload.try_error()));
                }
                if (payload.try_value()->entry().byte_size() != file.byteSize ||
                    payload.try_value()->entry().sha256() != file.contentHash)
                {
                    return Result<PublishedRuntimePackageSnapshot>::failure(
                        make_workflow_error(*assertContext, WorkflowError::ArtifactMismatch,
                                            "Build artifact bytes differ from the published inventory"));
                }
                payloads.push_back(std::move(*payload.try_value()));
            }
            artifactReadLease.reset();

            const RuntimeDataFile &projectData = a_inputs.runtimeData.project_data();
            Result<PackageFilePayload> projectPayload = PackageFilePayload::create(
                PackageFileRole::ProjectRuntimeData, std::string(projectData.relative_path()),
                copy_bytes(projectData.bytes()), *assertContext);
            const RuntimeDataFile &sceneData = a_inputs.runtimeData.startup_scene_data();
            Result<PackageFilePayload> scenePayload = PackageFilePayload::create(
                PackageFileRole::StartupSceneRuntimeData, std::string(sceneData.relative_path()),
                copy_bytes(sceneData.bytes()), *assertContext);
            if (!projectPayload || !scenePayload)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(
                    projectPayload ? std::move(*scenePayload.try_error()) : std::move(*projectPayload.try_error()));
            }
            if (projectPayload.try_value()->entry().sha256() != projectData.sha256() ||
                scenePayload.try_value()->entry().sha256() != sceneData.sha256())
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(
                    make_workflow_error(*assertContext, WorkflowError::ArtifactMismatch,
                                        "Runtime data bytes differ from the publication snapshot"));
            }
            payloads.push_back(std::move(*projectPayload.try_value()));
            payloads.push_back(std::move(*scenePayload.try_value()));

            const PackageFilePayload *runtimeHostPayload = nullptr;
            const PackageFilePayload *gameModulePayload = nullptr;
            std::vector<RuntimePeImageView> dependencyImages;
            dependencyImages.reserve(payloads.size());
            for (const PackageFilePayload &payload : payloads)
            {
                switch (payload.entry().role())
                {
                case PackageFileRole::RuntimeHost:
                    runtimeHostPayload = &payload;
                    break;
                case PackageFileRole::GameModule:
                    gameModulePayload = &payload;
                    break;
                case PackageFileRole::RuntimeDependency:
                    dependencyImages.push_back({package_file_name(payload.entry().relative_path()), payload.bytes()});
                    break;
                case PackageFileRole::GameModuleMetadata:
                case PackageFileRole::ProjectRuntimeData:
                case PackageFileRole::StartupSceneRuntimeData:
                case PackageFileRole::ApplicationExecutable:
                    break;
                }
            }
            if (runtimeHostPayload == nullptr || gameModulePayload == nullptr)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(make_workflow_error(
                    *assertContext, WorkflowError::ArtifactMismatch, "Package PE dependency inputs are incomplete"));
            }
            Result<void> dependencyClosure = validate_runtime_dependency_closure(
                a_artifact.configuration(),
                {package_file_name(runtimeHostPayload->entry().relative_path()), runtimeHostPayload->bytes()},
                {package_file_name(gameModulePayload->entry().relative_path()), gameModulePayload->bytes()},
                dependencyImages, *assertContext);
            if (!dependencyClosure)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*dependencyClosure.try_error()));
            }

            std::vector<PackageFileEntry> entries;
            entries.reserve(payloads.size());
            for (const PackageFilePayload &payload : payloads)
            {
                entries.push_back(payload.entry());
            }
            Result<PackageManifest> manifest =
                PackageManifest::create(a_inputs.projectId, a_inputs.engineVersion, a_artifact.configuration(),
                                        std::string(a_inputs.runtimeData.startup_scene_asset_id()),
                                        std::string(a_inputs.runtimeData.startup_scene_data().relative_path()),
                                        std::move(entries), *assertContext);
            if (!manifest)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*manifest.try_error()));
            }

            const std::string packageParent = join_relative("Generated/Packages", configuration);
            Result<RelativePath> parent = RelativePath::parse(packageParent, *assertContext);
            Result<RelativePath> destination =
                RelativePath::parse(join_relative(packageParent, a_operationId), *assertContext);
            if (!parent || !destination)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(parent ? std::move(*destination.try_error())
                                                                               : std::move(*parent.try_error()));
            }
            Result<void> directory = projectFilesystem->create_directories(*parent.try_value());
            if (!directory)
            {
                return Result<PublishedRuntimePackageSnapshot>::failure(std::move(*directory.try_error()));
            }
            PackagePublishReport report =
                publish_runtime_package(*projectFilesystem, *destination.try_value(), *manifest.try_value(), payloads,
                                        a_cancellation, *assertContext);
            return complete_publication(std::move(report), a_artifact, a_operationId, "CueRuntimeHost.exe",
                                        a_diagnostic);
        }
        catch (...)
        {
            terminate_workflow_exception(*assertContext);
        }
    }

    std::unique_ptr<GameBuildService> buildService;
    std::unique_ptr<BuildArtifactReader> artifactReader;
    std::unique_ptr<FilesystemRoot> projectFilesystem;
    std::unique_ptr<FilesystemRoot> engineBinaryFilesystem;
    std::unique_ptr<ChildProcessRunner> runProcessRunner;
    std::string projectRoot;
    std::vector<ChildProcessEnvironmentEntry> runEnvironment;
    RuntimeHostBuildSource runtimeHostBuildSource = RuntimeHostBuildSource::EngineBinaryRoot;
    const AssertContext *assertContext;
    std::thread::id ownerThread;
    mutable std::mutex mutex;
    std::thread worker;
    std::shared_ptr<PackageCancellation> packageCancellation;
    std::shared_ptr<ChildProcessCancellation> processCancellation;
    bool isCancellationRequested = false;
    std::optional<StagingArea> recoveryStaging;
    std::optional<PackageInputs> pendingInputs;
    std::optional<PackageInputs> retryInputs;
    PackageWorkflowSnapshot current;
};

GamePackageWorkflowService::GamePackageWorkflowService(std::unique_ptr<Impl> a_impl) noexcept
    : m_impl(std::move(a_impl))
{
}

GamePackageWorkflowService::~GamePackageWorkflowService()
{
    PackageWorkflowState state = PackageWorkflowState::Idle;
    {
        std::scoped_lock lock(m_impl->mutex);
        state = m_impl->current.state;
        if (m_impl->packageCancellation)
        {
            m_impl->packageCancellation->request_cancel();
        }
        if (m_impl->processCancellation)
        {
            m_impl->processCancellation->request_cancel();
        }
    }
    if (state == PackageWorkflowState::Building)
    {
        static_cast<void>(m_impl->buildService->request_cancel());
        static_cast<void>(m_impl->buildService->wait_for_completion());
    }
    if (m_impl->worker.joinable())
    {
        m_impl->worker.join();
    }
    static_cast<void>(m_impl->retry_staging_recovery());
}

Result<std::unique_ptr<GamePackageWorkflowService>> GamePackageWorkflowService::create(
    std::unique_ptr<GameBuildService> a_buildService, std::unique_ptr<BuildArtifactReader> a_artifactReader,
    std::unique_ptr<FilesystemRoot> a_projectFilesystem, std::unique_ptr<FilesystemRoot> a_engineBinaryFilesystem,
    std::unique_ptr<ChildProcessRunner> a_runProcessRunner, std::string a_projectRoot,
    std::vector<ChildProcessEnvironmentEntry> a_runEnvironment, RuntimeHostBuildSource a_runtimeHostBuildSource,
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!a_buildService || !a_artifactReader || !a_projectFilesystem || !a_engineBinaryFilesystem ||
            !a_runProcessRunner || a_projectRoot.empty() ||
            (a_runtimeHostBuildSource != RuntimeHostBuildSource::EngineBinaryRoot &&
             a_runtimeHostBuildSource != RuntimeHostBuildSource::PublishedBuildArtifact))
        {
            return Result<std::unique_ptr<GamePackageWorkflowService>>::failure(make_workflow_error(
                a_assertContext, WorkflowError::MissingDependency, "Package workflow dependency is missing"));
        }
        auto impl = std::make_unique<Impl>(std::move(a_buildService), std::move(a_artifactReader),
                                           std::move(a_projectFilesystem), std::move(a_engineBinaryFilesystem),
                                           std::move(a_runProcessRunner), std::move(a_projectRoot),
                                           std::move(a_runEnvironment), a_runtimeHostBuildSource, a_assertContext);
        return Result<std::unique_ptr<GamePackageWorkflowService>>::success(
            std::unique_ptr<GamePackageWorkflowService>(new GamePackageWorkflowService(std::move(impl))));
    }
    catch (...)
    {
        terminate_workflow_exception(a_assertContext);
    }
}

Result<void> GamePackageWorkflowService::start(BuildRequest a_buildRequest, CMakeConfigureMode a_configureMode,
                                               EngineVersion a_engineVersion, std::string a_projectId,
                                               MinimalRuntimeDataPublication a_runtimeData) noexcept
{
    try
    {
        const bool monolithicShipping = a_buildRequest.profile.target() == BuildTarget::ShippingProduct;
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                             WorkflowError::OwnerThreadViolation,
                                                             "Package workflow start requires owner thread"));
        }
        advance();
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->current.state == PackageWorkflowState::Building ||
                m_impl->current.state == PackageWorkflowState::Packaging ||
                m_impl->current.state == PackageWorkflowState::Running)
            {
                return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                                 WorkflowError::OperationAlreadyRunning,
                                                                 "Package workflow operation is already running"));
            }
        }
        Result<void> recovered = m_impl->retry_staging_recovery();
        if (!recovered)
        {
            return recovered;
        }
        Impl::PackageInputs inputs{a_engineVersion, std::move(a_projectId), std::move(a_runtimeData)};
        {
            std::scoped_lock lock(m_impl->mutex);
            Result<void> started = m_impl->buildService->start(std::move(a_buildRequest), a_configureMode);
            if (!started)
            {
                return started;
            }
            m_impl->pendingInputs = inputs;
            m_impl->retryInputs = std::move(inputs);
            m_impl->isCancellationRequested = false;
            m_impl->current.state = PackageWorkflowState::Building;
            m_impl->current.activeStage = PackageWorkflowStage::Build;
            m_impl->current.build = m_impl->buildService->snapshot();
            m_impl->current.package.reset();
            m_impl->current.publicationDiagnostic.reset();
            m_impl->current.recoveryStagingLocator.reset();
            m_impl->current.runOutput.clear();
            m_impl->current.message = monolithicShipping ? "Release Monolithic Shipping ProductのBuildを開始しました。"
                                                         : "Game Module Buildを開始しました。";
        }
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

Result<void> GamePackageWorkflowService::retry(std::string a_operationId, EngineVersion a_engineVersion,
                                               std::string a_projectId,
                                               MinimalRuntimeDataPublication a_runtimeData) noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                             WorkflowError::OwnerThreadViolation,
                                                             "Package workflow retry requires owner thread"));
        }
        advance();
        {
            std::scoped_lock lock(m_impl->mutex);
            if (!m_impl->retryInputs)
            {
                return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                                 WorkflowError::NoRetryableOperation,
                                                                 "No package workflow is available for retry"));
            }
            if (m_impl->current.state == PackageWorkflowState::Building ||
                m_impl->current.state == PackageWorkflowState::Packaging ||
                m_impl->current.state == PackageWorkflowState::Running)
            {
                return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                                 WorkflowError::OperationAlreadyRunning,
                                                                 "Package workflow operation is already running"));
            }
        }
        Result<void> recovered = m_impl->retry_staging_recovery();
        if (!recovered)
        {
            return recovered;
        }
        Impl::PackageInputs inputs{a_engineVersion, std::move(a_projectId), std::move(a_runtimeData)};
        {
            std::scoped_lock lock(m_impl->mutex);
            Result<void> restarted = m_impl->buildService->retry(std::move(a_operationId));
            if (!restarted)
            {
                return restarted;
            }
            m_impl->pendingInputs = inputs;
            m_impl->retryInputs = std::move(inputs);
            m_impl->isCancellationRequested = false;
            m_impl->current.state = PackageWorkflowState::Building;
            m_impl->current.activeStage = PackageWorkflowStage::Build;
            m_impl->current.build = m_impl->buildService->snapshot();
            m_impl->current.package.reset();
            m_impl->current.publicationDiagnostic.reset();
            m_impl->current.recoveryStagingLocator.reset();
            m_impl->current.runOutput.clear();
            m_impl->current.message = "BuildからPackageまでを再実行しました。";
        }
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

void GamePackageWorkflowService::advance() noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return;
        }
        PackageWorkflowState state;
        {
            std::scoped_lock lock(m_impl->mutex);
            state = m_impl->current.state;
        }
        if (state == PackageWorkflowState::Building)
        {
            BuildOperationSnapshot build = m_impl->buildService->snapshot();
            {
                std::scoped_lock lock(m_impl->mutex);
                m_impl->current.build = build;
            }
            if (build.state != GameBuildOperationState::Running)
            {
                static_cast<void>(m_impl->buildService->wait_for_completion());
                build = m_impl->buildService->snapshot();
                std::optional<Impl::PackageInputs> inputs;
                if (build.state == GameBuildOperationState::Succeeded && build.artifact)
                {
                    std::shared_ptr<PackageCancellation> cancellation = std::make_shared<PackageCancellation>();
                    {
                        std::scoped_lock lock(m_impl->mutex);
                        inputs = std::move(m_impl->pendingInputs);
                        m_impl->packageCancellation = cancellation;
                        if (m_impl->isCancellationRequested)
                        {
                            cancellation->request_cancel();
                        }
                        m_impl->current.build = build;
                        m_impl->current.state = PackageWorkflowState::Packaging;
                        m_impl->current.activeStage = PackageWorkflowStage::Package;
                        m_impl->current.message =
                            build.artifact->profile().target() == BuildTarget::ShippingProduct
                                ? "Runtime DataとMonolithic Shipping Packageを検証・公開しています。"
                                : "Runtime DataとModular Standalone Packageを検証・公開しています。";
                    }
                    if (!inputs)
                    {
                        std::scoped_lock lock(m_impl->mutex);
                        m_impl->packageCancellation.reset();
                        m_impl->isCancellationRequested = false;
                        m_impl->current.state = PackageWorkflowState::Failed;
                        m_impl->current.activeStage = PackageWorkflowStage::None;
                        m_impl->current.message = "Package入力が失われました。";
                    }
                    else
                    {
                        const BuildArtifactInventory artifact = *build.artifact;
                        const std::string operationId = build.operationId;
                        m_impl->worker = std::thread(
                            [impl = m_impl.get(), artifact, inputs = std::move(*inputs), operationId, cancellation]()
                            {
                                std::optional<PackagePublishDiagnosticSnapshot> diagnostic;
                                Result<PublishedRuntimePackageSnapshot> published =
                                    impl->publish_package(artifact, inputs, operationId, *cancellation, diagnostic);
                                std::scoped_lock lock(impl->mutex);
                                impl->packageCancellation.reset();
                                impl->isCancellationRequested = false;
                                impl->current.activeStage = PackageWorkflowStage::None;
                                impl->current.publicationDiagnostic = std::move(diagnostic);
                                if (!published)
                                {
                                    impl->current.recoveryStagingLocator =
                                        impl->recoveryStaging
                                            ? std::optional<std::string>(impl->recoveryStaging->path().text())
                                            : std::nullopt;
                                    impl->current.state = cancellation->is_cancel_requested()
                                                              ? PackageWorkflowState::Cancelled
                                                              : PackageWorkflowState::Failed;
                                    impl->current.message =
                                        error_message("Packageの検証または公開に失敗しました", *published.try_error());
                                    return;
                                }
                                impl->current.state = PackageWorkflowState::PackageReady;
                                impl->current.recoveryStagingLocator.reset();
                                impl->current.package = *published.try_value();
                                impl->current.latestSuccessfulPackage = std::move(*published.try_value());
                                impl->current.message =
                                    artifact.profile().target() == BuildTarget::ShippingProduct
                                        ? "Monolithic Shipping Packageを公開しました。ローカル実行専用です。"
                                        : "Modular Standalone Packageを公開しました。";
                            });
                    }
                }
                else
                {
                    std::scoped_lock lock(m_impl->mutex);
                    m_impl->pendingInputs.reset();
                    m_impl->current.build = build;
                    m_impl->isCancellationRequested = false;
                    m_impl->current.state = build_terminal_state(build.state);
                    m_impl->current.activeStage = PackageWorkflowStage::None;
                    m_impl->current.message = build.state == GameBuildOperationState::Cancelled
                                                  ? "BuildをキャンセルしたためPackageは開始していません。"
                                                  : "Buildに失敗したためPackageは開始していません。";
                }
            }
        }

        bool joinWorker = false;
        {
            std::scoped_lock lock(m_impl->mutex);
            joinWorker = m_impl->worker.joinable() && m_impl->current.state != PackageWorkflowState::Packaging &&
                         m_impl->current.state != PackageWorkflowState::Running;
        }
        if (joinWorker)
        {
            m_impl->worker.join();
        }
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

Result<void> GamePackageWorkflowService::request_cancel() noexcept
{
    try
    {
        PackageWorkflowState state;
        std::shared_ptr<PackageCancellation> packageCancellation;
        std::shared_ptr<ChildProcessCancellation> processCancellation;
        {
            std::scoped_lock lock(m_impl->mutex);
            state = m_impl->current.state;
            packageCancellation = m_impl->packageCancellation;
            processCancellation = m_impl->processCancellation;
            if (state == PackageWorkflowState::Building || state == PackageWorkflowState::Packaging ||
                state == PackageWorkflowState::Running)
            {
                m_impl->isCancellationRequested = true;
            }
        }
        if (state == PackageWorkflowState::Building)
        {
            static_cast<void>(m_impl->buildService->request_cancel());
            return Result<void>::success();
        }
        if (state == PackageWorkflowState::Packaging && packageCancellation)
        {
            packageCancellation->request_cancel();
            return Result<void>::success();
        }
        if (state == PackageWorkflowState::Running && processCancellation)
        {
            processCancellation->request_cancel();
            return Result<void>::success();
        }
        return Result<void>::failure(make_workflow_error(*m_impl->assertContext, WorkflowError::NoActiveOperation,
                                                         "No active package workflow can be cancelled"));
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

Result<void> GamePackageWorkflowService::run(PackageRunMode a_mode) noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_workflow_error(
                *m_impl->assertContext, WorkflowError::OwnerThreadViolation, "Package run requires owner thread"));
        }
        advance();
        std::optional<PublishedRuntimePackageSnapshot> package;
        std::shared_ptr<ChildProcessCancellation> cancellation = std::make_shared<ChildProcessCancellation>();
        {
            std::scoped_lock lock(m_impl->mutex);
            const bool failedRunCanRetry =
                m_impl->current.state == PackageWorkflowState::Failed && m_impl->current.package.has_value();
            if (m_impl->current.state != PackageWorkflowState::PackageReady &&
                m_impl->current.state != PackageWorkflowState::RunSucceeded && !failedRunCanRetry)
            {
                return Result<void>::failure(make_workflow_error(
                    *m_impl->assertContext, WorkflowError::NoPublishedPackage, "No published Package is ready to run"));
            }
            package = m_impl->current.package;
            if (!package)
            {
                return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                                 WorkflowError::NoPublishedPackage,
                                                                 "Published Package snapshot is missing"));
            }
        }
        const bool monolithic = package->manifest.executionModel == PackageExecutionModel::Monolithic;
        {
            std::scoped_lock lock(m_impl->mutex);
            m_impl->processCancellation = cancellation;
            m_impl->current.state = PackageWorkflowState::Running;
            m_impl->current.activeStage = PackageWorkflowStage::Run;
            m_impl->current.runOutput.clear();
            m_impl->current.message = monolithic ? "Monolithic Shipping Packageを検証しています。"
                                                 : "Modular Standalone Packageを検証しています。";
        }
        const std::string workingDirectory = join_absolute(m_impl->projectRoot, package->destination);
        const std::vector<std::string> arguments{a_mode == PackageRunMode::SmokeTest ? "--package-smoke-test"
                                                                                     : "--package"};
        ChildProcessRequest request(package->executable, arguments, workingDirectory, m_impl->runEnvironment,
                                    std::nullopt, k_maximumRuntimeOutputBytes);
        /// @brief Package検証とProcess監視をOwner Threadから隔離する
        m_impl->worker = std::thread(
            [impl = m_impl.get(), request = std::move(request), cancellation, monolithic,
             package = std::move(*package)]()
            {
                Result<PackageRunGuard> packageValidation = impl->validate_package_for_run(package);
                if (cancellation->is_cancel_requested())
                {
                    std::scoped_lock lock(impl->mutex);
                    impl->processCancellation.reset();
                    impl->isCancellationRequested = false;
                    impl->current.state = PackageWorkflowState::PackageReady;
                    impl->current.activeStage = PackageWorkflowStage::None;
                    impl->current.message = monolithic ? "Monolithic Shipping Productを停止しました。"
                                                       : "Modular Standalone Runtimeを停止しました。";
                    return;
                }
                if (!packageValidation)
                {
                    Error error = std::move(*packageValidation.try_error());
                    const std::string message = error_message("Packageの実行前検証に失敗しました", error);
                    std::scoped_lock lock(impl->mutex);
                    impl->processCancellation.reset();
                    impl->isCancellationRequested = false;
                    impl->current.state = PackageWorkflowState::Failed;
                    impl->current.activeStage = PackageWorkflowStage::None;
                    impl->current.message = message;
                    return;
                }
                PackageRunGuard packageGuard = std::move(*packageValidation.try_value());
                if (!packageGuard.arm_directory_change_cancellation(cancellation))
                {
                    std::scoped_lock lock(impl->mutex);
                    impl->processCancellation.reset();
                    impl->isCancellationRequested = false;
                    impl->current.state = PackageWorkflowState::Failed;
                    impl->current.activeStage = PackageWorkflowStage::None;
                    impl->current.message = "Package Treeの実行時監視を開始できませんでした。";
                    return;
                }
                {
                    std::scoped_lock lock(impl->mutex);
                    impl->current.message = monolithic ? "Monolithic Shipping Productを起動しました。"
                                                       : "Modular Standalone Runtimeを起動しました。";
                }
                Result<ChildProcessResult> runResult = impl->runProcessRunner->run(request, *cancellation);
                const bool packageChangedDuringRun = packageGuard.finish_and_has_directory_change();
                std::scoped_lock lock(impl->mutex);
                impl->processCancellation.reset();
                impl->isCancellationRequested = false;
                impl->current.activeStage = PackageWorkflowStage::None;
                if (packageChangedDuringRun)
                {
                    impl->current.state = PackageWorkflowState::Failed;
                    impl->current.message = "実行中にPackage Treeの変更を検知したため結果を拒否しました。";
                    return;
                }
                if (!runResult)
                {
                    impl->current.state = PackageWorkflowState::Failed;
                    impl->current.message =
                        error_message("Product Processの起動または監視に失敗しました", *runResult.try_error());
                    return;
                }
                impl->current.runOutput = runResult.try_value()->output();
                if (runResult.try_value()->outcome() == ChildProcessOutcome::Cancelled)
                {
                    impl->current.state = PackageWorkflowState::PackageReady;
                    impl->current.message = monolithic ? "Monolithic Shipping Productを停止しました。"
                                                       : "Modular Standalone Runtimeを停止しました。";
                    return;
                }
                if (runResult.try_value()->outcome() == ChildProcessOutcome::Exited &&
                    runResult.try_value()->exit_code() == std::optional<std::uint32_t>(0U))
                {
                    impl->current.state = PackageWorkflowState::RunSucceeded;
                    impl->current.message = monolithic ? "Monolithic Shipping Productが正常終了しました。"
                                                       : "Modular Standalone Runtimeが正常終了しました。";
                    return;
                }
                impl->current.state = PackageWorkflowState::Failed;
                impl->current.message = monolithic ? "Monolithic Shipping Productが異常終了しました。"
                                                   : "Modular Standalone Runtimeが異常終了しました。";
            });
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

Result<void> GamePackageWorkflowService::stop() noexcept
{
    try
    {
        std::shared_ptr<ChildProcessCancellation> processCancellation;
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->current.state != PackageWorkflowState::Running || !m_impl->processCancellation)
            {
                return Result<void>::failure(make_workflow_error(*m_impl->assertContext,
                                                                 WorkflowError::NoActiveOperation,
                                                                 "No running package runtime can be stopped"));
            }
            m_impl->isCancellationRequested = true;
            processCancellation = m_impl->processCancellation;
        }
        processCancellation->request_graceful_stop();
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}

Result<void> GamePackageWorkflowService::wait_for_package() noexcept
{
    if (!m_impl->is_owner_thread())
    {
        return Result<void>::failure(make_workflow_error(*m_impl->assertContext, WorkflowError::OwnerThreadViolation,
                                                         "Package wait requires owner thread"));
    }
    for (;;)
    {
        advance();
        const PackageWorkflowState state = snapshot().state;
        if (state != PackageWorkflowState::Building && state != PackageWorkflowState::Packaging)
        {
            return Result<void>::success();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

Result<void> GamePackageWorkflowService::wait_for_run_completion() noexcept
{
    if (!m_impl->is_owner_thread())
    {
        return Result<void>::failure(make_workflow_error(*m_impl->assertContext, WorkflowError::OwnerThreadViolation,
                                                         "Package run wait requires owner thread"));
    }
    for (;;)
    {
        advance();
        if (snapshot().state != PackageWorkflowState::Running)
        {
            return Result<void>::success();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

PackageWorkflowSnapshot GamePackageWorkflowService::snapshot() const noexcept
{
    try
    {
        std::scoped_lock lock(m_impl->mutex);
        return m_impl->current;
    }
    catch (...)
    {
        terminate_workflow_exception(*m_impl->assertContext);
    }
}
} // namespace cue::package
