#pragma once

#include <Cue/Distribution/Manifest.h>
#include <Cue/Foundation/Result.h>
#include <Cue/Platform/Process.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::distribution
{
/// @brief Developer Source SDK公開処理が最後に完了したStage
enum class SourceSdkPublishStage : std::uint8_t
{
    ValidateInput,
    CaptureRepository,
    MaterializeSource,
    RestoreDependencies,
    BuildTools,
    WriteManifest,
    ValidateStaging,
    Publish,
    ValidatePublished,
    Completed
};

/// @brief Windows x64 Developer Source SDKを固定Commitから生成する入力
struct WindowsSourceSdkPublishRequest final
{
    std::string repositoryRoot;
    std::string destinationParent;
    std::string operationsRoot;
    std::string dependenciesParent;
    std::string engineVersion;
    std::string bundleId;
    std::string gitExecutable;
    std::string cmakeExecutable;
    std::string powershellExecutable;
    std::string cmakeGenerator;
    PublisherBuildIdentity publisherBuildIdentity;
    MinimumToolchain minimumToolchain;
    std::vector<ChildProcessEnvironmentEntry> environmentAllowlist;
};

/// @brief 公開済みBundleのIdentityと再利用状態
struct SourceSdkPublishReport final
{
    SourceSdkPublishStage stage = SourceSdkPublishStage::ValidateInput;
    std::string bundleRoot;
    DistributionManifest manifest;
    bool reusedExisting = false;
};

/// @brief clean Repositoryの固定CommitからWindows x64 Developer Source SDKをAtomic Publishする
///
/// RequestとAssertContextは呼出中だけ借用する。Source管理PayloadはLive Worktreeから読まず、Git Commit Blobを
/// Materializeする。Dependency Rootと隔離Build RootはInstalled Version外へ置き、成功Bundleだけを同一Volume
/// Renameで公開する。同じIdentityの完成Bundleは全Inventory再検証後だけ冪等再利用する。
[[nodiscard]] Result<SourceSdkPublishReport> publish_windows_source_sdk(
    const WindowsSourceSdkPublishRequest &a_request, const AssertContext &a_assertContext) noexcept;
} // namespace cue::distribution
