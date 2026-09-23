#include <Cue/Build/DiagnosticBundle.h>

#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace
{
constexpr std::size_t k_hardMaximumFileCount = 64U;
constexpr std::uint64_t k_hardMaximumFileBytes = 64U * 1024U * 1024U;
constexpr std::uint64_t k_hardMaximumTotalBytes = 256U * 1024U * 1024U;
constexpr std::size_t k_hardMaximumPathMappings = 1024U;
constexpr std::size_t k_maximumArtifactFiles = 128U;
constexpr std::uint64_t k_maximumArtifactByteSize = 9007199254740991ULL;
constexpr std::array<std::string_view, 8U> k_bundlePaths = {"artifact.json", "environment.json", "manifest.json",
                                                            "plan.json",     "result.json",      "stages.json",
                                                            "stderr.log",    "stdout.log"};
constexpr std::array<std::string_view, 7U> k_manifestEntryPaths = {
    "artifact.json", "environment.json", "plan.json", "result.json", "stages.json", "stderr.log", "stdout.log"};

/// @brief 回復不能なBundle内部例外をFatalHandlerへ通知してProcessを停止する
[[noreturn]] void terminate_bundle_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Build diagnostic bundle failed unexpectedly");
    std::abort();
}

/// @brief Bundle固有の回復可能Errorを一貫したDomainで構築する
[[nodiscard]] cue::Error make_bundle_error(const cue::AssertContext &a_assertContext,
                                           cue::BuildDiagnosticBundleError a_code, std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.DiagnosticBundle",
                                                 static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief UUID検証で許可するlowercase hexadecimal文字か判定する
[[nodiscard]] bool is_lower_hex(char a_value) noexcept
{
    return (a_value >= '0' && a_value <= '9') || (a_value >= 'a' && a_value <= 'f');
}

/// @brief Operation IDがlowercase UUID v4のCanonical形式か検証する
[[nodiscard]] bool is_uuid_v4(std::string_view a_text) noexcept
{
    if (a_text.size() != 36U || a_text[8U] != '-' || a_text[13U] != '-' || a_text[18U] != '-' || a_text[23U] != '-' ||
        a_text[14U] != '4' || (a_text[19U] != '8' && a_text[19U] != '9' && a_text[19U] != 'a' && a_text[19U] != 'b'))
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_text.size(); ++index)
    {
        if (index != 8U && index != 13U && index != 18U && index != 23U && !is_lower_hex(a_text[index]))
        {
            return false;
        }
    }
    return true;
}

/// @brief Byte列がOverlong、Surrogate、範囲外Scalarを含まないStrict UTF-8か検証する
[[nodiscard]] bool valid_utf8_bytes(const unsigned char *a_bytes, std::size_t a_size) noexcept
{
    for (std::size_t index = 0U; index < a_size;)
    {
        const unsigned char first = a_bytes[index];
        std::size_t length = 0U;
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
        }
        else
        {
            return false;
        }
        if (length > a_size - index)
        {
            return false;
        }
        if (length > 1U)
        {
            const unsigned char second = a_bytes[index + 1U];
            if ((second & 0xC0U) != 0x80U || (first == 0xE0U && second < 0xA0U) ||
                (first == 0xEDU && second >= 0xA0U) || (first == 0xF0U && second < 0x90U) ||
                (first == 0xF4U && second >= 0x90U))
            {
                return false;
            }
            for (std::size_t offset = 2U; offset < length; ++offset)
            {
                if ((a_bytes[index + offset] & 0xC0U) != 0x80U)
                {
                    return false;
                }
            }
        }
        index += length;
    }
    return true;
}

/// @brief String ViewをStrict UTF-8として検証する
[[nodiscard]] bool valid_utf8(std::string_view a_text) noexcept
{
    return valid_utf8_bytes(reinterpret_cast<const unsigned char *>(a_text.data()), a_text.size());
}

/// @brief Bundle Byte ViewをStrict UTF-8として検証する
[[nodiscard]] bool valid_utf8(std::span<const std::byte> a_bytes) noexcept
{
    return valid_utf8_bytes(reinterpret_cast<const unsigned char *>(a_bytes.data()), a_bytes.size());
}

/// @brief Bundle FileがBOMなしStrict UTF-8、LF改行、末尾改行のText契約を満たすか検証する
[[nodiscard]] bool valid_bundle_text(std::span<const std::byte> a_bytes) noexcept
{
    constexpr std::array<std::byte, 3U> byteOrderMark = {std::byte{0xEFU}, std::byte{0xBBU}, std::byte{0xBFU}};
    return !a_bytes.empty() && a_bytes.back() == std::byte{'\n'} && valid_utf8(a_bytes) &&
           (a_bytes.size() < byteOrderMark.size() ||
            !std::equal(byteOrderMark.begin(), byteOrderMark.end(), a_bytes.begin())) &&
           std::find(a_bytes.begin(), a_bytes.end(), std::byte{'\r'}) == a_bytes.end();
}

/// @brief User指定上限が正数かつHard Limit内に収まるか検証する
[[nodiscard]] bool valid_limits(const cue::BuildDiagnosticBundleLimits &a_limits) noexcept
{
    return a_limits.maximumFileCount > 0U && a_limits.maximumFileCount <= k_hardMaximumFileCount &&
           a_limits.maximumFileBytes > 0U && a_limits.maximumFileBytes <= k_hardMaximumFileBytes &&
           a_limits.maximumTotalBytes >= a_limits.maximumFileBytes &&
           a_limits.maximumTotalBytes <= k_hardMaximumTotalBytes && a_limits.maximumPathMappings > 0U &&
           a_limits.maximumPathMappings <= k_hardMaximumPathMappings;
}

/// @brief Artifact Root外参照とWindowsで危険な要素を含まない相対Pathか検証する
[[nodiscard]] bool is_safe_artifact_relative_path(std::string_view a_path) noexcept
{
    if (a_path.empty() || a_path.front() == '/' || a_path.back() == '/' ||
        a_path.find('\\') != std::string_view::npos || a_path.find('\0') != std::string_view::npos)
    {
        return false;
    }
    std::size_t begin = 0U;
    while (begin < a_path.size())
    {
        const std::size_t end = a_path.find('/', begin);
        const std::string_view component =
            a_path.substr(begin, end == std::string_view::npos ? a_path.size() - begin : end - begin);
        if (component.empty() || component == "." || component == ".." || component.back() == ' ' ||
            component.back() == '.')
        {
            return false;
        }
        for (const unsigned char value : component)
        {
            if (value < 0x20U || value == ':' || value == '*' || value == '?' || value == '"' || value == '<' ||
                value == '>' || value == '|')
            {
                return false;
            }
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        begin = end + 1U;
    }
    return true;
}

/// @brief Windows上で同じArtifact PathとなるASCII大小文字違いを検出する
[[nodiscard]] bool artifact_paths_equal(std::string_view a_left, std::string_view a_right) noexcept
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
            left >= 'A' && left <= 'Z' ? static_cast<unsigned char>(left - 'A' + 'a') : left;
        const unsigned char foldedRight =
            right >= 'A' && right <= 'Z' ? static_cast<unsigned char>(right - 'A' + 'a') : right;
        if (foldedLeft != foldedRight)
        {
            return false;
        }
    }
    return true;
}

/// @brief Artifact Content Hashがlowercase SHA-256文字列表現か検証する
[[nodiscard]] bool valid_artifact_hash(std::string_view a_hash) noexcept
{
    return a_hash.size() == 64U && std::all_of(a_hash.begin(), a_hash.end(), is_lower_hex);
}

/// @brief Bundle Schemaが許可する固定File Pathか判定する
[[nodiscard]] bool known_bundle_path(std::string_view a_path) noexcept
{
    return std::find(k_bundlePaths.begin(), k_bundlePaths.end(), a_path) != k_bundlePaths.end();
}

/// @brief 公開境界のUTF-8文字列を埋め込みNULなしのNative Filesystem Pathへ変換する
[[nodiscard]] std::optional<std::filesystem::path> filesystem_path_from_utf8(std::string_view a_text)
{
    if (a_text.empty() || a_text.find('\0') != std::string_view::npos)
    {
        return std::nullopt;
    }
    std::u8string encoded;
    encoded.reserve(a_text.size());
    for (const unsigned char value : a_text)
    {
        encoded.push_back(static_cast<char8_t>(value));
    }
    try
    {
        return std::filesystem::path(encoded);
    }
    catch (const std::filesystem::filesystem_error &)
    {
        return std::nullopt;
    }
}

/// @brief Windows Native検査用にAbsolute PathをExtended-length形式へ変換する
[[nodiscard]] std::filesystem::path native_inspection_path(const std::filesystem::path &a_path)
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
        std::wstring extended = L"\\\\?\\UNC\\";
        extended.append(native.substr(2U));
        return std::filesystem::path(std::move(extended));
    }
    std::wstring extended = L"\\\\?\\";
    extended.append(native);
    return std::filesystem::path(std::move(extended));
#else
    return a_path;
#endif
}

/// @brief Native Filesystem Entryが追跡禁止のReparse Pointか属性で検証する
[[nodiscard]] bool is_reparse_point(const std::filesystem::path &a_path, std::error_code &a_error) noexcept
{
    a_error.clear();
#if defined(_WIN32)
    const std::filesystem::path inspectionPath = native_inspection_path(a_path);
    const DWORD attributes = GetFileAttributesW(inspectionPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        a_error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
#else
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(native_inspection_path(a_path), a_error);
    return !a_error && std::filesystem::is_symlink(status);
#endif
}

/// @brief 任意Byte列をControl文字を含め妥当なJSON StringへEscapeして追記する
void append_json_string(std::string &a_output, std::string_view a_value)
{
    constexpr char hexDigits[] = "0123456789ABCDEF";
    a_output.push_back('"');
    for (const unsigned char value : a_value)
    {
        switch (value)
        {
        case '"':
            a_output.append("\\\"");
            break;
        case '\\':
            a_output.append("\\\\");
            break;
        case '\n':
            a_output.append("\\n");
            break;
        case '\r':
            a_output.append("\\r");
            break;
        case '\t':
            a_output.append("\\t");
            break;
        default:
            if (value < 0x20U || value == 0x7FU)
            {
                a_output.append("\\u00");
                a_output.push_back(hexDigits[(value >> 4U) & 0x0FU]);
                a_output.push_back(hexDigits[value & 0x0FU]);
            }
            else
            {
                a_output.push_back(static_cast<char>(value));
            }
            break;
        }
    }
    a_output.push_back('"');
}

class BoundedTextBuilder final
{
  public:
    /// @brief 最大Byte数を超えない文字列Builderを初期化する
    explicit BoundedTextBuilder(std::size_t a_maximumBytes) noexcept : m_maximumBytes(a_maximumBytes)
    {
    }

    /// @brief 残り上限内なら文字列を追記する
    [[nodiscard]] bool append(std::string_view a_value)
    {
        if (a_value.size() > remaining())
        {
            m_exceeded = true;
            return false;
        }
        m_text.append(a_value);
        return true;
    }

    /// @brief 残り上限内なら一文字を追記する
    [[nodiscard]] bool push_back(char a_value)
    {
        if (remaining() == 0U)
        {
            m_exceeded = true;
            return false;
        }
        m_text.push_back(a_value);
        return true;
    }

    /// @brief 残り上限内でJSON StringをEscapeして追記する
    [[nodiscard]] bool append_json_string(std::string_view a_value)
    {
        constexpr char hexDigits[] = "0123456789ABCDEF";
        if (!push_back('"'))
        {
            return false;
        }
        for (const unsigned char value : a_value)
        {
            bool appended = false;
            switch (value)
            {
            case '"':
                appended = append("\\\"");
                break;
            case '\\':
                appended = append("\\\\");
                break;
            case '\n':
                appended = append("\\n");
                break;
            case '\r':
                appended = append("\\r");
                break;
            case '\t':
                appended = append("\\t");
                break;
            default:
                if (value < 0x20U || value == 0x7FU)
                {
                    appended = append("\\u00") && push_back(hexDigits[(value >> 4U) & 0x0FU]) &&
                               push_back(hexDigits[value & 0x0FU]);
                }
                else
                {
                    appended = push_back(static_cast<char>(value));
                }
                break;
            }
            if (!appended)
            {
                return false;
            }
        }
        return push_back('"');
    }

    /// @brief 現在残っている追記可能Byte数を返す
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return m_text.size() <= m_maximumBytes ? m_maximumBytes - m_text.size() : 0U;
    }

    /// @brief 上限内で完成した文字列だけを返す
    [[nodiscard]] std::optional<std::string> finish() &&
    {
        if (m_exceeded)
        {
            return std::nullopt;
        }
        return std::move(m_text);
    }

  private:
    std::string m_text;
    std::size_t m_maximumBytes = 0U;
    bool m_exceeded = false;
};

/// @brief ASCII英大文字だけを小文字へ正規化する
[[nodiscard]] unsigned char fold_ascii(unsigned char a_value) noexcept
{
    return a_value >= 'A' && a_value <= 'Z' ? static_cast<unsigned char>(a_value - 'A' + 'a') : a_value;
}

/// @brief ASCII大小文字とPath Separator表記を区別せず指定Offset以降のPath位置を検索する
[[nodiscard]] std::size_t find_path(std::string_view a_text, std::string_view a_pattern, std::size_t a_offset) noexcept
{
    if (a_pattern.empty() || a_pattern.size() > a_text.size())
    {
        return std::string_view::npos;
    }
    for (std::size_t begin = a_offset; begin + a_pattern.size() <= a_text.size(); ++begin)
    {
        bool equal = true;
        for (std::size_t index = 0U; index < a_pattern.size(); ++index)
        {
            const unsigned char textValue = static_cast<unsigned char>(a_text[begin + index]);
            const unsigned char patternValue = static_cast<unsigned char>(a_pattern[index]);
            const bool textSeparator = textValue == '/' || textValue == '\\';
            const bool patternSeparator = patternValue == '/' || patternValue == '\\';
            equal = equal && ((textSeparator && patternSeparator) || fold_ascii(textValue) == fold_ascii(patternValue));
        }
        if (equal)
        {
            return begin;
        }
    }
    return std::string_view::npos;
}

/// @brief Sensitive Path Prefixを大小文字とSeparator表現を保ったままTokenへ置換する
void replace_path(std::string &a_text, std::string_view a_prefix, std::string_view a_replacement)
{
    std::size_t offset = 0U;
    while (true)
    {
        const std::size_t found = find_path(a_text, a_prefix, offset);
        if (found == std::string::npos)
        {
            return;
        }
        a_text.replace(found, a_prefix.size(), a_replacement);
        offset = found + a_replacement.size();
    }
}

/// @brief 置換後も指定上限内に収まることを確認してSensitive PathをToken化する
[[nodiscard]] bool replace_path_bounded(std::string &a_text, std::string_view a_prefix, std::string_view a_replacement,
                                        std::size_t a_maximumBytes)
{
    std::size_t count = 0U;
    std::size_t offset = 0U;
    while (true)
    {
        const std::size_t found = find_path(a_text, a_prefix, offset);
        if (found == std::string_view::npos)
        {
            break;
        }
        ++count;
        offset = found + a_prefix.size();
    }
    if (a_replacement.size() > a_prefix.size())
    {
        const std::size_t growth = a_replacement.size() - a_prefix.size();
        if (a_text.size() > a_maximumBytes || count > (a_maximumBytes - a_text.size()) / growth)
        {
            return false;
        }
    }
    replace_path(a_text, a_prefix, a_replacement);
    return a_text.size() <= a_maximumBytes;
}

/// @brief Path Redaction規則がBoundedで安全なToken形式か検証する
[[nodiscard]] bool valid_mapping(const cue::BuildDiagnosticPathMapping &a_mapping) noexcept
{
    return !a_mapping.nativePrefix.empty() && a_mapping.nativePrefix.size() <= 4096U &&
           a_mapping.nativePrefix.find('\0') == std::string::npos && a_mapping.replacement.size() >= 3U &&
           a_mapping.replacement.size() <= 64U && a_mapping.replacement.front() == '<' &&
           a_mapping.replacement.back() == '>' &&
           a_mapping.replacement.find_first_of("/\\\r\n\t") == std::string::npos &&
           a_mapping.replacement.find('\0') == std::string::npos;
}

/// @brief 借用中のNative Prefixを所有文字列へCopyする前に上限と制御文字を検証する
[[nodiscard]] bool valid_native_mapping_prefix(std::string_view a_prefix) noexcept
{
    return a_prefix.size() <= 4096U && a_prefix.find('\0') == std::string_view::npos;
}

/// @brief 空Prefixと重複を除外しBoundedなRedaction規則だけを追加する
[[nodiscard]] bool add_mapping(std::vector<cue::BuildDiagnosticPathMapping> &a_mappings, std::string_view a_prefix,
                               std::string_view a_replacement)
{
    if (a_prefix.empty())
    {
        return true;
    }
    if (!valid_native_mapping_prefix(a_prefix))
    {
        return false;
    }
    const auto found =
        std::find_if(a_mappings.begin(), a_mappings.end(),
                     /// @brief 同じNative Prefixを持つ既存Mappingを検出する
                     [a_prefix](const auto &a_mapping) noexcept { return a_mapping.nativePrefix == a_prefix; });
    if (found == a_mappings.end())
    {
        a_mappings.push_back({std::string(a_prefix), std::string(a_replacement)});
    }
    return true;
}

/// @brief Version 1で永続化できるTool種別か検証する
[[nodiscard]] bool valid_tool_kind(cue::BuildToolKind a_kind) noexcept
{
    return a_kind >= cue::BuildToolKind::CMake && a_kind <= cue::BuildToolKind::Git;
}

/// @brief Version 1で永続化できる対応可否か検証する
[[nodiscard]] bool valid_environment_support(cue::BuildEnvironmentSupport a_support) noexcept
{
    return a_support == cue::BuildEnvironmentSupport::Supported ||
           a_support == cue::BuildEnvironmentSupport::Unsupported || a_support == cue::BuildEnvironmentSupport::Unknown;
}

/// @brief Version 1で永続化できるTool Architectureか検証する
[[nodiscard]] bool valid_build_architecture(cue::BuildArchitecture a_architecture) noexcept
{
    return a_architecture == cue::BuildArchitecture::Unknown || a_architecture == cue::BuildArchitecture::X64 ||
           a_architecture == cue::BuildArchitecture::X86 || a_architecture == cue::BuildArchitecture::Arm64;
}

/// @brief Version 1で永続化できるBuild Configurationか検証する
[[nodiscard]] bool valid_build_configuration(cue::BuildConfiguration a_configuration) noexcept
{
    return a_configuration == cue::BuildConfiguration::Debug ||
           a_configuration == cue::BuildConfiguration::Development ||
           a_configuration == cue::BuildConfiguration::Release;
}

/// @brief Version 1で永続化できるEnvironment診断種別か検証する
[[nodiscard]] bool valid_environment_diagnostic_code(cue::BuildEnvironmentDiagnosticCode a_code) noexcept
{
    return a_code >= cue::BuildEnvironmentDiagnosticCode::UnsupportedHostArchitecture &&
           a_code <= cue::BuildEnvironmentDiagnosticCode::MissingEngineBinary;
}

/// @brief Environment Report内の数値EnumがVersion 1の既知値だけか検証する
[[nodiscard]] bool valid_environment_enumerations(const cue::BuildEnvironmentReport &a_environment) noexcept
{
    if (!valid_environment_support(a_environment.support))
    {
        return false;
    }
    for (const cue::BuildToolCandidate &tool : a_environment.selectedTools)
    {
        if (!valid_tool_kind(tool.kind) || !valid_build_architecture(tool.architecture))
        {
            return false;
        }
    }
    for (const cue::BuildEnvironmentDiagnostic &diagnostic : a_environment.diagnostics)
    {
        if (!valid_environment_diagnostic_code(diagnostic.code) || !valid_environment_support(diagnostic.support) ||
            (diagnostic.tool && !valid_tool_kind(*diagnostic.tool)))
        {
            return false;
        }
    }
    return std::all_of(a_environment.supportedConfigurations.begin(), a_environment.supportedConfigurations.end(),
                       valid_build_configuration);
}

/// @brief Diagnostic Bundleへ永続化できる終端Build Operation Stateか検証する
[[nodiscard]] bool valid_terminal_state(cue::GameBuildOperationState a_state) noexcept
{
    return a_state == cue::GameBuildOperationState::Succeeded || a_state == cue::GameBuildOperationState::Failed ||
           a_state == cue::GameBuildOperationState::Cancelled || a_state == cue::GameBuildOperationState::TimedOut;
}

/// @brief 終端Stateと現在Operationが公開したArtifactの有無がService契約と一致するか検証する
[[nodiscard]] bool valid_operation_artifact_state(const cue::BuildOperationSnapshot &a_operation) noexcept
{
    return (a_operation.state == cue::GameBuildOperationState::Succeeded) == a_operation.artifact.has_value();
}

/// @brief Stage、Outcome、Exit CodeがBuildStageResult契約と一致するか検証する
[[nodiscard]] bool valid_stage_snapshot(const cue::BuildStageSnapshot &a_stage) noexcept
{
    const bool validStage = a_stage.stage == cue::BuildStage::Configure || a_stage.stage == cue::BuildStage::Build;
    const bool validOutcome =
        a_stage.outcome == cue::BuildStageOutcome::Succeeded || a_stage.outcome == cue::BuildStageOutcome::Failed ||
        a_stage.outcome == cue::BuildStageOutcome::Cancelled || a_stage.outcome == cue::BuildStageOutcome::TimedOut;
    const bool exited =
        a_stage.outcome == cue::BuildStageOutcome::Succeeded || a_stage.outcome == cue::BuildStageOutcome::Failed;
    return validStage && validOutcome && exited == a_stage.exitCode.has_value() &&
           (a_stage.outcome != cue::BuildStageOutcome::Succeeded || a_stage.exitCode == 0U) &&
           (a_stage.outcome != cue::BuildStageOutcome::Failed || a_stage.exitCode != 0U);
}

/// @brief 成功OperationがBuild Stage成功まで完了したSnapshotか検証する
[[nodiscard]] bool valid_succeeded_operation_stages(const cue::BuildOperationSnapshot &a_operation) noexcept
{
    if (a_operation.state != cue::GameBuildOperationState::Succeeded)
    {
        return true;
    }
    return !a_operation.stages.empty() && a_operation.stages.back().stage == cue::BuildStage::Build &&
           a_operation.stages.back().outcome == cue::BuildStageOutcome::Succeeded;
}

/// @brief 現在Operationへ属するLogのStream種別を検証する
[[nodiscard]] bool valid_operation_logs(const cue::BuildOperationSnapshot &a_operation) noexcept
{
    for (const cue::BuildLogSnapshot &log : a_operation.logs)
    {
        const bool knownStream = log.stream == cue::ChildProcessStream::StandardOutput ||
                                 log.stream == cue::ChildProcessStream::StandardError;
        if (log.operationId == a_operation.operationId && !knownStream)
        {
            return false;
        }
    }
    return true;
}

/// @brief 入力Copyと中間置換を含め指定上限を超えない場合だけSensitive PathをToken化する
[[nodiscard]] std::optional<std::string> redact_bounded(std::string_view a_text,
                                                        const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings,
                                                        std::size_t a_maximumBytes)
{
    if (a_text.size() > a_maximumBytes)
    {
        return std::nullopt;
    }
    std::string redacted(a_text);
    for (const cue::BuildDiagnosticPathMapping &mapping : a_mappings)
    {
        if (!replace_path_bounded(redacted, mapping.nativePrefix, mapping.replacement, a_maximumBytes))
        {
            return std::nullopt;
        }
    }
    return redacted;
}

/// @brief Build Operation Stateを永続化用の安定文字列へ変換する
[[nodiscard]] const char *state_text(cue::GameBuildOperationState a_state) noexcept
{
    switch (a_state)
    {
    case cue::GameBuildOperationState::Idle:
        return "idle";
    case cue::GameBuildOperationState::Running:
        return "running";
    case cue::GameBuildOperationState::Succeeded:
        return "succeeded";
    case cue::GameBuildOperationState::Failed:
        return "failed";
    case cue::GameBuildOperationState::Cancelled:
        return "cancelled";
    case cue::GameBuildOperationState::TimedOut:
        return "timedOut";
    }
    return "unknown";
}

/// @brief 永続化済みState文字列をBuild Operation Stateへ検証変換する
[[nodiscard]] std::optional<cue::GameBuildOperationState> parse_state(std::string_view a_value) noexcept
{
    constexpr std::array states = {cue::GameBuildOperationState::Succeeded, cue::GameBuildOperationState::Failed,
                                   cue::GameBuildOperationState::Cancelled, cue::GameBuildOperationState::TimedOut};
    for (const cue::GameBuildOperationState state : states)
    {
        if (a_value == state_text(state))
        {
            return state;
        }
    }
    return std::nullopt;
}

/// @brief Build Stageを永続化用の安定文字列へ変換する
[[nodiscard]] const char *stage_text(cue::BuildStage a_stage) noexcept
{
    return a_stage == cue::BuildStage::Configure ? "configure" : "build";
}

/// @brief Build Stage Outcomeを永続化用の安定文字列へ変換する
[[nodiscard]] const char *outcome_text(cue::BuildStageOutcome a_outcome) noexcept
{
    switch (a_outcome)
    {
    case cue::BuildStageOutcome::Succeeded:
        return "succeeded";
    case cue::BuildStageOutcome::Failed:
        return "failed";
    case cue::BuildStageOutcome::Cancelled:
        return "cancelled";
    case cue::BuildStageOutcome::TimedOut:
        return "timedOut";
    }
    return "unknown";
}

/// @brief Build Configurationを永続化用の安定文字列へ変換する
[[nodiscard]] const char *configuration_text(cue::BuildConfiguration a_configuration) noexcept
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
    return "Unknown";
}

/// @brief Build Architectureを永続化用の安定文字列へ変換する
[[nodiscard]] const char *architecture_text(cue::BuildArchitecture a_architecture) noexcept
{
    switch (a_architecture)
    {
    case cue::BuildArchitecture::Unknown:
        return "unknown";
    case cue::BuildArchitecture::X64:
        return "x64";
    case cue::BuildArchitecture::X86:
        return "x86";
    case cue::BuildArchitecture::Arm64:
        return "arm64";
    }
    return "unknown";
}

/// @brief 所有文字列を内容を変えずBundle用Byte列へ変換する
[[nodiscard]] std::vector<std::byte> to_bytes(std::string a_text)
{
    std::vector<std::byte> bytes(a_text.size());
    std::transform(a_text.begin(), a_text.end(), bytes.begin(),
                   /// @brief 一文字を同値のUnsigned Byteへ変換する
                   [](char a_value) noexcept { return static_cast<std::byte>(static_cast<unsigned char>(a_value)); });
    return bytes;
}

/// @brief Bundle Byte列を内容を変えず検証用文字列へ変換する
[[nodiscard]] std::string from_bytes(std::span<const std::byte> a_bytes)
{
    std::string text(a_bytes.size(), '\0');
    std::transform(a_bytes.begin(), a_bytes.end(), text.begin(),
                   /// @brief 一Byteを同値の文字表現へ変換する
                   [](std::byte a_value) noexcept
                   { return static_cast<char>(std::to_integer<unsigned char>(a_value)); });
    return text;
}

/// @brief Redaction後の値を残り上限内でJSON Stringとして追記する
[[nodiscard]] bool append_redacted_json(BoundedTextBuilder &a_output, std::string_view a_value,
                                        const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings)
{
    std::optional<std::string> redacted = redact_bounded(a_value, a_mappings, a_output.remaining());
    return redacted && a_output.append_json_string(*redacted);
}

/// @brief Build Plan Snapshotを上限内のRedact済みJSONへSerializeする
[[nodiscard]] std::optional<std::string> serialize_plan(const cue::BuildDiagnosticPlanSnapshot &a_plan,
                                                        const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings,
                                                        std::size_t a_maximumBytes)
{
    BoundedTextBuilder output(a_maximumBytes);
    if (!output.append("{\n\"schemaVersion\":1,\n"))
    {
        return std::nullopt;
    }
    /// @brief Plan FieldをRedactしてJSON Memberとして追記する
    const auto add = [&output, &a_mappings](std::string_view a_name, const std::string &a_value, bool a_last)
    {
        return output.append_json_string(a_name) && output.push_back(':') &&
               append_redacted_json(output, a_value, a_mappings) && output.append(a_last ? "\n" : ",\n");
    };
    if (!add("projectRoot", a_plan.projectRoot, false) || !add("presetName", a_plan.presetName, false) ||
        !add("workspaceKey", a_plan.workspaceKey, false) || !add("binaryDirectory", a_plan.binaryDirectory, false) ||
        !add("candidateDirectory", a_plan.candidateDirectory, false) ||
        !add("operationDirectory", a_plan.operationDirectory, false) ||
        !add("artifactStoreDirectory", a_plan.artifactStoreDirectory, false) ||
        !add("targetName", a_plan.targetName, true) || !output.append("}\n"))
    {
        return std::nullopt;
    }
    return std::move(output).finish();
}

/// @brief Toolchain Environment Reportを上限内のRedact済みJSONへSerializeする
[[nodiscard]] std::optional<std::string> serialize_environment(
    const cue::BuildEnvironmentReport &a_environment, const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings,
    std::size_t a_maximumBytes)
{
    BoundedTextBuilder output(a_maximumBytes);
    const std::string_view support =
        a_environment.support == cue::BuildEnvironmentSupport::Supported
            ? "supported"
            : (a_environment.support == cue::BuildEnvironmentSupport::Unsupported ? "unsupported" : "unknown");
    if (!output.append("{\n\"schemaVersion\":1,\n\"support\":") || !output.append_json_string(support) ||
        !output.append(",\n\"engineSourceRoot\":") ||
        !append_redacted_json(output, a_environment.engineSourceRoot, a_mappings) ||
        !output.append(",\n\"engineBinaryRoot\":") ||
        !append_redacted_json(output, a_environment.engineBinaryRoot, a_mappings) ||
        !output.append(",\n\"supportedConfigurations\":["))
    {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < a_environment.supportedConfigurations.size(); ++index)
    {
        if ((index > 0U && !output.push_back(',')) ||
            !output.append_json_string(configuration_text(a_environment.supportedConfigurations[index])))
        {
            return std::nullopt;
        }
    }
    if (!output.append("],\n\"selectedTools\":["))
    {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < a_environment.selectedTools.size(); ++index)
    {
        const cue::BuildToolCandidate &tool = a_environment.selectedTools[index];
        if ((index > 0U && !output.push_back(',')) || !output.append("{\"kind\":") ||
            !output.append(std::to_string(static_cast<std::uint32_t>(tool.kind))) || !output.append(",\"path\":") ||
            !append_redacted_json(output, tool.nativePath, a_mappings) || !output.append(",\"root\":") ||
            !append_redacted_json(output, tool.installationRoot, a_mappings) || !output.append(",\"version\":"))
        {
            return std::nullopt;
        }
        if (tool.version)
        {
            const std::string version = std::to_string(tool.version->major) + "." +
                                        std::to_string(tool.version->minor) + "." +
                                        std::to_string(tool.version->patch) + "." + std::to_string(tool.version->build);
            if (!output.append_json_string(version))
            {
                return std::nullopt;
            }
        }
        else if (!output.append("null"))
        {
            return std::nullopt;
        }
        if (!output.append(",\"architecture\":") || !output.append_json_string(architecture_text(tool.architecture)) ||
            !output.append(",\"available\":") || !output.append(tool.available ? "true" : "false") ||
            !output.push_back('}'))
        {
            return std::nullopt;
        }
    }
    if (!output.append("],\n\"diagnostics\":["))
    {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < a_environment.diagnostics.size(); ++index)
    {
        const cue::BuildEnvironmentDiagnostic &diagnostic = a_environment.diagnostics[index];
        const std::string_view diagnosticSupport =
            diagnostic.support == cue::BuildEnvironmentSupport::Supported
                ? "supported"
                : (diagnostic.support == cue::BuildEnvironmentSupport::Unsupported ? "unsupported" : "unknown");
        if ((index > 0U && !output.push_back(',')) || !output.append("{\"code\":") ||
            !output.append(std::to_string(static_cast<std::uint32_t>(diagnostic.code))) || !output.append(",\"tool\":"))
        {
            return std::nullopt;
        }
        if (diagnostic.tool)
        {
            if (!output.append(std::to_string(static_cast<std::uint32_t>(*diagnostic.tool))))
            {
                return std::nullopt;
            }
        }
        else if (!output.append("null"))
        {
            return std::nullopt;
        }
        if (!output.append(",\"support\":") || !output.append_json_string(diagnosticSupport) ||
            !output.append(",\"path\":") || !append_redacted_json(output, diagnostic.nativePath, a_mappings) ||
            !output.append(",\"summary\":") || !append_redacted_json(output, diagnostic.summary, a_mappings) ||
            !output.append(",\"repairHint\":") || !append_redacted_json(output, diagnostic.repairHint, a_mappings) ||
            !output.push_back('}'))
        {
            return std::nullopt;
        }
    }
    if (!output.append("]\n}\n"))
    {
        return std::nullopt;
    }
    return std::move(output).finish();
}

/// @brief 完了済みBuild Stage列を上限内のJSONへSerializeする
[[nodiscard]] std::optional<std::string> serialize_stages(const cue::BuildOperationSnapshot &a_operation,
                                                          std::size_t a_maximumBytes)
{
    BoundedTextBuilder output(a_maximumBytes);
    if (!output.append("{\n\"schemaVersion\":1,\n\"stages\":["))
    {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < a_operation.stages.size(); ++index)
    {
        const cue::BuildStageSnapshot &stage = a_operation.stages[index];
        if ((index > 0U && !output.push_back(',')) || !output.append("{\"stage\":") ||
            !output.append_json_string(stage_text(stage.stage)) || !output.append(",\"outcome\":") ||
            !output.append_json_string(outcome_text(stage.outcome)) || !output.append(",\"exitCode\":") ||
            !output.append(stage.exitCode ? std::to_string(*stage.exitCode) : "null") || !output.push_back('}'))
        {
            return std::nullopt;
        }
    }
    if (!output.append("]\n}\n"))
    {
        return std::nullopt;
    }
    return std::move(output).finish();
}

/// @brief Build終端StateとError Chainを上限内のRedact済みJSONへSerializeする
[[nodiscard]] std::optional<std::string> serialize_result(
    const cue::BuildOperationSnapshot &a_operation, const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings,
    std::size_t a_maximumBytes)
{
    BoundedTextBuilder output(a_maximumBytes);
    if (!output.append("{\n\"schemaVersion\":1,\n\"state\":") ||
        !output.append_json_string(state_text(a_operation.state)) || !output.append(",\n\"diagnostics\":["))
    {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < a_operation.diagnostics.size(); ++index)
    {
        const cue::BuildDiagnosticSnapshot &diagnostic = a_operation.diagnostics[index];
        if ((index > 0U && !output.push_back(',')) || !output.append("{\"domain\":") ||
            !output.append_json_string(diagnostic.domain) || !output.append(",\"code\":") ||
            !output.append(std::to_string(diagnostic.code)) || !output.append(",\"summary\":") ||
            !append_redacted_json(output, diagnostic.summary, a_mappings) || !output.append(",\"contexts\":["))
        {
            return std::nullopt;
        }
        for (std::size_t contextIndex = 0U; contextIndex < diagnostic.contexts.size(); ++contextIndex)
        {
            if ((contextIndex > 0U && !output.push_back(',')) ||
                !append_redacted_json(output, diagnostic.contexts[contextIndex], a_mappings))
            {
                return std::nullopt;
            }
        }
        if (!output.append("],\"nativeError\":"))
        {
            return std::nullopt;
        }
        if (diagnostic.nativeError)
        {
            if (!output.append("{\"domain\":") || !output.append_json_string(diagnostic.nativeError->domain) ||
                !output.append(",\"code\":") || !output.append(std::to_string(diagnostic.nativeError->code)) ||
                !output.push_back('}'))
            {
                return std::nullopt;
            }
        }
        else if (!output.append("null"))
        {
            return std::nullopt;
        }
        if (!output.push_back('}'))
        {
            return std::nullopt;
        }
    }
    if (!output.append("]\n}\n"))
    {
        return std::nullopt;
    }
    return std::move(output).finish();
}

/// @brief 一つのArtifact Inventoryを指定JSON Memberへ上限内で追記する
[[nodiscard]] bool append_artifact(BoundedTextBuilder &a_output, std::string_view a_name,
                                   const cue::BuildArtifactInventory &a_artifact)
{
    if (!a_output.append_json_string(a_name) || !a_output.append(":{\"artifactId\":") ||
        !a_output.append_json_string(a_artifact.artifact_id()) || !a_output.append(",\"configuration\":") ||
        !a_output.append_json_string(configuration_text(a_artifact.configuration())) ||
        !a_output.append(",\"files\":["))
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_artifact.files().size(); ++index)
    {
        const cue::BuildArtifactFile &file = a_artifact.files()[index];
        if ((index > 0U && !a_output.push_back(',')) || !a_output.append("{\"path\":") ||
            !a_output.append_json_string(file.relativePath) || !a_output.append(",\"sizeBytes\":") ||
            !a_output.append(std::to_string(file.byteSize)) || !a_output.append(",\"contentHash\":") ||
            !a_output.append_json_string(file.contentHash) || !a_output.push_back('}'))
        {
            return false;
        }
    }
    return a_output.append("]}");
}

/// @brief CurrentとLatest Successful Artifact Metadataを上限内のJSONへSerializeする
[[nodiscard]] std::optional<std::string> serialize_artifacts(const cue::BuildOperationSnapshot &a_operation,
                                                             std::size_t a_maximumBytes)
{
    BoundedTextBuilder output(a_maximumBytes);
    if (!output.append("{\n\"schemaVersion\":1,\n"))
    {
        return std::nullopt;
    }
    bool needsComma = false;
    if (a_operation.artifact)
    {
        if (!append_artifact(output, "operationArtifact", *a_operation.artifact))
        {
            return std::nullopt;
        }
        needsComma = true;
    }
    if (a_operation.latestSuccessfulArtifact)
    {
        if ((needsComma && !output.append(",\n")) ||
            !append_artifact(output, "latestSuccessfulArtifact", *a_operation.latestSuccessfulArtifact))
        {
            return std::nullopt;
        }
    }
    if (!output.append("\n}\n"))
    {
        return std::nullopt;
    }
    return std::move(output).finish();
}

/// @brief Log直列化が失敗した安定分類
enum class LogSerializationFailure : std::uint8_t
{
    None,
    InvalidInput,
    SizeLimitExceeded
};

/// @brief Log直列化の所有Byte列と失敗分類
struct LogSerializationResult final
{
    std::string bytes;
    LogSerializationFailure failure = LogSerializationFailure::None;
};

/// @brief 指定Streamの全Chunkを上限内で連結してからUTF-8検証とRedactionを適用する
[[nodiscard]] LogSerializationResult serialize_log(const cue::BuildOperationSnapshot &a_operation,
                                                   cue::ChildProcessStream a_stream,
                                                   const std::vector<cue::BuildDiagnosticPathMapping> &a_mappings,
                                                   std::size_t a_maximumBytes)
{
    std::string source;
    for (const cue::BuildLogSnapshot &log : a_operation.logs)
    {
        if (log.operationId == a_operation.operationId && log.stream == a_stream)
        {
            if (log.bytes.size() > a_maximumBytes - source.size())
            {
                return {{}, LogSerializationFailure::SizeLimitExceeded};
            }
            source.append(log.bytes);
        }
    }
    if (!valid_utf8(source))
    {
        return {{}, LogSerializationFailure::InvalidInput};
    }
    std::optional<std::string> redacted = redact_bounded(source, a_mappings, a_maximumBytes);
    if (!redacted)
    {
        return {{}, LogSerializationFailure::SizeLimitExceeded};
    }
    std::string output = std::move(*redacted);
    constexpr std::string_view byteOrderMark = "\xEF\xBB\xBF";
    if (output.starts_with(byteOrderMark))
    {
        output.erase(0U, byteOrderMark.size());
    }
    std::size_t writeOffset = 0U;
    for (std::size_t readOffset = 0U; readOffset < output.size(); ++readOffset)
    {
        if (output[readOffset] == '\r')
        {
            output[writeOffset++] = '\n';
            if (readOffset + 1U < output.size() && output[readOffset + 1U] == '\n')
            {
                ++readOffset;
            }
        }
        else
        {
            output[writeOffset++] = output[readOffset];
        }
    }
    output.resize(writeOffset);
    if (output.empty() || output.back() != '\n')
    {
        if (output.size() >= a_maximumBytes)
        {
            return {{}, LogSerializationFailure::SizeLimitExceeded};
        }
        output.push_back('\n');
    }
    return {std::move(output), LogSerializationFailure::None};
}

/// @brief Size Policyを適用しながら一FileをBundle候補へ追加する
[[nodiscard]] std::optional<cue::BuildDiagnosticBundleError> add_file(
    std::vector<cue::BuildDiagnosticBundleFile> &a_files, std::string a_path, std::string a_text,
    const cue::BuildDiagnosticBundleLimits &a_limits, std::uint64_t &a_totalBytes)
{
    const std::uint64_t size = static_cast<std::uint64_t>(a_text.size());
    if (a_files.size() >= a_limits.maximumFileCount)
    {
        return cue::BuildDiagnosticBundleError::FileCountLimitExceeded;
    }
    if (size > a_limits.maximumFileBytes)
    {
        return cue::BuildDiagnosticBundleError::FileSizeLimitExceeded;
    }
    if (size > a_limits.maximumTotalBytes - a_totalBytes)
    {
        return cue::BuildDiagnosticBundleError::TotalSizeLimitExceeded;
    }
    a_totalBytes += size;
    a_files.push_back({std::move(a_path), to_bytes(std::move(a_text))});
    return std::nullopt;
}

/// @brief 収集項目と欠損理由を再読込可能なManifest JSONへSerializeする
[[nodiscard]] std::string serialize_manifest(std::string_view a_operationId, cue::GameBuildOperationState a_state,
                                             std::span<const cue::BuildDiagnosticManifestEntry> a_entries)
{
    std::string output = "{\n\"schemaVersion\":1,\n\"operationId\":";
    append_json_string(output, a_operationId);
    output.append(",\n\"state\":");
    append_json_string(output, state_text(a_state));
    output.append(",\n\"entries\":[\n");
    for (std::size_t index = 0U; index < a_entries.size(); ++index)
    {
        const cue::BuildDiagnosticManifestEntry &entry = a_entries[index];
        output.append("{\"path\":");
        append_json_string(output, entry.relativePath);
        output.append(",\"collected\":");
        output.append(entry.collected ? "true" : "false");
        output.append(",\"sizeBytes\":");
        output.append(std::to_string(entry.byteSize));
        output.append(",\"missingReason\":");
        append_json_string(output, entry.missingReason);
        output.append(index + 1U == a_entries.size() ? "}\n" : "},\n");
    }
    output.append("]\n}\n");
    return output;
}

class JsonSchemaReader final
{
  public:
    /// @brief 検証対象JSON全体を参照するReaderを初期化する
    explicit JsonSchemaReader(std::string_view a_input) noexcept : m_input(a_input)
    {
    }

    /// @brief Object開始Tokenを読み取る
    [[nodiscard]] bool begin_object() noexcept
    {
        return consume('{');
    }

    /// @brief Object終了Tokenを読み取る
    [[nodiscard]] bool end_object() noexcept
    {
        return consume('}');
    }

    /// @brief Array開始Tokenを読み取る
    [[nodiscard]] bool begin_array() noexcept
    {
        return consume('[');
    }

    /// @brief Array終了Tokenを読み取る
    [[nodiscard]] bool end_array() noexcept
    {
        return consume(']');
    }

    /// @brief 値またはMember間のSeparatorを読み取る
    [[nodiscard]] bool comma() noexcept
    {
        return consume(',');
    }

    /// @brief Escapeを許可しない固定ASCII Member名とColonを読み取る
    [[nodiscard]] bool member(std::string_view a_name) noexcept
    {
        skip_whitespace();
        if (m_offset >= m_input.size() || m_input[m_offset] != '"')
        {
            return false;
        }
        const std::size_t required = a_name.size() + 2U;
        if (required > m_input.size() - m_offset || m_input.substr(m_offset + 1U, a_name.size()) != a_name ||
            m_input[m_offset + required - 1U] != '"')
        {
            return false;
        }
        m_offset += required;
        return consume(':');
    }

    /// @brief 任意内容の妥当なJSON Stringを読み取る
    [[nodiscard]] bool string() noexcept
    {
        skip_whitespace();
        if (m_offset >= m_input.size() || m_input[m_offset++] != '"')
        {
            return false;
        }
        while (m_offset < m_input.size())
        {
            const unsigned char value = static_cast<unsigned char>(m_input[m_offset++]);
            if (value == '"')
            {
                return true;
            }
            if (value < 0x20U)
            {
                return false;
            }
            if (value != '\\')
            {
                continue;
            }
            if (m_offset >= m_input.size())
            {
                return false;
            }
            const char escape = m_input[m_offset++];
            if (escape == 'u')
            {
                for (std::size_t index = 0U; index < 4U; ++index)
                {
                    if (m_offset >= m_input.size() || !is_hex(m_input[m_offset++]))
                    {
                        return false;
                    }
                }
            }
            else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos)
            {
                return false;
            }
        }
        return false;
    }

    /// @brief Escapeなしの固定候補JSON Stringを読み取る
    [[nodiscard]] bool string_is(std::initializer_list<std::string_view> a_values) noexcept
    {
        skip_whitespace();
        for (const std::string_view value : a_values)
        {
            const std::size_t required = value.size() + 2U;
            if (required <= m_input.size() - m_offset && m_input[m_offset] == '"' &&
                m_input.substr(m_offset + 1U, value.size()) == value && m_input[m_offset + required - 1U] == '"')
            {
                m_offset += required;
                return true;
            }
        }
        return false;
    }

    /// @brief Escapeを含まないJSON Stringの借用値を読み取る
    [[nodiscard]] bool unescaped_string(std::string_view &a_value) noexcept
    {
        skip_whitespace();
        if (m_offset >= m_input.size() || m_input[m_offset++] != '"')
        {
            return false;
        }
        const std::size_t begin = m_offset;
        while (m_offset < m_input.size())
        {
            const unsigned char value = static_cast<unsigned char>(m_input[m_offset]);
            if (value == '"')
            {
                a_value = m_input.substr(begin, m_offset - begin);
                ++m_offset;
                return true;
            }
            if (value < 0x20U || value == '\\')
            {
                return false;
            }
            ++m_offset;
        }
        return false;
    }

    /// @brief JSONの符号なし整数を値、表現範囲、指定上限付きで読み取る
    [[nodiscard]] bool unsigned_integer_value(
        std::uint64_t &a_value, std::uint64_t a_maximum = std::numeric_limits<std::uint64_t>::max()) noexcept
    {
        skip_whitespace();
        const std::size_t begin = m_offset;
        if (!consume_digits())
        {
            return false;
        }
        const auto parsed = std::from_chars(m_input.data() + begin, m_input.data() + m_offset, a_value);
        return parsed.ec == std::errc{} && parsed.ptr == m_input.data() + m_offset && a_value <= a_maximum;
    }

    /// @brief JSONの符号なし整数を表現範囲と指定上限で検証して読み取る
    [[nodiscard]] bool unsigned_integer(std::uint64_t a_maximum = std::numeric_limits<std::uint64_t>::max()) noexcept
    {
        std::uint64_t value = 0U;
        return unsigned_integer_value(value, a_maximum);
    }

    /// @brief JSONの0以外の符号なし整数を読み取る
    [[nodiscard]] bool positive_unsigned_integer(
        std::uint64_t a_maximum = std::numeric_limits<std::uint64_t>::max()) noexcept
    {
        std::uint64_t value = 0U;
        return unsigned_integer_value(value, a_maximum) && value > 0U;
    }

    /// @brief JSONの符号付き整数を範囲検証して読み取る
    [[nodiscard]] bool signed_integer() noexcept
    {
        skip_whitespace();
        const std::size_t begin = m_offset;
        if (m_offset < m_input.size() && m_input[m_offset] == '-')
        {
            ++m_offset;
        }
        if (!consume_digits())
        {
            return false;
        }
        std::int64_t value = 0;
        const auto parsed = std::from_chars(m_input.data() + begin, m_input.data() + m_offset, value);
        return parsed.ec == std::errc{} && parsed.ptr == m_input.data() + m_offset;
    }

    /// @brief JSONのBooleanを読み取る
    [[nodiscard]] bool boolean() noexcept
    {
        return consume_word("true") || consume_word("false");
    }

    /// @brief JSONのnullを読み取る
    [[nodiscard]] bool null_value() noexcept
    {
        return consume_word("null");
    }

    /// @brief Schema Version 1の固定整数を読み取る
    [[nodiscard]] bool version_one() noexcept
    {
        return consume_word("1");
    }

    /// @brief 次の非Whitespace文字が指定Tokenか判定する
    [[nodiscard]] bool next_is(char a_value) noexcept
    {
        skip_whitespace();
        return m_offset < m_input.size() && m_input[m_offset] == a_value;
    }

    /// @brief 末尾Whitespace以外をすべて消費したか判定する
    [[nodiscard]] bool finished() noexcept
    {
        skip_whitespace();
        return m_offset == m_input.size();
    }

  private:
    /// @brief JSON Whitespaceを読み飛ばす
    void skip_whitespace() noexcept
    {
        while (m_offset < m_input.size() && (m_input[m_offset] == ' ' || m_input[m_offset] == '\t' ||
                                             m_input[m_offset] == '\r' || m_input[m_offset] == '\n'))
        {
            ++m_offset;
        }
    }

    /// @brief 指定する一文字Tokenを読み取る
    [[nodiscard]] bool consume(char a_value) noexcept
    {
        skip_whitespace();
        if (m_offset >= m_input.size() || m_input[m_offset] != a_value)
        {
            return false;
        }
        ++m_offset;
        return true;
    }

    /// @brief 指定する固定Keywordを読み取る
    [[nodiscard]] bool consume_word(std::string_view a_value) noexcept
    {
        skip_whitespace();
        if (a_value.size() > m_input.size() - m_offset || m_input.substr(m_offset, a_value.size()) != a_value)
        {
            return false;
        }
        m_offset += a_value.size();
        return true;
    }

    /// @brief Leading Zero規則を含む一桁以上の十進数字を読み取る
    [[nodiscard]] bool consume_digits() noexcept
    {
        if (m_offset >= m_input.size() || m_input[m_offset] < '0' || m_input[m_offset] > '9')
        {
            return false;
        }
        if (m_input[m_offset] == '0')
        {
            ++m_offset;
            return m_offset >= m_input.size() || m_input[m_offset] < '0' || m_input[m_offset] > '9';
        }
        while (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9')
        {
            ++m_offset;
        }
        return true;
    }

    /// @brief JSON Unicode Escapeで許可されるHex文字か判定する
    [[nodiscard]] static bool is_hex(char a_value) noexcept
    {
        return (a_value >= '0' && a_value <= '9') || (a_value >= 'a' && a_value <= 'f') ||
               (a_value >= 'A' && a_value <= 'F');
    }

    std::string_view m_input;
    std::size_t m_offset = 0U;
};

/// @brief JSON ObjectのSchema Version 1 Headerを読み取る
[[nodiscard]] bool read_schema_header(JsonSchemaReader &a_reader) noexcept
{
    return a_reader.begin_object() && a_reader.member("schemaVersion") && a_reader.version_one();
}

/// @brief 任意JSON Stringだけを含むArrayを読み取る
[[nodiscard]] bool read_string_array(JsonSchemaReader &a_reader) noexcept
{
    if (!a_reader.begin_array())
    {
        return false;
    }
    if (a_reader.next_is(']'))
    {
        return a_reader.end_array();
    }
    while (a_reader.string())
    {
        if (a_reader.next_is(']'))
        {
            return a_reader.end_array();
        }
        if (!a_reader.comma())
        {
            return false;
        }
    }
    return false;
}

/// @brief Build Configuration文字列だけを含むArrayを読み取る
[[nodiscard]] bool read_configuration_array(JsonSchemaReader &a_reader) noexcept
{
    if (!a_reader.begin_array())
    {
        return false;
    }
    if (a_reader.next_is(']'))
    {
        return a_reader.end_array();
    }
    while (a_reader.string_is({"Debug", "Development", "Release"}))
    {
        if (a_reader.next_is(']'))
        {
            return a_reader.end_array();
        }
        if (!a_reader.comma())
        {
            return false;
        }
    }
    return false;
}

/// @brief Toolchain Report内のTool Objectを固定Schemaで読み取る
[[nodiscard]] bool read_tool(JsonSchemaReader &a_reader) noexcept
{
    if (!a_reader.begin_object() || !a_reader.member("kind") ||
        !a_reader.unsigned_integer(static_cast<std::uint64_t>(cue::BuildToolKind::Git)) || !a_reader.comma() ||
        !a_reader.member("path") || !a_reader.string() || !a_reader.comma() || !a_reader.member("root") ||
        !a_reader.string() || !a_reader.comma() || !a_reader.member("version"))
    {
        return false;
    }
    if (!(a_reader.next_is('n') ? a_reader.null_value() : a_reader.string()))
    {
        return false;
    }
    return a_reader.comma() && a_reader.member("architecture") &&
           a_reader.string_is({"unknown", "x64", "x86", "arm64"}) && a_reader.comma() && a_reader.member("available") &&
           a_reader.boolean() && a_reader.end_object();
}

/// @brief Toolchain Report内のTool Arrayを固定Schemaで読み取る
[[nodiscard]] bool read_tool_array(JsonSchemaReader &a_reader) noexcept
{
    if (!a_reader.begin_array())
    {
        return false;
    }
    if (a_reader.next_is(']'))
    {
        return a_reader.end_array();
    }
    while (read_tool(a_reader))
    {
        if (a_reader.next_is(']'))
        {
            return a_reader.end_array();
        }
        if (!a_reader.comma())
        {
            return false;
        }
    }
    return false;
}

/// @brief Toolchain Report内のDiagnostic Objectを固定Schemaで読み取る
[[nodiscard]] bool read_environment_diagnostic(JsonSchemaReader &a_reader) noexcept
{
    if (!a_reader.begin_object() || !a_reader.member("code") ||
        !a_reader.unsigned_integer(
            static_cast<std::uint64_t>(cue::BuildEnvironmentDiagnosticCode::MissingEngineBinary)) ||
        !a_reader.comma() || !a_reader.member("tool"))
    {
        return false;
    }
    if (!(a_reader.next_is('n')
              ? a_reader.null_value()
              : a_reader.unsigned_integer(static_cast<std::uint64_t>(cue::BuildToolKind::Git))))
    {
        return false;
    }
    return a_reader.comma() && a_reader.member("support") &&
           a_reader.string_is({"supported", "unsupported", "unknown"}) && a_reader.comma() && a_reader.member("path") &&
           a_reader.string() && a_reader.comma() && a_reader.member("summary") && a_reader.string() &&
           a_reader.comma() && a_reader.member("repairHint") && a_reader.string() && a_reader.end_object();
}

/// @brief Toolchain Report内のDiagnostic Arrayを固定Schemaで読み取る
[[nodiscard]] bool read_environment_diagnostic_array(JsonSchemaReader &a_reader) noexcept
{
    if (!a_reader.begin_array())
    {
        return false;
    }
    if (a_reader.next_is(']'))
    {
        return a_reader.end_array();
    }
    while (read_environment_diagnostic(a_reader))
    {
        if (a_reader.next_is(']'))
        {
            return a_reader.end_array();
        }
        if (!a_reader.comma())
        {
            return false;
        }
    }
    return false;
}

/// @brief Stage結果Objectを固定Schemaで読み取りStageとOutcomeを返す
[[nodiscard]] bool read_stage(JsonSchemaReader &a_reader, cue::BuildStage &a_stage,
                              cue::BuildStageOutcome &a_outcome) noexcept
{
    if (!a_reader.begin_object() || !a_reader.member("stage"))
    {
        return false;
    }
    if (a_reader.string_is({"configure"}))
    {
        a_stage = cue::BuildStage::Configure;
    }
    else if (a_reader.string_is({"build"}))
    {
        a_stage = cue::BuildStage::Build;
    }
    else
    {
        return false;
    }
    if (!a_reader.comma() || !a_reader.member("outcome"))
    {
        return false;
    }
    enum class ExitCodeRequirement : std::uint8_t
    {
        Zero,
        Positive,
        Null
    };
    ExitCodeRequirement requirement = ExitCodeRequirement::Null;
    if (a_reader.string_is({"succeeded"}))
    {
        a_outcome = cue::BuildStageOutcome::Succeeded;
        requirement = ExitCodeRequirement::Zero;
    }
    else if (a_reader.string_is({"failed"}))
    {
        a_outcome = cue::BuildStageOutcome::Failed;
        requirement = ExitCodeRequirement::Positive;
    }
    else if (a_reader.string_is({"cancelled"}))
    {
        a_outcome = cue::BuildStageOutcome::Cancelled;
    }
    else if (a_reader.string_is({"timedOut"}))
    {
        a_outcome = cue::BuildStageOutcome::TimedOut;
    }
    else
    {
        return false;
    }
    if (!a_reader.comma() || !a_reader.member("exitCode"))
    {
        return false;
    }
    const bool validExitCode = requirement == ExitCodeRequirement::Zero ? a_reader.unsigned_integer(0U)
                               : requirement == ExitCodeRequirement::Positive
                                   ? a_reader.positive_unsigned_integer(std::numeric_limits<std::uint32_t>::max())
                                   : a_reader.null_value();
    return validExitCode && a_reader.end_object();
}

/// @brief Stage結果Arrayを固定Schemaで読み取り要素数と最終結果を返す
[[nodiscard]] bool read_stage_array(JsonSchemaReader &a_reader, std::size_t &a_count, cue::BuildStage &a_lastStage,
                                    cue::BuildStageOutcome &a_lastOutcome) noexcept
{
    a_count = 0U;
    if (!a_reader.begin_array())
    {
        return false;
    }
    if (a_reader.next_is(']'))
    {
        return a_reader.end_array();
    }
    cue::BuildStage stage = cue::BuildStage::Configure;
    cue::BuildStageOutcome outcome = cue::BuildStageOutcome::Failed;
    while (read_stage(a_reader, stage, outcome))
    {
        ++a_count;
        a_lastStage = stage;
        a_lastOutcome = outcome;
        if (a_reader.next_is(']'))
        {
            return a_reader.end_array();
        }
        if (!a_reader.comma())
        {
            return false;
        }
    }
    return false;
}

/// @brief Native Error Objectまたはnullを固定Schemaで読み取る
[[nodiscard]] bool read_native_error(JsonSchemaReader &a_reader) noexcept
{
    if (a_reader.next_is('n'))
    {
        return a_reader.null_value();
    }
    return a_reader.begin_object() && a_reader.member("domain") && a_reader.string() && a_reader.comma() &&
           a_reader.member("code") && a_reader.signed_integer() && a_reader.end_object();
}

/// @brief Build Result内のDiagnostic Objectを固定Schemaで読み取る
[[nodiscard]] bool read_result_diagnostic(JsonSchemaReader &a_reader) noexcept
{
    return a_reader.begin_object() && a_reader.member("domain") && a_reader.string() && a_reader.comma() &&
           a_reader.member("code") && a_reader.signed_integer() && a_reader.comma() && a_reader.member("summary") &&
           a_reader.string() && a_reader.comma() && a_reader.member("contexts") && read_string_array(a_reader) &&
           a_reader.comma() && a_reader.member("nativeError") && read_native_error(a_reader) && a_reader.end_object();
}

/// @brief Build Result内のDiagnostic Arrayを固定Schemaで読み取る
[[nodiscard]] bool read_result_diagnostic_array(JsonSchemaReader &a_reader) noexcept
{
    if (!a_reader.begin_array())
    {
        return false;
    }
    if (a_reader.next_is(']'))
    {
        return a_reader.end_array();
    }
    while (read_result_diagnostic(a_reader))
    {
        if (a_reader.next_is(']'))
        {
            return a_reader.end_array();
        }
        if (!a_reader.comma())
        {
            return false;
        }
    }
    return false;
}

/// @brief 読込中Artifact InventoryのFile制約をAllocationなしで追跡する
struct ArtifactReadState final
{
    std::array<std::string_view, k_maximumArtifactFiles> paths{};
    std::size_t fileCount = 0U;
    bool hasModule = false;
    bool hasMetadata = false;
};

/// @brief Artifact File ObjectをInventory値制約付きで読み取る
[[nodiscard]] bool read_artifact_file(JsonSchemaReader &a_reader, ArtifactReadState &a_state) noexcept
{
    std::string_view path;
    std::uint64_t byteSize = 0U;
    std::string_view contentHash;
    if (a_state.fileCount >= a_state.paths.size() || !a_reader.begin_object() || !a_reader.member("path") ||
        !a_reader.unescaped_string(path) || !a_reader.comma() || !a_reader.member("sizeBytes") ||
        !a_reader.unsigned_integer_value(byteSize, k_maximumArtifactByteSize) || !a_reader.comma() ||
        !a_reader.member("contentHash") || !a_reader.unescaped_string(contentHash) || !a_reader.end_object() ||
        !is_safe_artifact_relative_path(path) || !valid_artifact_hash(contentHash))
    {
        return false;
    }
    if (a_state.fileCount > 0U && !(a_state.paths[a_state.fileCount - 1U] < path))
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_state.fileCount; ++index)
    {
        if (artifact_paths_equal(a_state.paths[index], path))
        {
            return false;
        }
    }
    a_state.paths[a_state.fileCount++] = path;
    if (path == "CueGameModule.dll")
    {
        a_state.hasModule = byteSize > 0U;
    }
    else if (path == "CueGameModule.metadata.json")
    {
        a_state.hasMetadata = byteSize > 0U;
    }
    return true;
}

/// @brief Artifact File Arrayを件数、順序、重複、必須File制約付きで読み取る
[[nodiscard]] bool read_artifact_file_array(JsonSchemaReader &a_reader) noexcept
{
    ArtifactReadState state;
    if (!a_reader.begin_array() || a_reader.next_is(']'))
    {
        return false;
    }
    while (read_artifact_file(a_reader, state))
    {
        if (a_reader.next_is(']'))
        {
            return a_reader.end_array() && state.hasModule && state.hasMetadata;
        }
        if (!a_reader.comma())
        {
            return false;
        }
    }
    return false;
}

/// @brief Artifact Inventory Objectを固定Schemaで読み取る
[[nodiscard]] bool read_artifact(JsonSchemaReader &a_reader) noexcept
{
    std::string_view artifactId;
    return a_reader.begin_object() && a_reader.member("artifactId") && a_reader.unescaped_string(artifactId) &&
           is_uuid_v4(artifactId) && a_reader.comma() && a_reader.member("configuration") &&
           a_reader.string_is({"Debug", "Development", "Release"}) && a_reader.comma() && a_reader.member("files") &&
           read_artifact_file_array(a_reader) && a_reader.end_object();
}

/// @brief Plan PayloadをVersion 1固定Schemaとして検証する
[[nodiscard]] bool valid_plan_payload(std::string_view a_text) noexcept
{
    JsonSchemaReader reader(a_text);
    if (!read_schema_header(reader))
    {
        return false;
    }
    constexpr std::array<std::string_view, 8U> members = {
        "projectRoot",        "presetName",         "workspaceKey",           "binaryDirectory",
        "candidateDirectory", "operationDirectory", "artifactStoreDirectory", "targetName"};
    for (const std::string_view member : members)
    {
        if (!reader.comma() || !reader.member(member) || !reader.string())
        {
            return false;
        }
    }
    return reader.end_object() && reader.finished();
}

/// @brief Environment PayloadをVersion 1固定Schemaとして検証する
[[nodiscard]] bool valid_environment_payload(std::string_view a_text) noexcept
{
    JsonSchemaReader reader(a_text);
    return read_schema_header(reader) && reader.comma() && reader.member("support") &&
           reader.string_is({"supported", "unsupported", "unknown"}) && reader.comma() &&
           reader.member("engineSourceRoot") && reader.string() && reader.comma() &&
           reader.member("engineBinaryRoot") && reader.string() && reader.comma() &&
           reader.member("supportedConfigurations") && read_configuration_array(reader) && reader.comma() &&
           reader.member("selectedTools") && read_tool_array(reader) && reader.comma() &&
           reader.member("diagnostics") && read_environment_diagnostic_array(reader) && reader.end_object() &&
           reader.finished();
}

/// @brief Stage PayloadをVersion 1固定Schemaと成功Operationの終端条件で検証する
[[nodiscard]] bool valid_stages_payload(std::string_view a_text, cue::GameBuildOperationState a_expectedState) noexcept
{
    JsonSchemaReader reader(a_text);
    std::size_t count = 0U;
    cue::BuildStage lastStage = cue::BuildStage::Configure;
    cue::BuildStageOutcome lastOutcome = cue::BuildStageOutcome::Failed;
    const bool validSchema = read_schema_header(reader) && reader.comma() && reader.member("stages") &&
                             read_stage_array(reader, count, lastStage, lastOutcome) && reader.end_object() &&
                             reader.finished();
    return validSchema &&
           (a_expectedState != cue::GameBuildOperationState::Succeeded ||
            (count > 0U && lastStage == cue::BuildStage::Build && lastOutcome == cue::BuildStageOutcome::Succeeded));
}

/// @brief Result PayloadをVersion 1固定Schemaとして検証する
[[nodiscard]] bool valid_result_payload(std::string_view a_text, cue::GameBuildOperationState a_expectedState) noexcept
{
    JsonSchemaReader reader(a_text);
    return read_schema_header(reader) && reader.comma() && reader.member("state") &&
           reader.string_is({state_text(a_expectedState)}) && reader.comma() && reader.member("diagnostics") &&
           read_result_diagnostic_array(reader) && reader.end_object() && reader.finished();
}

/// @brief Artifact PayloadをVersion 1固定SchemaとOperation Stateの対応付きで検証する
[[nodiscard]] bool valid_artifact_payload(std::string_view a_text,
                                          cue::GameBuildOperationState a_expectedState) noexcept
{
    JsonSchemaReader reader(a_text);
    if (!read_schema_header(reader) || !reader.comma())
    {
        return false;
    }
    if (a_expectedState == cue::GameBuildOperationState::Succeeded)
    {
        if (!reader.member("operationArtifact") || !read_artifact(reader))
        {
            return false;
        }
        if (reader.next_is(','))
        {
            if (!reader.comma() || !reader.member("latestSuccessfulArtifact") || !read_artifact(reader))
            {
                return false;
            }
        }
    }
    else if (!reader.member("latestSuccessfulArtifact") || !read_artifact(reader))
    {
        return false;
    }
    return reader.end_object() && reader.finished();
}

/// @brief 全収集済みJSON PayloadがPath固有のVersion 1 Schemaに一致するか検証する
[[nodiscard]] bool valid_payload_schemas(std::span<const cue::BuildDiagnosticBundleFile> a_files,
                                         cue::GameBuildOperationState a_expectedState) noexcept
{
    for (const cue::BuildDiagnosticBundleFile &file : a_files)
    {
        if (file.relativePath == "manifest.json" || file.relativePath == "stdout.log" ||
            file.relativePath == "stderr.log")
        {
            continue;
        }
        const std::string text = from_bytes(file.bytes);
        const bool valid = file.relativePath == "plan.json"          ? valid_plan_payload(text)
                           : file.relativePath == "environment.json" ? valid_environment_payload(text)
                           : file.relativePath == "stages.json"      ? valid_stages_payload(text, a_expectedState)
                           : file.relativePath == "result.json"      ? valid_result_payload(text, a_expectedState)
                           : file.relativePath == "artifact.json"    ? valid_artifact_payload(text, a_expectedState)
                                                                     : false;
        if (!valid)
        {
            return false;
        }
    }
    return true;
}

/// @brief 固定Prefix直後の単純なQuoted JSON値を検証用に抽出する
[[nodiscard]] std::optional<std::string_view> extract_quoted_value(std::string_view a_text,
                                                                   std::string_view a_prefix) noexcept
{
    const std::size_t begin = a_text.find(a_prefix);
    if (begin == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::size_t valueBegin = begin + a_prefix.size();
    const std::size_t end = a_text.find('"', valueBegin);
    if (end == std::string_view::npos)
    {
        return std::nullopt;
    }
    return a_text.substr(valueBegin, end - valueBegin);
}

/// @brief 一行Manifest Entryを既知Pathと整合条件付きでParseする
[[nodiscard]] bool parse_manifest_entry(std::string_view a_line, cue::BuildDiagnosticManifestEntry &a_entry)
{
    const auto path = extract_quoted_value(a_line, "{\"path\":\"");
    const std::size_t collectedPosition = a_line.find("\",\"collected\":");
    const std::size_t sizePosition = a_line.find(",\"sizeBytes\":");
    const auto missingReason = extract_quoted_value(a_line, ",\"missingReason\":\"");
    if (!path || !missingReason || collectedPosition == std::string_view::npos ||
        sizePosition == std::string_view::npos || !known_bundle_path(*path) || *path == "manifest.json")
    {
        return false;
    }
    const std::size_t collectedBegin = collectedPosition + 14U;
    const std::string_view collectedText = a_line.substr(collectedBegin, sizePosition - collectedBegin);
    const std::size_t sizeBegin = sizePosition + 13U;
    const std::size_t sizeEnd = a_line.find(',', sizeBegin);
    if ((collectedText != "true" && collectedText != "false") || sizeEnd == std::string_view::npos)
    {
        return false;
    }
    std::uint64_t byteSize = 0U;
    const auto parsed = std::from_chars(a_line.data() + sizeBegin, a_line.data() + sizeEnd, byteSize);
    if (parsed.ec != std::errc{} || parsed.ptr != a_line.data() + sizeEnd ||
        (collectedText == "false" && (byteSize != 0U || missingReason->empty())) ||
        (collectedText == "true" && !missingReason->empty()))
    {
        return false;
    }
    a_entry = {std::string(*path), collectedText == "true", byteSize, std::string(*missingReason)};
    return true;
}

/// @brief Manifestの収集宣言と実際のBundle File集合が一致するか検証する
[[nodiscard]] bool files_match_manifest(std::span<const cue::BuildDiagnosticBundleFile> a_files,
                                        std::span<const cue::BuildDiagnosticManifestEntry> a_entries) noexcept
{
    for (const cue::BuildDiagnosticManifestEntry &entry : a_entries)
    {
        const auto file = std::find_if(a_files.begin(), a_files.end(),
                                       /// @brief Manifest Entryと同じ相対PathのFileを検出する
                                       [&entry](const auto &a_candidate) noexcept
                                       { return a_candidate.relativePath == entry.relativePath; });
        if (entry.collected != (file != a_files.end()) ||
            (file != a_files.end() && file->bytes.size() != entry.byteSize))
        {
            return false;
        }
    }
    for (const cue::BuildDiagnosticBundleFile &file : a_files)
    {
        if (file.relativePath == "manifest.json")
        {
            continue;
        }
        const auto entry =
            std::find_if(a_entries.begin(), a_entries.end(),
                         /// @brief Fileと同じPathを収集済みとするManifest Entryを検出する
                         [&file](const auto &a_candidate) noexcept
                         { return a_candidate.collected && a_candidate.relativePath == file.relativePath; });
        if (entry == a_entries.end())
        {
            return false;
        }
    }
    return true;
}

/// @brief Manifestが全既知項目を重複なく列挙し必須Fileを収集済みか検証する
[[nodiscard]] bool valid_manifest_entries(std::span<const cue::BuildDiagnosticManifestEntry> a_entries,
                                          cue::GameBuildOperationState a_state) noexcept
{
    if (a_entries.size() != k_manifestEntryPaths.size())
    {
        return false;
    }
    for (std::size_t index = 0U; index < k_manifestEntryPaths.size(); ++index)
    {
        const std::string_view path = k_manifestEntryPaths[index];
        const cue::BuildDiagnosticManifestEntry &entry = a_entries[index];
        if (entry.relativePath != path)
        {
            return false;
        }
        const bool optional = (path == "artifact.json" && a_state != cue::GameBuildOperationState::Succeeded) ||
                              path == "environment.json";
        if (!optional && !entry.collected)
        {
            return false;
        }
    }
    return true;
}

/// @brief Writerへ渡されたBundle全体が構築時と同じ公開契約を満たすか検証する
[[nodiscard]] bool valid_bundle_for_write(const cue::BuildDiagnosticBundle &a_bundle)
{
    if (!is_uuid_v4(a_bundle.operation_id()) || !valid_terminal_state(a_bundle.state()) ||
        !valid_manifest_entries(a_bundle.manifest_entries(), a_bundle.state()))
    {
        return false;
    }
    const auto files = a_bundle.files();
    const std::size_t collectedFileCount =
        static_cast<std::size_t>(std::count_if(a_bundle.manifest_entries().begin(), a_bundle.manifest_entries().end(),
                                               /// @brief 収集済みManifest Entryを数える
                                               [](const auto &a_entry) noexcept { return a_entry.collected; }));
    if (files.size() != collectedFileCount + 1U)
    {
        return false;
    }
    for (std::size_t index = 0U; index < files.size(); ++index)
    {
        if (!known_bundle_path(files[index].relativePath) || !valid_bundle_text(files[index].bytes) ||
            (index > 0U && files[index - 1U].relativePath >= files[index].relativePath))
        {
            return false;
        }
    }
    const auto manifestFile =
        std::find_if(files.begin(), files.end(),
                     /// @brief Writer検証対象のManifest Fileを検出する
                     [](const auto &a_file) noexcept { return a_file.relativePath == "manifest.json"; });
    if (manifestFile == files.end())
    {
        return false;
    }
    return from_bytes(manifestFile->bytes) ==
               serialize_manifest(a_bundle.operation_id(), a_bundle.state(), a_bundle.manifest_entries()) &&
           files_match_manifest(files, a_bundle.manifest_entries()) && valid_payload_schemas(files, a_bundle.state());
}
} // namespace

namespace cue
{
BuildDiagnosticBundle::BuildDiagnosticBundle(std::string a_operationId, GameBuildOperationState a_state,
                                             std::vector<BuildDiagnosticManifestEntry> a_entries,
                                             std::vector<BuildDiagnosticBundleFile> a_files) noexcept
    : m_operationId(std::move(a_operationId)), m_state(a_state), m_entries(std::move(a_entries)),
      m_files(std::move(a_files))
{
}

std::string_view BuildDiagnosticBundle::operation_id() const noexcept
{
    return m_operationId;
}

GameBuildOperationState BuildDiagnosticBundle::state() const noexcept
{
    return m_state;
}

std::span<const BuildDiagnosticManifestEntry> BuildDiagnosticBundle::manifest_entries() const noexcept
{
    return m_entries;
}

std::span<const BuildDiagnosticBundleFile> BuildDiagnosticBundle::files() const noexcept
{
    return m_files;
}

BuildDiagnosticPlanSnapshot make_build_diagnostic_plan_snapshot(const BuildPlan &a_plan,
                                                                const AssertContext &a_assertContext) noexcept
{
    try
    {
        return {std::string(a_plan.project_root()),
                std::string(a_plan.preset_name()),
                std::string(a_plan.workspace_key()),
                std::string(a_plan.binary_directory()),
                std::string(a_plan.candidate_directory()),
                std::string(a_plan.operation_directory()),
                std::string(a_plan.artifact_store_directory()),
                std::string(a_plan.cmake_target_name())};
    }
    catch (...)
    {
        terminate_bundle_exception(a_assertContext);
    }
}

Result<BuildDiagnosticBundle> create_build_diagnostic_bundle(const BuildDiagnosticBundleInput &a_input,
                                                             const BuildDiagnosticBundleLimits &a_limits,
                                                             const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!valid_limits(a_limits))
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidLimits, "Diagnostic bundle limits are invalid"));
        }
        if (!is_uuid_v4(a_input.operation.operationId) || !valid_terminal_state(a_input.operation.state) ||
            !valid_operation_artifact_state(a_input.operation) || a_input.plan.projectRoot.empty() ||
            a_input.plan.targetName.empty())
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidInput, "Diagnostic bundle input is invalid"));
        }
        if (a_input.environment && !valid_environment_enumerations(*a_input.environment))
        {
            return Result<BuildDiagnosticBundle>::failure(
                make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidInput,
                                  "Diagnostic environment contains unknown values"));
        }
        if (!std::all_of(a_input.operation.stages.begin(), a_input.operation.stages.end(), valid_stage_snapshot))
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidInput, "Diagnostic stages are inconsistent"));
        }
        if (!valid_succeeded_operation_stages(a_input.operation))
        {
            return Result<BuildDiagnosticBundle>::failure(
                make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidInput,
                                  "Successful diagnostic operation did not complete the build stage"));
        }
        if (!valid_operation_logs(a_input.operation))
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidInput, "Diagnostic logs are invalid"));
        }
        std::size_t candidateMappingCount = a_input.pathMappings.size();
        /// @brief 自動Mapping候補数を設定上限内で加算する
        const auto add_mapping_candidates = [&](std::size_t a_count) noexcept
        {
            if (a_count > a_limits.maximumPathMappings - candidateMappingCount)
            {
                return false;
            }
            candidateMappingCount += a_count;
            return true;
        };
        if (candidateMappingCount > a_limits.maximumPathMappings || !add_mapping_candidates(1U) ||
            (a_input.environment &&
             (!add_mapping_candidates(2U) || !add_mapping_candidates(a_input.environment->selectedTools.size()) ||
              !add_mapping_candidates(a_input.environment->selectedTools.size()) ||
              !add_mapping_candidates(a_input.environment->diagnostics.size()))))
        {
            return Result<BuildDiagnosticBundle>::failure(
                make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidInput,
                                  "Diagnostic path mapping count exceeds its configured limit"));
        }
        std::vector<BuildDiagnosticPathMapping> mappings;
        mappings.reserve(candidateMappingCount);
        for (const BuildDiagnosticPathMapping &mapping : a_input.pathMappings)
        {
            if (!valid_mapping(mapping))
            {
                return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                    a_assertContext, BuildDiagnosticBundleError::InvalidInput, "Diagnostic path mapping is invalid"));
            }
            mappings.push_back(mapping);
        }
        if (!add_mapping(mappings, a_input.plan.projectRoot, "<PROJECT_ROOT>"))
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidInput, "Diagnostic path mapping is invalid"));
        }
        if (a_input.environment)
        {
            if (!add_mapping(mappings, a_input.environment->engineSourceRoot, "<ENGINE_SOURCE_ROOT>") ||
                !add_mapping(mappings, a_input.environment->engineBinaryRoot, "<ENGINE_BINARY_ROOT>"))
            {
                return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                    a_assertContext, BuildDiagnosticBundleError::InvalidInput, "Diagnostic path mapping is invalid"));
            }
            for (std::size_t index = 0U; index < a_input.environment->selectedTools.size(); ++index)
            {
                if (!add_mapping(mappings, a_input.environment->selectedTools[index].installationRoot,
                                 "<TOOL_ROOT_" + std::to_string(index) + ">") ||
                    !add_mapping(mappings, a_input.environment->selectedTools[index].nativePath,
                                 "<TOOL_PATH_" + std::to_string(index) + ">"))
                {
                    return Result<BuildDiagnosticBundle>::failure(
                        make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidInput,
                                          "Diagnostic path mapping is invalid"));
                }
            }
            for (std::size_t index = 0U; index < a_input.environment->diagnostics.size(); ++index)
            {
                if (!add_mapping(mappings, a_input.environment->diagnostics[index].nativePath,
                                 "<DIAGNOSTIC_PATH_" + std::to_string(index) + ">"))
                {
                    return Result<BuildDiagnosticBundle>::failure(
                        make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidInput,
                                          "Diagnostic path mapping is invalid"));
                }
            }
        }
        if (!std::all_of(mappings.begin(), mappings.end(), valid_mapping))
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidInput, "Diagnostic path mapping is invalid"));
        }
        std::sort(mappings.begin(), mappings.end(),
                  /// @brief Longer Prefixを先に置いて包含Pathの部分置換を防ぐ
                  [](const auto &a_left, const auto &a_right) noexcept
                  { return a_left.nativePrefix.size() > a_right.nativePrefix.size(); });

        std::vector<BuildDiagnosticBundleFile> files;
        std::vector<BuildDiagnosticManifestEntry> entries;
        std::uint64_t totalBytes = 0U;
        /// @brief 一つの生成Fileへ上限を適用しManifest収集Entryも同時に記録する
        const auto collect = [&](std::string a_path, std::string a_text) -> std::optional<BuildDiagnosticBundleError>
        {
            if (!valid_utf8(a_text))
            {
                return BuildDiagnosticBundleError::InvalidInput;
            }
            const std::uint64_t size = static_cast<std::uint64_t>(a_text.size());
            if (auto failure = add_file(files, a_path, std::move(a_text), a_limits, totalBytes))
            {
                return failure;
            }
            entries.push_back({std::move(a_path), true, size, {}});
            return std::nullopt;
        };

        /// @brief 次のFileが使用できるFile単位とBundle全体の小さい方の上限を返す
        const auto maximum_payload_bytes = [&]() noexcept
        {
            return static_cast<std::size_t>(
                std::min(a_limits.maximumFileBytes, a_limits.maximumTotalBytes - totalBytes));
        };

        /// @brief Bounded Serializer結果を収集し上限超過種別を保持する
        const auto collect_serialized =
            [&](std::string a_path, std::optional<std::string> a_text) -> std::optional<BuildDiagnosticBundleError>
        {
            if (!a_text)
            {
                return maximum_payload_bytes() < a_limits.maximumFileBytes
                           ? BuildDiagnosticBundleError::TotalSizeLimitExceeded
                           : BuildDiagnosticBundleError::FileSizeLimitExceeded;
            }
            return collect(std::move(a_path), std::move(*a_text));
        };

        std::optional<BuildDiagnosticBundleError> failure =
            collect_serialized("plan.json", serialize_plan(a_input.plan, mappings, maximum_payload_bytes()));
        if (!failure && a_input.environment)
        {
            failure = collect_serialized(
                "environment.json", serialize_environment(*a_input.environment, mappings, maximum_payload_bytes()));
        }
        else if (!a_input.environment)
        {
            entries.push_back({"environment.json", false, 0U, "Environment report was not captured"});
        }
        if (!failure)
        {
            failure = collect_serialized("stages.json", serialize_stages(a_input.operation, maximum_payload_bytes()));
        }
        if (!failure)
        {
            failure = collect_serialized("result.json",
                                         serialize_result(a_input.operation, mappings, maximum_payload_bytes()));
        }
        if (!failure)
        {
            const std::uint64_t maximumBytes =
                std::min(a_limits.maximumFileBytes, a_limits.maximumTotalBytes - totalBytes);
            LogSerializationResult output = serialize_log(a_input.operation, ChildProcessStream::StandardOutput,
                                                          mappings, static_cast<std::size_t>(maximumBytes));
            failure =
                output.failure == LogSerializationFailure::None ? collect("stdout.log", std::move(output.bytes))
                : output.failure == LogSerializationFailure::InvalidInput
                    ? std::optional<BuildDiagnosticBundleError>(BuildDiagnosticBundleError::InvalidInput)
                    : std::optional<BuildDiagnosticBundleError>(
                          maximumBytes < a_limits.maximumFileBytes ? BuildDiagnosticBundleError::TotalSizeLimitExceeded
                                                                   : BuildDiagnosticBundleError::FileSizeLimitExceeded);
        }
        if (!failure)
        {
            const std::uint64_t maximumBytes =
                std::min(a_limits.maximumFileBytes, a_limits.maximumTotalBytes - totalBytes);
            LogSerializationResult output = serialize_log(a_input.operation, ChildProcessStream::StandardError,
                                                          mappings, static_cast<std::size_t>(maximumBytes));
            failure =
                output.failure == LogSerializationFailure::None ? collect("stderr.log", std::move(output.bytes))
                : output.failure == LogSerializationFailure::InvalidInput
                    ? std::optional<BuildDiagnosticBundleError>(BuildDiagnosticBundleError::InvalidInput)
                    : std::optional<BuildDiagnosticBundleError>(
                          maximumBytes < a_limits.maximumFileBytes ? BuildDiagnosticBundleError::TotalSizeLimitExceeded
                                                                   : BuildDiagnosticBundleError::FileSizeLimitExceeded);
        }
        if (!failure && (a_input.operation.artifact || a_input.operation.latestSuccessfulArtifact))
        {
            failure =
                collect_serialized("artifact.json", serialize_artifacts(a_input.operation, maximum_payload_bytes()));
        }
        else if (!a_input.operation.artifact && !a_input.operation.latestSuccessfulArtifact)
        {
            entries.push_back({"artifact.json", false, 0U, "No published artifact was available"});
        }
        if (failure)
        {
            return Result<BuildDiagnosticBundle>::failure(
                make_bundle_error(a_assertContext, *failure,
                                  *failure == BuildDiagnosticBundleError::InvalidInput
                                      ? "Diagnostic bundle contains invalid UTF-8"
                                      : "Diagnostic bundle exceeded its configured limits"));
        }
        std::sort(entries.begin(), entries.end(),
                  /// @brief Manifest EntryをSchema v1のPath昇順へ固定する
                  [](const auto &a_left, const auto &a_right) noexcept
                  { return a_left.relativePath < a_right.relativePath; });
        const std::string manifest =
            serialize_manifest(a_input.operation.operationId, a_input.operation.state, entries);
        failure = add_file(files, "manifest.json", manifest, a_limits, totalBytes);
        if (failure)
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, *failure, "Diagnostic bundle manifest exceeded its configured limits"));
        }
        std::sort(files.begin(), files.end(),
                  /// @brief Bundle Fileを決定的な相対Path順へ整列する
                  [](const auto &a_left, const auto &a_right) noexcept
                  { return a_left.relativePath < a_right.relativePath; });
        return Result<BuildDiagnosticBundle>::success(BuildDiagnosticBundle(
            a_input.operation.operationId, a_input.operation.state, std::move(entries), std::move(files)));
    }
    catch (...)
    {
        terminate_bundle_exception(a_assertContext);
    }
}

Result<void> write_build_diagnostic_bundle_directory(const BuildDiagnosticBundle &a_bundle,
                                                     std::string_view a_destination,
                                                     const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!valid_bundle_for_write(a_bundle))
        {
            return Result<void>::failure(make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidBundle,
                                                           "Diagnostic bundle is invalid"));
        }
        const auto destination = filesystem_path_from_utf8(a_destination);
        if (!destination || !destination->is_absolute())
        {
            return Result<void>::failure(make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidInput,
                                                           "Diagnostic destination must be absolute"));
        }
        const std::filesystem::path nativeDestination = native_inspection_path(*destination);
        std::error_code error;
        if (std::filesystem::exists(nativeDestination, error) || error)
        {
            return Result<void>::failure(make_bundle_error(a_assertContext,
                                                           error ? BuildDiagnosticBundleError::FilesystemFailure
                                                                 : BuildDiagnosticBundleError::DestinationAlreadyExists,
                                                           "Diagnostic destination is unavailable"));
        }
        const std::filesystem::path parent = destination->parent_path();
        const std::filesystem::path name = destination->filename();
        if (parent.empty() || name.empty())
        {
            return Result<void>::failure(make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidInput,
                                                           "Diagnostic destination has no parent or name"));
        }
        const std::filesystem::path nativeParent = native_inspection_path(parent);
        std::filesystem::create_directories(nativeParent, error);
        if (error)
        {
            return Result<void>::failure(make_bundle_error(a_assertContext,
                                                           BuildDiagnosticBundleError::FilesystemFailure,
                                                           "Diagnostic destination parent could not be created"));
        }
        const auto stagingSuffix = filesystem_path_from_utf8(".staging-" + std::string(a_bundle.operation_id()));
        if (!stagingSuffix)
        {
            return Result<void>::failure(make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidBundle,
                                                           "Diagnostic bundle operation ID is invalid"));
        }
        std::filesystem::path staging = parent / name;
        staging += *stagingSuffix;
        const std::filesystem::path nativeStaging = native_inspection_path(staging);
        if (std::filesystem::exists(nativeStaging, error) || error ||
            !std::filesystem::create_directory(nativeStaging, error) || error)
        {
            return Result<void>::failure(make_bundle_error(a_assertContext,
                                                           BuildDiagnosticBundleError::FilesystemFailure,
                                                           "Diagnostic staging destination is unavailable"));
        }
        bool stagingOwned = true;
        /// @brief この呼出しが所有するStagingだけをRollbackして失敗を返す
        const auto fail = [&](BuildDiagnosticBundleError a_code, std::string_view a_summary)
        {
            std::error_code cleanupError;
            if (stagingOwned)
            {
                std::filesystem::remove_all(nativeStaging, cleanupError);
            }
            return Result<void>::failure(make_bundle_error(a_assertContext, a_code, a_summary));
        };
        for (const BuildDiagnosticBundleFile &file : a_bundle.files())
        {
            if (!known_bundle_path(file.relativePath))
            {
                return fail(BuildDiagnosticBundleError::InvalidBundle, "Diagnostic bundle path is invalid");
            }
            std::ofstream stream(nativeStaging / file.relativePath, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char *>(file.bytes.data()),
                         static_cast<std::streamsize>(file.bytes.size()));
            stream.flush();
            if (!stream)
            {
                return fail(BuildDiagnosticBundleError::FilesystemFailure, "Diagnostic file write failed");
            }
        }
        std::filesystem::rename(nativeStaging, nativeDestination, error);
        if (error)
        {
            std::error_code destinationError;
            const bool destinationExists = std::filesystem::exists(nativeDestination, destinationError);
            return fail(destinationExists && !destinationError ? BuildDiagnosticBundleError::DestinationAlreadyExists
                                                               : BuildDiagnosticBundleError::FilesystemFailure,
                        "Diagnostic destination could not be published");
        }
        stagingOwned = false;
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_bundle_exception(a_assertContext);
    }
}

Result<BuildDiagnosticBundle> read_build_diagnostic_bundle_directory(std::string_view a_source,
                                                                     const BuildDiagnosticBundleLimits &a_limits,
                                                                     const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!valid_limits(a_limits))
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidLimits, "Diagnostic bundle limits are invalid"));
        }
        const auto source = filesystem_path_from_utf8(a_source);
        std::error_code error;
        const bool sourceIsReparsePoint = source && is_reparse_point(*source, error);
        const std::filesystem::path inspectionSource =
            source && !error ? native_inspection_path(*source) : std::filesystem::path{};
        const std::filesystem::file_status sourceStatus = source && !error
                                                              ? std::filesystem::symlink_status(inspectionSource, error)
                                                              : std::filesystem::file_status{};
        if (!source || !source->is_absolute() || error || sourceIsReparsePoint ||
            !std::filesystem::is_directory(sourceStatus))
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::FilesystemFailure, "Diagnostic source is unavailable"));
        }
        std::vector<BuildDiagnosticBundleFile> files;
        std::uint64_t totalBytes = 0U;
        std::optional<BuildDiagnosticBundleError> readFailure;
        for (std::filesystem::directory_iterator iterator(inspectionSource, error), end; iterator != end && !error;
             iterator.increment(error))
        {
            const bool entryIsReparsePoint = is_reparse_point(iterator->path(), error);
            if (error)
            {
                break;
            }
            const std::filesystem::file_status status = iterator->symlink_status(error);
            if (error)
            {
                break;
            }
            if (entryIsReparsePoint || !std::filesystem::is_regular_file(status))
            {
                readFailure = BuildDiagnosticBundleError::InvalidBundle;
                break;
            }
            const std::string path = iterator->path().filename().generic_string();
            if (!known_bundle_path(path))
            {
                readFailure = BuildDiagnosticBundleError::InvalidBundle;
                break;
            }
            if (files.size() >= a_limits.maximumFileCount)
            {
                readFailure = BuildDiagnosticBundleError::FileCountLimitExceeded;
                break;
            }
            const std::uint64_t size = iterator->file_size(error);
            if (error)
            {
                break;
            }
            if (size > a_limits.maximumFileBytes ||
                size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
            {
                readFailure = BuildDiagnosticBundleError::FileSizeLimitExceeded;
                break;
            }
            if (size > a_limits.maximumTotalBytes - totalBytes)
            {
                readFailure = BuildDiagnosticBundleError::TotalSizeLimitExceeded;
                break;
            }
            std::ifstream stream(iterator->path(), std::ios::binary);
            if (!stream.is_open())
            {
                error = std::make_error_code(std::errc::io_error);
                break;
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!stream && !bytes.empty())
            {
                error = std::make_error_code(std::errc::io_error);
                break;
            }
            if (!valid_bundle_text(bytes))
            {
                readFailure = BuildDiagnosticBundleError::InvalidBundle;
                break;
            }
            totalBytes += size;
            files.push_back({path, std::move(bytes)});
        }
        if (error)
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::FilesystemFailure, "Diagnostic directory read failed"));
        }
        if (readFailure)
        {
            return Result<BuildDiagnosticBundle>::failure(
                make_bundle_error(a_assertContext, *readFailure, "Diagnostic directory violates the bundle policy"));
        }
        std::sort(files.begin(), files.end(),
                  /// @brief 再読込Fileを決定的な相対Path順へ整列する
                  [](const auto &a_left, const auto &a_right) noexcept
                  { return a_left.relativePath < a_right.relativePath; });
        const auto manifestFile =
            std::find_if(files.begin(), files.end(),
                         /// @brief Bundle SchemaのManifest Fileを検出する
                         [](const auto &a_file) noexcept { return a_file.relativePath == "manifest.json"; });
        if (manifestFile == files.end())
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidBundle, "Diagnostic manifest is missing"));
        }
        const std::string manifest = from_bytes(manifestFile->bytes);
        const auto operationId = extract_quoted_value(manifest, "\"operationId\":\"");
        const auto stateValue = extract_quoted_value(manifest, "\"state\":\"");
        const auto state = stateValue ? parse_state(*stateValue) : std::nullopt;
        if (manifest.find("\"schemaVersion\":1") == std::string::npos || !operationId || !is_uuid_v4(*operationId) ||
            !state)
        {
            return Result<BuildDiagnosticBundle>::failure(make_bundle_error(
                a_assertContext, BuildDiagnosticBundleError::InvalidBundle, "Diagnostic manifest is invalid"));
        }
        std::vector<BuildDiagnosticManifestEntry> entries;
        std::size_t begin = 0U;
        while (begin < manifest.size())
        {
            const std::size_t end = manifest.find('\n', begin);
            const std::string_view line(manifest.data() + begin,
                                        (end == std::string::npos ? manifest.size() : end) - begin);
            if (line.starts_with("{\"path\":"))
            {
                BuildDiagnosticManifestEntry entry;
                if (!parse_manifest_entry(line, entry))
                {
                    return Result<BuildDiagnosticBundle>::failure(
                        make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidBundle,
                                          "Diagnostic manifest entry is invalid"));
                }
                entries.push_back(std::move(entry));
            }
            if (end == std::string::npos)
            {
                break;
            }
            begin = end + 1U;
        }
        if (!valid_manifest_entries(entries, *state) || manifest != serialize_manifest(*operationId, *state, entries) ||
            !files_match_manifest(files, entries) || !valid_payload_schemas(files, *state))
        {
            return Result<BuildDiagnosticBundle>::failure(
                make_bundle_error(a_assertContext, BuildDiagnosticBundleError::InvalidBundle,
                                  "Diagnostic manifest does not match directory files"));
        }
        return Result<BuildDiagnosticBundle>::success(
            BuildDiagnosticBundle(std::string(*operationId), *state, std::move(entries), std::move(files)));
    }
    catch (...)
    {
        terminate_bundle_exception(a_assertContext);
    }
}
} // namespace cue
