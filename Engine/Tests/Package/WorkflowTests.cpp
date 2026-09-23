#include <Cue/Package/Workflow.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Project/Generator.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Scene/SceneDocument.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <Windows.h>

namespace
{
constexpr std::string_view k_projectId = "00000000-0000-4000-8000-000000000901";
constexpr std::string_view k_sceneId = "10000000-0000-4000-8000-000000000001";
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 1U};

/// @brief Test内のFatalを固定Exit Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(76);
    }

    /// @brief Message付きFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

/// @brief Test所有の二RootをProcess終了時に除去する
class TestDirectory final
{
  public:
    /// @brief Process固有のTemporary Rootを作成する
    TestDirectory()
        : m_path(std::filesystem::temp_directory_path() /
                 (L"CuePackageWorkflowTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64())))
    {
        std::filesystem::create_directories(m_path);
    }

    TestDirectory(const TestDirectory &) = delete;
    TestDirectory &operator=(const TestDirectory &) = delete;

    /// @brief Test所有Rootだけを除去する
    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    /// @brief Temporary Rootを返す
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

enum class RunnerMode : std::uint8_t
{
    Succeed,
    Fail,
    BlockUntilCancelled
};

struct RunnerState final
{
    std::atomic<RunnerMode> mode = RunnerMode::Succeed;
    std::atomic<cue::ChildProcessCancellationMode> lastCancellationMode = cue::ChildProcessCancellationMode::None;
    std::atomic<std::uint32_t> calls = 0U;
    std::atomic<std::size_t> maximumCapturedOutputBytes = 0U;
    std::atomic<bool> active = false;
    std::atomic<bool> probePackageMutation = false;
    std::atomic<bool> packageMutationBlocked = false;
    std::atomic<bool> packageDirectoryRenameBlocked = false;
    std::atomic<bool> probePackageEntryCreation = false;
    std::atomic<bool> packageEntryCreationSucceeded = false;
    std::vector<std::string> mutationProbeRelativePaths;
    std::vector<std::filesystem::path> mutationProbeDirectories;
};

/// @brief BuildまたはRuntimeの成功、失敗、取消待機をProcessなしで再現する
class ControlledRunner final : public cue::ChildProcessRunner
{
  public:
    /// @brief 共有制御状態を借用する
    explicit ControlledRunner(RunnerState &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief 指定Modeに応じた所有Process結果を返す
    [[nodiscard]] cue::Result<cue::ChildProcessResult> run(
        const cue::ChildProcessRequest &a_request,
        const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        m_state->maximumCapturedOutputBytes.store(a_request.maximum_captured_output_bytes().value_or(0U),
                                                  std::memory_order_release);
        if (m_state->probePackageMutation.load(std::memory_order_acquire))
        {
            bool allFilesBlocked = true;
            bool allDirectoryRenamesBlocked = true;
            const std::filesystem::path root(a_request.working_directory());
            for (const std::string &relativePath : m_state->mutationProbeRelativePaths)
            {
                const std::filesystem::path path = relativePath.empty() ? std::filesystem::path(a_request.executable())
                                                                        : root / std::filesystem::path(relativePath);
                HANDLE mutation = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                const DWORD code = mutation == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                if (mutation != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(mutation);
                }
                allFilesBlocked =
                    allFilesBlocked && mutation == INVALID_HANDLE_VALUE && code == ERROR_SHARING_VIOLATION;
            }
            for (const std::filesystem::path &path : m_state->mutationProbeDirectories)
            {
                const std::filesystem::path renamed = path.parent_path() / (path.filename().wstring() + L".probe");
                const BOOL renamedDirectory = MoveFileExW(path.c_str(), renamed.c_str(), MOVEFILE_WRITE_THROUGH);
                const DWORD renameCode = renamedDirectory == FALSE ? GetLastError() : ERROR_SUCCESS;
                if (renamedDirectory != FALSE)
                {
                    static_cast<void>(MoveFileExW(renamed.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH));
                }
                allDirectoryRenamesBlocked =
                    allDirectoryRenamesBlocked && renamedDirectory == FALSE &&
                    (renameCode == ERROR_SHARING_VIOLATION || renameCode == ERROR_ACCESS_DENIED);
            }
            m_state->packageMutationBlocked.store(allFilesBlocked, std::memory_order_release);
            m_state->packageDirectoryRenameBlocked.store(allDirectoryRenamesBlocked, std::memory_order_release);
        }
        if (m_state->probePackageEntryCreation.load(std::memory_order_acquire))
        {
            const std::filesystem::path injected =
                std::filesystem::path(a_request.working_directory()) / L"transient-unlisted.dll";
            HANDLE created =
                CreateFileW(injected.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            const bool succeeded = created != INVALID_HANDLE_VALUE;
            if (succeeded)
            {
                CloseHandle(created);
                static_cast<void>(DeleteFileW(injected.c_str()));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            m_state->packageEntryCreationSucceeded.store(succeeded, std::memory_order_release);
        }
        m_state->active.store(true, std::memory_order_release);
        const std::uint32_t call = m_state->calls.fetch_add(1U, std::memory_order_relaxed);
        while (m_state->mode.load(std::memory_order_acquire) == RunnerMode::BlockUntilCancelled &&
               !a_cancellation.is_cancel_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_state->active.store(false, std::memory_order_release);
        m_state->lastCancellationMode.store(a_cancellation.cancellation_mode(), std::memory_order_release);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::cancelled({}));
        }
        const std::uint32_t exitCode = m_state->mode.load(std::memory_order_acquire) == RunnerMode::Fail ? 2U : 0U;
        return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::exited(
            exitCode, {{call, cue::ChildProcessStream::StandardOutput, "process-log"}}));
    }

  private:
    RunnerState *m_state;
};

struct PublisherState final
{
    std::atomic<bool> corruptInventory = false;
    std::atomic<bool> invalidPortableExecutable = false;
    std::atomic<bool> oversizedRuntimePeImage = false;
    std::atomic<bool> oversizedRuntimePeInventory = false;
    std::atomic<bool> runtimeHostOnlyInventoryOverflow = false;
    std::atomic<bool> readerLeaseActive = false;
    std::atomic<bool> artifactReadWithoutLease = false;
    std::atomic<bool> packageWriteWithLease = false;
    std::atomic<std::uint32_t> calls = 0U;
    std::atomic<std::uint32_t> readerCalls = 0U;
    std::atomic<std::uint32_t> artifactReadCalls = 0U;
};

struct RecoveryFilesystemState final
{
    std::atomic<std::uint32_t> writeFailuresRemaining = 0U;
    std::atomic<std::uint32_t> durabilityFailuresRemaining = 0U;
    std::atomic<std::uint32_t> rollbackFailuresRemaining = 0U;
    std::atomic<std::uint32_t> rollbackCalls = 0U;
    std::atomic<bool> blockPackageManifestRead = false;
    std::atomic<bool> packageManifestReadActive = false;
    std::atomic<bool> releasePackageManifestRead = false;
};

/// @brief Package WriteとRollbackの連続失敗を注入し、保持Tokenの再試行を検証するRoot
class RecoveryFilesystemRoot final : public cue::FilesystemRoot
{
  public:
    /// @brief 委譲先Rootと共有Failure状態を所有する
    RecoveryFilesystemRoot(std::unique_ptr<cue::FilesystemRoot> a_inner, RecoveryFilesystemState &a_state,
                           PublisherState &a_publisherState, const cue::AssertContext &a_assertContext) noexcept
        : m_inner(std::move(a_inner)), m_state(&a_state), m_publisherState(&a_publisherState),
          m_assertContext(&a_assertContext)
    {
    }

    /// @brief 委譲先Rootを解放する
    ~RecoveryFilesystemRoot() override = default;

    /// @brief 委譲先Root Identityを返す
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> root_identity() const noexcept override
    {
        return m_inner->root_identity();
    }

    /// @brief Entry照会を委譲する
    [[nodiscard]] cue::Result<cue::EntryType> query_entry(const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->query_entry(a_path);
    }

    /// @brief File読取りを委譲する
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &a_path,
                                                                std::size_t a_maxBytes) noexcept override
    {
        if (m_state->blockPackageManifestRead.load(std::memory_order_acquire) &&
            a_path.text().starts_with("Generated/Packages/") && a_path.text().ends_with("/CuePackage.json"))
        {
            m_state->packageManifestReadActive.store(true, std::memory_order_release);
            while (!m_state->releasePackageManifestRead.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            m_state->packageManifestReadActive.store(false, std::memory_order_release);
        }
        if (a_path.text().starts_with("Generated/Artifacts/") &&
            !m_publisherState->readerLeaseActive.load(std::memory_order_acquire))
        {
            m_publisherState->artifactReadWithoutLease.store(true, std::memory_order_release);
        }
        if (a_path.text().starts_with("Generated/Artifacts/"))
        {
            m_publisherState->artifactReadCalls.fetch_add(1U, std::memory_order_relaxed);
        }
        return m_inner->read_file(a_path, a_maxBytes);
    }

    /// @brief Directory作成を委譲する
    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->create_directories(a_path);
    }

    /// @brief 指定回数だけPackage Writeを失敗させ、それ以外を委譲する
    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &a_path,
                                                      std::span<const std::byte> a_bytes) noexcept override
    {
        if (a_path.text().starts_with("Generated/Packages/") &&
            m_publisherState->readerLeaseActive.load(std::memory_order_acquire))
        {
            m_publisherState->packageWriteWithLease.store(true, std::memory_order_release);
        }
        if (consume(m_state->writeFailuresRemaining))
        {
            return cue::Result<void>::failure(make_failure("Package content write failed"));
        }
        return m_inner->write_file_atomic(a_path, a_bytes);
    }

    /// @brief Recovery Backup書込みを委譲する
    [[nodiscard]] cue::Result<void> write_recovery_backup_atomic(
        const cue::RelativePath &a_destination, std::span<const std::byte> a_bytes,
        const cue::AssertContext &a_assertContext) noexcept override
    {
        return m_inner->write_recovery_backup_atomic(a_destination, a_bytes, a_assertContext);
    }

    /// @brief File Write Lease取得を委譲する
    [[nodiscard]] cue::Result<cue::FileWriteLease> acquire_file_write_lease(
        const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->acquire_file_write_lease(a_path);
    }

    /// @brief Conditional Atomic Writeを委譲する
    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(cue::FileWriteLease &a_lease,
                                                                   const cue::RelativePath &a_path,
                                                                   cue::FileFingerprint a_expected,
                                                                   std::size_t a_maximumExpectedBytes,
                                                                   std::span<const std::byte> a_bytes) noexcept override
    {
        return m_inner->write_file_atomic_if_unchanged(a_lease, a_path, a_expected, a_maximumExpectedBytes, a_bytes);
    }

    /// @brief Regular File削除を委譲する
    [[nodiscard]] cue::Result<void> remove_file(const cue::RelativePath &a_path) noexcept override
    {
        return m_inner->remove_file(a_path);
    }

    /// @brief Staging作成を委譲する
    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(
        const cue::RelativePath &a_destination) noexcept override
    {
        return m_inner->create_staging_area(a_destination);
    }

    /// @brief Staging公開を委譲する
    [[nodiscard]] cue::Result<void> publish_staging_area(
        cue::StagingArea &&a_staging, const cue::RelativePath &a_destination,
        const cue::StagingPublishAuthorization *a_authorization = nullptr) noexcept override
    {
        cue::Result<void> published =
            m_inner->publish_staging_area(std::move(a_staging), a_destination, a_authorization);
        if (published && consume(m_state->durabilityFailuresRemaining))
        {
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                                                 "Published Package durability is unknown"));
        }
        return published;
    }

    /// @brief 呼出回数を記録し、指定回数だけRollbackを失敗させる
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&a_staging) noexcept override
    {
        m_state->rollbackCalls.fetch_add(1U, std::memory_order_relaxed);
        if (consume(m_state->rollbackFailuresRemaining))
        {
            return cue::Result<void>::failure(make_failure("Package staging rollback failed"));
        }
        return m_inner->rollback_staging_area(std::move(a_staging));
    }

  private:
    /// @brief 残りFailure回数を一つ消費できたか返す
    [[nodiscard]] static bool consume(std::atomic<std::uint32_t> &a_remaining) noexcept
    {
        std::uint32_t remaining = a_remaining.load(std::memory_order_acquire);
        while (remaining != 0U)
        {
            if (a_remaining.compare_exchange_weak(remaining, remaining - 1U, std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
            {
                return true;
            }
        }
        return false;
    }

    /// @brief Test用Portable IO Errorを構築する
    [[nodiscard]] cue::Error make_failure(std::string_view a_summary) const noexcept
    {
        return cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, a_summary);
    }

    std::unique_ptr<cue::FilesystemRoot> m_inner;
    RecoveryFilesystemState *m_state;
    PublisherState *m_publisherState;
    const cue::AssertContext *m_assertContext;
};

/// @brief 失敗した検証の呼出位置を標準エラーへ出す
[[nodiscard]] bool require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::cerr << "Requirement failed at " << a_location.file_name() << ':' << a_location.line() << '\n';
    }
    return a_condition;
}

/// @brief Test Fileを所有Byte Snapshotとして読む
[[nodiscard]] std::optional<std::vector<std::byte>> read_binary_file(const std::filesystem::path &a_path)
{
    std::ifstream stream(a_path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        return std::nullopt;
    }
    const std::streampos end = stream.tellg();
    if (end < 0)
    {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream && !bytes.empty())
    {
        return std::nullopt;
    }
    return bytes;
}

/// @brief Test Fileを指定Byte列へ置換する
[[nodiscard]] bool write_binary_file(const std::filesystem::path &a_path, std::span<const std::byte> a_bytes)
{
    std::ofstream stream(a_path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
    return stream.good();
}

/// @brief FileのWrite／Delete共有拒否が解放済みか非破壊で確認する
[[nodiscard]] bool can_open_file_for_mutation(const std::filesystem::path &a_path) noexcept
{
    HANDLE handle =
        CreateFileW(a_path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    CloseHandle(handle);
    return true;
}

/// @brief Directoryを一時名へ往復RenameしてPath固定Leaseの解放を確認する
[[nodiscard]] bool can_rename_directory_round_trip(const std::filesystem::path &a_path) noexcept
{
    const std::filesystem::path renamed = a_path.parent_path() / (a_path.filename().wstring() + L".lease-release");
    if (MoveFileExW(a_path.c_str(), renamed.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        return false;
    }
    return MoveFileExW(renamed.c_str(), a_path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
}

/// @brief Test PEへLittle-endian 16-bit値を書き込む
void write_u16(std::vector<std::byte> &a_bytes, std::size_t a_offset, std::uint16_t a_value) noexcept
{
    a_bytes[a_offset] = static_cast<std::byte>(a_value & 0xffU);
    a_bytes[a_offset + 1U] = static_cast<std::byte>((a_value >> 8U) & 0xffU);
}

/// @brief Test PEへLittle-endian 32-bit値を書き込む
void write_u32(std::vector<std::byte> &a_bytes, std::size_t a_offset, std::uint32_t a_value) noexcept
{
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        a_bytes[a_offset + index] = static_cast<std::byte>((a_value >> (index * 8U)) & 0xffU);
    }
}

/// @brief Importを持たない最小x64 PE Test Imageを生成する
[[nodiscard]] std::vector<std::byte> make_test_pe()
{
    std::vector<std::byte> bytes(0x1000U, std::byte{0U});
    write_u16(bytes, 0U, 0x5a4dU);
    write_u32(bytes, 0x3cU, 0x80U);
    write_u32(bytes, 0x80U, 0x00004550U);
    write_u16(bytes, 0x84U, 0x8664U);
    write_u16(bytes, 0x86U, 1U);
    write_u16(bytes, 0x94U, 240U);
    constexpr std::size_t optional = 0x98U;
    write_u16(bytes, optional, 0x020bU);
    write_u32(bytes, optional + 32U, 0x1000U);
    write_u32(bytes, optional + 36U, 0x200U);
    write_u32(bytes, optional + 56U, 0x2000U);
    write_u32(bytes, optional + 60U, 0x200U);
    write_u32(bytes, optional + 108U, 16U);
    constexpr std::size_t section = 0x188U;
    write_u32(bytes, section + 8U, 0x0e00U);
    write_u32(bytes, section + 12U, 0x1000U);
    write_u32(bytes, section + 16U, 0x0e00U);
    write_u32(bytes, section + 20U, 0x200U);
    return bytes;
}

/// @brief Build成功ArtifactをTest Rootへ実体化するPublisher
class MaterializingPublisher final : public cue::BuildArtifactPublisher
{
  public:
    /// @brief Native Resourceを持たないTest Lease
    class Lease final : public cue::BuildWorkspaceLease
    {
      public:
        /// @brief 空Leaseを構築する
        Lease() noexcept = default;
        /// @brief 空Leaseを破棄する
        ~Lease() override = default;
    };

    /// @brief Artifact Rootと制御状態を借用する
    MaterializingPublisher(PublisherState &a_state, const cue::AssertContext &a_assertContext) noexcept
        : m_state(&a_state), m_assertContext(&a_assertContext)
    {
    }

    /// @brief 取消前なら空のExclusive Leaseを返す
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>> acquire_build_lease(
        const cue::BuildPlan &, const cue::ChildProcessCancellation &a_cancellation,
        cue::BuildArtifactLockDeadline) noexcept override
    {
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(std::nullopt);
        }
        return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(
            std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>(std::make_unique<Lease>()));
    }

    /// @brief Operation固有Versionへ必須ArtifactとPackage対象外PDBを書きInventoryを返す
    [[nodiscard]] cue::Result<std::optional<cue::BuildArtifactInventory>> publish(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation,
        std::unique_ptr<cue::BuildWorkspaceLease> a_buildLease, cue::BuildArtifactLockDeadline) noexcept override
    {
        static_cast<void>(a_buildLease);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
        }
        m_state->calls.fetch_add(1U, std::memory_order_relaxed);
        if (a_plan.profile().target() == cue::BuildTarget::ShippingProduct)
        {
            return publish_shipping_product(a_plan);
        }
        const std::vector<std::byte> moduleBytes = m_state->invalidPortableExecutable.load(std::memory_order_acquire)
                                                       ? text_bytes("test-game-module")
                                                       : make_test_pe();
        const std::vector<std::byte> runtimeHostBytes = make_test_pe();
        const std::vector<std::byte> pdbBytes = text_bytes("test-debug-symbols");
        const std::vector<std::byte> metadataBytes = text_bytes("{\"schemaVersion\":1}\n");
        const bool hasOversizedInventory = m_state->oversizedRuntimePeInventory.load(std::memory_order_acquire);
        const bool hasRuntimeHostOnlyOverflow =
            m_state->runtimeHostOnlyInventoryOverflow.load(std::memory_order_acquire);
        auto modulePayload = cue::package::PackageFilePayload::create(
            cue::package::PackageFileRole::GameModule, "CueGameModule.dll", moduleBytes, *m_assertContext);
        auto metadataPayload =
            cue::package::PackageFilePayload::create(cue::package::PackageFileRole::GameModuleMetadata,
                                                     "CueGameModule.metadata.json", metadataBytes, *m_assertContext);
        auto runtimeHostPayload = cue::package::PackageFilePayload::create(
            cue::package::PackageFileRole::RuntimeHost, "CueRuntimeHost.exe", runtimeHostBytes, *m_assertContext);
        if (!modulePayload || !metadataPayload || !runtimeHostPayload)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                !modulePayload ? std::move(*modulePayload.try_error())
                               : (!metadataPayload ? std::move(*metadataPayload.try_error())
                                                   : std::move(*runtimeHostPayload.try_error())));
        }
        const std::string artifactId(a_plan.operation_id());
        const std::filesystem::path versionDirectory = std::filesystem::path(a_plan.project_root()) /
                                                       std::filesystem::path(a_plan.artifact_store_directory()) /
                                                       L"Versions" / std::filesystem::path(artifactId);
        std::error_code error;
        std::filesystem::create_directories(versionDirectory, error);
        if (error || !write_file(versionDirectory / L"CueGameModule.dll", moduleBytes) ||
            !write_file(versionDirectory / L"CueGameModule.pdb", pdbBytes) ||
            !write_file(versionDirectory / L"CueRuntimeHost.exe", runtimeHostBytes) ||
            !write_file(versionDirectory / L"CueGameModule.metadata.json", metadataBytes) ||
            (hasOversizedInventory && (!write_file(versionDirectory / L"RuntimeDependencyA.dll", moduleBytes) ||
                                       !write_file(versionDirectory / L"RuntimeDependencyB.dll", moduleBytes))) ||
            (hasRuntimeHostOnlyOverflow &&
             !write_file(versionDirectory / L"RuntimeDependencyC.dll", moduleBytes)))
        {
            cue::ErrorCode code = cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.Package.Test", 1);
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(cue::Error::create(
                m_assertContext->fatal_handler(), std::move(code), "Artifact materialization failed"));
        }
        std::string moduleHash(modulePayload.try_value()->entry().sha256());
        if (m_state->corruptInventory.load(std::memory_order_acquire))
        {
            moduleHash.assign(64U, 'f');
        }
        const std::uint64_t moduleSize = m_state->oversizedRuntimePeImage.load(std::memory_order_acquire)
                                             ? cue::package::k_maximumRuntimePeImageBytes + 1U
                                             : moduleBytes.size();
        const std::uint64_t runtimeHostSize = hasRuntimeHostOnlyOverflow
                                                  ? cue::package::k_maximumRuntimePeImageBytes
                                                  : runtimeHostBytes.size();
        std::vector<cue::BuildArtifactFile> files = {{"CueGameModule.dll", moduleSize, std::move(moduleHash)},
                                                     {"CueGameModule.pdb", pdbBytes.size(), std::string(64U, 'a')},
                                                     {"CueGameModule.metadata.json", metadataBytes.size(),
                                                      std::string(metadataPayload.try_value()->entry().sha256())},
                                                     {"CueRuntimeHost.exe", runtimeHostSize,
                                                      std::string(runtimeHostPayload.try_value()->entry().sha256())}};
        if (hasOversizedInventory)
        {
            const std::string dependencyHash(modulePayload.try_value()->entry().sha256());
            files.push_back({"RuntimeDependencyA.dll", cue::package::k_maximumRuntimePeImageBytes, dependencyHash});
            files.push_back({"RuntimeDependencyB.dll", cue::package::k_maximumRuntimePeImageBytes, dependencyHash});
        }
        if (hasRuntimeHostOnlyOverflow)
        {
            files.push_back({"RuntimeDependencyC.dll", cue::package::k_maximumRuntimePeImageBytes,
                             std::string(modulePayload.try_value()->entry().sha256())});
        }
        auto inventory = cue::BuildArtifactInventory::create(a_plan, artifactId, std::move(files), *m_assertContext);
        if (!inventory)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(std::move(*inventory.try_error()));
        }
        return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(
            std::optional<cue::BuildArtifactInventory>(std::move(*inventory.try_value())));
    }

  private:
    /// @brief Release Shipping Productの最小ArtifactをVersion DirectoryへMaterializeする
    [[nodiscard]] cue::Result<std::optional<cue::BuildArtifactInventory>> publish_shipping_product(
        const cue::BuildPlan &a_plan) noexcept
    {
        const std::vector<std::byte> executableBytes = text_bytes("test-monolithic-product");
        const std::vector<std::byte> metadataBytes = text_bytes("{\"schemaVersion\":2}\n");
        auto executablePayload =
            cue::package::PackageFilePayload::create(cue::package::PackageFileRole::ApplicationExecutable,
                                                     "CueGameProduct.exe", executableBytes, *m_assertContext);
        auto metadataPayload =
            cue::package::PackageFilePayload::create(cue::package::PackageFileRole::GameModuleMetadata,
                                                     "CueGameProduct.metadata.json", metadataBytes, *m_assertContext);
        if (!executablePayload || !metadataPayload)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                executablePayload ? std::move(*metadataPayload.try_error())
                                  : std::move(*executablePayload.try_error()));
        }
        const std::string artifactId(a_plan.operation_id());
        const std::filesystem::path versionDirectory = std::filesystem::path(a_plan.project_root()) /
                                                       std::filesystem::path(a_plan.artifact_store_directory()) /
                                                       L"Versions" / std::filesystem::path(artifactId);
        std::error_code error;
        std::filesystem::create_directories(versionDirectory, error);
        if (error || !write_file(versionDirectory / L"CueGameProduct.exe", executableBytes) ||
            !write_file(versionDirectory / L"CueGameProduct.metadata.json", metadataBytes))
        {
            cue::ErrorCode code = cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.Package.Test", 2);
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(cue::Error::create(
                m_assertContext->fatal_handler(), std::move(code), "Shipping artifact materialization failed"));
        }
        std::vector<cue::BuildArtifactFile> files = {
            {"CueGameProduct.exe", executableBytes.size(),
             std::string(executablePayload.try_value()->entry().sha256())},
            {"CueGameProduct.metadata.json", metadataBytes.size(),
             std::string(metadataPayload.try_value()->entry().sha256())},
        };
        auto inventory = cue::BuildArtifactInventory::create(a_plan, artifactId, std::move(files), *m_assertContext);
        if (!inventory)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(std::move(*inventory.try_error()));
        }
        return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(
            std::optional<cue::BuildArtifactInventory>(std::move(*inventory.try_value())));
    }

    /// @brief ASCII Test入力を所有Byte列へ変換する
    [[nodiscard]] static std::vector<std::byte> text_bytes(std::string_view a_text)
    {
        const std::span<const char> characters(a_text.data(), a_text.size());
        const std::span<const std::byte> bytes = std::as_bytes(characters);
        return {bytes.begin(), bytes.end()};
    }

    /// @brief Test Artifact Byte列をBinary Fileへ書く
    [[nodiscard]] static bool write_file(const std::filesystem::path &a_path,
                                         std::span<const std::byte> a_bytes) noexcept
    {
        std::ofstream stream(a_path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
        return stream.good();
    }

    PublisherState *m_state;
    const cue::AssertContext *m_assertContext;
};

/// @brief WorkflowがArtifact読込中だけ保持するTest用Shared Read Leaseを発行するReader
class MaterializingArtifactReader final : public cue::BuildArtifactReader
{
  public:
    /// @brief Leaseの取得中状態を共有Test Stateへ反映するToken
    class Lease final : public cue::BuildArtifactReadLease
    {
      public:
        /// @brief Shared Read Lease取得を記録する
        explicit Lease(PublisherState &a_state) noexcept : m_state(&a_state)
        {
            m_state->readerLeaseActive.store(true, std::memory_order_release);
        }

        /// @brief Shared Read Lease解放を記録する
        ~Lease() override
        {
            m_state->readerLeaseActive.store(false, std::memory_order_release);
        }

        /// @brief Test StoreへBindingされたProject Identityを返す
        [[nodiscard]] std::string_view project_id() const noexcept override
        {
            return k_projectId;
        }

      private:
        PublisherState *m_state;
    };

    /// @brief 共有Test Stateを借用する
    explicit MaterializingArtifactReader(PublisherState &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief 取消前なら読込範囲を可視化する空Leaseを返す
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>> acquire_current_read_lease(
        const cue::BuildArtifactInventory &, const cue::BuildArtifactReadCancellation &a_cancellation,
        cue::BuildArtifactLockDeadline) noexcept override
    {
        m_state->readerCalls.fetch_add(1U, std::memory_order_relaxed);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::success(std::nullopt);
        }
        std::unique_ptr<cue::BuildArtifactReadLease> lease = std::make_unique<Lease>(*m_state);
        return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::success(
            std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>(std::move(lease)));
    }

  private:
    PublisherState *m_state;
};

/// @brief Processを起動しないBuild Runner設定を返す
[[nodiscard]] cue::CMakeRunnerSettings make_settings()
{
    return {"C:/Tools/cmake.exe", "C:/CueEngine", {}, std::chrono::seconds(5), std::chrono::seconds(5), "14.51.36231"};
}

/// @brief Debug Game Module用Build Requestを作る
[[nodiscard]] cue::BuildRequest make_request(std::string a_projectRoot, std::string a_operationId,
                                             const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    return {std::move(a_projectRoot), *profile.try_value(), std::move(a_operationId), k_workspaceCompatibility};
}

/// @brief Release UnsignedLocal Shipping Product用Build Requestを作る
[[nodiscard]] cue::BuildRequest make_shipping_request(std::string a_projectRoot, std::string a_operationId,
                                                      const cue::AssertContext &a_assertContext)
{
    auto profile = cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::UnsignedLocal, {}, a_assertContext);
    return {std::move(a_projectRoot), *profile.try_value(), std::move(a_operationId), k_workspaceCompatibility};
}

/// @brief 空Sceneから決定的Runtime Data Publicationを作る
[[nodiscard]] cue::Result<cue::package::MinimalRuntimeDataPublication> make_runtime_data(
    const cue::AssertContext &a_assertContext) noexcept
{
    auto projectId = cue::ProjectId::parse(k_projectId, a_assertContext);
    auto descriptor =
        projectId ? cue::create_blank_project_descriptor(
                        *projectId.try_value(), "Workflow Test",
                        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}},
                        k_sceneId, a_assertContext)
                  : cue::Result<cue::ProjectDescriptor>::failure(std::move(*projectId.try_error()));
    auto sceneId = cue::scene::SceneAssetId::parse(k_sceneId, a_assertContext);
    if (!descriptor || !sceneId)
    {
        return cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(
            descriptor ? std::move(*sceneId.try_error()) : std::move(*descriptor.try_error()));
    }
    cue::scene::SceneDocument scene = cue::scene::SceneDocument::create(*sceneId.try_value(), a_assertContext);
    auto snapshot = cue::scene::create_scene_snapshot(scene, a_assertContext);
    return snapshot
               ? cue::package::publish_minimal_runtime_data(*descriptor.try_value(), *snapshot.try_value(),
                                                            a_assertContext)
               : cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(std::move(*snapshot.try_error()));
}

/// @brief RunnerがProcess実行中になるまで上限付きで待つ
[[nodiscard]] bool wait_until_active(const RunnerState &a_state) noexcept
{
    for (std::uint32_t attempt = 0U; attempt < 1000U; ++attempt)
    {
        if (a_state.active.load(std::memory_order_acquire))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief Package Manifest検証がFilesystem境界で待機するまで上限付きで待つ
[[nodiscard]] bool wait_until_manifest_read(const RecoveryFilesystemState &a_state) noexcept
{
    for (std::uint32_t attempt = 0U; attempt < 1000U; ++attempt)
    {
        if (a_state.packageManifestReadActive.load(std::memory_order_acquire))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief 指定回数目のProcessが終了しBuild Serviceへ結果を渡すまで上限付きで待つ
[[nodiscard]] bool wait_until_completed_call(const RunnerState &a_state, std::uint32_t a_minimumCalls) noexcept
{
    for (std::uint32_t attempt = 0U; attempt < 1000U; ++attempt)
    {
        if (a_state.calls.load(std::memory_order_acquire) >= a_minimumCalls &&
            !a_state.active.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief Build・Package・Runの成功、失敗保全、停止をHeadlessで検証する
[[nodiscard]] bool test_workflow(const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    const std::filesystem::path projectRoot = directory.path() / L"Project";
    const std::filesystem::path engineRoot = directory.path() / L"Engine";
    std::filesystem::create_directories(projectRoot);
    std::filesystem::create_directories(engineRoot);
    constexpr std::string_view firstOperation = "01234567-89ab-4cde-8f01-23456789abcd";
    cue::BuildRequest firstRequest =
        make_request(projectRoot.generic_string(), std::string(firstOperation), a_assertContext);
    RunnerState buildRunner;
    RunnerState runRunner;
    PublisherState publisher;
    RecoveryFilesystemState recoveryFilesystem;
    auto build = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(buildRunner),
                                               std::make_unique<MaterializingPublisher>(publisher, a_assertContext),
                                               a_assertContext);
    auto projectFilesystem = cue::create_windows_filesystem_root(projectRoot.generic_string(), a_assertContext);
    auto engineFilesystem = cue::create_windows_filesystem_root(engineRoot.generic_string(), a_assertContext);
    if (!require(build && projectFilesystem && engineFilesystem))
    {
        return false;
    }
    std::unique_ptr<cue::GameBuildService> buildService = std::move(*build.try_value());
    auto guardedProjectFilesystem = std::make_unique<RecoveryFilesystemRoot>(
        std::move(*projectFilesystem.try_value()), recoveryFilesystem, publisher, a_assertContext);
    auto workflow = cue::package::GamePackageWorkflowService::create(
        std::move(buildService), std::make_unique<MaterializingArtifactReader>(publisher),
        std::move(guardedProjectFilesystem), std::move(*engineFilesystem.try_value()),
        std::make_unique<ControlledRunner>(runRunner), projectRoot.generic_string(), {},
        cue::package::RuntimeHostBuildSource::PublishedBuildArtifact, a_assertContext);
    auto runtimeData = make_runtime_data(a_assertContext);
    if (!require(workflow && runtimeData))
    {
        return false;
    }
    std::unique_ptr<cue::package::GamePackageWorkflowService> service = std::move(*workflow.try_value());
    if (!require(
            service->start(std::move(firstRequest), cue::CMakeConfigureMode::Required, {1U, 0U, 0U},
                           std::string(k_projectId), *runtimeData.try_value()) &&
            service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot first = service->snapshot();
    if (first.state != cue::package::PackageWorkflowState::PackageReady)
    {
        std::cerr << "Initial package state=" << static_cast<int>(first.state) << " message=" << first.message << '\n';
    }
    if (!require(first.state == cue::package::PackageWorkflowState::PackageReady && first.package &&
                 first.latestSuccessfulPackage && first.package->operationId == firstOperation &&
                 publisher.readerCalls.load(std::memory_order_acquire) == 1U &&
                 !publisher.readerLeaseActive.load(std::memory_order_acquire) &&
                 !publisher.artifactReadWithoutLease.load(std::memory_order_acquire) &&
                 !publisher.packageWriteWithLease.load(std::memory_order_acquire) &&
                 std::filesystem::exists(projectRoot / std::filesystem::path(first.package->destination) /
                                         L"CuePackage.json") &&
                 !std::filesystem::exists(projectRoot / std::filesystem::path(first.package->destination) / L"Runtime" /
                                          L"CueGameModule.pdb")))
    {
        return false;
    }
    const std::string firstDestination = first.package->destination;

    const std::uint32_t buildCallsBeforeCancel = buildRunner.calls.load(std::memory_order_acquire);
    constexpr std::string_view cancelledOperation = "09234567-89ab-4cde-8f01-23456789abcd";
    if (!require(
            service->start(make_request(projectRoot.generic_string(), std::string(cancelledOperation), a_assertContext),
                           cue::CMakeConfigureMode::Required, {1U, 0U, 0U}, std::string(k_projectId),
                           *runtimeData.try_value()) &&
            wait_until_completed_call(buildRunner, buildCallsBeforeCancel + 1U) && service->request_cancel() &&
            service->wait_for_package()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot transitionCancelled = service->snapshot();
    if (!require(transitionCancelled.state == cue::package::PackageWorkflowState::Cancelled &&
                 !transitionCancelled.package && transitionCancelled.latestSuccessfulPackage &&
                 transitionCancelled.latestSuccessfulPackage->destination == firstDestination &&
                 !std::filesystem::exists(projectRoot / L"Generated" / L"Packages" / L"Debug" /
                                          std::filesystem::path(cancelledOperation))))
    {
        return false;
    }
    const std::uint32_t publisherCallsAfterTransitionCancel = publisher.calls.load(std::memory_order_acquire);

    buildRunner.mode.store(RunnerMode::Fail, std::memory_order_release);
    if (!require(service->start(make_request(projectRoot.generic_string(), "11234567-89ab-4cde-8f01-23456789abcd",
                                             a_assertContext),
                                cue::CMakeConfigureMode::Required, {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot buildFailed = service->snapshot();
    if (!require(buildFailed.state == cue::package::PackageWorkflowState::Failed && !buildFailed.package &&
                 buildFailed.latestSuccessfulPackage &&
                 buildFailed.latestSuccessfulPackage->destination == firstDestination &&
                 publisher.calls.load(std::memory_order_acquire) == publisherCallsAfterTransitionCancel))
    {
        return false;
    }

    buildRunner.mode.store(RunnerMode::Succeed, std::memory_order_release);
    publisher.invalidPortableExecutable.store(true, std::memory_order_release);
    if (!require(service->retry("16234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot invalidPe = service->snapshot();
    if (!require(invalidPe.state == cue::package::PackageWorkflowState::Failed && !invalidPe.package &&
                 invalidPe.latestSuccessfulPackage &&
                 invalidPe.latestSuccessfulPackage->destination == firstDestination))
    {
        return false;
    }

    publisher.invalidPortableExecutable.store(false, std::memory_order_release);
    const std::uint32_t artifactReadsBeforeResourceLimits = publisher.artifactReadCalls.load(std::memory_order_acquire);
    publisher.oversizedRuntimePeImage.store(true, std::memory_order_release);
    if (!require(service->retry("17234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    if (!require(service->snapshot().state == cue::package::PackageWorkflowState::Failed &&
                 publisher.artifactReadCalls.load(std::memory_order_acquire) == artifactReadsBeforeResourceLimits))
    {
        return false;
    }
    publisher.oversizedRuntimePeImage.store(false, std::memory_order_release);
    publisher.oversizedRuntimePeInventory.store(true, std::memory_order_release);
    if (!require(service->retry("18234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    if (!require(service->snapshot().state == cue::package::PackageWorkflowState::Failed &&
                 publisher.artifactReadCalls.load(std::memory_order_acquire) == artifactReadsBeforeResourceLimits))
    {
        return false;
    }
    publisher.oversizedRuntimePeInventory.store(false, std::memory_order_release);
    publisher.runtimeHostOnlyInventoryOverflow.store(true, std::memory_order_release);
    if (!require(service->retry("19234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    if (!require(service->snapshot().state == cue::package::PackageWorkflowState::Failed &&
                 publisher.artifactReadCalls.load(std::memory_order_acquire) > artifactReadsBeforeResourceLimits))
    {
        return false;
    }
    publisher.runtimeHostOnlyInventoryOverflow.store(false, std::memory_order_release);
    publisher.corruptInventory.store(true, std::memory_order_release);
    if (!require(service->retry("21234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    cue::package::PackageWorkflowSnapshot packageFailed = service->snapshot();
    if (!require(packageFailed.state == cue::package::PackageWorkflowState::Failed && !packageFailed.package &&
                 packageFailed.message.starts_with("Packageの検証または公開に失敗しました") &&
                 packageFailed.latestSuccessfulPackage &&
                 packageFailed.latestSuccessfulPackage->destination == firstDestination))
    {
        return false;
    }

    publisher.corruptInventory.store(false, std::memory_order_release);
    recoveryFilesystem.durabilityFailuresRemaining.store(1U, std::memory_order_release);
    constexpr std::string_view durabilityOperation = "23234567-89ab-4cde-8f01-23456789abcd";
    if (!require(service->retry(std::string(durabilityOperation), {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot durabilityUnknown = service->snapshot();
    if (!require(durabilityUnknown.state == cue::package::PackageWorkflowState::Failed && !durabilityUnknown.package &&
                 durabilityUnknown.publicationDiagnostic &&
                 durabilityUnknown.publicationDiagnostic->stage == cue::package::PackagePublishStage::Publish &&
                 durabilityUnknown.publicationDiagnostic->outcome ==
                     cue::package::PackagePublishOutcome::PublishedButDurabilityUnknown &&
                 durabilityUnknown.publicationDiagnostic->destination ==
                     std::string("Generated/Packages/Debug/") + std::string(durabilityOperation) &&
                 durabilityUnknown.publicationDiagnostic->manifest.projectId == k_projectId &&
                 durabilityUnknown.publicationDiagnostic->manifest.fileCount != 0U &&
                 durabilityUnknown.latestSuccessfulPackage &&
                 durabilityUnknown.latestSuccessfulPackage->destination == firstDestination &&
                 std::filesystem::exists(projectRoot /
                                         std::filesystem::path(durabilityUnknown.publicationDiagnostic->destination) /
                                         L"CuePackage.json")))
    {
        return false;
    }

    recoveryFilesystem.writeFailuresRemaining.store(1U, std::memory_order_release);
    recoveryFilesystem.rollbackFailuresRemaining.store(2U, std::memory_order_release);
    if (!require(service->retry("26234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot recoveryFailed = service->snapshot();
    if (!require(recoveryFailed.state == cue::package::PackageWorkflowState::Failed &&
                 recoveryFailed.recoveryStagingLocator &&
                 std::filesystem::exists(projectRoot / std::filesystem::path(*recoveryFailed.recoveryStagingLocator)) &&
                 recoveryFilesystem.rollbackCalls.load(std::memory_order_acquire) == 2U))
    {
        return false;
    }
    const std::string recoveryStaging = *recoveryFailed.recoveryStagingLocator;

    if (!require(service->retry("31234567-89ab-4cde-8f01-23456789abcd", {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package() && service->run(cue::package::PackageRunMode::SmokeTest) &&
                 service->wait_for_run_completion()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot runSucceeded = service->snapshot();
    if (!require(runSucceeded.state == cue::package::PackageWorkflowState::RunSucceeded &&
                 !runSucceeded.recoveryStagingLocator &&
                 !std::filesystem::exists(projectRoot / std::filesystem::path(recoveryStaging)) &&
                 recoveryFilesystem.rollbackCalls.load(std::memory_order_acquire) == 3U &&
                 runSucceeded.runOutput.size() == 1U &&
                 runRunner.maximumCapturedOutputBytes.load(std::memory_order_acquire) == 4U * 1024U * 1024U))
    {
        return false;
    }

    runRunner.mode.store(RunnerMode::Fail, std::memory_order_release);
    if (!require(service->run(cue::package::PackageRunMode::SmokeTest) && service->wait_for_run_completion()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot runFailed = service->snapshot();
    runRunner.mode.store(RunnerMode::Succeed, std::memory_order_release);
    if (!require(runFailed.state == cue::package::PackageWorkflowState::Failed && runFailed.package &&
                 service->run(cue::package::PackageRunMode::SmokeTest) && service->wait_for_run_completion() &&
                 service->snapshot().state == cue::package::PackageWorkflowState::RunSucceeded))
    {
        return false;
    }

    runRunner.mode.store(RunnerMode::BlockUntilCancelled, std::memory_order_release);
    if (!require(service->run(cue::package::PackageRunMode::Interactive) && wait_until_active(runRunner) &&
                 service->stop() && service->wait_for_run_completion()))
    {
        return false;
    }
    if (!require(service->snapshot().state == cue::package::PackageWorkflowState::PackageReady &&
                 !runRunner.active.load(std::memory_order_acquire) &&
                 runRunner.lastCancellationMode.load(std::memory_order_acquire) ==
                     cue::ChildProcessCancellationMode::Graceful))
    {
        return false;
    }

    runRunner.mode.store(RunnerMode::Succeed, std::memory_order_release);
    recoveryFilesystem.blockPackageManifestRead.store(true, std::memory_order_release);
    recoveryFilesystem.releasePackageManifestRead.store(false, std::memory_order_release);
    const std::uint32_t callsBeforeValidationCancellation = runRunner.calls.load(std::memory_order_acquire);
    if (!require(service->run(cue::package::PackageRunMode::Interactive) &&
                 wait_until_manifest_read(recoveryFilesystem) && service->stop()))
    {
        recoveryFilesystem.releasePackageManifestRead.store(true, std::memory_order_release);
        static_cast<void>(service->wait_for_run_completion());
        return false;
    }
    recoveryFilesystem.releasePackageManifestRead.store(true, std::memory_order_release);
    if (!require(static_cast<bool>(service->wait_for_run_completion())))
    {
        return false;
    }
    recoveryFilesystem.blockPackageManifestRead.store(false, std::memory_order_release);
    if (!require(service->snapshot().state == cue::package::PackageWorkflowState::PackageReady &&
                 runRunner.calls.load(std::memory_order_acquire) == callsBeforeValidationCancellation))
    {
        return false;
    }

    runRunner.mode.store(RunnerMode::BlockUntilCancelled, std::memory_order_release);
    if (!require(service->run(cue::package::PackageRunMode::Interactive) && wait_until_active(runRunner)))
    {
        return false;
    }
    service.reset();
    return require(!runRunner.active.load(std::memory_order_acquire) &&
                   runRunner.lastCancellationMode.load(std::memory_order_acquire) ==
                       cue::ChildProcessCancellationMode::Immediate);
}

/// @brief Release Monolithic Shippingの公開、Run前再検証、Process実行を検証する
[[nodiscard]] bool test_shipping_workflow(const cue::AssertContext &a_assertContext)
{
    if (!require(!cue::BuildProfile::create_shipping_product(
                     cue::BuildConfiguration::Debug, cue::ShippingTrustMode::UnsignedLocal, {}, a_assertContext) &&
                 !cue::BuildProfile::create_shipping_product(
                     cue::BuildConfiguration::Development, cue::ShippingTrustMode::UnsignedLocal, {}, a_assertContext)))
    {
        return false;
    }

    TestDirectory directory;
    const std::filesystem::path projectRoot = directory.path() / L"ShippingProject";
    const std::filesystem::path engineRoot = directory.path() / L"Engine";
    std::filesystem::create_directories(projectRoot);
    std::filesystem::create_directories(engineRoot);

    RunnerState buildRunner;
    RunnerState runRunner;
    PublisherState publisher;
    RecoveryFilesystemState recoveryFilesystem;
    auto build = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(buildRunner),
                                               std::make_unique<MaterializingPublisher>(publisher, a_assertContext),
                                               a_assertContext);
    auto projectFilesystem = cue::create_windows_filesystem_root(projectRoot.generic_string(), a_assertContext);
    auto engineFilesystem = cue::create_windows_filesystem_root(engineRoot.generic_string(), a_assertContext);
    if (!require(build && projectFilesystem && engineFilesystem))
    {
        return false;
    }
    auto guardedProjectFilesystem = std::make_unique<RecoveryFilesystemRoot>(
        std::move(*projectFilesystem.try_value()), recoveryFilesystem, publisher, a_assertContext);
    auto workflow = cue::package::GamePackageWorkflowService::create(
        std::move(*build.try_value()), std::make_unique<MaterializingArtifactReader>(publisher),
        std::move(guardedProjectFilesystem), std::move(*engineFilesystem.try_value()),
        std::make_unique<ControlledRunner>(runRunner), projectRoot.generic_string(), {},
        cue::package::RuntimeHostBuildSource::EngineBinaryRoot, a_assertContext);
    auto runtimeData = make_runtime_data(a_assertContext);
    if (!require(workflow && runtimeData))
    {
        return false;
    }
    std::unique_ptr<cue::package::GamePackageWorkflowService> service = std::move(*workflow.try_value());
    constexpr std::string_view firstOperation = "41234567-89ab-4cde-8f01-23456789abcd";
    if (!require(service->start(
                     make_shipping_request(projectRoot.generic_string(), std::string(firstOperation), a_assertContext),
                     cue::CMakeConfigureMode::Required, {1U, 0U, 0U}, std::string(k_projectId),
                     *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot first = service->snapshot();
    if (!require(first.state == cue::package::PackageWorkflowState::PackageReady && first.package &&
                 first.package->manifest.configuration == cue::BuildConfiguration::Release &&
                 first.package->manifest.executionModel == cue::package::PackageExecutionModel::Monolithic &&
                 first.package->manifest.trustMode == cue::ShippingTrustMode::UnsignedLocal &&
                 !first.package->manifest.publicDistributionReady && first.package->manifest.fileCount == 3U &&
                 first.package->destination ==
                     std::string("Generated/Packages/Shipping/Release/") + std::string(firstOperation) &&
                 first.package->executable.ends_with("/CueGameProduct.exe") &&
                 std::filesystem::exists(projectRoot / std::filesystem::path(first.package->destination) /
                                         L"CueGameProduct.exe") &&
                 !std::filesystem::exists(projectRoot / std::filesystem::path(first.package->destination) /
                                          L"CueGameModule.dll") &&
                 runRunner.calls.load(std::memory_order_acquire) == 0U))
    {
        return false;
    }

    const std::filesystem::path packageRoot = projectRoot / std::filesystem::path(first.package->destination);
    const std::filesystem::path manifestPath = packageRoot / L"CuePackage.json";
    const std::filesystem::path tamperedScene =
        packageRoot / std::filesystem::path(runtimeData.try_value()->startup_scene_data().relative_path());
    std::optional<std::vector<std::byte>> manifestBytes = read_binary_file(manifestPath);
    std::optional<std::vector<std::byte>> sceneBytes = read_binary_file(tamperedScene);
    if (!require(manifestBytes && sceneBytes))
    {
        return false;
    }
    auto manifest = cue::package::parse_package_manifest(
        {reinterpret_cast<const char *>(manifestBytes->data()), manifestBytes->size()}, a_assertContext);
    sceneBytes->push_back(static_cast<std::byte>('\n'));
    auto replacementScene =
        manifest ? cue::package::PackageFilePayload::create(
                       cue::package::PackageFileRole::StartupSceneRuntimeData,
                       std::string(runtimeData.try_value()->startup_scene_data().relative_path()),
                       std::move(*sceneBytes), a_assertContext)
                 : cue::Result<cue::package::PackageFilePayload>::failure(std::move(*manifest.try_error()));
    if (!require(manifest && replacementScene))
    {
        return false;
    }
    std::vector<cue::package::PackageFileEntry> replacementEntries;
    replacementEntries.reserve(manifest.try_value()->files().size());
    for (const cue::package::PackageFileEntry &entry : manifest.try_value()->files())
    {
        replacementEntries.push_back(entry.role() == cue::package::PackageFileRole::StartupSceneRuntimeData
                                         ? replacementScene.try_value()->entry()
                                         : entry);
    }
    const std::optional<std::string_view> publisherKeyId = manifest.try_value()->publisher_key_id();
    const std::optional<std::string_view> signaturePath = manifest.try_value()->manifest_signature_path();
    auto replacementManifest = cue::package::PackageManifest::create_monolithic(
        std::string(manifest.try_value()->project_id()), manifest.try_value()->engine_version(),
        manifest.try_value()->configuration(), std::string(manifest.try_value()->startup_scene_asset_id()),
        std::string(manifest.try_value()->startup_scene_runtime_data_path()), *manifest.try_value()->trust_mode(),
        publisherKeyId ? std::optional<std::string>(std::string(*publisherKeyId)) : std::nullopt,
        signaturePath ? std::optional<std::string>(std::string(*signaturePath)) : std::nullopt,
        std::move(replacementEntries), a_assertContext);
    auto replacementManifestText =
        replacementManifest
            ? cue::package::serialize_package_manifest(*replacementManifest.try_value(), a_assertContext)
            : cue::Result<std::string>::failure(std::move(*replacementManifest.try_error()));
    const std::span<const char> replacementManifestCharacters(
        replacementManifestText ? replacementManifestText.try_value()->data() : nullptr,
        replacementManifestText ? replacementManifestText.try_value()->size() : 0U);
    if (!require(replacementManifestText && write_binary_file(tamperedScene, replacementScene.try_value()->bytes()) &&
                 write_binary_file(manifestPath, std::as_bytes(replacementManifestCharacters))))
    {
        return false;
    }
    if (!require(service->run(cue::package::PackageRunMode::SmokeTest) && service->wait_for_run_completion() &&
                 service->snapshot().state == cue::package::PackageWorkflowState::Failed &&
                 service->snapshot().message.starts_with("Packageの実行前検証に失敗しました") &&
                 runRunner.calls.load(std::memory_order_acquire) == 0U))
    {
        return false;
    }

    constexpr std::string_view retryOperation = "51234567-89ab-4cde-8f01-23456789abcd";
    if (!require(service->retry(std::string(retryOperation), {1U, 0U, 0U}, std::string(k_projectId),
                                *runtimeData.try_value()) &&
                 service->wait_for_package()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot ready = service->snapshot();
    if (!require(ready.package.has_value()))
    {
        return false;
    }
    const std::filesystem::path completedRoot = projectRoot / std::filesystem::path(ready.package->destination);
    runRunner.mutationProbeRelativePaths = {"CuePackage.json"};
    runRunner.mutationProbeDirectories = {
        completedRoot,
        completedRoot.parent_path(),
        completedRoot.parent_path().parent_path(),
        completedRoot.parent_path().parent_path().parent_path(),
        completedRoot.parent_path().parent_path().parent_path().parent_path(),
        projectRoot,
    };
    for (const cue::package::PackageFileEntry &entry : ready.package->manifest.files)
    {
        runRunner.mutationProbeRelativePaths.emplace_back(entry.relative_path());
        const std::filesystem::path parent = std::filesystem::path(entry.relative_path()).parent_path();
        if (!parent.empty())
        {
            runRunner.mutationProbeDirectories.emplace_back(completedRoot / parent);
        }
    }
    runRunner.probePackageMutation.store(true, std::memory_order_release);
    if (!require(service->run(cue::package::PackageRunMode::SmokeTest) && service->wait_for_run_completion()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot completed = service->snapshot();
    bool fileLeasesReleased = can_open_file_for_mutation(completedRoot / L"CuePackage.json");
    for (const cue::package::PackageFileEntry &entry : ready.package->manifest.files)
    {
        fileLeasesReleased = fileLeasesReleased &&
                             can_open_file_for_mutation(completedRoot / std::filesystem::path(entry.relative_path()));
    }
    bool renameLeasesReleased = true;
    for (const std::filesystem::path &path : runRunner.mutationProbeDirectories)
    {
        if (path != projectRoot)
        {
            renameLeasesReleased = renameLeasesReleased && can_rename_directory_round_trip(path);
        }
    }
    if (!require(completed.state == cue::package::PackageWorkflowState::RunSucceeded && completed.package &&
                 completed.package->operationId == retryOperation &&
                 completed.package->manifest.executionModel == cue::package::PackageExecutionModel::Monolithic &&
                 runRunner.calls.load(std::memory_order_acquire) == 1U))
    {
        return false;
    }
    if (!require(runRunner.packageMutationBlocked.load(std::memory_order_acquire)))
    {
        return false;
    }
    if (!require(runRunner.packageDirectoryRenameBlocked.load(std::memory_order_acquire)))
    {
        return false;
    }
    if (!require(fileLeasesReleased && renameLeasesReleased))
    {
        return false;
    }

    runRunner.probePackageMutation.store(false, std::memory_order_release);
    runRunner.probePackageEntryCreation.store(true, std::memory_order_release);
    runRunner.mode.store(RunnerMode::BlockUntilCancelled, std::memory_order_release);
    if (!require(service->run(cue::package::PackageRunMode::SmokeTest) && service->wait_for_run_completion()))
    {
        return false;
    }
    const cue::package::PackageWorkflowSnapshot changedDuringRun = service->snapshot();
    return require(runRunner.packageEntryCreationSucceeded.load(std::memory_order_acquire) &&
                   runRunner.lastCancellationMode.load(std::memory_order_acquire) ==
                       cue::ChildProcessCancellationMode::Immediate &&
                   changedDuringRun.state == cue::package::PackageWorkflowState::Failed &&
                   changedDuringRun.message.find("Package Treeの変更") != std::string_view::npos &&
                   !std::filesystem::exists(completedRoot / L"transient-unlisted.dll"));
}
} // namespace

/// @brief Build・Package・Run CoordinatorをUIなしで検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_workflow(assertContext) && test_shipping_workflow(assertContext) ? 0 : 1;
}
