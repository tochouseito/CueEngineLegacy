#pragma once

#include <Cue/Build/Windows/WindowsArtifactPublisher.h>
#include <Cue/Build/Windows/WindowsProductSecurity.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace cue::detail
{
/// @brief Publisher内でSecurity Snapshot Leaseを検証するTest観測点
enum class WindowsProductSecuritySnapshotStage : std::uint8_t
{
    CandidateBeforeProbe,
    CandidateBeforeRelease,
    VersionAfterValidation,
    VersionBeforeCurrentPublication,
    Count
};

/// @brief Production経路のSecurity Snapshot Lease寿命をTestから観測する
class WindowsProductSecuritySnapshotObserver
{
  public:
    /// @brief Observerを多態的に破棄する
    virtual ~WindowsProductSecuritySnapshotObserver() = default;

    /// @brief 指定StageでSnapshotが保持されているProduct Pathを通知する
    virtual void on_snapshot_held(WindowsProductSecuritySnapshotStage a_stage,
                                  const std::filesystem::path &a_productPath) noexcept = 0;
};

/// @brief 検証済みShipping ProductのRead-only Snapshotと排他的変更Leaseを保持する
class WindowsProductSecuritySnapshot final
{
  public:
    /// @brief Snapshotの複製を禁止する
    WindowsProductSecuritySnapshot(const WindowsProductSecuritySnapshot &) = delete;
    /// @brief Snapshotの複製代入を禁止する
    WindowsProductSecuritySnapshot &operator=(const WindowsProductSecuritySnapshot &) = delete;
    /// @brief Snapshot所有権を移動する
    WindowsProductSecuritySnapshot(WindowsProductSecuritySnapshot &&a_other) noexcept;
    /// @brief 既存Snapshotを解放して所有権を移動代入する
    WindowsProductSecuritySnapshot &operator=(WindowsProductSecuritySnapshot &&a_other) noexcept;
    /// @brief MappingとFile Handleを解放する
    ~WindowsProductSecuritySnapshot() noexcept;

    /// @brief 同じFile Handleから確定したSecurity Evidenceを返す
    [[nodiscard]] const WindowsProductSecurityValidation &validation() const noexcept;
    /// @brief 検証済みEvidenceの所有権をSnapshotから取り出す
    [[nodiscard]] WindowsProductSecurityValidation take_validation() noexcept;

  private:
    struct State;

    /// @brief 検証済みStateの所有権を取得する
    explicit WindowsProductSecuritySnapshot(std::unique_ptr<State> a_state) noexcept;

    std::unique_ptr<State> m_state;

    friend Result<WindowsProductSecuritySnapshot> validate_windows_shipping_product_security_snapshot(
        std::string a_absoluteProductPath, const BuildProfile &a_profile,
        const AssertContext &a_assertContext) noexcept;
};

/// @brief Shipping Productを検証し変更を拒否するRead HandleとEvidenceを同じ寿命で返す
[[nodiscard]] Result<WindowsProductSecuritySnapshot> validate_windows_shipping_product_security_snapshot(
    std::string a_absoluteProductPath, const BuildProfile &a_profile, const AssertContext &a_assertContext) noexcept;

/// @brief WinVerifyTrust Statusと直後のLastErrorを署名状態へ副作用なしで分類する
[[nodiscard]] WindowsProductSignatureStatus classify_windows_product_trust_status(std::int32_t a_status,
                                                                                  std::uint32_t a_lastError) noexcept;

/// @brief Security Snapshot Observerを借用するTest用Windows Artifact Publisherを構築する
[[nodiscard]] Result<std::unique_ptr<BuildArtifactPublisher>> create_windows_build_artifact_publisher_for_test(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor,
    WindowsProductSecuritySnapshotObserver &a_observer, const AssertContext &a_assertContext) noexcept;

/// @brief Installed SDK ProvenanceとSecurity Snapshot Observerを借用するTest用Publisherを構築する
[[nodiscard]] Result<std::unique_ptr<BuildArtifactPublisher>> create_windows_build_artifact_publisher_for_test(
    std::string a_projectRoot, const ProjectDescriptor &a_descriptor,
    WindowsInstalledEngineSourceProvenance a_engineSourceProvenance,
    WindowsProductSecuritySnapshotObserver &a_observer, const AssertContext &a_assertContext) noexcept;
} // namespace cue::detail
