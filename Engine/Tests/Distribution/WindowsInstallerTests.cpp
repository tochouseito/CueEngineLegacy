#include <Cue/Distribution/Error.h>
#include <Cue/Distribution/InstallState.h>
#include <Cue/Distribution/Manifest.h>
#include <Cue/Distribution/Publisher.h>
#include <Cue/Distribution/Windows/Installer.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }

    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

/// @brief Test条件を満たさない場合にSource位置を出力して終了する
void require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::fprintf(stderr, "Requirement failed at %s:%u\n", a_location.file_name(), a_location.line());
        std::_Exit(1);
    }
}

/// @brief Test Process固有のTemporary Rootを所有して終了時に回収する
class TemporaryRoot final
{
  public:
    /// @brief System Temporary Directory配下にProcess固有Rootを作成する
    TemporaryRoot()
    {
        std::array<wchar_t, MAX_PATH + 1U> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        require(length > 0U && length < temporary.size());
        m_path = std::filesystem::path(temporary.data()) /
                 (L"CueDistributionInstallerTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64()));
        std::error_code error;
        std::filesystem::create_directories(m_path, error);
        require(!error);
    }
    TemporaryRoot(const TemporaryRoot &) = delete;
    TemporaryRoot &operator=(const TemporaryRoot &) = delete;
    TemporaryRoot(TemporaryRoot &&) = delete;
    TemporaryRoot &operator=(TemporaryRoot &&) = delete;
    /// @brief Test Rootを再帰回収する
    ~TemporaryRoot() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }
    /// @brief Temporary Root Pathを返す
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

/// @brief 起動ProcessとPrimary Thread Handleを一意所有する
class ProcessOwner final
{
  public:
    /// @brief CreateProcess結果のHandle所有権を取得する
    explicit ProcessOwner(PROCESS_INFORMATION a_process) noexcept
        : m_process(a_process.hProcess), m_thread(a_process.hThread)
    {
    }
    ProcessOwner(const ProcessOwner &) = delete;
    ProcessOwner &operator=(const ProcessOwner &) = delete;
    /// @brief Process Handle所有権を移動する
    ProcessOwner(ProcessOwner &&a_other) noexcept
        : m_process(std::exchange(a_other.m_process, nullptr)), m_thread(std::exchange(a_other.m_thread, nullptr))
    {
    }
    ProcessOwner &operator=(ProcessOwner &&) = delete;
    /// @brief ProcessとThread Handleを閉じる
    ~ProcessOwner() noexcept
    {
        if (m_thread != nullptr)
        {
            static_cast<void>(CloseHandle(m_thread));
        }
        if (m_process != nullptr)
        {
            static_cast<void>(CloseHandle(m_process));
        }
    }
    /// @brief Process Handleを返す
    [[nodiscard]] HANDLE process() const noexcept
    {
        return m_process;
    }

  private:
    HANDLE m_process;
    HANDLE m_thread;
};

/// @brief UTF-16 Pathを公開API用UTF-8へ変換する
[[nodiscard]] std::string utf8_path(const std::filesystem::path &a_path)
{
    const std::u8string value = a_path.u8string();
    return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}

/// @brief Byte列を親Directory作成後にFileへ書き込む
void write_bytes(const std::filesystem::path &a_path, std::span<const std::byte> a_bytes)
{
    std::error_code error;
    std::filesystem::create_directories(a_path.parent_path(), error);
    require(!error);
    std::ofstream output(a_path, std::ios::binary | std::ios::trunc);
    require(output.good());
    output.write(reinterpret_cast<const char *>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
    require(output.good());
}

/// @brief UTF-8 TextをFileへ書き込む
void write_text(const std::filesystem::path &a_path, std::string_view a_text)
{
    const auto *begin = reinterpret_cast<const std::byte *>(a_text.data());
    write_bytes(a_path, std::span(begin, a_text.size()));
}

/// @brief File全体をByte列として読み込む
[[nodiscard]] std::vector<std::byte> read_bytes(const std::filesystem::path &a_path)
{
    std::ifstream input(a_path, std::ios::binary | std::ios::ate);
    require(input.good());
    const std::streamsize size = input.tellg();
    require(size >= 0);
    input.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char *>(bytes.data()), size);
    require(input.good());
    return bytes;
}

/// @brief File全体のSHA-256を計算する
[[nodiscard]] std::string file_digest(const std::filesystem::path &a_path, const cue::AssertContext &a_assertContext)
{
    const std::vector<std::byte> bytes = read_bytes(a_path);
    auto digest = cue::distribution::compute_distribution_sha256(bytes, a_assertContext);
    require(digest.has_value());
    return std::move(*digest.try_value());
}

/// @brief 現在のBuild出力にあるInstaller Tool Pathを返す
[[nodiscard]] std::filesystem::path installer_executable()
{
    std::array<wchar_t, 32768U> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(length > 0U && length < path.size());
    return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path() / L"CueEngineInstallerTool.exe";
}

/// @brief 現在のBuild出力にあるBlocking Probe Helper Pathを返す
[[nodiscard]] std::filesystem::path probe_helper_executable()
{
    return installer_executable().parent_path() / L"CueDistributionInstallerProbeHelper.exe";
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

/// @brief Installer CLIを独立Processとして起動する
[[nodiscard]] std::optional<ProcessOwner> start_installer_process(const std::filesystem::path &a_bundleRoot,
                                                                  const std::filesystem::path &a_installRoot)
{
    const std::filesystem::path executable = installer_executable();
    std::wstring command = quote_argument(executable.native());
    command.append(L" install --bundle-root ");
    command.append(quote_argument(a_bundleRoot.native()));
    command.append(L" --install-root ");
    command.append(quote_argument(a_installRoot.native()));
    command.append(L" --allow-unsigned-local");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = a_bundleRoot.parent_path().native();
    if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       workingDirectory.c_str(), &startup, &process) == FALSE)
    {
        return std::nullopt;
    }
    return ProcessOwner(process);
}

/// @brief Process終了を待ってExit Codeを返す
[[nodiscard]] std::optional<DWORD> wait_process(const ProcessOwner &a_process, DWORD a_timeoutMilliseconds) noexcept
{
    if (WaitForSingleObject(a_process.process(), a_timeoutMilliseconds) != WAIT_OBJECT_0)
    {
        return std::nullopt;
    }
    DWORD exitCode = 0U;
    return GetExitCodeProcess(a_process.process(), &exitCode) != FALSE ? std::optional(exitCode) : std::nullopt;
}

/// @brief Ready通知Fileへ有効なProcess IDが書き終わるまで待機する
[[nodiscard]] std::optional<DWORD> wait_for_process_id(const std::filesystem::path &a_path,
                                                       std::chrono::seconds a_timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + a_timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::ifstream input(a_path);
        unsigned long value = 0UL;
        if ((input >> value) && value != 0UL)
        {
            return static_cast<DWORD>(value);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

/// @brief Publisher Build Identityの有効なTest値を生成する
[[nodiscard]] cue::distribution::PublisherBuildIdentity publisher_identity()
{
    return {
        std::string(40U, 'b'),
        "x64-windows",
        cue::distribution::DistributionArchitecture::X64,
        cue::distribution::DistributionArchitecture::X64,
        "msvc",
        "19.51.36231",
        "14.51.36231",
        "dynamic",
        "14.51.36231",
        "10.0.26100.0",
        "Release",
    };
}

/// @brief BundleへFileを追加してManifest Inventory Entryを返す
[[nodiscard]] cue::distribution::DistributionFileEntry add_file(const std::filesystem::path &a_bundleRoot,
                                                                cue::distribution::DistributionFileRole a_role,
                                                                std::string a_relativePath,
                                                                std::span<const std::byte> a_bytes,
                                                                const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path path = a_bundleRoot / std::filesystem::path(a_relativePath);
    write_bytes(path, a_bytes);
    return {a_role, std::move(a_relativePath), a_bytes.size(), file_digest(path, a_assertContext)};
}

/// @brief Install Transaction用の完全なCanonical Test Bundleを作成する
void create_bundle(const std::filesystem::path &a_bundleRoot, const cue::AssertContext &a_assertContext,
                   const std::filesystem::path &a_probeExecutable = installer_executable(),
                   std::size_t a_paddingBytes = 0U, std::string a_bundleId = "12345678-1234-4abc-8def-1234567890ab",
                   std::string a_engineVersion = "1.0.0", std::byte a_sourceByte = std::byte{'c'})
{
    using cue::distribution::DistributionFileRole;
    const std::vector<std::byte> executable = read_bytes(installer_executable());
    const std::vector<std::byte> probeExecutable = read_bytes(a_probeExecutable);
    const std::array<std::byte, 4U> sourceBytes = {a_sourceByte, a_sourceByte, a_sourceByte, std::byte{'\n'}};
    cue::distribution::DistributionManifest manifest;
    manifest.bundleId = std::move(a_bundleId);
    manifest.engineVersion = std::move(a_engineVersion);
    manifest.engineSourceRevision = std::string(40U, 'b');
    manifest.sourceInventoryHash = std::string(64U, '1');
    manifest.dependencyDefinitionId = std::string(64U, '2');
    manifest.publisherBuildIdentity = publisher_identity();
    manifest.minimumToolchain = {"4.2.0", "2.44.0", "msvc", "19.51.36231", "10.0.26100.0"};
    manifest.entryPoints = {
        "Bin/CueEngineBootstrap.exe", "Bin/CueProjectHubTool.exe",      "Bin/CueEditorTool.exe",
        "Bin/CueRuntimeHost.exe",     "Bin/CueEngineInstallerTool.exe", "Bin/CueEngineInstallWorker.exe",
    };
    const std::array toolFiles = {
        std::pair{DistributionFileRole::Bootstrap, "Bin/CueEngineBootstrap.exe"},
        std::pair{DistributionFileRole::ProjectHub, "Bin/CueProjectHubTool.exe"},
        std::pair{DistributionFileRole::Editor, "Bin/CueEditorTool.exe"},
        std::pair{DistributionFileRole::RuntimeHost, "Bin/CueRuntimeHost.exe"},
        std::pair{DistributionFileRole::Installer, "Bin/CueEngineInstallerTool.exe"},
        std::pair{DistributionFileRole::InstallWorker, "Bin/CueEngineInstallWorker.exe"},
    };
    for (const auto &[role, path] : toolFiles)
    {
        manifest.files.push_back(
            add_file(a_bundleRoot, role, path,
                     role == DistributionFileRole::Installer ? std::span(probeExecutable) : std::span(executable),
                     a_assertContext));
    }
    const std::array sourceFiles = {
        std::pair{DistributionFileRole::EngineSource, "Engine/Source/Foundation/Test.cpp"},
        std::pair{DistributionFileRole::Hlsl, "Engine/Source/Renderer/Shaders/Test.hlsl"},
        std::pair{DistributionFileRole::CMake, "CMakeLists.txt"},
        std::pair{DistributionFileRole::Script, "Tools/Dependencies/RestoreVcpkg.ps1"},
        std::pair{DistributionFileRole::Document, "Engine/Documents/CODING_RULES.md"},
        std::pair{DistributionFileRole::License, "LICENSE.txt"},
        std::pair{DistributionFileRole::DependencyDefinition, "ThirdParty/vcpkg.json"},
        std::pair{DistributionFileRole::DependencyDefinition, "ThirdParty/vcpkg-configuration.json"},
        std::pair{DistributionFileRole::DependencyDefinition, "ThirdParty/vcpkg-tool.json"},
        std::pair{DistributionFileRole::ThirdPartyNotice, "ThirdParty/THIRD_PARTY_NOTICES.md"},
        std::pair{DistributionFileRole::ThirdPartyLicense, "ThirdParty/Licenses/DearImGui-LICENSE.txt"},
        std::pair{DistributionFileRole::ThirdPartyLicense, "ThirdParty/Licenses/vcpkg-LICENSE.txt"},
    };
    for (const auto &[role, path] : sourceFiles)
    {
        manifest.files.push_back(add_file(a_bundleRoot, role, path, sourceBytes, a_assertContext));
    }
    if (a_paddingBytes != 0U)
    {
        const std::vector<std::byte> padding(a_paddingBytes, std::byte{0x5a});
        manifest.files.push_back(add_file(a_bundleRoot, DistributionFileRole::EngineSource,
                                          "Engine/Source/Foundation/CrashPadding.cpp", padding, a_assertContext));
    }
    auto manifestBytes = cue::distribution::write_distribution_manifest(manifest, a_assertContext);
    require(manifestBytes.has_value());
    write_text(a_bundleRoot / L"CueEngineDistribution.json", *manifestBytes.try_value());
}

/// @brief RegistryをCanonical Readerで読み込む
[[nodiscard]] cue::distribution::InstalledVersionsRegistry read_registry(const std::filesystem::path &a_installRoot,
                                                                         const cue::AssertContext &a_assertContext)
{
    const std::vector<std::byte> bytes = read_bytes(a_installRoot / L"State" / L"InstalledVersions.json");
    const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    auto registry = cue::distribution::read_installed_versions_registry(text, a_assertContext);
    require(registry.has_value());
    return std::move(*registry.try_value());
}

/// @brief Install Root内のUser所有Sentinelが一切変化していないことを検証する
void require_sentinels(const std::filesystem::path &a_installRoot)
{
    constexpr std::array roots = {L"Projects", L"Recent", L"Preferences", L"BuildArtifacts"};
    for (const wchar_t *root : roots)
    {
        const std::vector<std::byte> bytes = read_bytes(a_installRoot / root / L"keep.txt");
        require(bytes.size() == 4U);
        require(std::to_integer<char>(bytes[0]) == 'k');
    }
}

/// @brief Install／冪等再実行／Registry Recoveryと非対象Data不変を統合検証する
void test_install_transaction(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"Bundle";
    const std::filesystem::path collisionBundleRoot = temporary.path() / L"CollisionBundle";
    const std::filesystem::path updateBundleRoot = temporary.path() / L"UpdateBundle";
    const std::filesystem::path installRoot = temporary.path() / L"Install";
    create_bundle(bundleRoot, a_assertContext);
    create_bundle(collisionBundleRoot, a_assertContext, installer_executable(), 0U,
                  "12345678-1234-4abc-8def-1234567890ab", "1.0.0", std::byte{'x'});
    create_bundle(updateBundleRoot, a_assertContext, installer_executable(), 0U, "22345678-1234-4abc-8def-1234567890ab",
                  "1.1.0", std::byte{'u'});
    constexpr std::array sentinelRoots = {L"Projects", L"Recent", L"Preferences", L"BuildArtifacts"};
    for (const wchar_t *root : sentinelRoots)
    {
        write_text(installRoot / root / L"keep.txt", "keep");
    }

    cue::distribution::WindowsInstallRequest request{utf8_path(bundleRoot), utf8_path(installRoot), false, true};
    auto installed = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    if (!installed)
    {
        std::fprintf(stderr, "Install failed: %.*s\n", static_cast<int>(installed.try_error()->summary().size()),
                     installed.try_error()->summary().data());
        for (const cue::ErrorContext &context : installed.try_error()->contexts())
        {
            std::fprintf(stderr, "  Context: %.*s\n", static_cast<int>(context.message().size()),
                         context.message().data());
        }
    }
    require(installed.has_value());
    require(!installed.try_value()->wasAlreadyInstalled);
    require(installed.try_value()->registryRevision == 2U);
    require(std::filesystem::is_regular_file(installRoot / L"Versions" /
                                             std::filesystem::path(installed.try_value()->versionDirectory) /
                                             L"CueEngineProbe.complete.json"));
    require(std::filesystem::is_regular_file(installRoot / L"Operations" / L"Workers" /
                                             std::filesystem::path(installed.try_value()->workerId) /
                                             L"CueEngineInstallWorker.complete.json"));
    cue::distribution::InstalledVersionsRegistry registry = read_registry(installRoot, a_assertContext);
    require(registry.revision == 2U && registry.versions.size() == 1U);
    require_sentinels(installRoot);

    constexpr std::string_view completedOperationId = "55555555-5555-4555-8555-555555555555";
    cue::distribution::InstallOperationJournal completedJournal;
    completedJournal.operationId = completedOperationId;
    completedJournal.kind = cue::distribution::InstallOperationKind::Install;
    completedJournal.stage = cue::distribution::InstallOperationStage::RegistryPublished;
    completedJournal.workerId = installed.try_value()->workerId;
    completedJournal.expectedRegistry =
        cue::distribution::ExpectedRegistry{registry.generationId, registry.revision - 1U};
    completedJournal.target = cue::distribution::InstallOperationTarget{registry.versions.front().directoryName,
                                                                        registry.versions.front().bundleId,
                                                                        registry.versions.front().manifestDigest};
    auto completedJournalBytes = cue::distribution::write_install_operation_journal(completedJournal, a_assertContext);
    require(completedJournalBytes.has_value());
    const std::filesystem::path completedJournalPath =
        installRoot / L"Operations" / L"Journals" / L"55555555-5555-4555-8555-555555555555.json";
    write_text(completedJournalPath, *completedJournalBytes.try_value());
    auto completedResume = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(completedResume.has_value() && completedResume.try_value()->wasAlreadyInstalled);
    require(!std::filesystem::exists(completedJournalPath));
    require(completedResume.try_value()->registryRevision == 2U);

    cue::distribution::WindowsInstallRequest collisionRequest{utf8_path(collisionBundleRoot), utf8_path(installRoot),
                                                              false, true};
    auto collision = cue::distribution::install_windows_source_sdk(collisionRequest, a_assertContext);
    require(!collision);
    registry = read_registry(installRoot, a_assertContext);
    require(registry.revision == 2U && registry.versions.size() == 1U);
    require_sentinels(installRoot);

    cue::distribution::WindowsInstallRequest updateRequest{utf8_path(updateBundleRoot), utf8_path(installRoot), true,
                                                           true};
    auto updated = cue::distribution::install_windows_source_sdk(updateRequest, a_assertContext);
    require(updated.has_value() && !updated.try_value()->wasAlreadyInstalled);
    registry = read_registry(installRoot, a_assertContext);
    require(registry.revision == 3U && registry.versions.size() == 2U);
    require_sentinels(installRoot);

    auto repeated = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(repeated.has_value() && repeated.try_value()->wasAlreadyInstalled);
    require(repeated.try_value()->registryRevision == 3U);
    require_sentinels(installRoot);

    const std::filesystem::path abandonedJournalTemporary =
        installRoot / L"Operations" / L"Journals" /
        L"88888888-8888-4888-8888-888888888888.json.tmp-99999999-9999-4999-8999-999999999999";
    write_text(abandonedJournalTemporary, "partial\n");
    auto temporaryRecovered = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(temporaryRecovered.has_value() && temporaryRecovered.try_value()->wasAlreadyInstalled);
    require(!std::filesystem::exists(abandonedJournalTemporary));
    require_sentinels(installRoot);

    auto registryBytes = cue::distribution::write_installed_versions_registry(registry, a_assertContext);
    require(registryBytes.has_value());
    std::string unknownRegistry = *registryBytes.try_value();
    unknownRegistry.insert(unknownRegistry.find(",\"versions\""), ",\"unknown\":false");
    write_text(installRoot / L"State" / L"InstalledVersions.json", unknownRegistry);
    require(!cue::distribution::install_windows_source_sdk(request, a_assertContext));
    const std::vector<std::byte> preservedUnknown = read_bytes(installRoot / L"State" / L"InstalledVersions.json");
    require(std::string(reinterpret_cast<const char *>(preservedUnknown.data()), preservedUnknown.size()) ==
            unknownRegistry);

    const std::string unsupportedRegistry = "{\"schemaVersion\":2}\n";
    write_text(installRoot / L"State" / L"InstalledVersions.json", unsupportedRegistry);
    require(!cue::distribution::install_windows_source_sdk(request, a_assertContext));
    const std::vector<std::byte> preservedUnsupported = read_bytes(installRoot / L"State" / L"InstalledVersions.json");
    require(std::string(reinterpret_cast<const char *>(preservedUnsupported.data()), preservedUnsupported.size()) ==
            unsupportedRegistry);

    const std::string truncatedUnsupportedRegistry = "{\"schemaVersion\":2";
    write_text(installRoot / L"State" / L"InstalledVersions.json", truncatedUnsupportedRegistry);
    require(!cue::distribution::install_windows_source_sdk(request, a_assertContext));
    const std::vector<std::byte> preservedTruncatedUnsupported =
        read_bytes(installRoot / L"State" / L"InstalledVersions.json");
    require(std::string(reinterpret_cast<const char *>(preservedTruncatedUnsupported.data()),
                        preservedTruncatedUnsupported.size()) == truncatedUnsupportedRegistry);

    write_text(installRoot / L"State" / L"InstalledVersions.json", "{\"schemaVersion\":1");
    auto truncatedRecovered = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(truncatedRecovered.has_value() && truncatedRecovered.try_value()->wasAlreadyInstalled);
    registry = read_registry(installRoot, a_assertContext);
    require(registry.revision == 1U && registry.versions.size() == 2U);

    const std::string corruptRegistry = "{broken}\n";
    write_text(installRoot / L"State" / L"InstalledVersions.json", corruptRegistry);
    const auto *corruptBegin = reinterpret_cast<const std::byte *>(corruptRegistry.data());
    auto corruptDigest = cue::distribution::compute_distribution_sha256(std::span(corruptBegin, corruptRegistry.size()),
                                                                        a_assertContext);
    require(corruptDigest.has_value());
    constexpr std::string_view recoveryId = "33333333-3333-4333-8333-333333333333";
    cue::distribution::InstallOperationJournal recoveryJournal;
    recoveryJournal.operationId = recoveryId;
    recoveryJournal.kind = cue::distribution::InstallOperationKind::RegistryRecovery;
    recoveryJournal.stage = cue::distribution::InstallOperationStage::Prepared;
    recoveryJournal.workerId = installed.try_value()->workerId;
    recoveryJournal.sourceRegistryEvidence = cue::distribution::RegistrySourceEvidence{
        false, "InstalledVersions.corrupt-33333333-3333-4333-8333-333333333333.json", corruptRegistry.size(),
        std::move(*corruptDigest.try_value())};
    auto recoveryJournalBytes = cue::distribution::write_install_operation_journal(recoveryJournal, a_assertContext);
    require(recoveryJournalBytes.has_value());
    write_text(installRoot / L"Operations" / L"Journals" / L"33333333-3333-4333-8333-333333333333.json",
               *recoveryJournalBytes.try_value());
    auto recovered = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(recovered.has_value() && recovered.try_value()->wasAlreadyInstalled);
    registry = read_registry(installRoot, a_assertContext);
    require(registry.revision == 1U && registry.versions.size() == 2U);
    require(registry.generationId == recoveryId);
    require(std::filesystem::is_regular_file(installRoot / L"Operations" / L"Evidence" /
                                             L"InstalledVersions.corrupt-33333333-3333-4333-8333-333333333333.json"));
    require_sentinels(installRoot);

    write_text(installRoot / L"State" / L"InstalledVersions.json", "");
    auto emptyRecovered = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(emptyRecovered.has_value() && emptyRecovered.try_value()->wasAlreadyInstalled);
    registry = read_registry(installRoot, a_assertContext);
    require(registry.revision == 1U && registry.versions.size() == 2U);
    bool hasEmptyEvidence = false;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(installRoot / L"Operations" / L"Evidence"))
    {
        if (entry.is_regular_file() && entry.file_size() == 0U)
        {
            hasEmptyEvidence = true;
        }
    }
    require(hasEmptyEvidence);
    require_sentinels(installRoot);

    const std::filesystem::path unsignedRoot = temporary.path() / L"UnsignedRejected";
    cue::distribution::WindowsInstallRequest unsignedRequest{utf8_path(bundleRoot), utf8_path(unsignedRoot), false,
                                                             false};
    require(!cue::distribution::install_windows_source_sdk(unsignedRequest, a_assertContext));
    require(!std::filesystem::exists(unsignedRoot));

    const std::filesystem::path embeddedNullRoot = temporary.path() / L"EmbeddedNullRejected";
    std::string embeddedNullPath = utf8_path(embeddedNullRoot);
    embeddedNullPath.append("\0outside", 8U);
    cue::distribution::WindowsInstallRequest embeddedNullRequest{utf8_path(bundleRoot), std::move(embeddedNullPath),
                                                                 false, true};
    require(!cue::distribution::install_windows_source_sdk(embeddedNullRequest, a_assertContext));
    require(!std::filesystem::exists(embeddedNullRoot));

    const std::filesystem::path collisionRoot = temporary.path() / L"ManagedPathCollision";
    write_text(collisionRoot / L"Operations" / L"Journals", "collision\n");
    cue::distribution::WindowsInstallRequest pathCollisionRequest{utf8_path(bundleRoot), utf8_path(collisionRoot),
                                                                  false, true};
    require(!cue::distribution::install_windows_source_sdk(pathCollisionRequest, a_assertContext));
    require(!std::filesystem::exists(collisionRoot / L"State" / L"InstalledVersions.json"));
    require(!std::filesystem::exists(collisionRoot / L"Versions" / L"v1.0.0--12345678-1234-4abc-8def-1234567890ab"));
}

/// @brief Registry公開後に残ったInstall JournalをRegistry Recovery後も完了できることを検証する
void test_registry_recovery_resumes_published_install(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"Bundle";
    const std::filesystem::path installRoot = temporary.path() / L"Install";
    create_bundle(bundleRoot, a_assertContext);
    cue::distribution::WindowsInstallRequest request{utf8_path(bundleRoot), utf8_path(installRoot), false, true};
    auto installed = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(installed.has_value());
    cue::distribution::InstalledVersionsRegistry registry = read_registry(installRoot, a_assertContext);
    require(registry.versions.size() == 1U);

    cue::distribution::InstallOperationJournal journal;
    journal.operationId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    journal.kind = cue::distribution::InstallOperationKind::Install;
    journal.stage = cue::distribution::InstallOperationStage::RegistryPublished;
    journal.workerId = installed.try_value()->workerId;
    journal.expectedRegistry =
        cue::distribution::ExpectedRegistry{registry.generationId, registry.revision - 1U};
    journal.target = cue::distribution::InstallOperationTarget{registry.versions.front().directoryName,
                                                               registry.versions.front().bundleId,
                                                               registry.versions.front().manifestDigest};
    auto journalBytes = cue::distribution::write_install_operation_journal(journal, a_assertContext);
    require(journalBytes.has_value());
    const std::filesystem::path journalPath =
        installRoot / L"Operations" / L"Journals" / L"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa.json";
    write_text(journalPath, *journalBytes.try_value());
    write_text(installRoot / L"State" / L"InstalledVersions.json", "");

    auto resumed = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(resumed.has_value() && resumed.try_value()->wasAlreadyInstalled);
    require(!std::filesystem::exists(journalPath));
    registry = read_registry(installRoot, a_assertContext);
    require(registry.revision == 1U && registry.versions.size() == 1U);
}

/// @brief Registry公開前のUpdate Journalを回復Generationへ調停して再開できることを検証する
void test_registry_recovery_reconciles_pending_update(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"Bundle";
    const std::filesystem::path updateBundleRoot = temporary.path() / L"UpdateBundle";
    const std::filesystem::path installRoot = temporary.path() / L"Install";
    create_bundle(bundleRoot, a_assertContext);
    create_bundle(updateBundleRoot, a_assertContext, installer_executable(), 0U,
                  "22345678-1234-4abc-8def-1234567890ab", "1.1.0", std::byte{'u'});
    cue::distribution::WindowsInstallRequest request{utf8_path(bundleRoot), utf8_path(installRoot), false, true};
    cue::distribution::WindowsInstallRequest updateRequest{utf8_path(updateBundleRoot), utf8_path(installRoot), true,
                                                           true};
    require(cue::distribution::install_windows_source_sdk(request, a_assertContext).has_value());
    auto updated = cue::distribution::install_windows_source_sdk(updateRequest, a_assertContext);
    require(updated.has_value());
    cue::distribution::InstalledVersionsRegistry complete = read_registry(installRoot, a_assertContext);
    require(complete.versions.size() == 2U);
    const auto updateEntry = std::ranges::find_if(
        complete.versions, [](const cue::distribution::InstalledVersionEntry &a_entry) noexcept
        { return a_entry.engineVersion == "1.1.0"; });
    require(updateEntry != complete.versions.end());

    cue::distribution::InstalledVersionsRegistry beforeUpdate = complete;
    std::erase_if(beforeUpdate.versions, [](const cue::distribution::InstalledVersionEntry &a_entry) noexcept
                  { return a_entry.engineVersion == "1.1.0"; });
    beforeUpdate.selectedVersion = beforeUpdate.versions.front().directoryName;
    beforeUpdate.revision = 2U;
    auto beforeUpdateBytes =
        cue::distribution::write_installed_versions_registry(beforeUpdate, a_assertContext);
    require(beforeUpdateBytes.has_value());
    write_text(installRoot / L"State" / L"InstalledVersions.json", *beforeUpdateBytes.try_value());

    cue::distribution::InstallOperationJournal journal;
    journal.operationId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    journal.kind = cue::distribution::InstallOperationKind::Update;
    journal.stage = cue::distribution::InstallOperationStage::WorkerPublished;
    journal.workerId = updated.try_value()->workerId;
    journal.expectedRegistry =
        cue::distribution::ExpectedRegistry{beforeUpdate.generationId, beforeUpdate.revision};
    journal.target = cue::distribution::InstallOperationTarget{updateEntry->directoryName, updateEntry->bundleId,
                                                               updateEntry->manifestDigest};
    auto journalBytes = cue::distribution::write_install_operation_journal(journal, a_assertContext);
    require(journalBytes.has_value());
    const std::filesystem::path journalPath =
        installRoot / L"Operations" / L"Journals" / L"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb.json";
    write_text(journalPath, *journalBytes.try_value());
    write_text(installRoot / L"State" / L"InstalledVersions.json", "");

    auto resumed = cue::distribution::install_windows_source_sdk(updateRequest, a_assertContext);
    require(resumed.has_value() && !resumed.try_value()->wasAlreadyInstalled);
    require(!std::filesystem::exists(journalPath));
    const cue::distribution::InstalledVersionsRegistry recovered = read_registry(installRoot, a_assertContext);
    require(recovered.revision == 2U && recovered.versions.size() == 2U);
}

/// @brief 複数の未完了Journalを変更せず明示修復要求として拒否することを検証する
void test_registry_recovery_rejects_multiple_pending_operations(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"Bundle";
    const std::filesystem::path updateBundleRoot = temporary.path() / L"UpdateBundle";
    const std::filesystem::path installRoot = temporary.path() / L"Install";
    create_bundle(bundleRoot, a_assertContext);
    create_bundle(updateBundleRoot, a_assertContext, installer_executable(), 0U,
                  "22345678-1234-4abc-8def-1234567890ab", "1.1.0", std::byte{'u'});
    cue::distribution::WindowsInstallRequest request{utf8_path(bundleRoot), utf8_path(installRoot), false, true};
    cue::distribution::WindowsInstallRequest updateRequest{utf8_path(updateBundleRoot), utf8_path(installRoot), true,
                                                           true};
    require(cue::distribution::install_windows_source_sdk(request, a_assertContext).has_value());
    auto updated = cue::distribution::install_windows_source_sdk(updateRequest, a_assertContext);
    require(updated.has_value());
    const cue::distribution::InstalledVersionsRegistry registry = read_registry(installRoot, a_assertContext);
    require(registry.versions.size() == 2U);

    constexpr std::array operationIds = {"cccccccc-cccc-4ccc-8ccc-cccccccccccc",
                                         "dddddddd-dddd-4ddd-8ddd-dddddddddddd"};
    std::array<std::filesystem::path, 2U> journalPaths;
    std::array<std::vector<std::byte>, 2U> journalBytesBefore;
    for (std::size_t index = 0U; index < operationIds.size(); ++index)
    {
        cue::distribution::InstallOperationJournal journal;
        journal.operationId = operationIds[index];
        journal.kind = index == 0U ? cue::distribution::InstallOperationKind::Install
                                  : cue::distribution::InstallOperationKind::Update;
        journal.stage = cue::distribution::InstallOperationStage::Prepared;
        journal.workerId = updated.try_value()->workerId;
        journal.expectedRegistry = cue::distribution::ExpectedRegistry{registry.generationId, registry.revision};
        journal.target = cue::distribution::InstallOperationTarget{registry.versions[index].directoryName,
                                                                   registry.versions[index].bundleId,
                                                                   registry.versions[index].manifestDigest};
        auto journalBytes = cue::distribution::write_install_operation_journal(journal, a_assertContext);
        require(journalBytes.has_value());
        journalPaths[index] = installRoot / L"Operations" / L"Journals" /
                              (std::filesystem::path(operationIds[index]).native() + L".json");
        write_text(journalPaths[index], *journalBytes.try_value());
        journalBytesBefore[index] = read_bytes(journalPaths[index]);
    }
    const std::filesystem::path registryPath = installRoot / L"State" / L"InstalledVersions.json";
    const std::filesystem::path journalsRoot = installRoot / L"Operations" / L"Journals";
    const std::filesystem::path evidenceRoot = installRoot / L"Operations" / L"Evidence";
    write_text(registryPath, "");
    const std::vector<std::byte> registryBytesBefore = read_bytes(registryPath);
    const auto journalCountBefore = std::distance(std::filesystem::directory_iterator(journalsRoot),
                                                  std::filesystem::directory_iterator{});
    const auto evidenceCountBefore = std::distance(std::filesystem::directory_iterator(evidenceRoot),
                                                   std::filesystem::directory_iterator{});

    auto blocked = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(!blocked);
    require(blocked.try_error()->root_code().value() ==
            static_cast<std::int64_t>(cue::distribution::DistributionError::InstallRecoveryBlocked));
    for (std::size_t index = 0U; index < journalPaths.size(); ++index)
    {
        require(read_bytes(journalPaths[index]) == journalBytesBefore[index]);
    }
    require(read_bytes(registryPath) == registryBytesBefore);
    require(std::distance(std::filesystem::directory_iterator(journalsRoot),
                          std::filesystem::directory_iterator{}) == journalCountBefore);
    require(std::distance(std::filesystem::directory_iterator(evidenceRoot),
                          std::filesystem::directory_iterator{}) == evidenceCountBefore);
}

/// @brief Read-only属性を持つPayloadとWorkerを属性保持したままInstallできることを検証する
void test_read_only_payload_install(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"Bundle";
    const std::filesystem::path installRoot = temporary.path() / L"Install";
    create_bundle(bundleRoot, a_assertContext);
    const std::filesystem::path sourcePayload = bundleRoot / L"Engine" / L"Source" / L"Foundation" / L"Test.cpp";
    const std::filesystem::path sourceWorker = bundleRoot / L"Bin" / L"CueEngineInstallWorker.exe";
    require(SetFileAttributesW(sourcePayload.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    require(SetFileAttributesW(sourceWorker.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);

    cue::distribution::WindowsInstallRequest request{utf8_path(bundleRoot), utf8_path(installRoot), false, true};
    auto installed = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(installed.has_value());
    const std::filesystem::path versionRoot =
        installRoot / L"Versions" / std::filesystem::path(installed.try_value()->versionDirectory);
    const std::filesystem::path installedPayload =
        versionRoot / L"Engine" / L"Source" / L"Foundation" / L"Test.cpp";
    const std::filesystem::path versionWorker = versionRoot / L"Bin" / L"CueEngineInstallWorker.exe";
    const std::filesystem::path publishedWorker = installRoot / L"Operations" / L"Workers" /
                                                  std::filesystem::path(installed.try_value()->workerId) /
                                                  L"CueEngineInstallWorker.exe";
    require((GetFileAttributesW(installedPayload.c_str()) & FILE_ATTRIBUTE_READONLY) != 0U);
    require((GetFileAttributesW(publishedWorker.c_str()) & FILE_ATTRIBUTE_READONLY) != 0U);

    for (const std::filesystem::path &path :
         {sourcePayload, sourceWorker, installedPayload, versionWorker, publishedWorker})
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES);
        require(SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) != FALSE);
    }
}

/// @brief Install Worker EvidenceがReparse Pointへ置換された場合に再検証を拒否する
void test_worker_reparse_rejected(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"Bundle";
    const std::filesystem::path installRoot = temporary.path() / L"Install";
    const std::filesystem::path outsideWorker = temporary.path() / L"OutsideWorker.exe";
    create_bundle(bundleRoot, a_assertContext);
    cue::distribution::WindowsInstallRequest request{utf8_path(bundleRoot), utf8_path(installRoot), false, true};
    auto installed = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(installed.has_value());
    const std::filesystem::path worker = installRoot / L"Operations" / L"Workers" /
                                         std::filesystem::path(installed.try_value()->workerId) /
                                         L"CueEngineInstallWorker.exe";
    require(CopyFileW(worker.c_str(), outsideWorker.c_str(), TRUE) != FALSE);
    require(DeleteFileW(worker.c_str()) != FALSE);
    if (CreateSymbolicLinkW(worker.c_str(), outsideWorker.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE)
    {
        const DWORD linkError = GetLastError();
        require(linkError == ERROR_PRIVILEGE_NOT_HELD || linkError == ERROR_INVALID_PARAMETER);
        return;
    }
    require(!cue::distribution::install_windows_source_sdk(request, a_assertContext));
}

/// @brief Probe入口が検証済みControl Leaseの子Process再継承を無効化することを検証する
void test_probe_clears_lease_inheritance(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path installRoot = temporary.path() / L"Install";
    const std::filesystem::path operations = installRoot / L"Operations";
    std::error_code error;
    std::filesystem::create_directories(operations, error);
    require(!error);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE lease = CreateFileW((operations / L"CueEngine.control.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0U,
                               &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(lease != INVALID_HANDLE_VALUE);
    DWORD flags = 0U;
    require(GetHandleInformation(lease, &flags) != FALSE && (flags & HANDLE_FLAG_INHERIT) != 0U);
    cue::distribution::WindowsInstallProbeRequest request{
        reinterpret_cast<std::uintptr_t>(lease), utf8_path(installRoot), "v1.0.0--12345678-1234-4abc-8def-1234567890ab",
        "66666666-6666-4666-8666-666666666666",  std::string(64U, 'a'),
    };
    require(!cue::distribution::run_windows_install_probe(request, a_assertContext));
    require(GetHandleInformation(lease, &flags) != FALSE && (flags & HANDLE_FLAG_INHERIT) == 0U);
    static_cast<void>(CloseHandle(lease));
}

/// @brief Bundle／Install Rootの既存祖先にReparse Pointがある場合は変更前に拒否することを検証する
void test_reparse_ancestor_rejected(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path directBundleRoot = temporary.path() / L"Bundle";
    const std::filesystem::path targetRoot = temporary.path() / L"Target";
    const std::filesystem::path linkRoot = temporary.path() / L"Link";
    create_bundle(directBundleRoot, a_assertContext);
    std::error_code error;
    std::filesystem::create_directories(targetRoot, error);
    require(!error);
    if (CreateSymbolicLinkW(linkRoot.c_str(), targetRoot.c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE)
    {
        const DWORD linkError = GetLastError();
        require(linkError == ERROR_PRIVILEGE_NOT_HELD || linkError == ERROR_INVALID_PARAMETER);
        return;
    }
    const std::filesystem::path installRoot = linkRoot / L"Install";
    cue::distribution::WindowsInstallRequest installRequest{utf8_path(directBundleRoot), utf8_path(installRoot), false,
                                                            true};
    require(!cue::distribution::install_windows_source_sdk(installRequest, a_assertContext));
    require(!std::filesystem::exists(targetRoot / L"Install"));

    const std::filesystem::path targetBundleRoot = targetRoot / L"Bundle";
    const std::filesystem::path linkedBundleRoot = linkRoot / L"Bundle";
    const std::filesystem::path directInstallRoot = temporary.path() / L"BundleAncestorInstall";
    create_bundle(targetBundleRoot, a_assertContext);
    cue::distribution::WindowsInstallRequest bundleRequest{utf8_path(linkedBundleRoot), utf8_path(directInstallRoot),
                                                           false, true};
    require(!cue::distribution::install_windows_source_sdk(bundleRequest, a_assertContext));
    require(!std::filesystem::exists(directInstallRoot));
}

/// @brief Probe MarkerのAtomic一時Fileが残ったVersionPublished Journalを再開できることを検証する
void test_probe_marker_temporary_resume(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"Bundle";
    const std::filesystem::path installRoot = temporary.path() / L"Install";
    create_bundle(bundleRoot, a_assertContext);
    cue::distribution::WindowsInstallRequest request{utf8_path(bundleRoot), utf8_path(installRoot), false, true};
    auto installed = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(installed.has_value());
    cue::distribution::InstalledVersionsRegistry registry = read_registry(installRoot, a_assertContext);
    require(registry.versions.size() == 1U);
    const cue::distribution::InstalledVersionEntry entry = registry.versions.front();

    constexpr std::string_view operationId = "88888888-8888-4888-8888-888888888888";
    cue::distribution::InstallOperationJournal journal;
    journal.operationId = operationId;
    journal.kind = cue::distribution::InstallOperationKind::Install;
    journal.stage = cue::distribution::InstallOperationStage::VersionPublished;
    journal.workerId = installed.try_value()->workerId;
    journal.expectedRegistry = cue::distribution::ExpectedRegistry{registry.generationId, 1U};
    journal.target =
        cue::distribution::InstallOperationTarget{entry.directoryName, entry.bundleId, entry.manifestDigest};
    auto journalBytes = cue::distribution::write_install_operation_journal(journal, a_assertContext);
    require(journalBytes.has_value());
    const std::filesystem::path journalPath =
        installRoot / L"Operations" / L"Journals" / L"88888888-8888-4888-8888-888888888888.json";
    write_text(journalPath, *journalBytes.try_value());

    registry.versions.clear();
    registry.selectedVersion.clear();
    registry.revision = 1U;
    auto registryBytes = cue::distribution::write_installed_versions_registry(registry, a_assertContext);
    require(registryBytes.has_value());
    write_text(installRoot / L"State" / L"InstalledVersions.json", *registryBytes.try_value());

    const std::filesystem::path versionRoot = installRoot / L"Versions" / std::filesystem::path(entry.directoryName);
    require(DeleteFileW((versionRoot / L"CueEngineProbe.complete.json").c_str()) != FALSE);
    const std::filesystem::path temporaryMarker =
        versionRoot / L"CueEngineProbe.complete.json.tmp-99999999-9999-4999-8999-999999999999";
    write_text(temporaryMarker, "{\"partial\":");

    auto resumed = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(resumed.has_value());
    require(!std::filesystem::exists(temporaryMarker));
    require(std::filesystem::is_regular_file(versionRoot / L"CueEngineProbe.complete.json"));
    require(!std::filesystem::exists(journalPath));
}

/// @brief Probe失敗VersionのQuarantine直後に停止しても決定的PathからJournalを解消できることを検証する
void test_probe_quarantine_crash_resume(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"Bundle";
    const std::filesystem::path installRoot = temporary.path() / L"Install";
    create_bundle(bundleRoot, a_assertContext);
    cue::distribution::WindowsInstallRequest request{utf8_path(bundleRoot), utf8_path(installRoot), false, true};
    auto installed = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(installed.has_value());
    cue::distribution::InstalledVersionsRegistry registry = read_registry(installRoot, a_assertContext);
    require(registry.versions.size() == 1U);
    const cue::distribution::InstalledVersionEntry entry = registry.versions.front();

    constexpr std::string_view operationId = "77777777-7777-4777-8777-777777777777";
    cue::distribution::InstallOperationJournal journal;
    journal.operationId = operationId;
    journal.kind = cue::distribution::InstallOperationKind::Install;
    journal.stage = cue::distribution::InstallOperationStage::VersionPublished;
    journal.workerId = installed.try_value()->workerId;
    journal.expectedRegistry = cue::distribution::ExpectedRegistry{registry.generationId, 1U};
    journal.target =
        cue::distribution::InstallOperationTarget{entry.directoryName, entry.bundleId, entry.manifestDigest};
    auto journalBytes = cue::distribution::write_install_operation_journal(journal, a_assertContext);
    require(journalBytes.has_value());
    const std::filesystem::path journalPath =
        installRoot / L"Operations" / L"Journals" / L"77777777-7777-4777-8777-777777777777.json";
    write_text(journalPath, *journalBytes.try_value());

    registry.versions.clear();
    registry.selectedVersion.clear();
    registry.revision = 1U;
    auto registryBytes = cue::distribution::write_installed_versions_registry(registry, a_assertContext);
    require(registryBytes.has_value());
    write_text(installRoot / L"State" / L"InstalledVersions.json", *registryBytes.try_value());

    const std::filesystem::path versionRoot = installRoot / L"Versions" / std::filesystem::path(entry.directoryName);
    const std::filesystem::path quarantine =
        installRoot / L"Operations" / L"Quarantine" /
        (std::filesystem::path(entry.directoryName).native() + L"-77777777-7777-4777-8777-777777777777");
    require(MoveFileExW(versionRoot.c_str(), quarantine.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE);
    const std::filesystem::path quarantinedSource = quarantine / L"Engine" / L"Source" / L"Foundation" / L"Test.cpp";
    write_text(quarantinedSource, "bad\n");
    require(!cue::distribution::install_windows_source_sdk(request, a_assertContext));
    require(std::filesystem::exists(journalPath));
    require(CopyFileW((bundleRoot / L"Engine" / L"Source" / L"Foundation" / L"Test.cpp").c_str(),
                      quarantinedSource.c_str(), FALSE) != FALSE);
    require(!cue::distribution::install_windows_source_sdk(request, a_assertContext));
    require(!std::filesystem::exists(journalPath));
    require(std::filesystem::is_directory(quarantine));

    auto retried = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    require(retried.has_value());
    registry = read_registry(installRoot, a_assertContext);
    require(registry.versions.size() == 1U && registry.revision == 2U);
}

/// @brief 破損Journalを隔離し明示修復までRegistry Recoveryを継続しないことを検証する
void test_corrupt_journal_quarantine(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"Bundle";
    const std::filesystem::path installRoot = temporary.path() / L"Install";
    create_bundle(bundleRoot, a_assertContext);
    cue::distribution::WindowsInstallRequest request{utf8_path(bundleRoot), utf8_path(installRoot), false, true};
    require(cue::distribution::install_windows_source_sdk(request, a_assertContext).has_value());
    const std::filesystem::path registryPath = installRoot / L"State" / L"InstalledVersions.json";
    const std::vector<std::byte> registryBefore = read_bytes(registryPath);

    const std::filesystem::path corruptJournal =
        installRoot / L"Operations" / L"Journals" / L"44444444-4444-4444-8444-444444444444.json";
    write_text(corruptJournal, "{\"schemaVersion\":2}\n");
    require(!cue::distribution::install_windows_source_sdk(request, a_assertContext));
    require(!std::filesystem::exists(corruptJournal));
    const std::filesystem::path quarantineRoot = installRoot / L"Operations" / L"Quarantine" / L"Journals";
    require(std::distance(std::filesystem::directory_iterator(quarantineRoot), std::filesystem::directory_iterator{}) ==
            1);
    require(read_bytes(registryPath) == registryBefore);

    require(!cue::distribution::install_windows_source_sdk(request, a_assertContext));
    require(read_bytes(registryPath) == registryBefore);
}

/// @brief Parent Crash後もProbe継承Handleが排他Leaseを保持しJournal再開できることを検証する
void test_probe_inherited_lease(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"ProbeBundle";
    const std::filesystem::path installRoot = temporary.path() / L"ProbeInstall";
    create_bundle(bundleRoot, a_assertContext, probe_helper_executable());
    const std::filesystem::path operations = installRoot / L"Operations";
    const std::filesystem::path enabled = operations / L"TestProbeGate.enabled";
    const std::filesystem::path ready = operations / L"TestProbeGate.ready";
    const std::filesystem::path release = operations / L"TestProbeGate.release";
    write_text(enabled, "enabled\n");

    auto owner = start_installer_process(bundleRoot, installRoot);
    require(owner.has_value());
    auto probeId = wait_for_process_id(ready, std::chrono::seconds(60));
    require(probeId.has_value());
    HANDLE probeProcess = OpenProcess(SYNCHRONIZE, FALSE, *probeId);
    require(probeProcess != nullptr);

    auto contender = start_installer_process(bundleRoot, installRoot);
    require(contender.has_value());
    auto contenderExit = wait_process(*contender, 60000U);
    require(contenderExit.has_value() && *contenderExit != 0U);

    require(TerminateProcess(owner->process(), 91U) != FALSE);
    require(wait_process(*owner, 10000U).has_value());
    auto inheritedContender = start_installer_process(bundleRoot, installRoot);
    require(inheritedContender.has_value());
    auto inheritedExit = wait_process(*inheritedContender, 60000U);
    require(inheritedExit.has_value() && *inheritedExit != 0U);

    write_text(release, "release\n");
    require(WaitForSingleObject(probeProcess, 10000U) == WAIT_OBJECT_0);
    static_cast<void>(CloseHandle(probeProcess));
    std::error_code error;
    std::filesystem::remove(enabled, error);
    error.clear();
    std::filesystem::remove(ready, error);
    error.clear();
    std::filesystem::remove(release, error);

    auto resumed = start_installer_process(bundleRoot, installRoot);
    require(resumed.has_value());
    auto resumedExit = wait_process(*resumed, 60000U);
    require(resumedExit.has_value() && *resumedExit == 0U);
    const cue::distribution::InstalledVersionsRegistry registry = read_registry(installRoot, a_assertContext);
    require(registry.versions.size() == 1U && registry.revision == 2U);
}

/// @brief Worker公開直後のProcess CrashからJournalどおり再開できることを検証する
void test_worker_publish_crash_resume(const cue::AssertContext &a_assertContext)
{
    TemporaryRoot temporary;
    const std::filesystem::path bundleRoot = temporary.path() / L"WorkerBundle";
    const std::filesystem::path installRoot = temporary.path() / L"WorkerInstall";
    create_bundle(bundleRoot, a_assertContext, installer_executable(), 32U * 1024U * 1024U);
    auto installer = start_installer_process(bundleRoot, installRoot);
    require(installer.has_value());
    const std::filesystem::path workers = installRoot / L"Operations" / L"Workers";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    bool workerPublished = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::error_code error;
        const std::filesystem::directory_iterator iterator(workers, error);
        workerPublished = !error && iterator != std::filesystem::directory_iterator{};
        if (workerPublished)
        {
            break;
        }
        require(WaitForSingleObject(installer->process(), 0U) == WAIT_TIMEOUT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(workerPublished);
    require(WaitForSingleObject(installer->process(), 0U) == WAIT_TIMEOUT);
    require(TerminateProcess(installer->process(), 92U) != FALSE);
    require(wait_process(*installer, 10000U).has_value());

    const std::filesystem::path journals = installRoot / L"Operations" / L"Journals";
    std::error_code error;
    std::filesystem::directory_iterator iterator(journals, error);
    require(!error && iterator != std::filesystem::directory_iterator{});
    const std::vector<std::byte> journalBytes = read_bytes(iterator->path());
    const std::string journalText(reinterpret_cast<const char *>(journalBytes.data()), journalBytes.size());
    auto journal = cue::distribution::read_install_operation_journal(journalText, a_assertContext);
    require(journal.has_value());
    require(journal.try_value()->stage == cue::distribution::InstallOperationStage::ProbeSucceeded ||
            journal.try_value()->stage == cue::distribution::InstallOperationStage::WorkerPublished);

    cue::distribution::WindowsInstallRequest request{utf8_path(bundleRoot), utf8_path(installRoot), false, true};
    auto resumed = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    if (!resumed)
    {
        std::fprintf(stderr, "Worker crash resume failed: %.*s\n",
                     static_cast<int>(resumed.try_error()->summary().size()), resumed.try_error()->summary().data());
    }
    require(resumed.has_value());
    const cue::distribution::InstalledVersionsRegistry registry = read_registry(installRoot, a_assertContext);
    require(registry.versions.size() == 1U && registry.revision == 2U);
    require(std::filesystem::directory_iterator(journals) == std::filesystem::directory_iterator{});
}
} // namespace

int main(int a_argumentCount, char **a_arguments)
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    if (a_argumentCount == 2 && std::string_view(a_arguments[1]) == "--registry-recovery")
    {
        test_registry_recovery_resumes_published_install(assertContext);
        test_registry_recovery_reconciles_pending_update(assertContext);
        test_registry_recovery_rejects_multiple_pending_operations(assertContext);
        return 0;
    }
    if (a_argumentCount != 1)
    {
        return 2;
    }
    test_install_transaction(assertContext);
    test_read_only_payload_install(assertContext);
    test_worker_reparse_rejected(assertContext);
    test_probe_clears_lease_inheritance(assertContext);
    test_reparse_ancestor_rejected(assertContext);
    test_probe_marker_temporary_resume(assertContext);
    test_probe_quarantine_crash_resume(assertContext);
    test_corrupt_journal_quarantine(assertContext);
    test_probe_inherited_lease(assertContext);
    test_worker_publish_crash_resume(assertContext);
    return 0;
}
