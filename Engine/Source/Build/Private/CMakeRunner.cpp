#include <Cue/Build/CMakeRunner.h>

#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace
{
/// @brief Windows Unicode Environment Blockの終端を含む最大Code Unit数
constexpr std::size_t k_maxWindowsEnvironmentLength = 32767U;

/// @brief CMake Runner処理中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_runner_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("CMake runner failed unexpectedly");
    std::abort();
}

/// @brief CMake Runner固有Errorを生成する
[[nodiscard]] cue::Error make_runner_error(const cue::AssertContext &a_assertContext, cue::CMakeRunnerError a_code,
                                           std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.CMakeRunner",
                                                 static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Environment名がWindows Process境界と同じ比較を行えるASCIIだけで構成されるか検証する
[[nodiscard]] bool is_ascii_environment_name(std::string_view a_name) noexcept
{
    return std::ranges::all_of(a_name, [](unsigned char a_value) { return a_value <= 0x7FU; });
}

/// @brief ASCII Environment名をWindows規則のCase-insensitive比較する
[[nodiscard]] bool equals_environment_name(std::string_view a_left, std::string_view a_right) noexcept
{
    if (a_left.size() != a_right.size())
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_left.size(); ++index)
    {
        const unsigned char left = static_cast<unsigned char>(a_left[index]);
        const unsigned char right = static_cast<unsigned char>(a_right[index]);
        const unsigned char foldedLeft =
            left >= 'a' && left <= 'z' ? static_cast<unsigned char>(left - 'a' + 'A') : left;
        const unsigned char foldedRight =
            right >= 'a' && right <= 'z' ? static_cast<unsigned char>(right - 'a' + 'A') : right;
        if (foldedLeft != foldedRight)
        {
            return false;
        }
    }
    return true;
}

/// @brief Strict UTF-8 TextのWindows UTF-16 Code Unit数を検証して返す
[[nodiscard]] std::optional<std::size_t> windows_utf16_length(std::string_view a_text) noexcept
{
    std::size_t codeUnits = 0U;
    for (std::size_t index = 0U; index < a_text.size();)
    {
        const auto first = static_cast<unsigned char>(a_text[index]);
        std::size_t length = 0U;
        std::size_t scalarCodeUnits = 1U;
        if (first < 0x80U)
        {
            length = 1U;
        }
        else if (first >= 0xC2U && first <= 0xDFU)
        {
            length = 2U;
        }
        else if (first >= 0xE0U && first <= 0xEFU)
        {
            length = 3U;
        }
        else if (first >= 0xF0U && first <= 0xF4U)
        {
            length = 4U;
            scalarCodeUnits = 2U;
        }
        else
        {
            return std::nullopt;
        }
        if (index + length > a_text.size())
        {
            return std::nullopt;
        }
        if (length > 1U)
        {
            const auto second = static_cast<unsigned char>(a_text[index + 1U]);
            if ((second & 0xC0U) != 0x80U || (first == 0xE0U && second < 0xA0U) ||
                (first == 0xEDU && second >= 0xA0U) || (first == 0xF0U && second < 0x90U) ||
                (first == 0xF4U && second >= 0x90U))
            {
                return std::nullopt;
            }
            for (std::size_t offset = 2U; offset < length; ++offset)
            {
                if ((static_cast<unsigned char>(a_text[index + offset]) & 0xC0U) != 0x80U)
                {
                    return std::nullopt;
                }
            }
        }
        codeUnits += scalarCodeUnits;
        if (codeUnits > k_maxWindowsEnvironmentLength)
        {
            return std::nullopt;
        }
        index += length;
    }
    return codeUnits;
}

/// @brief UTF-8 Environment EntryをWindows Block長へ加算できるか検証する
[[nodiscard]] bool append_environment_entry_length(std::string_view a_name, std::string_view a_value,
                                                   std::size_t &a_blockLength) noexcept
{
    const std::optional<std::size_t> nameLength = windows_utf16_length(a_name);
    const std::optional<std::size_t> valueLength = windows_utf16_length(a_value);
    if (!nameLength || !valueLength)
    {
        return false;
    }
    const std::size_t entryLength = *nameLength + *valueLength + 2U;
    if (entryLength > k_maxWindowsEnvironmentLength - a_blockLength)
    {
        return false;
    }
    a_blockLength += entryLength;
    return true;
}

/// @brief CMake起動に使うUTF-8 Pathが埋め込みNULのないAbsolute Pathか検証する
[[nodiscard]] bool is_absolute_utf8_path(std::string_view a_path)
{
    if (a_path.empty() || a_path.find('\0') != std::string_view::npos)
    {
        return false;
    }
    std::u8string encoded;
    encoded.reserve(a_path.size());
    for (const unsigned char value : a_path)
    {
        encoded.push_back(static_cast<char8_t>(value));
    }
    try
    {
        return std::filesystem::path(encoded).is_absolute();
    }
    catch (const std::filesystem::filesystem_error &)
    {
        return false;
    }
}

/// @brief CMakeとMSBuildへ渡せる2から4要素のVisual Studio Toolset Versionか検証する
[[nodiscard]] bool is_visual_studio_toolset_version(std::string_view a_version) noexcept
{
    if (a_version.size() < 3U || a_version.size() > 48U || a_version.front() == '.' || a_version.back() == '.')
    {
        return false;
    }
    std::size_t componentCount = 1U;
    bool previousWasDot = false;
    for (const char value : a_version)
    {
        if (value == '.')
        {
            if (previousWasDot)
            {
                return false;
            }
            previousWasDot = true;
            ++componentCount;
        }
        else if (value >= '0' && value <= '9')
        {
            previousWasDot = false;
        }
        else
        {
            return false;
        }
    }
    return componentCount >= 2U && componentCount <= 4U;
}

/// @brief 設定値、Absolute Path、予約Environment名の衝突を検証する
[[nodiscard]] bool valid_settings(const cue::CMakeRunnerSettings &a_settings)
{
    if (!is_absolute_utf8_path(a_settings.cmakeExecutable) || !is_absolute_utf8_path(a_settings.engineSourceRoot) ||
        !is_visual_studio_toolset_version(a_settings.visualStudioToolsetVersion) ||
        (a_settings.configureTimeout && a_settings.configureTimeout->count() <= 0) ||
        (a_settings.buildTimeout && a_settings.buildTimeout->count() <= 0))
    {
        return false;
    }
    std::size_t environmentBlockLength = 1U;
    for (std::size_t index = 0U; index < a_settings.environmentAllowlist.size(); ++index)
    {
        const cue::ChildProcessEnvironmentEntry &entry = a_settings.environmentAllowlist[index];
        if (entry.name.empty() || !is_ascii_environment_name(entry.name) || entry.name.find('=') != std::string::npos ||
            entry.name.find('\0') != std::string::npos || entry.value.find('\0') != std::string::npos ||
            equals_environment_name(entry.name, "CUE_ENGINE_ROOT"))
        {
            return false;
        }
        if (!append_environment_entry_length(entry.name, entry.value, environmentBlockLength))
        {
            return false;
        }
        for (std::size_t previous = 0U; previous < index; ++previous)
        {
            if (equals_environment_name(a_settings.environmentAllowlist[previous].name, entry.name))
            {
                return false;
            }
        }
    }
    return append_environment_entry_length("CUE_ENGINE_ROOT", a_settings.engineSourceRoot, environmentBlockLength);
}

/// @brief Build ConfigurationをCMakeのmulti-config構成名へ変換する
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

/// @brief Child Process結果をBuild Stage結果へ変換する
[[nodiscard]] cue::Result<cue::BuildStageResult> make_stage_result(cue::BuildStage a_stage,
                                                                   const cue::ChildProcessResult &a_processResult,
                                                                   const cue::AssertContext &a_assertContext) noexcept
{
    switch (a_processResult.outcome())
    {
    case cue::ChildProcessOutcome::Exited:
        if (!a_processResult.exit_code())
        {
            return cue::Result<cue::BuildStageResult>::failure(make_runner_error(
                a_assertContext, cue::CMakeRunnerError::ProcessExecutionFailed, "CMake process exit code is missing"));
        }
        return cue::BuildStageResult::create(a_stage,
                                             *a_processResult.exit_code() == 0U ? cue::BuildStageOutcome::Succeeded
                                                                                : cue::BuildStageOutcome::Failed,
                                             a_processResult.exit_code(), a_assertContext);
    case cue::ChildProcessOutcome::Cancelled:
        return cue::BuildStageResult::create(a_stage, cue::BuildStageOutcome::Cancelled, std::nullopt, a_assertContext);
    case cue::ChildProcessOutcome::TimedOut:
        return cue::BuildStageResult::create(a_stage, cue::BuildStageOutcome::TimedOut, std::nullopt, a_assertContext);
    }
    return cue::Result<cue::BuildStageResult>::failure(make_runner_error(
        a_assertContext, cue::CMakeRunnerError::ProcessExecutionFailed, "CMake process outcome is invalid"));
}

/// @brief 一StageをArgument Vectorで実行してRecordを返す
[[nodiscard]] cue::Result<cue::CMakeStageRecord> run_stage(
    cue::BuildStage a_stage, std::vector<std::string> a_arguments, std::optional<std::chrono::milliseconds> a_timeout,
    const cue::BuildPlan &a_plan, const cue::CMakeRunnerSettings &a_settings, cue::ChildProcessRunner &a_processRunner,
    const cue::ChildProcessCancellation &a_cancellation, cue::CMakeStageObserver &a_observer,
    const cue::AssertContext &a_assertContext)
{
    std::vector<cue::ChildProcessEnvironmentEntry> environment = a_settings.environmentAllowlist;
    environment.push_back({"CUE_ENGINE_ROOT", a_settings.engineSourceRoot});
    cue::ChildProcessRequest request(a_settings.cmakeExecutable, std::move(a_arguments),
                                     std::string(a_plan.project_root()), std::move(environment), a_timeout);
    a_observer.on_stage_started(a_stage);
    auto process = a_processRunner.run(request, a_cancellation);
    if (!process)
    {
        return cue::Result<cue::CMakeStageRecord>::failure(std::move(*process.try_error()));
    }
    auto stageResult = make_stage_result(a_stage, *process.try_value(), a_assertContext);
    if (!stageResult)
    {
        return cue::Result<cue::CMakeStageRecord>::failure(std::move(*stageResult.try_error()));
    }
    cue::CMakeStageRecord record{std::move(*stageResult.try_value()), process.try_value()->output()};
    a_observer.on_stage_completed(record);
    return cue::Result<cue::CMakeStageRecord>::success(std::move(record));
}
} // namespace

namespace cue
{
CMakeBuildResult::CMakeBuildResult(std::vector<CMakeStageRecord> a_stages) noexcept : m_stages(std::move(a_stages))
{
}

std::span<const CMakeStageRecord> CMakeBuildResult::stages() const noexcept
{
    return m_stages;
}

bool CMakeBuildResult::succeeded() const noexcept
{
    return !m_stages.empty() && m_stages.back().result.stage() == BuildStage::Build &&
           m_stages.back().result.outcome() == BuildStageOutcome::Succeeded;
}

Result<CMakeBuildResult> run_cmake_build(const BuildPlan &a_plan, const CMakeRunnerSettings &a_settings,
                                         CMakeConfigureMode a_configureMode, ChildProcessRunner &a_processRunner,
                                         const ChildProcessCancellation &a_cancellation, CMakeStageObserver &a_observer,
                                         const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!valid_settings(a_settings) || (a_configureMode != CMakeConfigureMode::Required &&
                                            a_configureMode != CMakeConfigureMode::ReuseCompatibleTree))
        {
            return Result<CMakeBuildResult>::failure(make_runner_error(
                a_assertContext, CMakeRunnerError::InvalidSettings, "CMake runner settings are invalid"));
        }

        std::vector<CMakeStageRecord> stages;
        stages.reserve(a_configureMode == CMakeConfigureMode::Required ? 2U : 1U);
        if (a_configureMode == CMakeConfigureMode::Required)
        {
            std::vector<std::string> configureArguments = {"--preset", std::string(a_plan.preset_name()), "-B",
                                                           std::string(a_plan.binary_directory()), "-T",
                                                           "version=" + a_settings.visualStudioToolsetVersion};
            auto configured =
                run_stage(BuildStage::Configure, std::move(configureArguments), a_settings.configureTimeout, a_plan,
                          a_settings, a_processRunner, a_cancellation, a_observer, a_assertContext);
            if (!configured)
            {
                return Result<CMakeBuildResult>::failure(std::move(*configured.try_error()));
            }
            stages.push_back(std::move(*configured.try_value()));
            if (stages.back().result.outcome() != BuildStageOutcome::Succeeded)
            {
                return Result<CMakeBuildResult>::success(CMakeBuildResult(std::move(stages)));
            }
        }

        std::vector<std::string> buildArguments = {
            "--build",  std::string(a_plan.binary_directory()),
            "--config", std::string(configuration_name(a_plan.profile().configuration())),
            "--target", std::string(a_plan.cmake_target_name()),
        };
        if (a_settings.buildsRuntimeHost && a_plan.profile().target() == BuildTarget::GameModule)
        {
            buildArguments.push_back("CueRuntimeHostForProject");
        }
        buildArguments.push_back("--");
        buildArguments.push_back("/p:VCToolsVersion=" + a_settings.visualStudioToolsetVersion);
        auto built = run_stage(BuildStage::Build, std::move(buildArguments), a_settings.buildTimeout, a_plan,
                               a_settings, a_processRunner, a_cancellation, a_observer, a_assertContext);
        if (!built)
        {
            return Result<CMakeBuildResult>::failure(std::move(*built.try_error()));
        }
        stages.push_back(std::move(*built.try_value()));
        return Result<CMakeBuildResult>::success(CMakeBuildResult(std::move(stages)));
    }
    catch (...)
    {
        terminate_runner_exception(a_assertContext);
    }
}
} // namespace cue
