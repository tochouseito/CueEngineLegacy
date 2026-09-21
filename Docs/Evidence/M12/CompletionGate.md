# M12 Editor Core and Project Hub Completion Gate

## Gate Result

M12の先行Issue #156から#164がGitHub上でClosedであり、本Gate Issue #165がMilestone最後の1件であることを
2026-09-06に確認した。本変更のPRが`Closes #165`でMergeされた時点で、M12は10/10 Closedとなる。

| Acceptance Gate | Result | Evidence |
|---|---|---|
| M12配下の全Issue | Pass予定 | 先行9件Closed、本PRが最後の#165をCloseする |
| Debug／Development／Release Build | Pass | 既存CheckoutとFresh Cloneの両方で全Targetを3構成Build成功 |
| CTestとHeadless／Process Test | Pass | 既存CheckoutとFresh Cloneの両方で3構成とも204 Test、失敗0。Headless RuntimeWorldと8件のProcess Label Testを含む |
| 手動UI Workflow | Pass | 2026-09-06にUserが`ManualWorkflow.md`の実Window操作を実施し、問題なしと報告。Test Projectは確認後に削除済み |
| Save失敗時の元File保全 | Pass | `Cue.EditorCore.DocumentState`と`Cue.IO.Storage`が書込失敗、競合、保存未確定時の元File保全を決定的に検証 |
| Runtime Graphics機能の非追加 | Pass | M11完了CommitからM12 Headまで`Engine/Source/RHI`と`Engine/Source/RuntimeHost`に差分なし。追加D3D12 HostはTool UI表示境界のみ |
| 未実行検証と残るRisk | Pass | 本文末尾へ明記 |

Releaseの全CTestでは、Debug Layer／InfoQueue／DREDを必要とする既存4 Testが構成条件によりSkippedとなった。
M12対象のEditor、EditorCore、ProjectHub、ToolHost、Process Testは全構成で実行され、成功した。

## Coverage Map

| M12 Scope | Verification |
|---|---|
| EditorDocument境界 | `Cue.EditorCore.DocumentState`がDocument Identity、Selection、Dirty State、Revision、Close Stateを検証 |
| Scene編集Command | Object追加／削除／Rename／Reparent／Transform編集と失敗時のDocument不変を検証 |
| Undo／Redo | Transaction単位のUndo／Redo、分岐後のRedo破棄、History上限、永続変更後のHistory破棄を検証 |
| Save／Reload／Recovery | 保存、Save As、競合、破損Scene Reload失敗、保存未確定の再検証／破棄、Recovery再Openを検証 |
| Project Hub | `Cue.ProjectHub.Service`とWindows／ImGui Testが一覧、選択、Pin、作成、互換性表示、Keyboard／Cancelを検証 |
| Process境界 | `Cue.ProjectHub.Windows.EditorProcess`がEditor起動と正常／異常終了後の回復を検証 |
| End-to-End Workflow | `Cue.Editor.Workflow.ProcessRoundTrip`がProjectとSceneを作成し、4回の実Editor ProcessをまたいでRecovery、Dirty Close Save、再Open、ID維持を検証 |
| Tool UI Host | `Cue.ToolHost.WindowsD3D12.Smoke`がWin32、D3D12、Dear ImGuiのTool表示Hostを実Windowで検証 |
| 手動制作Workflow | `ManualWorkflow.md`に沿ってProject作成、Editor起動、Scene編集、Undo／Redo、Save／Save As、再起動、Error／Recovery経路をUserが確認 |

## Save Failure Safety

`Cue.EditorCore.DocumentState`の`test_scene_persistence_workflow`は、既存Sceneを編集した後に保存先への書込を失敗させ、
結果が`SceneSaveStatus::NotPublished`であること、DocumentがDirtyのままであること、元Scene Fileの内容が変化しないことを
検証する。Save As失敗では元Locatorを維持し、既存の別Locatorへ競合した場合も相手Fileを置換しない。

同Testは`PublishedButDurabilityUnknown`、`PublishedButBackupDurabilityUnknown`、
`PublishedButVerificationFailed`を成功と区別し、再検証または不確定記録の破棄までDocumentをDirtyかつClose判断待ちに保つ。
`Cue.IO.Storage`はAtomic置換前の失敗では元FileとBackupを保持し、公開後のDurability不明を別Statusとして扱う下位契約を検証する。

## Runtime Graphics Boundary

M11完了Commit `527c0d41d26eb4e4d51052724a8ecec53d657b67`からM12 Gate開始時のSource Commit
`1a68be63accf4e6c240b63e4550d8a022096f390`まで、`Engine/Source/RHI`と`Engine/Source/RuntimeHost`には変更がない。
M12で追加した`Cue.ToolHost.WindowsD3D12`はProject HubとEditorのDear ImGui描画を提供するTool所有のPresentation Hostであり、
Runtime WorldのGame Rendering、3D Viewport、Camera、Material、Lighting、FrameGraph機能は追加していない。

## Manual UI Record

2026-09-06にUserがDebug Toolを実Windowで操作し、Project HubからTest Projectを作成してEditorを別Processで起動した。
Sceneの作成、Hierarchy／Inspector編集、Undo／Redo、Save／Save As、Reload、Editor再起動後の再Open、Dirty Close、
不正Locator、Recoveryを確認し、問題なしと報告した。検証用ProjectはUserが確認後に削除しており、Repositoryへ含めていない。
この記録はUser報告を正本とし、AgentによるScreen Captureまたは操作録画は保存していない。

## Validation Commands

- `git clone --branch codex/m12-165-completion-gate --single-branch https://github.com/tochouseito/CueEngine.git <clean-checkout>`
- Dependency Restore（当時実行済み。現行の再実行は`README.md`の手順で`ToolRoot`、`InstallRoot`、
  `GitExecutable`を明示する）
- `cmake --preset windows-vs2026`
- `cmake --build --preset windows-vs2026-debug --parallel`
- `cmake --build --preset windows-vs2026-development --parallel`
- `cmake --build --preset windows-vs2026-release --parallel`
- `ctest --preset windows-vs2026-debug --output-on-failure`
- `ctest --preset windows-vs2026-development --output-on-failure`
- `ctest --preset windows-vs2026-release --output-on-failure`
- `ctest --preset windows-vs2026-debug --output-on-failure -R "^Cue.IO.Storage$"`
- `git diff --name-status 527c0d41d26eb4e4d51052724a8ecec53d657b67..1a68be63accf4e6c240b63e4550d8a022096f390 -- Engine/Source/RHI Engine/Source/RuntimeHost Engine/Tests/RHI Engine/Tests/RuntimeHost`
- `git diff --check`

全TargetのBuildと全204 TestはGate開始時のSource Commit
`1a68be63accf4e6c240b63e4550d8a022096f390`に対して実行した。

Codex ReviewでADR-0019が求めるClean Checkout検証の不足が判明したため、PR Head
`eea55559e9c13f79877d0dbd7d90799a010e4d5b`をGitHubから新しいDirectoryへCloneした。Pin済みvcpkg Toolと
Dear ImGui `1.92.6`のRestore、CacheのないCMake Configure、全TargetのDebug／Development／Release Build、
各構成204 Testを順に実行し、すべて成功した。その後の変更は本証跡とREADMEのDependency Restore手順だけであり、
Build Inputは変更していない。Pull Requestの最終HeadはWindows CIで3構成を再検証する。

Clean CheckoutはWindowsのTemp Directory配下で実行したため、MSBuildは中間／出力Directoryの配置に対して
`MSB8029`を警告した。これはIncremental Buildへの注意であり、今回のCacheなしBuildは3構成ともCompile／Linkに成功した。

最初の制限環境内Debug BuildはWindows SDK設定DirectoryへのAccess Deniedで停止したため、通常のLocal権限で3構成を再実行した。
最初の制限環境内Debug CTestでは`Cue.IO.Storage`だけが失敗し、同Test単独と全204 Testを通常のLocal権限で再実行して成功した。
Gate結果には通常のLocal権限で行った再実行を採用する。

## Not Run

- AddressSanitizer、ThreadSanitizer、UndefinedBehaviorSanitizer
- 数時間以上のProject Hub／Editor連続起動、Scene編集、Save／Reload Soak Test
- 複数Machine、異なるGPU Vendor、異なるWindows／Visual Studio Versionでの検証
- 実Diskの電源断、Process強制終了中の書込、Disk Full、権限喪失を伴うFailure Injection
- 保存未確定状態を実Filesystem障害で発生させる手動UI検証
- 手動UI操作のScreen Captureまたは動画記録
- Game Rendering、3D Viewport、Play Mode、Scripting、Prefab、Asset Import／Cook

## Remaining Risks

- 手動UI結果はUser報告に基づき、再確認用のTest Projectや画面記録は保持していない
- Atomic Saveの詳細な失敗分岐はMemory Filesystemによる決定的Failure Injectionが中心で、実Hardware障害時の完全性は未検証
- Process Round-tripは有限FrameのTest Modeであり、長時間のInteractive SessionにおけるResource寿命は未検証
- Tool Hostは単一Windows／D3D12環境で検証しており、異なるDisplay Scale、Multi-monitor、GPU切替は未検証
- M12 UIはProjectとSceneの基本操作に限定され、Viewport、Game実行、Build／Package、Asset Pipelineは後続Milestone対象
