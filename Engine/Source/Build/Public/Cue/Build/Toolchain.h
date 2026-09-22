#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;

/// @brief Project Build前に検出・検証するTool種別
enum class BuildToolKind : std::uint8_t
{
    CMake,
    VisualStudio,
    MsvcCompiler,
    WindowsSdk
};

/// @brief Build ToolまたはHostのArchitecture
enum class BuildArchitecture : std::uint8_t
{
    Unknown,
    X64,
    X86,
    Arm64
};

/// @brief Game ProjectとHostを混在させずBuildするConfiguration
enum class BuildConfiguration : std::uint8_t
{
    Debug,
    Development,
    Release
};

/// @brief Build Tool Versionを比較可能な4要素で保持する
struct BuildToolVersion final
{
    std::uint32_t major = 0U;
    std::uint32_t minor = 0U;
    std::uint32_t patch = 0U;
    std::uint32_t build = 0U;

    /// @brief 全Version要素を辞書順で比較する
    [[nodiscard]] auto operator<=>(const BuildToolVersion &) const noexcept = default;
};

/// @brief 一つのToolに許容する半開Version範囲とArchitectureを保持する
struct BuildToolRequirement final
{
    BuildToolKind kind = BuildToolKind::CMake;
    BuildToolVersion minimumVersion;
    BuildToolVersion maximumVersionExclusive;
    BuildArchitecture architecture = BuildArchitecture::X64;
};

/// @brief 検出元が観測したTool候補を実行せずに保持する
struct BuildToolCandidate final
{
    BuildToolKind kind = BuildToolKind::CMake;
    std::string nativePath;
    std::string installationRoot;
    std::optional<BuildToolVersion> version;
    BuildArchitecture architecture = BuildArchitecture::Unknown;
    bool available = false;
};

/// @brief Engine Build MetadataとHostから収集した検証前Inventory
struct BuildEnvironmentInventory final
{
    BuildArchitecture hostArchitecture = BuildArchitecture::Unknown;
    std::vector<BuildToolCandidate> candidates;
    std::string engineSourceRoot;
    std::string engineBinaryRoot;
    bool engineSourceAvailable = false;
    bool engineBinaryAvailable = false;
};

/// @brief Engineが許容するToolchainとHostの契約
struct BuildEnvironmentRequirements final
{
    BuildArchitecture hostArchitecture = BuildArchitecture::X64;
    std::vector<BuildToolRequirement> tools;
    std::vector<BuildConfiguration> supportedConfigurations;
    bool requiresEngineBinary = true;
};

/// @brief 検出結果全体または個別問題の対応可否
enum class BuildEnvironmentSupport : std::uint8_t
{
    Supported,
    Unsupported,
    Unknown
};

/// @brief User向け修復Hintへ安定して対応付ける診断種別
enum class BuildEnvironmentDiagnosticCode : std::uint8_t
{
    UnsupportedHostArchitecture,
    MissingTool,
    UnknownToolIdentity,
    UnsupportedTool,
    AmbiguousTool,
    MissingEngineSource,
    MissingEngineBinary
};

/// @brief Tool実行前Validationの一件の診断
struct BuildEnvironmentDiagnostic final
{
    BuildEnvironmentDiagnosticCode code = BuildEnvironmentDiagnosticCode::MissingTool;
    BuildEnvironmentSupport support = BuildEnvironmentSupport::Unknown;
    std::optional<BuildToolKind> tool;
    std::string nativePath;
    std::string summary;
    std::string repairHint;
};

/// @brief 実行可能と確定したTool選択または修復可能な診断を返すReport
struct BuildEnvironmentReport final
{
    BuildEnvironmentSupport support = BuildEnvironmentSupport::Unknown;
    std::vector<BuildToolCandidate> selectedTools;
    std::vector<BuildEnvironmentDiagnostic> diagnostics;
    std::vector<BuildConfiguration> supportedConfigurations;
    std::string engineSourceRoot;
    std::string engineBinaryRoot;
};

/// @brief 候補InventoryをEngine要件と照合し、Toolを実行せずに選択または診断する
///
/// 全引数は呼出中だけ借用し、返却Reportは必要な値を所有して参照を保持しない。同じ入力Objectを変更しない独立呼出しは並行可能。
/// Allocation等の予期しない例外はAssertContextのFatalHandlerでProcessを終了し、境界外へ例外を送出しない。
[[nodiscard]] BuildEnvironmentReport validate_build_environment(const BuildEnvironmentInventory &a_inventory,
                                                                const BuildEnvironmentRequirements &a_requirements,
                                                                const AssertContext &a_assertContext) noexcept;

/// @brief Native Pathを改行や制御文字でLog構造を壊さない引用済みUTF-8へ変換する
///
/// 引数は呼出中だけ借用し、返却文字列は全Byteを所有する。独立した呼出しは並行可能。Allocation失敗はFatalHandlerへ渡す。
[[nodiscard]] std::string format_native_path_for_log(std::string_view a_nativePath,
                                                     const AssertContext &a_assertContext) noexcept;

/// @brief 各Argumentを個別にEscapeし、実行用文字列と混同しないLog専用Command Lineへ変換する
///
/// Spanと各文字列は呼出中だけ借用し、返却文字列は全Byteを所有する。独立した呼出しは並行可能。Allocation失敗はFatalHandlerへ渡す。
[[nodiscard]] std::string format_command_line_for_log(std::span<const std::string_view> a_arguments,
                                                      const AssertContext &a_assertContext) noexcept;
} // namespace cue
