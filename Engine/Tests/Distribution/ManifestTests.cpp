#include <Cue/Distribution/Identity.h>
#include <Cue/Distribution/Manifest.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

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

void require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::fprintf(stderr, "Requirement failed at %s:%u\n", a_location.file_name(), a_location.line());
        std::abort();
    }
}

[[nodiscard]] std::string hash(char a_value)
{
    return std::string(64U, a_value);
}

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
        DistributionFileEntry{DistributionFileRole::ProjectHub, manifest.entryPoints.projectHub, 10U, hash('2')},
        DistributionFileEntry{DistributionFileRole::EngineSource, "Engine/Source/Foundation/Foo.cpp", 0U, hash('1')},
        DistributionFileEntry{DistributionFileRole::Bootstrap, manifest.entryPoints.bootstrap, 10U, hash('3')},
        DistributionFileEntry{DistributionFileRole::Editor, manifest.entryPoints.editor, 10U, hash('4')},
        DistributionFileEntry{DistributionFileRole::RuntimeHost, manifest.entryPoints.runtimeHost, 10U, hash('5')},
        DistributionFileEntry{DistributionFileRole::Installer, manifest.entryPoints.installer, 10U, hash('6')},
        DistributionFileEntry{DistributionFileRole::InstallWorker, manifest.entryPoints.installWorker, 10U, hash('7')},
    };
    return manifest;
}

[[nodiscard]] bool test_manifest_round_trip(const cue::AssertContext &a_assertContext)
{
    cue::distribution::DistributionManifest manifest = make_manifest();
    auto written = cue::distribution::write_distribution_manifest(manifest, a_assertContext);
    require(written.has_value());
    require(written.try_value()->ends_with("\n"));
    require(written.try_value()->find("Bin/CueProjectHubTool.exe") <
            written.try_value()->find("Engine/Source/Foundation/Foo.cpp"));

    auto read = cue::distribution::read_distribution_manifest(*written.try_value(), a_assertContext);
    require(read.has_value());
    auto rewritten = cue::distribution::write_distribution_manifest(*read.try_value(), a_assertContext);
    return rewritten.has_value() && *rewritten.try_value() == *written.try_value();
}

[[nodiscard]] bool test_noncanonical_and_unknown_rejected(const cue::AssertContext &a_assertContext)
{
    auto written = cue::distribution::write_distribution_manifest(make_manifest(), a_assertContext);
    require(written.has_value());

    std::string whitespace = *written.try_value();
    whitespace.insert(1U, " ");
    require(!cue::distribution::read_distribution_manifest(whitespace, a_assertContext));

    std::string unknown = *written.try_value();
    const std::size_t position = unknown.find(",\"files\":[");
    require(position != std::string::npos);
    unknown.insert(position, ",\"unknown\":1");
    require(!cue::distribution::read_distribution_manifest(unknown, a_assertContext));

    std::string unsupported = *written.try_value();
    unsupported.replace(unsupported.find("\"schemaVersion\":1"), 17U, "\"schemaVersion\":2");
    return !cue::distribution::read_distribution_manifest(unsupported, a_assertContext);
}

[[nodiscard]] bool test_manifest_identity_and_inventory_rejected(const cue::AssertContext &a_assertContext)
{
    cue::distribution::DistributionManifest manifest = make_manifest();
    manifest.engineVersion = "01.2.3";
    require(!cue::distribution::write_distribution_manifest(manifest, a_assertContext));

    manifest = make_manifest();
    manifest.bundleId = "12345678-1234-5abc-8def-1234567890ab";
    require(!cue::distribution::write_distribution_manifest(manifest, a_assertContext));

    manifest = make_manifest();
    manifest.files.push_back(manifest.files.front());
    require(!cue::distribution::write_distribution_manifest(manifest, a_assertContext));

    manifest = make_manifest();
    manifest.entryPoints.editor = "Bin/Other.exe";
    require(!cue::distribution::write_distribution_manifest(manifest, a_assertContext));

    manifest = make_manifest();
    manifest.files.front().relativePath = "../outside";
    require(!cue::distribution::write_distribution_manifest(manifest, a_assertContext));

    manifest = make_manifest();
    manifest.files.front().relativePath = "Bin/CON.exe";
    require(!cue::distribution::write_distribution_manifest(manifest, a_assertContext));

    manifest = make_manifest();
    manifest.files.push_back(
        {cue::distribution::DistributionFileRole::EngineSource, "engine/source/foundation/foo.cpp", 1U, hash('8')});
    return !cue::distribution::write_distribution_manifest(manifest, a_assertContext);
}

[[nodiscard]] bool test_identity_derivation(const cue::AssertContext &a_assertContext)
{
    auto definition =
        cue::distribution::make_dependency_definition_id("manifest\n", "configuration\n", "tool\n", a_assertContext);
    auto same =
        cue::distribution::make_dependency_definition_id("manifest\n", "configuration\n", "tool\n", a_assertContext);
    auto changed =
        cue::distribution::make_dependency_definition_id("manifest2\n", "configuration\n", "tool\n", a_assertContext);
    require(definition.has_value() && same.has_value() && changed.has_value());
    require(*definition.try_value() == "ba65ccf99c9f76a85b04158503f2a9764f12facf8d4d4b0244f150805ba1f4bd");
    require(*definition.try_value() == *same.try_value() && *definition.try_value() != *changed.try_value());

    cue::distribution::DependencyBuildIdentity build = {"x64-windows",
                                                        cue::distribution::DistributionArchitecture::X64,
                                                        cue::distribution::DistributionArchitecture::X64,
                                                        "msvc",
                                                        "19.44.35221",
                                                        "14.44.35221",
                                                        "dynamic",
                                                        "14.44.35221",
                                                        "10.0.26100.0"};
    auto root = cue::distribution::make_dependency_root_id(*definition.try_value(), build, a_assertContext);
    require(root.has_value() && cue::distribution::is_canonical_sha256(*root.try_value()));
    build.compilerVersion = "19.45.1";
    auto otherRoot = cue::distribution::make_dependency_root_id(*definition.try_value(), build, a_assertContext);
    require(otherRoot.has_value() && *root.try_value() != *otherRoot.try_value());

    auto versionDirectory = cue::distribution::make_distribution_version_directory(
        "1.2.3", "12345678-1234-4abc-8def-1234567890ab", a_assertContext);
    auto dependencyDirectory = cue::distribution::make_dependency_root_directory(*root.try_value(), a_assertContext);
    return versionDirectory.has_value() &&
           *versionDirectory.try_value() == "v1.2.3--12345678-1234-4abc-8def-1234567890ab" &&
           dependencyDirectory.has_value() && *dependencyDirectory.try_value() == *root.try_value();
}
} // namespace

int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    require(test_manifest_round_trip(assertContext));
    require(test_noncanonical_and_unknown_rejected(assertContext));
    require(test_manifest_identity_and_inventory_rejected(assertContext));
    require(test_identity_derivation(assertContext));
    return 0;
}
