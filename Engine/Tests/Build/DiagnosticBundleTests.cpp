#include <Cue/Build/DiagnosticBundle.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 1U};

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief 想定外の引数なしFatal終了を固有Exit Codeで検出する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief 想定外のMessage付きFatal終了を固有Exit Codeで検出する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

/// @brief Test前提違反をSource Line由来のExit Codeで即時報告する
void require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::_Exit(static_cast<int>((a_location.line() % 200U) + 20U));
    }
}

/// @brief 成功Resultから値所有権を取得し失敗をTest Errorとして扱う
template <typename T> [[nodiscard]] T take_value(cue::Result<T> a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief Native Filesystem Pathを公開境界へ渡せるUTF-8 Generic Pathへ変換する
[[nodiscard]] std::string generic_utf8_path(const std::filesystem::path &a_path)
{
    const std::u8string encoded = a_path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t value : encoded)
    {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

/// @brief Windows Test用にAbsolute PathをExtended-length形式へ変換する
[[nodiscard]] std::filesystem::path native_test_path(const std::filesystem::path &a_path)
{
#if defined(_WIN32)
    std::filesystem::path preferred = a_path;
    preferred.make_preferred();
    const std::wstring &native = preferred.native();
    if (native.starts_with(L"\\\\?\\"))
    {
        return preferred;
    }
    if (native.starts_with(L"\\\\"))
    {
        return std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2U));
    }
    return std::filesystem::path(L"\\\\?\\" + native);
#else
    return a_path;
#endif
}

/// @brief 指定Project RootからDiagnostic検証用Build Planを作成する
[[nodiscard]] cue::BuildPlan make_plan(std::string_view a_projectRoot, const cue::AssertContext &a_assertContext)
{
    cue::BuildProfile profile = take_value(
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext));
    return take_value(cue::create_build_plan({std::string(a_projectRoot), std::move(profile),
                                              "01234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility},
                                             a_assertContext));
}

/// @brief 必須Fileを含むDiagnostic検証用Artifact Inventoryを作成する
[[nodiscard]] cue::BuildArtifactInventory make_artifact(const cue::BuildPlan &a_plan,
                                                        const cue::AssertContext &a_assertContext)
{
    return take_value(cue::BuildArtifactInventory::create(a_plan, "11234567-89ab-4cde-8f01-23456789abcd",
                                                          {{"CueGameModule.dll", 256U, std::string(64U, 'a')},
                                                           {"CueGameModule.metadata.json", 64U, std::string(64U, 'b')}},
                                                          a_assertContext));
}

/// @brief Redaction対象Pathを含むDiagnostic検証用Environment Reportを作成する
[[nodiscard]] cue::BuildEnvironmentReport make_environment()
{
    cue::BuildEnvironmentReport environment;
    environment.support = cue::BuildEnvironmentSupport::Supported;
    environment.engineSourceRoot = "C:/Users/Tester/CueEngine";
    environment.engineBinaryRoot = "C:/Users/Tester/CueEngine/out/build";
    environment.selectedTools.push_back({cue::BuildToolKind::CMake, "C:/Program Files/CMake/bin/cmake.exe",
                                         "C:/Program Files/CMake", cue::BuildToolVersion{4U, 2U, 0U, 0U},
                                         cue::BuildArchitecture::X64, true});
    environment.selectedTools.push_back({cue::BuildToolKind::Git, "C:/Program Files/Git/cmd/git.exe",
                                         "C:/Program Files/Git", cue::BuildToolVersion{2U, 44U, 0U, 1U},
                                         cue::BuildArchitecture::X64, true});
    environment.diagnostics.push_back({cue::BuildEnvironmentDiagnosticCode::UnsupportedTool,
                                       cue::BuildEnvironmentSupport::Unsupported, cue::BuildToolKind::MsvcCompiler,
                                       "D:/Internal/Toolchain/cl.exe", "Unsupported compiler",
                                       "Install a compatible compiler"});
    environment.supportedConfigurations = {cue::BuildConfiguration::Debug};
    return environment;
}

/// @brief Stage失敗、Log、診断、直近Artifactを含むOperation Snapshotを作成する
[[nodiscard]] cue::BuildOperationSnapshot make_failed_operation(const cue::BuildPlan &a_plan,
                                                                const cue::AssertContext &a_assertContext)
{
    cue::BuildOperationSnapshot operation;
    operation.state = cue::GameBuildOperationState::Failed;
    operation.operationId = std::string(a_plan.operation_id());
    operation.profile = a_plan.profile();
    operation.stages = {{cue::BuildStage::Configure, cue::BuildStageOutcome::Succeeded, 0U},
                        {cue::BuildStage::Build, cue::BuildStageOutcome::Failed, 2U}};
    operation.logs = {{operation.operationId, cue::BuildStage::Configure, 0U, cue::ChildProcessStream::StandardOutput,
                       "Configuring C:\\Users/Tester\\CueProject/Source\\Game\n"},
                      {operation.operationId, cue::BuildStage::Build, 1U, cue::ChildProcessStream::StandardError,
                       "C:/Users/Tester/CueProject/Source/Game/Game.cpp(1): error\n"}};
    operation.diagnostics = {
        {"Cue.Build", 2, "Build failed", {"C:/Users/Tester/CueProject"}, cue::BuildNativeErrorSnapshot{"Win32", 5}}};
    operation.latestSuccessfulArtifact = make_artifact(a_plan, a_assertContext);
    return operation;
}

/// @brief Manifestから指定PathのEntryを検索する
[[nodiscard]] const cue::BuildDiagnosticManifestEntry *find_entry(const cue::BuildDiagnosticBundle &a_bundle,
                                                                  std::string_view a_path) noexcept
{
    for (const cue::BuildDiagnosticManifestEntry &entry : a_bundle.manifest_entries())
    {
        if (entry.relativePath == a_path)
        {
            return &entry;
        }
    }
    return nullptr;
}

/// @brief Bundleから指定PathのFileを検索する
[[nodiscard]] const cue::BuildDiagnosticBundleFile *find_file(const cue::BuildDiagnosticBundle &a_bundle,
                                                              std::string_view a_path) noexcept
{
    const auto file = std::find_if(a_bundle.files().begin(), a_bundle.files().end(),
                                   /// @brief 指定Pathと一致するBundle Fileを検出する
                                   [a_path](const auto &a_file) noexcept { return a_file.relativePath == a_path; });
    return file == a_bundle.files().end() ? nullptr : &*file;
}

/// @brief Bundle FileのByte列を検査用文字列へ変換する
[[nodiscard]] std::string file_text(const cue::BuildDiagnosticBundleFile &a_file)
{
    std::string text;
    text.reserve(a_file.bytes.size());
    for (const std::byte value : a_file.bytes)
    {
        text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    return text;
}

/// @brief Bundle全FileのByte列を検査用文字列へ連結する
[[nodiscard]] std::string bundle_text(const cue::BuildDiagnosticBundle &a_bundle)
{
    std::string text;
    for (const cue::BuildDiagnosticBundleFile &file : a_bundle.files())
    {
        for (const std::byte value : file.bytes)
        {
            text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
        }
    }
    return text;
}

/// @brief 失敗Build、Redaction、欠損理由、上限、Directory再読込を検証する
void test_diagnostic_bundle(std::string_view a_testRoot, const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path projectRoot = std::filesystem::path(a_testRoot) / "CueBuildDiagnosticProject";
    std::error_code cleanupError;
    std::filesystem::remove_all(projectRoot, cleanupError);
    require(!cleanupError);
    require(std::filesystem::create_directories(projectRoot));
    cue::BuildPlan plan = make_plan(projectRoot.generic_string(), a_assertContext);
    cue::BuildDiagnosticBundleInput input{make_failed_operation(plan, a_assertContext),
                                          cue::make_build_diagnostic_plan_snapshot(plan, a_assertContext),
                                          make_environment(),
                                          {{"C:/Users/Tester", "<USER_HOME>"}}};
    cue::BuildDiagnosticBundleLimits limits;
    cue::BuildDiagnosticBundle bundle = take_value(cue::create_build_diagnostic_bundle(input, limits, a_assertContext));
    require(bundle.state() == cue::GameBuildOperationState::Failed);
    require(bundle.files().size() == 8U);
    constexpr std::array<std::string_view, 7U> expectedEntryPaths = {
        "artifact.json", "environment.json", "plan.json", "result.json", "stages.json", "stderr.log", "stdout.log"};
    require(bundle.manifest_entries().size() == expectedEntryPaths.size());
    for (std::size_t index = 0U; index < expectedEntryPaths.size(); ++index)
    {
        require(bundle.manifest_entries()[index].relativePath == expectedEntryPaths[index]);
    }
    require(find_entry(bundle, "environment.json") != nullptr);
    require(find_entry(bundle, "environment.json")->collected);
    require(find_entry(bundle, "artifact.json") != nullptr);
    require(find_entry(bundle, "artifact.json")->collected);
    const std::string allText = bundle_text(bundle);
    require(allText.find("C:/Users/Tester") == std::string::npos);
    require(allText.find("C:\\Users\\Tester") == std::string::npos);
    require(allText.find("C:\\Users/Tester") == std::string::npos);
    require(allText.find("<PROJECT_ROOT>") != std::string::npos);
    require(allText.find("Source/Game/Game.cpp") != std::string::npos);
    require(allText.find("4.2.0.0") != std::string::npos);
    require(allText.find("supportedConfigurations") != std::string::npos);
    require(allText.find("\"nativeError\":{\"domain\":\"Win32\",\"code\":5}") != std::string::npos);
    require(allText.find("D:/Internal/Toolchain/cl.exe") == std::string::npos);
    require(allText.find("<DIAGNOSTIC_PATH_0>") != std::string::npos);
    require(allText.find("\"tool\":2") != std::string::npos);

    cue::BuildDiagnosticBundleInput expandingInput = input;
    expandingInput.operation.logs = {{expandingInput.operation.operationId, cue::BuildStage::Build, 0U,
                                      cue::ChildProcessStream::StandardOutput, std::string(1024U, 'x')}};
    expandingInput.pathMappings.push_back({"x", "<EXPANDED>"});
    cue::BuildDiagnosticBundleLimits expandingLimits{16U, 1024U, 16U * 1024U};
    require(!cue::create_build_diagnostic_bundle(expandingInput, expandingLimits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput oversizedChunkInput = input;
    oversizedChunkInput.operation.logs = {{oversizedChunkInput.operation.operationId, cue::BuildStage::Build, 0U,
                                           cue::ChildProcessStream::StandardOutput, std::string(2048U, 'z')}};
    require(!cue::create_build_diagnostic_bundle(oversizedChunkInput, expandingLimits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput oversizedMetadataInput = input;
    oversizedMetadataInput.operation.diagnostics.front().summary = std::string(2048U, 'm');
    require(!cue::create_build_diagnostic_bundle(oversizedMetadataInput, expandingLimits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput oversizedAutomaticPathInput = input;
    oversizedAutomaticPathInput.environment->engineSourceRoot = std::string(4097U, 'p');
    require(!cue::create_build_diagnostic_bundle(oversizedAutomaticPathInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput oversizedExplicitMappingInput = input;
    oversizedExplicitMappingInput.pathMappings.front().nativePrefix = std::string(4097U, 'p');
    require(!cue::create_build_diagnostic_bundle(oversizedExplicitMappingInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput excessiveMappingCountInput = input;
    excessiveMappingCountInput.pathMappings.resize(limits.maximumPathMappings + 1U);
    require(!cue::create_build_diagnostic_bundle(excessiveMappingCountInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput unknownToolKindInput = input;
    unknownToolKindInput.environment->selectedTools.front().kind = static_cast<cue::BuildToolKind>(255U);
    require(!cue::create_build_diagnostic_bundle(unknownToolKindInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput unknownDiagnosticCodeInput = input;
    unknownDiagnosticCodeInput.environment->diagnostics.front().code =
        static_cast<cue::BuildEnvironmentDiagnosticCode>(255U);
    require(!cue::create_build_diagnostic_bundle(unknownDiagnosticCodeInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput unknownDiagnosticToolInput = input;
    unknownDiagnosticToolInput.environment->diagnostics.front().tool = static_cast<cue::BuildToolKind>(255U);
    require(!cue::create_build_diagnostic_bundle(unknownDiagnosticToolInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput unknownEnvironmentSupportInput = input;
    unknownEnvironmentSupportInput.environment->support = static_cast<cue::BuildEnvironmentSupport>(255U);
    require(!cue::create_build_diagnostic_bundle(unknownEnvironmentSupportInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput unknownDiagnosticSupportInput = input;
    unknownDiagnosticSupportInput.environment->diagnostics.front().support =
        static_cast<cue::BuildEnvironmentSupport>(255U);
    require(!cue::create_build_diagnostic_bundle(unknownDiagnosticSupportInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput unknownArchitectureInput = input;
    unknownArchitectureInput.environment->selectedTools.front().architecture =
        static_cast<cue::BuildArchitecture>(255U);
    require(!cue::create_build_diagnostic_bundle(unknownArchitectureInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput unknownConfigurationInput = input;
    unknownConfigurationInput.environment->supportedConfigurations.front() = static_cast<cue::BuildConfiguration>(255U);
    require(!cue::create_build_diagnostic_bundle(unknownConfigurationInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput knownArchitectureInput = input;
    knownArchitectureInput.environment->selectedTools.front().architecture = cue::BuildArchitecture::X86;
    require(
        bundle_text(take_value(cue::create_build_diagnostic_bundle(knownArchitectureInput, limits, a_assertContext)))
            .find("\"architecture\":\"x86\"") != std::string::npos);

    cue::BuildDiagnosticBundleInput unknownStateInput = input;
    unknownStateInput.operation.state = static_cast<cue::GameBuildOperationState>(255U);
    require(!cue::create_build_diagnostic_bundle(unknownStateInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput failedWithOperationArtifactInput = input;
    failedWithOperationArtifactInput.operation.artifact = make_artifact(plan, a_assertContext);
    require(
        !cue::create_build_diagnostic_bundle(failedWithOperationArtifactInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput succeededWithoutOperationArtifactInput = input;
    succeededWithoutOperationArtifactInput.operation.state = cue::GameBuildOperationState::Succeeded;
    require(!cue::create_build_diagnostic_bundle(succeededWithoutOperationArtifactInput, limits, a_assertContext)
                 .has_value());

    cue::BuildDiagnosticBundleInput successfulInput = input;
    successfulInput.operation.state = cue::GameBuildOperationState::Succeeded;
    successfulInput.operation.stages.back() = {cue::BuildStage::Build, cue::BuildStageOutcome::Succeeded, 0U};
    successfulInput.operation.artifact = successfulInput.operation.latestSuccessfulArtifact;
    cue::BuildDiagnosticBundleInput succeededWithoutStagesInput = successfulInput;
    succeededWithoutStagesInput.operation.stages.clear();
    require(!cue::create_build_diagnostic_bundle(succeededWithoutStagesInput, limits, a_assertContext).has_value());
    cue::BuildDiagnosticBundleInput succeededWithFailedStageInput = successfulInput;
    succeededWithFailedStageInput.operation.stages.back() = {cue::BuildStage::Build, cue::BuildStageOutcome::Failed,
                                                             2U};
    require(!cue::create_build_diagnostic_bundle(succeededWithFailedStageInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput unknownStageInput = input;
    unknownStageInput.operation.stages.front().stage = static_cast<cue::BuildStage>(255U);
    require(!cue::create_build_diagnostic_bundle(unknownStageInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput unknownStageOutcomeInput = input;
    unknownStageOutcomeInput.operation.stages.front().outcome = static_cast<cue::BuildStageOutcome>(255U);
    require(!cue::create_build_diagnostic_bundle(unknownStageOutcomeInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput inconsistentStageInput = input;
    inconsistentStageInput.operation.stages.front().exitCode.reset();
    require(!cue::create_build_diagnostic_bundle(inconsistentStageInput, limits, a_assertContext).has_value());

    cue::BuildDiagnosticBundleInput invalidUtf8LogInput = input;
    invalidUtf8LogInput.operation.logs.front().bytes = std::string("\xC3", 1U);
    auto invalidUtf8Result = cue::create_build_diagnostic_bundle(invalidUtf8LogInput, limits, a_assertContext);
    require(!invalidUtf8Result.has_value());
    require(invalidUtf8Result.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::BuildDiagnosticBundleError::InvalidInput));

    cue::BuildDiagnosticBundleInput nonCanonicalLogInput = input;
    nonCanonicalLogInput.operation.logs.front().bytes = std::string("\xEF\xBB\xBF", 3U) + "First\r\nSecond";
    cue::BuildDiagnosticBundle normalizedLogBundle =
        take_value(cue::create_build_diagnostic_bundle(nonCanonicalLogInput, limits, a_assertContext));
    require(find_file(normalizedLogBundle, "stdout.log") != nullptr);
    require(file_text(*find_file(normalizedLogBundle, "stdout.log")) == "First\nSecond\n");

    const std::filesystem::path destination =
        std::filesystem::path(a_testRoot) / L"CueBuildDiagnosticBundleTests-\u8A3A\u65AD-01234567";
    const std::string destinationUtf8 = generic_utf8_path(destination);
    std::filesystem::path staging = destination;
    staging += ".staging-" + std::string(bundle.operation_id());
    std::filesystem::remove_all(destination, cleanupError);
    require(!cleanupError);

    const std::filesystem::path movedDestination = destination.parent_path() / "CueBuildDiagnosticBundleMovedFrom";
    std::filesystem::path movedStaging = movedDestination;
    movedStaging += ".staging-";
    std::filesystem::remove_all(movedDestination, cleanupError);
    require(!cleanupError);
    std::filesystem::remove_all(movedStaging, cleanupError);
    require(!cleanupError);
    cue::BuildDiagnosticBundle movedFrom =
        take_value(cue::create_build_diagnostic_bundle(successfulInput, limits, a_assertContext));
    cue::BuildDiagnosticBundle movedOwner = std::move(movedFrom);
    require(!movedOwner.files().empty());
    auto movedWrite =
        cue::write_build_diagnostic_bundle_directory(movedFrom, generic_utf8_path(movedDestination), a_assertContext);
    require(!movedWrite.has_value());
    require(movedWrite.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::BuildDiagnosticBundleError::InvalidBundle));
    require(!std::filesystem::exists(movedDestination));
    require(!std::filesystem::exists(movedStaging));

    require(cue::write_build_diagnostic_bundle_directory(bundle, destinationUtf8, a_assertContext).has_value());
    require(!std::filesystem::exists(staging));
    require(!cue::write_build_diagnostic_bundle_directory(bundle, destinationUtf8, a_assertContext).has_value());
    cue::BuildDiagnosticBundle reloaded =
        take_value(cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext));
    require(reloaded.operation_id() == bundle.operation_id());
    require(reloaded.state() == bundle.state());
    require(reloaded.manifest_entries().size() == bundle.manifest_entries().size());

    cue::BuildDiagnosticBundle successfulBundle =
        take_value(cue::create_build_diagnostic_bundle(successfulInput, limits, a_assertContext));
    const std::filesystem::path successfulDestination =
        destination.parent_path() / "CueBuildDiagnosticBundleSuccessful";
    std::filesystem::remove_all(successfulDestination, cleanupError);
    require(!cleanupError);
    require(cue::write_build_diagnostic_bundle_directory(successfulBundle, generic_utf8_path(successfulDestination),
                                                         a_assertContext)
                .has_value());
    const cue::BuildDiagnosticBundleFile *successfulStagesFile = find_file(successfulBundle, "stages.json");
    require(successfulStagesFile != nullptr);
    std::string mismatchedSuccessfulStages = file_text(*successfulStagesFile);
    const std::string_view successfulOutcome = "\"outcome\":\"succeeded\",\"exitCode\":0";
    const std::size_t lastSuccessfulOutcome = mismatchedSuccessfulStages.rfind(successfulOutcome);
    require(lastSuccessfulOutcome != std::string::npos);
    mismatchedSuccessfulStages.replace(lastSuccessfulOutcome, successfulOutcome.size(),
                                       "\"outcome\":\"failed\",\"exitCode\":2   ");
    require(mismatchedSuccessfulStages.size() == successfulStagesFile->bytes.size());
    {
        std::ofstream stream(successfulDestination / "stages.json", std::ios::binary | std::ios::trunc);
        stream.write(mismatchedSuccessfulStages.data(),
                     static_cast<std::streamsize>(mismatchedSuccessfulStages.size()));
        stream.close();
        require(stream.good());
    }
    require(
        !cue::read_build_diagnostic_bundle_directory(generic_utf8_path(successfulDestination), limits, a_assertContext)
             .has_value());
    std::filesystem::remove_all(successfulDestination, cleanupError);
    require(!cleanupError);

#if defined(_WIN32)
    std::filesystem::path longDestination = destination.parent_path();
    while (longDestination.native().size() <= 300U)
    {
        longDestination /= L"LongPathComponent-0123456789012345678901234567890123456789";
    }
    longDestination /= destination.filename();
    std::filesystem::remove_all(native_test_path(longDestination.parent_path()), cleanupError);
    require(!cleanupError &&
            cue::write_build_diagnostic_bundle_directory(bundle, generic_utf8_path(longDestination), a_assertContext)
                .has_value());
    cue::BuildDiagnosticBundle longPathReloaded = take_value(
        cue::read_build_diagnostic_bundle_directory(generic_utf8_path(longDestination), limits, a_assertContext));
    require(longPathReloaded.operation_id() == bundle.operation_id());
    std::filesystem::remove_all(native_test_path(longDestination.parent_path()), cleanupError);
    require(!cleanupError);
#endif

    cue::BuildOperationSnapshot splitLogOperation = input.operation;
    splitLogOperation.logs = {{std::string(input.operation.operationId), cue::BuildStage::Build, 0U,
                               cue::ChildProcessStream::StandardOutput, "prefix C:/Users/Tes"},
                              {std::string(input.operation.operationId), cue::BuildStage::Build, 1U,
                               cue::ChildProcessStream::StandardOutput, "ter/Compiler.exe\n"},
                              {std::string(input.operation.operationId), cue::BuildStage::Build, 2U,
                               cue::ChildProcessStream::StandardError, std::string("UTF-8: ") + "\xE6\x97"},
                              {std::string(input.operation.operationId), cue::BuildStage::Build, 3U,
                               cue::ChildProcessStream::StandardError, std::string("\xA5\xE6\x9C\xAC\n")}};
    cue::BuildDiagnosticBundleInput splitLogInput = input;
    splitLogInput.operation = std::move(splitLogOperation);
    cue::BuildDiagnosticBundle splitLogBundle =
        take_value(cue::create_build_diagnostic_bundle(splitLogInput, limits, a_assertContext));
    require(file_text(*find_file(splitLogBundle, "stdout.log")) == "prefix <USER_HOME>/Compiler.exe\n");
    require(file_text(*find_file(splitLogBundle, "stderr.log")) == "UTF-8: 日本\n");

    const std::filesystem::path linkedSource = destination.parent_path() / "CueBuildDiagnosticBundleSourceLink";
    std::filesystem::remove(linkedSource, cleanupError);
    require(!cleanupError);
    std::filesystem::create_directory_symlink(destination, linkedSource, cleanupError);
    if (!cleanupError)
    {
        require(!cue::read_build_diagnostic_bundle_directory(generic_utf8_path(linkedSource), limits, a_assertContext)
                     .has_value());
        require(std::filesystem::remove(linkedSource));
    }
    cleanupError.clear();

    const auto stdoutFile =
        std::find_if(bundle.files().begin(), bundle.files().end(),
                     /// @brief UTF-8改変検証対象の標準出力Logを検出する
                     [](const auto &a_file) noexcept { return a_file.relativePath == "stdout.log"; });
    require(stdoutFile != bundle.files().end() && !stdoutFile->bytes.empty());
    std::vector<std::byte> invalidUtf8Log = stdoutFile->bytes;
    invalidUtf8Log.front() = std::byte{0xFFU};
    /// @brief 標準出力LogのByte列を差替えてReaderへ渡す
    const auto write_stdout = [&destination](std::span<const std::byte> a_bytes)
    {
        std::ofstream stream(destination / "stdout.log", std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
        stream.close();
        require(stream.good());
    };
    write_stdout(invalidUtf8Log);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    std::vector<std::byte> carriageReturnLog = stdoutFile->bytes;
    const auto lineFeed = std::find(carriageReturnLog.begin(), carriageReturnLog.end(), std::byte{'\n'});
    require(lineFeed != carriageReturnLog.end());
    *lineFeed = std::byte{'\r'};
    write_stdout(carriageReturnLog);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    std::vector<std::byte> unterminatedLog = stdoutFile->bytes;
    unterminatedLog.back() = std::byte{'x'};
    write_stdout(unterminatedLog);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    require(stdoutFile->bytes.size() >= 3U);
    std::vector<std::byte> byteOrderMarkedLog = stdoutFile->bytes;
    byteOrderMarkedLog[0U] = std::byte{0xEFU};
    byteOrderMarkedLog[1U] = std::byte{0xBBU};
    byteOrderMarkedLog[2U] = std::byte{0xBFU};
    write_stdout(byteOrderMarkedLog);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    write_stdout(stdoutFile->bytes);

    const cue::BuildDiagnosticBundleFile *stagesFile = find_file(bundle, "stages.json");
    require(stagesFile != nullptr);
    const std::string originalStages = file_text(*stagesFile);
    /// @brief Stage Payload差替えを完了してからReaderへ渡す
    const auto write_stages = [&destination](std::string_view a_text)
    {
        std::ofstream stream(destination / "stages.json", std::ios::binary | std::ios::trunc);
        stream.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
        stream.close();
        require(stream.good());
    };
    std::string inconsistentStage = originalStages;
    const std::size_t successfulExitCode = inconsistentStage.find("\"exitCode\":0");
    require(successfulExitCode != std::string::npos);
    inconsistentStage[successfulExitCode + std::string_view("\"exitCode\":").size()] = '1';
    write_stages(inconsistentStage);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    write_stages(originalStages);

    const cue::BuildDiagnosticBundleFile *artifactFile = find_file(bundle, "artifact.json");
    require(artifactFile != nullptr);
    const std::string originalArtifact = file_text(*artifactFile);
    /// @brief Artifact Payload差替えを完了してからReaderへ渡す
    const auto write_artifact = [&destination](std::string_view a_text)
    {
        std::ofstream stream(destination / "artifact.json", std::ios::binary | std::ios::trunc);
        stream.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
        stream.close();
        require(stream.good());
    };
    std::string invalidArtifactHash = originalArtifact;
    const std::size_t artifactHash = invalidArtifactHash.find("\"contentHash\":\"");
    require(artifactHash != std::string::npos);
    invalidArtifactHash[artifactHash + std::string_view("\"contentHash\":\"").size()] = 'z';
    write_artifact(invalidArtifactHash);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    std::string invalidArtifactId = originalArtifact;
    const std::size_t artifactId = invalidArtifactId.find("\"artifactId\":\"");
    require(artifactId != std::string::npos);
    invalidArtifactId[artifactId + std::string_view("\"artifactId\":\"").size()] = 'z';
    write_artifact(invalidArtifactId);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    write_artifact(originalArtifact);

    const auto planFile = std::find_if(bundle.files().begin(), bundle.files().end(),
                                       /// @brief Payload Schema改変検証対象のPlan Fileを検出する
                                       [](const auto &a_file) noexcept { return a_file.relativePath == "plan.json"; });
    require(planFile != bundle.files().end());
    std::string originalPlan;
    for (const std::byte value : planFile->bytes)
    {
        originalPlan.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    /// @brief Plan Payload差替えを完了してからReaderへ渡す
    const auto write_plan = [&destination](std::string_view a_text)
    {
        std::ofstream stream(destination / "plan.json", std::ios::binary | std::ios::trunc);
        stream.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
        stream.close();
        require(stream.good());
    };
    std::string unknownPlanSchema = originalPlan;
    const std::size_t planSchema = unknownPlanSchema.find("\"schemaVersion\":1");
    require(planSchema != std::string::npos);
    unknownPlanSchema[planSchema + std::string_view("\"schemaVersion\":").size()] = '2';
    write_plan(unknownPlanSchema);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    std::string invalidPlan = originalPlan;
    const std::size_t projectRootMember = invalidPlan.find("\"projectRoot\"");
    require(projectRootMember != std::string::npos);
    invalidPlan[projectRootMember + 1U] = 'x';
    write_plan(invalidPlan);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    write_plan(originalPlan);

    const auto environmentFile =
        std::find_if(bundle.files().begin(), bundle.files().end(),
                     /// @brief Numeric Enum改変検証対象のEnvironment Fileを検出する
                     [](const auto &a_file) noexcept { return a_file.relativePath == "environment.json"; });
    require(environmentFile != bundle.files().end());
    std::string originalEnvironment;
    for (const std::byte value : environmentFile->bytes)
    {
        originalEnvironment.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    require(originalEnvironment.find("\"schemaVersion\":2") != std::string::npos);
    require(originalEnvironment.find("\"kind\":4") != std::string::npos);
    /// @brief Environment Payload差替えを完了してからReaderへ渡す
    const auto write_environment = [&destination](std::string_view a_text)
    {
        std::ofstream stream(destination / "environment.json", std::ios::binary | std::ios::trunc);
        stream.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
        stream.close();
        require(stream.good());
    };
    std::string legacyEnvironment = originalEnvironment;
    const std::size_t environmentSchema = legacyEnvironment.find("\"schemaVersion\":2");
    const std::size_t gitKind = legacyEnvironment.find("\"kind\":4");
    require(environmentSchema != std::string::npos);
    require(gitKind != std::string::npos);
    legacyEnvironment[environmentSchema + std::string_view("\"schemaVersion\":").size()] = '1';
    legacyEnvironment[gitKind + std::string_view("\"kind\":").size()] = '3';
    write_environment(legacyEnvironment);
    require(cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    std::string invalidLegacyEnvironment = originalEnvironment;
    invalidLegacyEnvironment[environmentSchema + std::string_view("\"schemaVersion\":").size()] = '1';
    write_environment(invalidLegacyEnvironment);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    std::string unknownToolKind = originalEnvironment;
    const std::size_t toolKind = unknownToolKind.find("\"kind\":0");
    require(toolKind != std::string::npos);
    unknownToolKind[toolKind + std::string_view("\"kind\":").size()] = '9';
    write_environment(unknownToolKind);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    std::string unknownDiagnosticCode = originalEnvironment;
    const std::size_t diagnosticCode = unknownDiagnosticCode.find("\"code\":3");
    require(diagnosticCode != std::string::npos);
    unknownDiagnosticCode[diagnosticCode + std::string_view("\"code\":").size()] = '9';
    write_environment(unknownDiagnosticCode);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    std::string unknownDiagnosticTool = originalEnvironment;
    const std::size_t diagnosticTool = unknownDiagnosticTool.find("\"tool\":2");
    require(diagnosticTool != std::string::npos);
    unknownDiagnosticTool[diagnosticTool + std::string_view("\"tool\":").size()] = '9';
    write_environment(unknownDiagnosticTool);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    write_environment(originalEnvironment);

    cue::BuildDiagnosticBundleLimits smallLimits = limits;
    smallLimits.maximumFileBytes = 32U;
    smallLimits.maximumTotalBytes = 512U;
    require(!cue::create_build_diagnostic_bundle(input, smallLimits, a_assertContext).has_value());
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, smallLimits, a_assertContext).has_value());

    const std::filesystem::path unexpectedDirectory = destination / "unexpected";
    require(std::filesystem::create_directory(unexpectedDirectory));
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    require(std::filesystem::remove(unexpectedDirectory));

    const auto manifestFile =
        std::find_if(bundle.files().begin(), bundle.files().end(),
                     /// @brief Tamper検証対象のManifest Fileを検出する
                     [](const auto &a_file) noexcept { return a_file.relativePath == "manifest.json"; });
    require(manifestFile != bundle.files().end());
    std::string tamperedManifest;
    for (const std::byte value : manifestFile->bytes)
    {
        tamperedManifest.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    /// @brief Manifest差替えを完了してからReaderへ渡す
    const auto write_manifest = [&destination](std::string_view a_text)
    {
        std::ofstream stream(destination / "manifest.json", std::ios::binary | std::ios::trunc);
        stream.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
        stream.close();
        require(stream.good());
    };
    std::string oversizedStageCode = originalStages;
    const std::size_t failedExitCode = oversizedStageCode.find("\"exitCode\":2");
    require(failedExitCode != std::string::npos);
    oversizedStageCode.replace(failedExitCode + std::string_view("\"exitCode\":").size(), 1U, "4294967296");
    write_stages(oversizedStageCode);
    std::string oversizedStageManifest = tamperedManifest;
    const std::size_t stageEntry = oversizedStageManifest.find("\"path\":\"stages.json\"");
    const std::size_t stageSize = oversizedStageManifest.find("\"sizeBytes\":", stageEntry);
    const std::size_t stageSizeEnd = oversizedStageManifest.find(',', stageSize);
    require(stageEntry != std::string::npos && stageSize != std::string::npos && stageSizeEnd != std::string::npos);
    const std::size_t stageValue = stageSize + std::string_view("\"sizeBytes\":").size();
    oversizedStageManifest.replace(stageValue, stageSizeEnd - stageValue, std::to_string(oversizedStageCode.size()));
    write_manifest(oversizedStageManifest);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    write_stages(originalStages);
    write_manifest(tamperedManifest);

    std::string unexpectedOperationArtifact = originalArtifact;
    const std::size_t latestArtifactMember = unexpectedOperationArtifact.find("latestSuccessfulArtifact");
    require(latestArtifactMember != std::string::npos);
    unexpectedOperationArtifact.replace(latestArtifactMember, std::string_view("latestSuccessfulArtifact").size(),
                                        "operationArtifact");
    write_artifact(unexpectedOperationArtifact);
    std::string unexpectedArtifactManifest = tamperedManifest;
    const std::size_t artifactEntry = unexpectedArtifactManifest.find("\"path\":\"artifact.json\"");
    const std::size_t artifactSize = unexpectedArtifactManifest.find("\"sizeBytes\":", artifactEntry);
    const std::size_t artifactSizeEnd = unexpectedArtifactManifest.find(',', artifactSize);
    require(artifactEntry != std::string::npos && artifactSize != std::string::npos &&
            artifactSizeEnd != std::string::npos);
    const std::size_t artifactValue = artifactSize + std::string_view("\"sizeBytes\":").size();
    unexpectedArtifactManifest.replace(artifactValue, artifactSizeEnd - artifactValue,
                                       std::to_string(unexpectedOperationArtifact.size()));
    write_manifest(unexpectedArtifactManifest);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());
    write_artifact(originalArtifact);
    write_manifest(tamperedManifest);

    std::string unknownSchema = tamperedManifest;
    const std::size_t schemaValue = unknownSchema.find("\"schemaVersion\":1");
    require(schemaValue != std::string::npos);
    unknownSchema.insert(schemaValue + std::string_view("\"schemaVersion\":1").size(), "0");
    write_manifest(unknownSchema);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());

    std::string runningManifest = tamperedManifest;
    const std::size_t failedState = runningManifest.find("\"state\":\"failed\"");
    require(failedState != std::string::npos);
    runningManifest.replace(failedState, std::string_view("\"state\":\"failed\"").size(), "\"state\":\"running\"");
    write_manifest(runningManifest);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());

    std::string mismatchedManifest = tamperedManifest;
    mismatchedManifest.replace(failedState, std::string_view("\"state\":\"failed\"").size(), "\"state\":\"cancelled\"");
    write_manifest(mismatchedManifest);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());

    std::string reorderedManifest = tamperedManifest;
    const std::size_t firstEntryBegin = reorderedManifest.find("{\"path\":");
    const std::size_t firstEntryEnd = reorderedManifest.find('\n', firstEntryBegin) + 1U;
    const std::size_t secondEntryEnd = reorderedManifest.find('\n', firstEntryEnd) + 1U;
    require(firstEntryBegin != std::string::npos && firstEntryEnd > firstEntryBegin && secondEntryEnd > firstEntryEnd);
    const std::string firstEntry = reorderedManifest.substr(firstEntryBegin, firstEntryEnd - firstEntryBegin);
    const std::string secondEntry = reorderedManifest.substr(firstEntryEnd, secondEntryEnd - firstEntryEnd);
    reorderedManifest.replace(firstEntryBegin, secondEntryEnd - firstEntryBegin, secondEntry + firstEntry);
    write_manifest(reorderedManifest);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());

    const std::size_t entryBegin = tamperedManifest.find("{\"path\":");
    const std::size_t entryEnd = tamperedManifest.find('\n', entryBegin);
    require(entryBegin != std::string::npos && entryEnd != std::string::npos);
    tamperedManifest.insert(entryEnd + 1U, tamperedManifest.substr(entryBegin, entryEnd - entryBegin + 1U));
    write_manifest(tamperedManifest);
    require(!cue::read_build_diagnostic_bundle_directory(destinationUtf8, limits, a_assertContext).has_value());

    const std::filesystem::path blockedDestination = destination.parent_path() / "CueBuildDiagnosticBundleBlocked";
    std::filesystem::path blockedStaging = blockedDestination;
    blockedStaging += ".staging-" + std::string(bundle.operation_id());
    std::filesystem::remove_all(blockedDestination, cleanupError);
    require(!cleanupError);
    std::filesystem::remove_all(blockedStaging, cleanupError);
    require(!cleanupError && std::filesystem::create_directory(blockedStaging));
    require(
        !cue::write_build_diagnostic_bundle_directory(bundle, generic_utf8_path(blockedDestination), a_assertContext)
             .has_value());
    require(!std::filesystem::exists(blockedDestination));
    require(std::filesystem::remove(blockedStaging));

    input.environment.reset();
    input.operation.latestSuccessfulArtifact.reset();
    cue::BuildDiagnosticBundle missing =
        take_value(cue::create_build_diagnostic_bundle(input, limits, a_assertContext));
    require(!find_entry(missing, "environment.json")->collected);
    require(!find_entry(missing, "environment.json")->missingReason.empty());
    require(!find_entry(missing, "artifact.json")->collected);
    require(!find_entry(missing, "artifact.json")->missingReason.empty());

    std::filesystem::remove_all(destination, cleanupError);
    require(!cleanupError);
    std::filesystem::remove_all(projectRoot, cleanupError);
    require(!cleanupError);
}
} // namespace

/// @brief Build Diagnostic Bundleの安全な生成、保存、再読込契約を検証する
int main(int a_argumentCount, char **a_arguments)
{
    require(a_argumentCount == 2);
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    test_diagnostic_bundle(a_arguments[1], assertContext);
    return 0;
}
