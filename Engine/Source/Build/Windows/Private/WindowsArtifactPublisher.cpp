#include <Cue/Build/Windows/WindowsArtifactPublisher.h>
#include <Cue/Build/Windows/WindowsProductSecurity.h>

#include "WindowsProductSecurityInternal.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Platform/Windows/WindowsProcess.h>
#include <Cue/Project/Descriptor.h>

#include <EngineBuildMetadata.h>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_hashBlockBytes = 64U * 1024U;
constexpr std::size_t k_sha256Bytes = 32U;
constexpr std::size_t k_maximumSourceInventoryFiles = 8192U;
constexpr std::size_t k_maximumSourceInventoryBytes = 32U * 1024U * 1024U;
constexpr std::uint64_t k_maximumSourceInputBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t k_maximumToolchainEvidenceBytes = 2ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t k_maximumRuntimeMetadataBytes = 2ULL * 1024ULL * 1024ULL;
/// @brief WindowsのExtended-length Path上限で128 Entryを直列化しても超えないCurrent読込上限
constexpr std::uint64_t k_maximumCurrentManifestBytes = 32U * 1024U * 1024U;
constexpr DWORD k_lockRetryMilliseconds = 10U;
constexpr std::string_view k_moduleProbeCompletionMarker = "CueGameModuleProbe:v1\n";
constexpr std::string_view k_productProbeCompletionMarker = "CueGameProductProbe:v1\n";

[[nodiscard]] std::string_view configuration_name(cue::BuildConfiguration a_configuration) noexcept;

/// @brief Target別Artifactの固定File名とPDB要件
struct ArtifactLayout final
{
    std::string_view payload;
    std::string_view metadata;
    std::string_view symbol;
    bool symbolRequired = false;
};

/// @brief Build前後で比較するSource入力集合のCanonical Identity
struct SourceInventoryIdentity final
{
    std::size_t fileCount = 0U;
    std::string hash;

    [[nodiscard]] bool operator==(const SourceInventoryIdentity &) const noexcept = default;
};

/// @brief Shipping Productへ記録しBuild前後で照合するFirst-party Provenance
struct ShippingBuildProvenance final
{
    std::string engineCommit;
    bool engineDirty = false;
    SourceInventoryIdentity engineSource;
    SourceInventoryIdentity gameSource;
    std::string vcpkgManifestHash;
    std::string vcpkgBaselineHash;
    std::string distributionSourceInventoryHash;
    std::string publisherBuildIdentityDigest;

    [[nodiscard]] bool operator==(const ShippingBuildProvenance &) const noexcept = default;
};

/// @brief Project Binary Treeから再検証した実使用Toolchain Identity
struct ShippingToolchainIdentity final
{
    std::string cmakeVersion;
    std::string cmakeGenerator;
    std::string platformToolset;
    std::string msvcToolsetVersion;
    std::string compilerFileVersion;
    std::string compilerSha256;
    std::string windowsSdkVersion;
};

/// @brief Build PlanからTarget別Artifact Layoutを返す
[[nodiscard]] ArtifactLayout artifact_layout(const cue::BuildPlan &a_plan) noexcept
{
    if (a_plan.profile().target() == cue::BuildTarget::ShippingProduct)
    {
        return {"CueGameProduct.exe", "CueGameProduct.metadata.json", "CueGameProduct.pdb", false};
    }
    return {"CueGameModule.dll", "CueGameModule.metadata.json", "CueGameModule.pdb",
            a_plan.profile().configuration() != cue::BuildConfiguration::Release};
}

/// @brief Build Profileに対応するArtifact Storeの絶対Pathを返す
[[nodiscard]] std::filesystem::path artifact_store_path(const std::filesystem::path &a_projectRoot,
                                                        const cue::BuildProfile &a_profile)
{
    std::filesystem::path store = a_projectRoot / "Generated" / "Artifacts";
    store /= a_profile.target() == cue::BuildTarget::GameModule ? "GameModule" : "ShippingProduct";
    store /= configuration_name(a_profile.configuration());
    if (a_profile.target() == cue::BuildTarget::GameModule)
    {
        store /= "modular";
    }
    else if (a_profile.minimum_trust_mode() == cue::ShippingTrustMode::UnsignedLocal)
    {
        store /= "unsigned-local";
    }
    else
    {
        std::string variant("publisher-");
        variant.append(a_profile.publisher_key_id().substr(0U, 16U));
        store /= variant;
    }
    return store.lexically_normal();
}

/// @brief Windows Artifact処理中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_artifact_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Windows build artifact publication failed unexpectedly");
    std::abort();
}

/// @brief Windows Artifact固有の回復可能Errorを構築する
[[nodiscard]] cue::Error make_error(const cue::AssertContext &a_assertContext, cue::WindowsBuildArtifactError a_code,
                                    std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Windows.Artifact",
                                                 static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Platform非依存なPublisher Lock待機Timeoutを構築する
[[nodiscard]] cue::Error make_lock_timeout_error(const cue::AssertContext &a_assertContext,
                                                 std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Publisher",
                               static_cast<std::int64_t>(cue::BuildArtifactPublisherError::LockWaitTimedOut));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Artifact ProbeのTimeoutをServiceが識別できるErrorへ変換する
[[nodiscard]] cue::Error make_artifact_probe_timeout_error(const cue::AssertContext &a_assertContext,
                                                           std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Publisher",
                               static_cast<std::int64_t>(cue::BuildArtifactPublisherError::ModuleProbeTimedOut));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Win32 Codeを保持するWindows Artifact Errorを構築する
[[nodiscard]] cue::Error make_windows_error(const cue::AssertContext &a_assertContext,
                                            cue::WindowsBuildArtifactError a_code, DWORD a_nativeCode,
                                            std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Windows.Artifact",
                                                 static_cast<std::int64_t>(a_code));
    cue::NativeError native =
        cue::NativeError::create(a_assertContext.fatal_handler(), "Win32", static_cast<std::int64_t>(a_nativeCode));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary, std::move(native));
}

/// @brief NTSTATUSを保持するWindows Artifact Errorを構築する
[[nodiscard]] cue::Error make_nt_error(const cue::AssertContext &a_assertContext, cue::WindowsBuildArtifactError a_code,
                                       NTSTATUS a_nativeCode, std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Windows.Artifact",
                                                 static_cast<std::int64_t>(a_code));
    cue::NativeError native =
        cue::NativeError::create(a_assertContext.fatal_handler(), "NTSTATUS", static_cast<std::int64_t>(a_nativeCode));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary, std::move(native));
}

/// @brief HANDLEを一意所有し全経路でCloseする
class UniqueHandle final
{
  public:
    /// @brief 無効Handleとして構築する
    UniqueHandle() noexcept = default;
    /// @brief Native Handleの一意所有権を取得する
    explicit UniqueHandle(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    /// @brief Native Handle所有権のCopy構築を禁止する
    UniqueHandle(const UniqueHandle &) = delete;
    /// @brief Native Handle所有権のCopy代入を禁止する
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    /// @brief Native Handleを移動する
    UniqueHandle(UniqueHandle &&a_other) noexcept : m_handle(std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE))
    {
    }
    /// @brief 現在のHandleをCloseしてNative Handleを移動する
    UniqueHandle &operator=(UniqueHandle &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset();
            m_handle = std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    /// @brief 所有Native HandleをCloseする
    ~UniqueHandle()
    {
        reset();
    }
    /// @brief Native API呼出し用Handleを返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }
    /// @brief 有効なNative Handleを所有しているか返す
    [[nodiscard]] bool is_valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

  private:
    /// @brief 所有HandleがあればCloseして無効化する
    void reset() noexcept
    {
        if (is_valid())
        {
            CloseHandle(m_handle);
        }
        m_handle = INVALID_HANDLE_VALUE;
    }

    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

/// @brief Filesystem操作中にDirectory ChainのRenameとReparse Point差替えを阻止するHandle集合
class DirectoryChainGuard final
{
  public:
    /// @brief 検証済みDirectory Handle集合を所有する
    explicit DirectoryChainGuard(std::vector<UniqueHandle> a_handles) noexcept : m_handles(std::move(a_handles))
    {
    }
    /// @brief Directory Handle集合の複製を禁止する
    DirectoryChainGuard(const DirectoryChainGuard &) = delete;
    /// @brief Directory Handle集合の複製代入を禁止する
    DirectoryChainGuard &operator=(const DirectoryChainGuard &) = delete;
    /// @brief Directory Handle集合の所有権を移動する
    DirectoryChainGuard(DirectoryChainGuard &&) noexcept = default;
    /// @brief Directory Handle集合の所有権を移動代入する
    DirectoryChainGuard &operator=(DirectoryChainGuard &&) noexcept = default;
    /// @brief 全HandleをCloseしてDirectory Chainの差替え禁止を解除する
    ~DirectoryChainGuard() = default;
    /// @brief 検証済みLeaf DirectoryのNative Handleを返す
    [[nodiscard]] HANDLE leaf_handle() const noexcept
    {
        return m_handles.back().get();
    }

  private:
    std::vector<UniqueHandle> m_handles;
};

/// @brief Byte Range Lockと検証済みDirectory Chainを同じ寿命で所有する
class GuardedByteRangeLock final
{
  public:
    /// @brief Lock済みHandleとDirectory Chain Guardを所有する
    GuardedByteRangeLock(UniqueHandle a_handle, DirectoryChainGuard a_parentGuard) noexcept
        : m_handle(std::move(a_handle)), m_parentGuard(std::move(a_parentGuard))
    {
    }
    /// @brief Guard付きLockの複製を禁止する
    GuardedByteRangeLock(const GuardedByteRangeLock &) = delete;
    /// @brief Guard付きLockの複製代入を禁止する
    GuardedByteRangeLock &operator=(const GuardedByteRangeLock &) = delete;
    /// @brief Guard付きLockの所有権を移動する
    GuardedByteRangeLock(GuardedByteRangeLock &&) noexcept = default;
    /// @brief Guard付きLockの所有権を移動代入する
    GuardedByteRangeLock &operator=(GuardedByteRangeLock &&) noexcept = default;
    /// @brief Lock HandleとDirectory Chain Guardを解放する
    ~GuardedByteRangeLock() = default;
    /// @brief Byte Range操作用Native Handleを返す
    [[nodiscard]] HANDLE handle() const noexcept
    {
        return m_handle.get();
    }

  private:
    UniqueHandle m_handle;
    DirectoryChainGuard m_parentGuard;
};

/// @brief Offset 0、Length 1のWindows File LockをHandle寿命へ束ねる
class ByteRangeLease
{
  public:
    /// @brief Guard付きLockの一意所有権を取得する
    explicit ByteRangeLease(GuardedByteRangeLock a_lock) noexcept : m_lock(std::move(a_lock))
    {
    }
    /// @brief Byte Range Lock所有権のCopy構築を禁止する
    ByteRangeLease(const ByteRangeLease &) = delete;
    /// @brief Byte Range Lock所有権のCopy代入を禁止する
    ByteRangeLease &operator=(const ByteRangeLease &) = delete;
    /// @brief Byte RangeをUnlockしてHandleをCloseする
    virtual ~ByteRangeLease()
    {
        OVERLAPPED overlap{};
        static_cast<void>(UnlockFileEx(m_lock.handle(), 0U, 1U, 0U, &overlap));
    }

  private:
    GuardedByteRangeLock m_lock;
};

/// @brief GameBuildServiceへ渡すWindows Build Workspace Lease
class WindowsBuildWorkspaceLease final : public cue::BuildWorkspaceLease, public ByteRangeLease
{
  public:
    /// @brief Lock済みBuild FileとPlan Keyを所有する
    WindowsBuildWorkspaceLease(GuardedByteRangeLock a_lock, DirectoryChainGuard a_workspaceGuard,
                               std::string a_workspaceKey,
                               std::optional<ShippingBuildProvenance> a_shippingProvenance) noexcept
        : ByteRangeLease(std::move(a_lock)), m_workspaceGuard(std::move(a_workspaceGuard)),
          m_workspaceKey(std::move(a_workspaceKey)), m_shippingProvenance(std::move(a_shippingProvenance))
    {
    }
    /// @brief Build Lockを解放する
    ~WindowsBuildWorkspaceLease() override = default;
    /// @brief Leaseが保護するPlan Keyと一致するか返す
    [[nodiscard]] bool matches(std::string_view a_workspaceKey) const noexcept
    {
        return m_workspaceKey == a_workspaceKey;
    }
    /// @brief Shipping Build開始時に固定したSource Provenanceを返す
    [[nodiscard]] const std::optional<ShippingBuildProvenance> &shipping_provenance() const noexcept
    {
        return m_shippingProvenance;
    }

  private:
    DirectoryChainGuard m_workspaceGuard;
    std::string m_workspaceKey;
    std::optional<ShippingBuildProvenance> m_shippingProvenance;
};

/// @brief UTF-8 PathをWindows Filesystem Pathへ変換する
[[nodiscard]] std::optional<std::filesystem::path> to_path(std::string_view a_path)
{
    try
    {
        return std::filesystem::path(
            std::u8string_view(reinterpret_cast<const char8_t *>(a_path.data()), a_path.size()));
    }
    catch (const std::filesystem::filesystem_error &)
    {
        return std::nullopt;
    }
}

/// @brief Project RootとPlan Rootが同じWindows Pathを表すか比較する
[[nodiscard]] bool same_root(const std::filesystem::path &a_left, std::string_view a_right)
{
    const std::optional<std::filesystem::path> right = to_path(a_right);
    if (!right)
    {
        return false;
    }
    std::error_code leftError;
    std::error_code rightError;
    const std::filesystem::path leftCanonical = std::filesystem::weakly_canonical(a_left, leftError);
    const std::filesystem::path rightCanonical = std::filesystem::weakly_canonical(*right, rightError);
    return !leftError && !rightError && _wcsicmp(leftCanonical.c_str(), rightCanonical.c_str()) == 0;
}

/// @brief Build Configurationを安定したDirectory／Manifest名へ変換する
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

/// @brief Build TargetをCurrent／Metadataの安定名へ変換する
[[nodiscard]] std::string_view target_name(cue::BuildTarget a_target) noexcept
{
    switch (a_target)
    {
    case cue::BuildTarget::GameModule:
        return "GameModule";
    case cue::BuildTarget::ShippingProduct:
        return "ShippingProduct";
    }
    return {};
}

/// @brief Shipping Trust ModeをCurrent／Metadataの安定名へ変換する
[[nodiscard]] std::string_view trust_mode_name(cue::ShippingTrustMode a_mode) noexcept
{
    switch (a_mode)
    {
    case cue::ShippingTrustMode::UnsignedLocal:
        return "UnsignedLocal";
    case cue::ShippingTrustMode::PublisherSigned:
        return "PublisherSigned";
    }
    return {};
}

/// @brief Product署名状態をMetadataの安定名へ変換する
[[nodiscard]] std::string_view product_signature_status_name(cue::WindowsProductSignatureStatus a_status) noexcept
{
    switch (a_status)
    {
    case cue::WindowsProductSignatureStatus::Trusted:
        return "Trusted";
    case cue::WindowsProductSignatureStatus::Unsigned:
        return "Unsigned";
    case cue::WindowsProductSignatureStatus::InvalidSignature:
        return "InvalidSignature";
    case cue::WindowsProductSignatureStatus::CertificateExpired:
        return "CertificateExpired";
    case cue::WindowsProductSignatureStatus::CertificateRevoked:
        return "CertificateRevoked";
    case cue::WindowsProductSignatureStatus::ChainInvalid:
        return "ChainInvalid";
    case cue::WindowsProductSignatureStatus::VerificationUnavailable:
        return "VerificationUnavailable";
    }
    return {};
}

/// @brief Product Artifact単体の配布到達点をMetadataの安定名へ変換する
[[nodiscard]] std::string_view product_distribution_status_name(cue::WindowsProductDistributionStatus a_status) noexcept
{
    switch (a_status)
    {
    case cue::WindowsProductDistributionStatus::LocalExecutionOnly:
        return "LocalExecutionOnly";
    case cue::WindowsProductDistributionStatus::PublisherVerifiedArtifact:
        return "PublisherVerifiedArtifact";
    }
    return {};
}

/// @brief Artifact File用途をCurrentの安定名へ変換する
[[nodiscard]] std::string_view file_purpose_name(cue::BuildArtifactFilePurpose a_purpose) noexcept
{
    switch (a_purpose)
    {
    case cue::BuildArtifactFilePurpose::DistributionPayload:
        return "DistributionPayload";
    case cue::BuildArtifactFilePurpose::RuntimeMetadata:
        return "RuntimeMetadata";
    case cue::BuildArtifactFilePurpose::DevelopmentSymbol:
        return "DevelopmentSymbol";
    case cue::BuildArtifactFilePurpose::Unspecified:
        break;
    }
    return {};
}

/// @brief Filesystem PathをEngine内部のUTF-8表示へ変換する
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &a_path)
{
    const std::u8string text = a_path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

/// @brief Artifact DirectoryがTarget別許可File集合と完全一致するか検証する
[[nodiscard]] cue::Result<void> validate_artifact_directory_contents(const std::filesystem::path &a_directory,
                                                                     const ArtifactLayout &a_layout, bool a_hasSymbol,
                                                                     const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code iteratorError;
    std::size_t fileCount = 0U;
    for (std::filesystem::directory_iterator iterator(a_directory, iteratorError), end;
         !iteratorError && iterator != end; iterator.increment(iteratorError))
    {
        const std::filesystem::directory_entry &entry = *iterator;
        std::error_code statusError;
        const std::filesystem::file_status status = entry.symlink_status(statusError);
        const std::string name = path_to_utf8(entry.path().filename());
        const bool allowedName =
            name == a_layout.payload || name == a_layout.metadata || (a_hasSymbol && name == a_layout.symbol);
        if (statusError || !std::filesystem::is_regular_file(status) || !allowedName)
        {
            return cue::Result<void>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                           "Artifact directory contains an unexpected or non-regular entry"));
        }
        ++fileCount;
    }
    const std::size_t expectedCount = a_hasSymbol ? 3U : 2U;
    if (iteratorError || fileCount != expectedCount)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                               iteratorError ? static_cast<DWORD>(iteratorError.value()) : ERROR_FILE_INVALID,
                               "Artifact directory file inventory is incomplete or could not be enumerated"));
    }
    return cue::Result<void>::success();
}

/// @brief Absolute Windows PathをExtended-length形式へ変換する
[[nodiscard]] std::filesystem::path native_path(const std::filesystem::path &a_path)
{
    std::filesystem::path preferred = a_path;
    preferred.make_preferred();
    const std::wstring &value = preferred.native();
    if (value.starts_with(L"\\\\?\\"))
    {
        return preferred;
    }
    if (value.starts_with(L"\\\\"))
    {
        std::wstring extended = L"\\\\?\\UNC\\";
        extended.append(value.substr(2U));
        return std::filesystem::path(std::move(extended));
    }
    std::wstring extended = L"\\\\?\\";
    extended.append(value);
    return std::filesystem::path(std::move(extended));
}

/// @brief Directory ComponentをReparse Pointを追跡せず検査し存在有無を返す
[[nodiscard]] cue::Result<bool> inspect_directory_component(const std::filesystem::path &a_path,
                                                            cue::WindowsBuildArtifactError a_code,
                                                            const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path inspected = native_path(a_path);
    UniqueHandle handle(CreateFileW(inspected.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.is_valid())
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        {
            return cue::Result<bool>::success(false);
        }
        return cue::Result<bool>::failure(
            make_windows_error(a_assertContext, a_code, code, "Artifact directory component could not be inspected"));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle.get(), &information) == FALSE)
    {
        return cue::Result<bool>::failure(make_windows_error(a_assertContext, a_code, GetLastError(),
                                                             "Artifact directory attributes could not be read"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return cue::Result<bool>::failure(make_windows_error(a_assertContext, a_code, ERROR_REPARSE_TAG_MISMATCH,
                                                             "Artifact directory contains a reparse point"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)
    {
        return cue::Result<bool>::failure(
            make_windows_error(a_assertContext, a_code, ERROR_DIRECTORY, "Artifact path component is not a directory"));
    }
    return cue::Result<bool>::success(true);
}

/// @brief RootからDirectoryまでの既存ComponentがReparse Pointを含まないことを検証する
[[nodiscard]] cue::Result<void> validate_directory_chain(const std::filesystem::path &a_root,
                                                         const std::filesystem::path &a_directory,
                                                         cue::WindowsBuildArtifactError a_code,
                                                         const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path root = a_root.lexically_normal();
    const std::filesystem::path directory = a_directory.lexically_normal();
    const std::filesystem::path relative = directory.lexically_relative(root);
    if (relative.empty() || relative.is_absolute())
    {
        return cue::Result<void>::failure(
            make_error(a_assertContext, a_code, "Artifact directory is outside the Project Root"));
    }

    cue::Result<bool> rootState = inspect_directory_component(root, a_code, a_assertContext);
    if (!rootState)
    {
        return cue::Result<void>::failure(std::move(*rootState.try_error()));
    }
    if (!*rootState.try_value())
    {
        return cue::Result<void>::failure(
            make_error(a_assertContext, a_code, "Artifact Project Root no longer exists"));
    }

    std::filesystem::path current = root;
    for (const std::filesystem::path &component : relative)
    {
        if (component == ".")
        {
            continue;
        }
        if (component == "..")
        {
            return cue::Result<void>::failure(
                make_error(a_assertContext, a_code, "Artifact directory escaped the Project Root"));
        }
        current /= component;
        cue::Result<bool> state = inspect_directory_component(current, a_code, a_assertContext);
        if (!state)
        {
            return cue::Result<void>::failure(std::move(*state.try_error()));
        }
        if (!*state.try_value())
        {
            return cue::Result<void>::success();
        }
    }
    return cue::Result<void>::success();
}

/// @brief Rootから既存Directoryまでを追跡せず開き操作完了まで差替えを阻止する
[[nodiscard]] cue::Result<DirectoryChainGuard> acquire_directory_chain_guard(
    const std::filesystem::path &a_root, const std::filesystem::path &a_directory,
    cue::WindowsBuildArtifactError a_code, const cue::AssertContext &a_assertContext,
    bool a_requestLeafDeleteAccess = false) noexcept
{
    const std::filesystem::path root = a_root.lexically_normal();
    const std::filesystem::path directory = a_directory.lexically_normal();
    const std::filesystem::path relative = directory.lexically_relative(root);
    if (relative.empty() || relative.is_absolute())
    {
        return cue::Result<DirectoryChainGuard>::failure(
            make_error(a_assertContext, a_code, "Artifact directory is outside the Project Root"));
    }

    std::vector<UniqueHandle> handles;
    std::filesystem::path current = root;
    /// @brief 一Directoryを追跡せず開いて検証しGuard集合へ追加する
    const auto openComponent = [&](const std::filesystem::path &a_path, bool a_isLeaf) -> cue::Result<void>
    {
        const std::filesystem::path inspected = native_path(a_path);
        const DWORD desiredAccess =
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | (a_requestLeafDeleteAccess && a_isLeaf ? DELETE : 0U);
        const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                            (a_requestLeafDeleteAccess && a_isLeaf ? FILE_FLAG_WRITE_THROUGH : 0U);
        UniqueHandle handle(CreateFileW(inspected.c_str(), desiredAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, flags, nullptr));
        if (!handle.is_valid())
        {
            return cue::Result<void>::failure(make_windows_error(a_assertContext, a_code, GetLastError(),
                                                                 "Artifact directory guard could not be acquired"));
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(handle.get(), &information) == FALSE)
        {
            return cue::Result<void>::failure(make_windows_error(
                a_assertContext, a_code, GetLastError(), "Artifact directory guard attributes could not be read"));
        }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        {
            return cue::Result<void>::failure(make_windows_error(a_assertContext, a_code, ERROR_REPARSE_TAG_MISMATCH,
                                                                 "Artifact directory guard found a reparse point"));
        }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)
        {
            return cue::Result<void>::failure(make_windows_error(a_assertContext, a_code, ERROR_DIRECTORY,
                                                                 "Artifact directory guard found a non-directory"));
        }
        handles.push_back(std::move(handle));
        return cue::Result<void>::success();
    };

    cue::Result<void> rootOpened = openComponent(current, current == directory);
    if (!rootOpened)
    {
        return cue::Result<DirectoryChainGuard>::failure(std::move(*rootOpened.try_error()));
    }
    for (const std::filesystem::path &component : relative)
    {
        if (component == ".")
        {
            continue;
        }
        if (component == "..")
        {
            return cue::Result<DirectoryChainGuard>::failure(
                make_error(a_assertContext, a_code, "Artifact directory escaped the Project Root"));
        }
        current /= component;
        cue::Result<void> opened = openComponent(current, current == directory);
        if (!opened)
        {
            return cue::Result<DirectoryChainGuard>::failure(std::move(*opened.try_error()));
        }
    }
    return cue::Result<DirectoryChainGuard>::success(DirectoryChainGuard(std::move(handles)));
}

/// @brief 二つのNative Handleが同じFilesystem Objectを参照するか判定する
[[nodiscard]] bool has_same_file_identity(HANDLE a_left, HANDLE a_right) noexcept
{
    BY_HANDLE_FILE_INFORMATION left{};
    BY_HANDLE_FILE_INFORMATION right{};
    return GetFileInformationByHandle(a_left, &left) != FALSE && GetFileInformationByHandle(a_right, &right) != FALSE &&
           left.dwVolumeSerialNumber == right.dwVolumeSerialNumber && left.nFileIndexHigh == right.nFileIndexHigh &&
           left.nFileIndexLow == right.nFileIndexLow;
}

/// @brief Write-through Handleで固定中のCandidateを同一IdentityのままVersionへRenameする
[[nodiscard]] cue::Result<void> rename_guarded_directory(DirectoryChainGuard &a_guard,
                                                         const std::filesystem::path &a_source,
                                                         const std::filesystem::path &a_destination,
                                                         const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        const std::wstring destination = native_path(a_destination).native();
        const std::size_t fileNameBytes = destination.size() * sizeof(wchar_t);
        // FileNameLengthからは除外するが、可変長Bufferには明示的なNUL終端領域を確保する。
        const std::size_t byteSize = offsetof(FILE_RENAME_INFO, FileName) + fileNameBytes + sizeof(wchar_t);
        std::vector<std::uint64_t> storage((byteSize + sizeof(std::uint64_t) - 1U) / sizeof(std::uint64_t), 0U);
        auto *information = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
        information->ReplaceIfExists = FALSE;
        information->RootDirectory = nullptr;
        information->FileNameLength = static_cast<DWORD>(fileNameBytes);
        std::memcpy(information->FileName, destination.data(), information->FileNameLength);
        if (SetFileInformationByHandle(a_guard.leaf_handle(), FileRenameInfo, information,
                                       static_cast<DWORD>(byteSize)) == FALSE)
        {
            const DWORD renameCode = GetLastError();
            UniqueHandle destinationVisible(
                CreateFileW(destination.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            const std::wstring source = native_path(a_source).native();
            UniqueHandle sourceVisible(CreateFileW(source.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                   OPEN_EXISTING,
                                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            const bool isDestinationIdentity = destinationVisible.is_valid() &&
                                               has_same_file_identity(a_guard.leaf_handle(), destinationVisible.get());
            const bool isSourceIdentity =
                sourceVisible.is_valid() && has_same_file_identity(a_guard.leaf_handle(), sourceVisible.get());
            const cue::WindowsBuildArtifactError error =
                isSourceIdentity && !isDestinationIdentity
                    ? cue::WindowsBuildArtifactError::CandidateInvalid
                    : cue::WindowsBuildArtifactError::ArtifactVersionDurabilityUnknown;
            return cue::Result<void>::failure(make_windows_error(
                a_assertContext, error, renameCode,
                error == cue::WindowsBuildArtifactError::ArtifactVersionDurabilityUnknown
                    ? "Guarded write-through rename completion could not be classified from both namespaces"
                    : "Artifact Version could not be published through the guarded write-through Candidate handle"));
        }
        return cue::Result<void>::success();
    }
    catch (...)
    {
        terminate_artifact_exception(a_assertContext);
    }
}

/// @brief Operation所有Candidateの直下Regular Fileだけを削除しLeaf DirectoryをHandle経由で削除予約する
[[nodiscard]] cue::Result<void> delete_guarded_candidate(DirectoryChainGuard &a_guard,
                                                         const std::filesystem::path &a_candidate,
                                                         const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code iteratorError;
    std::size_t entryCount = 0U;
    for (std::filesystem::directory_iterator iterator(a_candidate, iteratorError), end;
         !iteratorError && iterator != end; iterator.increment(iteratorError))
    {
        if (++entryCount > 16U)
        {
            return cue::Result<void>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                           "Unpublished Candidate contains too many entries for guarded rollback"));
        }
        std::error_code statusError;
        const std::filesystem::file_status status = iterator->symlink_status(statusError);
        if (statusError || !std::filesystem::is_regular_file(status))
        {
            return cue::Result<void>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                           "Unpublished Candidate contains an entry that cannot be removed safely"));
        }
        const std::filesystem::path path = native_path(iterator->path());
        if (DeleteFileW(path.c_str()) == FALSE)
        {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
            {
                return cue::Result<void>::failure(
                    make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, code,
                                       "Unpublished Candidate file could not be removed"));
            }
        }
    }
    if (iteratorError)
    {
        return cue::Result<void>::failure(make_windows_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
            static_cast<DWORD>(iteratorError.value()), "Unpublished Candidate could not be enumerated"));
    }
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (SetFileInformationByHandle(a_guard.leaf_handle(), FileDispositionInfo, &disposition, sizeof(disposition)) ==
        FALSE)
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, GetLastError(),
                               "Unpublished Candidate directory could not be removed through its guarded handle"));
    }
    return cue::Result<void>::success();
}

/// @brief 現在のEngine Processと同じDirectoryを返す
[[nodiscard]] std::optional<std::filesystem::path> current_process_directory()
{
    std::vector<wchar_t> path(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size())
    {
        return std::nullopt;
    }
    return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
}

/// @brief Engine VersionをMetadata用major.minor.patchへ変換する
[[nodiscard]] std::string version_text(const cue::EngineVersion &a_version)
{
    return std::to_string(a_version.major) + "." + std::to_string(a_version.minor) + "." +
           std::to_string(a_version.patch);
}

/// @brief Directoryを不足分だけ作成し回復可能なFilesystem Errorへ変換する
[[nodiscard]] cue::Result<void> ensure_directory(const std::filesystem::path &a_root,
                                                 const std::filesystem::path &a_directory,
                                                 cue::WindowsBuildArtifactError a_code,
                                                 const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path root = a_root.lexically_normal();
    const std::filesystem::path directory = a_directory.lexically_normal();
    const std::filesystem::path relative = directory.lexically_relative(root);
    if (relative.empty() || relative.is_absolute())
    {
        return cue::Result<void>::failure(
            make_error(a_assertContext, a_code, "Artifact directory is outside the Project Root"));
    }

    std::filesystem::path current = root;
    cue::Result<bool> rootState = inspect_directory_component(current, a_code, a_assertContext);
    if (!rootState)
    {
        return cue::Result<void>::failure(std::move(*rootState.try_error()));
    }
    if (!*rootState.try_value())
    {
        return cue::Result<void>::failure(
            make_error(a_assertContext, a_code, "Artifact Project Root no longer exists"));
    }

    for (const std::filesystem::path &component : relative)
    {
        if (component == ".")
        {
            continue;
        }
        if (component == "..")
        {
            return cue::Result<void>::failure(
                make_error(a_assertContext, a_code, "Artifact directory escaped the Project Root"));
        }
        current /= component;
        cue::Result<bool> state = inspect_directory_component(current, a_code, a_assertContext);
        if (!state)
        {
            return cue::Result<void>::failure(std::move(*state.try_error()));
        }
        if (!*state.try_value())
        {
            const std::filesystem::path created = native_path(current);
            if (CreateDirectoryW(created.c_str(), nullptr) == FALSE)
            {
                const DWORD code = GetLastError();
                if (code != ERROR_ALREADY_EXISTS)
                {
                    return cue::Result<void>::failure(make_windows_error(
                        a_assertContext, a_code, code, "Artifact directory component could not be created"));
                }
            }
            state = inspect_directory_component(current, a_code, a_assertContext);
            if (!state)
            {
                return cue::Result<void>::failure(std::move(*state.try_error()));
            }
            if (!*state.try_value())
            {
                return cue::Result<void>::failure(
                    make_error(a_assertContext, a_code, "Artifact directory component was not created"));
            }
        }
    }
    return cue::Result<void>::success();
}

/// @brief Lock Fileを作成して取消可能なSharedまたはExclusive Byte Range Lockを取得する
template <typename Cancellation>
[[nodiscard]] cue::Result<std::optional<GuardedByteRangeLock>> acquire_byte_range_lock(
    const std::filesystem::path &a_root, const std::filesystem::path &a_path, const Cancellation &a_cancellation,
    cue::BuildArtifactLockDeadline a_deadline, cue::WindowsBuildArtifactError a_code, bool a_isExclusive,
    const cue::AssertContext &a_assertContext) noexcept
{
    cue::Result<void> parent = ensure_directory(a_root, a_path.parent_path(), a_code, a_assertContext);
    if (!parent)
    {
        return cue::Result<std::optional<GuardedByteRangeLock>>::failure(std::move(*parent.try_error()));
    }
    cue::Result<DirectoryChainGuard> parentGuard =
        acquire_directory_chain_guard(a_root, a_path.parent_path(), a_code, a_assertContext);
    if (!parentGuard)
    {
        return cue::Result<std::optional<GuardedByteRangeLock>>::failure(std::move(*parentGuard.try_error()));
    }
    const std::filesystem::path lockPath = native_path(a_path);
    UniqueHandle handle(CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr));
    if (!handle.is_valid())
    {
        return cue::Result<std::optional<GuardedByteRangeLock>>::failure(
            make_windows_error(a_assertContext, a_code, GetLastError(), "Artifact lock file could not be opened"));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle.get(), &information) == FALSE)
    {
        return cue::Result<std::optional<GuardedByteRangeLock>>::failure(make_windows_error(
            a_assertContext, a_code, GetLastError(), "Artifact lock file attributes could not be read"));
    }
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0U)
    {
        return cue::Result<std::optional<GuardedByteRangeLock>>::failure(make_windows_error(
            a_assertContext, a_code, ERROR_FILE_INVALID, "Artifact lock path is not a regular file"));
    }
    while (!a_cancellation.is_cancel_requested())
    {
        if (a_deadline && std::chrono::steady_clock::now() >= *a_deadline)
        {
            return cue::Result<std::optional<GuardedByteRangeLock>>::failure(
                make_lock_timeout_error(a_assertContext, "Artifact byte-range lock wait timed out"));
        }
        OVERLAPPED overlap{};
        const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (a_isExclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0U);
        if (LockFileEx(handle.get(), flags, 0U, 1U, 0U, &overlap) != FALSE)
        {
            GuardedByteRangeLock guardedLock(std::move(handle), std::move(*parentGuard.try_value()));
            return cue::Result<std::optional<GuardedByteRangeLock>>::success(
                std::optional<GuardedByteRangeLock>(std::move(guardedLock)));
        }
        const DWORD code = GetLastError();
        if (code != ERROR_LOCK_VIOLATION && code != ERROR_IO_PENDING)
        {
            return cue::Result<std::optional<GuardedByteRangeLock>>::failure(
                make_windows_error(a_assertContext, a_code, code, "Artifact byte-range lock could not be acquired"));
        }
        DWORD retryMilliseconds = k_lockRetryMilliseconds;
        if (a_deadline)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= *a_deadline)
            {
                return cue::Result<std::optional<GuardedByteRangeLock>>::failure(
                    make_lock_timeout_error(a_assertContext, "Artifact byte-range lock wait timed out"));
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*a_deadline - now);
            retryMilliseconds = static_cast<DWORD>(
                std::clamp<std::int64_t>(remaining.count(), 1, static_cast<std::int64_t>(k_lockRetryMilliseconds)));
        }
        Sleep(retryMilliseconds);
    }
    return cue::Result<std::optional<GuardedByteRangeLock>>::success(std::nullopt);
}

/// @brief Lock Fileを作成して取消可能なExclusive Byte Range Lockを取得する
[[nodiscard]] cue::Result<std::optional<GuardedByteRangeLock>> acquire_exclusive_lock(
    const std::filesystem::path &a_root, const std::filesystem::path &a_path,
    const cue::ChildProcessCancellation &a_cancellation, cue::BuildArtifactLockDeadline a_deadline,
    cue::WindowsBuildArtifactError a_code, const cue::AssertContext &a_assertContext) noexcept
{
    return acquire_byte_range_lock(a_root, a_path, a_cancellation, a_deadline, a_code, true, a_assertContext);
}

/// @brief Lock Fileを作成して取消可能なShared Byte Range Lockを取得する
[[nodiscard]] cue::Result<std::optional<GuardedByteRangeLock>> acquire_shared_lock(
    const std::filesystem::path &a_root, const std::filesystem::path &a_path,
    const cue::BuildArtifactReadCancellation &a_cancellation, cue::BuildArtifactLockDeadline a_deadline,
    cue::WindowsBuildArtifactError a_code, const cue::AssertContext &a_assertContext) noexcept
{
    return acquire_byte_range_lock(a_root, a_path, a_cancellation, a_deadline, a_code, false, a_assertContext);
}

/// @brief Regular FileをSHA-256でStreaming HashしSizeとDigestを返す
[[nodiscard]] cue::Result<cue::BuildArtifactFile> hash_file(const std::filesystem::path &a_path,
                                                            std::string a_relativePath,
                                                            cue::BuildArtifactFilePurpose a_purpose,
                                                            const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code statusError;
    const std::filesystem::file_status status = std::filesystem::symlink_status(a_path, statusError);
    if (statusError || !std::filesystem::is_regular_file(status))
    {
        return cue::Result<cue::BuildArtifactFile>::failure(make_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, "Artifact entry is not a regular file"));
    }
    std::error_code sizeError;
    const std::uintmax_t byteSize = std::filesystem::file_size(a_path, sizeError);
    if (sizeError || byteSize > 9007199254740991ULL)
    {
        return cue::Result<cue::BuildArtifactFile>::failure(make_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, "Artifact entry size is invalid"));
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS statusCode = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
    if (statusCode < 0)
    {
        return cue::Result<cue::BuildArtifactFile>::failure(make_nt_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, statusCode, "SHA-256 provider failed"));
    }
    /// @brief BCrypt Algorithm Providerを全経路でCloseする
    const auto closeAlgorithm = [](BCRYPT_ALG_HANDLE *a_algorithm) noexcept
    {
        if (*a_algorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(*a_algorithm, 0U);
        }
    };
    std::unique_ptr<BCRYPT_ALG_HANDLE, decltype(closeAlgorithm)> algorithmOwner(&algorithm, closeAlgorithm);

    DWORD objectBytes = 0U;
    DWORD copied = 0U;
    statusCode = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                                   sizeof(objectBytes), &copied, 0U);
    if (statusCode < 0 || copied != sizeof(objectBytes))
    {
        return cue::Result<cue::BuildArtifactFile>::failure(
            make_nt_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, statusCode,
                          "SHA-256 state query failed"));
    }
    std::vector<std::uint8_t> object(objectBytes);
    BCRYPT_HASH_HANDLE hash = nullptr;
    statusCode = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0U, 0U);
    if (statusCode < 0)
    {
        return cue::Result<cue::BuildArtifactFile>::failure(
            make_nt_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, statusCode,
                          "SHA-256 state creation failed"));
    }
    /// @brief BCrypt Hash Stateを全経路で破棄する
    const auto destroyHash = [](BCRYPT_HASH_HANDLE *a_hash) noexcept
    {
        if (*a_hash != nullptr)
        {
            BCryptDestroyHash(*a_hash);
        }
    };
    std::unique_ptr<BCRYPT_HASH_HANDLE, decltype(destroyHash)> hashOwner(&hash, destroyHash);

    std::ifstream input(a_path, std::ios::binary);
    if (!input)
    {
        return cue::Result<cue::BuildArtifactFile>::failure(make_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, "Artifact entry could not be opened"));
    }
    std::array<std::uint8_t, k_hashBlockBytes> block{};
    while (input)
    {
        input.read(reinterpret_cast<char *>(block.data()), static_cast<std::streamsize>(block.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
        {
            statusCode = BCryptHashData(hash, block.data(), static_cast<ULONG>(count), 0U);
            if (statusCode < 0)
            {
                return cue::Result<cue::BuildArtifactFile>::failure(
                    make_nt_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, statusCode,
                                  "SHA-256 update failed"));
            }
        }
    }
    if (!input.eof())
    {
        return cue::Result<cue::BuildArtifactFile>::failure(make_error(
            a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, "Artifact entry could not be read"));
    }
    std::array<std::uint8_t, k_sha256Bytes> digest{};
    statusCode = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0U);
    if (statusCode < 0)
    {
        return cue::Result<cue::BuildArtifactFile>::failure(
            make_nt_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, statusCode,
                          "SHA-256 finalization failed"));
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hashText;
    hashText.reserve(digest.size() * 2U);
    for (const std::uint8_t value : digest)
    {
        hashText.push_back(digits[value >> 4U]);
        hashText.push_back(digits[value & 0x0fU]);
    }
    return cue::Result<cue::BuildArtifactFile>::success(
        {std::move(a_relativePath), static_cast<std::uint64_t>(byteSize), std::move(hashText), a_purpose});
}

/// @brief 上限確認済みCanonical Byte列をSHA-256 Textへ変換する
[[nodiscard]] cue::Result<std::string> hash_bytes(std::string_view a_bytes,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    if (a_bytes.size() > static_cast<std::size_t>(UINT32_MAX))
    {
        return cue::Result<std::string>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                       "Source inventory canonical bytes exceed the hashing limit"));
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    const NTSTATUS openStatus = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
    if (openStatus < 0)
    {
        return cue::Result<std::string>::failure(make_nt_error(a_assertContext,
                                                               cue::WindowsBuildArtifactError::CandidateInvalid,
                                                               openStatus, "Source inventory SHA-256 provider failed"));
    }
    const auto closeAlgorithm = [](BCRYPT_ALG_HANDLE *a_algorithm) noexcept
    {
        if (*a_algorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(*a_algorithm, 0U);
        }
    };
    std::unique_ptr<BCRYPT_ALG_HANDLE, decltype(closeAlgorithm)> algorithmOwner(&algorithm, closeAlgorithm);
    std::array<std::uint8_t, k_sha256Bytes> digest{};
    const NTSTATUS hashStatus =
        BCryptHash(algorithm, nullptr, 0U, reinterpret_cast<PUCHAR>(const_cast<char *>(a_bytes.data())),
                   static_cast<ULONG>(a_bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
    if (hashStatus < 0)
    {
        return cue::Result<std::string>::failure(
            make_nt_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, hashStatus,
                          "Source inventory SHA-256 calculation failed"));
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hashText;
    hashText.reserve(digest.size() * 2U);
    for (const std::uint8_t value : digest)
    {
        hashText.push_back(digits[value >> 4U]);
        hashText.push_back(digits[value & 0x0fU]);
    }
    return cue::Result<std::string>::success(std::move(hashText));
}

/// @brief Provenance検証用の小さいRegular Text Fileを上限付きで読む
[[nodiscard]] cue::Result<std::string> read_toolchain_evidence_file(const std::filesystem::path &a_path,
                                                                    const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code statusError;
    const std::filesystem::file_status status = std::filesystem::symlink_status(a_path, statusError);
    std::error_code sizeError;
    const std::uintmax_t byteSize = std::filesystem::file_size(a_path, sizeError);
    if (statusError || sizeError || !std::filesystem::is_regular_file(status) || byteSize == 0U ||
        byteSize > k_maximumToolchainEvidenceBytes)
    {
        return cue::Result<std::string>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                       "Shipping toolchain evidence is unavailable or outside the supported limit"));
    }
    std::ifstream input(a_path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(byteSize), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof() || bytes.find('\0') != std::string::npos)
    {
        return cue::Result<std::string>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                       "Shipping toolchain evidence could not be read as bounded text"));
    }
    return cue::Result<std::string>::success(std::move(bytes));
}

/// @brief 一意な行Prefixに続く値を返す
[[nodiscard]] std::optional<std::string> unique_line_value(std::string_view a_text, std::string_view a_prefix)
{
    std::optional<std::string> value;
    std::size_t cursor = 0U;
    while (cursor <= a_text.size())
    {
        const std::size_t end = a_text.find('\n', cursor);
        std::string_view line =
            a_text.substr(cursor, end == std::string_view::npos ? a_text.size() - cursor : end - cursor);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1U);
        }
        if (line.starts_with(a_prefix))
        {
            if (value)
            {
                return std::nullopt;
            }
            value.emplace(line.substr(a_prefix.size()));
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        cursor = end + 1U;
    }
    return value;
}

/// @brief 一意なCanonical JSON String行からEscapeなしの値を取得する
[[nodiscard]] std::optional<std::string> unique_metadata_string_value(std::string_view a_text,
                                                                      std::string_view a_prefix)
{
    std::optional<std::string> value = unique_line_value(a_text, a_prefix);
    if (!value || value->size() < 2U || !value->ends_with("\","))
    {
        return std::nullopt;
    }
    value->resize(value->size() - 2U);
    if (value->find('"') != std::string::npos || value->find('\\') != std::string::npos)
    {
        return std::nullopt;
    }
    return value;
}

/// @brief Publisher生成Metadata v1のArtifactとProject Identityを厳格に取得する
[[nodiscard]] std::optional<std::string> parse_runtime_metadata_project_id(
    std::string_view a_text, const cue::BuildArtifactInventory &a_inventory)
{
    const std::optional<std::string> schema = unique_line_value(a_text, "    \"schemaVersion\": ");
    const std::optional<std::string> artifact = unique_metadata_string_value(a_text, "    \"artifactId\": \"");
    std::optional<std::string> project = unique_metadata_string_value(a_text, "    \"projectId\": \"");
    if (!schema || *schema != "1," || !artifact || *artifact != a_inventory.artifact_id() || !project)
    {
        return std::nullopt;
    }
    if (a_inventory.profile().target() == cue::BuildTarget::ShippingProduct)
    {
        const std::optional<std::string> target = unique_metadata_string_value(a_text, "    \"target\": \"");
        if (!target || *target != "ShippingProduct")
        {
            return std::nullopt;
        }
    }
    return project;
}

/// @brief 一意なCMake Cache Entryの型に依存せず値を返す
[[nodiscard]] std::optional<std::string> unique_cmake_cache_value(std::string_view a_text, std::string_view a_name)
{
    std::string prefix(a_name);
    prefix.push_back(':');
    std::optional<std::string> value;
    std::size_t cursor = 0U;
    while (cursor <= a_text.size())
    {
        const std::size_t end = a_text.find('\n', cursor);
        std::string_view line =
            a_text.substr(cursor, end == std::string_view::npos ? a_text.size() - cursor : end - cursor);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1U);
        }
        if (line.starts_with(prefix))
        {
            const std::size_t separator = line.find('=', prefix.size());
            if (value || separator == std::string_view::npos || separator + 1U == line.size())
            {
                return std::nullopt;
            }
            value.emplace(line.substr(separator + 1U));
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        cursor = end + 1U;
    }
    return value;
}

/// @brief CMake Compiler設定の一意なquoted set値を返す
[[nodiscard]] std::optional<std::string> unique_cmake_quoted_value(std::string_view a_text, std::string_view a_variable)
{
    std::string prefix("set(");
    prefix.append(a_variable);
    prefix.append(" \"");
    std::optional<std::string> value = unique_line_value(a_text, prefix);
    constexpr std::string_view suffix = "\")";
    if (!value || !std::string_view(*value).ends_with(suffix))
    {
        return std::nullopt;
    }
    value->resize(value->size() - suffix.size());
    return value;
}

/// @brief XML内に一回以上現れる同一Tag値だけを返す
[[nodiscard]] std::optional<std::string> uniform_xml_tag_value(std::string_view a_text, std::string_view a_tag)
{
    std::string opening("<");
    opening.append(a_tag);
    opening.push_back('>');
    std::string closing("</");
    closing.append(a_tag);
    closing.push_back('>');
    std::optional<std::string> value;
    std::size_t cursor = 0U;
    while ((cursor = a_text.find(opening, cursor)) != std::string_view::npos)
    {
        const std::size_t valueStart = cursor + opening.size();
        const std::size_t valueEnd = a_text.find(closing, valueStart);
        if (valueEnd == std::string_view::npos)
        {
            return std::nullopt;
        }
        const std::string current(a_text.substr(valueStart, valueEnd - valueStart));
        if (current.empty() || (value && *value != current))
        {
            return std::nullopt;
        }
        value = current;
        cursor = valueEnd + closing.size();
    }
    return value;
}

/// @brief XMLのdouble-quoted Attribute値へ埋め込める表現を末尾へ追加する
void append_xml_double_quoted_attribute(std::string &a_output, std::string_view a_value)
{
    for (const char value : a_value)
    {
        switch (value)
        {
        case '&':
            a_output.append("&amp;");
            break;
        case '<':
            a_output.append("&lt;");
            break;
        case '>':
            a_output.append("&gt;");
            break;
        case '"':
            a_output.append("&quot;");
            break;
        default:
            a_output.push_back(value);
            break;
        }
    }
}

/// @brief Build Tool VersionをCMakeの4要素表現へ変換する
[[nodiscard]] std::string build_tool_version_text(const cue::BuildToolVersion &a_version)
{
    std::string output = std::to_string(a_version.major);
    output.push_back('.');
    output.append(std::to_string(a_version.minor));
    output.push_back('.');
    output.append(std::to_string(a_version.patch));
    output.push_back('.');
    output.append(std::to_string(a_version.build));
    return output;
}

/// @brief Dot区切りVersionを最大4要素のVersionへ変換する
[[nodiscard]] std::optional<cue::BuildToolVersion> parse_build_tool_version(std::string_view a_text) noexcept
{
    cue::BuildToolVersion version;
    std::uint32_t *parts[] = {&version.major, &version.minor, &version.patch, &version.build};
    std::size_t partIndex = 0U;
    std::uint64_t value = 0U;
    bool hasDigit = false;
    for (std::size_t index = 0U; index <= a_text.size(); ++index)
    {
        if (index < a_text.size() && a_text[index] >= '0' && a_text[index] <= '9')
        {
            hasDigit = true;
            const std::uint64_t digit = static_cast<std::uint64_t>(a_text[index] - '0');
            if (value > (UINT32_MAX - digit) / 10U)
            {
                return std::nullopt;
            }
            value = value * 10U + digit;
            continue;
        }
        if (!hasDigit || partIndex >= 4U || (index < a_text.size() && a_text[index] != '.'))
        {
            return std::nullopt;
        }
        *parts[partIndex++] = static_cast<std::uint32_t>(value);
        value = 0U;
        hasDigit = false;
    }
    if (partIndex < 2U)
    {
        return std::nullopt;
    }
    while (partIndex < 4U)
    {
        *parts[partIndex++] = 0U;
    }
    return std::optional<cue::BuildToolVersion>(version);
}

/// @brief 実ファイルからVersionリソースを取得する
[[nodiscard]] std::optional<cue::BuildToolVersion> read_binary_file_version(std::wstring_view a_path) noexcept
{
    DWORD ignored = 0U;
    const DWORD size = GetFileVersionInfoSizeW(a_path.data(), &ignored);
    if (size == 0U)
    {
        return std::nullopt;
    }
    try
    {
        std::vector<std::byte> data(size);
        if (GetFileVersionInfoW(a_path.data(), 0U, size, data.data()) == FALSE)
        {
            return std::nullopt;
        }
        VS_FIXEDFILEINFO *info = nullptr;
        UINT infoSize = 0U;
        if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &infoSize) == FALSE ||
            info == nullptr || infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE)
        {
            return std::nullopt;
        }
        return cue::BuildToolVersion{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                                     HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
    }
    catch (...)
    {
        return std::nullopt;
    }
}

/// @brief Project Binary Treeが実際に選択したCMake、MSVC、Windows SDKを検証する
[[nodiscard]] cue::Result<std::optional<ShippingToolchainIdentity>> collect_shipping_toolchain_identity(
    const std::filesystem::path &a_binary, const cue::BuildPlan &a_plan,
    const cue::ChildProcessCancellation &a_cancellation, std::string_view a_expectedEngineSourceRoot,
    const cue::AssertContext &a_assertContext) noexcept
{
    if (a_cancellation.is_cancel_requested())
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::success(std::nullopt);
    }
    cue::Result<std::string> cache = read_toolchain_evidence_file(a_binary / "CMakeCache.txt", a_assertContext);
    if (!cache)
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::failure(std::move(*cache.try_error()));
    }
    const std::optional<std::string> cmakeCommand = unique_line_value(*cache.try_value(), "CMAKE_COMMAND:INTERNAL=");
    const std::optional<std::string> cmakeMajor =
        unique_line_value(*cache.try_value(), "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=");
    const std::optional<std::string> cmakeMinor =
        unique_line_value(*cache.try_value(), "CMAKE_CACHE_MINOR_VERSION:INTERNAL=");
    const std::optional<std::string> cmakePatch =
        unique_line_value(*cache.try_value(), "CMAKE_CACHE_PATCH_VERSION:INTERNAL=");
    const std::optional<std::string> generator = unique_line_value(*cache.try_value(), "CMAKE_GENERATOR:INTERNAL=");
    const std::optional<std::string> generatorInstance =
        unique_line_value(*cache.try_value(), "CMAKE_GENERATOR_INSTANCE:INTERNAL=");
    const std::optional<std::string> generatorPlatform =
        unique_line_value(*cache.try_value(), "CMAKE_GENERATOR_PLATFORM:INTERNAL=");
    const std::optional<std::string> generatorToolset =
        unique_line_value(*cache.try_value(), "CMAKE_GENERATOR_TOOLSET:INTERNAL=");
    const std::optional<std::string> engineRoot = unique_cmake_cache_value(*cache.try_value(), "CUE_ENGINE_ROOT");
    if (!cmakeCommand || !cmakeMajor || !cmakeMinor || !cmakePatch || !generator || !generatorInstance ||
        !generatorPlatform || !generatorToolset || !engineRoot)
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                       "Shipping CMake cache is missing required toolchain identity"));
    }
    const std::optional<std::filesystem::path> cmakePath = to_path(*cmakeCommand);
    const std::optional<std::filesystem::path> visualStudioPath = to_path(*generatorInstance);
    const std::optional<std::filesystem::path> engineSourcePath = to_path(*engineRoot);
    std::string cmakeVersion = *cmakeMajor + "." + *cmakeMinor + "." + *cmakePatch;
    const std::string expectedGeneratorToolset = "version=" + std::string(cue::build_metadata::k_msvcToolsetVersion);
    if (!cmakePath || !visualStudioPath || !engineSourcePath || cmakeVersion != cue::build_metadata::k_cmakeVersion ||
        *generator != cue::build_metadata::k_cmakeGenerator || *generatorPlatform != "x64" ||
        *generatorToolset != expectedGeneratorToolset || !same_root(*cmakePath, cue::build_metadata::k_cmakeCommand) ||
        !same_root(*visualStudioPath, cue::build_metadata::k_visualStudioRoot) ||
        !same_root(*engineSourcePath, a_expectedEngineSourceRoot))
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                       "Shipping CMake selection differs from the Engine Build Plan"));
    }

    cue::Result<std::string> compilerEvidence = read_toolchain_evidence_file(
        a_binary / "CMakeFiles" / cmakeVersion / "CMakeCXXCompiler.cmake", a_assertContext);
    if (!compilerEvidence)
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::failure(std::move(*compilerEvidence.try_error()));
    }
    const std::optional<std::string> compiler =
        unique_cmake_quoted_value(*compilerEvidence.try_value(), "CMAKE_CXX_COMPILER");
    const std::optional<std::string> compilerVersion =
        unique_cmake_quoted_value(*compilerEvidence.try_value(), "CMAKE_CXX_COMPILER_VERSION");
    const std::optional<cue::BuildToolVersion> parsedCompilerVersion =
        compilerVersion ? parse_build_tool_version(*compilerVersion) : std::nullopt;
    const std::optional<std::string> compilerArchitecture =
        unique_cmake_quoted_value(*compilerEvidence.try_value(), "CMAKE_CXX_COMPILER_ARCHITECTURE_ID");
    const std::optional<std::filesystem::path> compilerPath = compiler ? to_path(*compiler) : std::nullopt;
    if (!compilerPath || !parsedCompilerVersion || !compilerArchitecture || *compilerArchitecture != "x64" ||
        !(*parsedCompilerVersion == a_plan.workspace_compatibility().toolsetVersion) ||
        !same_root(*compilerPath, cue::build_metadata::k_msvcCompiler))
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                       "Shipping MSVC compiler differs from the Build Plan"));
    }

    cue::Result<std::string> project =
        read_toolchain_evidence_file(a_binary / "Source" / "Game" / "CueGameProduct.vcxproj", a_assertContext);
    if (!project)
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::failure(std::move(*project.try_error()));
    }
    const std::optional<std::string> platformToolset = uniform_xml_tag_value(*project.try_value(), "PlatformToolset");
    const std::optional<std::string> windowsSdkVersion =
        uniform_xml_tag_value(*project.try_value(), "WindowsTargetPlatformVersion");
    const std::optional<std::string> vcToolsVersion = uniform_xml_tag_value(*project.try_value(), "VCToolsVersion");
    constexpr std::string_view msvcToolsetVersion = cue::build_metadata::k_msvcToolsetVersion;
    const std::size_t firstVersionSeparator = msvcToolsetVersion.find('.');
    const std::size_t secondVersionSeparator =
        firstVersionSeparator == std::string_view::npos ? std::string_view::npos
                                                        : msvcToolsetVersion.find('.', firstVersionSeparator + 1U);
    const std::string_view propsVersion = secondVersionSeparator == std::string_view::npos
                                              ? std::string_view{}
                                              : msvcToolsetVersion.substr(0U, secondVersionSeparator);
    std::string expectedToolsetImport("Project=\"");
    append_xml_double_quoted_attribute(expectedToolsetImport, cue::build_metadata::k_visualStudioRoot);
    expectedToolsetImport.append("/VC/Auxiliary/Build/");
    expectedToolsetImport.append(propsVersion);
    expectedToolsetImport.append("/Microsoft.VCToolsVersion.");
    expectedToolsetImport.append(propsVersion);
    expectedToolsetImport.append(".props\"");
    const std::size_t toolsetImport = project.try_value()->find(expectedToolsetImport);
    const bool validToolsetImport = !propsVersion.empty() && toolsetImport != std::string::npos &&
                                    project.try_value()->find(expectedToolsetImport, toolsetImport + 1U) ==
                                        std::string::npos;
    const bool validPlatformToolset =
        platformToolset && platformToolset->size() <= 32U &&
        std::all_of(platformToolset->begin(), platformToolset->end(),
                    [](char a_value)
                    {
                        return (a_value >= '0' && a_value <= '9') || (a_value >= 'A' && a_value <= 'Z') ||
                               (a_value >= 'a' && a_value <= 'z') || a_value == '.' || a_value == '_' || a_value == '-';
                    });
    if (!validPlatformToolset || *platformToolset != cue::build_metadata::k_platformToolset ||
        !validToolsetImport || !vcToolsVersion || *vcToolsVersion != msvcToolsetVersion || !windowsSdkVersion ||
        *windowsSdkVersion != cue::build_metadata::k_windowsSdkVersion)
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                       "Shipping Visual Studio project differs from the selected Windows toolchain"));
    }
    const std::wstring compilerPathUtf16 = compilerPath->generic_wstring();
    const std::optional<cue::BuildToolVersion> compilerFileVersion = read_binary_file_version(compilerPathUtf16);
    if (!compilerFileVersion || !(*compilerFileVersion == *parsedCompilerVersion))
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                       "Shipping MSVC compiler binary differs from the CMake compiler version"));
    }
    cue::Result<cue::BuildArtifactFile> compilerHash =
        hash_file(*compilerPath, "cl.exe", cue::BuildArtifactFilePurpose::Unspecified, a_assertContext);
    if (!compilerHash)
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::failure(std::move(*compilerHash.try_error()));
    }
    if (a_cancellation.is_cancel_requested())
    {
        return cue::Result<std::optional<ShippingToolchainIdentity>>::success(std::nullopt);
    }
    ShippingToolchainIdentity identity;
    identity.cmakeVersion = std::move(cmakeVersion);
    identity.cmakeGenerator = std::move(*generator);
    identity.platformToolset = std::move(*platformToolset);
    identity.msvcToolsetVersion = std::move(*vcToolsVersion);
    identity.compilerFileVersion = build_tool_version_text(*compilerFileVersion);
    identity.compilerSha256 = std::move(compilerHash.try_value()->contentHash);
    identity.windowsSdkVersion = std::move(*windowsSdkVersion);
    return cue::Result<std::optional<ShippingToolchainIdentity>>::success(
        std::optional<ShippingToolchainIdentity>(std::move(identity)));
}

/// @brief 指定Root配下のBuild入力を決定的なPath、Size、Content Hash集合へ変換する
[[nodiscard]] cue::Result<std::optional<SourceInventoryIdentity>> collect_source_inventory(
    const std::filesystem::path &a_root, std::span<const std::filesystem::path> a_directories,
    std::span<const std::filesystem::path> a_files, const cue::ChildProcessCancellation &a_cancellation,
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::error_code rootError;
        const std::filesystem::path root = std::filesystem::weakly_canonical(a_root, rootError);
        if (rootError || !root.is_absolute())
        {
            return cue::Result<std::optional<SourceInventoryIdentity>>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                           "Source inventory root is unavailable"));
        }
        std::vector<std::filesystem::path> inputFiles;
        for (const std::filesystem::path &relativeDirectory : a_directories)
        {
            const std::filesystem::path directory = root / relativeDirectory;
            std::error_code statusError;
            const std::filesystem::file_status directoryStatus =
                std::filesystem::symlink_status(directory, statusError);
            if (statusError || !std::filesystem::is_directory(directoryStatus))
            {
                return cue::Result<std::optional<SourceInventoryIdentity>>::failure(
                    make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                               "Required source inventory directory is unavailable or is a reparse point"));
            }
            std::error_code iteratorError;
            for (std::filesystem::recursive_directory_iterator iterator(directory, iteratorError), end;
                 !iteratorError && iterator != end; iterator.increment(iteratorError))
            {
                if (a_cancellation.is_cancel_requested())
                {
                    return cue::Result<std::optional<SourceInventoryIdentity>>::success(std::nullopt);
                }
                std::error_code entryError;
                const std::filesystem::file_status entryStatus = iterator->symlink_status(entryError);
                if (entryError ||
                    (!std::filesystem::is_directory(entryStatus) && !std::filesystem::is_regular_file(entryStatus)))
                {
                    return cue::Result<std::optional<SourceInventoryIdentity>>::failure(
                        make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                                   "Source inventory contains an unsupported or reparse entry"));
                }
                if (std::filesystem::is_regular_file(entryStatus))
                {
                    inputFiles.push_back(iterator->path());
                }
            }
            if (iteratorError)
            {
                return cue::Result<std::optional<SourceInventoryIdentity>>::failure(make_windows_error(
                    a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                    static_cast<DWORD>(iteratorError.value()), "Source inventory directory could not be enumerated"));
            }
        }
        for (const std::filesystem::path &relativeFile : a_files)
        {
            const std::filesystem::path file = root / relativeFile;
            std::error_code statusError;
            const std::filesystem::file_status status = std::filesystem::symlink_status(file, statusError);
            if (statusError || !std::filesystem::is_regular_file(status))
            {
                return cue::Result<std::optional<SourceInventoryIdentity>>::failure(
                    make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                               "Required source inventory file is unavailable or is a reparse point"));
            }
            inputFiles.push_back(file);
        }
        if (inputFiles.empty() || inputFiles.size() > k_maximumSourceInventoryFiles)
        {
            return cue::Result<std::optional<SourceInventoryIdentity>>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                           "Source inventory file count is outside the supported limit"));
        }
        std::sort(
            inputFiles.begin(), inputFiles.end(),
            [&root](const std::filesystem::path &a_left, const std::filesystem::path &a_right)
            { return path_to_utf8(a_left.lexically_relative(root)) < path_to_utf8(a_right.lexically_relative(root)); });

        std::string canonical;
        canonical.reserve(std::min(k_maximumSourceInventoryBytes, inputFiles.size() * 160U));
        std::string previousPath;
        std::uint64_t totalBytes = 0U;
        for (const std::filesystem::path &file : inputFiles)
        {
            if (a_cancellation.is_cancel_requested())
            {
                return cue::Result<std::optional<SourceInventoryIdentity>>::success(std::nullopt);
            }
            const std::filesystem::path relative = file.lexically_relative(root);
            const std::string relativeText = path_to_utf8(relative);
            if (relative.empty() || relative.is_absolute() || relativeText.empty() || relativeText == previousPath ||
                relativeText.starts_with("../") || relativeText.find("/../") != std::string::npos)
            {
                return cue::Result<std::optional<SourceInventoryIdentity>>::failure(
                    make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                               "Source inventory contains an unsafe or duplicate path"));
            }
            cue::Result<cue::BuildArtifactFile> identity =
                hash_file(file, relativeText, cue::BuildArtifactFilePurpose::Unspecified, a_assertContext);
            if (!identity)
            {
                return cue::Result<std::optional<SourceInventoryIdentity>>::failure(std::move(*identity.try_error()));
            }
            if (identity.try_value()->byteSize > k_maximumSourceInputBytes - totalBytes)
            {
                return cue::Result<std::optional<SourceInventoryIdentity>>::failure(
                    make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                               "Source inventory content exceeds the supported limit"));
            }
            totalBytes += identity.try_value()->byteSize;
            canonical.append(relativeText);
            canonical.push_back('\0');
            canonical.append(std::to_string(identity.try_value()->byteSize));
            canonical.push_back('\0');
            canonical.append(identity.try_value()->contentHash);
            canonical.push_back('\n');
            if (canonical.size() > k_maximumSourceInventoryBytes)
            {
                return cue::Result<std::optional<SourceInventoryIdentity>>::failure(
                    make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                               "Source inventory canonical form exceeds the supported limit"));
            }
            previousPath = relativeText;
        }
        cue::Result<std::string> inventoryHash = hash_bytes(canonical, a_assertContext);
        if (!inventoryHash)
        {
            return cue::Result<std::optional<SourceInventoryIdentity>>::failure(std::move(*inventoryHash.try_error()));
        }
        return cue::Result<std::optional<SourceInventoryIdentity>>::success(std::optional<SourceInventoryIdentity>(
            SourceInventoryIdentity{inputFiles.size(), std::move(*inventoryHash.try_value())}));
    }
    catch (...)
    {
        terminate_artifact_exception(a_assertContext);
    }
}

/// @brief 可視なCurrentが参照するVersion全FileをInventoryのSizeとHashへ再照合する
[[nodiscard]] cue::Result<void> verify_inventory_files(const std::filesystem::path &a_version,
                                                       const cue::BuildArtifactInventory &a_inventory,
                                                       const cue::AssertContext &a_assertContext) noexcept
{
    for (const cue::BuildArtifactFile &expected : a_inventory.files())
    {
        const std::optional<std::filesystem::path> relative = to_path(expected.relativePath);
        if (!relative)
        {
            return cue::Result<void>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed,
                           "Visible Current artifact contains an invalid inventory path"));
        }
        cue::Result<cue::BuildArtifactFile> actual =
            hash_file(a_version / *relative, expected.relativePath, expected.purpose, a_assertContext);
        if (!actual)
        {
            return cue::Result<void>::failure(std::move(*actual.try_error()));
        }
        if (actual.try_value()->byteSize != expected.byteSize ||
            actual.try_value()->contentHash != expected.contentHash)
        {
            return cue::Result<void>::failure(make_error(a_assertContext,
                                                         cue::WindowsBuildArtifactError::CurrentManifestFailed,
                                                         "Visible Current artifact differs from its inventory"));
        }
    }
    return cue::Result<void>::success();
}

/// @brief Current更新Errorが可視化後の耐久性不明を表すか判定する
[[nodiscard]] bool is_current_durability_unknown(const cue::Error &a_error) noexcept
{
    const cue::ErrorCode &root = a_error.root_code();
    return root.domain() == "Cue.Build.Windows.Artifact" &&
           root.value() == static_cast<std::int64_t>(cue::WindowsBuildArtifactError::CurrentManifestDurabilityUnknown);
}

/// @brief Version公開Errorが可視化後の耐久性不明を表すか判定する
[[nodiscard]] bool is_version_durability_unknown(const cue::Error &a_error) noexcept
{
    const cue::ErrorCode &root = a_error.root_code();
    return root.domain() == "Cue.Build.Windows.Artifact" &&
           root.value() == static_cast<std::int64_t>(cue::WindowsBuildArtifactError::ArtifactVersionDurabilityUnknown);
}

/// @brief Byte列を新規Fileへ書きFlushする
[[nodiscard]] cue::Result<void> write_new_file(const std::filesystem::path &a_path, std::string_view a_bytes,
                                               cue::WindowsBuildArtifactError a_code,
                                               const cue::AssertContext &a_assertContext) noexcept
{
    std::optional<cue::Error> writeError;
    {
        UniqueHandle file(
            CreateFileW(a_path.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.is_valid())
        {
            return cue::Result<void>::failure(
                make_windows_error(a_assertContext, a_code, GetLastError(), "Artifact file could not be created"));
        }
        std::size_t offset = 0U;
        while (offset < a_bytes.size())
        {
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(a_bytes.size() - offset, MAXDWORD));
            DWORD written = 0U;
            const BOOL succeeded = WriteFile(file.get(), a_bytes.data() + offset, request, &written, nullptr);
            if (succeeded == FALSE || written == 0U)
            {
                const DWORD code = succeeded == FALSE ? GetLastError() : ERROR_WRITE_FAULT;
                writeError.emplace(make_windows_error(a_assertContext, a_code, code, "Artifact file write failed"));
                break;
            }
            offset += written;
        }
        if (!writeError.has_value() && FlushFileBuffers(file.get()) == FALSE)
        {
            writeError.emplace(
                make_windows_error(a_assertContext, a_code, GetLastError(), "Artifact file flush failed"));
        }
    }
    if (!writeError.has_value())
    {
        return cue::Result<void>::success();
    }
    if (DeleteFileW(a_path.c_str()) == FALSE)
    {
        const DWORD cleanupCode = GetLastError();
        if (cleanupCode != ERROR_FILE_NOT_FOUND && cleanupCode != ERROR_PATH_NOT_FOUND)
        {
            cue::Error cleanupError =
                make_windows_error(a_assertContext, a_code, cleanupCode, "Incomplete artifact file rollback failed");
            writeError->append_secondary_diagnostics(a_assertContext, cleanupError,
                                                     "Incomplete artifact file could not be removed", "Rollback");
        }
    }
    return cue::Result<void>::failure(std::move(*writeError));
}

/// @brief Build出力を新規Candidate FileへCopyし、File内容をFlushしてから返す
[[nodiscard]] cue::Result<void> copy_new_file_durable(const std::filesystem::path &a_source,
                                                      const std::filesystem::path &a_destination,
                                                      const cue::AssertContext &a_assertContext) noexcept
{
    UniqueHandle source(CreateFileW(a_source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!source.is_valid())
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, GetLastError(),
                               "Build artifact source could not be opened"));
    }
    UniqueHandle destination(CreateFileW(a_destination.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!destination.is_valid())
    {
        return cue::Result<void>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid, GetLastError(),
                               "Build artifact candidate could not be created"));
    }

    std::array<std::byte, 64U * 1024U> buffer{};
    for (;;)
    {
        DWORD read = 0U;
        if (ReadFile(source.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == FALSE)
        {
            return cue::Result<void>::failure(make_windows_error(a_assertContext,
                                                                 cue::WindowsBuildArtifactError::CandidateInvalid,
                                                                 GetLastError(), "Build artifact source read failed"));
        }
        if (read == 0U)
        {
            break;
        }
        DWORD offset = 0U;
        while (offset < read)
        {
            DWORD written = 0U;
            const BOOL succeeded =
                WriteFile(destination.get(), buffer.data() + offset, read - offset, &written, nullptr);
            if (succeeded == FALSE || written == 0U)
            {
                const DWORD code = succeeded == FALSE ? GetLastError() : ERROR_WRITE_FAULT;
                return cue::Result<void>::failure(make_windows_error(a_assertContext,
                                                                     cue::WindowsBuildArtifactError::CandidateInvalid,
                                                                     code, "Build artifact candidate write failed"));
            }
            offset += written;
        }
    }
    if (FlushFileBuffers(destination.get()) == FALSE)
    {
        return cue::Result<void>::failure(make_windows_error(a_assertContext,
                                                             cue::WindowsBuildArtifactError::CandidateInvalid,
                                                             GetLastError(), "Build artifact candidate flush failed"));
    }
    return cue::Result<void>::success();
}

/// @brief 別ProcessでのArtifact検証完了種別
enum class ArtifactProbeStatus : std::uint8_t
{
    Valid,
    Cancelled
};

/// @brief 絶対DeadlineをChild Process Runner用の残り時間へ変換する
[[nodiscard]] std::optional<std::chrono::milliseconds> remaining_timeout(
    cue::BuildArtifactLockDeadline a_deadline) noexcept
{
    if (!a_deadline)
    {
        return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= *a_deadline)
    {
        return std::chrono::milliseconds(0);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*a_deadline - now);
    return std::max(remaining, std::chrono::milliseconds(1));
}

/// @brief Engine Git MetadataをShell非依存Processで読込み、取消時だけnulloptを返す
[[nodiscard]] cue::Result<std::optional<std::string>> read_git_output(
    cue::ChildProcessRunner &a_processRunner, const std::filesystem::path &a_engineRoot,
    std::vector<std::string> a_arguments, const cue::ChildProcessCancellation &a_cancellation,
    cue::BuildArtifactLockDeadline a_deadline, const cue::AssertContext &a_assertContext) noexcept
{
    const std::optional<std::chrono::milliseconds> timeout = remaining_timeout(a_deadline);
    if (timeout && timeout->count() <= 0)
    {
        return cue::Result<std::optional<std::string>>::failure(
            make_lock_timeout_error(a_assertContext, "Shipping provenance collection timed out"));
    }
    std::vector<std::string> arguments = {"-c", "safe.directory=" + path_to_utf8(a_engineRoot), "-C",
                                          path_to_utf8(a_engineRoot)};
    arguments.insert(arguments.end(), std::make_move_iterator(a_arguments.begin()),
                     std::make_move_iterator(a_arguments.end()));
    cue::ChildProcessRequest request(std::string(cue::build_metadata::k_gitCommand), std::move(arguments),
                                     path_to_utf8(a_engineRoot), {}, timeout, 512U * 1024U);
    cue::Result<cue::ChildProcessResult> process = a_processRunner.run(request, a_cancellation);
    if (!process)
    {
        return cue::Result<std::optional<std::string>>::failure(std::move(*process.try_error()));
    }
    if (process.try_value()->outcome() == cue::ChildProcessOutcome::Cancelled)
    {
        return cue::Result<std::optional<std::string>>::success(std::nullopt);
    }
    if (process.try_value()->outcome() == cue::ChildProcessOutcome::TimedOut)
    {
        return cue::Result<std::optional<std::string>>::failure(
            make_lock_timeout_error(a_assertContext, "Shipping provenance collection timed out"));
    }
    if (!process.try_value()->exit_code() || *process.try_value()->exit_code() != 0U)
    {
        return cue::Result<std::optional<std::string>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                       "Engine Git provenance could not be collected"));
    }
    std::string output;
    for (const cue::ChildProcessOutputChunk &chunk : process.try_value()->output())
    {
        if (chunk.stream == cue::ChildProcessStream::StandardOutput)
        {
            output.append(chunk.bytes);
        }
    }
    while (!output.empty() && (output.back() == '\r' || output.back() == '\n'))
    {
        output.pop_back();
    }
    return cue::Result<std::optional<std::string>>::success(std::optional<std::string>(std::move(output)));
}

/// @brief Shipping BuildのEngine／Game入力とToolchain由来情報を採取する
[[nodiscard]] cue::Result<std::optional<ShippingBuildProvenance>> collect_shipping_provenance(
    const std::filesystem::path &a_projectRoot, cue::ChildProcessRunner &a_processRunner,
    const cue::ChildProcessCancellation &a_cancellation, cue::BuildArtifactLockDeadline a_deadline,
    const std::optional<cue::WindowsInstalledEngineSourceProvenance> &a_installedEngineSource,
    const cue::AssertContext &a_assertContext) noexcept
{
    const std::string_view engineRootText = a_installedEngineSource
                                                ? std::string_view(a_installedEngineSource->sourceRoot)
                                                : cue::build_metadata::k_engineSourceRoot;
    const std::optional<std::filesystem::path> engineRoot = to_path(engineRootText);
    if (!engineRoot || !engineRoot->is_absolute())
    {
        return cue::Result<std::optional<ShippingBuildProvenance>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                       "Engine source root for Shipping provenance is unavailable"));
    }
    std::string commitText;
    std::string statusText;
    if (a_installedEngineSource)
    {
        commitText = a_installedEngineSource->sourceRevision;
    }
    else
    {
        cue::Result<std::optional<std::string>> commit =
            read_git_output(a_processRunner, *engineRoot, {"rev-parse", "--verify", "HEAD"}, a_cancellation,
                            a_deadline, a_assertContext);
        if (!commit)
        {
            return cue::Result<std::optional<ShippingBuildProvenance>>::failure(std::move(*commit.try_error()));
        }
        if (!commit.try_value()->has_value())
        {
            return cue::Result<std::optional<ShippingBuildProvenance>>::success(std::nullopt);
        }
        commitText = std::move(**commit.try_value());
    }
    const bool validCommit =
        (commitText.size() == 40U || commitText.size() == 64U) &&
        std::all_of(commitText.begin(), commitText.end(), [](char a_value)
                    { return (a_value >= '0' && a_value <= '9') || (a_value >= 'a' && a_value <= 'f'); });
    const auto isCanonicalSha256 = [](std::string_view a_value) noexcept
    {
        return a_value.size() == 64U &&
               std::all_of(a_value.begin(), a_value.end(), [](char a_character)
                           {
                               return (a_character >= '0' && a_character <= '9') ||
                                      (a_character >= 'a' && a_character <= 'f');
                           });
    };
    if (!validCommit || (a_installedEngineSource &&
                         (!isCanonicalSha256(a_installedEngineSource->sourceInventoryHash) ||
                          !isCanonicalSha256(a_installedEngineSource->publisherBuildIdentityDigest))))
    {
        return cue::Result<std::optional<ShippingBuildProvenance>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                       "Engine Git commit provenance is invalid"));
    }
    if (!a_installedEngineSource)
    {
        cue::Result<std::optional<std::string>> status =
            read_git_output(a_processRunner, *engineRoot,
                            {"status", "--porcelain=v1", "--untracked-files=all", "--", "Engine/Source", "CMake",
                             "CMakeLists.txt", "CMakePresets.json", "ThirdParty/vcpkg.json",
                             "ThirdParty/vcpkg-configuration.json", "ThirdParty/vcpkg-tool.json"},
                            a_cancellation, a_deadline, a_assertContext);
        if (!status)
        {
            return cue::Result<std::optional<ShippingBuildProvenance>>::failure(std::move(*status.try_error()));
        }
        if (!status.try_value()->has_value())
        {
            return cue::Result<std::optional<ShippingBuildProvenance>>::success(std::nullopt);
        }
        statusText = std::move(**status.try_value());
    }

    const std::array<std::filesystem::path, 2U> engineDirectories = {"Engine/Source", "CMake"};
    const std::array<std::filesystem::path, 5U> engineFiles = {
        "CMakeLists.txt", "CMakePresets.json", "ThirdParty/vcpkg.json", "ThirdParty/vcpkg-configuration.json",
        "ThirdParty/vcpkg-tool.json"};
    const std::array<std::filesystem::path, 1U> gameDirectories = {"Source/Game"};
    const std::array<std::filesystem::path, 3U> gameFiles = {"CMakeLists.txt", "CMakePresets.json", "CueProject.json"};
    cue::Result<std::optional<SourceInventoryIdentity>> engineSource =
        collect_source_inventory(*engineRoot, engineDirectories, engineFiles, a_cancellation, a_assertContext);
    if (!engineSource)
    {
        return cue::Result<std::optional<ShippingBuildProvenance>>::failure(std::move(*engineSource.try_error()));
    }
    if (!engineSource.try_value()->has_value())
    {
        return cue::Result<std::optional<ShippingBuildProvenance>>::success(std::nullopt);
    }
    cue::Result<std::optional<SourceInventoryIdentity>> gameSource =
        collect_source_inventory(a_projectRoot, gameDirectories, gameFiles, a_cancellation, a_assertContext);
    if (!gameSource)
    {
        return cue::Result<std::optional<ShippingBuildProvenance>>::failure(std::move(*gameSource.try_error()));
    }
    if (!gameSource.try_value()->has_value())
    {
        return cue::Result<std::optional<ShippingBuildProvenance>>::success(std::nullopt);
    }
    cue::Result<cue::BuildArtifactFile> vcpkgManifest =
        hash_file(*engineRoot / "ThirdParty/vcpkg.json", "ThirdParty/vcpkg.json",
                  cue::BuildArtifactFilePurpose::Unspecified, a_assertContext);
    cue::Result<cue::BuildArtifactFile> vcpkgBaseline =
        hash_file(*engineRoot / "ThirdParty/vcpkg-configuration.json", "ThirdParty/vcpkg-configuration.json",
                  cue::BuildArtifactFilePurpose::Unspecified, a_assertContext);
    if (!vcpkgManifest)
    {
        return cue::Result<std::optional<ShippingBuildProvenance>>::failure(std::move(*vcpkgManifest.try_error()));
    }
    if (!vcpkgBaseline)
    {
        return cue::Result<std::optional<ShippingBuildProvenance>>::failure(std::move(*vcpkgBaseline.try_error()));
    }
    if (a_cancellation.is_cancel_requested())
    {
        return cue::Result<std::optional<ShippingBuildProvenance>>::success(std::nullopt);
    }
    ShippingBuildProvenance provenance;
    provenance.engineCommit = std::move(commitText);
    provenance.engineDirty = !statusText.empty();
    provenance.engineSource = std::move(**engineSource.try_value());
    provenance.gameSource = std::move(**gameSource.try_value());
    provenance.vcpkgManifestHash = std::move(vcpkgManifest.try_value()->contentHash);
    provenance.vcpkgBaselineHash = std::move(vcpkgBaseline.try_value()->contentHash);
    if (a_installedEngineSource)
    {
        provenance.distributionSourceInventoryHash = a_installedEngineSource->sourceInventoryHash;
        provenance.publisherBuildIdentityDigest = a_installedEngineSource->publisherBuildIdentityDigest;
    }
    return cue::Result<std::optional<ShippingBuildProvenance>>::success(
        std::optional<ShippingBuildProvenance>(std::move(provenance)));
}

/// @brief 前回Probeの完了Markerを除去し新しい検証との混同を防ぐ
[[nodiscard]] cue::Result<void> remove_probe_marker(const std::filesystem::path &a_path,
                                                    const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code error;
    static_cast<void>(std::filesystem::remove(a_path, error));
    if (error)
    {
        return cue::Result<void>::failure(make_windows_error(
            a_assertContext, cue::WindowsBuildArtifactError::ModuleContractMismatch, static_cast<DWORD>(error.value()),
            "Artifact probe completion marker could not be removed"));
    }
    return cue::Result<void>::success();
}

/// @brief Probeが全検証後に作成した固定内容のRegular Fileだけを完了通知として認める
[[nodiscard]] bool probe_marker_matches(const std::filesystem::path &a_path, std::string_view a_expected) noexcept
{
    std::error_code statusError;
    const std::filesystem::file_status status = std::filesystem::symlink_status(a_path, statusError);
    if (statusError || !std::filesystem::is_regular_file(status))
    {
        return false;
    }
    std::error_code sizeError;
    if (std::filesystem::file_size(a_path, sizeError) != a_expected.size() || sizeError)
    {
        return false;
    }
    std::ifstream input(a_path, std::ios::binary);
    std::string bytes(a_expected.size(), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return input && bytes == a_expected;
}

/// @brief Candidate DLLの公開ABIを取消／Timeout可能な別Processで検証する
[[nodiscard]] cue::Result<ArtifactProbeStatus> validate_module(
    const std::filesystem::path &a_path, const cue::BuildPlan &a_plan, std::string_view a_projectId,
    const std::filesystem::path &a_markerDirectory, std::string_view a_probeExecutable,
    cue::ChildProcessRunner &a_processRunner, const cue::ChildProcessCancellation &a_cancellation,
    cue::BuildArtifactLockDeadline a_deadline, const cue::AssertContext &a_assertContext) noexcept
{
    const std::optional<std::chrono::milliseconds> timeout = remaining_timeout(a_deadline);
    if (timeout && timeout->count() <= 0)
    {
        return cue::Result<ArtifactProbeStatus>::failure(
            make_artifact_probe_timeout_error(a_assertContext, "Game Module ABI probe timed out"));
    }
    const std::filesystem::path marker = a_markerDirectory / ".probe-complete";
    cue::Result<void> staleMarkerRemoved = remove_probe_marker(marker, a_assertContext);
    if (!staleMarkerRemoved)
    {
        return cue::Result<ArtifactProbeStatus>::failure(std::move(*staleMarkerRemoved.try_error()));
    }
    /// @brief Probe失敗へMarker Cleanup診断を追加して返す
    const auto failProbe = [&](cue::Error a_error) -> cue::Result<ArtifactProbeStatus>
    {
        cue::Result<void> markerRemoved = remove_probe_marker(marker, a_assertContext);
        if (!markerRemoved)
        {
            a_error.append_secondary_diagnostics(a_assertContext, *markerRemoved.try_error(),
                                                 "Probe completion marker cleanup failed", "Probe cleanup");
        }
        return cue::Result<ArtifactProbeStatus>::failure(std::move(a_error));
    };
    cue::ChildProcessRequest request(std::string(a_probeExecutable),
                                     {path_to_utf8(a_path),
                                      std::string(configuration_name(a_plan.profile().configuration())),
                                      std::string(a_projectId), path_to_utf8(marker)},
                                     path_to_utf8(a_path.parent_path()), {}, timeout, 0U);
    auto process = a_processRunner.run(request, a_cancellation);
    if (!process)
    {
        return failProbe(std::move(*process.try_error()));
    }
    switch (process.try_value()->outcome())
    {
    case cue::ChildProcessOutcome::Cancelled:
    {
        cue::Result<void> markerRemoved = remove_probe_marker(marker, a_assertContext);
        if (!markerRemoved)
        {
            return cue::Result<ArtifactProbeStatus>::failure(std::move(*markerRemoved.try_error()));
        }
        return cue::Result<ArtifactProbeStatus>::success(ArtifactProbeStatus::Cancelled);
    }
    case cue::ChildProcessOutcome::TimedOut:
        return failProbe(make_artifact_probe_timeout_error(a_assertContext, "Game Module ABI probe timed out"));
    case cue::ChildProcessOutcome::Exited:
        break;
    }
    if (!process.try_value()->exit_code() || *process.try_value()->exit_code() != 0U)
    {
        std::string summary("Game Module ABI probe rejected the candidate");
        if (process.try_value()->exit_code())
        {
            summary.append(" with exit code ");
            summary.append(std::to_string(*process.try_value()->exit_code()));
        }
        return failProbe(make_error(a_assertContext, cue::WindowsBuildArtifactError::ModuleContractMismatch, summary));
    }
    const bool isComplete = probe_marker_matches(marker, k_moduleProbeCompletionMarker);
    cue::Result<void> markerRemoved = remove_probe_marker(marker, a_assertContext);
    if (!isComplete)
    {
        cue::Error error = make_error(a_assertContext, cue::WindowsBuildArtifactError::ModuleContractMismatch,
                                      "Game Module ABI probe exited without a valid completion marker");
        if (!markerRemoved)
        {
            error.append_secondary_diagnostics(a_assertContext, *markerRemoved.try_error(),
                                               "Probe completion marker cleanup failed", "Probe cleanup");
        }
        return cue::Result<ArtifactProbeStatus>::failure(std::move(error));
    }
    if (!markerRemoved)
    {
        return cue::Result<ArtifactProbeStatus>::failure(std::move(*markerRemoved.try_error()));
    }
    return cue::Result<ArtifactProbeStatus>::success(ArtifactProbeStatus::Valid);
}

/// @brief Candidate ProductのStatic Startup契約を取消／Timeout可能な別Processで検証する
[[nodiscard]] cue::Result<ArtifactProbeStatus> validate_product(
    const std::filesystem::path &a_path, std::string_view a_projectId, const std::filesystem::path &a_markerDirectory,
    cue::ChildProcessRunner &a_processRunner, const cue::ChildProcessCancellation &a_cancellation,
    cue::BuildArtifactLockDeadline a_deadline, const cue::AssertContext &a_assertContext) noexcept
{
    const std::optional<std::chrono::milliseconds> timeout = remaining_timeout(a_deadline);
    if (timeout && timeout->count() <= 0)
    {
        return cue::Result<ArtifactProbeStatus>::failure(
            make_artifact_probe_timeout_error(a_assertContext, "Game Product startup probe timed out"));
    }
    const std::filesystem::path marker = a_markerDirectory / ".probe-complete";
    cue::Result<void> staleMarkerRemoved = remove_probe_marker(marker, a_assertContext);
    if (!staleMarkerRemoved)
    {
        return cue::Result<ArtifactProbeStatus>::failure(std::move(*staleMarkerRemoved.try_error()));
    }
    const auto failProbe = [&](cue::Error a_error) -> cue::Result<ArtifactProbeStatus>
    {
        cue::Result<void> markerRemoved = remove_probe_marker(marker, a_assertContext);
        if (!markerRemoved)
        {
            a_error.append_secondary_diagnostics(a_assertContext, *markerRemoved.try_error(),
                                                 "Probe completion marker cleanup failed", "Probe cleanup");
        }
        return cue::Result<ArtifactProbeStatus>::failure(std::move(a_error));
    };
    cue::ChildProcessRequest request(path_to_utf8(a_path),
                                     {"--cue-artifact-probe", "Release", std::string(a_projectId)},
                                     path_to_utf8(a_markerDirectory), {}, timeout, 0U);
    auto process = a_processRunner.run(request, a_cancellation);
    if (!process)
    {
        return failProbe(std::move(*process.try_error()));
    }
    switch (process.try_value()->outcome())
    {
    case cue::ChildProcessOutcome::Cancelled:
    {
        cue::Result<void> markerRemoved = remove_probe_marker(marker, a_assertContext);
        if (!markerRemoved)
        {
            return cue::Result<ArtifactProbeStatus>::failure(std::move(*markerRemoved.try_error()));
        }
        return cue::Result<ArtifactProbeStatus>::success(ArtifactProbeStatus::Cancelled);
    }
    case cue::ChildProcessOutcome::TimedOut:
        return failProbe(make_artifact_probe_timeout_error(a_assertContext, "Game Product startup probe timed out"));
    case cue::ChildProcessOutcome::Exited:
        break;
    }
    if (!process.try_value()->exit_code() || *process.try_value()->exit_code() != 0U)
    {
        std::string summary("Game Product startup probe rejected the candidate");
        if (process.try_value()->exit_code())
        {
            summary.append(" with exit code ");
            summary.append(std::to_string(*process.try_value()->exit_code()));
        }
        return failProbe(make_error(a_assertContext, cue::WindowsBuildArtifactError::ModuleContractMismatch, summary));
    }
    const bool isComplete = probe_marker_matches(marker, k_productProbeCompletionMarker);
    cue::Result<void> markerRemoved = remove_probe_marker(marker, a_assertContext);
    if (!isComplete)
    {
        cue::Error error = make_error(a_assertContext, cue::WindowsBuildArtifactError::ModuleContractMismatch,
                                      "Game Product startup probe exited without a valid completion marker");
        if (!markerRemoved)
        {
            error.append_secondary_diagnostics(a_assertContext, *markerRemoved.try_error(),
                                               "Probe completion marker cleanup failed", "Probe cleanup");
        }
        return cue::Result<ArtifactProbeStatus>::failure(std::move(error));
    }
    if (!markerRemoved)
    {
        return cue::Result<ArtifactProbeStatus>::failure(std::move(*markerRemoved.try_error()));
    }
    return cue::Result<ArtifactProbeStatus>::success(ArtifactProbeStatus::Valid);
}

/// @brief Metadata v1を決定的なUTF-8 JSONへ直列化する
[[nodiscard]] std::string serialize_metadata(std::string_view a_artifactId, std::string_view a_projectId,
                                             const cue::EngineCompatibility &a_compatibility,
                                             cue::BuildConfiguration a_configuration,
                                             const cue::BuildToolVersion &a_toolsetVersion)
{
    const std::uint64_t compilerVersion =
        static_cast<std::uint64_t>(a_toolsetVersion.major) * 100U + static_cast<std::uint64_t>(a_toolsetVersion.minor);
    const std::uint64_t fullVersion = compilerVersion * 100000U + static_cast<std::uint64_t>(a_toolsetVersion.patch);
    std::string output;
    output.reserve(768U);
    output.append("{\n    \"schemaVersion\": 1,\n    \"artifactId\": \"");
    output.append(a_artifactId);
    output.append("\",\n    \"projectId\": \"");
    output.append(a_projectId);
    output.append("\",\n    \"engineCompatibility\": {\n        \"minimum\": \"");
    output.append(version_text(a_compatibility.minimum));
    output.append("\",\n        \"maximumExclusive\": ");
    if (a_compatibility.maximumExclusive)
    {
        output.push_back('"');
        output.append(version_text(*a_compatibility.maximumExclusive));
        output.push_back('"');
    }
    else
    {
        output.append("null");
    }
    output.append("\n    },\n    \"abiVersion\": 1,\n    \"configuration\": \"");
    output.append(configuration_name(a_configuration));
    output.append("\",\n    \"architecture\": \"x64\",\n    \"compilerFamily\": \"msvc\",\n"
                  "    \"msvcToolset\": {\n        \"compilerVersion\": ");
    output.append(std::to_string(compilerVersion));
    output.append(",\n        \"fullVersion\": ");
    output.append(std::to_string(fullVersion));
    output.append(",\n        \"build\": ");
    output.append(std::to_string(a_toolsetVersion.build));
    output.append("\n    },\n    \"runtimeLibrary\": \"");
    output.append(a_configuration == cue::BuildConfiguration::Debug ? "DebugDll" : "Dll");
    output.append("\",\n    \"iteratorDebugLevel\": ");
    output.append(a_configuration == cue::BuildConfiguration::Debug ? "2" : "0");
    output.append(",\n    \"moduleFile\": \"CueGameModule.dll\",\n"
                  "    \"entrySymbol\": \"cue_game_module_query\"\n}\n");
    return output;
}

/// @brief Shipping Product Metadata v1をBuild由来情報とPayload Hashから決定的に直列化する
[[nodiscard]] std::string serialize_product_metadata(
    std::string_view a_artifactId, std::string_view a_projectId, const cue::EngineCompatibility &a_compatibility,
    const cue::BuildPlan &a_plan, const ShippingBuildProvenance &a_provenance,
    const ShippingToolchainIdentity &a_toolchain, const cue::WindowsProductSecurityValidation &a_security,
    const cue::BuildArtifactFile &a_product, const std::optional<cue::BuildArtifactFile> &a_symbol)
{
    const cue::BuildToolVersion &toolsetVersion = a_plan.workspace_compatibility().toolsetVersion;
    const std::uint64_t compilerVersion =
        static_cast<std::uint64_t>(toolsetVersion.major) * 100U + static_cast<std::uint64_t>(toolsetVersion.minor);
    const std::uint64_t fullVersion = compilerVersion * 100000U + static_cast<std::uint64_t>(toolsetVersion.patch);
    std::string output;
    output.reserve(2200U);
    output.append("{\n    \"schemaVersion\": 1,\n    \"artifactId\": \"");
    output.append(a_artifactId);
    output.append("\",\n    \"target\": \"ShippingProduct\",\n    \"projectId\": \"");
    output.append(a_projectId);
    output.append("\",\n    \"configuration\": \"Release\",\n    \"architecture\": \"x64\",\n"
                  "    \"engineCompatibility\": {\n        \"minimum\": \"");
    output.append(version_text(a_compatibility.minimum));
    output.append("\",\n        \"maximumExclusive\": ");
    if (a_compatibility.maximumExclusive)
    {
        output.push_back('"');
        output.append(version_text(*a_compatibility.maximumExclusive));
        output.push_back('"');
    }
    else
    {
        output.append("null");
    }
    output.append("\n    },\n    \"buildProvenance\": {\n        \"engineVersion\": \"");
    output.append(cue::build_metadata::k_engineVersion);
    output.append("\",\n        \"engineCommit\": \"");
    output.append(a_provenance.engineCommit);
    output.append("\",\n        \"engineSourceTreeState\": \"");
    output.append(a_provenance.engineDirty ? "dirty" : "clean");
    output.append("\",\n        \"engineSourceOrigin\": \"");
    output.append(a_provenance.distributionSourceInventoryHash.empty() ? "Repository" : "InstalledDistribution");
    output.append("\",\n        \"distributionSourceInventorySha256\": ");
    if (a_provenance.distributionSourceInventoryHash.empty())
    {
        output.append("null");
    }
    else
    {
        output.push_back('"');
        output.append(a_provenance.distributionSourceInventoryHash);
        output.push_back('"');
    }
    output.append(",\n        \"publisherBuildIdentitySha256\": ");
    if (a_provenance.publisherBuildIdentityDigest.empty())
    {
        output.append("null");
    }
    else
    {
        output.push_back('"');
        output.append(a_provenance.publisherBuildIdentityDigest);
        output.push_back('"');
    }
    output.append(",\n        \"engineSourceInventory\": {\n            \"fileCount\": ");
    output.append(std::to_string(a_provenance.engineSource.fileCount));
    output.append(",\n            \"sha256\": \"");
    output.append(a_provenance.engineSource.hash);
    output.append("\"\n        },\n        \"gameSourceInventory\": {\n            \"fileCount\": ");
    output.append(std::to_string(a_provenance.gameSource.fileCount));
    output.append(",\n            \"sha256\": \"");
    output.append(a_provenance.gameSource.hash);
    output.append("\"\n        },\n        \"cmake\": {\n            \"version\": \"");
    output.append(a_toolchain.cmakeVersion);
    output.append("\",\n            \"generator\": \"");
    output.append(a_toolchain.cmakeGenerator);
    output.append("\"\n        },\n        \"engineBuildPolicyVersion\": ");
    output.append(std::to_string(a_plan.workspace_compatibility().engineBuildPolicyVersion));
    output.append(",\n        \"msvcToolset\": {\n            \"compilerVersion\": ");
    output.append(std::to_string(compilerVersion));
    output.append(",\n            \"fullVersion\": ");
    output.append(std::to_string(fullVersion));
    output.append(",\n            \"build\": ");
    output.append(std::to_string(toolsetVersion.build));
    output.append("\n        },\n        \"platformToolset\": \"");
    output.append(a_toolchain.platformToolset);
    output.append("\",\n        \"msvcToolsetVersion\": \"");
    output.append(a_toolchain.msvcToolsetVersion);
    output.append("\",\n        \"compilerSha256\": \"");
    output.append(a_toolchain.compilerSha256);
    output.append("\",\n        \"compilerFileVersion\": \"");
    output.append(a_toolchain.compilerFileVersion);
    output.append("\",\n        \"windowsSdkVersion\": \"");
    output.append(a_toolchain.windowsSdkVersion);
    output.append("\",\n        \"architecture\": \"x64\",\n        \"configuration\": \"Release\",\n"
                  "        \"vcpkgManifestSha256\": \"");
    output.append(a_provenance.vcpkgManifestHash);
    output.append("\",\n        \"vcpkgBaselineSha256\": \"");
    output.append(a_provenance.vcpkgBaselineHash);
    output.append("\"\n    },\n    \"minimumTrustMode\": \"");
    output.append(trust_mode_name(*a_plan.profile().minimum_trust_mode()));
    output.append("\",\n    \"publisherKeyId\": ");
    if (a_plan.profile().publisher_key_id().empty())
    {
        output.append("null");
    }
    else
    {
        output.push_back('"');
        output.append(a_plan.profile().publisher_key_id());
        output.push_back('"');
    }
    output.append(",\n    \"securityValidation\": {\n        \"policyVersion\": 1,\n"
                  "        \"machine\": \"x64\",\n        \"aslr\": true,\n"
                  "        \"highEntropyVa\": true,\n        \"dep\": true,\n"
                  "        \"controlFlowGuard\": true,\n        \"cetCompatible\": true,\n"
                  "        \"stackSecurityCheck\": true,\n"
                  "        \"dependentLoadFlags\": \"0x0800\",\n"
                  "        \"importPolicy\": \"M17AllowlistV1\",\n"
                  "        \"gameModuleLoaderLinked\": false,\n"
                  "        \"importedLibraries\": [");
    for (std::size_t index = 0U; index < a_security.importedLibraries.size(); ++index)
    {
        if (index != 0U)
        {
            output.append(", ");
        }
        output.push_back('"');
        output.append(a_security.importedLibraries[index]);
        output.push_back('"');
    }
    output.append("],\n        \"signatureStatus\": \"");
    output.append(product_signature_status_name(a_security.trustEvidence.signatureStatus));
    output.append("\",\n        \"distributionStatus\": \"");
    output.append(product_distribution_status_name(a_security.distributionStatus));
    output.append("\",\n        \"publicDistributionReady\": false\n    },\n    \"product\": {\n        \"path\": \"");
    output.append(a_product.relativePath);
    output.append("\",\n        \"sizeBytes\": ");
    output.append(std::to_string(a_product.byteSize));
    output.append(",\n        \"hashAlgorithm\": \"sha256\",\n        \"contentHash\": \"");
    output.append(a_product.contentHash);
    output.append("\"\n    },\n    \"developmentSymbol\": ");
    if (a_symbol)
    {
        output.append("{\n        \"path\": \"");
        output.append(a_symbol->relativePath);
        output.append("\",\n        \"sizeBytes\": ");
        output.append(std::to_string(a_symbol->byteSize));
        output.append(",\n        \"hashAlgorithm\": \"sha256\",\n        \"contentHash\": \"");
        output.append(a_symbol->contentHash);
        output.append("\"\n    }");
    }
    else
    {
        output.append("null");
    }
    output.append("\n}\n");
    return output;
}

/// @brief Current Manifest v2をArtifact Inventoryから決定的に直列化する
[[nodiscard]] std::string serialize_current(const cue::BuildArtifactInventory &a_inventory)
{
    std::string output;
    output.reserve(768U);
    output.append("{\n    \"schemaVersion\": 2,\n    \"artifactId\": \"");
    output.append(a_inventory.artifact_id());
    output.append("\",\n    \"configuration\": \"");
    output.append(configuration_name(a_inventory.configuration()));
    output.append("\",\n    \"target\": \"");
    output.append(target_name(a_inventory.profile().target()));
    output.append("\",\n    \"minimumTrustMode\": ");
    if (a_inventory.profile().minimum_trust_mode())
    {
        output.push_back('"');
        output.append(trust_mode_name(*a_inventory.profile().minimum_trust_mode()));
        output.push_back('"');
    }
    else
    {
        output.append("null");
    }
    output.append(",\n    \"publisherKeyId\": ");
    if (a_inventory.profile().publisher_key_id().empty())
    {
        output.append("null");
    }
    else
    {
        output.push_back('"');
        output.append(a_inventory.profile().publisher_key_id());
        output.push_back('"');
    }
    output.append(",\n    \"files\": [\n");
    for (std::size_t index = 0U; index < a_inventory.files().size(); ++index)
    {
        const cue::BuildArtifactFile &file = a_inventory.files()[index];
        output.append("        {\n            \"path\": \"");
        output.append(file.relativePath);
        output.append("\",\n            \"sizeBytes\": ");
        output.append(std::to_string(file.byteSize));
        output.append(",\n            \"hashAlgorithm\": \"sha256\",\n            \"contentHash\": \"");
        output.append(file.contentHash);
        output.append("\",\n            \"purpose\": \"");
        output.append(file_purpose_name(file.purpose));
        output.append("\"\n        }");
        output.append(index + 1U == a_inventory.files().size() ? "\n" : ",\n");
    }
    output.append("    ]\n}\n");
    return output;
}

enum class ArtifactTextKind : std::uint8_t
{
    CurrentManifest,
    RuntimeMetadata
};

/// @brief Artifact Text FileをRegular File Handleから種別別上限付きで未変換読込する
[[nodiscard]] cue::Result<std::string> read_guarded_artifact_text(const std::filesystem::path &a_path,
                                                                  std::uint64_t a_maximumBytes, ArtifactTextKind a_kind,
                                                                  const cue::AssertContext &a_assertContext) noexcept
{
    UniqueHandle file(CreateFileW(native_path(a_path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                                  nullptr));
    if (!file.is_valid())
    {
        return cue::Result<std::string>::failure(make_windows_error(
            a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed, GetLastError(),
            a_kind == ArtifactTextKind::CurrentManifest ? "Current artifact manifest could not be opened"
                                                        : "Runtime Metadata could not be opened"));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(file.get(), &information) == FALSE)
    {
        return cue::Result<std::string>::failure(make_windows_error(
            a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed, GetLastError(),
            a_kind == ArtifactTextKind::CurrentManifest ? "Current artifact manifest attributes could not be read"
                                                        : "Runtime Metadata attributes could not be read"));
    }
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0U)
    {
        return cue::Result<std::string>::failure(make_windows_error(
            a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed, ERROR_FILE_INVALID,
            a_kind == ArtifactTextKind::CurrentManifest ? "Current artifact manifest is not a regular file"
                                                        : "Runtime Metadata is not a regular file"));
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == FALSE)
    {
        return cue::Result<std::string>::failure(make_windows_error(
            a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed, GetLastError(),
            a_kind == ArtifactTextKind::CurrentManifest ? "Current artifact manifest size could not be read"
                                                        : "Runtime Metadata size could not be read"));
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > a_maximumBytes)
    {
        return cue::Result<std::string>::failure(make_windows_error(
            a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed, ERROR_FILE_TOO_LARGE,
            a_kind == ArtifactTextKind::CurrentManifest ? "Current artifact manifest size is invalid"
                                                        : "Runtime Metadata size is invalid"));
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0U;
    while (offset < bytes.size())
    {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, MAXDWORD));
        DWORD read = 0U;
        if (ReadFile(file.get(), bytes.data() + offset, request, &read, nullptr) == FALSE || read == 0U)
        {
            const DWORD code = read == 0U ? ERROR_HANDLE_EOF : GetLastError();
            return cue::Result<std::string>::failure(make_windows_error(
                a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed, code,
                a_kind == ArtifactTextKind::CurrentManifest ? "Current artifact manifest could not be read"
                                                            : "Runtime Metadata could not be read"));
        }
        offset += read;
    }
    return cue::Result<std::string>::success(std::move(bytes));
}

/// @brief Current Manifestを上限付きで未変換読込する
[[nodiscard]] cue::Result<std::string> read_current_manifest(const std::filesystem::path &a_path,
                                                             const cue::AssertContext &a_assertContext) noexcept
{
    return read_guarded_artifact_text(a_path, k_maximumCurrentManifestBytes, ArtifactTextKind::CurrentManifest,
                                      a_assertContext);
}

/// @brief Runtime Metadataの検証済みByte列からProject IDを取得する
[[nodiscard]] cue::Result<std::string> read_runtime_metadata_project_id(
    const std::filesystem::path &a_version, const cue::BuildArtifactInventory &a_inventory,
    const cue::AssertContext &a_assertContext) noexcept
{
    const cue::BuildArtifactFile *metadata = nullptr;
    for (const cue::BuildArtifactFile &file : a_inventory.files())
    {
        if (file.purpose != cue::BuildArtifactFilePurpose::RuntimeMetadata)
        {
            continue;
        }
        if (metadata != nullptr)
        {
            return cue::Result<std::string>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed,
                           "Visible artifact contains multiple Runtime Metadata files"));
        }
        metadata = &file;
    }
    const std::optional<std::filesystem::path> relative =
        metadata == nullptr ? std::nullopt : to_path(metadata->relativePath);
    if (metadata == nullptr || !relative || metadata->byteSize == 0U ||
        metadata->byteSize > k_maximumRuntimeMetadataBytes)
    {
        return cue::Result<std::string>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed,
                       "Visible artifact Runtime Metadata is unavailable or outside the supported limit"));
    }

    cue::Result<std::string> bytes = read_guarded_artifact_text(a_version / *relative, k_maximumRuntimeMetadataBytes,
                                                                ArtifactTextKind::RuntimeMetadata, a_assertContext);
    if (!bytes)
    {
        return cue::Result<std::string>::failure(std::move(*bytes.try_error()));
    }
    cue::Result<std::string> hash = hash_bytes(*bytes.try_value(), a_assertContext);
    if (!hash)
    {
        return cue::Result<std::string>::failure(std::move(*hash.try_error()));
    }
    if (bytes.try_value()->size() != metadata->byteSize || *hash.try_value() != metadata->contentHash)
    {
        return cue::Result<std::string>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed,
                       "Visible artifact Runtime Metadata differs from its inventory"));
    }

    std::optional<std::string> projectId = parse_runtime_metadata_project_id(*bytes.try_value(), a_inventory);
    if (!projectId)
    {
        return cue::Result<std::string>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed,
                       "Visible artifact Runtime Metadata has no unique top-level Project identity"));
    }
    cue::Result<cue::ProjectId> parsed = cue::ProjectId::parse(*projectId, a_assertContext);
    if (!parsed)
    {
        return cue::Result<std::string>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed,
                       "Visible artifact Runtime Metadata Project identity is invalid"));
    }
    return cue::Result<std::string>::success(std::string(parsed.try_value()->text()));
}

/// @brief Current.jsonをSibling Temporary FileからAtomic Replaceする
[[nodiscard]] cue::Result<bool> publish_current(const std::filesystem::path &a_store, std::string_view a_operationId,
                                                std::string_view a_content,
                                                const cue::ChildProcessCancellation &a_cancellation,
                                                const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path temporary = a_store / (".Current-" + std::string(a_operationId) + ".tmp");
    const std::filesystem::path current = a_store / "Current.json";
    cue::Result<void> written =
        write_new_file(temporary, a_content, cue::WindowsBuildArtifactError::CurrentManifestFailed, a_assertContext);
    if (!written)
    {
        return cue::Result<bool>::failure(std::move(*written.try_error()));
    }
    if (a_cancellation.is_cancel_requested())
    {
        if (DeleteFileW(temporary.c_str()) == FALSE)
        {
            return cue::Result<bool>::failure(
                make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed,
                                   GetLastError(), "Cancelled Current artifact temporary manifest rollback failed"));
        }
        return cue::Result<bool>::success(false);
    }
    if (MoveFileExW(temporary.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        const DWORD code = GetLastError();
        std::ifstream visible(current, std::ios::binary);
        const std::string visibleBytes{std::istreambuf_iterator<char>(visible), std::istreambuf_iterator<char>()};
        const bool newManifestVisible = visible.is_open() && !visible.bad() && visibleBytes == a_content;
        std::optional<cue::Error> cleanupError;
        if (DeleteFileW(temporary.c_str()) == FALSE)
        {
            const DWORD cleanupCode = GetLastError();
            if (cleanupCode != ERROR_FILE_NOT_FOUND && cleanupCode != ERROR_PATH_NOT_FOUND)
            {
                cleanupError.emplace(
                    make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed,
                                       cleanupCode, "Current artifact temporary manifest rollback failed"));
            }
        }
        cue::Error publicationError =
            newManifestVisible
                ? make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestDurabilityUnknown,
                                     code, "Current artifact manifest is visible but durability is unknown")
                : make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::CurrentManifestFailed, code,
                                     "Current artifact manifest was not published");
        if (cleanupError)
        {
            publicationError.append_secondary_diagnostics(
                a_assertContext, *cleanupError, "Current temporary manifest could not be removed", "Rollback");
        }
        return cue::Result<bool>::failure(std::move(publicationError));
    }
    return cue::Result<bool>::success(true);
}

/// @brief Shared Byte Range LockをBuild Artifact Reader契約へ公開するRAII Token
class WindowsBuildArtifactReadLease final : public cue::BuildArtifactReadLease
{
  public:
    /// @brief Shared LockとStoreへBindingされたProject Identityの所有権を取得する
    WindowsBuildArtifactReadLease(GuardedByteRangeLock a_lock, std::string a_projectId) noexcept
        : m_lease(std::move(a_lock)), m_projectId(std::move(a_projectId))
    {
    }
    /// @brief Shared Byte Range Lockを解放する
    ~WindowsBuildArtifactReadLease() override = default;
    /// @brief Lease発行元StoreのProject Identityを返す
    [[nodiscard]] std::string_view project_id() const noexcept override
    {
        return m_projectId;
    }

  private:
    ByteRangeLease m_lease;
    std::string m_projectId;
};

/// @brief Current Artifactを同じShared Read Lease内で再検証するWindows Reader
class WindowsBuildArtifactReader final : public cue::BuildArtifactReader
{
  public:
    /// @brief Project RootとIdentityをReaderへ固定する
    WindowsBuildArtifactReader(std::filesystem::path a_projectRoot, std::string a_projectId,
                               const cue::AssertContext &a_assertContext) noexcept
        : m_projectRoot(std::move(a_projectRoot)), m_projectId(std::move(a_projectId)),
          m_assertContext(&a_assertContext)
    {
    }
    /// @brief Readerと未返却Resourceを解放する
    ~WindowsBuildArtifactReader() override = default;

    /// @brief Current再読込からInventory再検証までShared Lockを保持してLeaseを返す
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>> acquire_current_read_lease(
        const cue::BuildArtifactInventory &a_expected, const cue::BuildArtifactReadCancellation &a_cancellation,
        cue::BuildArtifactLockDeadline a_deadline) noexcept override
    {
        try
        {
            const std::string_view configuration = configuration_name(a_expected.configuration());
            const std::optional<std::filesystem::path> versionPath = to_path(a_expected.version_directory());
            const std::filesystem::path currentStore = artifact_store_path(m_projectRoot, a_expected.profile());
            const std::filesystem::path legacyStore =
                (m_projectRoot / "Generated" / "Artifacts" / configuration).lexically_normal();
            const std::filesystem::path currentVersion =
                (currentStore / "Versions" / a_expected.artifact_id()).lexically_normal();
            const std::filesystem::path legacyVersion =
                (legacyStore / "Versions" / a_expected.artifact_id()).lexically_normal();
            const std::filesystem::path normalizedVersion =
                versionPath ? versionPath->lexically_normal() : std::filesystem::path{};
            const bool usesCurrentStore = versionPath && normalizedVersion == currentVersion;
            const bool usesLegacyStore = versionPath && a_expected.profile().target() == cue::BuildTarget::GameModule &&
                                         normalizedVersion == legacyVersion;
            if (m_projectId.empty() || configuration.empty() || !versionPath || (!usesCurrentStore && !usesLegacyStore))
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Artifact Read Lease input is not bound to this Project store"));
            }
            const std::filesystem::path &store = usesCurrentStore ? currentStore : legacyStore;
            cue::Result<std::optional<GuardedByteRangeLock>> lock =
                acquire_shared_lock(m_projectRoot, store / "Access.lock", a_cancellation, a_deadline,
                                    cue::WindowsBuildArtifactError::ArtifactLockFailed, *m_assertContext);
            if (!lock)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::failure(
                    std::move(*lock.try_error()));
            }
            if (!lock.try_value()->has_value())
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::success(std::nullopt);
            }
            cue::Result<std::string> current = read_current_manifest(store / "Current.json", *m_assertContext);
            if (!current)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::failure(
                    std::move(*current.try_error()));
            }
            cue::Result<void> currentValidated =
                cue::validate_build_artifact_current_manifest(*current.try_value(), a_expected, *m_assertContext);
            if (!currentValidated)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::failure(
                    std::move(*currentValidated.try_error()));
            }
            cue::Result<void> verified = verify_inventory_files(normalizedVersion, a_expected, *m_assertContext);
            if (!verified)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::failure(
                    std::move(*verified.try_error()));
            }
            cue::Result<std::string> artifactProjectId =
                read_runtime_metadata_project_id(normalizedVersion, a_expected, *m_assertContext);
            if (!artifactProjectId)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::failure(
                    std::move(*artifactProjectId.try_error()));
            }
            if (*artifactProjectId.try_value() != m_projectId)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Visible artifact Project identity does not match the reader Project"));
            }
            std::unique_ptr<cue::BuildArtifactReadLease> lease = std::make_unique<WindowsBuildArtifactReadLease>(
                std::move(**lock.try_value()), std::move(*artifactProjectId.try_value()));
            return cue::Result<std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>>::success(
                std::optional<std::unique_ptr<cue::BuildArtifactReadLease>>(std::move(lease)));
        }
        catch (...)
        {
            terminate_artifact_exception(*m_assertContext);
        }
    }

  private:
    std::filesystem::path m_projectRoot;
    std::string m_projectId;
    const cue::AssertContext *m_assertContext;
};

/// @brief Windows上でBuild CandidateとArtifact Storeを公開する
class WindowsBuildArtifactPublisher final : public cue::BuildArtifactPublisher
{
  public:
    /// @brief 検証済みProject契約をPublisher全寿命へ所有する
    WindowsBuildArtifactPublisher(std::filesystem::path a_projectRoot, std::string a_projectId,
                                  cue::EngineCompatibility a_compatibility, std::string a_probeExecutable,
                                  std::unique_ptr<cue::ChildProcessRunner> a_processRunner,
                                  std::optional<cue::WindowsInstalledEngineSourceProvenance> a_installedEngineSource,
                                  cue::detail::WindowsProductSecuritySnapshotObserver *a_securityObserver,
                                  const cue::AssertContext &a_assertContext) noexcept
        : m_projectRoot(std::move(a_projectRoot)), m_projectId(std::move(a_projectId)),
          m_compatibility(std::move(a_compatibility)), m_probeExecutable(std::move(a_probeExecutable)),
          m_processRunner(std::move(a_processRunner)), m_installedEngineSource(std::move(a_installedEngineSource)),
          m_securityObserver(a_securityObserver),
          m_assertContext(&a_assertContext)
    {
    }
    /// @brief 所有Project契約を解放する
    ~WindowsBuildArtifactPublisher() override = default;

    /// @brief Plan固有Binary TreeのExclusive Byte Range Lockを取消可能に取得する
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>> acquire_build_lease(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation,
        cue::BuildArtifactLockDeadline a_deadline) noexcept override
    {
        try
        {
            if (!m_isAvailable)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::PublisherUnavailable,
                               "Artifact Publisher is unavailable after an unknown Current selection"));
            }
            if (!same_root(m_projectRoot, a_plan.project_root()))
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Build Plan belongs to another Project Root"));
            }
            const std::optional<std::filesystem::path> lock = to_path(a_plan.workspace_lock_file());
            if (!lock)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Build Workspace lock path could not be converted"));
            }
            auto acquired =
                acquire_exclusive_lock(m_projectRoot, *lock, a_cancellation, a_deadline,
                                       cue::WindowsBuildArtifactError::WorkspaceLockFailed, *m_assertContext);
            if (!acquired)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    std::move(*acquired.try_error()));
            }
            if (!acquired.try_value()->has_value())
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(std::nullopt);
            }
            const std::optional<std::filesystem::path> binary = to_path(a_plan.binary_directory());
            if (!binary)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Build Workspace path could not be converted"));
            }
            cue::Result<void> workspaceCreated = ensure_directory(
                m_projectRoot, *binary, cue::WindowsBuildArtifactError::WorkspaceLockFailed, *m_assertContext);
            if (!workspaceCreated)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    std::move(*workspaceCreated.try_error()));
            }
            cue::Result<DirectoryChainGuard> workspaceGuard = acquire_directory_chain_guard(
                m_projectRoot, *binary, cue::WindowsBuildArtifactError::WorkspaceLockFailed, *m_assertContext);
            if (!workspaceGuard)
            {
                return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                    std::move(*workspaceGuard.try_error()));
            }
            std::optional<ShippingBuildProvenance> shippingProvenance;
            if (a_plan.profile().target() == cue::BuildTarget::ShippingProduct)
            {
                cue::Result<std::optional<ShippingBuildProvenance>> collected = collect_shipping_provenance(
                    m_projectRoot, *m_processRunner, a_cancellation, a_deadline, m_installedEngineSource,
                    *m_assertContext);
                if (!collected)
                {
                    return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(
                        std::move(*collected.try_error()));
                }
                if (!collected.try_value()->has_value())
                {
                    return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(std::nullopt);
                }
                shippingProvenance.emplace(std::move(**collected.try_value()));
            }
            std::unique_ptr<cue::BuildWorkspaceLease> lease = std::make_unique<WindowsBuildWorkspaceLease>(
                std::move(**acquired.try_value()), std::move(*workspaceGuard.try_value()),
                std::string(a_plan.workspace_key()), std::move(shippingProvenance));
            return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(
                std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>(std::move(lease)));
        }
        catch (...)
        {
            terminate_artifact_exception(*m_assertContext);
        }
    }

    /// @brief Build出力をCandidateへ確定してから不変VersionとCurrent Manifestを公開する
    [[nodiscard]] cue::Result<std::optional<cue::BuildArtifactInventory>> publish(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation,
        std::unique_ptr<cue::BuildWorkspaceLease> a_buildLease,
        cue::BuildArtifactLockDeadline a_deadline) noexcept override
    {
        try
        {
            if (!m_isAvailable)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::PublisherUnavailable,
                               "Artifact Publisher is unavailable after an unknown Current selection"));
            }
            if (a_plan.profile().target() == cue::BuildTarget::ShippingProduct &&
                a_plan.profile().minimum_trust_mode() == cue::ShippingTrustMode::PublisherSigned)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::PublisherUnavailable,
                               "PublisherSigned artifact publication requires an external signer and sealed source "
                               "snapshot; M17 only publishes UnsignedLocal artifacts"));
            }
            auto *windowsLease = dynamic_cast<WindowsBuildWorkspaceLease *>(a_buildLease.get());
            if (windowsLease == nullptr || !windowsLease->matches(a_plan.workspace_key()) ||
                !same_root(m_projectRoot, a_plan.project_root()))
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Build Workspace Lease does not match the Build Plan"));
            }
            if (a_plan.profile().target() == cue::BuildTarget::ShippingProduct)
            {
                cue::Result<std::optional<ShippingBuildProvenance>> currentProvenance = collect_shipping_provenance(
                    m_projectRoot, *m_processRunner, a_cancellation, a_deadline, m_installedEngineSource,
                    *m_assertContext);
                if (!currentProvenance)
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                        std::move(*currentProvenance.try_error()));
                }
                if (!currentProvenance.try_value()->has_value())
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
                }
                if (!windowsLease->shipping_provenance() ||
                    *windowsLease->shipping_provenance() != **currentProvenance.try_value())
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                        make_error(*m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                                   "Shipping source inputs changed while the Build operation was running"));
                }
            }
            const std::string configuration(configuration_name(a_plan.profile().configuration()));
            const ArtifactLayout layout = artifact_layout(a_plan);
            const bool isShippingProduct = a_plan.profile().target() == cue::BuildTarget::ShippingProduct;
            const std::optional<std::filesystem::path> binary = to_path(a_plan.binary_directory());
            const std::optional<std::filesystem::path> candidatePath = to_path(a_plan.candidate_directory());
            const std::optional<std::filesystem::path> storePath = to_path(a_plan.artifact_store_directory());
            if (!binary || !candidatePath || !storePath)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Build Plan path could not be converted"));
            }
            const std::filesystem::path source = *binary / "bin" / configuration / std::string(layout.payload);
            const std::filesystem::path sourcePdb = *binary / "bin" / configuration / std::string(layout.symbol);
            const std::filesystem::path candidate = *candidatePath;
            cue::Result<void> sourceChain =
                validate_directory_chain(m_projectRoot, source.parent_path(),
                                         cue::WindowsBuildArtifactError::CandidateInvalid, *m_assertContext);
            cue::Result<void> candidateChain =
                validate_directory_chain(m_projectRoot, candidate.parent_path(),
                                         cue::WindowsBuildArtifactError::CandidateInvalid, *m_assertContext);
            cue::Result<void> storeChain = validate_directory_chain(
                m_projectRoot, *storePath, cue::WindowsBuildArtifactError::ArtifactLockFailed, *m_assertContext);
            if (!sourceChain)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*sourceChain.try_error()));
            }
            if (!candidateChain)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*candidateChain.try_error()));
            }
            if (!storeChain)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*storeChain.try_error()));
            }
            std::optional<ShippingToolchainIdentity> shippingToolchain;
            if (isShippingProduct)
            {
                cue::Result<std::optional<ShippingToolchainIdentity>> collectedToolchain =
                    collect_shipping_toolchain_identity(
                        *binary, a_plan, a_cancellation,
                        m_installedEngineSource ? std::string_view(m_installedEngineSource->sourceRoot)
                                                : cue::build_metadata::k_engineSourceRoot,
                        *m_assertContext);
                if (!collectedToolchain)
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                        std::move(*collectedToolchain.try_error()));
                }
                if (!collectedToolchain.try_value()->has_value())
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
                }
                shippingToolchain.emplace(std::move(**collectedToolchain.try_value()));
            }
            cue::Result<void> candidateParentCreated =
                ensure_directory(m_projectRoot, candidate.parent_path(),
                                 cue::WindowsBuildArtifactError::CandidateInvalid, *m_assertContext);
            if (!candidateParentCreated)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*candidateParentCreated.try_error()));
            }
            cue::Result<DirectoryChainGuard> sourceGuard =
                acquire_directory_chain_guard(m_projectRoot, source.parent_path(),
                                              cue::WindowsBuildArtifactError::CandidateInvalid, *m_assertContext);
            cue::Result<DirectoryChainGuard> candidateParentGuard =
                acquire_directory_chain_guard(m_projectRoot, candidate.parent_path(),
                                              cue::WindowsBuildArtifactError::CandidateInvalid, *m_assertContext);
            if (!sourceGuard)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*sourceGuard.try_error()));
            }
            if (!candidateParentGuard)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*candidateParentGuard.try_error()));
            }
            std::error_code filesystemError;
            const std::filesystem::file_status sourceStatus = std::filesystem::symlink_status(source, filesystemError);
            if (filesystemError || !std::filesystem::is_regular_file(sourceStatus))
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                    *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                    filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_FILE_INVALID,
                    "Build artifact payload is not a regular file"));
            }
            const bool hasPdb = std::filesystem::exists(sourcePdb, filesystemError);
            if (filesystemError || (layout.symbolRequired && !hasPdb))
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                    *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                    filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_FILE_NOT_FOUND,
                    "Required build artifact PDB is unavailable"));
            }
            if (hasPdb)
            {
                const std::filesystem::file_status pdbStatus =
                    std::filesystem::symlink_status(sourcePdb, filesystemError);
                if (filesystemError || !std::filesystem::is_regular_file(pdbStatus))
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                        *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                        filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_FILE_INVALID,
                        "Build artifact PDB is not a regular file"));
                }
            }
            if (std::filesystem::exists(candidate, filesystemError) || filesystemError)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(make_windows_error(
                    *m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                    filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_ALREADY_EXISTS,
                    "Operation candidate directory is unavailable"));
            }
            cue::Result<void> candidateCreated = ensure_directory(
                m_projectRoot, candidate, cue::WindowsBuildArtifactError::CandidateInvalid, *m_assertContext);
            if (!candidateCreated)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*candidateCreated.try_error()));
            }
            cue::Result<DirectoryChainGuard> candidateGuardResult = acquire_directory_chain_guard(
                m_projectRoot, candidate, cue::WindowsBuildArtifactError::CandidateInvalid, *m_assertContext, true);
            if (!candidateGuardResult)
            {
                cue::Error acquisitionError = std::move(*candidateGuardResult.try_error());
                const std::filesystem::path nativeCandidate = native_path(candidate);
                if (RemoveDirectoryW(nativeCandidate.c_str()) == FALSE)
                {
                    const DWORD cleanupCode = GetLastError();
                    if (cleanupCode != ERROR_FILE_NOT_FOUND && cleanupCode != ERROR_PATH_NOT_FOUND)
                    {
                        acquisitionError.append_secondary_diagnostics(
                            *m_assertContext,
                            make_windows_error(*m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                                               cleanupCode, "Unguarded empty Candidate rollback failed"),
                            "Candidate guard acquisition failed and the empty Candidate could not be removed",
                            "Rollback");
                    }
                }
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(std::move(acquisitionError));
            }
            std::optional<DirectoryChainGuard> candidateGuard(std::move(*candidateGuardResult.try_value()));
            std::optional<cue::detail::WindowsProductSecuritySnapshot> candidateSecuritySnapshot;
            std::optional<cue::detail::WindowsProductSecuritySnapshot> versionSecuritySnapshot;
            using PublishResult = cue::Result<std::optional<cue::BuildArtifactInventory>>;
            /// @brief Primary Errorを保持したまま未公開CandidateをRollbackする
            const auto failCandidate = [&](cue::Error a_error) -> PublishResult
            {
                candidateSecuritySnapshot.reset();
                versionSecuritySnapshot.reset();
                cue::Result<void> cleanup = delete_guarded_candidate(*candidateGuard, candidate, *m_assertContext);
                candidateGuard.reset();
                if (!cleanup)
                {
                    a_error.append_secondary_diagnostics(*m_assertContext, *cleanup.try_error(),
                                                         "Unpublished Candidate could not be removed", "Rollback");
                }
                return PublishResult::failure(std::move(a_error));
            };
            /// @brief 取消前に未公開CandidateをRollbackしCleanup失敗だけをErrorとして返す
            const auto cancelCandidate = [&]() -> PublishResult
            {
                candidateSecuritySnapshot.reset();
                versionSecuritySnapshot.reset();
                cue::Result<void> cleanup = delete_guarded_candidate(*candidateGuard, candidate, *m_assertContext);
                candidateGuard.reset();
                if (!cleanup)
                {
                    return PublishResult::failure(std::move(*cleanup.try_error()));
                }
                return PublishResult::success(std::nullopt);
            };
            const std::filesystem::path candidatePayload = candidate / std::string(layout.payload);
            cue::Result<void> payloadCopied = copy_new_file_durable(source, candidatePayload, *m_assertContext);
            if (!payloadCopied)
            {
                return failCandidate(std::move(*payloadCopied.try_error()));
            }
            if (hasPdb)
            {
                cue::Result<void> pdbCopied =
                    copy_new_file_durable(sourcePdb, candidate / std::string(layout.symbol), *m_assertContext);
                if (!pdbCopied)
                {
                    return failCandidate(std::move(*pdbCopied.try_error()));
                }
            }
            std::optional<cue::WindowsProductSecurityValidation> productSecurity;
            if (isShippingProduct)
            {
                cue::Result<cue::detail::WindowsProductSecuritySnapshot> imageValidated =
                    cue::detail::validate_windows_shipping_product_security_snapshot(
                        path_to_utf8(candidatePayload), a_plan.profile(), *m_assertContext);
                if (!imageValidated)
                {
                    return failCandidate(std::move(*imageValidated.try_error()));
                }
                productSecurity.emplace(imageValidated.try_value()->validation());
                candidateSecuritySnapshot.emplace(std::move(*imageValidated.try_value()));
            }
            if (m_securityObserver != nullptr && isShippingProduct)
            {
                m_securityObserver->on_snapshot_held(
                    cue::detail::WindowsProductSecuritySnapshotStage::CandidateBeforeProbe, candidatePayload);
            }
            cue::Result<ArtifactProbeStatus> validated =
                isShippingProduct ? validate_product(candidatePayload, m_projectId, candidate, *m_processRunner,
                                                     a_cancellation, a_deadline, *m_assertContext)
                                  : validate_module(candidatePayload, a_plan, m_projectId, candidate, m_probeExecutable,
                                                    *m_processRunner, a_cancellation, a_deadline, *m_assertContext);
            if (!validated)
            {
                return failCandidate(std::move(*validated.try_error()));
            }
            if (*validated.try_value() == ArtifactProbeStatus::Cancelled)
            {
                return cancelCandidate();
            }
            auto candidatePayloadHash = hash_file(candidatePayload, std::string(layout.payload),
                                                  cue::BuildArtifactFilePurpose::DistributionPayload, *m_assertContext);
            if (!candidatePayloadHash)
            {
                return failCandidate(std::move(*candidatePayloadHash.try_error()));
            }
            if (isShippingProduct && (candidatePayloadHash.try_value()->byteSize != productSecurity->byteSize ||
                                      candidatePayloadHash.try_value()->contentHash != productSecurity->contentHash))
            {
                return failCandidate(make_error(*m_assertContext,
                                                cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                                "Candidate payload differs from its verified security snapshot"));
            }
            std::optional<cue::BuildArtifactFile> candidatePdbHash;
            if (hasPdb)
            {
                auto hashed = hash_file(candidate / std::string(layout.symbol), std::string(layout.symbol),
                                        cue::BuildArtifactFilePurpose::DevelopmentSymbol, *m_assertContext);
                if (!hashed)
                {
                    return failCandidate(std::move(*hashed.try_error()));
                }
                candidatePdbHash.emplace(std::move(*hashed.try_value()));
            }
            const std::string metadata =
                isShippingProduct
                    ? serialize_product_metadata(a_plan.operation_id(), m_projectId, m_compatibility, a_plan,
                                                 *windowsLease->shipping_provenance(), *shippingToolchain,
                                                 *productSecurity, *candidatePayloadHash.try_value(), candidatePdbHash)
                    : serialize_metadata(a_plan.operation_id(), m_projectId, m_compatibility,
                                         a_plan.profile().configuration(),
                                         a_plan.workspace_compatibility().toolsetVersion);
            cue::Result<void> metadataWritten =
                write_new_file(candidate / std::string(layout.metadata), metadata,
                               cue::WindowsBuildArtifactError::CandidateInvalid, *m_assertContext);
            if (!metadataWritten)
            {
                return failCandidate(std::move(*metadataWritten.try_error()));
            }
            cue::Result<void> candidateContents =
                validate_artifact_directory_contents(candidate, layout, hasPdb, *m_assertContext);
            if (!candidateContents)
            {
                return failCandidate(std::move(*candidateContents.try_error()));
            }
            auto candidateMetadataHash =
                hash_file(candidate / std::string(layout.metadata), std::string(layout.metadata),
                          cue::BuildArtifactFilePurpose::RuntimeMetadata, *m_assertContext);
            if (!candidateMetadataHash)
            {
                return failCandidate(std::move(*candidateMetadataHash.try_error()));
            }
            if (m_securityObserver != nullptr && isShippingProduct)
            {
                m_securityObserver->on_snapshot_held(
                    cue::detail::WindowsProductSecuritySnapshotStage::CandidateBeforeRelease, candidatePayload);
            }
            candidateSecuritySnapshot.reset();
            a_buildLease.reset();
            if (a_cancellation.is_cancel_requested())
            {
                return cancelCandidate();
            }

            const std::filesystem::path store = *storePath;
            cue::Result<void> storeCreated =
                ensure_directory(m_projectRoot, store / "Versions", cue::WindowsBuildArtifactError::ArtifactLockFailed,
                                 *m_assertContext);
            if (!storeCreated)
            {
                return failCandidate(std::move(*storeCreated.try_error()));
            }
            cue::Result<DirectoryChainGuard> storeGuard =
                acquire_directory_chain_guard(m_projectRoot, store / "Versions",
                                              cue::WindowsBuildArtifactError::ArtifactLockFailed, *m_assertContext);
            if (!storeGuard)
            {
                return failCandidate(std::move(*storeGuard.try_error()));
            }
            auto artifactLock =
                acquire_exclusive_lock(m_projectRoot, store / "Access.lock", a_cancellation, a_deadline,
                                       cue::WindowsBuildArtifactError::ArtifactLockFailed, *m_assertContext);
            if (!artifactLock)
            {
                return failCandidate(std::move(*artifactLock.try_error()));
            }
            if (!artifactLock.try_value()->has_value())
            {
                return cancelCandidate();
            }
            ByteRangeLease mutationLease(std::move(**artifactLock.try_value()));
            const std::filesystem::path version = store / "Versions" / std::string(a_plan.operation_id());
            if (std::filesystem::exists(version, filesystemError) || filesystemError)
            {
                return failCandidate(make_error(*m_assertContext, cue::WindowsBuildArtifactError::ArtifactAlreadyExists,
                                                "Artifact Version already exists"));
            }
            if (a_cancellation.is_cancel_requested())
            {
                return cancelCandidate();
            }
            cue::Result<void> versionPublished =
                rename_guarded_directory(*candidateGuard, candidate, version, *m_assertContext);
            if (!versionPublished)
            {
                cue::Error publicationError = std::move(*versionPublished.try_error());
                if (is_version_durability_unknown(publicationError))
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                        std::move(publicationError));
                }
                return failCandidate(std::move(publicationError));
            }
            if (isShippingProduct)
            {
                cue::Result<cue::detail::WindowsProductSecuritySnapshot> imageValidated =
                    cue::detail::validate_windows_shipping_product_security_snapshot(
                        path_to_utf8(version / std::string(layout.payload)), a_plan.profile(), *m_assertContext);
                if (!imageValidated)
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                        std::move(*imageValidated.try_error()));
                }
                const cue::WindowsProductSecurityValidation &versionSecurity = imageValidated.try_value()->validation();
                if (versionSecurity.byteSize != productSecurity->byteSize ||
                    versionSecurity.contentHash != productSecurity->contentHash ||
                    versionSecurity.importedLibraries != productSecurity->importedLibraries ||
                    versionSecurity.trustEvidence.signatureStatus != productSecurity->trustEvidence.signatureStatus ||
                    versionSecurity.trustEvidence.publisherKeyId != productSecurity->trustEvidence.publisherKeyId ||
                    versionSecurity.distributionStatus != productSecurity->distributionStatus)
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                        make_error(*m_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                   "Published Product differs from its verified Candidate snapshot"));
                }
                versionSecuritySnapshot.emplace(std::move(*imageValidated.try_value()));
                if (m_securityObserver != nullptr)
                {
                    m_securityObserver->on_snapshot_held(
                        cue::detail::WindowsProductSecuritySnapshotStage::VersionAfterValidation,
                        version / std::string(layout.payload));
                }
            }
            cue::Result<void> versionContents =
                validate_artifact_directory_contents(version, layout, hasPdb, *m_assertContext);
            if (!versionContents)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*versionContents.try_error()));
            }
            auto versionPayloadHash = hash_file(version / std::string(layout.payload), std::string(layout.payload),
                                                cue::BuildArtifactFilePurpose::DistributionPayload, *m_assertContext);
            std::optional<cue::BuildArtifactFile> versionPdbHash;
            if (hasPdb)
            {
                auto hashed = hash_file(version / std::string(layout.symbol), std::string(layout.symbol),
                                        cue::BuildArtifactFilePurpose::DevelopmentSymbol, *m_assertContext);
                if (!hashed)
                {
                    return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                        std::move(*hashed.try_error()));
                }
                versionPdbHash.emplace(std::move(*hashed.try_value()));
            }
            auto versionMetadataHash = hash_file(version / std::string(layout.metadata), std::string(layout.metadata),
                                                 cue::BuildArtifactFilePurpose::RuntimeMetadata, *m_assertContext);
            if (!versionPayloadHash)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*versionPayloadHash.try_error()));
            }
            if (!versionMetadataHash)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*versionMetadataHash.try_error()));
            }
            if ((isShippingProduct &&
                 (versionPayloadHash.try_value()->byteSize != versionSecuritySnapshot->validation().byteSize ||
                  versionPayloadHash.try_value()->contentHash != versionSecuritySnapshot->validation().contentHash)) ||
                versionPayloadHash.try_value()->contentHash != candidatePayloadHash.try_value()->contentHash ||
                (candidatePdbHash.has_value() &&
                 (!versionPdbHash.has_value() || versionPdbHash->contentHash != candidatePdbHash->contentHash)) ||
                versionMetadataHash.try_value()->contentHash != candidateMetadataHash.try_value()->contentHash)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    make_error(*m_assertContext, cue::WindowsBuildArtifactError::CandidateInvalid,
                               "Published Artifact differs from the Candidate snapshot"));
            }
            std::vector<cue::BuildArtifactFile> files;
            files.push_back(std::move(*versionPayloadHash.try_value()));
            if (versionPdbHash.has_value())
            {
                files.push_back(std::move(*versionPdbHash));
            }
            files.push_back(std::move(*versionMetadataHash.try_value()));
            auto inventory = cue::BuildArtifactInventory::create(a_plan, std::string(a_plan.operation_id()),
                                                                 std::move(files), *m_assertContext);
            if (!inventory)
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                    std::move(*inventory.try_error()));
            }
            if (a_cancellation.is_cancel_requested())
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
            }
            if (m_securityObserver != nullptr && isShippingProduct)
            {
                m_securityObserver->on_snapshot_held(
                    cue::detail::WindowsProductSecuritySnapshotStage::VersionBeforeCurrentPublication,
                    version / std::string(layout.payload));
            }
            const std::string currentContent = serialize_current(*inventory.try_value());
            cue::Result<bool> current =
                publish_current(store, a_plan.operation_id(), currentContent, a_cancellation, *m_assertContext);
            if (!current)
            {
                cue::Error publicationError = std::move(*current.try_error());
                if (is_current_durability_unknown(publicationError))
                {
                    cue::Result<void> inventoryVerified =
                        verify_inventory_files(version, *inventory.try_value(), *m_assertContext);
                    cue::ChildProcessCancellation validationCancellation;
                    cue::Result<ArtifactProbeStatus> artifactVerified =
                        isShippingProduct
                            ? validate_product(version / std::string(layout.payload), m_projectId, store,
                                               *m_processRunner, validationCancellation, a_deadline, *m_assertContext)
                            : validate_module(version / std::string(layout.payload), a_plan, m_projectId, store,
                                              m_probeExecutable, *m_processRunner, validationCancellation, a_deadline,
                                              *m_assertContext);
                    if (inventoryVerified && artifactVerified &&
                        *artifactVerified.try_value() == ArtifactProbeStatus::Valid)
                    {
                        std::string context("Visible Current artifact ");
                        context.append(inventory.try_value()->artifact_id());
                        context.append(" at ");
                        context.append(inventory.try_value()->version_directory());
                        context.append(" matched canonical schema, inventory, size, hash, and runtime contract; "
                                       "durability remains unknown");
                        publicationError.add_context(m_assertContext->fatal_handler(), context);
                    }
                    else
                    {
                        m_isAvailable = false;
                        publicationError.add_context(
                            m_assertContext->fatal_handler(),
                            "Visible Current matched the requested manifest bytes but its referenced artifact could "
                            "not be fully revalidated; Current selection is unknown");
                        if (!inventoryVerified)
                        {
                            publicationError.append_secondary_diagnostics(
                                *m_assertContext, *inventoryVerified.try_error(),
                                "Visible Current inventory revalidation failed", "Inventory validation");
                        }
                        if (!artifactVerified)
                        {
                            publicationError.append_secondary_diagnostics(
                                *m_assertContext, *artifactVerified.try_error(),
                                "Visible Current artifact runtime revalidation failed", "Artifact validation");
                        }
                    }
                }
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(std::move(publicationError));
            }
            if (!*current.try_value())
            {
                return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
            }
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(
                std::optional<cue::BuildArtifactInventory>(std::move(*inventory.try_value())));
        }
        catch (...)
        {
            terminate_artifact_exception(*m_assertContext);
        }
    }

  private:
    std::filesystem::path m_projectRoot;
    std::string m_projectId;
    cue::EngineCompatibility m_compatibility;
    std::string m_probeExecutable;
    std::unique_ptr<cue::ChildProcessRunner> m_processRunner;
    std::optional<cue::WindowsInstalledEngineSourceProvenance> m_installedEngineSource;
    cue::detail::WindowsProductSecuritySnapshotObserver *m_securityObserver;
    const cue::AssertContext *m_assertContext;
    bool m_isAvailable = true;
};

/// @brief 任意のSecurity Snapshot Observerを借用してWindows Artifact Publisherを構築する
[[nodiscard]] cue::Result<std::unique_ptr<cue::BuildArtifactPublisher>> create_windows_build_artifact_publisher_impl(
    std::string a_projectRoot, const cue::ProjectDescriptor &a_descriptor,
    std::optional<cue::WindowsInstalledEngineSourceProvenance> a_installedEngineSource,
    cue::detail::WindowsProductSecuritySnapshotObserver *a_securityObserver,
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        const std::optional<std::filesystem::path> root = to_path(a_projectRoot);
        std::error_code error;
        if (!root || !root->is_absolute() || !std::filesystem::is_directory(*root, error) || error)
        {
            return cue::Result<std::unique_ptr<cue::BuildArtifactPublisher>>::failure(make_error(
                a_assertContext, cue::WindowsBuildArtifactError::InvalidSettings, "Artifact Project Root is invalid"));
        }
        if (a_installedEngineSource)
        {
            const std::optional<std::filesystem::path> sourceRoot = to_path(a_installedEngineSource->sourceRoot);
            if (!sourceRoot || !sourceRoot->is_absolute() ||
                !std::filesystem::is_directory(*sourceRoot, error) || error)
            {
                return cue::Result<std::unique_ptr<cue::BuildArtifactPublisher>>::failure(
                    make_error(a_assertContext, cue::WindowsBuildArtifactError::InvalidSettings,
                               "Installed Engine Source Root is invalid"));
            }
        }
        cue::Result<void> rootValidated =
            validate_directory_chain(*root, *root, cue::WindowsBuildArtifactError::InvalidSettings, a_assertContext);
        if (!rootValidated)
        {
            return cue::Result<std::unique_ptr<cue::BuildArtifactPublisher>>::failure(
                std::move(*rootValidated.try_error()));
        }
        const cue::EngineCompatibility &compatibility = a_descriptor.engine_compatibility();
        const std::optional<std::filesystem::path> processDirectory = current_process_directory();
        if (!processDirectory)
        {
            return cue::Result<std::unique_ptr<cue::BuildArtifactPublisher>>::failure(
                make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::InvalidSettings, GetLastError(),
                                   "Engine process path could not be resolved"));
        }
        auto processRunner = cue::create_windows_child_process_runner(a_assertContext);
        if (!processRunner)
        {
            return cue::Result<std::unique_ptr<cue::BuildArtifactPublisher>>::failure(
                std::move(*processRunner.try_error()));
        }
        const std::string probeExecutable = path_to_utf8(*processDirectory / L"CueGameModuleProbe.exe");
        return cue::Result<std::unique_ptr<cue::BuildArtifactPublisher>>::success(
            std::make_unique<WindowsBuildArtifactPublisher>(
                *root, std::string(a_descriptor.project_id().text()), compatibility, probeExecutable,
                std::move(*processRunner.try_value()), std::move(a_installedEngineSource), a_securityObserver,
                a_assertContext));
    }
    catch (...)
    {
        terminate_artifact_exception(a_assertContext);
    }
}
} // namespace

namespace cue
{
Result<std::unique_ptr<BuildArtifactPublisher>> create_windows_build_artifact_publisher(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor, const AssertContext &a_assertContext) noexcept
{
    return create_windows_build_artifact_publisher_impl(std::move(a_projectRoot), a_descriptor, std::nullopt,
                                                        nullptr, a_assertContext);
}

Result<std::unique_ptr<BuildArtifactPublisher>> create_windows_build_artifact_publisher(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor,
    WindowsInstalledEngineSourceProvenance a_engineSourceProvenance,
    const AssertContext &a_assertContext) noexcept
{
    return create_windows_build_artifact_publisher_impl(
        std::move(a_projectRoot), a_descriptor, std::move(a_engineSourceProvenance), nullptr, a_assertContext);
}

namespace detail
{
Result<std::unique_ptr<BuildArtifactPublisher>> create_windows_build_artifact_publisher_for_test(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor,
    WindowsProductSecuritySnapshotObserver &a_observer, const AssertContext &a_assertContext) noexcept
{
    return create_windows_build_artifact_publisher_impl(std::move(a_projectRoot), a_descriptor, std::nullopt,
                                                        &a_observer, a_assertContext);
}

Result<std::unique_ptr<BuildArtifactPublisher>> create_windows_build_artifact_publisher_for_test(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor,
    WindowsInstalledEngineSourceProvenance a_engineSourceProvenance,
    WindowsProductSecuritySnapshotObserver &a_observer, const AssertContext &a_assertContext) noexcept
{
    return create_windows_build_artifact_publisher_impl(
        std::move(a_projectRoot), a_descriptor, std::move(a_engineSourceProvenance), &a_observer, a_assertContext);
}
} // namespace detail

Result<std::unique_ptr<BuildArtifactReader>> create_windows_build_artifact_reader(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor, const AssertContext &a_assertContext) noexcept
{
    try
    {
        const std::optional<std::filesystem::path> root = to_path(a_projectRoot);
        std::error_code error;
        if (!root || !root->is_absolute() || !std::filesystem::is_directory(*root, error) || error)
        {
            return Result<std::unique_ptr<BuildArtifactReader>>::failure(make_error(
                a_assertContext, WindowsBuildArtifactError::InvalidSettings, "Artifact Project Root is invalid"));
        }
        Result<void> rootValidated =
            validate_directory_chain(*root, *root, WindowsBuildArtifactError::InvalidSettings, a_assertContext);
        if (!rootValidated)
        {
            return Result<std::unique_ptr<BuildArtifactReader>>::failure(std::move(*rootValidated.try_error()));
        }
        return Result<std::unique_ptr<BuildArtifactReader>>::success(std::make_unique<WindowsBuildArtifactReader>(
            *root, std::string(a_descriptor.project_id().text()), a_assertContext));
    }
    catch (...)
    {
        terminate_artifact_exception(a_assertContext);
    }
}
} // namespace cue
