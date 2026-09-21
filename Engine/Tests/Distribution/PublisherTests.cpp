#include <Cue/Distribution/Publisher.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
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

[[nodiscard]] cue::distribution::PublisherBuildIdentity make_identity()
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

[[nodiscard]] cue::distribution::SourceBlobEvidence source(std::string a_path, char a_hash)
{
    const std::string digest = hash(a_hash);
    return {std::move(a_path), std::string(40U, a_hash), 10U, digest, 10U, digest};
}

[[nodiscard]] std::vector<cue::distribution::SourceBlobEvidence> make_sources()
{
    return {
        source("Engine/Source/Foundation/Foo.cpp", '1'),
        source("Engine/Source/Renderer/Shaders/Foo.hlsl", '2'),
        source("CMakeLists.txt", '3'),
        source("Tools/Dependencies/RestoreVcpkg.ps1", '4'),
        source("Engine/Documents/CODING_RULES.md", '5'),
        source("LICENSE.txt", '6'),
        source("ThirdParty/vcpkg.json", '7'),
        source("ThirdParty/vcpkg-configuration.json", '8'),
        source("ThirdParty/vcpkg-tool.json", '9'),
        source("ThirdParty/THIRD_PARTY_NOTICES.md", 'a'),
        source("ThirdParty/Licenses/DearImGui-LICENSE.txt", 'b'),
    };
}

[[nodiscard]] std::vector<cue::distribution::GeneratedToolEvidence> make_tools(
    const cue::distribution::PublisherBuildIdentity &a_identity)
{
    using cue::distribution::DistributionArchitecture;
    using cue::distribution::DistributionFileRole;
    cue::distribution::PublisherBuildIdentity bootstrapIdentity = a_identity;
    bootstrapIdentity.crtLinkage = "static";
    return {
        {DistributionFileRole::Bootstrap, "CueEngineBootstrap", "Bin/CueEngineBootstrap.exe", "Release",
         a_identity.builtFromRevision, bootstrapIdentity, DistributionArchitecture::X64, 10U, hash('1')},
        {DistributionFileRole::ProjectHub, "CueProjectHubTool", "Bin/CueProjectHubTool.exe", "Release",
         a_identity.builtFromRevision, a_identity, DistributionArchitecture::X64, 10U, hash('2')},
        {DistributionFileRole::Editor, "CueEditorTool", "Bin/CueEditorTool.exe", "Release",
         a_identity.builtFromRevision, a_identity, DistributionArchitecture::X64, 10U, hash('3')},
        {DistributionFileRole::RuntimeHost, "CueRuntimeHost", "Bin/CueRuntimeHost.exe", "Release",
         a_identity.builtFromRevision, a_identity, DistributionArchitecture::X64, 10U, hash('4')},
        {DistributionFileRole::Installer, "CueEngineInstallerTool", "Bin/CueEngineInstallerTool.exe", "Release",
         a_identity.builtFromRevision, a_identity, DistributionArchitecture::X64, 10U, hash('5')},
        {DistributionFileRole::InstallWorker, "CueEngineInstallWorker", "Bin/CueEngineInstallWorker.exe", "Release",
         a_identity.builtFromRevision, a_identity, DistributionArchitecture::X64, 10U, hash('6')},
    };
}

[[nodiscard]] bool test_allowlist(const cue::AssertContext &a_assertContext)
{
    using cue::distribution::DistributionFileRole;
    const auto sourceRole =
        cue::distribution::classify_distribution_source_path("Engine/Source/Foundation/Foo.cpp", a_assertContext);
    const auto hlslRole =
        cue::distribution::classify_distribution_source_path("Engine/Source/RHI/Foo.hlsl", a_assertContext);
    const auto cmakeRole =
        cue::distribution::classify_distribution_source_path("Engine/Source/Foo/CMakeLists.txt", a_assertContext);
    const auto scriptRole =
        cue::distribution::classify_distribution_source_path("Tools/Dependencies/RestoreVcpkg.ps1", a_assertContext);
    const auto buildModuleRole =
        cue::distribution::classify_distribution_source_path("Engine/Source/Build/Private/Plan.cpp", a_assertContext);
    require(sourceRole.has_value() && *sourceRole.try_value() == DistributionFileRole::EngineSource);
    require(hlslRole.has_value() && *hlslRole.try_value() == DistributionFileRole::Hlsl);
    require(cmakeRole.has_value() && *cmakeRole.try_value() == DistributionFileRole::CMake);
    require(scriptRole.has_value() && *scriptRole.try_value() == DistributionFileRole::Script);
    require(buildModuleRole.has_value() && *buildModuleRole.try_value() == DistributionFileRole::EngineSource);
    require(cue::distribution::classify_distribution_source_path("Engine/Source/Editor/App.ico", a_assertContext)
                .has_value());
    require(cue::distribution::classify_distribution_source_path("Engine/Source/Editor/Logo.png", a_assertContext)
                .has_value());
    require(!cue::distribution::classify_distribution_source_path("Engine/Source/Foo/credentials.json",
                                                                  a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Engine/Source/Foo/.env", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Engine/Source/Foo/signing.p12", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Templates/.env", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Templates/.env.production", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Templates/credentials.json", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Templates/credentials.yaml", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Templates/secrets.yaml", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Templates/signing.p12", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Templates/signing.ppk", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Templates/id_ecdsa", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Templates/id_dsa", a_assertContext));
    require(cue::distribution::classify_distribution_source_path("Templates/Blank3D/CueProject.json", a_assertContext)
                .has_value());
    require(cue::distribution::classify_distribution_source_path("Templates/Blank3D/Scenes/Main.cuescene",
                                                                  a_assertContext)
                .has_value());
    require(cue::distribution::classify_distribution_source_path("Templates/Blank3D/CMakeLists.txt", a_assertContext)
                .has_value());
    require(!cue::distribution::classify_distribution_source_path("Templates/build/generated.json", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("CMake/build/cache.cmake", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("CMake/CMakeCache.txt", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("CMake/CMakeFiles/compiler_depend.make",
                                                                  a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Engine/Tests/Foo.cpp", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Tools/GameCoreBenchmark/CMakeLists.txt",
                                                                  a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Tools/Package/Injected.cmake", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Engine/Source", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("ThirdParty/Licenses", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path(".codex/config.toml", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("Build/CueEditorTool.exe", a_assertContext));
    require(!cue::distribution::classify_distribution_source_path("../outside", a_assertContext));
    return !cue::distribution::classify_distribution_source_path("Docs/Evidence/report.md", a_assertContext);
}

[[nodiscard]] bool test_repository_evidence(const cue::AssertContext &a_assertContext)
{
    cue::distribution::RepositoryStateEvidence started{std::string(40U, '1'), std::string(40U, '2'), hash('3'),
                                                       hash('4'), true};
    cue::distribution::RepositoryStateEvidence completed = started;
    require(cue::distribution::validate_repository_evidence(started, completed, a_assertContext).has_value());
    completed.headRevision = std::string(40U, '5');
    require(!cue::distribution::validate_repository_evidence(started, completed, a_assertContext));
    completed = started;
    completed.clean = false;
    return !cue::distribution::validate_repository_evidence(started, completed, a_assertContext);
}

[[nodiscard]] bool test_inventory(const cue::AssertContext &a_assertContext)
{
    const cue::distribution::PublisherBuildIdentity identity = make_identity();
    std::vector sources = make_sources();
    std::vector tools = make_tools(identity);
    auto inventory = cue::distribution::make_distribution_publisher_inventory(
        sources, tools, identity.builtFromRevision, identity, a_assertContext);
    require(inventory.has_value());
    require(cue::distribution::is_canonical_sha256(inventory.try_value()->sourceInventoryHash));
    require(inventory.try_value()->files.size() == sources.size() + tools.size());
    require(std::ranges::is_sorted(inventory.try_value()->files, {},
                                   &cue::distribution::DistributionFileEntry::relativePath));

    std::ranges::reverse(sources);
    auto reordered = cue::distribution::make_distribution_publisher_inventory(
        sources, tools, identity.builtFromRevision, identity, a_assertContext);
    require(reordered.has_value() &&
            reordered.try_value()->sourceInventoryHash == inventory.try_value()->sourceInventoryHash);

    sources = make_sources();
    sources.front().stagedSha256 = hash('f');
    require(!cue::distribution::make_distribution_publisher_inventory(sources, tools, identity.builtFromRevision,
                                                                      identity, a_assertContext));

    sources = make_sources();
    tools.front().configuration = "Debug";
    require(!cue::distribution::make_distribution_publisher_inventory(sources, tools, identity.builtFromRevision,
                                                                      identity, a_assertContext));

    tools = make_tools(identity);
    tools.front().publisherBuildIdentity.crtLinkage = "dynamic";
    require(!cue::distribution::make_distribution_publisher_inventory(sources, tools, identity.builtFromRevision,
                                                                      identity, a_assertContext));

    tools = make_tools(identity);
    tools[1].publisherBuildIdentity.crtLinkage = "static";
    require(!cue::distribution::make_distribution_publisher_inventory(sources, tools, identity.builtFromRevision,
                                                                      identity, a_assertContext));

    tools = make_tools(identity);
    tools.pop_back();
    require(!cue::distribution::make_distribution_publisher_inventory(sources, tools, identity.builtFromRevision,
                                                                      identity, a_assertContext));

    sources.pop_back();
    tools = make_tools(identity);
    return !cue::distribution::make_distribution_publisher_inventory(sources, tools, identity.builtFromRevision,
                                                                     identity, a_assertContext);
}
} // namespace

int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    require(test_allowlist(assertContext));
    require(test_repository_evidence(assertContext));
    require(test_inventory(assertContext));
    constexpr std::array emptyBytes = {std::byte{}};
    const auto emptyHash = cue::distribution::compute_distribution_sha256(
        std::span<const std::byte>(emptyBytes.data(), 0U), assertContext);
    require(emptyHash.has_value());
    require(*emptyHash.try_value() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    return 0;
}
