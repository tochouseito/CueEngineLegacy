# M18 Editor／Engine Developer Distribution Completion Gate

## Gate Result

2026-09-23に、M18の先行Issue #365、#375から#380がGitHub上でClosedであり、
本Gate Issue #381だけがMilestone #19の未完了Issueであることを確認した。

ADR-0030が定義する`DeveloperSourceSdk`について、Version付きBundle Publish、Per-user Install、
Side-by-side Update、Rollback、自己Uninstall、Registry／Journal Recovery、Installed Engineからの
Project Hub／Editor／RuntimeHost／Shipping Build接続を、Windows x64のローカル開発者向け境界として検証した。

M18は未署名Bundleを同一開発者が管理するLocal Fixed Driveから明示許可した場合だけ扱う。
実運用Code Signing、Timestamp、Online Revocation、署名済みManifest、外部Trust Anchor、Network配布は
未実施であるため、`PublicDistributionReady = false`を維持する。Size／SHA-256は真正性ではなく、
偶発破損とOperation整合性だけを検出する。

| Acceptance Gate | Result | Evidence |
| --- | --- | --- |
| 先行IssueとADR-0030の一致 | Pass | #365、#375から#380はClosed。Source SDK、Immutable Version、Worker、Registry／Journal、Installed Mode、Prerequisiteの契約を実装 |
| Bundle PublishとCanonical Identity | Pass | `Cue.Distribution.Manifest`、`Cue.Distribution.Publisher`、`Cue.Distribution.WindowsPublisher`でAllowlist、Revision、Inventory、Size／SHA-256、Target／PE、Atomic Publishを検証 |
| Install／Probe／Worker Publish | Pass | `Cue.Distribution.WindowsInstaller`で排他Control Lease、専用Probe、Probe Marker、Version外Worker、Registry Publish、同一Bundle再実行を検証 |
| Update／Rollback／自己Uninstall | Pass | `Cue.Distribution.WindowsInstallerVersionOperations`でSide-by-side Update、Rollback、Execution Lease、Version外Workerによる自己Uninstallを検証 |
| Crash／Registry／Journal Recovery | Pass | `Cue.Distribution.WindowsInstallerRecovery`で各Stage、Blocked Operation、破損Registry、欠落Worker、Cleanup／Journal削除の再開とFail-closed経路を検証 |
| Worker Canonical ID／Path／Marker | Pass | Manifest／Install State／Installer Testで64文字lowercase SHA-256 ID、`Operations/Workers`直下Path、Worker本体と固定完了MarkerのIdentity／Digestを検証 |
| Dependency Root分離とRestore | Pass | `Cue.Distribution.RestoreVcpkgContract`、Build／Installer TestでDefinition ID、Build Identity、Root ID、Version外Root、Process間Lease、Immutable再利用を検証 |
| Prerequisite／License／禁止File | Pass | `Cue.Distribution.BootstrapProbe`、`BootstrapStaticContract`、`WindowsPrerequisites`、Manifest／Publisher TestでWindows x64、VC++ Runtime、CMake、MSVC、Windows SDK、Git for Windows、Notice／License、禁止Inventoryを検証 |
| Project HubからInstalled Editor起動 | Pass | `Cue.ProjectHub.Windows.EditorProcess`とProject Hub Service／ImGui TestでInstall Root明示引渡し、互換Version選択、Project／User Data非変更を検証 |
| 3構成Editor Playと`.git`なしShipping | Pass | `Cue.Editor.Workflow.ProcessRoundTrip`でDebug／Development／ReleaseのConfiguration一致Host、Installed SDK由来Provenance、Shipping Build／Packageを検証 |
| RuntimeHost／Package回帰 | Pass | `Cue.RuntimeHost.Package.Process`を含むRuntimeHost 18 TestとPackage 5 Testを3構成で検証 |
| 3構成Build・全CTest・Diff Check | Pass | Debug／Development／Releaseの全Target Build成功。310 Test枠で失敗0。`git diff --check`成功 |
| Public Trustの未実施記録 | Pass | 実運用署名と外部Trust Anchorを未実施として本書に記録し、`PublicDistributionReady = false`を維持 |

## Verified Source Tree

M18実装の統合先は`Rebuild`のCommit `8f12f41244e4becf8a4ca834334acb0bc15adfb5`、
Git tree `c2153b7858dd1fe05f6b43b6955936674e852800`である。

M18固有の統合PRは#382、#395から#400である。最後の実装PR #400は最終Head
`e6e9c32d35b256c64958a0c9861aa5eb2cb280f2`を`8f12f41244e4becf8a4ca834334acb0bc15adfb5`へ
Squash Mergeした。Windows CI run `35812126628`はDebug／Development／Releaseの全Jobが成功し、
9件のReview Threadは全て解決済みである。

#381は本Completion Gate文書だけを追加し、M18製品コード、公開API、永続Schema、配布形式を変更しない。

## Architecture and Compatibility Map

| M18 Scope | Verification |
| --- | --- |
| Distribution Manifest v1 | Canonical JSON、Engine／Bundle Identity、Source Revision／State／Inventory、Dependency Definition、Publisher Identity、完全File Inventory |
| Source SDK Publisher | 固定Commit Blob、生成Release Tool、Allowlist、Staging再検証、同一Volume Atomic Rename、dirty／HEAD変化拒否 |
| Dependency Workspace | Version外Tool／Install Root、Build Identity別Root、Process間Lease、完了Marker、失敗Staging非公開 |
| Installed Version State | Immutable Version Directory、Registry Schema v1、Generation／Revision、Expected State照合、Control／Execution Lease |
| Operation Recovery | Kind別Canonical Journal、単調Stage、耐久副作用、Blocked Operation、Source Evidence、候補Identity／Digest、Fail-closed |
| Version外Install Worker | Canonical Worker ID、固定Path、Static Runtime、Worker／Marker Digest、Atomic Publish、自己Uninstall |
| Project Hub／Editor | Installed Version選択、Install Root明示引渡し、Repository Path非使用、Version外Build／Dependency Workspace |
| Build／Play／Package | Configuration一致RuntimeHost、`.git`なしProvenance、Shipping Build／Package、Project／User Data保持 |
| Prerequisite／Trust | Bootstrap、VC++ Runtime、CMake、MSVC、Windows SDK、Git for Windows、LocalDeveloperOnly受付条件、Public Trust非対応 |

## Local Validation Results

- CMake Configure: 成功、310 Test登録
- Debug: 全Target Build成功、310/310 Test成功、955.66秒
- Development: 全Target Build成功、310/310 Test成功、313.27秒
- Release: 全Target Build成功、305 Test成功、既定5 Test Skip、失敗0、390.23秒
- M18 Distribution Test: Debug／Development／Releaseの12 Testを全て実行し、失敗0
- Project Hub: 3構成で6 Test成功
- Editor: 3構成で9 Test成功。`Cue.Editor.Workflow.ProcessRoundTrip`はDebug 23.14秒、Development 17.65秒、Release 96.03秒
- RuntimeHost: 3構成で18 Test成功。`Cue.RuntimeHost.Package.Process`はDebug 34.20秒、Development 6.87秒、Release 5.08秒
- `git diff --check`: 成功
- 検証Host: Windows x64、Visual Studio 18 2026、MSVC 19.51.36257.0、Windows SDK 10.0.26100.0、Git for Windows 2.47.1.windows.2
- Dependency Restore: pinned vcpkg commit `386d7c478221b7ee0c97bfe6ea61dcf65121d564`、imgui 1.92.9を固定ManifestとLocal Cacheから復元

ReleaseでSkipされた既存Testは次の5件である。Debug Layer／InfoQueue／DREDまたはRelease固有の
Device Removal診断条件によるもので、M18のDistribution、Project Hub、Editor Workflow、RuntimeHost、Package Testは
全て実行された。

- `Cue.RHI.D3D12.FrameCommand.InfoQueue300`
- `Cue.RHI.D3D12.RtvHeap.InfoQueue`
- `Cue.RHI.D3D12.SwapChain.InfoQueue`
- `Cue.RHI.D3D12.SwapChain.SceneDirectPresentDeviceRemoved`
- `Cue.RHI.D3D12.SwapChain.DeviceRemovalDredFailure`

## GitHub Gate

- 先行Issue: #365、#375から#380はClosed
- 最終実装PR: #400、Merged
- PR #400最終Head: `e6e9c32d35b256c64958a0c9861aa5eb2cb280f2`
- Windows CI: run `35812126628`、Debug／Development／Release成功
- Review Thread: 9件全て解決、未解決0件
- M18実装Merge: `8f12f41244e4becf8a4ca834334acb0bc15adfb5`
- Gate Issue: #381だけがOpen
- Branch: `codex/issue-381-m18-completion-gate`、開始時の追跡先`origin/Rebuild`
- Working Tree: #381開始時点でClean

本Gate PRのReview、CI、未解決Thread、Head／Base／Tree一致はMerge直前に別途Live確認する。

## Scope Audit

M18固有PR #382、#395から#400の変更は、Developer DistributionのResearch／ADR、Distribution Manifest、
Source SDK Publisher、Installer／Bootstrap／Install Worker、Version Registry／Journal、Dependency Restore、
Project Hub／Editor／Build／Package接続、Windows Toolchain／Prerequisite、Test、CMake、利用文書に限定される。

- ECS Storage／Query、並列化、永続契約を変更していない。
- Renderer、Sound、Effect、Physicsの機能を追加または変更していない。
- Asset Import／Cook、Prefab、Scripting／Hot Reload、Runtime Plugin、Modを追加していない。
- 新規第三者LibraryまたはVersion更新を行っていない。
- 旧CueEngineまたは外部ProjectからSource Codeをコピー、移植、部分抽出していない。
- MSI／MSIX、第三者Installer Framework、VC++ Redistributable Binaryを導入または同梱していない。

## Validation Commands

- `cmake --preset windows-vs2026`
- `cmake --build --preset windows-vs2026-debug --parallel 4`
- `ctest --preset windows-vs2026-debug --output-on-failure`
- `cmake --build --preset windows-vs2026-development --parallel 4`
- `ctest --preset windows-vs2026-development --output-on-failure`
- `cmake --build --preset windows-vs2026-release --parallel 4`
- `ctest --preset windows-vs2026-release --output-on-failure`
- `git status --short --branch`
- `git rev-parse HEAD`
- `git rev-parse "HEAD^{tree}"`
- `git diff --check`

`AGENTS.md`が原則指定する`scripts/codex_build.ps1`はRepository内に存在しないため、正式なCMake Presetで
Configure、3構成Build、CTestを実行した。NuGet Restoreは実行していない。

## Not Run

- 実運用CertificateによるAuthenticode署名、Timestamp、Online Revocation、Certificate Chain検証
- 署名済みManifest、許可Publisherを強制する外部Trust Anchor、公開Channel／Network Download
- MSI／MSIX、Machine-wide Install、Store配布、Background Updater、Delta Patch
- 別Machine、別Windows Build、別User Account、Remote／UNC配布元での実導入
- 実際のVC++ Runtime、CMake、MSVC、Windows SDK、Git欠落Machine。自動Testでは不足／不一致経路を検証済み
- Project Hub／Editor UIを人手で操作する画面確認。Service／ImGui／Process E2Eは自動Test済み
- 数時間以上のSoak、電源断、AddressSanitizer、ThreadSanitizer、UndefinedBehaviorSanitizer

## Existing Problems and Remaining Risks

- `LocalDeveloperOnly`の未署名BundleはPublisher真正性を保証しない。第三者から受領したBundleやNetwork配布へ使用できない。
- `PublicDistributionReady = false`であり、実運用署名と外部Trust Anchorを導入するまで公開配布用とは扱わない。
- `DeveloperSourceSdk`は安定Binary SDKではない。導入先に対応ToolchainとVersion外Dependency Restore／Build Workspaceが必要である。
- Worker／Registry／Journalの回復契約はWindows x64の現在のFilesystem／Process／Lease実装を前提とする。
- Toolchain、vcpkg Pin、第三者Version、Windows SDK更新時はDependency Build Identity、License Inventory、Bootstrap／Workerを再検証する必要がある。
- ReleaseでSkipされた5件はM18変更とは無関係だが、Release構成のDebug Layer／InfoQueue／DRED診断経路は未実行である。
- Debug初回全CTestはDistributionとProject生成の隔離Buildを含み955.66秒を要した。この値は一台の検証Host上のGate所要時間であり、性能改善値ではない。

## Scope-out Candidates

- 実運用Code Signing、署名済みManifest、外部Trust Anchor、公開Download Channel
- 安定Binary SDK／Plugin SDKとCompiler／CRT／STL ABI互換方針
- Machine-wide Installer、File Association、Store配布、Background Update
- 別Machine／別User／Windows別Buildを含むDistribution Compatibility Matrix

## Next Action

Gate PRのReview／Windows CI／未解決ThreadをLive確認して#381をMerge／Closeする。
その後、ユーザーの直前確認を得てからMilestone M18をCloseする。
