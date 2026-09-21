# M14 Runtime Application and Play Session Completion Gate

## Gate Result

M14の先行Issue #208から#216、#275がGitHub上でClosedであり、本Gate Issue #217が
Milestone最後の1件であることを2026-09-08に確認した。

自動検証Gateと通常の実Window Play／Stop WorkflowはPassしている。ADR-0021が手動確認を求める
開始失敗後の再Playは、実WindowからFailureを注入する経路がないため未実行とし、
自動Process Testの結果と残るRiskを明記する。

| Acceptance Gate | Result | Evidence |
|---|---|---|
| M14配下の全Issue | Pass予定 | 先行10件Closed、本PRが最後の#217をCloseする |
| Debug／Development／Release Build | Pass | Source tree `c04f4d312a326bcc57a5ee435c4e3e41d2a8a0d4`で全Targetを3構成Build成功 |
| CTestとHeadless／Process Test | Pass | Debug／Developmentは238/238、Releaseは234成功と既定4 Skip、失敗0 |
| 手動Play／Stop Workflow | Pass | 通常Play／Stop、Shortcut、10回反復、Console、終了／再Openを実Windowで確認。開始失敗後の再PlayはNot Run |
| ECS Storage／Query契約の非変更 | Pass | M14開始点からの変更一覧にECS Storage／Query API変更なし |
| Renderer／Sound／Effect／Physicsの非追加 | Pass | M14開始点からの変更一覧に対象ModuleのSource変更なし |
| 未実行検証と残るRisk | Pass | 本文末尾へ明記 |

Releaseの全CTestでは、Debug Layer／InfoQueue／DREDを必要とする既存4 Testが構成条件によりSkippedとなった。
M14対象のInput、GameCore、Runtime、RuntimeHost、EditorCore、Editor ImGui、Editor Workflow Testは
全構成で実行され、成功した。

## Verified Source Tree

全Target Build、全CTest、実Window検証は、#275のsquash merge Commit
`56f6aa68a3980cb8f79141060d23bbd54688d46d`、Git tree
`c04f4d312a326bcc57a5ee435c4e3e41d2a8a0d4`に対して実行した。

#217は検証結果と証跡文書だけを追加する。最終PR HeadはWindows CIで
Debug／Development／Releaseを再検証する。

## Coverage Map

| M14 Scope | Verification |
|---|---|
| Keyboard／Mouse Input | `Cue.Input.State`、`Cue.Input.Windows.MessageSink`、`Cue.Platform.Windows.Events` |
| Clock／Update Context | `Cue.GameCore.Clock` |
| System Registry／Update Phase | `Cue.GameCore.RuntimeSystemRegistry`、GameCore Process Test |
| Scene Session／Rollback | `Cue.Runtime.SceneSession`、Runtime Process Test |
| Runtime Application Loop | `Cue.Runtime.ApplicationSession`、`Cue.RuntimeHost.Smoke`、RuntimeHost Failure Test |
| Play Session Controller | `Cue.EditorCore.PlaySession` |
| Play／Stop UI／Console | `Cue.Editor.ImGui.PlaySession` |
| Process Workflow | `Cue.Editor.Workflow.ProcessRoundTrip` |
| Runtime Window配置 | `Cue.Editor.ImGui.HierarchyInspector`、実WindowのPointer操作 |
| D3D12非依存のRuntime契約 | Runtime／GameCore／InputのPublic HeaderとDependency Test |

## Manual UI Status

Computer UseでLocal Windows x64のDebug版`CueProjectHubTool.exe`と`CueEditorTool.exe`を操作した。
Test Project `M12 Gate`の保存済み`Scenes/NewScene.cuescene`を使用し、操作中の画面は都度確認したが、
Screenshotまたは操作録画はFileとして保存していない。詳細は`ManualWorkflow.md`へ記録した。

完了した主な確認は次のとおりである。

- PointerとF5／Shift+F5のPlay／Stop
- Files focus時のF5更新とShift+F5停止
- Authoring SceneのSelection、Dirty State、Transform保持
- 1秒以上のPlayを含む10回連続Session
- Runtime ConsoleのFilter、Clear、Copy
- Play中終了のCancel、継続、Stopして終了
- Project Hubからの再Openと保存済みSceneの再読込み

## Scope Audit

M14開始点`9d0efb4c`から検証対象Commitまでの変更は、Input、GameCore、Runtime、RuntimeHost、
EditorCore、Editor ImGui、EditorTool、Test、CMake、ADR、手動手順に限定されている。

- ECS Storage／Queryの公開契約は変更していない。
- Renderer、Sound、Effect、PhysicsのSourceを追加または変更していない。
- Game Rendering、Parallel ECS／Job System、Scripting／Hot Reload、Asset Pipelineは実装していない。
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
- `git diff --name-only 9d0efb4c..56f6aa68`
- `git diff --check`

## Automated Results

- Dependency Restore: Dear ImGui `1.92.6`を固定済みvcpkg Manifestから復元成功
- CMake Configure: 成功、238 Test登録
- Debug: 全Target Build成功、238/238 Test成功
- Development: 全Target Build成功、238/238 Test成功
- Release: 全Target Build成功、234 Test成功、既定4 Test Skip、失敗0
- Release Skip:
  - `Cue.RHI.D3D12.FrameCommand.InfoQueue300`
  - `Cue.RHI.D3D12.RtvHeap.InfoQueue`
  - `Cue.RHI.D3D12.SwapChain.InfoQueue`
  - `Cue.RHI.D3D12.SwapChain.DeviceRemovalDredFailure`

## Not Run

- AddressSanitizer、ThreadSanitizer、UndefinedBehaviorSanitizer
- 数時間以上のRuntime／Editor Play Soak Test
- 複数ThreadまたはJob Systemを使うRuntime Update
- 複数Editor Processから同一Projectを開く競合検証
- Process強制終了、OS Shutdown、実Hardware喪失中のPlay Session終了
- Game Rendering、Sound、Effect、Physics、Scripting／Hot Reload、Asset Pipeline
- Gamepad、IME、Input Mapping Asset、Rebinding UI
- 手動UI操作のScreenshot保存または動画記録
- 実WindowのLoad／System Start Failure Injectionと、その失敗表示からの再Play

## Remaining Risks

- Runtime Loopは単一Thread契約だけを検証しており、並列UpdateとJob Systemは未設計
- 手動反復は10回かつ各1秒以上であり、長時間Sessionや数千回の再起動を保証しない
- Runtime Consoleは対話操作とBounded Log Testで確認したが、大量Logの長時間負荷は未測定
- 開始失敗後の再Playは`Cue.Editor.Workflow.ProcessRoundTrip`で自動検証しただけで、実WindowのFailure表示と再操作は未確認
- Editor入力とRuntime入力の最小Routingだけを扱い、Input Mapping、Gamepad、IMEは対象外
- RuntimeはAuthoring Scene Snapshotから生成されるが、Game Renderingや実Game Systemをまだ接続していない
- ReleaseでSkipされた4件はM14変更とは無関係だが、Release構成のDebug Layer／InfoQueue／DRED経路は未実行
