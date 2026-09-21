#include "DistributionBuildMetadata.h"

#include <Cue/Distribution/Windows/SourceSdkPublisher.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <Windows.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
[[nodiscard]] std::string utf8_text(std::wstring_view a_value)
{
    const std::u8string value = std::filesystem::path(a_value).u8string();
    return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}

[[nodiscard]] std::optional<std::string> argument_value(int a_argumentCount, wchar_t **a_arguments,
                                                        std::wstring_view a_name)
{
    for (int index = 1; index + 1 < a_argumentCount; ++index)
    {
        if (std::wstring_view(a_arguments[index]) == a_name)
        {
            return utf8_text(a_arguments[index + 1]);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> environment_value(const wchar_t *a_name)
{
    const DWORD required = GetEnvironmentVariableW(a_name, nullptr, 0U);
    if (required == 0U)
    {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(a_name, value.data(), required);
    if (written == 0U || written >= required)
    {
        return std::nullopt;
    }
    value.resize(written);
    return utf8_text(value);
}

[[nodiscard]] std::optional<std::vector<cue::ChildProcessEnvironmentEntry>> capture_environment()
{
    struct Variable final
    {
        const wchar_t *nativeName;
        const char *name;
    };
    constexpr std::array variables = {
        Variable{L"SystemRoot", "SYSTEMROOT"},
        Variable{L"windir", "windir"},
        Variable{L"SystemDrive", "SystemDrive"},
        Variable{L"ProgramFiles", "ProgramFiles"},
        Variable{L"ProgramFiles(x86)", "ProgramFiles(x86)"},
        Variable{L"ProgramData", "ProgramData"},
        Variable{L"CommonProgramFiles", "CommonProgramFiles"},
        Variable{L"CommonProgramFiles(x86)", "CommonProgramFiles(x86)"},
        Variable{L"LOCALAPPDATA", "LOCALAPPDATA"},
        Variable{L"USERPROFILE", "USERPROFILE"},
        Variable{L"TEMP", "TEMP"},
        Variable{L"TMP", "TMP"},
        Variable{L"ComSpec", "ComSpec"},
        Variable{L"PATH", "PATH"},
        Variable{L"PATHEXT", "PATHEXT"},
        Variable{L"OS", "OS"},
        Variable{L"NUMBER_OF_PROCESSORS", "NUMBER_OF_PROCESSORS"},
        Variable{L"PROCESSOR_ARCHITECTURE", "PROCESSOR_ARCHITECTURE"},
        Variable{L"PROCESSOR_IDENTIFIER", "PROCESSOR_IDENTIFIER"},
        Variable{L"PROCESSOR_LEVEL", "PROCESSOR_LEVEL"},
        Variable{L"PROCESSOR_REVISION", "PROCESSOR_REVISION"},
    };
    std::vector<cue::ChildProcessEnvironmentEntry> result;
    result.reserve(variables.size());
    for (const Variable &variable : variables)
    {
        auto value = environment_value(variable.nativeName);
        if (!value)
        {
            return std::nullopt;
        }
        result.push_back({variable.name, std::move(*value)});
    }
    return result;
}

[[nodiscard]] int run(int a_argumentCount, wchar_t **a_arguments, cue::Logger &a_logger,
                      const cue::AssertContext &a_assertContext)
{
    auto destinationParent = argument_value(a_argumentCount, a_arguments, L"--destination-parent");
    auto operationsRoot = argument_value(a_argumentCount, a_arguments, L"--operations-root");
    auto dependenciesParent = argument_value(a_argumentCount, a_arguments, L"--dependencies-parent");
    auto bundleId = argument_value(a_argumentCount, a_arguments, L"--bundle-id");
    auto engineVersion = argument_value(a_argumentCount, a_arguments, L"--engine-version");
    auto environment = capture_environment();
    if (!destinationParent || !operationsRoot || !dependenciesParent || !bundleId || !environment)
    {
        static_cast<void>(a_logger.log(
            cue::LogLevel::Error,
            "Usage: CueEngineDistributionPublisherTool --destination-parent <absolute> --operations-root <absolute> "
            "--dependencies-parent <absolute> --bundle-id <uuid> [--engine-version <major.minor.patch>]"));
        return 2;
    }

    cue::distribution::WindowsSourceSdkPublishRequest request;
    request.repositoryRoot = cue::distribution_build_metadata::k_repositoryRoot;
    request.destinationParent = std::move(*destinationParent);
    request.operationsRoot = std::move(*operationsRoot);
    request.dependenciesParent = std::move(*dependenciesParent);
    request.engineVersion = engineVersion ? std::move(*engineVersion) : "1.0.0";
    request.bundleId = std::move(*bundleId);
    request.gitExecutable = cue::distribution_build_metadata::k_gitCommand;
    request.cmakeExecutable = cue::distribution_build_metadata::k_cmakeCommand;
    request.powershellExecutable = cue::distribution_build_metadata::k_powerShellCommand;
    request.cmakeGenerator = cue::distribution_build_metadata::k_cmakeGenerator;
    request.publisherBuildIdentity = {"",
                                      "x64-windows",
                                      cue::distribution::DistributionArchitecture::X64,
                                      cue::distribution::DistributionArchitecture::X64,
                                      "msvc",
                                      std::string(cue::distribution_build_metadata::k_compilerVersion),
                                      std::string(cue::distribution_build_metadata::k_toolsetVersion),
                                      "dynamic",
                                      std::string(cue::distribution_build_metadata::k_toolsetVersion),
                                      std::string(cue::distribution_build_metadata::k_windowsSdkVersion),
                                      "Release"};
    request.minimumToolchain = {std::string(cue::distribution_build_metadata::k_cmakeVersion),
                                "2.44.0",
                                "msvc",
                                std::string(cue::distribution_build_metadata::k_compilerVersion),
                                std::string(cue::distribution_build_metadata::k_windowsSdkVersion)};
    request.environmentAllowlist = std::move(*environment);

    auto published = cue::distribution::publish_windows_source_sdk(request, a_assertContext);
    if (!published)
    {
        static_cast<void>(a_logger.log(cue::LogLevel::Error, "Developer Source SDK publication failed",
                                       std::move(*published.try_error())));
        return 1;
    }
    static_cast<void>(a_logger.log(cue::LogLevel::Info, "Developer Source SDK publication completed"));
    return 0;
}
} // namespace

/// @brief Developer Source SDK Publisherの診断寿命を所有してCLI結果を返す
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    cue::AbortFatalHandler fatalHandler;
    try
    {
        std::vector<std::unique_ptr<cue::LogSink>> sinks;
        sinks.push_back(std::make_unique<cue::ConsoleLogSink>());
        cue::Logger logger(fatalHandler, std::move(sinks));
        cue::AssertContext assertContext(logger, fatalHandler);
        return run(a_argumentCount, a_arguments, logger, assertContext);
    }
    catch (...)
    {
        fatalHandler.terminate("Distribution Publisher Tool allocation failed");
    }
}
