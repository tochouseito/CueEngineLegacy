#include <Cue/Distribution/InstallState.h>

#include <Cue/Distribution/Error.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Testを継続不能な失敗として停止する
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }

    /// @brief 診断付きTest失敗を継続不能として停止する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

/// @brief Test前提を満たさない位置を表示して停止する
void require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::fprintf(stderr, "Requirement failed at %s:%u\n", a_location.file_name(), a_location.line());
        std::abort();
    }
}

/// @brief Test用のCanonical SHA-256文字列を生成する
[[nodiscard]] std::string hash(char a_value)
{
    return std::string(64U, a_value);
}

/// @brief Test用Publisher Build Identityを構築する
[[nodiscard]] cue::distribution::PublisherBuildIdentity make_publisher_identity()
{
    return {
        std::string(40U, 'b'),
        "x64-windows",
        cue::distribution::DistributionArchitecture::X64,
        cue::distribution::DistributionArchitecture::X64,
        "msvc",
        "19.44.35221",
        "14.44.35221",
        "dynamic",
        "14.44.35221",
        "10.0.26100.0",
        "Release",
    };
}

/// @brief Worker ID導出に必要な最小の有効Manifestを構築する
[[nodiscard]] cue::distribution::DistributionManifest make_manifest()
{
    using cue::distribution::DistributionFileEntry;
    using cue::distribution::DistributionFileRole;
    cue::distribution::DistributionManifest manifest;
    manifest.bundleId = "12345678-1234-4abc-8def-1234567890ab";
    manifest.engineVersion = "1.2.3";
    manifest.engineSourceRevision = std::string(40U, 'b');
    manifest.sourceInventoryHash = hash('c');
    manifest.dependencyDefinitionId = hash('d');
    manifest.publisherBuildIdentity = make_publisher_identity();
    manifest.minimumToolchain = {"4.2.0", "2.44.0", "msvc", "19.44.35221", "10.0.26100.0"};
    manifest.entryPoints = {
        "Bin/CueEngineBootstrap.exe", "Bin/CueProjectHubTool.exe",      "Bin/CueEditorTool.exe",
        "Bin/CueRuntimeHost.exe",     "Bin/CueEngineInstallerTool.exe", "Bin/CueEngineInstallWorker.exe",
    };
    manifest.files = {
        DistributionFileEntry{DistributionFileRole::EngineSource, "Engine/Source/Foundation/Foo.cpp", 0U, hash('1')},
        DistributionFileEntry{DistributionFileRole::Hlsl, "Engine/Source/Renderer/Shaders/Foo.hlsl", 0U, hash('2')},
        DistributionFileEntry{DistributionFileRole::CMake, "CMakeLists.txt", 0U, hash('3')},
        DistributionFileEntry{DistributionFileRole::Script, "Tools/Dependencies/RestoreVcpkg.ps1", 0U, hash('4')},
        DistributionFileEntry{DistributionFileRole::Document, "Engine/Documents/CODING_RULES.md", 0U, hash('5')},
        DistributionFileEntry{DistributionFileRole::License, "LICENSE.txt", 0U, hash('6')},
        DistributionFileEntry{DistributionFileRole::DependencyDefinition, "ThirdParty/vcpkg.json", 0U, hash('7')},
        DistributionFileEntry{DistributionFileRole::DependencyDefinition, "ThirdParty/vcpkg-configuration.json", 0U,
                              hash('8')},
        DistributionFileEntry{DistributionFileRole::DependencyDefinition, "ThirdParty/vcpkg-tool.json", 0U, hash('9')},
        DistributionFileEntry{DistributionFileRole::ThirdPartyNotice, "ThirdParty/THIRD_PARTY_NOTICES.md", 0U,
                              hash('a')},
        DistributionFileEntry{DistributionFileRole::ThirdPartyLicense, "ThirdParty/Licenses/DearImGui-LICENSE.txt", 0U,
                              hash('b')},
        DistributionFileEntry{DistributionFileRole::ThirdPartyLicense, "ThirdParty/Licenses/vcpkg-LICENSE.txt", 0U,
                              hash('c')},
        DistributionFileEntry{DistributionFileRole::Bootstrap, manifest.entryPoints.bootstrap, 10U, hash('d')},
        DistributionFileEntry{DistributionFileRole::ProjectHub, manifest.entryPoints.projectHub, 10U, hash('e')},
        DistributionFileEntry{DistributionFileRole::Editor, manifest.entryPoints.editor, 10U, hash('f')},
        DistributionFileEntry{DistributionFileRole::RuntimeHost, manifest.entryPoints.runtimeHost, 10U, hash('0')},
        DistributionFileEntry{DistributionFileRole::Installer, manifest.entryPoints.installer, 10U, hash('1')},
        DistributionFileEntry{DistributionFileRole::InstallWorker, manifest.entryPoints.installWorker, 10U, hash('2')},
    };
    return manifest;
}

/// @brief Test用Installed Version Entryを構築する
[[nodiscard]] cue::distribution::InstalledVersionEntry make_version_entry()
{
    return {
        "v1.2.3--12345678-1234-4abc-8def-1234567890ab",
        "1.2.3",
        "12345678-1234-4abc-8def-1234567890ab",
        hash('1'),
        hash('2'),
        hash('3'),
        hash('4'),
        hash('5'),
        hash('6'),
        cue::distribution::InstalledVersionState::Selectable,
    };
}

/// @brief Registry v1のRound-tripと空初期Registryを検証する
[[nodiscard]] bool test_registry_round_trip(const cue::AssertContext &a_assertContext)
{
    using namespace cue::distribution;
    InstalledVersionsRegistry empty{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", 1U, {}, {}};
    auto emptyBytes = write_installed_versions_registry(empty, a_assertContext);
    require(emptyBytes.has_value());
    auto emptyRead = read_installed_versions_registry(*emptyBytes.try_value(), a_assertContext);
    require(emptyRead.has_value() && *emptyRead.try_value() == empty);

    InstalledVersionEntry version = make_version_entry();
    InstalledVersionsRegistry registry{"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", 2U, version.directoryName, {version}};
    auto bytes = write_installed_versions_registry(registry, a_assertContext);
    require(bytes.has_value() && bytes.try_value()->ends_with('\n'));
    auto read = read_installed_versions_registry(*bytes.try_value(), a_assertContext);
    return read.has_value() && *read.try_value() == registry;
}

/// @brief Registry Readerが未知Member、Whitespace、対応外Schemaを拒否することを検証する
[[nodiscard]] bool test_registry_fail_closed(const cue::AssertContext &a_assertContext)
{
    using namespace cue::distribution;
    InstalledVersionsRegistry registry{"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", 2U, {}, {make_version_entry()}};
    auto bytes = write_installed_versions_registry(registry, a_assertContext);
    require(bytes.has_value());
    std::string whitespace = *bytes.try_value();
    whitespace.insert(1U, " ");
    require(!read_installed_versions_registry(whitespace, a_assertContext));
    std::string unknown = *bytes.try_value();
    unknown.insert(unknown.find(",\"versions\""), ",\"unknown\":1");
    require(!read_installed_versions_registry(unknown, a_assertContext));
    std::string schema = *bytes.try_value();
    schema.replace(schema.find("\"schemaVersion\":1"), 17U, "\"schemaVersion\":2");
    auto unsupported = read_installed_versions_registry(schema, a_assertContext);
    require(!unsupported);
    require(unsupported.try_error()->root_code().value() ==
            static_cast<std::int64_t>(DistributionError::UnsupportedInstallSchema));
    registry.versions.front().directoryName = "../outside";
    require(!write_installed_versions_registry(registry, a_assertContext));
    registry = InstalledVersionsRegistry{
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", 2U, make_version_entry().directoryName, {make_version_entry()}};
    registry.versions.front().state = InstalledVersionState::PendingRemoval;
    require(!write_installed_versions_registry(registry, a_assertContext));
    registry.selectedVersion.clear();
    registry.versions.front().state = static_cast<InstalledVersionState>(255U);
    return !write_installed_versions_registry(registry, a_assertContext);
}

/// @brief 全Operation Kindのv1 Stage列挙と隣接遷移だけが許可されることを検証する
[[nodiscard]] bool test_stage_contract() noexcept
{
    using enum cue::distribution::InstallOperationKind;
    using enum cue::distribution::InstallOperationStage;
    using cue::distribution::is_valid_install_stage_transition;
    require(is_valid_install_stage_transition(Install, Prepared, PayloadStaged));
    require(is_valid_install_stage_transition(Update, ProbeSucceeded, WorkerPublished));
    require(is_valid_install_stage_transition(Rollback, Prepared, SelectionPublished));
    require(is_valid_install_stage_transition(Uninstall, RemovalBlocked, VersionQuarantined));
    require(is_valid_install_stage_transition(RegistryRecovery, CandidatesValidated, RegistryPublished));
    require(!is_valid_install_stage_transition(Install, Prepared, VersionPublished));
    require(!is_valid_install_stage_transition(Update, RegistryPublished, Prepared));
    require(!is_valid_install_stage_transition(Rollback, Prepared, RegistryPublished));
    require(!is_valid_install_stage_transition(Uninstall, Prepared, VersionQuarantined));
    return !is_valid_install_stage_transition(RegistryRecovery, Prepared, RegistryPublished);
}

/// @brief 通常操作JournalとRecovery JournalのCanonical Round-tripを検証する
[[nodiscard]] bool test_journal_round_trip(const cue::AssertContext &a_assertContext)
{
    using namespace cue::distribution;
    InstallOperationJournal install;
    install.operationId = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
    install.kind = InstallOperationKind::Install;
    install.stage = InstallOperationStage::Prepared;
    install.workerId = hash('1');
    install.expectedRegistry = ExpectedRegistry{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", 1U};
    install.target =
        InstallOperationTarget{make_version_entry().directoryName, "12345678-1234-4abc-8def-1234567890ab", hash('2')};
    auto installBytes = write_install_operation_journal(install, a_assertContext);
    require(installBytes.has_value());
    auto installRead = read_install_operation_journal(*installBytes.try_value(), a_assertContext);
    require(installRead.has_value() && *installRead.try_value() == install);

    InstallOperationJournal uninstall = install;
    uninstall.kind = InstallOperationKind::Uninstall;
    uninstall.workerExecutableDigest = hash('7');
    uninstall.workerMarkerDigest = hash('8');
    auto uninstallBytes = write_install_operation_journal(uninstall, a_assertContext);
    require(uninstallBytes.has_value());
    auto uninstallRead = read_install_operation_journal(*uninstallBytes.try_value(), a_assertContext);
    require(uninstallRead.has_value() && *uninstallRead.try_value() == uninstall);
    uninstall.workerMarkerDigest.clear();
    require(!write_install_operation_journal(uninstall, a_assertContext));

    InstallOperationJournal recovery;
    recovery.operationId = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
    recovery.kind = InstallOperationKind::RegistryRecovery;
    recovery.stage = InstallOperationStage::CandidatesValidated;
    recovery.workerId = hash('3');
    recovery.sourceRegistryEvidence = RegistrySourceEvidence{};
    recovery.blockedOperations.push_back({"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", InstallOperationKind::Install,
                                          make_version_entry().directoryName, InstallOperationStage::VersionPublished,
                                          hash('4')});
    recovery.candidates.push_back({make_version_entry()});
    auto recoveryBytes = write_install_operation_journal(recovery, a_assertContext);
    require(recoveryBytes.has_value());
    auto recoveryRead = read_install_operation_journal(*recoveryBytes.try_value(), a_assertContext);
    require(recoveryRead.has_value() && *recoveryRead.try_value() == recovery);

    std::string unknown = *recoveryBytes.try_value();
    unknown.insert(unknown.find(",\"candidates\""), ",\"unknown\":false");
    require(!read_install_operation_journal(unknown, a_assertContext));
    recovery.expectedRegistry = ExpectedRegistry{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", 1U};
    require(!write_install_operation_journal(recovery, a_assertContext));
    recovery.expectedRegistry.reset();
    recovery.sourceRegistryEvidence = RegistrySourceEvidence{
        false, "InstalledVersions.corrupt-dddddddd-dddd-4ddd-8ddd-dddddddddddd.json", 0U, hash('5')};
    auto emptyEvidenceBytes = write_install_operation_journal(recovery, a_assertContext);
    require(emptyEvidenceBytes.has_value());
    auto emptyEvidenceRead = read_install_operation_journal(*emptyEvidenceBytes.try_value(), a_assertContext);
    require(emptyEvidenceRead.has_value() && *emptyEvidenceRead.try_value() == recovery);
    recovery.sourceRegistryEvidence = RegistrySourceEvidence{
        false, "../InstalledVersions.corrupt-dddddddd-dddd-4ddd-8ddd-dddddddddddd.json", 10U, hash('6')};
    return !write_install_operation_journal(recovery, a_assertContext);
}

/// @brief Payload、Probe、Worker MarkerとWorker IDの決定性を検証する
[[nodiscard]] bool test_markers_and_worker_identity(const cue::AssertContext &a_assertContext)
{
    using namespace cue::distribution;
    const InstalledVersionEntry version = make_version_entry();
    PayloadCompleteMarker payload{version.directoryName, version.bundleId, version.manifestDigest};
    auto payloadBytes = write_payload_complete_marker(payload, a_assertContext);
    require(payloadBytes.has_value());
    auto payloadRead = read_payload_complete_marker(*payloadBytes.try_value(), a_assertContext);
    require(payloadRead.has_value() && *payloadRead.try_value() == payload);

    InstallProbeMarker probe{"cccccccc-cccc-4ccc-8ccc-cccccccccccc", version.directoryName, version.bundleId,
                             version.manifestDigest};
    auto probeBytes = write_install_probe_marker(probe, a_assertContext);
    require(probeBytes.has_value());
    auto probeRead = read_install_probe_marker(*probeBytes.try_value(), a_assertContext);
    require(probeRead.has_value() && *probeRead.try_value() == probe);

    DistributionManifest manifest = make_manifest();
    auto workerId = make_install_worker_id(manifest, a_assertContext);
    auto sameWorkerId = make_install_worker_id(manifest, a_assertContext);
    auto publisherDigest = make_publisher_build_identity_digest(manifest.publisherBuildIdentity, a_assertContext);
    require(workerId.has_value() && sameWorkerId.has_value() && publisherDigest.has_value());
    require(*workerId.try_value() == *sameWorkerId.try_value());
    const auto worker = manifest.files.back();
    InstallWorkerMarker workerMarker{*workerId.try_value(),
                                     manifest.bundleId,
                                     manifest.engineSourceRevision,
                                     *publisherDigest.try_value(),
                                     worker,
                                     DistributionArchitecture::X64};
    auto workerBytes = write_install_worker_marker(workerMarker, a_assertContext);
    require(workerBytes.has_value());
    auto workerRead = read_install_worker_marker(*workerBytes.try_value(), a_assertContext);
    require(workerRead.has_value() && *workerRead.try_value() == workerMarker);

    manifest.files.back().sha256 = hash('9');
    auto changedWorkerId = make_install_worker_id(manifest, a_assertContext);
    require(changedWorkerId.has_value() && *changedWorkerId.try_value() != *workerId.try_value());
    std::string unknown = *workerBytes.try_value();
    unknown.insert(unknown.find(",\"peArchitecture\""), ",\"unknown\":0");
    return !read_install_worker_marker(unknown, a_assertContext);
}
} // namespace

/// @brief Install State v1のCanonical Contract Testを実行する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    require(test_registry_round_trip(assertContext));
    require(test_registry_fail_closed(assertContext));
    require(test_stage_contract());
    require(test_journal_round_trip(assertContext));
    require(test_markers_and_worker_identity(assertContext));
    return 0;
}
