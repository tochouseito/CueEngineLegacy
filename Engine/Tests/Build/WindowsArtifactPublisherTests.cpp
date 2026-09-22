#include <Cue/Build/Windows/WindowsArtifactPublisher.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Project/Descriptor.h>

#include "WindowsProductSecurityInternal.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winioctl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_projectId = "41234567-89ab-4cde-8f01-23456789abcd";
constexpr std::string_view k_otherProjectId = "61234567-89ab-4cde-8f01-23456789abcd";
constexpr cue::BuildToolVersion k_currentCompilerVersion{static_cast<std::uint32_t>(_MSC_VER / 100),
                                                         static_cast<std::uint32_t>(_MSC_VER % 100),
                                                         static_cast<std::uint32_t>(_MSC_FULL_VER % 100000), 0U};

/// @brief Publisherの各観測点でProduct Write／Delete共有が拒否されることを記録する
class SecuritySnapshotObserver final : public cue::detail::WindowsProductSecuritySnapshotObserver
{
  public:
    /// @brief Snapshot保持中のProductへWrite／Delete Handleを開けないことを確認する
    void on_snapshot_held(cue::detail::WindowsProductSecuritySnapshotStage a_stage,
                          const std::filesystem::path &a_productPath) noexcept override
    {
        HANDLE writeHandle =
            CreateFileW(a_productPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD writeError = writeHandle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (writeHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(writeHandle);
        }
        HANDLE deleteHandle =
            CreateFileW(a_productPath.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD deleteError = deleteHandle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (deleteHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(deleteHandle);
        }
        const std::size_t index = static_cast<std::size_t>(a_stage);
        if (index >= m_locked.size())
        {
            return;
        }
        m_locked[index] = writeHandle == INVALID_HANDLE_VALUE && writeError == ERROR_SHARING_VIOLATION &&
                          deleteHandle == INVALID_HANDLE_VALUE && deleteError == ERROR_SHARING_VIOLATION;
        ++m_observations[index];
    }

    /// @brief 全Stageが一度ずつ観測されWrite／Deleteを拒否したか返す
    [[nodiscard]] bool all_stages_locked() const noexcept
    {
        for (std::size_t index = 0U; index < m_locked.size(); ++index)
        {
            if (!m_locked[index] || m_observations[index] != 1U)
            {
                return false;
            }
        }
        return true;
    }

  private:
    static constexpr std::size_t k_stageCount =
        static_cast<std::size_t>(cue::detail::WindowsProductSecuritySnapshotStage::Count);
    std::array<bool, k_stageCount> m_locked{};
    std::array<std::uint32_t, k_stageCount> m_observations{};
};

/// @brief TestをCompileした実MSVCの4要素File Version表現を返す
[[nodiscard]] std::string current_compiler_version_text()
{
    std::string text = std::to_string(k_currentCompilerVersion.major);
    text.push_back('.');
    text.append(std::to_string(k_currentCompilerVersion.minor));
    text.push_back('.');
    text.append(std::to_string(k_currentCompilerVersion.patch));
    text.push_back('.');
    text.append(std::to_string(k_currentCompilerVersion.build));
    return text;
}

/// @brief Junction用Mount Point Reparse BufferのNative Layoutを表す
struct MountPointReparseBuffer final
{
    DWORD reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    WORD reparseDataLength = 0U;
    WORD reserved = 0U;
    WORD substituteNameOffset = 0U;
    WORD substituteNameLength = 0U;
    WORD printNameOffset = 0U;
    WORD printNameLength = 0U;
    wchar_t pathBuffer[1]{};
};

/// @brief 特権不要のDirectory JunctionをPublisher再検証Fixtureとして作成する
[[nodiscard]] bool create_directory_link(const std::filesystem::path &a_linkPath,
                                         const std::filesystem::path &a_targetPath)
{
    if (CreateDirectoryW(a_linkPath.c_str(), nullptr) == FALSE)
    {
        return false;
    }

    const std::wstring substituteName = L"\\??\\" + a_targetPath.native();
    const std::wstring printName = a_targetPath.native();
    const std::size_t substituteBytes = substituteName.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    const std::size_t totalBytes = offsetof(MountPointReparseBuffer, pathBuffer) + pathBytes;
    if (substituteBytes > MAXWORD || printBytes > MAXWORD || pathBytes + 8U > MAXWORD || totalBytes > MAXDWORD)
    {
        RemoveDirectoryW(a_linkPath.c_str());
        return false;
    }

    std::vector<std::uint32_t> storage((totalBytes + sizeof(std::uint32_t) - 1U) / sizeof(std::uint32_t), 0U);
    auto *buffer = reinterpret_cast<MountPointReparseBuffer *>(storage.data());
    buffer->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer->reparseDataLength = static_cast<WORD>(pathBytes + 8U);
    buffer->substituteNameLength = static_cast<WORD>(substituteBytes);
    buffer->printNameOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    buffer->printNameLength = static_cast<WORD>(printBytes);
    std::memcpy(buffer->pathBuffer, substituteName.data(), substituteBytes);
    std::memcpy(reinterpret_cast<std::byte *>(buffer->pathBuffer) + buffer->printNameOffset, printName.data(),
                printBytes);

    HANDLE link = CreateFileW(a_linkPath.c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (link == INVALID_HANDLE_VALUE)
    {
        RemoveDirectoryW(a_linkPath.c_str());
        return false;
    }
    DWORD returned = 0U;
    const BOOL succeeded = DeviceIoControl(link, FSCTL_SET_REPARSE_POINT, buffer, static_cast<DWORD>(totalBytes),
                                           nullptr, 0U, &returned, nullptr);
    CloseHandle(link);
    if (succeeded == FALSE)
    {
        RemoveDirectoryW(a_linkPath.c_str());
        return false;
    }
    return true;
}

#if CUE_TEST_BUILD_CONFIGURATION == 1
constexpr cue::BuildConfiguration k_configuration = cue::BuildConfiguration::Debug;
constexpr std::string_view k_configurationName = "Debug";
#elif CUE_TEST_BUILD_CONFIGURATION == 2
constexpr cue::BuildConfiguration k_configuration = cue::BuildConfiguration::Development;
constexpr std::string_view k_configurationName = "Development";
#elif CUE_TEST_BUILD_CONFIGURATION == 3
constexpr cue::BuildConfiguration k_configuration = cue::BuildConfiguration::Release;
constexpr std::string_view k_configurationName = "Release";
#else
#error CUE_TEST_BUILD_CONFIGURATION must identify a supported configuration
#endif

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の引数なしFatalを即時失敗として終了する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(EXIT_FAILURE);
    }
    /// @brief Test中のFatalを即時失敗として終了する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(EXIT_FAILURE);
    }
};

/// @brief Artifact Readerの取消なし経路を提供するTest Cancellation
class TestArtifactReadCancellation final : public cue::BuildArtifactReadCancellation
{
  public:
    /// @brief 取消されていない状態を返す
    [[nodiscard]] bool is_cancel_requested() const noexcept override
    {
        return false;
    }
};

/// @brief 条件違反時にTest Processを失敗終了する
void require(bool a_condition, const std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::fprintf(stderr, "Requirement failed at %s:%u\n", a_location.file_name(), a_location.line());
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
}

/// @brief Result成功値を所有値として取得する
template <typename T>
[[nodiscard]] T take_value(cue::Result<T> a_result,
                           const std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_result.has_value() && a_result.try_error() != nullptr)
    {
        const cue::Error &error = *a_result.try_error();
        std::fprintf(stderr, "Result error: %.*s/%lld %.*s\n", static_cast<int>(error.root_code().domain().size()),
                     error.root_code().domain().data(), static_cast<long long>(error.root_code().value()),
                     static_cast<int>(error.summary().size()), error.summary().data());
        for (const cue::ErrorContext &context : error.contexts())
        {
            std::fprintf(stderr, "  Context: %.*s\n", static_cast<int>(context.message().size()),
                         context.message().data());
        }
        if (const cue::NativeError *native = error.try_native_error(); native != nullptr)
        {
            std::fprintf(stderr, "  Native: %.*s/%lld\n", static_cast<int>(native->domain().size()),
                         native->domain().data(), static_cast<long long>(native->value()));
        }
    }
    require(a_result.has_value(), a_location);
    return std::move(*a_result.try_value());
}

/// @brief UTF-8 Path表示をTest入力用stringへ変換する
[[nodiscard]] std::string generic_path(const std::filesystem::path &a_path)
{
    const std::u8string text = a_path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

/// @brief 小さいManifest Fileを検証用文字列として読む
[[nodiscard]] std::string read_text(const std::filesystem::path &a_path)
{
    std::ifstream input(a_path, std::ios::binary);
    require(static_cast<bool>(input));
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/// @brief 小さい検証Manifestを一回のStream Writeで置換する
void write_text(const std::filesystem::path &a_path, std::string_view a_text)
{
    std::ofstream output(a_path, std::ios::binary | std::ios::trunc);
    output.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
    require(static_cast<bool>(output));
}

/// @brief 同じCurrent Inventoryを異なるMember順と空白で表すJSONを構築する
[[nodiscard]] std::string make_reordered_current(const cue::BuildArtifactInventory &a_inventory)
{
    std::string json = "{ \n  \"files\" : [";
    for (std::size_t index = 0U; index < a_inventory.files().size(); ++index)
    {
        const cue::BuildArtifactFile &file = a_inventory.files()[index];
        if (index != 0U)
        {
            json.push_back(',');
        }
        json.append("{\"contentHash\":\"");
        json.append(file.contentHash);
        json.append("\",\"hashAlgorithm\":\"sha256\",\"sizeBytes\":");
        json.append(std::to_string(file.byteSize));
        json.append(",\"path\":\"");
        json.append(file.relativePath);
        json.append("\"}");
    }
    json.append("],\n\"configuration\":\"");
    json.append(k_configurationName);
    json.append("\",\"artifactId\":\"");
    json.append(a_inventory.artifact_id());
    json.append("\",\"schemaVersion\":1 }\n");
    return json;
}

/// @brief 指定IdentityでTest Project契約を満たすDescriptorを構築する
[[nodiscard]] cue::ProjectDescriptor make_descriptor(std::string_view a_projectId,
                                                     const cue::AssertContext &a_assertContext)
{
    cue::ProjectId projectId = take_value(cue::ProjectId::parse(a_projectId, a_assertContext));
    return take_value(cue::create_blank_project_descriptor(projectId, "Artifact Publisher Test",
                                                           {{1U, 0U, 0U}, std::nullopt},
                                                           "00000000-0000-4000-8000-000000000099", a_assertContext));
}

/// @brief 既定IdentityでTest Project契約を満たすDescriptorを構築する
[[nodiscard]] cue::ProjectDescriptor make_descriptor(const cue::AssertContext &a_assertContext)
{
    return make_descriptor(k_projectId, a_assertContext);
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

/// @brief Shipping Publisherが実使用Toolchainを照合するCMake生成物Fixtureを作る
void write_shipping_toolchain_evidence(const std::filesystem::path &a_binary,
                                       std::string_view a_windowsSdkVersion = CUE_TEST_WINDOWS_SDK_VERSION,
                                       std::string_view a_platformToolset = CUE_TEST_PLATFORM_TOOLSET,
                                       std::string_view a_engineRoot = CUE_TEST_ENGINE_ROOT,
                                       std::string_view a_msvcToolsetVersion = CUE_TEST_MSVC_TOOLSET_VERSION,
                                       std::string_view a_projectMsvcToolsetVersion = {})
{
    const std::string cmakeVersion(CUE_TEST_CMAKE_VERSION);
    const std::size_t firstDot = cmakeVersion.find('.');
    const std::size_t secondDot = cmakeVersion.find('.', firstDot + 1U);
    require(firstDot != std::string::npos && secondDot != std::string::npos &&
            cmakeVersion.find('.', secondDot + 1U) == std::string::npos);
    std::string cache;
    cache.append("CMAKE_COMMAND:INTERNAL=");
    cache.append(CUE_TEST_CMAKE_COMMAND);
    cache.append("\nCMAKE_CACHE_MAJOR_VERSION:INTERNAL=");
    cache.append(cmakeVersion.substr(0U, firstDot));
    cache.append("\nCMAKE_CACHE_MINOR_VERSION:INTERNAL=");
    cache.append(cmakeVersion.substr(firstDot + 1U, secondDot - firstDot - 1U));
    cache.append("\nCMAKE_CACHE_PATCH_VERSION:INTERNAL=");
    cache.append(cmakeVersion.substr(secondDot + 1U));
    cache.append("\nCMAKE_GENERATOR:INTERNAL=");
    cache.append(CUE_TEST_CMAKE_GENERATOR);
    cache.append("\nCMAKE_GENERATOR_INSTANCE:INTERNAL=");
    cache.append(CUE_TEST_CMAKE_GENERATOR_INSTANCE);
    cache.append("\nCMAKE_GENERATOR_PLATFORM:INTERNAL=x64\nCUE_ENGINE_ROOT:UNINITIALIZED=");
    cache.append(a_engineRoot);
    cache.append("\nCMAKE_GENERATOR_TOOLSET:INTERNAL=version=");
    cache.append(a_msvcToolsetVersion);
    cache.push_back('\n');
    write_text(a_binary / "CMakeCache.txt", cache);

    const std::filesystem::path compilerDirectory = a_binary / "CMakeFiles" / cmakeVersion;
    require(std::filesystem::create_directories(compilerDirectory) || std::filesystem::is_directory(compilerDirectory));
    std::string compilerEvidence("set(CMAKE_CXX_COMPILER \"");
    compilerEvidence.append(CUE_TEST_CXX_COMPILER);
    compilerEvidence.append("\")\nset(CMAKE_CXX_COMPILER_VERSION \"");
    compilerEvidence.append(current_compiler_version_text());
    compilerEvidence.append("\")\nset(CMAKE_CXX_COMPILER_ARCHITECTURE_ID \"x64\")\n");
    write_text(compilerDirectory / "CMakeCXXCompiler.cmake", compilerEvidence);

    if (a_projectMsvcToolsetVersion.empty())
    {
        a_projectMsvcToolsetVersion = a_msvcToolsetVersion;
    }
    std::string project("<Project><PropertyGroup><VCToolsVersion>");
    project.append(a_projectMsvcToolsetVersion);
    project.append("</VCToolsVersion><WindowsTargetPlatformVersion>");
    project.append(a_windowsSdkVersion);
    project.append("</WindowsTargetPlatformVersion><PlatformToolset>");
    project.append(a_platformToolset);
    project.append("</PlatformToolset></PropertyGroup><Import Project=\"");
    const std::size_t firstVersionSeparator = a_msvcToolsetVersion.find('.');
    const std::size_t secondVersionSeparator =
        firstVersionSeparator == std::string_view::npos
            ? std::string_view::npos
            : a_msvcToolsetVersion.find('.', firstVersionSeparator + 1U);
    require(secondVersionSeparator != std::string_view::npos);
    const std::string_view propsVersion = a_msvcToolsetVersion.substr(0U, secondVersionSeparator);
    append_xml_double_quoted_attribute(project, CUE_TEST_CMAKE_GENERATOR_INSTANCE);
    project.append("/VC/Auxiliary/Build/");
    project.append(propsVersion);
    project.append("/Microsoft.VCToolsVersion.");
    project.append(propsVersion);
    project.append(".props\" /></Project>\n");
    const std::filesystem::path projectDirectory = a_binary / "Source" / "Game";
    require(std::filesystem::create_directories(projectDirectory) || std::filesystem::is_directory(projectDirectory));
    write_text(projectDirectory / "CueGameProduct.vcxproj", project);
}

/// @brief Test用Build PlanをOperation ID別に構築する
[[nodiscard]] cue::BuildPlan make_plan(const std::filesystem::path &a_projectRoot, std::string a_operationId,
                                       const cue::AssertContext &a_assertContext)
{
    cue::BuildProfile profile =
        take_value(cue::BuildProfile::create(k_configuration, cue::BuildTarget::GameModule, a_assertContext));
    constexpr cue::BuildWorkspaceCompatibility compatibility{
        cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 36256U, 0U}, 1U};
    return take_value(cue::create_build_plan(
        {generic_path(a_projectRoot), std::move(profile), std::move(a_operationId), compatibility}, a_assertContext));
}

/// @brief UnsignedLocal Shipping Product用Release Build Planを構築する
[[nodiscard]] cue::BuildPlan make_shipping_plan(const std::filesystem::path &a_projectRoot, std::string a_operationId,
                                                const cue::AssertContext &a_assertContext)
{
    cue::BuildProfile profile = take_value(cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::UnsignedLocal, {}, a_assertContext));
    constexpr cue::BuildWorkspaceCompatibility compatibility{cue::BuildGenerator::VisualStudio2026,
                                                             cue::BuildArchitecture::X64, k_currentCompilerVersion, 1U};
    return take_value(cue::create_build_plan(
        {generic_path(a_projectRoot), std::move(profile), std::move(a_operationId), compatibility}, a_assertContext));
}

/// @brief #304 Trust Verifier未実装時のPublisherSigned拒否用Build Planを構築する
[[nodiscard]] cue::BuildPlan make_signed_shipping_plan(const std::filesystem::path &a_projectRoot,
                                                       std::string a_operationId,
                                                       const cue::AssertContext &a_assertContext)
{
    cue::BuildProfile profile = take_value(cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::PublisherSigned, std::string(64U, 'a'),
        a_assertContext));
    constexpr cue::BuildWorkspaceCompatibility compatibility{cue::BuildGenerator::VisualStudio2026,
                                                             cue::BuildArchitecture::X64, k_currentCompilerVersion, 1U};
    return take_value(cue::create_build_plan(
        {generic_path(a_projectRoot), std::move(profile), std::move(a_operationId), compatibility}, a_assertContext));
}

/// @brief Plan作成後に追加されたJunction経由のRoot外読書きとRollbackを拒否する
void test_reparse_revalidation(const std::filesystem::path &a_probe, const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() /
        ("CueBuildArtifactReparse-" + std::to_string(GetCurrentProcessId()) + "-" + std::string(k_configurationName));
    const std::filesystem::path project = parent / "Project";
    const std::filesystem::path outside = parent / "Outside";
    std::error_code error;
    std::filesystem::remove_all(parent, error);
    require(!error);
    require(std::filesystem::create_directories(project, error));
    require(!error);
    require(std::filesystem::create_directories(outside, error));
    require(!error);

    cue::ProjectDescriptor descriptor = make_descriptor(a_assertContext);
    std::unique_ptr<cue::BuildArtifactPublisher> publisher =
        take_value(cue::create_windows_build_artifact_publisher(generic_path(project), descriptor, a_assertContext));
    cue::BuildPlan lockPlan = make_plan(project, "91234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    require(create_directory_link(project / "Generated", outside));
    cue::ChildProcessCancellation cancellation;
    require(!publisher->acquire_build_lease(lockPlan, cancellation, std::nullopt).has_value());
    require(!std::filesystem::exists(outside / "Build"));
    require(std::filesystem::remove(project / "Generated", error));
    require(!error);

    cue::BuildPlan candidatePlan = make_plan(project, "a1234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    const std::filesystem::path output =
        std::filesystem::path(candidatePlan.binary_directory()) / "bin" / k_configurationName;
    require(std::filesystem::create_directories(output, error));
    require(!error);
    require(std::filesystem::copy_file(a_probe, output / "CueGameModule.dll"));
    {
        std::ofstream pdb(output / "CueGameModule.pdb", std::ios::binary | std::ios::trunc);
        pdb << "reparse-test-symbols-" << k_configurationName;
        require(static_cast<bool>(pdb));
    }
    auto candidateLease = take_value(publisher->acquire_build_lease(candidatePlan, cancellation, std::nullopt));
    require(candidateLease.has_value());
    const std::filesystem::path outsideCandidates = outside / "Candidates";
    require(std::filesystem::create_directories(outsideCandidates, error));
    require(!error);
    require(create_directory_link(project / "Generated" / "Build" / "Candidates", outsideCandidates));
    require(!publisher->publish(candidatePlan, cancellation, std::move(*candidateLease), std::nullopt).has_value());
    require(std::filesystem::is_empty(outsideCandidates));
    require(std::filesystem::remove(project / "Generated" / "Build" / "Candidates", error));
    require(!error);

    cue::BuildPlan storePlan = make_plan(project, "b1234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto storeLease = take_value(publisher->acquire_build_lease(storePlan, cancellation, std::nullopt));
    require(storeLease.has_value());
    const std::filesystem::path outsideStore = outside / "Store";
    require(std::filesystem::create_directories(outsideStore, error));
    require(!error);
    const std::filesystem::path targetStore = project / "Generated" / "Artifacts" / "GameModule";
    require(std::filesystem::create_directories(targetStore, error));
    require(!error);
    require(create_directory_link(targetStore / k_configurationName, outsideStore));
    require(!publisher->publish(storePlan, cancellation, std::move(*storeLease), std::nullopt).has_value());
    require(std::filesystem::is_empty(outsideStore));
    require(!std::filesystem::exists(std::filesystem::path(storePlan.candidate_directory())));
    require(std::filesystem::remove(targetStore / k_configurationName, error));
    require(!error);

    std::filesystem::remove_all(parent, error);
    require(!error);
}

/// @brief Artifact公開、Lock取消、失敗時Current保全を一つのProject Rootで検証する
void test_windows_artifact_publisher(const std::filesystem::path &a_probe, const std::filesystem::path &a_invalidProbe,
                                     const std::filesystem::path &a_crashingProbe,
                                     const std::filesystem::path &a_hangingProbe,
                                     const std::filesystem::path &a_zeroExitProbe,
                                     const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path projectRoot =
        std::filesystem::temp_directory_path() /
        ("CueBuildArtifactPublisherTests-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::string(k_configurationName));
    std::error_code error;
    std::filesystem::remove_all(projectRoot, error);
    require(!error);
    require(std::filesystem::create_directories(projectRoot));

    cue::ProjectDescriptor descriptor = make_descriptor(a_assertContext);
    std::unique_ptr<cue::BuildArtifactPublisher> publisher = take_value(
        cue::create_windows_build_artifact_publisher(generic_path(projectRoot), descriptor, a_assertContext));
    cue::BuildPlan plan = make_plan(projectRoot, "01234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    const std::filesystem::path binaryDirectory = std::filesystem::path(plan.binary_directory());
    const std::filesystem::path outputDirectory = binaryDirectory / "bin" / k_configurationName;
    require(std::filesystem::create_directories(outputDirectory));
    require(std::filesystem::copy_file(a_probe, outputDirectory / "CueGameModule.dll"));
    {
        std::ofstream pdb(outputDirectory / "CueGameModule.pdb", std::ios::binary | std::ios::trunc);
        pdb << "test-symbols-" << k_configurationName;
        require(static_cast<bool>(pdb));
    }

    cue::ChildProcessCancellation cancellation;
    auto lease = take_value(publisher->acquire_build_lease(plan, cancellation, std::nullopt));
    require(lease.has_value());
    const std::filesystem::path displacedWorkspace = binaryDirectory.parent_path() / "DisplacedWorkspace";
    require(MoveFileExW(binaryDirectory.c_str(), displacedWorkspace.c_str(), 0U) == FALSE);

    std::unique_ptr<cue::BuildArtifactPublisher> contender = take_value(
        cue::create_windows_build_artifact_publisher(generic_path(projectRoot), descriptor, a_assertContext));
    cue::ChildProcessCancellation contenderCancellation;
    using LeaseResult = cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>;
    std::unique_ptr<LeaseResult> contenderResult;
    std::thread contenderThread(
        [&]()
        {
            contenderResult = std::make_unique<LeaseResult>(
                contender->acquire_build_lease(plan, contenderCancellation, std::nullopt));
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    contenderCancellation.request_cancel();
    contenderThread.join();
    require(contenderResult != nullptr && contenderResult->has_value() && !contenderResult->try_value()->has_value());

    cue::ChildProcessCancellation timeoutCancellation;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    auto timedOut = contender->acquire_build_lease(plan, timeoutCancellation, deadline);
    require(!timedOut && timedOut.try_error()->root_code().domain() == "Cue.Build.Publisher" &&
            timedOut.try_error()->root_code().value() ==
                static_cast<std::int64_t>(cue::BuildArtifactPublisherError::LockWaitTimedOut));

    auto published = take_value(publisher->publish(plan, cancellation, std::move(*lease), std::nullopt));
    require(published.has_value());
    require(published->artifact_id() == plan.operation_id());
    require(published->files().size() == 3U);
    const std::filesystem::path store = std::filesystem::path(plan.artifact_store_directory());
    const std::filesystem::path version = store / "Versions" / std::string(plan.operation_id());
    require(std::filesystem::is_regular_file(version / "CueGameModule.dll"));
    require(std::filesystem::is_regular_file(version / "CueGameModule.pdb"));
    require(std::filesystem::is_regular_file(version / "CueGameModule.metadata.json"));
    const std::string metadata = read_text(version / "CueGameModule.metadata.json");
    require(metadata.find(std::string(k_projectId)) != std::string::npos);
    require(metadata.find("\"configuration\": \"" + std::string(k_configurationName) + "\"") != std::string::npos);
    require(metadata.find("\"compilerVersion\": 1951") != std::string::npos);
    require(metadata.find("\"fullVersion\": 195136256") != std::string::npos);
    require(metadata.find("\"build\": 0") != std::string::npos);
    const std::filesystem::path currentPath = store / "Current.json";
    const std::string current = read_text(currentPath);
    require(current.find(std::string(plan.operation_id())) != std::string::npos);
    require(current.find("sha256") != std::string::npos);
    require(current.find("CueGameModule.pdb") != std::string::npos);

    std::unique_ptr<cue::BuildArtifactReader> reader =
        take_value(cue::create_windows_build_artifact_reader(generic_path(projectRoot), descriptor, a_assertContext));
    TestArtifactReadCancellation readCancellation;
    auto currentV2ReadLease =
        take_value(reader->acquire_current_read_lease(*published, readCancellation, std::nullopt));
    require(currentV2ReadLease.has_value() && (*currentV2ReadLease)->project_id() == k_projectId);
    currentV2ReadLease.reset();
    cue::ProjectDescriptor otherDescriptor = make_descriptor(k_otherProjectId, a_assertContext);
    std::unique_ptr<cue::BuildArtifactReader> otherProjectReader = take_value(
        cue::create_windows_build_artifact_reader(generic_path(projectRoot), otherDescriptor, a_assertContext));
    auto otherProjectRead = otherProjectReader->acquire_current_read_lease(*published, readCancellation, std::nullopt);
    require(!otherProjectRead && otherProjectRead.try_error()->root_code().domain() == "Cue.Build.Windows.Artifact" &&
            otherProjectRead.try_error()->root_code().value() ==
                static_cast<std::int64_t>(cue::WindowsBuildArtifactError::InvalidSettings));
    std::vector<cue::BuildArtifactFile> legacyFiles(published->files().begin(), published->files().end());
    cue::BuildArtifactInventory legacyInventory = take_value(cue::BuildArtifactInventory::create_legacy_game_module(
        plan, std::string(published->artifact_id()), std::move(legacyFiles), a_assertContext));
    const std::filesystem::path legacyVersion(legacyInventory.version_directory());
    require(std::filesystem::create_directories(legacyVersion));
    for (const cue::BuildArtifactFile &file : legacyInventory.files())
    {
        require(std::filesystem::copy_file(version / file.relativePath, legacyVersion / file.relativePath));
    }
    const std::filesystem::path legacyStore = legacyVersion.parent_path().parent_path();
    write_text(legacyStore / "Current.json", make_reordered_current(legacyInventory));
    auto legacyReadLease =
        take_value(reader->acquire_current_read_lease(legacyInventory, readCancellation, std::nullopt));
    require(legacyReadLease.has_value());
    legacyReadLease.reset();
    write_text(currentPath, make_reordered_current(*published));
    auto firstReadLease = take_value(reader->acquire_current_read_lease(*published, readCancellation, std::nullopt));
    auto secondReadLease = take_value(reader->acquire_current_read_lease(*published, readCancellation, std::nullopt));
    require(firstReadLease.has_value() && secondReadLease.has_value());
    HANDLE exclusive = CreateFileW((store / "Access.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    require(exclusive != INVALID_HANDLE_VALUE);
    OVERLAPPED overlap{};
    require(LockFileEx(exclusive, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U, 1U, 0U, &overlap) == FALSE);
    require(GetLastError() == ERROR_LOCK_VIOLATION);
    firstReadLease.reset();
    secondReadLease.reset();
    overlap = {};
    require(LockFileEx(exclusive, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U, 1U, 0U, &overlap) != FALSE);
    require(UnlockFileEx(exclusive, 0U, 1U, 0U, &overlap) != FALSE);
    require(CloseHandle(exclusive) != FALSE);
    write_text(currentPath, R"json({"schemaVersion":2})json");
    require(!reader->acquire_current_read_lease(*published, readCancellation, std::nullopt).has_value());
    write_text(currentPath, current);

    if (k_configuration != cue::BuildConfiguration::Release)
    {
        cue::BuildPlan missingPdbPlan = make_plan(projectRoot, "31234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
        auto missingPdbLease = take_value(publisher->acquire_build_lease(missingPdbPlan, cancellation, std::nullopt));
        require(missingPdbLease.has_value());
        require(std::filesystem::remove(outputDirectory / "CueGameModule.pdb"));
        require(
            !publisher->publish(missingPdbPlan, cancellation, std::move(*missingPdbLease), std::nullopt).has_value());
        require(read_text(currentPath) == current);
        std::ofstream pdb(outputDirectory / "CueGameModule.pdb", std::ios::binary | std::ios::trunc);
        pdb << "restored-test-symbols-" << k_configurationName;
        require(static_cast<bool>(pdb));
    }

    cue::BuildPlan invalidPlan = make_plan(projectRoot, "11234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto invalidLease = take_value(publisher->acquire_build_lease(invalidPlan, cancellation, std::nullopt));
    require(invalidLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(std::filesystem::copy_file(a_invalidProbe, outputDirectory / "CueGameModule.dll"));
    require(!publisher->publish(invalidPlan, cancellation, std::move(*invalidLease), std::nullopt).has_value());
    require(read_text(currentPath) == current);
    require(!std::filesystem::exists(std::filesystem::path(invalidPlan.candidate_directory())));

    cue::BuildPlan crashingPlan = make_plan(projectRoot, "51234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto crashingLease = take_value(publisher->acquire_build_lease(crashingPlan, cancellation, std::nullopt));
    require(crashingLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(std::filesystem::copy_file(a_crashingProbe, outputDirectory / "CueGameModule.dll"));
    require(!publisher
                 ->publish(crashingPlan, cancellation, std::move(*crashingLease),
                           std::chrono::steady_clock::now() + std::chrono::seconds(2))
                 .has_value());
    require(read_text(currentPath) == current);
    require(!std::filesystem::exists(std::filesystem::path(crashingPlan.candidate_directory())));

    cue::BuildPlan zeroExitPlan = make_plan(projectRoot, "81234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto zeroExitLease = take_value(publisher->acquire_build_lease(zeroExitPlan, cancellation, std::nullopt));
    require(zeroExitLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(std::filesystem::copy_file(a_zeroExitProbe, outputDirectory / "CueGameModule.dll"));
    require(!publisher->publish(zeroExitPlan, cancellation, std::move(*zeroExitLease), std::nullopt).has_value());
    require(read_text(currentPath) == current);
    require(!std::filesystem::exists(std::filesystem::path(zeroExitPlan.candidate_directory())));

    cue::BuildPlan hangingPlan = make_plan(projectRoot, "61234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto hangingLease = take_value(publisher->acquire_build_lease(hangingPlan, cancellation, std::nullopt));
    require(hangingLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(std::filesystem::copy_file(a_hangingProbe, outputDirectory / "CueGameModule.dll"));
    auto timedOutProbe = publisher->publish(hangingPlan, cancellation, std::move(*hangingLease),
                                            std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    require(!timedOutProbe && timedOutProbe.try_error()->root_code().domain() == "Cue.Build.Publisher" &&
            timedOutProbe.try_error()->root_code().value() ==
                static_cast<std::int64_t>(cue::BuildArtifactPublisherError::ModuleProbeTimedOut));
    require(read_text(currentPath) == current);
    require(!std::filesystem::exists(std::filesystem::path(hangingPlan.candidate_directory())));

    cue::BuildPlan cancelledPlan = make_plan(projectRoot, "71234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto cancelledLease = take_value(publisher->acquire_build_lease(cancelledPlan, cancellation, std::nullopt));
    require(cancelledLease.has_value());
    using PublishResult = cue::Result<std::optional<cue::BuildArtifactInventory>>;
    std::unique_ptr<PublishResult> cancelledResult;
    cue::ChildProcessCancellation probeCancellation;
    std::thread probeThread(
        [&]()
        {
            cancelledResult = std::make_unique<PublishResult>(
                publisher->publish(cancelledPlan, probeCancellation, std::move(*cancelledLease), std::nullopt));
        });
    const std::filesystem::path cancelledCandidate(cancelledPlan.candidate_directory());
    const std::filesystem::path cancelledPayload = cancelledCandidate / "CueGameModule.dll";
    for (std::size_t attempt = 0U; attempt < 50U && !std::filesystem::exists(cancelledPayload); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(std::filesystem::exists(cancelledPayload));
    const std::filesystem::path displacedOutput = outputDirectory.parent_path() / "DisplacedOutput";
    const std::filesystem::path candidateParent = cancelledCandidate.parent_path();
    const std::filesystem::path displacedCandidate = candidateParent / "DisplacedCandidate";
    const std::filesystem::path displacedCandidates = candidateParent.parent_path() / "DisplacedCandidates";
    require(MoveFileExW(outputDirectory.c_str(), displacedOutput.c_str(), 0U) == FALSE);
    const DWORD outputRenameError = GetLastError();
    require(outputRenameError == ERROR_ACCESS_DENIED || outputRenameError == ERROR_SHARING_VIOLATION);
    require(MoveFileExW(cancelledCandidate.c_str(), displacedCandidate.c_str(), 0U) == FALSE);
    const DWORD candidateRenameError = GetLastError();
    require(candidateRenameError == ERROR_ACCESS_DENIED || candidateRenameError == ERROR_SHARING_VIOLATION);
    require(MoveFileExW(candidateParent.c_str(), displacedCandidates.c_str(), 0U) == FALSE);
    const DWORD parentRenameError = GetLastError();
    require(parentRenameError == ERROR_ACCESS_DENIED || parentRenameError == ERROR_SHARING_VIOLATION);
    probeCancellation.request_cancel();
    probeThread.join();
    require(cancelledResult != nullptr && cancelledResult->has_value() && !cancelledResult->try_value()->has_value());
    require(read_text(currentPath) == current);
    require(!std::filesystem::exists(std::filesystem::path(cancelledPlan.candidate_directory())));

    cue::BuildPlan missingPlan = make_plan(projectRoot, "21234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto missingLease = take_value(publisher->acquire_build_lease(missingPlan, cancellation, std::nullopt));
    require(missingLease.has_value());
    require(std::filesystem::remove(outputDirectory / "CueGameModule.dll"));
    require(!publisher->publish(missingPlan, cancellation, std::move(*missingLease), std::nullopt).has_value());
    require(read_text(currentPath) == current);

    std::filesystem::remove_all(projectRoot, error);
    require(!error);
}

/// @brief Shipping Productの公開、Identity拒否、Current保全、Tamper検出を検証する
void test_shipping_product_publisher(const std::filesystem::path &a_product,
                                     const std::filesystem::path &a_wrongProjectProduct,
                                     const std::filesystem::path &a_wrongConfigurationProduct,
                                     const std::filesystem::path &a_extraFileProduct,
                                     const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path projectRoot =
        std::filesystem::temp_directory_path() /
        ("CueBSA-" + std::to_string(GetCurrentProcessId()) + "-" + std::string(k_configurationName));
    std::error_code error;
    std::filesystem::remove_all(projectRoot, error);
    require(!error && std::filesystem::create_directories(projectRoot));
    require(std::filesystem::create_directories(projectRoot / "Source" / "Game"));
    write_text(projectRoot / "CMakeLists.txt", "cmake_minimum_required(VERSION 4.2.0)\n");
    write_text(projectRoot / "CMakePresets.json", "{}\n");
    write_text(projectRoot / "CueProject.json", "{}\n");
    write_text(projectRoot / "Source" / "Game" / "Test.cpp", "int cue_shipping_test = 1;\n");
    const std::filesystem::path installedEngineRoot = projectRoot / "InstalledEngine";
    require(std::filesystem::create_directories(installedEngineRoot / "Engine" / "Source"));
    require(std::filesystem::create_directories(installedEngineRoot / "CMake"));
    require(std::filesystem::create_directories(installedEngineRoot / "ThirdParty"));
    write_text(installedEngineRoot / "Engine" / "Source" / "Test.cpp", "int cue_engine_test = 1;\n");
    write_text(installedEngineRoot / "CMake" / "Test.cmake", "set(CUE_TEST ON)\n");
    write_text(installedEngineRoot / "CMakeLists.txt", "cmake_minimum_required(VERSION 4.2.0)\n");
    write_text(installedEngineRoot / "CMakePresets.json", "{}\n");
    write_text(installedEngineRoot / "ThirdParty" / "vcpkg.json", "{}\n");
    write_text(installedEngineRoot / "ThirdParty" / "vcpkg-configuration.json", "{}\n");
    write_text(installedEngineRoot / "ThirdParty" / "vcpkg-tool.json", "{}\n");

    cue::ProjectDescriptor descriptor = make_descriptor(a_assertContext);
    SecuritySnapshotObserver securityObserver;
    cue::WindowsInstalledEngineSourceProvenance installedProvenance{
        generic_path(installedEngineRoot), std::string(40U, 'c'), std::string(64U, 'a'), std::string(64U, 'b')};
    std::unique_ptr<cue::BuildArtifactPublisher> publisher =
        take_value(cue::detail::create_windows_build_artifact_publisher_for_test(generic_path(projectRoot), descriptor,
                                                                                 std::move(installedProvenance),
                                                                                 securityObserver, a_assertContext));
    cue::BuildPlan plan = make_shipping_plan(projectRoot, "01234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    const std::filesystem::path binary(plan.binary_directory());
    const std::filesystem::path output = binary / "bin" / "Release";
    require(std::filesystem::create_directories(output));
    const auto writeToolchainEvidence = [&](std::string_view a_windowsSdkVersion = CUE_TEST_WINDOWS_SDK_VERSION,
                                            std::string_view a_platformToolset = CUE_TEST_PLATFORM_TOOLSET,
                                            std::string_view a_engineRoot = {})
    {
        write_shipping_toolchain_evidence(binary, a_windowsSdkVersion, a_platformToolset,
                                          a_engineRoot.empty() ? generic_path(installedEngineRoot) : a_engineRoot);
    };
    writeToolchainEvidence();
    require(std::filesystem::copy_file(a_product, output / "CueGameProduct.exe"));
    {
        std::ofstream pdb(output / "CueGameProduct.pdb", std::ios::binary | std::ios::trunc);
        pdb << "shipping-product-development-symbol";
        require(static_cast<bool>(pdb));
    }
    cue::ChildProcessCancellation cancellation;
    auto lease = take_value(publisher->acquire_build_lease(plan, cancellation, std::nullopt));
    require(lease.has_value());
    auto published = take_value(publisher->publish(plan, cancellation, std::move(*lease), std::nullopt));
    require(published.has_value() && published->profile().target() == cue::BuildTarget::ShippingProduct &&
            published->profile().minimum_trust_mode() == cue::ShippingTrustMode::UnsignedLocal &&
            published->files().size() == 3U && securityObserver.all_stages_locked());
    require(published->files()[0].relativePath == "CueGameProduct.exe" &&
            published->files()[0].purpose == cue::BuildArtifactFilePurpose::DistributionPayload);
    require(published->files()[1].relativePath == "CueGameProduct.metadata.json" &&
            published->files()[1].purpose == cue::BuildArtifactFilePurpose::RuntimeMetadata);
    require(published->files()[2].relativePath == "CueGameProduct.pdb" &&
            published->files()[2].purpose == cue::BuildArtifactFilePurpose::DevelopmentSymbol);

    const std::filesystem::path store(plan.artifact_store_directory());
    const std::filesystem::path version = store / "Versions" / std::string(plan.operation_id());
    const std::filesystem::path currentPath = store / "Current.json";
    const std::filesystem::path metadataPath = version / "CueGameProduct.metadata.json";
    const std::string current = read_text(currentPath);
    const std::string metadata = read_text(metadataPath);
    require(current.find("\"schemaVersion\": 2") != std::string::npos &&
            current.find("\"target\": \"ShippingProduct\"") != std::string::npos &&
            current.find("\"minimumTrustMode\": \"UnsignedLocal\"") != std::string::npos &&
            current.find("\"publisherKeyId\": null") != std::string::npos &&
            current.find("\"purpose\": \"DevelopmentSymbol\"") != std::string::npos);
    require(
        metadata.find(std::string(k_projectId)) != std::string::npos &&
        metadata.find("\"configuration\": \"Release\"") != std::string::npos &&
        metadata.find("\"architecture\": \"x64\"") != std::string::npos &&
        metadata.find("\"engineBuildPolicyVersion\": 1") != std::string::npos &&
        metadata.find("\"engineCommit\": \"" + std::string(40U, 'c') + "\"") != std::string::npos &&
        metadata.find("\"engineSourceTreeState\": \"clean\"") != std::string::npos &&
        metadata.find("\"engineSourceOrigin\": \"InstalledDistribution\"") != std::string::npos &&
        metadata.find("\"distributionSourceInventorySha256\": \"" + std::string(64U, 'a') + "\"") !=
            std::string::npos &&
        metadata.find("\"publisherBuildIdentitySha256\": \"" + std::string(64U, 'b') + "\"") !=
            std::string::npos &&
        metadata.find("\"engineSourceInventory\": {") != std::string::npos &&
        metadata.find("\"gameSourceInventory\": {") != std::string::npos &&
        metadata.find("\"cmake\": {") != std::string::npos &&
        metadata.find("\"version\": \"" + std::string(CUE_TEST_CMAKE_VERSION) + "\"") != std::string::npos &&
        metadata.find("\"generator\": \"" + std::string(CUE_TEST_CMAKE_GENERATOR) + "\"") != std::string::npos &&
        metadata.find("\"platformToolset\": \"" + std::string(CUE_TEST_PLATFORM_TOOLSET) + "\"") != std::string::npos &&
        metadata.find("\"msvcToolsetVersion\": \"" + std::string(CUE_TEST_MSVC_TOOLSET_VERSION) + "\"") !=
            std::string::npos &&
        metadata.find("\"compilerSha256\": \"") != std::string::npos &&
        metadata.find("\"compilerFileVersion\": \"" + current_compiler_version_text() + "\"") != std::string::npos &&
        metadata.find("\"windowsSdkVersion\": \"" + std::string(CUE_TEST_WINDOWS_SDK_VERSION) + "\"") !=
            std::string::npos &&
        metadata.find("\"vcpkgManifestSha256\": \"") != std::string::npos &&
        metadata.find("\"vcpkgBaselineSha256\": \"") != std::string::npos &&
        metadata.find("\"securityValidation\": {") != std::string::npos &&
        metadata.find("\"dependentLoadFlags\": \"0x0800\"") != std::string::npos &&
        metadata.find("\"importPolicy\": \"M17AllowlistV1\"") != std::string::npos &&
        metadata.find("\"gameModuleLoaderLinked\": false") != std::string::npos &&
        metadata.find("\"kernel32.dll\"") != std::string::npos &&
        metadata.find("\"signatureStatus\": \"Unsigned\"") != std::string::npos &&
        metadata.find("\"distributionStatus\": \"LocalExecutionOnly\"") != std::string::npos &&
        metadata.find("\"publicDistributionReady\": false") != std::string::npos &&
        metadata.find(published->files()[0].contentHash) != std::string::npos);

    std::unique_ptr<cue::BuildArtifactReader> reader =
        take_value(cue::create_windows_build_artifact_reader(generic_path(projectRoot), descriptor, a_assertContext));
    TestArtifactReadCancellation readCancellation;
    auto initialRead = take_value(reader->acquire_current_read_lease(*published, readCancellation, std::nullopt));
    require(initialRead.has_value() && (*initialRead)->project_id() == k_projectId);
    initialRead.reset();
    cue::ProjectDescriptor otherDescriptor = make_descriptor(k_otherProjectId, a_assertContext);
    std::unique_ptr<cue::BuildArtifactReader> otherProjectReader = take_value(
        cue::create_windows_build_artifact_reader(generic_path(projectRoot), otherDescriptor, a_assertContext));
    auto otherProjectRead = otherProjectReader->acquire_current_read_lease(*published, readCancellation, std::nullopt);
    require(!otherProjectRead && otherProjectRead.try_error()->root_code().domain() == "Cue.Build.Windows.Artifact" &&
            otherProjectRead.try_error()->root_code().value() ==
                static_cast<std::int64_t>(cue::WindowsBuildArtifactError::InvalidSettings));

    cue::BuildPlan acquireCancelledPlan =
        make_shipping_plan(projectRoot, "71234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    cue::ChildProcessCancellation acquireCancellation;
    acquireCancellation.request_cancel();
    auto acquireCancelled = publisher->acquire_build_lease(acquireCancelledPlan, acquireCancellation, std::nullopt);
    require(acquireCancelled.has_value() && !acquireCancelled.try_value()->has_value());
    require(read_text(currentPath) == current);

    cue::BuildPlan publishCancelledPlan =
        make_shipping_plan(projectRoot, "81234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto publishCancelledLease =
        take_value(publisher->acquire_build_lease(publishCancelledPlan, cancellation, std::nullopt));
    require(publishCancelledLease.has_value());
    cue::ChildProcessCancellation publishCancellation;
    publishCancellation.request_cancel();
    auto publishCancelled =
        publisher->publish(publishCancelledPlan, publishCancellation, std::move(*publishCancelledLease), std::nullopt);
    require(publishCancelled.has_value() && !publishCancelled.try_value()->has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(publishCancelledPlan.candidate_directory())));

    cue::BuildPlan lateCancelledPlan =
        make_shipping_plan(projectRoot, "a1234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto lateCancelledLease = take_value(publisher->acquire_build_lease(lateCancelledPlan, cancellation, std::nullopt));
    require(lateCancelledLease.has_value());
    {
        constexpr std::streamoff cancellationWindowBytes = 32LL * 1024LL * 1024LL;
        std::ofstream pdb(output / "CueGameProduct.pdb", std::ios::binary | std::ios::trunc);
        pdb.seekp(cancellationWindowBytes - 1LL);
        pdb.put('\0');
        require(static_cast<bool>(pdb));
    }
    using PublishResult = cue::Result<std::optional<cue::BuildArtifactInventory>>;
    std::unique_ptr<PublishResult> lateCancelledResult;
    cue::ChildProcessCancellation lateCancellation;
    std::thread lateCancellationThread(
        [&]()
        {
            lateCancelledResult = std::make_unique<PublishResult>(
                publisher->publish(lateCancelledPlan, lateCancellation, std::move(*lateCancelledLease), std::nullopt));
        });
    const std::filesystem::path lateVersion = std::filesystem::path(lateCancelledPlan.artifact_store_directory()) /
                                              "Versions" / std::string(lateCancelledPlan.operation_id());
    for (std::size_t attempt = 0U; attempt < 5000U && !std::filesystem::exists(lateVersion); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(std::filesystem::exists(lateVersion));
    lateCancellation.request_cancel();
    lateCancellationThread.join();
    require(lateCancelledResult != nullptr && lateCancelledResult->has_value() &&
            !lateCancelledResult->try_value()->has_value());
    require(read_text(currentPath) == current && std::filesystem::is_directory(lateVersion));
    write_text(output / "CueGameProduct.pdb", "shipping-product-development-symbol");

    cue::BuildPlan mismatchedToolchainPlan =
        make_shipping_plan(projectRoot, "91234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto mismatchedToolchainLease =
        take_value(publisher->acquire_build_lease(mismatchedToolchainPlan, cancellation, std::nullopt));
    require(mismatchedToolchainLease.has_value());
    writeToolchainEvidence("0.0.0.0");
    require(
        !publisher->publish(mismatchedToolchainPlan, cancellation, std::move(*mismatchedToolchainLease), std::nullopt)
             .has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(mismatchedToolchainPlan.candidate_directory())));
    writeToolchainEvidence();
    auto mismatchedMinorToolsetLease =
        take_value(publisher->acquire_build_lease(mismatchedToolchainPlan, cancellation, std::nullopt));
    require(mismatchedMinorToolsetLease.has_value());
    write_shipping_toolchain_evidence(binary, CUE_TEST_WINDOWS_SDK_VERSION, CUE_TEST_PLATFORM_TOOLSET,
                                      generic_path(installedEngineRoot), "14.99.99999");
    require(!publisher
                 ->publish(mismatchedToolchainPlan, cancellation, std::move(*mismatchedMinorToolsetLease), std::nullopt)
                 .has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(mismatchedToolchainPlan.candidate_directory())));
    writeToolchainEvidence();
    auto mismatchedProjectToolsetLease =
        take_value(publisher->acquire_build_lease(mismatchedToolchainPlan, cancellation, std::nullopt));
    require(mismatchedProjectToolsetLease.has_value());
    write_shipping_toolchain_evidence(binary, CUE_TEST_WINDOWS_SDK_VERSION, CUE_TEST_PLATFORM_TOOLSET,
                                      generic_path(installedEngineRoot), CUE_TEST_MSVC_TOOLSET_VERSION,
                                      "14.51.99999");
    require(!publisher
                 ->publish(mismatchedToolchainPlan, cancellation, std::move(*mismatchedProjectToolsetLease),
                           std::nullopt)
                 .has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(mismatchedToolchainPlan.candidate_directory())));
    writeToolchainEvidence();
    auto mismatchedPlatformToolsetLease =
        take_value(publisher->acquire_build_lease(mismatchedToolchainPlan, cancellation, std::nullopt));
    require(mismatchedPlatformToolsetLease.has_value());
    writeToolchainEvidence(CUE_TEST_WINDOWS_SDK_VERSION, "v999");
    require(
        !publisher
             ->publish(mismatchedToolchainPlan, cancellation, std::move(*mismatchedPlatformToolsetLease), std::nullopt)
             .has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(mismatchedToolchainPlan.candidate_directory())));
    writeToolchainEvidence();
    auto mismatchedEngineRootLease =
        take_value(publisher->acquire_build_lease(mismatchedToolchainPlan, cancellation, std::nullopt));
    require(mismatchedEngineRootLease.has_value());
    write_shipping_toolchain_evidence(binary, CUE_TEST_WINDOWS_SDK_VERSION, CUE_TEST_PLATFORM_TOOLSET,
                                      generic_path(projectRoot));
    require(
        !publisher->publish(mismatchedToolchainPlan, cancellation, std::move(*mismatchedEngineRootLease), std::nullopt)
             .has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(mismatchedToolchainPlan.candidate_directory())));
    writeToolchainEvidence();

    cue::BuildPlan changedSourcePlan =
        make_shipping_plan(projectRoot, "41234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto changedSourceLease = take_value(publisher->acquire_build_lease(changedSourcePlan, cancellation, std::nullopt));
    require(changedSourceLease.has_value());
    write_text(projectRoot / "Source" / "Game" / "Test.cpp", "int cue_shipping_test = 2;\n");
    require(
        !publisher->publish(changedSourcePlan, cancellation, std::move(*changedSourceLease), std::nullopt).has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(changedSourcePlan.candidate_directory())));
    write_text(projectRoot / "Source" / "Game" / "Test.cpp", "int cue_shipping_test = 1;\n");

    cue::BuildPlan changedEngineSourcePlan =
        make_shipping_plan(projectRoot, "71234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto changedEngineSourceLease =
        take_value(publisher->acquire_build_lease(changedEngineSourcePlan, cancellation, std::nullopt));
    require(changedEngineSourceLease.has_value());
    write_text(installedEngineRoot / "Engine" / "Source" / "Test.cpp", "int cue_engine_test = 2;\n");
    require(!publisher
                 ->publish(changedEngineSourcePlan, cancellation, std::move(*changedEngineSourceLease), std::nullopt)
                 .has_value());
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(changedEngineSourcePlan.candidate_directory())));
    write_text(installedEngineRoot / "Engine" / "Source" / "Test.cpp", "int cue_engine_test = 1;\n");

    cue::BuildPlan signedPlan =
        make_signed_shipping_plan(projectRoot, "51234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    auto signedLease = take_value(publisher->acquire_build_lease(signedPlan, cancellation, std::nullopt));
    require(signedLease.has_value());
    auto signedPublish = publisher->publish(signedPlan, cancellation, std::move(*signedLease), std::nullopt);
    require(!signedPublish && signedPublish.try_error()->root_code().domain() == "Cue.Build.Windows.Artifact" &&
            signedPublish.try_error()->root_code().value() ==
                static_cast<std::int64_t>(cue::WindowsBuildArtifactError::PublisherUnavailable));
    require(read_text(currentPath) == current &&
            !std::filesystem::exists(std::filesystem::path(signedPlan.artifact_store_directory()) / "Current.json"));

    const auto publishRejected = [&](const std::filesystem::path &a_source, std::string a_operationId)
    {
        cue::BuildPlan rejectedPlan = make_shipping_plan(projectRoot, std::move(a_operationId), a_assertContext);
        require(std::filesystem::remove(output / "CueGameProduct.exe"));
        require(std::filesystem::copy_file(a_source, output / "CueGameProduct.exe"));
        auto rejectedLease = take_value(publisher->acquire_build_lease(rejectedPlan, cancellation, std::nullopt));
        require(rejectedLease.has_value());
        require(!publisher->publish(rejectedPlan, cancellation, std::move(*rejectedLease), std::nullopt).has_value());
        require(read_text(currentPath) == current &&
                !std::filesystem::exists(std::filesystem::path(rejectedPlan.candidate_directory())));
    };
    publishRejected(a_wrongProjectProduct, "11234567-89ab-4cde-8f01-23456789abcd");
    publishRejected(a_wrongConfigurationProduct, "21234567-89ab-4cde-8f01-23456789abcd");
    publishRejected(a_extraFileProduct, "61234567-89ab-4cde-8f01-23456789abcd");

    cue::BuildPlan incompletePlan =
        make_shipping_plan(projectRoot, "31234567-89ab-4cde-8f01-23456789abcd", a_assertContext);
    require(std::filesystem::remove(output / "CueGameProduct.exe"));
    auto incompleteLease = take_value(publisher->acquire_build_lease(incompletePlan, cancellation, std::nullopt));
    require(incompleteLease.has_value());
    require(!publisher->publish(incompletePlan, cancellation, std::move(*incompleteLease), std::nullopt).has_value());
    require(read_text(currentPath) == current);

    auto retainedRead = take_value(reader->acquire_current_read_lease(*published, readCancellation, std::nullopt));
    require(retainedRead.has_value());
    retainedRead.reset();
    write_text(metadataPath, metadata + "tampered");
    require(!reader->acquire_current_read_lease(*published, readCancellation, std::nullopt).has_value());

    std::filesystem::remove_all(projectRoot, error);
    require(!error);
}
} // namespace

/// @brief Windows Artifact PublisherのProcess間契約とAtomic Current保全を検証する
int main(int a_argumentCount, char **a_arguments)
{
    require(a_argumentCount == 10);
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    test_reparse_revalidation(std::filesystem::path(a_arguments[1]), assertContext);
    test_windows_artifact_publisher(std::filesystem::path(a_arguments[1]), std::filesystem::path(a_arguments[2]),
                                    std::filesystem::path(a_arguments[3]), std::filesystem::path(a_arguments[4]),
                                    std::filesystem::path(a_arguments[5]), assertContext);
    test_shipping_product_publisher(std::filesystem::path(a_arguments[6]), std::filesystem::path(a_arguments[7]),
                                    std::filesystem::path(a_arguments[8]), std::filesystem::path(a_arguments[9]),
                                    assertContext);
    return 0;
}
