#include <Cue/Distribution/InstallState.h>

#include <Cue/Distribution/Error.h>
#include <Cue/Distribution/Publisher.h>
#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_maximumInstallStateBytes = 16U * 1024U * 1024U;
constexpr std::size_t k_maximumInstalledVersions = 4096U;
constexpr std::size_t k_maximumRecoveryItems = 4096U;

/// @brief Allocation失敗をDistribution Fatalへ変換する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Cue.Distribution install state allocation failed");
}

/// @brief 予期しない例外をDistribution Fatalへ変換する
[[noreturn]] void terminate_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Cue.Distribution install state unexpected exception");
}

/// @brief Canonical Install State文字列として安全なportable ASCIIか返す
[[nodiscard]] bool is_safe_string(std::string_view a_value, bool a_allowEmpty = false) noexcept
{
    if ((!a_allowEmpty && a_value.empty()) || a_value.size() > 4096U)
    {
        return false;
    }
    /// @brief Canonical JSONへEscapeなしで書けるASCII文字だけを許可する
    return std::ranges::all_of(a_value,
                               [](char a_character) noexcept
                               {
                                   const auto value = static_cast<unsigned char>(a_character);
                                   return value >= 0x20U && value <= 0x7eU && a_character != '"' && a_character != '\\';
                               });
}

/// @brief JSON文字列をEscape不要の検証済み値として追加する
void append_string(std::string &a_output, std::string_view a_value)
{
    a_output.push_back('"');
    a_output.append(a_value);
    a_output.push_back('"');
}

/// @brief 長さ付きFieldをIdentity Byte列へ追加する
void append_identity_field(std::string &a_output, std::string_view a_value)
{
    a_output.append(std::to_string(a_value.size()));
    a_output.push_back(':');
    a_output.append(a_value);
}

/// @brief uint64値を長さ付きFieldへ追加する
void append_identity_number(std::string &a_output, std::uint64_t a_value)
{
    const std::string value = std::to_string(a_value);
    append_identity_field(a_output, value);
}

/// @brief String Byte列をDistribution SHA-256境界へ渡す
[[nodiscard]] cue::Result<std::string> hash_string(std::string_view a_value,
                                                   const cue::AssertContext &a_assertContext) noexcept
{
    const auto *begin = reinterpret_cast<const std::byte *>(a_value.data());
    return cue::distribution::compute_distribution_sha256(std::span(begin, a_value.size()), a_assertContext);
}

/// @brief Versions直下のCanonical Directory名か返す
[[nodiscard]] bool is_version_directory(std::string_view a_value) noexcept
{
    if (a_value.size() < 42U || a_value.front() != 'v')
    {
        return false;
    }
    const std::size_t separator = a_value.find("--", 1U);
    if (separator == std::string_view::npos)
    {
        return false;
    }
    const std::string_view version = a_value.substr(1U, separator - 1U);
    const std::string_view bundleId = a_value.substr(separator + 2U);
    return cue::distribution::is_canonical_engine_version(version) &&
           cue::distribution::is_canonical_bundle_id(bundleId);
}

/// @brief Installed Version Entryの全IdentityとEvidenceを検証する
[[nodiscard]] bool is_valid_version_entry(const cue::distribution::InstalledVersionEntry &a_entry) noexcept
{
    using namespace cue::distribution;
    if (!is_version_directory(a_entry.directoryName) || !is_canonical_engine_version(a_entry.engineVersion) ||
        !is_canonical_bundle_id(a_entry.bundleId) || !is_canonical_sha256(a_entry.manifestDigest) ||
        !is_canonical_sha256(a_entry.payloadMarkerDigest) || !is_canonical_sha256(a_entry.probeMarkerDigest) ||
        !is_canonical_sha256(a_entry.workerId) || !is_canonical_sha256(a_entry.workerExecutableDigest) ||
        !is_canonical_sha256(a_entry.workerMarkerDigest) ||
        (a_entry.state != InstalledVersionState::Selectable && a_entry.state != InstalledVersionState::PendingRemoval))
    {
        return false;
    }
    const std::string expected = "v" + a_entry.engineVersion + "--" + a_entry.bundleId;
    return a_entry.directoryName == expected;
}

/// @brief Registryの順序、選択Version、Revisionを検証する
[[nodiscard]] bool is_valid_registry(const cue::distribution::InstalledVersionsRegistry &a_registry) noexcept
{
    using namespace cue::distribution;
    if (!is_canonical_bundle_id(a_registry.generationId) || a_registry.revision == 0U ||
        a_registry.versions.size() > k_maximumInstalledVersions ||
        (!a_registry.selectedVersion.empty() && !is_version_directory(a_registry.selectedVersion)))
    {
        return false;
    }
    std::string_view previous;
    bool foundSelection = a_registry.selectedVersion.empty();
    for (const InstalledVersionEntry &version : a_registry.versions)
    {
        if (!is_valid_version_entry(version) || (!previous.empty() && previous >= version.directoryName))
        {
            return false;
        }
        if (version.directoryName == a_registry.selectedVersion)
        {
            foundSelection = version.state == InstalledVersionState::Selectable;
        }
        previous = version.directoryName;
    }
    return foundSelection;
}

/// @brief Kindが指定Stageをv1列挙として許可するか返す
[[nodiscard]] bool is_stage_for_kind(cue::distribution::InstallOperationKind a_kind,
                                     cue::distribution::InstallOperationStage a_stage) noexcept
{
    using enum cue::distribution::InstallOperationKind;
    using enum cue::distribution::InstallOperationStage;
    switch (a_kind)
    {
    case Install:
    case Update:
        return a_stage == Prepared || a_stage == PayloadStaged || a_stage == VersionPublished ||
               a_stage == ProbeSucceeded || a_stage == WorkerPublished || a_stage == RegistryPublished;
    case Rollback:
        return a_stage == Prepared || a_stage == SelectionPublished;
    case Uninstall:
        return a_stage == Prepared || a_stage == RemovalBlocked || a_stage == VersionQuarantined ||
               a_stage == RegistryEntryRemoved;
    case RegistryRecovery:
        return a_stage == Prepared || a_stage == CandidatesValidated || a_stage == RegistryPublished;
    }
    return false;
}

/// @brief Source Registry EvidenceのDiscriminated Contractを検証する
[[nodiscard]] bool is_valid_source_evidence(const cue::distribution::RegistrySourceEvidence &a_evidence) noexcept
{
    if (a_evidence.wasMissing)
    {
        return a_evidence.evidenceName.empty() && a_evidence.byteSize == 0U && a_evidence.sha256.empty();
    }
    constexpr std::string_view prefix = "InstalledVersions.corrupt-";
    constexpr std::string_view suffix = ".json";
    if (!a_evidence.evidenceName.starts_with(prefix) || !a_evidence.evidenceName.ends_with(suffix) ||
        a_evidence.evidenceName.size() != prefix.size() + 36U + suffix.size())
    {
        return false;
    }
    const std::string_view evidenceId = std::string_view(a_evidence.evidenceName).substr(prefix.size(), 36U);
    return cue::distribution::is_canonical_bundle_id(evidenceId) &&
           cue::distribution::is_canonical_sha256(a_evidence.sha256);
}

/// @brief JournalのKind固有MemberとCanonical配列順を検証する
[[nodiscard]] bool is_valid_journal(const cue::distribution::InstallOperationJournal &a_journal) noexcept
{
    using namespace cue::distribution;
    if (!is_canonical_bundle_id(a_journal.operationId) || !is_canonical_sha256(a_journal.workerId) ||
        !is_stage_for_kind(a_journal.kind, a_journal.stage) ||
        a_journal.blockedOperations.size() > k_maximumRecoveryItems ||
        a_journal.candidates.size() > k_maximumRecoveryItems)
    {
        return false;
    }
    const bool isRecovery = a_journal.kind == InstallOperationKind::RegistryRecovery;
    const bool hasWorkerEvidence =
        is_canonical_sha256(a_journal.workerExecutableDigest) && is_canonical_sha256(a_journal.workerMarkerDigest);
    const bool hasNoWorkerEvidence = a_journal.workerExecutableDigest.empty() && a_journal.workerMarkerDigest.empty();
    if (isRecovery != a_journal.sourceRegistryEvidence.has_value() ||
        isRecovery == a_journal.expectedRegistry.has_value() || isRecovery == a_journal.target.has_value() ||
        (a_journal.kind == InstallOperationKind::Uninstall ? !hasWorkerEvidence : !hasNoWorkerEvidence))
    {
        return false;
    }
    if (!isRecovery)
    {
        if (!is_canonical_bundle_id(a_journal.expectedRegistry->generationId) ||
            a_journal.expectedRegistry->revision == 0U || !is_version_directory(a_journal.target->directoryName) ||
            !is_canonical_bundle_id(a_journal.target->bundleId) ||
            !is_canonical_sha256(a_journal.target->manifestDigest) || !a_journal.blockedOperations.empty() ||
            !a_journal.candidates.empty())
        {
            return false;
        }
        return true;
    }
    if (!is_valid_source_evidence(*a_journal.sourceRegistryEvidence))
    {
        return false;
    }
    std::string_view previousOperation;
    for (const BlockedInstallOperation &blocked : a_journal.blockedOperations)
    {
        const bool kindAllowed = blocked.kind == InstallOperationKind::Install ||
                                 blocked.kind == InstallOperationKind::Update ||
                                 blocked.kind == InstallOperationKind::Uninstall;
        if (!kindAllowed || !is_canonical_bundle_id(blocked.operationId) ||
            !is_version_directory(blocked.directoryName) || !is_stage_for_kind(blocked.kind, blocked.stage) ||
            !is_canonical_sha256(blocked.journalDigest) ||
            (!previousOperation.empty() && previousOperation >= blocked.operationId))
        {
            return false;
        }
        previousOperation = blocked.operationId;
    }
    std::string_view previousVersion;
    for (const RegistryRecoveryCandidate &candidate : a_journal.candidates)
    {
        if (!is_valid_version_entry(candidate.version) ||
            (!previousVersion.empty() && previousVersion >= candidate.version.directoryName))
        {
            return false;
        }
        previousVersion = candidate.version.directoryName;
    }
    if (a_journal.stage == InstallOperationStage::Prepared)
    {
        return a_journal.blockedOperations.empty() && a_journal.candidates.empty();
    }
    return true;
}

/// @brief Payload完了MarkerのIdentityを検証する
[[nodiscard]] bool is_valid_payload_marker(const cue::distribution::PayloadCompleteMarker &a_marker) noexcept
{
    return is_version_directory(a_marker.directoryName) &&
           cue::distribution::is_canonical_bundle_id(a_marker.bundleId) &&
           cue::distribution::is_canonical_sha256(a_marker.manifestDigest);
}

/// @brief Probe成功MarkerのOperationとVersion Identityを検証する
[[nodiscard]] bool is_valid_probe_marker(const cue::distribution::InstallProbeMarker &a_marker) noexcept
{
    return cue::distribution::is_canonical_bundle_id(a_marker.operationId) &&
           is_version_directory(a_marker.directoryName) &&
           cue::distribution::is_canonical_bundle_id(a_marker.bundleId) &&
           cue::distribution::is_canonical_sha256(a_marker.manifestDigest);
}

/// @brief Worker MarkerのIdentityと固定Inventoryを検証する
[[nodiscard]] bool is_valid_worker_marker(const cue::distribution::InstallWorkerMarker &a_marker) noexcept
{
    using namespace cue::distribution;
    return is_canonical_sha256(a_marker.workerId) && is_canonical_bundle_id(a_marker.bundleId) &&
           is_canonical_git_revision(a_marker.engineSourceRevision) &&
           is_canonical_sha256(a_marker.publisherBuildIdentityDigest) &&
           a_marker.executable.role == DistributionFileRole::InstallWorker &&
           a_marker.executable.relativePath == "Bin/CueEngineInstallWorker.exe" && a_marker.executable.byteSize > 0U &&
           is_canonical_sha256(a_marker.executable.sha256) && a_marker.peArchitecture == DistributionArchitecture::X64;
}

/// @brief JSON読取位置を所有して既知Schemaだけを順に読む
class JsonCursor final
{
  public:
    /// @brief LFを除いたJSON本体へCursorをBindingする
    explicit JsonCursor(std::string_view a_bytes) noexcept : m_bytes(a_bytes)
    {
    }

    /// @brief 指定Tokenが現在位置に完全一致するときだけ進める
    [[nodiscard]] bool consume(std::string_view a_token) noexcept
    {
        if (!m_bytes.substr(m_position).starts_with(a_token))
        {
            return false;
        }
        m_position += a_token.size();
        return true;
    }

    /// @brief Escapeを持たないCanonical JSON文字列を読む
    [[nodiscard]] std::optional<std::string> read_string()
    {
        if (!consume("\""))
        {
            return std::nullopt;
        }
        const std::size_t start = m_position;
        while (m_position < m_bytes.size() && m_bytes[m_position] != '"')
        {
            const unsigned char value = static_cast<unsigned char>(m_bytes[m_position]);
            if (value < 0x20U || value > 0x7eU || m_bytes[m_position] == '\\')
            {
                return std::nullopt;
            }
            ++m_position;
        }
        if (m_position >= m_bytes.size())
        {
            return std::nullopt;
        }
        std::string value(m_bytes.substr(start, m_position - start));
        ++m_position;
        return value;
    }

    /// @brief 先頭Zeroなしuint64を読む
    [[nodiscard]] std::optional<std::uint64_t> read_number() noexcept
    {
        const std::size_t start = m_position;
        while (m_position < m_bytes.size() && m_bytes[m_position] >= '0' && m_bytes[m_position] <= '9')
        {
            ++m_position;
        }
        if (start == m_position || (m_position - start > 1U && m_bytes[start] == '0'))
        {
            return std::nullopt;
        }
        std::uint64_t value = 0U;
        const auto result = std::from_chars(m_bytes.data() + start, m_bytes.data() + m_position, value);
        if (result.ec != std::errc{} || result.ptr != m_bytes.data() + m_position)
        {
            return std::nullopt;
        }
        return value;
    }

    /// @brief JSON本体末尾へ到達したか返す
    [[nodiscard]] bool at_end() const noexcept
    {
        return m_position == m_bytes.size();
    }

  private:
    std::string_view m_bytes;
    std::size_t m_position = 0U;
};

/// @brief 固定名MemberのString値を読む
[[nodiscard]] std::optional<std::string> read_string_member(JsonCursor &a_cursor, std::string_view a_name,
                                                            std::string_view a_prefix)
{
    std::string token(a_prefix);
    token.push_back('"');
    token.append(a_name);
    token.append("\":");
    if (!a_cursor.consume(token))
    {
        return std::nullopt;
    }
    return a_cursor.read_string();
}

/// @brief 固定名Memberのuint64値を読む
[[nodiscard]] std::optional<std::uint64_t> read_number_member(JsonCursor &a_cursor, std::string_view a_name,
                                                              std::string_view a_prefix) noexcept
{
    std::string token(a_prefix);
    token.push_back('"');
    token.append(a_name);
    token.append("\":");
    if (!a_cursor.consume(token))
    {
        return std::nullopt;
    }
    return a_cursor.read_number();
}

/// @brief Canonical Kind名をenumへ変換する
[[nodiscard]] std::optional<cue::distribution::InstallOperationKind> parse_kind(std::string_view a_value) noexcept
{
    using enum cue::distribution::InstallOperationKind;
    if (a_value == "install")
        return Install;
    if (a_value == "update")
        return Update;
    if (a_value == "rollback")
        return Rollback;
    if (a_value == "uninstall")
        return Uninstall;
    if (a_value == "registryRecovery")
        return RegistryRecovery;
    return std::nullopt;
}

/// @brief Canonical Stage名をenumへ変換する
[[nodiscard]] std::optional<cue::distribution::InstallOperationStage> parse_stage(std::string_view a_value) noexcept
{
    using enum cue::distribution::InstallOperationStage;
    constexpr std::array values = {
        std::pair{std::string_view("prepared"), Prepared},
        std::pair{std::string_view("payloadStaged"), PayloadStaged},
        std::pair{std::string_view("versionPublished"), VersionPublished},
        std::pair{std::string_view("probeSucceeded"), ProbeSucceeded},
        std::pair{std::string_view("workerPublished"), WorkerPublished},
        std::pair{std::string_view("registryPublished"), RegistryPublished},
        std::pair{std::string_view("selectionPublished"), SelectionPublished},
        std::pair{std::string_view("removalBlocked"), RemovalBlocked},
        std::pair{std::string_view("versionQuarantined"), VersionQuarantined},
        std::pair{std::string_view("registryEntryRemoved"), RegistryEntryRemoved},
        std::pair{std::string_view("candidatesValidated"), CandidatesValidated},
    };
    /// @brief Canonical文字列に対応する列挙値だけを検索する
    const auto iterator =
        std::ranges::find_if(values, [a_value](const auto &a_item) noexcept { return a_item.first == a_value; });
    return iterator == values.end() ? std::nullopt : std::optional(iterator->second);
}

/// @brief Canonical Version State名をenumへ変換する
[[nodiscard]] std::optional<cue::distribution::InstalledVersionState> parse_version_state(
    std::string_view a_value) noexcept
{
    if (a_value == "selectable")
    {
        return cue::distribution::InstalledVersionState::Selectable;
    }
    if (a_value == "pendingRemoval")
    {
        return cue::distribution::InstalledVersionState::PendingRemoval;
    }
    return std::nullopt;
}

/// @brief Installed Version Entryを固定Member順で追加する
void append_version_entry(std::string &a_output, const cue::distribution::InstalledVersionEntry &a_entry)
{
    a_output.append("{\"directoryName\":");
    append_string(a_output, a_entry.directoryName);
    a_output.append(",\"engineVersion\":");
    append_string(a_output, a_entry.engineVersion);
    a_output.append(",\"bundleId\":");
    append_string(a_output, a_entry.bundleId);
    a_output.append(",\"manifestDigest\":");
    append_string(a_output, a_entry.manifestDigest);
    a_output.append(",\"payloadMarkerDigest\":");
    append_string(a_output, a_entry.payloadMarkerDigest);
    a_output.append(",\"probeMarkerDigest\":");
    append_string(a_output, a_entry.probeMarkerDigest);
    a_output.append(",\"workerId\":");
    append_string(a_output, a_entry.workerId);
    a_output.append(",\"workerExecutableDigest\":");
    append_string(a_output, a_entry.workerExecutableDigest);
    a_output.append(",\"workerMarkerDigest\":");
    append_string(a_output, a_entry.workerMarkerDigest);
    a_output.append(",\"state\":");
    append_string(a_output, cue::distribution::installed_version_state_name(a_entry.state));
    a_output.push_back('}');
}

/// @brief Installed Version Entryを固定Member順で読む
[[nodiscard]] std::optional<cue::distribution::InstalledVersionEntry> read_version_entry(JsonCursor &a_cursor)
{
    using cue::distribution::InstalledVersionEntry;
    InstalledVersionEntry entry;
    auto directory = read_string_member(a_cursor, "directoryName", "{");
    auto version = read_string_member(a_cursor, "engineVersion", ",");
    auto bundle = read_string_member(a_cursor, "bundleId", ",");
    auto manifest = read_string_member(a_cursor, "manifestDigest", ",");
    auto payload = read_string_member(a_cursor, "payloadMarkerDigest", ",");
    auto probe = read_string_member(a_cursor, "probeMarkerDigest", ",");
    auto worker = read_string_member(a_cursor, "workerId", ",");
    auto executable = read_string_member(a_cursor, "workerExecutableDigest", ",");
    auto marker = read_string_member(a_cursor, "workerMarkerDigest", ",");
    auto stateName = read_string_member(a_cursor, "state", ",");
    if (!directory || !version || !bundle || !manifest || !payload || !probe || !worker || !executable || !marker ||
        !stateName || !a_cursor.consume("}"))
    {
        return std::nullopt;
    }
    auto state = parse_version_state(*stateName);
    if (!state)
    {
        return std::nullopt;
    }
    entry.directoryName = std::move(*directory);
    entry.engineVersion = std::move(*version);
    entry.bundleId = std::move(*bundle);
    entry.manifestDigest = std::move(*manifest);
    entry.payloadMarkerDigest = std::move(*payload);
    entry.probeMarkerDigest = std::move(*probe);
    entry.workerId = std::move(*worker);
    entry.workerExecutableDigest = std::move(*executable);
    entry.workerMarkerDigest = std::move(*marker);
    entry.state = *state;
    return entry;
}

/// @brief Optional Expected Registry Objectを追加する
void append_expected_registry(std::string &a_output,
                              const std::optional<cue::distribution::ExpectedRegistry> &a_expected)
{
    if (!a_expected)
    {
        a_output.append("null");
        return;
    }
    a_output.append("{\"generationId\":");
    append_string(a_output, a_expected->generationId);
    a_output.append(",\"revision\":");
    a_output.append(std::to_string(a_expected->revision));
    a_output.push_back('}');
}

/// @brief Optional Expected Registry Objectを読む
[[nodiscard]] bool read_expected_registry(JsonCursor &a_cursor,
                                          std::optional<cue::distribution::ExpectedRegistry> &a_output)
{
    if (a_cursor.consume("null"))
    {
        a_output.reset();
        return true;
    }
    auto generation = read_string_member(a_cursor, "generationId", "{");
    auto revision = read_number_member(a_cursor, "revision", ",");
    if (!generation || !revision || !a_cursor.consume("}"))
    {
        return false;
    }
    a_output = cue::distribution::ExpectedRegistry{std::move(*generation), *revision};
    return true;
}

/// @brief Optional Operation Target Objectを追加する
void append_target(std::string &a_output, const std::optional<cue::distribution::InstallOperationTarget> &a_target)
{
    if (!a_target)
    {
        a_output.append("null");
        return;
    }
    a_output.append("{\"directoryName\":");
    append_string(a_output, a_target->directoryName);
    a_output.append(",\"bundleId\":");
    append_string(a_output, a_target->bundleId);
    a_output.append(",\"manifestDigest\":");
    append_string(a_output, a_target->manifestDigest);
    a_output.push_back('}');
}

/// @brief Optional Operation Target Objectを読む
[[nodiscard]] bool read_target(JsonCursor &a_cursor, std::optional<cue::distribution::InstallOperationTarget> &a_output)
{
    if (a_cursor.consume("null"))
    {
        a_output.reset();
        return true;
    }
    auto directory = read_string_member(a_cursor, "directoryName", "{");
    auto bundle = read_string_member(a_cursor, "bundleId", ",");
    auto digest = read_string_member(a_cursor, "manifestDigest", ",");
    if (!directory || !bundle || !digest || !a_cursor.consume("}"))
    {
        return false;
    }
    a_output = cue::distribution::InstallOperationTarget{std::move(*directory), std::move(*bundle), std::move(*digest)};
    return true;
}

/// @brief Optional Source Registry Evidence Objectを追加する
void append_source_evidence(std::string &a_output,
                            const std::optional<cue::distribution::RegistrySourceEvidence> &a_evidence)
{
    if (!a_evidence)
    {
        a_output.append("null");
        return;
    }
    if (a_evidence->wasMissing)
    {
        a_output.append("{\"kind\":\"missing\"}");
        return;
    }
    a_output.append("{\"kind\":\"corrupt\",\"evidenceName\":");
    append_string(a_output, a_evidence->evidenceName);
    a_output.append(",\"byteSize\":");
    a_output.append(std::to_string(a_evidence->byteSize));
    a_output.append(",\"sha256\":");
    append_string(a_output, a_evidence->sha256);
    a_output.push_back('}');
}

/// @brief Optional Source Registry Evidence Objectを読む
[[nodiscard]] bool read_source_evidence(JsonCursor &a_cursor,
                                        std::optional<cue::distribution::RegistrySourceEvidence> &a_output)
{
    if (a_cursor.consume("null"))
    {
        a_output.reset();
        return true;
    }
    auto kind = read_string_member(a_cursor, "kind", "{");
    if (!kind)
    {
        return false;
    }
    if (*kind == "missing")
    {
        if (!a_cursor.consume("}"))
        {
            return false;
        }
        a_output = cue::distribution::RegistrySourceEvidence{};
        return true;
    }
    if (*kind != "corrupt")
    {
        return false;
    }
    auto name = read_string_member(a_cursor, "evidenceName", ",");
    auto size = read_number_member(a_cursor, "byteSize", ",");
    auto digest = read_string_member(a_cursor, "sha256", ",");
    if (!name || !size || !digest || !a_cursor.consume("}"))
    {
        return false;
    }
    a_output = cue::distribution::RegistrySourceEvidence{false, std::move(*name), *size, std::move(*digest)};
    return true;
}

/// @brief Registryを検証済みCanonical Byte列へ変換する
[[nodiscard]] std::string serialize_registry(const cue::distribution::InstalledVersionsRegistry &a_registry)
{
    std::string output;
    output.append("{\"schemaVersion\":1,\"generationId\":");
    append_string(output, a_registry.generationId);
    output.append(",\"revision\":");
    output.append(std::to_string(a_registry.revision));
    output.append(",\"selectedVersion\":");
    append_string(output, a_registry.selectedVersion);
    output.append(",\"versions\":[");
    for (std::size_t index = 0U; index < a_registry.versions.size(); ++index)
    {
        if (index != 0U)
        {
            output.push_back(',');
        }
        append_version_entry(output, a_registry.versions[index]);
    }
    output.append("]}\n");
    return output;
}

/// @brief Registry Byte列を既知SchemaへParseする
[[nodiscard]] std::optional<cue::distribution::InstalledVersionsRegistry> parse_registry(std::string_view a_bytes)
{
    if (a_bytes.empty() || a_bytes.size() > k_maximumInstallStateBytes || a_bytes.back() != '\n')
    {
        return std::nullopt;
    }
    JsonCursor cursor(a_bytes.substr(0U, a_bytes.size() - 1U));
    if (!cursor.consume("{\"schemaVersion\":1"))
    {
        return std::nullopt;
    }
    auto generation = read_string_member(cursor, "generationId", ",");
    auto revision = read_number_member(cursor, "revision", ",");
    auto selected = read_string_member(cursor, "selectedVersion", ",");
    if (!generation || !revision || !selected || !cursor.consume(",\"versions\":["))
    {
        return std::nullopt;
    }
    cue::distribution::InstalledVersionsRegistry registry{std::move(*generation), *revision, std::move(*selected), {}};
    if (!cursor.consume("]"))
    {
        while (true)
        {
            auto version = read_version_entry(cursor);
            if (!version)
            {
                return std::nullopt;
            }
            registry.versions.push_back(std::move(*version));
            if (cursor.consume("]"))
            {
                break;
            }
            if (!cursor.consume(","))
            {
                return std::nullopt;
            }
        }
    }
    return cursor.consume("}") && cursor.at_end() ? std::optional(std::move(registry)) : std::nullopt;
}

/// @brief Journalを検証済みCanonical Byte列へ変換する
[[nodiscard]] std::string serialize_journal(const cue::distribution::InstallOperationJournal &a_journal)
{
    using namespace cue::distribution;
    std::string output;
    output.append("{\"schemaVersion\":1,\"operationId\":");
    append_string(output, a_journal.operationId);
    output.append(",\"kind\":");
    append_string(output, install_operation_kind_name(a_journal.kind));
    output.append(",\"stage\":");
    append_string(output, install_operation_stage_name(a_journal.stage));
    output.append(",\"workerId\":");
    append_string(output, a_journal.workerId);
    output.append(",\"workerExecutableDigest\":");
    append_string(output, a_journal.workerExecutableDigest);
    output.append(",\"workerMarkerDigest\":");
    append_string(output, a_journal.workerMarkerDigest);
    output.append(",\"expectedRegistry\":");
    append_expected_registry(output, a_journal.expectedRegistry);
    output.append(",\"target\":");
    append_target(output, a_journal.target);
    output.append(",\"sourceRegistryEvidence\":");
    append_source_evidence(output, a_journal.sourceRegistryEvidence);
    output.append(",\"blockedOperations\":[");
    for (std::size_t index = 0U; index < a_journal.blockedOperations.size(); ++index)
    {
        if (index != 0U)
        {
            output.push_back(',');
        }
        const BlockedInstallOperation &blocked = a_journal.blockedOperations[index];
        output.append("{\"operationId\":");
        append_string(output, blocked.operationId);
        output.append(",\"kind\":");
        append_string(output, install_operation_kind_name(blocked.kind));
        output.append(",\"directoryName\":");
        append_string(output, blocked.directoryName);
        output.append(",\"stage\":");
        append_string(output, install_operation_stage_name(blocked.stage));
        output.append(",\"journalDigest\":");
        append_string(output, blocked.journalDigest);
        output.push_back('}');
    }
    output.append("],\"candidates\":[");
    for (std::size_t index = 0U; index < a_journal.candidates.size(); ++index)
    {
        if (index != 0U)
        {
            output.push_back(',');
        }
        append_version_entry(output, a_journal.candidates[index].version);
    }
    output.append("]}\n");
    return output;
}

/// @brief Journal Byte列を既知SchemaへParseする
[[nodiscard]] std::optional<cue::distribution::InstallOperationJournal> parse_journal(std::string_view a_bytes)
{
    using namespace cue::distribution;
    if (a_bytes.empty() || a_bytes.size() > k_maximumInstallStateBytes || a_bytes.back() != '\n')
    {
        return std::nullopt;
    }
    JsonCursor cursor(a_bytes.substr(0U, a_bytes.size() - 1U));
    if (!cursor.consume("{\"schemaVersion\":1"))
    {
        return std::nullopt;
    }
    auto operation = read_string_member(cursor, "operationId", ",");
    auto kindName = read_string_member(cursor, "kind", ",");
    auto stageName = read_string_member(cursor, "stage", ",");
    auto worker = read_string_member(cursor, "workerId", ",");
    auto workerExecutableDigest = read_string_member(cursor, "workerExecutableDigest", ",");
    auto workerMarkerDigest = read_string_member(cursor, "workerMarkerDigest", ",");
    auto kind = kindName ? parse_kind(*kindName) : std::nullopt;
    auto stage = stageName ? parse_stage(*stageName) : std::nullopt;
    if (!operation || !worker || !workerExecutableDigest || !workerMarkerDigest || !kind || !stage ||
        !cursor.consume(",\"expectedRegistry\":"))
    {
        return std::nullopt;
    }
    InstallOperationJournal journal;
    journal.operationId = std::move(*operation);
    journal.kind = *kind;
    journal.stage = *stage;
    journal.workerId = std::move(*worker);
    journal.workerExecutableDigest = std::move(*workerExecutableDigest);
    journal.workerMarkerDigest = std::move(*workerMarkerDigest);
    if (!read_expected_registry(cursor, journal.expectedRegistry) || !cursor.consume(",\"target\":") ||
        !read_target(cursor, journal.target) || !cursor.consume(",\"sourceRegistryEvidence\":") ||
        !read_source_evidence(cursor, journal.sourceRegistryEvidence) || !cursor.consume(",\"blockedOperations\":["))
    {
        return std::nullopt;
    }
    if (!cursor.consume("]"))
    {
        while (true)
        {
            auto blockedOperation = read_string_member(cursor, "operationId", "{");
            auto blockedKindName = read_string_member(cursor, "kind", ",");
            auto directory = read_string_member(cursor, "directoryName", ",");
            auto blockedStageName = read_string_member(cursor, "stage", ",");
            auto digest = read_string_member(cursor, "journalDigest", ",");
            auto blockedKind = blockedKindName ? parse_kind(*blockedKindName) : std::nullopt;
            auto blockedStage = blockedStageName ? parse_stage(*blockedStageName) : std::nullopt;
            if (!blockedOperation || !blockedKind || !directory || !blockedStage || !digest || !cursor.consume("}"))
            {
                return std::nullopt;
            }
            journal.blockedOperations.push_back(BlockedInstallOperation{
                std::move(*blockedOperation), *blockedKind, std::move(*directory), *blockedStage, std::move(*digest)});
            if (cursor.consume("]"))
            {
                break;
            }
            if (!cursor.consume(","))
            {
                return std::nullopt;
            }
        }
    }
    if (!cursor.consume(",\"candidates\":["))
    {
        return std::nullopt;
    }
    if (!cursor.consume("]"))
    {
        while (true)
        {
            auto candidate = read_version_entry(cursor);
            if (!candidate)
            {
                return std::nullopt;
            }
            journal.candidates.push_back(RegistryRecoveryCandidate{std::move(*candidate)});
            if (cursor.consume("]"))
            {
                break;
            }
            if (!cursor.consume(","))
            {
                return std::nullopt;
            }
        }
    }
    return cursor.consume("}") && cursor.at_end() ? std::optional(std::move(journal)) : std::nullopt;
}

/// @brief 三つのIdentity文字列を持つMarkerを固定順で生成する
[[nodiscard]] std::string serialize_payload_marker(const cue::distribution::PayloadCompleteMarker &a_marker)
{
    std::string output("{\"schemaVersion\":1,\"directoryName\":");
    append_string(output, a_marker.directoryName);
    output.append(",\"bundleId\":");
    append_string(output, a_marker.bundleId);
    output.append(",\"manifestDigest\":");
    append_string(output, a_marker.manifestDigest);
    output.append("}\n");
    return output;
}

/// @brief Payload Marker Byte列を既知SchemaへParseする
[[nodiscard]] std::optional<cue::distribution::PayloadCompleteMarker> parse_payload_marker(std::string_view a_bytes)
{
    if (a_bytes.empty() || a_bytes.back() != '\n')
    {
        return std::nullopt;
    }
    JsonCursor cursor(a_bytes.substr(0U, a_bytes.size() - 1U));
    if (!cursor.consume("{\"schemaVersion\":1"))
    {
        return std::nullopt;
    }
    auto directory = read_string_member(cursor, "directoryName", ",");
    auto bundle = read_string_member(cursor, "bundleId", ",");
    auto digest = read_string_member(cursor, "manifestDigest", ",");
    if (!directory || !bundle || !digest || !cursor.consume("}") || !cursor.at_end())
    {
        return std::nullopt;
    }
    return cue::distribution::PayloadCompleteMarker{std::move(*directory), std::move(*bundle), std::move(*digest)};
}

/// @brief Probe Markerを固定順で生成する
[[nodiscard]] std::string serialize_probe_marker(const cue::distribution::InstallProbeMarker &a_marker)
{
    std::string output("{\"schemaVersion\":1,\"operationId\":");
    append_string(output, a_marker.operationId);
    output.append(",\"directoryName\":");
    append_string(output, a_marker.directoryName);
    output.append(",\"bundleId\":");
    append_string(output, a_marker.bundleId);
    output.append(",\"manifestDigest\":");
    append_string(output, a_marker.manifestDigest);
    output.append("}\n");
    return output;
}

/// @brief Probe Marker Byte列を既知SchemaへParseする
[[nodiscard]] std::optional<cue::distribution::InstallProbeMarker> parse_probe_marker(std::string_view a_bytes)
{
    if (a_bytes.empty() || a_bytes.back() != '\n')
    {
        return std::nullopt;
    }
    JsonCursor cursor(a_bytes.substr(0U, a_bytes.size() - 1U));
    if (!cursor.consume("{\"schemaVersion\":1"))
    {
        return std::nullopt;
    }
    auto operation = read_string_member(cursor, "operationId", ",");
    auto directory = read_string_member(cursor, "directoryName", ",");
    auto bundle = read_string_member(cursor, "bundleId", ",");
    auto digest = read_string_member(cursor, "manifestDigest", ",");
    if (!operation || !directory || !bundle || !digest || !cursor.consume("}") || !cursor.at_end())
    {
        return std::nullopt;
    }
    return cue::distribution::InstallProbeMarker{std::move(*operation), std::move(*directory), std::move(*bundle),
                                                 std::move(*digest)};
}

/// @brief Worker Markerを固定順で生成する
[[nodiscard]] std::string serialize_worker_marker(const cue::distribution::InstallWorkerMarker &a_marker)
{
    std::string output("{\"schemaVersion\":1,\"workerId\":");
    append_string(output, a_marker.workerId);
    output.append(",\"bundleId\":");
    append_string(output, a_marker.bundleId);
    output.append(",\"engineSourceRevision\":");
    append_string(output, a_marker.engineSourceRevision);
    output.append(",\"publisherBuildIdentityDigest\":");
    append_string(output, a_marker.publisherBuildIdentityDigest);
    output.append(",\"executable\":{\"role\":\"installWorker\",\"relativePath\":");
    append_string(output, a_marker.executable.relativePath);
    output.append(",\"byteSize\":");
    output.append(std::to_string(a_marker.executable.byteSize));
    output.append(",\"sha256\":");
    append_string(output, a_marker.executable.sha256);
    output.append("},\"peArchitecture\":\"x64\"}\n");
    return output;
}

/// @brief Worker Marker Byte列を既知SchemaへParseする
[[nodiscard]] std::optional<cue::distribution::InstallWorkerMarker> parse_worker_marker(std::string_view a_bytes)
{
    using namespace cue::distribution;
    if (a_bytes.empty() || a_bytes.back() != '\n')
    {
        return std::nullopt;
    }
    JsonCursor cursor(a_bytes.substr(0U, a_bytes.size() - 1U));
    if (!cursor.consume("{\"schemaVersion\":1"))
    {
        return std::nullopt;
    }
    auto worker = read_string_member(cursor, "workerId", ",");
    auto bundle = read_string_member(cursor, "bundleId", ",");
    auto revision = read_string_member(cursor, "engineSourceRevision", ",");
    auto identity = read_string_member(cursor, "publisherBuildIdentityDigest", ",");
    auto role = read_string_member(cursor, "role", ",\"executable\":{");
    auto path = read_string_member(cursor, "relativePath", ",");
    auto size = read_number_member(cursor, "byteSize", ",");
    auto digest = read_string_member(cursor, "sha256", ",");
    if (!worker || !bundle || !revision || !identity || !role || *role != "installWorker" || !path || !size ||
        !digest || !cursor.consume("}"))
    {
        return std::nullopt;
    }
    auto architecture = read_string_member(cursor, "peArchitecture", ",");
    if (!architecture || *architecture != "x64" || !cursor.consume("}") || !cursor.at_end())
    {
        return std::nullopt;
    }
    DistributionFileEntry executable{DistributionFileRole::InstallWorker, std::move(*path), *size, std::move(*digest)};
    return InstallWorkerMarker{std::move(*worker),   std::move(*bundle),    std::move(*revision),
                               std::move(*identity), std::move(executable), DistributionArchitecture::X64};
}

/// @brief Parse失敗を安定したInstall State Errorへ変換する
template <typename T>
[[nodiscard]] cue::Result<T> parse_failure(const cue::AssertContext &a_assertContext,
                                           std::string_view a_summary) noexcept
{
    return cue::Result<T>::failure(cue::distribution::make_distribution_error(
        a_assertContext, cue::distribution::DistributionError::NonCanonicalInstallState, a_summary));
}

/// @brief Canonical先頭Memberに明示された対応外Schema Versionを検出する
[[nodiscard]] bool has_unsupported_schema(std::string_view a_bytes) noexcept
{
    constexpr std::string_view prefix = "{\"schemaVersion\":";
    if (!a_bytes.starts_with(prefix))
    {
        return false;
    }
    std::uint64_t version = 0U;
    const char *begin = a_bytes.data() + prefix.size();
    const char *end = a_bytes.data() + a_bytes.size();
    const auto parsed = std::from_chars(begin, end, version);
    return parsed.ec == std::errc{} && parsed.ptr != end && *parsed.ptr == ',' && version != 1U;
}

/// @brief 対応外Install State Schemaを旧Readerで上書きさせないErrorを返す
template <typename T>
[[nodiscard]] cue::Result<T> unsupported_schema(const cue::AssertContext &a_assertContext) noexcept
{
    return cue::Result<T>::failure(cue::distribution::make_distribution_error(
        a_assertContext, cue::distribution::DistributionError::UnsupportedInstallSchema,
        "Install State schema version is not supported"));
}
} // namespace

namespace cue::distribution
{
std::string_view install_operation_kind_name(InstallOperationKind a_kind) noexcept
{
    switch (a_kind)
    {
    case InstallOperationKind::Install:
        return "install";
    case InstallOperationKind::Update:
        return "update";
    case InstallOperationKind::Rollback:
        return "rollback";
    case InstallOperationKind::Uninstall:
        return "uninstall";
    case InstallOperationKind::RegistryRecovery:
        return "registryRecovery";
    }
    return {};
}

std::string_view install_operation_stage_name(InstallOperationStage a_stage) noexcept
{
    switch (a_stage)
    {
    case InstallOperationStage::Prepared:
        return "prepared";
    case InstallOperationStage::PayloadStaged:
        return "payloadStaged";
    case InstallOperationStage::VersionPublished:
        return "versionPublished";
    case InstallOperationStage::ProbeSucceeded:
        return "probeSucceeded";
    case InstallOperationStage::WorkerPublished:
        return "workerPublished";
    case InstallOperationStage::RegistryPublished:
        return "registryPublished";
    case InstallOperationStage::SelectionPublished:
        return "selectionPublished";
    case InstallOperationStage::RemovalBlocked:
        return "removalBlocked";
    case InstallOperationStage::VersionQuarantined:
        return "versionQuarantined";
    case InstallOperationStage::RegistryEntryRemoved:
        return "registryEntryRemoved";
    case InstallOperationStage::CandidatesValidated:
        return "candidatesValidated";
    }
    return {};
}

std::string_view installed_version_state_name(InstalledVersionState a_state) noexcept
{
    switch (a_state)
    {
    case InstalledVersionState::Selectable:
        return "selectable";
    case InstalledVersionState::PendingRemoval:
        return "pendingRemoval";
    }
    return {};
}

bool is_valid_install_stage_transition(InstallOperationKind a_kind, InstallOperationStage a_current,
                                       InstallOperationStage a_next) noexcept
{
    using enum InstallOperationKind;
    using enum InstallOperationStage;
    if (!is_stage_for_kind(a_kind, a_current) || !is_stage_for_kind(a_kind, a_next))
    {
        return false;
    }
    switch (a_kind)
    {
    case Install:
    case Update:
        return (a_current == Prepared && a_next == PayloadStaged) ||
               (a_current == PayloadStaged && a_next == VersionPublished) ||
               (a_current == VersionPublished && a_next == ProbeSucceeded) ||
               (a_current == ProbeSucceeded && a_next == WorkerPublished) ||
               (a_current == WorkerPublished && a_next == RegistryPublished);
    case Rollback:
        return a_current == Prepared && a_next == SelectionPublished;
    case Uninstall:
        return (a_current == Prepared && a_next == RemovalBlocked) ||
               (a_current == RemovalBlocked && a_next == VersionQuarantined) ||
               (a_current == VersionQuarantined && a_next == RegistryEntryRemoved);
    case RegistryRecovery:
        return (a_current == Prepared && a_next == CandidatesValidated) ||
               (a_current == CandidatesValidated && a_next == RegistryPublished);
    }
    return false;
}

Result<std::string> write_installed_versions_registry(const InstalledVersionsRegistry &a_registry,
                                                      const AssertContext &a_assertContext) noexcept
{
    if (!is_valid_registry(a_registry))
    {
        return Result<std::string>::failure(make_distribution_error(
            a_assertContext, DistributionError::InvalidInstallState, "Installed Versions Registry is invalid"));
    }
    try
    {
        return Result<std::string>::success(serialize_registry(a_registry));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<InstalledVersionsRegistry> read_installed_versions_registry(std::string_view a_bytes,
                                                                   const AssertContext &a_assertContext) noexcept
{
    if (has_unsupported_schema(a_bytes))
    {
        return unsupported_schema<InstalledVersionsRegistry>(a_assertContext);
    }
    try
    {
        auto registry = parse_registry(a_bytes);
        if (!registry || !is_valid_registry(*registry) || serialize_registry(*registry) != a_bytes)
        {
            return parse_failure<InstalledVersionsRegistry>(a_assertContext,
                                                            "Installed Versions Registry is not canonical v1");
        }
        return Result<InstalledVersionsRegistry>::success(std::move(*registry));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<std::string> write_install_operation_journal(const InstallOperationJournal &a_journal,
                                                    const AssertContext &a_assertContext) noexcept
{
    if (!is_valid_journal(a_journal))
    {
        return Result<std::string>::failure(make_distribution_error(
            a_assertContext, DistributionError::InvalidInstallState, "Install Operation Journal is invalid"));
    }
    try
    {
        return Result<std::string>::success(serialize_journal(a_journal));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<InstallOperationJournal> read_install_operation_journal(std::string_view a_bytes,
                                                               const AssertContext &a_assertContext) noexcept
{
    if (has_unsupported_schema(a_bytes))
    {
        return unsupported_schema<InstallOperationJournal>(a_assertContext);
    }
    try
    {
        auto journal = parse_journal(a_bytes);
        if (!journal || !is_valid_journal(*journal) || serialize_journal(*journal) != a_bytes)
        {
            return parse_failure<InstallOperationJournal>(a_assertContext,
                                                          "Install Operation Journal is not canonical v1");
        }
        return Result<InstallOperationJournal>::success(std::move(*journal));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<std::string> write_payload_complete_marker(const PayloadCompleteMarker &a_marker,
                                                  const AssertContext &a_assertContext) noexcept
{
    if (!is_valid_payload_marker(a_marker))
    {
        return Result<std::string>::failure(make_distribution_error(
            a_assertContext, DistributionError::InvalidInstallState, "Payload complete marker is invalid"));
    }
    try
    {
        return Result<std::string>::success(serialize_payload_marker(a_marker));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<PayloadCompleteMarker> read_payload_complete_marker(std::string_view a_bytes,
                                                           const AssertContext &a_assertContext) noexcept
{
    if (has_unsupported_schema(a_bytes))
    {
        return unsupported_schema<PayloadCompleteMarker>(a_assertContext);
    }
    try
    {
        auto marker = parse_payload_marker(a_bytes);
        if (!marker || !is_valid_payload_marker(*marker) || serialize_payload_marker(*marker) != a_bytes)
        {
            return parse_failure<PayloadCompleteMarker>(a_assertContext, "Payload complete marker is not canonical v1");
        }
        return Result<PayloadCompleteMarker>::success(std::move(*marker));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<std::string> write_install_probe_marker(const InstallProbeMarker &a_marker,
                                               const AssertContext &a_assertContext) noexcept
{
    if (!is_valid_probe_marker(a_marker))
    {
        return Result<std::string>::failure(make_distribution_error(
            a_assertContext, DistributionError::InvalidInstallState, "Install Probe marker is invalid"));
    }
    try
    {
        return Result<std::string>::success(serialize_probe_marker(a_marker));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<InstallProbeMarker> read_install_probe_marker(std::string_view a_bytes,
                                                     const AssertContext &a_assertContext) noexcept
{
    if (has_unsupported_schema(a_bytes))
    {
        return unsupported_schema<InstallProbeMarker>(a_assertContext);
    }
    try
    {
        auto marker = parse_probe_marker(a_bytes);
        if (!marker || !is_valid_probe_marker(*marker) || serialize_probe_marker(*marker) != a_bytes)
        {
            return parse_failure<InstallProbeMarker>(a_assertContext, "Install Probe marker is not canonical v1");
        }
        return Result<InstallProbeMarker>::success(std::move(*marker));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<std::string> write_install_worker_marker(const InstallWorkerMarker &a_marker,
                                                const AssertContext &a_assertContext) noexcept
{
    if (!is_valid_worker_marker(a_marker))
    {
        return Result<std::string>::failure(make_distribution_error(
            a_assertContext, DistributionError::InvalidInstallState, "Install Worker marker is invalid"));
    }
    try
    {
        return Result<std::string>::success(serialize_worker_marker(a_marker));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<InstallWorkerMarker> read_install_worker_marker(std::string_view a_bytes,
                                                       const AssertContext &a_assertContext) noexcept
{
    if (has_unsupported_schema(a_bytes))
    {
        return unsupported_schema<InstallWorkerMarker>(a_assertContext);
    }
    try
    {
        auto marker = parse_worker_marker(a_bytes);
        if (!marker || !is_valid_worker_marker(*marker) || serialize_worker_marker(*marker) != a_bytes)
        {
            return parse_failure<InstallWorkerMarker>(a_assertContext, "Install Worker marker is not canonical v1");
        }
        return Result<InstallWorkerMarker>::success(std::move(*marker));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<std::string> make_publisher_build_identity_digest(const PublisherBuildIdentity &a_identity,
                                                         const AssertContext &a_assertContext) noexcept
{
    if (!is_valid_publisher_build_identity(a_identity))
    {
        return Result<std::string>::failure(make_distribution_error(a_assertContext, DistributionError::InvalidIdentity,
                                                                    "Publisher Build Identity is invalid"));
    }
    try
    {
        std::string bytes;
        append_identity_field(bytes, a_identity.builtFromRevision);
        append_identity_field(bytes, a_identity.targetTriplet);
        append_identity_field(bytes, "x64");
        append_identity_field(bytes, "x64");
        append_identity_field(bytes, a_identity.compilerVendor);
        append_identity_field(bytes, a_identity.compilerVersion);
        append_identity_field(bytes, a_identity.toolsetVersion);
        append_identity_field(bytes, a_identity.crtLinkage);
        append_identity_field(bytes, a_identity.crtVersion);
        append_identity_field(bytes, a_identity.windowsSdkTargetVersion);
        append_identity_field(bytes, a_identity.configuration);
        return hash_string(bytes, a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}

Result<std::string> make_install_worker_id(const DistributionManifest &a_manifest,
                                           const AssertContext &a_assertContext) noexcept
{
    Result<std::string> canonical = write_distribution_manifest(a_manifest, a_assertContext);
    if (!canonical)
    {
        return Result<std::string>::failure(std::move(*canonical.try_error()));
    }
    /// @brief Manifest内の単一Install Worker Inventoryを検索する
    const auto iterator = std::ranges::find_if(a_manifest.files, [](const DistributionFileEntry &a_file) noexcept
                                               { return a_file.role == DistributionFileRole::InstallWorker; });
    if (iterator == a_manifest.files.end())
    {
        return Result<std::string>::failure(make_distribution_error(
            a_assertContext, DistributionError::MissingRequiredPayload, "Install Worker inventory is missing"));
    }
    Result<std::string> identity =
        make_publisher_build_identity_digest(a_manifest.publisherBuildIdentity, a_assertContext);
    if (!identity)
    {
        return identity;
    }
    try
    {
        std::string bytes;
        append_identity_field(bytes, a_manifest.bundleId);
        append_identity_field(bytes, a_manifest.engineSourceRevision);
        append_identity_field(bytes, *identity.try_value());
        append_identity_field(bytes, distribution_file_role_name(iterator->role));
        append_identity_field(bytes, iterator->relativePath);
        append_identity_number(bytes, iterator->byteSize);
        append_identity_field(bytes, iterator->sha256);
        append_identity_field(bytes, "x64");
        return hash_string(bytes, a_assertContext);
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext);
    }
    catch (...)
    {
        terminate_exception(a_assertContext);
    }
}
} // namespace cue::distribution
