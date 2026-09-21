# M16 Standalone Packaging and First Usable Engine Completion Gate

## Gate Result

M12からM15のCompletion Gate #165、#207、#217、#227と、M16の先行Issue #228から#236が
GitHub上でClosedであり、本Gate Issue #237がMilestone最後の1件であることを2026-09-11に確認した。

自動検証Gateと新規Workspaceからの実Window End-to-End WorkflowはPassした。手動結果は
`FirstUsableWorkflow.md`と本書へ記録した。

| Acceptance Gate | Result | Evidence |
|---|---|---|
| M12からM16の必須Issue | Pass | M12からM15のCompletion GateとM16先行IssueをClosed確認。本PRが最後の#237をCloseする |
| Debug／Development／Release Build | Pass | 全Targetを3構成Build成功。最終Review修正対象も3構成Build成功 |
| 全CTest／Headless／Process Test | Pass | Debug／Developmentは262/262、Releaseは258成功と既定4 Skip、失敗0 |
| 新規Workspaceの手動End-to-End | Pass | 2026-09-11にユーザーが正本手順を実行し「問題なし」と確認 |
| PackageのSource Assets／Workspace Cache非参照 | Pass | `Cue.Editor.Workflow.ProcessRoundTrip`のSource Root非表示、別Current Directory、Relocation Testが3構成成功 |
| Package Manifest Parse／全File Hash時間 | Pass | Release実PackageでParse 10,000回、Hash検証20回を測定し、本文へ条件と結果を記録 |
| ECS改良、Asset Import／Cook、Renderer、Sound、Effect、Physicsの非追加 | Pass | M16開始点からの変更一覧に対象Moduleの機能追加なし |
| 未実行検証と残るRisk | Pass | 本文末尾へ明記 |

Releaseの全CTestでは、Debug Layer／InfoQueue／DREDを必要とする既存4 Testが構成条件によりSkippedとなった。
M16対象のProject、Build、Package、RuntimeHost、Editor Workflow Testは全構成で実行され、成功した。

## Verified Source Tree

最終コードはPR #296のReview／CI対象Commit `d37628b2d8d2f7efa38010482c3ba519eb82e0e0`、
Git tree `f9341665d707f98ad71a20d0f4e753ac16d1f024`である。#237 Branchも同一Treeであることを確認した。

同Treeに対して全CTestを再実行し、Debug／Developmentは262/262、Releaseは258成功、既定4 Skip、
失敗0だった。最後のReview修正で追加したGame Module Diagnostic所有Copyは、RuntimeHost Test Targetの
3構成Buildと`Cue.RuntimeHost.Package.Process`の3構成各1/1でも個別確認した。

#237は検証結果と証跡文書、再実行可能なManifest Benchmark経路、Tamper Test用Packageの分離だけを追加する。
最終PR HeadはWindows CIでDebug／Development／Releaseを再検証する。

## Coverage Map

| M16 Scope | Verification |
|---|---|
| Blank Project／Default Scene | `Cue.Project.Generator`、`Cue.Project.Generator.Windows`、`Cue.ProjectHub.Service` |
| Startup Scene Descriptor／Migration | `Cue.Project.Descriptor`、`Cue.Project.Compatibility`、`Cue.Project.M09Process` |
| Runtime Data Publish | `Cue.Package.RuntimeData`、`Cue.Scene.Instantiation`、`Cue.Scene.Serialization` |
| Package Manifest／Inventory | `Cue.Package.Manifest`、`Cue.RuntimeHost.Dependencies` |
| Staging／Atomic Publish／Rollback | `Cue.Package.Publisher`、`Cue.Package.Workflow` |
| Standalone RuntimeHost | `Cue.RuntimeHost.Package.Process`、`Cue.RuntimeHost.Smoke`、RuntimeHost Failure Test |
| Editor Build／Package／Run | `Cue.Editor.ImGui.Build`、`Cue.Editor.Workflow.ProcessRoundTrip` |
| Relocation／Source非参照 | `Cue.Editor.Workflow.ProcessRoundTrip`、`RelocationReproducibility.md` |

## Manual UI Status

2026-09-11にユーザーが`Docs/Testing/M16-first-usable-engine-workflow.md`の実Window手順を実行し、
「問題なし」と確認した。

- Blank 3D Project作成とDefault Scene Open
- Hierarchy／Inspector編集、親子化、Undo／Redo、Save、再Open
- FilesのFolder／File作成、Rename、Copy、Move、Trash、Restore
- Play／Stopの反復
- Debug／Development／ReleaseのBuild & Package
- Standalone Run／Stop
- Build失敗後のSource復元とRetry
- Build／Package／Runtime実行中の終了確認とChild Process終了

Screenshotまたは動画は保存していない。結果はユーザーによる実行確認を根拠とする。

## Scope Audit

M16開始点`3dbd4dc3`から検証対象Commitまでの変更は、Project、Build、Package、RuntimeHost、
Editor Workflow、Scene Runtime Data接続、Test、CMake、ADR、利用手順に限定されている。

- ECS Storage／Queryの公開契約と並列化方式は変更していない。
- Renderer、Sound、Effect、PhysicsのSourceを追加または変更していない。
- 一般Asset Import／Cook、Prefab、Scripting／Hot Reloadは実装していない。
- 新規第三者LibraryまたはVersion更新は行っていない。

## Validation Commands

- Dependency Restore（当時実行済み。現行の再実行は`README.md`の手順で`ToolRoot`、`InstallRoot`、
  `GitExecutable`を明示する）
- `cmake --preset windows-vs2026`
- `cmake --build --preset windows-vs2026-debug --parallel`
- `ctest --preset windows-vs2026-debug --output-on-failure`
- `cmake --build --preset windows-vs2026-development --parallel`
- `ctest --preset windows-vs2026-development --output-on-failure`
- `cmake --build --preset windows-vs2026-release --parallel`
- `ctest --preset windows-vs2026-release --output-on-failure`
- `CuePackageManifestTests.exe --benchmark <absolute-package-root>`
- `git diff --name-only 3dbd4dc3..d37628b2`
- `git diff --check`

## Automated Results

- Dependency Restore: Dear ImGui `1.92.6`を固定済みvcpkg Manifestから復元成功
- CMake Configure: 成功、262 Test登録
- Debug: 全Target Build成功、262/262 Test成功
- Development: 全Target Build成功、262/262 Test成功
- Release: 全Target Build成功、258 Test成功、既定4 Test Skip、失敗0
- Review Follow-up: `Cue.RuntimeHost.Package.Process`は3構成各1/1成功
- Release Skip:
  - `Cue.RHI.D3D12.FrameCommand.InfoQueue300`
  - `Cue.RHI.D3D12.RtvHeap.InfoQueue`
  - `Cue.RHI.D3D12.SwapChain.InfoQueue`
  - `Cue.RHI.D3D12.SwapChain.DeviceRemovalDredFailure`

## Package Manifest Timing Baseline

ADR-0023のMitigationに従い、Release構成の`Cue.RuntimeHost.Package.Process`が生成・検証したPackageを使って
Manifest Parseと全Manifest EntryのSize／SHA-256検証時間を測定した。測定は初回検証によるWarm-up後に行い、
File読込み済みMemoryではなく、RuntimeHostと同じ`verify_package_manifest_files`によるFile Open／Read／Hash／照合を含む。

- 測定日: 2026-09-11
- OS: Windows 11 Home 10.0.26200（Build 26200）
- CPU: AMD Ryzen 7 3700X、8 Core／16 Logical Processor
- Build: Release、MSBuild 18.9.1
- Manifest: 1,265 bytes、6 File Entry
- Hash対象: 合計829,871 bytes
- Manifest Parse 10,000回: 平均17.268 us、p50 15.700 us、p95 22.900 us
- 全File Hash検証20回: 平均7.066 ms、p50 6.993 ms、p95 8.135 ms

これは一台のMachine上のWarm filesystem cache Baselineであり、性能目標の達成や最適化効果を主張する値ではない。
後続の最適化判断では同じ入力、Build、Machine条件を固定して比較する。

## Not Run

- AddressSanitizer、ThreadSanitizer、UndefinedBehaviorSanitizer
- 数時間以上のEditor／Runtime／Standalone Soak Test
- 別MachineでのGame Module Binary再現性とPackage起動
- Windows x64／DirectX 12以外のHostとGraphics API
- Installer、Code Signing、Store／Device配布
- 一般Asset Import／Cook、Texture／Mesh／Audio Runtime形式
- Game Rendering、3D Viewport、Gizmo、Sound、Effect、Physics
- ECS並列化、Prefab、Scripting／Hot Reload
- 手動UI操作のScreenshot保存または動画記録
- Cold filesystem cache、低速Storage、大容量PackageでのManifest Parse／Hash測定
- PublisherのCopy／Hashを分離したPackage作成時間測定

## Remaining Risks

- Standalone Runtimeの可視出力は固定色Clearで、Game Renderingは未接続
- Runtime Data変換はProject設定とStartup Sceneだけで、一般AssetのDependency解決とCookを含まない
- Blank Game ModuleはABI接続用の最小実装で、Gameplay Framework、Scripting、Hot Reloadを含まない
- Package Contentは同一入力Snapshotで一致するが、別Machineで生成したMSVC BinaryのByte一致は保証しない
- Build／PackageはLocalのVisual Studio、CMake、vcpkg環境へ依存し、Remote BuildとDistributed Cacheを含まない
- ReleaseでSkipされた4件はM16変更とは無関係だが、Release構成のDebug Layer／InfoQueue／DRED経路は未実行
- Parse／Hash Baselineは829,871 bytesの最小M16 Packageに限られ、将来のAsset追加時のCostを予測しない
