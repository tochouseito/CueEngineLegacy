#include <Cue/Distribution/Windows/SourceSdkPublisher.h>

#include <Cue/Distribution/Error.h>
#include <Cue/Distribution/Identity.h>
#include <Cue/Distribution/Publisher.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Platform/Windows/WindowsProcess.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_maximumProcessOutputBytes = 512U * 1024U * 1024U;
constexpr std::size_t k_maximumPayloadBytes = 512U * 1024U * 1024U;
constexpr std::string_view k_manifestPath = "CueEngineDistribution.json";

struct ProcessCapture final
{
    std::string standardOutput;
    std::string standardError;
};

struct SourcePayload final
{
    cue::distribution::SourceBlobEvidence evidence;
    std::vector<std::byte> bytes;
};

struct ExpectedTool final
{
    cue::distribution::DistributionFileRole role;
    std::string_view target;
    std::string_view path;
};

constexpr std::array k_expectedTools = {
    ExpectedTool{cue::distribution::DistributionFileRole::Bootstrap, "CueEngineBootstrap",
                 "Bin/CueEngineBootstrap.exe"},
    ExpectedTool{cue::distribution::DistributionFileRole::ProjectHub, "CueProjectHubTool",
                 "Bin/CueProjectHubTool.exe"},
    ExpectedTool{cue::distribution::DistributionFileRole::Editor, "CueEditorTool", "Bin/CueEditorTool.exe"},
    ExpectedTool{cue::distribution::DistributionFileRole::RuntimeHost, "CueRuntimeHost", "Bin/CueRuntimeHost.exe"},
    ExpectedTool{cue::distribution::DistributionFileRole::Installer, "CueEngineInstallerTool",
                 "Bin/CueEngineInstallerTool.exe"},
    ExpectedTool{cue::distribution::DistributionFileRole::InstallWorker, "CueEngineInstallWorker",
                 "Bin/CueEngineInstallWorker.exe"},
};

[[noreturn]] void terminate_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Windows Source SDK publication failed unexpectedly");
    std::abort();
}

[[nodiscard]] cue::Error make_error(const cue::AssertContext &a_assertContext,
                                    cue::distribution::DistributionError a_error,
                                    std::string_view a_summary) noexcept
{
    return cue::distribution::make_distribution_error(a_assertContext, a_error, a_summary);
}

/// @brief Operation Rootを削除し、失敗を診断可能なResultへ変換する
[[nodiscard]] cue::Result<void> cleanup_operation_root(
    const std::filesystem::path &a_operationRoot, const cue::AssertContext &a_assertContext) noexcept
{
    std::error_code error;
    static_cast<void>(std::filesystem::remove_all(a_operationRoot, error));
    if (!error)
    {
        return cue::Result<void>::success();
    }
    cue::ErrorCode code = cue::ErrorCode::create(
        a_assertContext.fatal_handler(), "Cue.Distribution",
        static_cast<std::int64_t>(cue::distribution::DistributionError::PlatformOperationFailed));
    cue::NativeError native = cue::NativeError::create(
        a_assertContext.fatal_handler(), "Win32", static_cast<std::int64_t>(error.value()));
    return cue::Result<void>::failure(cue::Error::create(
        a_assertContext.fatal_handler(), std::move(code), "Source SDK operation root cleanup failed",
        std::move(native)));
}

[[nodiscard]] std::filesystem::path native_path(std::string_view a_path)
{
    std::u8string value;
    value.reserve(a_path.size());
    for (const char byte : a_path)
    {
        value.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path(std::move(value));
}

[[nodiscard]] std::string utf8_path(const std::filesystem::path &a_path)
{
    const std::u8string value = a_path.u8string();
    return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}

[[nodiscard]] bool is_absolute_existing_file(std::string_view a_path)
{
    const std::filesystem::path path = native_path(a_path);
    std::error_code error;
    return path.is_absolute() && std::filesystem::is_regular_file(path, error) && !error;
}

[[nodiscard]] bool is_absolute_existing_directory(std::string_view a_path)
{
    const std::filesystem::path path = native_path(a_path);
    std::error_code error;
    return path.is_absolute() && std::filesystem::is_directory(path, error) && !error;
}

[[nodiscard]] bool is_path_inside(const std::filesystem::path &a_candidate,
                                  const std::filesystem::path &a_root) noexcept
{
    std::error_code error;
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(a_candidate, error);
    if (error)
    {
        return false;
    }
    const std::filesystem::path root = std::filesystem::weakly_canonical(a_root, error);
    if (error)
    {
        return false;
    }
    const std::filesystem::path relative = std::filesystem::relative(candidate, root, error);
    if (error || relative.empty() || relative.is_absolute())
    {
        return false;
    }
    const auto first = relative.begin();
    return relative == "." || (first != relative.end() && *first != "..");
}

[[nodiscard]] std::string trim_line_endings(std::string a_value)
{
    while (!a_value.empty() && (a_value.back() == '\r' || a_value.back() == '\n'))
    {
        a_value.pop_back();
    }
    return a_value;
}

[[nodiscard]] cue::Result<ProcessCapture> run_process(
    cue::ChildProcessRunner &a_runner, std::string_view a_executable, std::vector<std::string> a_arguments,
    std::string_view a_workingDirectory, const std::vector<cue::ChildProcessEnvironmentEntry> &a_environment,
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        cue::ChildProcessRequest request(std::string(a_executable), std::move(a_arguments),
                                         std::string(a_workingDirectory), a_environment, std::nullopt,
                                         k_maximumProcessOutputBytes);
        cue::ChildProcessCancellation cancellation;
        auto result = a_runner.run(request, cancellation);
        if (!result)
        {
            return cue::Result<ProcessCapture>::failure(std::move(*result.try_error()));
        }
        ProcessCapture capture;
        for (const cue::ChildProcessOutputChunk &chunk : result.try_value()->output())
        {
            std::string &destination = chunk.stream == cue::ChildProcessStream::StandardOutput
                                           ? capture.standardOutput
                                           : capture.standardError;
            destination.append(chunk.bytes);
        }
        if (result.try_value()->outcome() != cue::ChildProcessOutcome::Exited ||
            !result.try_value()->exit_code() || *result.try_value()->exit_code() != 0U)
        {
            std::string summary = "Distribution child process failed: ";
            summary.append(utf8_path(native_path(a_executable).filename()));
            if (result.try_value()->exit_code())
            {
                summary.append(" (exit code ").append(std::to_string(*result.try_value()->exit_code())).push_back(')');
            }
            const auto appendDiagnostic = [&summary](std::string_view a_label, std::string_view a_diagnostic)
            {
                constexpr std::size_t maximumDiagnosticBytes = 2048U;
                if (a_diagnostic.empty())
                {
                    return;
                }
                summary.append("; ").append(a_label).append(": ");
                const std::size_t offset = a_diagnostic.size() > maximumDiagnosticBytes
                                               ? a_diagnostic.size() - maximumDiagnosticBytes
                                               : 0U;
                for (const char value : a_diagnostic.substr(offset, maximumDiagnosticBytes))
                {
                    const unsigned char byte = static_cast<unsigned char>(value);
                    summary.push_back(byte < 0x20U || byte == 0x7fU ? ' ' : value);
                }
            };
            appendDiagnostic("stdout", capture.standardOutput);
            appendDiagnostic("stderr", capture.standardError);
            return cue::Result<ProcessCapture>::failure(
                make_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                           summary));
        }
        return cue::Result<ProcessCapture>::success(std::move(capture));
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

[[nodiscard]] cue::Result<std::string> hash_bytes(std::span<const std::byte> a_bytes,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    return cue::distribution::compute_distribution_sha256(a_bytes, a_assertContext);
}

[[nodiscard]] std::vector<std::byte> bytes_from_string(std::string_view a_bytes)
{
    const std::span<const char> characters(a_bytes.data(), a_bytes.size());
    const std::span<const std::byte> bytes = std::as_bytes(characters);
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

[[nodiscard]] cue::Result<std::vector<std::byte>> read_native_file(
    const std::filesystem::path &a_path, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(a_path, error);
        if (error || size > k_maximumPayloadBytes || size > static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)()))
        {
            return cue::Result<std::vector<std::byte>>::failure(
                make_error(a_assertContext, cue::distribution::DistributionError::ResourceLimitExceeded,
                           "Distribution payload size is invalid"));
        }
        std::ifstream stream(a_path, std::ios::binary);
        if (!stream)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                make_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                           "Distribution payload could not be opened"));
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (!bytes.empty())
        {
            stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        if (!stream || stream.peek() != std::char_traits<char>::eof())
        {
            return cue::Result<std::vector<std::byte>>::failure(
                make_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                           "Distribution payload read was incomplete"));
        }
        return cue::Result<std::vector<std::byte>>::success(std::move(bytes));
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

[[nodiscard]] cue::Result<cue::RelativePath> join_relative(const cue::RelativePath &a_root,
                                                           std::string_view a_suffix,
                                                           const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::string path(a_root.text());
        path.push_back('/');
        path.append(a_suffix);
        return cue::RelativePath::parse(path, a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

[[nodiscard]] cue::Result<void> write_payload(cue::FilesystemRoot &a_filesystem,
                                              const cue::RelativePath &a_staging,
                                              std::string_view a_relativePath,
                                              std::span<const std::byte> a_bytes,
                                              const cue::AssertContext &a_assertContext) noexcept
{
    const std::size_t separator = a_relativePath.rfind('/');
    if (separator != std::string_view::npos)
    {
        auto parent = join_relative(a_staging, a_relativePath.substr(0U, separator), a_assertContext);
        if (!parent)
        {
            return cue::Result<void>::failure(std::move(*parent.try_error()));
        }
        auto created = a_filesystem.create_directories(*parent.try_value());
        if (!created)
        {
            return created;
        }
    }
    auto path = join_relative(a_staging, a_relativePath, a_assertContext);
    return path ? a_filesystem.write_file_atomic(*path.try_value(), a_bytes)
                : cue::Result<void>::failure(std::move(*path.try_error()));
}

[[nodiscard]] cue::Result<std::vector<std::byte>> read_payload(
    cue::FilesystemRoot &a_filesystem, const cue::RelativePath &a_root, std::string_view a_relativePath,
    std::uint64_t a_expectedSize, const cue::AssertContext &a_assertContext) noexcept
{
    if (a_expectedSize > k_maximumPayloadBytes ||
        a_expectedSize > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)() - 1U))
    {
        return cue::Result<std::vector<std::byte>>::failure(
            make_error(a_assertContext, cue::distribution::DistributionError::ResourceLimitExceeded,
                       "Distribution payload size exceeds the validation limit"));
    }
    auto path = join_relative(a_root, a_relativePath, a_assertContext);
    return path ? a_filesystem.read_file(*path.try_value(), static_cast<std::size_t>(a_expectedSize) + 1U)
                : cue::Result<std::vector<std::byte>>::failure(std::move(*path.try_error()));
}

[[nodiscard]] bool is_source_candidate(std::string_view a_path) noexcept
{
    return a_path == "CMakeLists.txt" || a_path == "CMakePresets.json" || a_path == "README.md" ||
           a_path == "LICENSE.txt" || a_path.starts_with("CMake/") || a_path.starts_with("Engine/Source/") ||
           a_path.starts_with("Engine/Documents/") || a_path.starts_with("Templates/") ||
           a_path == "Tools/Dependencies/RestoreVcpkg.ps1" || a_path.starts_with("ThirdParty/");
}

[[nodiscard]] cue::Result<cue::distribution::RepositoryStateEvidence> capture_repository_state(
    cue::ChildProcessRunner &a_runner, const cue::distribution::WindowsSourceSdkPublishRequest &a_request,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto head = run_process(a_runner, a_request.gitExecutable, {"rev-parse", "--verify", "HEAD"},
                            a_request.repositoryRoot, a_request.environmentAllowlist, a_assertContext);
    auto index = run_process(a_runner, a_request.gitExecutable, {"write-tree"}, a_request.repositoryRoot,
                             a_request.environmentAllowlist, a_assertContext);
    auto status = run_process(a_runner, a_request.gitExecutable,
                              {"status", "--porcelain=v1", "-z", "--untracked-files=all"},
                              a_request.repositoryRoot, a_request.environmentAllowlist, a_assertContext);
    if (!head || !index || !status)
    {
        return cue::Result<cue::distribution::RepositoryStateEvidence>::failure(
            head ? (index ? std::move(*status.try_error()) : std::move(*index.try_error()))
                 : std::move(*head.try_error()));
    }

    const std::string revision = trim_line_endings(std::move(head.try_value()->standardOutput));
    const std::string indexTree = trim_line_endings(std::move(index.try_value()->standardOutput));
    const std::vector<std::byte> statusBytes = bytes_from_string(status.try_value()->standardOutput);
    auto statusHash = hash_bytes(statusBytes, a_assertContext);
    if (!statusHash)
    {
        return cue::Result<cue::distribution::RepositoryStateEvidence>::failure(std::move(*statusHash.try_error()));
    }
    return cue::Result<cue::distribution::RepositoryStateEvidence>::success(
        {revision, indexTree, *statusHash.try_value(), *statusHash.try_value(), statusBytes.empty()});
}

[[nodiscard]] cue::Result<std::vector<SourcePayload>> read_commit_sources(
    cue::ChildProcessRunner &a_runner, const cue::distribution::WindowsSourceSdkPublishRequest &a_request,
    std::string_view a_revision, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        auto tree = run_process(a_runner, a_request.gitExecutable,
                                {"ls-tree", "-r", "-z", "--full-tree", std::string(a_revision)},
                                a_request.repositoryRoot, a_request.environmentAllowlist, a_assertContext);
        if (!tree)
        {
            return cue::Result<std::vector<SourcePayload>>::failure(std::move(*tree.try_error()));
        }

        std::vector<SourcePayload> payloads;
        std::size_t offset = 0U;
        while (offset < tree.try_value()->standardOutput.size())
        {
            const std::size_t end = tree.try_value()->standardOutput.find('\0', offset);
            if (end == std::string::npos)
            {
                return cue::Result<std::vector<SourcePayload>>::failure(
                    make_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                               "Git tree output is truncated"));
            }
            const std::string_view record(tree.try_value()->standardOutput.data() + offset, end - offset);
            offset = end + 1U;
            const std::size_t tab = record.find('\t');
            const std::size_t firstSpace = record.find(' ');
            const std::size_t secondSpace = firstSpace == std::string_view::npos
                                                ? std::string_view::npos
                                                : record.find(' ', firstSpace + 1U);
            if (tab == std::string_view::npos || firstSpace == std::string_view::npos ||
                secondSpace == std::string_view::npos || secondSpace >= tab)
            {
                return cue::Result<std::vector<SourcePayload>>::failure(
                    make_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                               "Git tree record is invalid"));
            }
            const std::string_view mode = record.substr(0U, firstSpace);
            const std::string_view type = record.substr(firstSpace + 1U, secondSpace - firstSpace - 1U);
            const std::string_view blobId = record.substr(secondSpace + 1U, tab - secondSpace - 1U);
            const std::string_view path = record.substr(tab + 1U);

            auto role = cue::distribution::classify_distribution_source_path(path, a_assertContext);
            if (!role)
            {
                if (is_source_candidate(path))
                {
                    return cue::Result<std::vector<SourcePayload>>::failure(std::move(*role.try_error()));
                }
                continue;
            }
            if (type != "blob" || (mode != "100644" && mode != "100755") ||
                !cue::distribution::is_canonical_git_revision(blobId))
            {
                return cue::Result<std::vector<SourcePayload>>::failure(
                    make_error(a_assertContext, cue::distribution::DistributionError::ForbiddenPayload,
                               "Allowlisted source entry is not a regular Commit Blob"));
            }

            auto blob = run_process(a_runner, a_request.gitExecutable, {"cat-file", "blob", std::string(blobId)},
                                    a_request.repositoryRoot, a_request.environmentAllowlist, a_assertContext);
            if (!blob)
            {
                return cue::Result<std::vector<SourcePayload>>::failure(std::move(*blob.try_error()));
            }
            std::vector<std::byte> bytes = bytes_from_string(blob.try_value()->standardOutput);
            auto hash = hash_bytes(bytes, a_assertContext);
            if (!hash)
            {
                return cue::Result<std::vector<SourcePayload>>::failure(std::move(*hash.try_error()));
            }
            cue::distribution::SourceBlobEvidence evidence{std::string(path),
                                                           std::string(blobId),
                                                           static_cast<std::uint64_t>(bytes.size()),
                                                           *hash.try_value(),
                                                           static_cast<std::uint64_t>(bytes.size()),
                                                           *hash.try_value()};
            payloads.push_back({std::move(evidence), std::move(bytes)});
        }
        return cue::Result<std::vector<SourcePayload>>::success(std::move(payloads));
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

[[nodiscard]] const SourcePayload *find_source(std::span<const SourcePayload> a_sources,
                                               std::string_view a_path) noexcept
{
    const auto iterator = std::ranges::find(a_sources, a_path, [](const SourcePayload &a_source) noexcept
                                            { return std::string_view(a_source.evidence.relativePath); });
    return iterator == a_sources.end() ? nullptr : &*iterator;
}

[[nodiscard]] std::string text_from_bytes(std::span<const std::byte> a_bytes)
{
    return std::string(reinterpret_cast<const char *>(a_bytes.data()), a_bytes.size());
}

[[nodiscard]] cue::distribution::DependencyBuildIdentity dependency_identity(
    const cue::distribution::PublisherBuildIdentity &a_identity)
{
    return {a_identity.targetTriplet,
            a_identity.hostArchitecture,
            a_identity.targetArchitecture,
            a_identity.compilerVendor,
            a_identity.compilerVersion,
            a_identity.toolsetVersion,
            a_identity.crtLinkage,
            a_identity.crtVersion,
            a_identity.windowsSdkTargetVersion};
}

[[nodiscard]] std::string dependency_identity_json(const cue::distribution::DependencyBuildIdentity &a_identity)
{
    std::string json;
    json.reserve(512U);
    json.append("{\"targetTriplet\":\"").append(a_identity.targetTriplet);
    json.append("\",\"hostArchitecture\":\"x64\",\"targetArchitecture\":\"x64\",\"compilerVendor\":\"")
        .append(a_identity.compilerVendor);
    json.append("\",\"compilerVersion\":\"").append(a_identity.compilerVersion);
    json.append("\",\"toolsetVersion\":\"").append(a_identity.toolsetVersion);
    json.append("\",\"crtLinkage\":\"").append(a_identity.crtLinkage);
    json.append("\",\"crtVersion\":\"").append(a_identity.crtVersion);
    json.append("\",\"windowsSdkTargetVersion\":\"").append(a_identity.windowsSdkTargetVersion).append("\"}");
    return json;
}

[[nodiscard]] bool is_x64_pe(std::span<const std::byte> a_bytes) noexcept
{
    const auto read16 = [&a_bytes](std::size_t a_offset) noexcept -> std::optional<std::uint16_t>
    {
        if (a_offset + 2U > a_bytes.size())
        {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(a_bytes[a_offset]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(a_bytes[a_offset + 1U]) << 8U));
    };
    const auto read32 = [&a_bytes](std::size_t a_offset) noexcept -> std::optional<std::uint32_t>
    {
        if (a_offset + 4U > a_bytes.size())
        {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(a_bytes[a_offset]) |
               (static_cast<std::uint32_t>(a_bytes[a_offset + 1U]) << 8U) |
               (static_cast<std::uint32_t>(a_bytes[a_offset + 2U]) << 16U) |
               (static_cast<std::uint32_t>(a_bytes[a_offset + 3U]) << 24U);
    };
    const auto dos = read16(0U);
    const auto peOffset = read32(0x3cU);
    if (!dos || *dos != 0x5a4dU || !peOffset)
    {
        return false;
    }
    const auto signature = read32(*peOffset);
    const auto machine = read16(static_cast<std::size_t>(*peOffset) + 4U);
    return signature && *signature == 0x00004550U && machine && *machine == 0x8664U;
}

[[nodiscard]] cue::distribution::PublisherBuildIdentity tool_build_identity(
    cue::distribution::DistributionFileRole a_role,
    const cue::distribution::PublisherBuildIdentity &a_publisherIdentity)
{
    cue::distribution::PublisherBuildIdentity identity = a_publisherIdentity;
    if (a_role == cue::distribution::DistributionFileRole::Bootstrap ||
        a_role == cue::distribution::DistributionFileRole::InstallWorker)
    {
        identity.crtLinkage = "static";
    }
    return identity;
}

[[nodiscard]] cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>> tool_evidence_from_manifest(
    const cue::distribution::DistributionManifest &a_manifest, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::vector<cue::distribution::GeneratedToolEvidence> evidence;
        evidence.reserve(k_expectedTools.size());
        for (const ExpectedTool &tool : k_expectedTools)
        {
            const auto file = std::ranges::find(a_manifest.files, tool.path,
                                                &cue::distribution::DistributionFileEntry::relativePath);
            if (file == a_manifest.files.end() || file->role != tool.role)
            {
                return cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>>::failure(
                    make_error(a_assertContext, cue::distribution::DistributionError::MissingRequiredPayload,
                               "Published Source SDK Tool payload is missing"));
            }
            evidence.push_back({tool.role,
                                std::string(tool.target),
                                std::string(tool.path),
                                "Release",
                                a_manifest.engineSourceRevision,
                                tool_build_identity(tool.role, a_manifest.publisherBuildIdentity),
                                cue::distribution::DistributionArchitecture::X64,
                                file->byteSize,
                                file->sha256});
        }
        return cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>>::success(std::move(evidence));
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

[[nodiscard]] cue::Result<void> validate_directory_inventory(
    const std::filesystem::path &a_root, const cue::distribution::DistributionManifest &a_manifest,
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::set<std::string> expected;
        expected.emplace(k_manifestPath);
        for (const cue::distribution::DistributionFileEntry &entry : a_manifest.files)
        {
            expected.emplace(entry.relativePath);
        }

        std::set<std::string> actual;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator iterator(
                 a_root, std::filesystem::directory_options::skip_permission_denied, error),
             end;
             !error && iterator != end; iterator.increment(error))
        {
            const std::filesystem::file_status status = iterator->symlink_status(error);
            if (error)
            {
                break;
            }
            if (std::filesystem::is_directory(status))
            {
                continue;
            }
            if (!std::filesystem::is_regular_file(status))
            {
                return cue::Result<void>::failure(
                    make_error(a_assertContext, cue::distribution::DistributionError::ForbiddenPayload,
                               "Distribution Bundle contains a non-regular entry"));
            }
            const std::filesystem::path relative = std::filesystem::relative(iterator->path(), a_root, error);
            if (error)
            {
                break;
            }
            const std::string path = utf8_path(relative.generic_string());
            if (!cue::distribution::is_canonical_distribution_path(path))
            {
                return cue::Result<void>::failure(
                    make_error(a_assertContext, cue::distribution::DistributionError::InvalidPayloadPath,
                               "Distribution Bundle contains a non-canonical path"));
            }
            actual.emplace(path);
        }
        if (error || actual != expected)
        {
            return cue::Result<void>::failure(
                make_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                           "Distribution Bundle file set differs from the Manifest"));
        }
        return cue::Result<void>::success();
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

[[nodiscard]] cue::Result<void> validate_inventory(cue::FilesystemRoot &a_filesystem,
                                                   const cue::RelativePath &a_root,
                                                   const cue::distribution::DistributionManifest &a_manifest,
                                                   const cue::AssertContext &a_assertContext) noexcept
{
    for (const cue::distribution::DistributionFileEntry &entry : a_manifest.files)
    {
        auto bytes = read_payload(a_filesystem, a_root, entry.relativePath, entry.byteSize, a_assertContext);
        if (!bytes || bytes.try_value()->size() != entry.byteSize)
        {
            return cue::Result<void>::failure(
                bytes ? make_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                                   "Distribution payload size differs from the Manifest")
                      : std::move(*bytes.try_error()));
        }
        auto hash = hash_bytes(*bytes.try_value(), a_assertContext);
        if (!hash || *hash.try_value() != entry.sha256)
        {
            return cue::Result<void>::failure(
                hash ? make_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                                  "Distribution payload hash differs from the Manifest")
                     : std::move(*hash.try_error()));
        }
    }

    auto manifestPath = join_relative(a_root, k_manifestPath, a_assertContext);
    auto manifestBytes = manifestPath ? a_filesystem.read_file(*manifestPath.try_value(), 16U * 1024U * 1024U)
                                      : cue::Result<std::vector<std::byte>>::failure(
                                            std::move(*manifestPath.try_error()));
    if (!manifestBytes)
    {
        return cue::Result<void>::failure(std::move(*manifestBytes.try_error()));
    }
    auto parsed = cue::distribution::read_distribution_manifest(text_from_bytes(*manifestBytes.try_value()),
                                                                a_assertContext);
    if (!parsed || *parsed.try_value() != a_manifest)
    {
        return cue::Result<void>::failure(
            parsed ? make_error(a_assertContext, cue::distribution::DistributionError::BundleValidationFailed,
                                "Distribution Manifest differs from the expected identity")
                   : std::move(*parsed.try_error()));
    }
    return cue::Result<void>::success();
}

[[nodiscard]] cue::Result<void> validate_generated_tools(
    cue::FilesystemRoot &a_filesystem, const cue::RelativePath &a_root,
    const cue::distribution::DistributionManifest &a_manifest,
    const cue::AssertContext &a_assertContext) noexcept
{
    for (const ExpectedTool &tool : k_expectedTools)
    {
        const auto file = std::ranges::find(a_manifest.files, tool.path,
                                            &cue::distribution::DistributionFileEntry::relativePath);
        if (file == a_manifest.files.end() || file->role != tool.role)
        {
            return cue::Result<void>::failure(
                make_error(a_assertContext, cue::distribution::DistributionError::MissingRequiredPayload,
                           "Distribution Tool payload is missing from the Manifest"));
        }
        auto bytes = read_payload(a_filesystem, a_root, tool.path, file->byteSize, a_assertContext);
        if (!bytes || bytes.try_value()->size() != file->byteSize || !is_x64_pe(*bytes.try_value()))
        {
            return cue::Result<void>::failure(
                bytes ? make_error(a_assertContext, cue::distribution::DistributionError::InvalidGeneratedTool,
                                   "Distribution Tool is not the expected x64 PE payload")
                      : std::move(*bytes.try_error()));
        }
    }
    return cue::Result<void>::success();
}

[[nodiscard]] cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>> build_release_tools(
    cue::ChildProcessRunner &a_runner, cue::FilesystemRoot &a_filesystem, const cue::RelativePath &a_staging,
    const std::filesystem::path &a_stagingAbsolute, const std::filesystem::path &a_buildRoot,
    const std::filesystem::path &a_dependencyRoot,
    const cue::distribution::WindowsSourceSdkPublishRequest &a_request,
    const cue::distribution::PublisherBuildIdentity &a_identity,
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::error_code error;
        if (!std::filesystem::create_directories(a_buildRoot, error) || error)
        {
            return cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>>::failure(
                make_error(a_assertContext, cue::distribution::DistributionError::PlatformOperationFailed,
                           "Isolated Distribution build root could not be created"));
        }

        const std::filesystem::path toolRoot = a_dependencyRoot / "Tool" / "vcpkg";
        const std::filesystem::path installRoot = a_dependencyRoot / "Installed";
        const std::string generatorPlatform = "x64,version=" + a_identity.windowsSdkTargetVersion;
        const std::string generatorToolset = "host=x64,version=" + a_identity.toolsetVersion;
        auto configured = run_process(
            a_runner, a_request.cmakeExecutable,
            {"-S",
             utf8_path(a_stagingAbsolute),
             "-B",
             utf8_path(a_buildRoot),
             "-G",
             a_request.cmakeGenerator,
             "-A",
             generatorPlatform,
             "-T",
             generatorToolset,
             "-DBUILD_TESTING=OFF",
             "-DCMAKE_TOOLCHAIN_FILE=" + utf8_path(a_stagingAbsolute / "CMake" / "CueVcpkgToolchain.cmake"),
             "-DCUE_VCPKG_ROOT=" + utf8_path(toolRoot),
             "-DVCPKG_INSTALLED_DIR=" + utf8_path(installRoot),
             "-DVCPKG_MANIFEST_DIR=" + utf8_path(a_stagingAbsolute / "ThirdParty"),
             "-DVCPKG_MANIFEST_INSTALL=OFF",
             "-DCUE_ENGINE_INSTALLED_VERSION_ROOT=" + utf8_path(a_stagingAbsolute)},
            utf8_path(a_stagingAbsolute), a_request.environmentAllowlist, a_assertContext);
        if (!configured)
        {
            return cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>>::failure(
                std::move(*configured.try_error()));
        }

        std::vector<std::string> buildArguments = {"--build", utf8_path(a_buildRoot), "--config", "Release", "--target"};
        for (const ExpectedTool &tool : k_expectedTools)
        {
            buildArguments.emplace_back(tool.target);
        }
        auto built = run_process(a_runner, a_request.cmakeExecutable, std::move(buildArguments),
                                 utf8_path(a_stagingAbsolute), a_request.environmentAllowlist, a_assertContext);
        if (!built)
        {
            return cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>>::failure(
                std::move(*built.try_error()));
        }

        std::vector<cue::distribution::GeneratedToolEvidence> evidence;
        evidence.reserve(k_expectedTools.size());
        for (const ExpectedTool &tool : k_expectedTools)
        {
            const std::filesystem::path binary = a_buildRoot / "bin" / "Release" /
                                                 std::filesystem::path(std::string(tool.path.substr(4U)));
            auto bytes = read_native_file(binary, a_assertContext);
            if (!bytes)
            {
                return cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>>::failure(
                    std::move(*bytes.try_error()));
            }
            if (!is_x64_pe(*bytes.try_value()))
            {
                return cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>>::failure(
                    make_error(a_assertContext, cue::distribution::DistributionError::InvalidGeneratedTool,
                               "Generated Distribution Tool is not an x64 PE image"));
            }
            auto hash = hash_bytes(*bytes.try_value(), a_assertContext);
            if (!hash)
            {
                return cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>>::failure(
                    std::move(*hash.try_error()));
            }
            auto written = write_payload(a_filesystem, a_staging, tool.path, *bytes.try_value(), a_assertContext);
            if (!written)
            {
                return cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>>::failure(
                    std::move(*written.try_error()));
            }
            evidence.push_back({tool.role,
                                std::string(tool.target),
                                std::string(tool.path),
                                "Release",
                                a_identity.builtFromRevision,
                                tool_build_identity(tool.role, a_identity),
                                cue::distribution::DistributionArchitecture::X64,
                                static_cast<std::uint64_t>(bytes.try_value()->size()),
                                *hash.try_value()});
        }
        return cue::Result<std::vector<cue::distribution::GeneratedToolEvidence>>::success(std::move(evidence));
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}
} // namespace

namespace cue::distribution
{
Result<SourceSdkPublishReport> publish_windows_source_sdk(const WindowsSourceSdkPublishRequest &a_request,
                                                          const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_absolute_existing_directory(a_request.repositoryRoot) ||
            !is_absolute_existing_directory(a_request.destinationParent) ||
            !is_absolute_existing_directory(a_request.operationsRoot) ||
            !is_absolute_existing_directory(a_request.dependenciesParent) ||
            !is_absolute_existing_file(a_request.gitExecutable) ||
            !is_absolute_existing_file(a_request.cmakeExecutable) ||
            !is_absolute_existing_file(a_request.powershellExecutable) || a_request.cmakeGenerator.empty() ||
            !is_canonical_engine_version(a_request.engineVersion) || !is_canonical_bundle_id(a_request.bundleId) ||
            a_request.publisherBuildIdentity.configuration != "Release")
        {
            return Result<SourceSdkPublishReport>::failure(
                make_error(a_assertContext, DistributionError::InvalidIdentity,
                           "Windows Source SDK Publisher input is invalid"));
        }
        const std::filesystem::path repositoryRoot = native_path(a_request.repositoryRoot);
        if (is_path_inside(native_path(a_request.destinationParent), repositoryRoot) ||
            is_path_inside(native_path(a_request.operationsRoot), repositoryRoot) ||
            is_path_inside(native_path(a_request.dependenciesParent), repositoryRoot))
        {
            return Result<SourceSdkPublishReport>::failure(
                make_error(a_assertContext, DistributionError::InvalidPayloadPath,
                           "Distribution output, operation, and dependency roots must remain outside the Repository"));
        }

        auto runner = create_windows_child_process_runner(a_assertContext);
        if (!runner)
        {
            return Result<SourceSdkPublishReport>::failure(std::move(*runner.try_error()));
        }
        auto started = capture_repository_state(**runner.try_value(), a_request, a_assertContext);
        if (!started)
        {
            return Result<SourceSdkPublishReport>::failure(std::move(*started.try_error()));
        }
        if (!started.try_value()->clean)
        {
            return Result<SourceSdkPublishReport>::failure(
                make_error(a_assertContext, DistributionError::DirtyRepository,
                           "Source SDK publication requires a clean Repository"));
        }

        PublisherBuildIdentity publisherIdentity = a_request.publisherBuildIdentity;
        publisherIdentity.builtFromRevision = started.try_value()->headRevision;
        if (!is_valid_publisher_build_identity(publisherIdentity))
        {
            return Result<SourceSdkPublishReport>::failure(
                make_error(a_assertContext, DistributionError::InvalidIdentity,
                           "Source SDK Publisher Build Identity is invalid"));
        }

        auto sources = read_commit_sources(**runner.try_value(), a_request, started.try_value()->headRevision,
                                           a_assertContext);
        if (!sources)
        {
            return Result<SourceSdkPublishReport>::failure(std::move(*sources.try_error()));
        }
        const SourcePayload *manifestDefinition = find_source(*sources.try_value(), "ThirdParty/vcpkg.json");
        const SourcePayload *configurationDefinition =
            find_source(*sources.try_value(), "ThirdParty/vcpkg-configuration.json");
        const SourcePayload *toolDefinition = find_source(*sources.try_value(), "ThirdParty/vcpkg-tool.json");
        if (manifestDefinition == nullptr || configurationDefinition == nullptr || toolDefinition == nullptr)
        {
            return Result<SourceSdkPublishReport>::failure(
                make_error(a_assertContext, DistributionError::MissingRequiredPayload,
                           "Dependency definition payload is missing from the fixed Commit"));
        }
        auto definitionId = make_dependency_definition_id(text_from_bytes(manifestDefinition->bytes),
                                                          text_from_bytes(configurationDefinition->bytes),
                                                          text_from_bytes(toolDefinition->bytes), a_assertContext);
        const DependencyBuildIdentity dependencyBuild = dependency_identity(publisherIdentity);
        auto dependencyRootId = definitionId ? make_dependency_root_id(*definitionId.try_value(), dependencyBuild,
                                                                       a_assertContext)
                                             : Result<std::string>::failure(std::move(*definitionId.try_error()));
        if (!definitionId || !dependencyRootId)
        {
            return Result<SourceSdkPublishReport>::failure(
                definitionId ? std::move(*dependencyRootId.try_error()) : std::move(*definitionId.try_error()));
        }

        std::vector<SourceBlobEvidence> sourceEvidence;
        sourceEvidence.reserve(sources.try_value()->size());
        for (const SourcePayload &source : *sources.try_value())
        {
            sourceEvidence.push_back(source.evidence);
        }

        std::string destinationName = "CueEngine-";
        destinationName.append(a_request.engineVersion).append("-windows-x64");
        auto destination = RelativePath::parse(destinationName, a_assertContext);
        auto filesystem = create_windows_filesystem_root(a_request.destinationParent, a_assertContext);
        if (!destination || !filesystem)
        {
            return Result<SourceSdkPublishReport>::failure(
                destination ? std::move(*filesystem.try_error()) : std::move(*destination.try_error()));
        }
        auto destinationType = filesystem.try_value()->get()->query_entry(*destination.try_value());
        if (!destinationType)
        {
            return Result<SourceSdkPublishReport>::failure(std::move(*destinationType.try_error()));
        }
        if (*destinationType.try_value() != EntryType::Missing)
        {
            if (*destinationType.try_value() != EntryType::Directory)
            {
                return Result<SourceSdkPublishReport>::failure(
                    make_error(a_assertContext, DistributionError::PublishConflict,
                               "Source SDK destination conflicts with a non-directory entry"));
            }
            auto existingManifestPath = join_relative(*destination.try_value(), k_manifestPath, a_assertContext);
            auto existingManifestBytes =
                existingManifestPath
                    ? filesystem.try_value()->get()->read_file(*existingManifestPath.try_value(),
                                                               16U * 1024U * 1024U)
                    : Result<std::vector<std::byte>>::failure(std::move(*existingManifestPath.try_error()));
            auto existingManifest =
                existingManifestBytes
                    ? read_distribution_manifest(text_from_bytes(*existingManifestBytes.try_value()), a_assertContext)
                    : Result<DistributionManifest>::failure(std::move(*existingManifestBytes.try_error()));
            if (!existingManifest || existingManifest.try_value()->bundleId != a_request.bundleId ||
                existingManifest.try_value()->engineVersion != a_request.engineVersion ||
                existingManifest.try_value()->engineSourceRevision != started.try_value()->headRevision ||
                existingManifest.try_value()->dependencyDefinitionId != *definitionId.try_value() ||
                existingManifest.try_value()->publisherBuildIdentity != publisherIdentity ||
                existingManifest.try_value()->minimumToolchain != a_request.minimumToolchain)
            {
                return Result<SourceSdkPublishReport>::failure(
                    existingManifest ? make_error(a_assertContext, DistributionError::PublishConflict,
                                                  "Existing Source SDK identity differs from this publication")
                                     : std::move(*existingManifest.try_error()));
            }
            auto existingTools = tool_evidence_from_manifest(*existingManifest.try_value(), a_assertContext);
            auto expectedInventory =
                existingTools ? make_distribution_publisher_inventory(sourceEvidence, *existingTools.try_value(),
                                                                       started.try_value()->headRevision,
                                                                       publisherIdentity, a_assertContext)
                              : Result<DistributionPublisherInventory>::failure(
                                    std::move(*existingTools.try_error()));
            if (!existingTools || !expectedInventory ||
                expectedInventory.try_value()->sourceInventoryHash != existingManifest.try_value()->sourceInventoryHash ||
                expectedInventory.try_value()->files != existingManifest.try_value()->files)
            {
                return Result<SourceSdkPublishReport>::failure(
                    expectedInventory ? make_error(a_assertContext, DistributionError::PublishConflict,
                                                   "Existing Source SDK Inventory differs from the fixed Commit")
                                      : std::move(*expectedInventory.try_error()));
            }
            auto inventoryValid = validate_inventory(**filesystem.try_value(), *destination.try_value(),
                                                     *existingManifest.try_value(), a_assertContext);
            auto directoryValid = validate_directory_inventory(
                native_path(a_request.destinationParent) / native_path(destinationName),
                *existingManifest.try_value(), a_assertContext);
            if (!inventoryValid || !directoryValid)
            {
                return Result<SourceSdkPublishReport>::failure(
                    inventoryValid ? std::move(*directoryValid.try_error()) : std::move(*inventoryValid.try_error()));
            }
            auto toolsValid = validate_generated_tools(**filesystem.try_value(), *destination.try_value(),
                                                       *existingManifest.try_value(), a_assertContext);
            if (!toolsValid)
            {
                return Result<SourceSdkPublishReport>::failure(std::move(*toolsValid.try_error()));
            }
            auto completed = capture_repository_state(**runner.try_value(), a_request, a_assertContext);
            auto repositoryValid = completed ? validate_repository_evidence(*started.try_value(),
                                                                            *completed.try_value(), a_assertContext)
                                             : Result<void>::failure(std::move(*completed.try_error()));
            if (!repositoryValid)
            {
                return Result<SourceSdkPublishReport>::failure(std::move(*repositoryValid.try_error()));
            }
            return Result<SourceSdkPublishReport>::success(
                {SourceSdkPublishStage::Completed,
                 utf8_path(native_path(a_request.destinationParent) / native_path(destinationName)),
                 std::move(*existingManifest.try_value()),
                 true});
        }

        auto stagingResult = filesystem.try_value()->get()->create_staging_area(*destination.try_value());
        if (!stagingResult)
        {
            return Result<SourceSdkPublishReport>::failure(std::move(*stagingResult.try_error()));
        }
        StagingArea staging = std::move(*stagingResult.try_value());
        const std::filesystem::path stagingAbsolute = native_path(a_request.destinationParent) /
                                                      native_path(staging.path().text());
        const std::vector<std::byte> operationSeed = bytes_from_string(staging.path().text());
        auto operationDigest = hash_bytes(operationSeed, a_assertContext);
        if (!operationDigest)
        {
            auto rollback = filesystem.try_value()->get()->rollback_staging_area(std::move(staging));
            if (!rollback)
            {
                operationDigest.try_error()->append_secondary_diagnostics(
                    a_assertContext, *rollback.try_error(), "Source SDK staging rollback failed", "Rollback");
            }
            return Result<SourceSdkPublishReport>::failure(std::move(*operationDigest.try_error()));
        }
        const std::string operationName = "op-" + operationDigest.try_value()->substr(0U, 16U);
        const std::filesystem::path operationRoot = native_path(a_request.operationsRoot) /
                                                    native_path(operationName);

        const auto failStaging = [&](Error a_error) -> Result<SourceSdkPublishReport>
        {
            auto rollback = filesystem.try_value()->get()->rollback_staging_area(std::move(staging));
            if (!rollback)
            {
                a_error.append_secondary_diagnostics(a_assertContext, *rollback.try_error(),
                                                     "Source SDK staging rollback failed", "Rollback");
            }
            auto operationCleanup = cleanup_operation_root(operationRoot, a_assertContext);
            if (!operationCleanup)
            {
                a_error.append_secondary_diagnostics(a_assertContext, *operationCleanup.try_error(),
                                                     "Source SDK operation root cleanup failed", "Cleanup");
            }
            return Result<SourceSdkPublishReport>::failure(std::move(a_error));
        };

        for (std::size_t sourceIndex = 0U; sourceIndex < sources.try_value()->size(); ++sourceIndex)
        {
            SourcePayload &source = (*sources.try_value())[sourceIndex];
            auto written = write_payload(**filesystem.try_value(), staging.path(), source.evidence.relativePath,
                                         source.bytes, a_assertContext);
            if (!written)
            {
                return failStaging(std::move(*written.try_error()));
            }
            auto staged = read_payload(**filesystem.try_value(), staging.path(), source.evidence.relativePath,
                                       source.evidence.commitByteSize, a_assertContext);
            if (!staged)
            {
                return failStaging(std::move(*staged.try_error()));
            }
            auto stagedHash = hash_bytes(*staged.try_value(), a_assertContext);
            if (!stagedHash)
            {
                return failStaging(std::move(*stagedHash.try_error()));
            }
            source.evidence.stagedByteSize = staged.try_value()->size();
            source.evidence.stagedSha256 = std::move(*stagedHash.try_value());
            sourceEvidence[sourceIndex] = source.evidence;
        }

        const std::filesystem::path dependencyRoot = native_path(a_request.dependenciesParent) /
                                                     native_path(*dependencyRootId.try_value());
        auto restored = run_process(
            **runner.try_value(), a_request.powershellExecutable,
            {"-NoProfile",
             "-File",
             utf8_path(stagingAbsolute / "Tools" / "Dependencies" / "RestoreVcpkg.ps1"),
             "-ToolRoot",
             utf8_path(dependencyRoot / "Tool" / "vcpkg"),
             "-InstallRoot",
             utf8_path(dependencyRoot / "Installed"),
             "-GitExecutable",
             a_request.gitExecutable,
             "-InstalledVersionRoot",
             utf8_path(stagingAbsolute),
             "-DependencyRootId",
             *dependencyRootId.try_value(),
             "-DependencyDefinitionId",
             *definitionId.try_value(),
             "-DependencyBuildIdentityJson",
             dependency_identity_json(dependencyBuild)},
            utf8_path(stagingAbsolute), a_request.environmentAllowlist, a_assertContext);
        if (!restored)
        {
            return failStaging(std::move(*restored.try_error()));
        }

        auto tools = build_release_tools(**runner.try_value(), **filesystem.try_value(), staging.path(),
                                         stagingAbsolute, operationRoot / "Build", dependencyRoot, a_request,
                                         publisherIdentity, a_assertContext);
        if (!tools)
        {
            return failStaging(std::move(*tools.try_error()));
        }

        auto inventory = make_distribution_publisher_inventory(sourceEvidence, *tools.try_value(),
                                                                started.try_value()->headRevision, publisherIdentity,
                                                                a_assertContext);
        if (!inventory)
        {
            return failStaging(std::move(*inventory.try_error()));
        }

        DistributionManifest manifest;
        manifest.bundleId = a_request.bundleId;
        manifest.engineVersion = a_request.engineVersion;
        manifest.engineSourceRevision = started.try_value()->headRevision;
        manifest.sourceInventoryHash = inventory.try_value()->sourceInventoryHash;
        manifest.dependencyDefinitionId = *definitionId.try_value();
        manifest.publisherBuildIdentity = publisherIdentity;
        manifest.minimumToolchain = a_request.minimumToolchain;
        manifest.entryPoints = {"Bin/CueEngineBootstrap.exe",
                                "Bin/CueProjectHubTool.exe",
                                "Bin/CueEditorTool.exe",
                                "Bin/CueRuntimeHost.exe",
                                "Bin/CueEngineInstallerTool.exe",
                                "Bin/CueEngineInstallWorker.exe"};
        manifest.files = std::move(inventory.try_value()->files);

        auto manifestBytes = write_distribution_manifest(manifest, a_assertContext);
        if (!manifestBytes)
        {
            return failStaging(std::move(*manifestBytes.try_error()));
        }
        const std::vector<std::byte> manifestPayload = bytes_from_string(*manifestBytes.try_value());
        auto manifestWritten = write_payload(**filesystem.try_value(), staging.path(), k_manifestPath,
                                             manifestPayload, a_assertContext);
        if (!manifestWritten)
        {
            return failStaging(std::move(*manifestWritten.try_error()));
        }

        auto stagingValid = validate_inventory(**filesystem.try_value(), staging.path(), manifest, a_assertContext);
        auto stagingDirectoryValid = validate_directory_inventory(stagingAbsolute, manifest, a_assertContext);
        auto stagingToolsValid = validate_generated_tools(**filesystem.try_value(), staging.path(), manifest,
                                                          a_assertContext);
        auto completed = capture_repository_state(**runner.try_value(), a_request, a_assertContext);
        auto repositoryValid = completed ? validate_repository_evidence(*started.try_value(), *completed.try_value(),
                                                                        a_assertContext)
                                         : Result<void>::failure(std::move(*completed.try_error()));
        if (!stagingValid || !stagingDirectoryValid || !stagingToolsValid || !repositoryValid)
        {
            return failStaging(!stagingValid            ? std::move(*stagingValid.try_error())
                               : !stagingDirectoryValid ? std::move(*stagingDirectoryValid.try_error())
                               : !stagingToolsValid     ? std::move(*stagingToolsValid.try_error())
                                                        : std::move(*repositoryValid.try_error()));
        }

        auto published = filesystem.try_value()->get()->publish_staging_area(std::move(staging),
                                                                             *destination.try_value());
        if (!published)
        {
            return failStaging(std::move(*published.try_error()));
        }
        auto publishedValid = validate_inventory(**filesystem.try_value(), *destination.try_value(), manifest,
                                                 a_assertContext);
        auto publishedDirectoryValid = validate_directory_inventory(
            native_path(a_request.destinationParent) / native_path(destinationName), manifest, a_assertContext);
        auto publishedToolsValid = validate_generated_tools(**filesystem.try_value(), *destination.try_value(),
                                                            manifest, a_assertContext);
        auto operationCleanup = cleanup_operation_root(operationRoot, a_assertContext);
        if (!publishedValid || !publishedDirectoryValid || !publishedToolsValid)
        {
            Error publicationError =
                !publishedValid            ? std::move(*publishedValid.try_error())
                : !publishedDirectoryValid ? std::move(*publishedDirectoryValid.try_error())
                                             : std::move(*publishedToolsValid.try_error());
            if (!operationCleanup)
            {
                publicationError.append_secondary_diagnostics(
                    a_assertContext, *operationCleanup.try_error(), "Source SDK operation root cleanup failed",
                    "Cleanup");
            }
            return Result<SourceSdkPublishReport>::failure(std::move(publicationError));
        }
        if (!operationCleanup)
        {
            return Result<SourceSdkPublishReport>::failure(std::move(*operationCleanup.try_error()));
        }
        return Result<SourceSdkPublishReport>::success(
            {SourceSdkPublishStage::Completed,
             utf8_path(native_path(a_request.destinationParent) / native_path(destinationName)),
             std::move(manifest),
             false});
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}
} // namespace cue::distribution
