# M13 Editor Workspace and File Operations Completion Gate

## Gate Result

M13の先行Issue #198から#206、#257、#262がGitHub上でClosedであり、本Gate Issue #207が
Milestone最後の1件であることを2026-09-07に確認した。

自動検証GateはすべてPassしている。実Windowを使うFiles UI WorkflowとNative Dialog確認は、
2026-09-08にUserが実行し、問題なしと報告した。CodexのWindows app controlは利用できなかったため、
手動結果はUser報告を正本とし、画面Captureまたは操作録画は保存していない。

| Acceptance Gate | Result | Evidence |
|---|---|---|
| M13配下の全Issue | Pass予定 | 先行11件Closed、本PRが最後の#207をCloseする |
| Debug／Development／Release Build | Pass | Source tree `3c94b0f3907b792027245afc4a7d552e279b02c3`で全Targetを3構成Build成功 |
| CTestとWindows IO／Watcher | Pass | Debug／Developmentは217/217、Releaseは213成功と既定4 Skip、失敗0。Watcherは各構成20回連続成功 |
| 手動Files UI Workflow | Pass | Userが実WindowでFiles UI、Recovery、Root境界、外部変更、再起動、Native Dialogを確認し、問題なしと報告 |
| Failure InjectionでData／Root保全 | Pass | Atomic Storage、Create／Copy失敗、Restore競合、Root外／Reparse拒否を決定的Testで検証 |
| 未実行検証と残るRisk | Pass | 本文末尾へ明記 |

Releaseの全CTestでは、Debug Layer／InfoQueue／DREDを必要とする既存4 Testが構成条件によりSkippedとなった。
M13対象のIO、ProjectFiles、EditorCore、Editor ImGui、Editor Workflow Testは全構成で実行され、成功した。

## Verified Source Tree

全Target Build、全CTest、Watcher反復検証は、#206 PR Head
`8f75abcee08452baba6f474a2f13e555f3b56674`に対して実行した。#206のsquash merge Commit
`11c68ff14f7555b843330d1cbdf401f42f5b7a6b`と両CommitのGit treeは
`3c94b0f3907b792027245afc4a7d552e279b02c3`で一致する。

#207は検証手順と証跡文書だけを追加する。最終PR HeadはWindows CIでDebug／Development／Releaseを再検証する。

## Coverage Map

| M13 Scope | Verification |
|---|---|
| Directory／File列挙・検索 | `Cue.IO.WorkspaceCore`、`Cue.IO.WindowsWorkspace`、`Cue.ProjectFiles.Create`がRoot相対列挙、検索、上限、消失Entryを検証 |
| Create／Rename／Move／Copy | `Cue.ProjectFiles.Create`と`Cue.EditorCore.FilesWorkspace`が全Mutation、競合、Operation状態、Snapshot同期を検証 |
| Delete／Restore | `Cue.ProjectFiles.Recovery`がProject-local Trash、Catalog再構築、Restore競合、Data保持を検証 |
| Native Dialog | `Cue.Platform.FileDialog`、`Cue.Platform.Windows.FileDialog`、`Cue.ProjectFiles.FileDialogRevalidation`がRequest、Thread、Cancel、Path再検証を検証 |
| External Change／Rescan | `Cue.IO.WindowsWorkspaceWatcher`と`Cue.ProjectFiles.WatcherService`がCreate／Modify／Rename／Delete、Overflow、停止、Rescanを検証 |
| Files ViewModel | `Cue.EditorCore.FilesWorkspace`がSelection、展開、検索、Operation、Error、Watcher停止後のstale状態を検証 |
| ImGui Files UI | `Cue.Editor.ImGui.Files`が操作Intent、日本語Error、Focus限定Shortcut、Delete確認、Escape Cancel、Restoreを検証 |
| Process Workflow | `Cue.Editor.Workflow.ProcessRoundTrip`が実`CueEditorTool.exe`でCreate、Rename、Move、Copy、Delete、RestoreとEditor再Openを検証 |
| Root境界 | IO、ProjectFiles、Dialog Revalidation TestがRoot脱出、Absolute Path、別Area、Case Alias、Reparse Pointを拒否 |

## Failure Injection and Data Safety

| Failure | Expected invariant | Verification |
|---|---|---|
| Atomic Writeの各Failure Point | 元Fileを保持し、未完成Destinationを公開しない | `Cue.IO.Storage` |
| 読取り不能なCreate入力 | DestinationとStagingを残さない | `Cue.ProjectFiles.Create` |
| Directory Copy上限超過 | Destinationを公開せず、既存Destinationを変更しない | `Cue.ProjectFiles.Create` |
| Restore先の競合 | 既存SourceとRecovery Dataの双方を保持する | `Cue.ProjectFiles.Recovery` |
| Root外／別Area／Reparse選択 | Mutationを実行せずRoot内外のDataを変更しない | `Cue.ProjectFiles.FileDialogRevalidation` |
| Watcher Overflow／停止 | 差分推測を止め、staleと再走査要求を公開する | `Cue.IO.WindowsWorkspaceWatcher`、`Cue.EditorCore.FilesWorkspace` |

## Manual UI Status

Computer Useの実行時Stateは`apps: []`であり、利用可能APIは`getState`とBrowser操作に限定されていた。
明示PathからWindows appを取得する正規APIも`cua.getApp is not a function`で利用できなかったため、
CodexはHeadless ImGui TestやProcess Testを手動確認の代用として扱わなかった。

Userは2026-09-08に`Docs/Testing/M13-files-workflow-manual-test.md`と
`Docs/Testing/M13-native-file-dialog-manual-test.md`の実Window確認を実行し、問題なしと報告した。
結果は`ManualWorkflow.md`へ記録した。

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
- `ctest --test-dir out/build/windows-vs2026 -C Debug -R "^Cue\\.IO\\.WindowsWorkspaceWatcher$" --repeat until-fail:20 --output-on-failure`
- `ctest --test-dir out/build/windows-vs2026 -C Development -R "^Cue\\.IO\\.WindowsWorkspaceWatcher$" --repeat until-fail:20 --output-on-failure`
- `ctest --test-dir out/build/windows-vs2026 -C Release -R "^Cue\\.IO\\.WindowsWorkspaceWatcher$" --repeat until-fail:20 --output-on-failure`
- `git diff --check`

## Automated Results

- Dependency Restore: Dear ImGui `1.92.6`を固定済みvcpkg Manifestから復元成功
- CMake Configure: 成功、217 Test登録
- Debug: 全Target Build成功、217/217 Test成功
- Development: 全Target Build成功、217/217 Test成功
- Release: 全Target Build成功、213 Test成功、既定4 Test Skip、失敗0
- Windows Workspace Watcher: Debug／Development／Releaseで各20回連続成功
- #206 Windows CI: 3構成成功

## Not Run

- AddressSanitizer、ThreadSanitizer、UndefinedBehaviorSanitizer
- 数時間以上のFiles操作、Watcher、Editor再起動Soak Test
- 複数Editor Processまたは非協調Writerによる同時Mutation
- 実Diskの電源断、Disk Full、実権限喪失、Process強制終了中の書込
- UNC、Network Drive、ReFS、複数Machine、異なるWindows／Visual Studio Version
- 手動UI操作のScreen Captureまたは動画記録
- Asset Import／Cook、Asset Database、Source Control、Cloud Storage

## Remaining Risks

- Project-local Trashに自動Purgeと容量上限がなく、長期利用時の容量管理は後続設計が必要
- 複数Editor Processと非協調Writerをまたぐ完全なFilesystem Transactionは保証しない
- Spaceまたは非ASCII文字を含む名前は、M13の保守的な入力契約により操作不能Entryとなる
- Windows AdapterはLocal NTFSを検証対象とし、UNC、Network Drive、ReFSは未検証
- WatcherはFilesystem Journalではなく、通知欠落またはOverflow時は権威的な再走査へ退避する
- Native DialogはModalであり、CIでは実選択操作を行わない
- 手動UI結果はUser報告に基づき、再確認用の画面Captureまたは操作録画を保持していない
- 実Hardware障害と長時間Interactive SessionのResource寿命は未検証
