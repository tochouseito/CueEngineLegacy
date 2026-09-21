# ADR-0018: Editor Document, Command Transaction, and Tool UI Contract

- Status: Accepted
- Date: 2026-09-04
- Decision Owners: CueEngine Project
- Amendment: ADR-0019 replaces the Project Hub / ImGui / Platform host dependency edges below
- Amendment: ADR-0031 permits a private `Cue.EngineAssets`／`Cue.Renderer` dependency for typed Create Primitive validation

## Context

M12では、Projectを選択し、Sceneを編集し、Undo／Redo、保存、再読込を行える最小Editor制作Loopを構築する。
ADR-0017は`SceneDocument`をAuthoring Sceneと保存Dataの正本とし、Selection、Dirty、編集履歴、File Locator、
Recovery Policyを将来の`EditorDocument`が所有すると決定した。本ADRは、そのApplication層とTool UIの境界を決定する。

Editor UIが`SceneDocument`、Serializer、Filesystem、`RuntimeWorld`を直接操作すると、同じ操作をHeadless Test、
Automation、将来の別Presentationから再利用できない。さらに、UI Widgetの寿命、Runtime Entityの寿命、永続Objectの寿命が
混在し、Undo、外部変更競合、保存失敗、Editor再起動で状態の正本が不明確になる。

M12ではViewport Rendering、Play Mode、Scripting、Prefab、Asset Import／Cook、Game Runtime Packagingを決定しない。

## Legacy Reference

### Legacy Problem

旧CueEngineも、Projectを選択してEditorを起動し、HierarchyとInspectorからSceneを編集し、保存する必要があった。

### Legacy Approach

旧設計ではEditor Manager、Project Hub、Scene、Command、GUIの責務が近く、UI CallbackからModelやRuntime Objectへ
到達しやすい構造だった。Project Open、Window、Scene、Runtime Objectの寿命もApplication全体の運用へ依存していた。

### Legacy Strengths

- Project選択からScene編集までの経路が短かった
- HierarchyとInspectorの操作をCommandへまとめる意図があった
- Editor固有機能を一つのApplicationとして提供できた

### Legacy Problems

- UI、Authoring Data、Runtime Objectの依存方向が明示されていなかった
- Commandが永続Identityではなく実行中Objectの参照を保持し得た
- Dirty、Saved、Undo位置の関係がRevisionとして定義されていなかった
- 複数変更の途中失敗時に完全Rollbackできる契約が不足していた
- SaveのPublish後不明状態、外部変更、Recoveryの正本が明確でなかった
- Project HubとEditor Processの失敗境界が呼び出し側へ漏れていた

### Current Requirements

- `SceneDocument`を唯一のAuthoring Scene正本として維持する
- Editor Session状態を`SceneDocument`とScene Fileへ混入させない
- すべての編集をStable IDを対象とするCommand境界へ集約する
- 一つのUser操作をAtomicなTransactionとしてUndo／Redoする
- Dirtyを現在Revisionと保存Revisionの比較で判定する
- Save、Reload、Close、Recovery、外部変更競合をHeadlessに検証する
- Project HubをRuntime Graphicsなしで起動可能にする
- ImGuiをPresentation Adapterに限定する
- RuntimeからEditorへの依存を作らない

### New Design

現在の要件から`Cue.EditorCore`を新しいFirst-party Moduleとして設計する。旧Source Code、型、Command実装、
Window Layout、Process起動実装はコピー、移植、部分抽出しない。Legacyは解決対象と境界不足の確認だけに使用する。

### Validation

依存Graph、Revision遷移、Selection整合、Command失敗Rollback、Undo／Redo、保存失敗、外部変更、Recovery、
Project Hub Process監視をUnit TestとProcess Testで検証する。ImGui AdapterはSemantic IntentとViewModelの接続だけを検証する。

## Reference Engine Comparison

| Engine | 参考にする点 | CueEngineでそのまま採用しない点 |
| --- | --- | --- |
| Unreal Engine | Transaction単位の編集、詳細Panel、AssetとWorldの明確な制作Workflow | UObject、Editor Transaction、Package形式、Reflection ABIを共通基底として移植しない |
| Unity | HubからProjectを選び、Hierarchy／InspectorでSceneを編集する理解しやすいLoop | Instance ID、Library内部状態、SerializedObjectをCueEngineの永続IdentityやCommandへ流用しない |
| Godot | Scene Tree中心の編集とTool実行時の軽量なFeedback | Node継承TreeをRuntime ECSまたはEditor Commandの共通Object Modelへ強制しない |
| SOL-AVES | Runtimeと制作Pipelineを分け、Iterationの待ち時間を独立した設計課題として扱う視点 | 公開資料から非公開ABI、内部Format、実装詳細を推測せず、Source CodeやData Layoutを取り込まない |

CueEngineは、ProjectからScene編集までの短い導線、Transaction単位の安全な編集、明確なHierarchy／Inspectorを取り入れる。
一方、各Engine固有のObject Model、Serializer、Package、Reflection ABIは採用せず、M09からM11で確立した
Project、Schema、Scene、Runtime World境界へ接続する。

## Decision

### Module Boundary

`Cue.EditorCore`はPlatform、Window、ImGuiに依存しないApplication Moduleとする。M12の初期依存は
`Cue.Foundation`、`Cue.IO`、`Cue.Project`、`Cue.Scene`、`Cue.Schema`だけを許可する。

ADR-0031のM19 Amendmentとして、`Cue.EditorCore`のPrivate実装だけが`Cue.EngineAssets`と`Cue.Renderer`へ
依存してよい。用途はCreate Primitive IntentをTransactionへ変換する直前のTyped Catalog解決、Mesh Componentの
Type／Field検証、Asset ID一致検証に限定する。公開Header、ViewModel、Intentは`Cue.EngineAssets`／`Cue.Renderer`型を
公開せず、Stable IDと`SceneDocument`の値だけを使用する。これによりImGui以外のPresentationやAutomationが
Catalog外IDまたはMesh以外のComponentを渡しても、Authoring Sceneへ不正値を永続化する前にController境界で拒否する。

依存方向は次のとおりとする。

```text
Cue.ProjectHub.ImGui ----> Cue.ProjectHub ----> Cue.Project / Cue.IO / Cue.Foundation
          `--------------> Cue.ImGui.Core

Cue.Editor.ImGui --------> Cue.EditorCore
          `--------------> Cue.ImGui.Core

Cue.ToolHost.WindowsD3D12 -> Cue.Platform.Windows
          `----------------> Cue.ImGui.Backend.Win32D3D12

Cue.ProjectHub.Windows --> Cue.ProjectHub / Cue.IO.Windows / Cue.Foundation.Windows

Cue.ProjectHub.Tool -----> Cue.ToolHost.WindowsD3D12 / Cue.ProjectHub.ImGui
          `--------------> Cue.ProjectHub.Windows / Cue.IO.Windows

Cue.Editor.Tool ---------> Cue.ToolHost.WindowsD3D12 / Cue.Editor.ImGui / Cue.EditorCore / Cue.IO.Windows

Cue.RuntimeHost ---------> Cue.GameCore / Cue.Scene
```

`Cue.EditorCore`の公開APIは`Cue.Platform`、`Cue.RHI`、D3D12、Renderer Native型、ImGuiへ依存しない。
Private実装の`Cue.EngineAssets`／`Cue.Renderer`依存は上記Create Primitive検証だけに限定し、描画実行、GPU Resource、
Runtime World抽出を所有しない。`Cue.Scene`、`Cue.Project`、`Cue.GameCore`、`Cue.RuntimeHost`は`Cue.EditorCore`へ依存しない。

この依存追加によりEditorCoreのBuild Closureは大きくなるが、CatalogとRenderer Schemaの正本をUI側へ複製せず、
すべてのPresentationに同じFail-closed検証を適用できる。Catalog／Schema Validatorを注入する案は、呼び出し側が
正本と異なるValidatorを供給でき、公開APIへM19専用の抽象を追加するため採用しない。

M12のImGui HostはEditor用Descriptor、Game Renderer、Viewport Render Targetを要求しない。WindowとImGui Backendに必要な
Platform接続はPresentation Hostが所有し、`Cue.EditorCore`の公開APIへNative Handleを出さない。
`Cue.ProjectHub.ImGui`と`Cue.Editor.ImGui`は`Cue.Platform`へ直接依存せず、Platform接続とD3D12 compositionは
ADR-0019で決定した`Cue.ToolHost.WindowsD3D12`が提供する。各最終Executable RootがHost、Application Service、
Presentation Adapter、Platform Adapter、Filesystem Rootを組み合わせて所有する。本修正はPresentation／Hostの依存辺だけを更新し、
本ADRのEditorDocument、Command、Transaction、Save、Process契約を変更しない。

`Cue.Editor.Tool`は起動後に再検証したProject DescriptorからSource Assets RootとSaved Rootを`Cue.IO.Windows`で生成し、
`ScenePersistenceServices`へ注入する。これらのRootは`EditorController`と`ProjectWorkspaceSession`より長く一意所有する。
Process境界からFilesystem ObjectやNative Handleを受け取らず、Presentation Adapterへも公開しない。

Editor起動時は、既存Project RootをBindingしてDescriptorを再検証した後、Source Assets RootをOpen-existing、Saved Rootを
Create-or-openとして扱う。Source Assetsの欠損は`NotFound`、通常File衝突は`TypeMismatch`、Reparse Pointは
`UnsupportedEntry`で起動失敗とする。Savedが欠損している場合だけ、Binding済みProject Rootの`create_directories()`で
Project相対Rootを作成し、File／Reparse Point衝突は同じ分類で失敗する。Root BindingまたはSession生成が途中で失敗した場合、
部分Objectは逆順に破棄するが、作成済みSaved Directoryは削除しない。競合Processの利用や作成後の内容を判別せず削除する方が
Data Loss Riskになるためであり、空Directoryの残留を許容して再試行時に冪等に再利用する。#162ではADR-0014に従い、
Native Createが`AlreadyExists`相当になった競合経路もEntryを再検査し、Directoryは成功、Fileは`TypeMismatch`、
Reparse Pointは`UnsupportedEntry`へ分類するようWindows `create_directories()`を適合させてからSaved Rootへ使用する。

### EditorDocument Ownership

`EditorDocument`は、一つの開いているSceneに対するEditor Session状態を一意に所有する。

所有する状態は次のとおりとする。

- 一つの`SceneDocument`
- Project Rootから解決されるScene Locator
- 現在State Revisionと最後に確定保存されたState Revision
- SelectionとHierarchy展開状態
- Undo／Redo History
- 外部変更Conflict状態
- RecoveryとClose Workflowの進行状態

`EditorDocument`は`RuntimeWorld`、`SceneInstance`、Runtime `EntityHandle`、Renderer Object、ImGui Widgetを所有しない。
`ProjectWorkspaceSession`は一つのProject Descriptorと複数の`EditorDocument`を所有するが、M12のUIは一つのScene Tabだけを
表示してよい。複数TabのPresentationは将来Scopeとする。

`EditorDocument`のMutationは作成Threadに限定する。Read-only Viewは同じThreadで取得し、非同期I/O結果は
Session Generationを照合してからOwner Threadで適用する。M12ではBackground Threadから直接Mutationしない。

### Stable Editor Identity

Selection、Command、Historyは`SceneAssetId`、`ObjectId`、`ComponentInstanceId`を使用する。
Runtime `EntityHandle`、Object Address、Container Index、ImGui ID、File PathだけをIdentityとして保存しない。

Editor Session内では`EditorDocumentId`とSession Generationを一時Identityとして使用できる。ただし、これらはScene Fileへ
永続化せず、Process再起動後の同一性へ使用しない。

### State Revision and Dirty

各EditorDocumentは単調増加し、再利用しない`DocumentStateId`を発行する。受理された永続Data変更だけが新しいStateを作る。

- `currentStateId`: 現在表示しているAuthoring状態
- `savedStateId`: 最後に確定保存されたAuthoring状態
- `nextStateId`: 次に発行する値

Dirtyは`currentStateId != savedStateId`で判定する。選択、Hierarchy展開、Panel配置などのSession-only変更はStateを進めない。
Undoで保存済みStateへ戻った場合はCleanになり、Redoで離れた場合はDirtyになる。Redo分岐を破棄してもState IDを再利用しない。

新規未保存Sceneは保存先が確定していない状態を別に持ち、State IDが一致していても保存要求が必要である。
単純な`bool isDirty`を独立した正本として保持しない。

### Command Boundary

Presentationは`EditorIntent`を生成し、`EditorController`がIntentを検証して具体的なCommandまたはWorkflowへ変換する。
UIは`SceneDocument`のMutation API、Scene Serializer、Filesystem、`RuntimeWorld`を直接呼ばない。

Commandは次の契約を満たす。

- 対象EditorDocument、Scene、Object、ComponentをStable IDで指定する
- Runtime Pointer、Runtime `EntityHandle`、ImGui Stateを保持しない
- 実行前に対象存在、Document一致、Schema、Hierarchy Cycle、Depth、値範囲を検証する
- 失敗時にDocument、Selection、Revision、Historyを変更しない
- 単体またはTransaction内のCommand成功だけでは`DocumentStateId`を発行しない
- 診断可能な`Result`を返す

Command TypeはEditor Coreの公開Requestであり、`SceneDocument`の公開Modelを万能Command基底型へ変更しない。
Renderer、Physics、Audio、ScriptのRuntime ObjectをCommand対象へ追加する場合は、各機能のAuthoring Schemaが決定された後に
別Issueで拡張する。

### Transaction and Strong Failure

一つのUser操作は一つの`EditorTransaction`として扱う。Transactionは一つ以上のCommand Requestと表示用Labelを持ち、
全変更を一つのHistory EntryとしてCommitする。

Transaction実行前に、Editor Coreは現在の完全なAuthoring状態を`SceneDocumentCheckpoint`として取得する。
CheckpointはScene ID、Object、Hierarchy、Transform、Component値、未知Component／Field、Extension Dataを保持し、
Runtime状態とEditor Session状態を含まない。

すべてのCommand適用と最終`SceneDocument::validate()`が成功した場合だけ、Revision、History、Selection整合をCommitする。
途中失敗またはValidation失敗時はCheckpointから完全に復元し、Transaction前の公開状態を維持する。
一つ以上の永続Data変更を含むTransaction Commitが成功した時点で、Transaction全体に対して一つだけ新しい
`DocumentStateId`を発行する。Commandごとの仮Revisionまたは公開Revisionを発行しない。

`SceneDocumentCheckpoint`はRuntime実体化用`SceneSnapshot`と別型とする。M12では正しさと未知Data保持を優先して完全Checkpointを
基準実装とし、Subtree／Component単位の差分最適化は同じ復元契約を満たす場合だけ追加できる。

### Undo and Redo

History EntryはTransactionのLabel、Before／After `SceneDocumentCheckpoint`、Before／After `DocumentStateId`を所有する。
UndoはBefore、RedoはAfterを復元する。復元後にScene検証を行い、Selectionから存在しないIDを除去する。

新規Transaction成功時はRedo Stackを破棄する。Historyは無制限に保持せず、Entry数と推定Byte数の両方で上限を持つ。
上限超過時は最古EntryからTransaction単位で破棄する。保存済みStateのHistory Entryが破棄されても`savedStateId`は維持し、
Dirty判定のためにState IDを再利用しない。

Persistent Undo History、Collaborative History、Play Mode変更取込はM12対象外とする。

### Selection Reconciliation

Selectionは順序を持つStable `ObjectId`集合とPrimary Selectionで表す。Command、Undo、Redo、Reload後に、存在しないObject IDと
Component IDを除去する。Primary Selectionが消えた場合は、残るSelectionの先頭をPrimaryとし、残らない場合は未選択とする。

Delete後に親や隣接Objectを自動選択するかはPresentation Policyとし、Coreの既定動作は削除対象をSelectionから除去するだけとする。

### Save, Reload, and External Change

保存は`EditorController`のWorkflowとして、`Cue.Scene`のAtomic Saveを使用する。UIはSerializerとFilesystemを直接呼ばない。

`ProjectWorkspaceSession`は一つの`SceneSaveCoordinator`をSession開始から終了まで一意所有する。Coordinatorは同じSession内の
Scene Save／Save As／Recovery Publishを直列化し、Sceneごとの進行中Saveと`SceneWriteLease`を所有する。
`EditorDocument`はCoordinatorを所有せず、Session終了より長く参照しない。

`SceneWriteLease`は本文Pathを排他範囲とする。Leaseは本文Entry確認前に取得し、
旧Byte列読込、Backup公開、本文公開、公開結果の検証、結果状態の記録が完了するまで保持する。Windows AdapterはRootのVolume／File
IdentityとDestination Comparison KeyのDigestからProcess環境に依存しないGlobal Named Mutex名を構築する。
別Rootから同じ物理RootとFileへ到達するCueEngine Writerは、`TEMP`／`TMP`やCurrent Directoryが異なっても同じMutexで競合する。
取得失敗は非待機のBusyとして返し、Process終了時はOSがMutexをAbandonedとして次のWriterへ回収可能にする。

M12の同期実装では、Owner Thread限定の`EditorController` Save WorkflowがSession内Coordinatorを担い、`Cue.IO`のMove-only
`FileWriteLease`をSave結果の状態記録まで保持する。排他用Keyは本文Comparison Keyから構築する。
Lease Stateは取得元の正確なPath Keyと取得Thread IDも保持し、Conditional Publish先と実行Threadの一致を要求する。
`FileWriteLease`のMove、Conditional Write、破棄は取得Threadに限定し、違反したWriteは拒否、違反した破棄はFatalとする。
Named Mutex名は固定長Digestを使うため、有効な最大長Locatorへ追加Suffixを付けず、利用者FileをLock Entryとして開くこともない。
`write_file_atomic_if_unchanged`はLease所有PathとExpected FingerprintをTemporary FileのPublish直前に再検査し、`Missing`期待では
Replace Flagを使用しない。既存File期待では再検査後に限りReplaceし、上記の非協調Writerに対する残存競合窓を許容する。

M12では既存Scene本文が複数Hard Link名を持つ場合を`UnsupportedEntry`としてSave／Save Asの置換対象から拒否する。
Windows AdapterはLease取得後、Backup作成前にNative File InformationのLink Countを検査する。別名ごとに異なるLockを取得して
同じFile Identityを同時置換することを許可しない。Hard Linkを安全に編集する必要が生じた場合は、Volume／File IdentityをKeyにする
Cross-process Leaseを別Research Issueで決定する。

協調しない外部ProcessはLease Protocolに参加しないため、CoordinatorはLease取得直後かつBackup作成直前にFile Fingerprintを再取得し、
操作が保持するDestination Expected Fingerprintと一致しなければ本文を公開せずExternal Conflictへ遷移する。Save Asで期待値が
`Missing`の場合は、Destinationが存在しない場合だけ成功するCreate-new Publishを使用し、公開直前に作成されたEntryを置換しない。
Backup元Byte列もExpected FingerprintのSize／Digestと照合し、一致した旧Byte列だけをSibling Backupへ公開する。
本文のConditional Publish直前には別のFingerprint再検査を維持し、Backup前検査後の変更を本文へ上書きしない。
旧Byte列は本文Publishが成功するまでMemoryに保持し、本文が未公開またはDurability不明なら既存Backupを変更しない。
本文Publish成功後だけSibling BackupをAtomic更新する。Backup更新失敗時は本文を公開済みとしてVerification Failedを返し、
既存Backupを維持する。Backup更新が公開済みだがDurability不明の場合は、本文側のDurability不明と区別した
`PublishedButBackupDurabilityUnknown`を返し、保存前の旧Byte列を結果へ保持する。
Sibling BackupとAtomic Write用Temporary FileはFilesystem Adapterが本文LocatorからNative内部Pathとして導出し、
Portable `RelativePath`のPath長・Segment長上限を再適用しない。Userが指定できるScene Locatorの有効範囲を内部Suffixや
Temporary名の長さによって狭めず、導出先にもRoot境界、Reparse Point拒否、同一Directory公開の検査を適用する。
WindowsのSibling Backupは本文と同じDirectoryに、利用者`RelativePath`では表現できない`.`開始のHidden Segmentとして導出する。
`.backup`を含む有効な利用者Locatorは通常Fileとして独立させ、別Sceneの正本をRecovery Backupとして置換しない。
この契約は`FilesystemRoot`の各Adapterが必ず実装し、Portable上限を再適用する共通既定実装は持たない。

既存Destinationの置換について、WindowsのPath単位Fingerprint再検査と`MoveFileExW`による公開の間を、Lease Protocolへ参加しない
Processに対してAtomicなCompare-and-Publishにはできない。Publish後の再読込はCandidateが公開されたことの検証であり、その間に上書きした
外部変更を検出する競合検査として扱わない。M12で保証する同時Writerは`SceneWriteLease`へ参加するCueEngine Processだけとし、
非協調Processによる再検査後の同時書込みは未対応の競合として明示する。外部変更の監視または事前Fingerprint不一致を検出したDocumentは
Saveを開始せずExternal Conflictへ遷移し、Reload、Save As、Cancelを要求する。非協調Writerまで含む既存FileのCompare-and-Publishが
必要になった場合は、Windows Native境界と復旧契約を別Research Issueで決定する。

通常SaveのDestination Expected FingerprintはEditorDocumentのBase Fingerprintとする。Save Asでは選択した保存先のFingerprintを
操作開始時に取得し、新規Fileなら明示的な`Missing`を期待値とする。Save As開始時にEditorDocumentのLocatorまたはBase Fingerprintを
変更せず、保存先への`Committed`成功時だけ切り替える。

Save開始時の`currentStateId`、Destination Locator、Destination Expected Fingerprint、Candidate Byte列のContent Digest、
Candidate Checkpointを`PendingSaveRecord`としてEditorDocumentが所有する。結果状態が確定するまで破棄しない。
結果は次のように処理する。

| Save結果 | Editor状態 |
| --- | --- |
| `Committed` | `savedStateId`をSave開始時Stateへ更新し、Destination Locatorと新しいFingerprintを記録する。現在Stateが進んでいればDirtyを維持する |
| `NotPublished` | Document、History、Dirty、現在Locator、元Fileを維持し、失敗を通知する |
| `PublishedButDurabilityUnknown` | Recordを保持したSave Uncertainへ遷移し、明示的な再確認を要求する |
| `PublishedButBackupDurabilityUnknown` | 保存前Byte列を含むRecordを保持したSave Uncertainへ遷移し、Backupだけの再試行を要求する |
| `PublishedButVerificationFailed` | Recordを保持したSave Uncertainへ遷移し、再確認後にExternal Conflictか保存済み状態を確定する |

保存中に編集が進んだ場合も、保存開始時Stateだけを`savedStateId`として記録するため、現在Stateとの差によりDirtyを維持する。
Undoで開始時Stateへ戻ればCleanになる。M12の同期実装ではOwner ThreadをBlockしてよいが、将来非同期化してもこの契約を維持する。

Save Uncertainの再確認は、Coordinatorが`PendingSaveRecord`のDestinationへ新しいLeaseを取得して行う。
`PublishedButDurabilityUnknown`は、ADR-0014で定義したWindows Publish境界だけでは公開済みFileと親Directoryに対する独立した
Durability Barrierを再試行できないため、Byte列の再読込一致では解除しない。再保存前に、Leaseを保持したまま現在のDestinationを
上限付きで再読込し、完全Parse、Migration、ValidationしたScene IdentityとDigestがRecordのCandidateに一致することを確認する。
最初の本文PublishがDurability不明になった時点で、Lease内で取得済みの保存前Byte列もRecordへ保持し、後続Retryで上書きしない。
一致した場合だけ、その時点のDestination FingerprintをRetry用Expected Fingerprintとして新しく記録し、同じCandidateを再保存する。
再保存時のSibling Backupには現在のCandidate本文ではなく、Recordに保持した最初の保存前Byte列を使用する。
元の操作開始時FingerprintまたはSave Asの`Missing`をRetryへ流用しない。一致しない、再読込できない、または別Scene Identityの場合は
External Conflictへ遷移し、Destinationを上書きしない。

Retry用Expected Fingerprintを用いた`Cue.Scene`のAtomic Saveから新しい`Committed`を得た場合だけDurabilityを確定する。
この場合はRecordの開始時Stateを`savedStateId`へ設定し、Destination Locatorと現在Fingerprintを記録してUncertainを解除する。
再保存が失敗または再び`PublishedButDurabilityUnknown`になった場合はRecordとUncertainを維持する。

`PublishedButBackupDurabilityUnknown`は、本文を上限付きで再読込してCandidate DigestとScene Identityが一致することを確認後、
同じLease範囲でRecordに保持した保存前Byte列をSibling BackupへだけAtomic再書込みする。本文は再保存しない。
Backup再書込みが失敗または再びDurability不明になった場合はRecordと保存前Byte列を維持し、成功した場合だけ
本文Fingerprintを再取得してCandidate Byte SizeとDigestが維持されていることを確認する。一致した場合だけRecordの開始時Stateを
`savedStateId`へ設定してUncertainを解除し、一致しない場合はRecordを保持したままExternal Conflictへ遷移する。

`PublishedButVerificationFailed`はDurability成功済みであるため、本文を上限付きで再読込し、完全Parse、Migration、Validationした
Byte列のDigestをCandidate Digestと比較してよい。DigestとScene Identityが一致した場合は、Recordの開始時Stateを
`savedStateId`へ設定し、Destination Locatorと現在Fingerprintを記録してUncertainを解除する。

どちらの解除経路でも、現在Stateが進んでいればDirtyを維持する。再読込内容が一致しない、再読込できない、別Scene Identityである場合は
External Conflictへ遷移し、RecordとCandidate Checkpointを保持したままReload、Save As、Retry Save／Verification、Cancelの
明示Intentを要求する。
明示的なDiscardまたはSession CloseまでRecordを破棄しない。
通常SaveとDurability Retryは、`Committed`後に取得した最終Fingerprintの存在、Byte Size、Content DigestをCandidateと照合する。
非協調Writerにより一致しない場合はSaved Stateを更新せずExternal Conflictとし、Memory上の編集をDirtyのまま維持する。
Save Uncertain中の通常SaveとReloadはRecordを暗黙破棄またはClean化し得るため拒否する。`retry_uncertain_save`または
`discard_uncertain_save`の明示Intentを先に要求し、DiscardはDocument、History、Dirtyを変更しない。Recovery適用も同じRecordを
孤立させ得るため、RetryまたはDiscardでSave Uncertainを解決するまで拒否する。

File Fingerprintは最終更新時刻だけに依存せず、File SizeとContent Digestを含む。外部変更を検出した場合は暗黙に上書きせず、
Reload、Save As、Cancelの明示Intentを要求する。

Reloadは現在Fileを一時Documentへ完全Load、Migration、Validationしてから入れ替える。失敗時は現在Document、History、Selection、
Dirtyを維持する。成功時はHistoryをClearし、新しいState IDを発行して`currentStateId`と`savedStateId`を一致させる。

M12の単一Scene UIでNewまたはOpenへ切り替える場合は、対象を二つ目の準備済みDocumentとして完全作成またはLoadしてから、
現在DocumentのSave、Discard、Cancel判断を開始する。対象の作成・Load失敗またはCancelでは準備済みDocumentだけを破棄し、
現在DocumentとSelection、History、Dirtyを維持する。Reloadは現在Documentを閉じず、上記の一時Document経路を直接使用する。
新規Sceneの初回保存はDestinationの`Missing`を必須Expected Fingerprintとし、準備時またはWrite Lease取得までに同名Entryが
作成された場合は既存Fileを置換せずExternal Conflictとして失敗する。既存初回保存先の置換は別の明示確認を要求し、通常の
Save操作やSave Uncertain RecordのDiscardだけを上書き許可として扱わない。
保存済みSceneまたはRecoveryがExternal Conflict状態でも編集内容を退避できるよう、Editorは`Ctrl+Shift+S`のSave Asを公開する。
Save Asの初回試行はDestinationの`Missing`を要求し、既存Entryは明示的な上書き確認を経た場合だけ置換する。

### Recovery

Recovery FileはADR-0013で定義したProject Descriptorの`Saved` Root配下にある`Editor/Recovery`へ置き、Scene正本と異なるLocator、
Metadata、Lifecycleを持つ。User Workspace、Source Assets、Runtime Assets、Generated、CacheへRecovery本文を置かない。

RecoveryはVersion付きEnvelopeとし、MetadataはRecovery Format Version、Project ID、Scene ID、正本Locator、Base Fingerprint、
State ID、Scene Data Digestを含む。既知の古いVersionは`N -> N + 1`の連続MigrationをMemory上で完了し、現行Versionとして
再検証してから利用する。Migration Step欠落、Resource上限超過、未来Version、未知必須FieldはRecoveryを削除または正本へ適用せず、
`UnsupportedRecovery`として診断し、元Recovery Fileを維持する。Recoveryを開いただけでは暗黙に現行Versionへ書き戻さない。

Recovery書込み成功は`savedStateId`を更新しない。起動時に有効なRecoveryを検出した場合は、正本を暗黙置換せず、Recover、Discard、
Inspectの明示Intentを要求する。Recover後のDocumentはDirtyな新Stateとして開き、通常Saveが成功するまで正本としない。
Autosave対象はDirtyなDocumentに加え、`currentStateId == savedStateId`でも保存先をまだ持たない新規Documentを含む。
Editor ToolのFrame Compositionは、Dirtyな各Persistent Stateを一度だけ`autosave_recovery`へ接続する。同じStateの失敗を毎Frame
再試行せず、次のPersistent Stateで再試行してUIへ失敗を報告する。Save Uncertain中はRecoveryを更新せず、先にRetryまたはDiscardを
要求する。
`ignore_recovery`は現在Sessionの表示だけを解除し、`discard_recovery`はSaved RootのRecovery File削除成功時だけ候補を解除する。
削除失敗時はRecovery Fileと候補状態を維持する。

未保存Sceneを再起動後にも発見できるよう、`Editor/Recovery/Registry.cueindex`へProject IDとRecoveryを作成したScene IDを
Version付きのAppend-only Registryとして保持する。AutosaveはEnvelopeより先にScene IDをRegistryへAtomic登録し、Envelope公開後に
列挙不能なFileを残さない。Registry登録後にEnvelope公開が失敗した場合や明示Discard後もRegistry Entryは残してよいが、列挙時は
対応するRegular Fileが存在し、Envelope、Project ID、Scene ID、Scene本文を完全検証できるEntryだけを候補として返す。
`list_recovery_candidates`で起動時候補を列挙し、`open_document_from_recovery`は正本Fileの存在を前提にせずEnvelopeから直接
Dirty Documentを開く。Recover後も正本は暗黙作成・置換しない。
Registry本文自体の破損は列挙全体の失敗とするが、個別Entryの読込、Envelope、Identity、Scene検証失敗はそのEntryだけを隔離し、
有効候補とEntry単位診断を同じ列挙結果で返す。Recovery直接Open時に正本Fingerprintを取得できない場合もEnvelopeからのOpenは
継続し、`ExternalChangeState::Unknown`として通常Saveを拒否してSave As、Reload、Cancelの明示判断を要求する。

Registry v1はMagic `CueRecoveryRegistry`、Registry Version、Project ID、Scene ID列を改行区切りで保持する。EntryはScene ID順に
決定的に並べ、重複、未知Version、Project不一致、上限超過を`InvalidRecovery`または`UnsupportedRecovery`として拒否する。

#### Recovery Envelope v1

M12のRecovery本文はSaved Root基準の`Editor/Recovery/<SceneAssetId>.cuerecovery`へ保存する。
Envelope v1は次のUTF-8 Headerを改行区切りで保持し、空行の後へ現行Scene JSONを指定Byte数だけ連結する。

1. Magic `CueRecovery`
2. Recovery Format Version
3. Project ID
4. Scene ID
5. Source Assets Root基準の正本Locator
6. Base Fileの存在Flag
7. Base File Byte Size
8. Base File Content Digest
9. Recovery作成時のDocument State値
10. Scene JSON Content Digest
11. Scene JSON Byte Size

DigestはFirst-partyのFNV-1a 64-bitを用い、File Sizeと組み合わせて偶発的な外部変更と破損を検出する。
これは暗号学的な真正性を保証せず、信頼境界を越える改ざん検出には使用しない。Header、本文Size、Digest、Project ID、Scene ID、
Locator、Scene Parse／Migration／Validationの全検査に成功した候補だけをRecover可能とする。未知Version、欠落Field、上限超過、
Digest不一致は元Fileを変更せず`UnsupportedRecovery`または`InvalidRecovery`として返す。

RecoveryのBase Fingerprintが現在の正本Fingerprintと異なる場合、Recover自体は明示IntentとしてMemoryへ適用するが、Documentを
External Conflict状態にする。これによりRecover後の通常Saveは正本を暗黙上書きせず、Save As、Reload、Cancelの明示判断を要求する。

### Close State Machine

Close要求はCoreの状態遷移として処理し、UI Dialogを状態の正本にしない。

```text
CloseRequested
    | clean and no uncertain state
    v
Closed

CloseRequested
    | dirty / unsaved / conflict / save uncertain
    v
AwaitingDecision --Cancel--> Open
    |--Discard-------------> Closed
    `--Save--> Saving --Committed and clean, no uncertain--> Closed
                    |--Committed but dirty / uncertain-----> AwaitingDecision
                    `--Failure-----------------------------> AwaitingDecision
```

Save完了後は、同期／非同期の実装方式にかかわらず、Closeを確定する直前に`currentStateId == savedStateId`かつ
Save Uncertainが存在しないことを再確認する。保存開始後に編集が進んだ場合やUncertainが残る場合はDocumentを閉じず、
新しい状態に対するSave、Discard、Cancelを選択する`AwaitingDecision`へ戻す。
Lease取得、Fingerprint取得、Serializationを含む保存前の失敗も`SaveRequested`へ留めず、同じ`AwaitingDecision`へ戻す。
Save UncertainのRetryも、失敗または再び不確定な結果になった場合はPending Recordを維持したまま`AwaitingDecision`へ戻す。
Tool UIはSave Uncertainを通常の保存成功として扱わず、Retry／再検証とPending RecordだけのDiscardを明示的に提示する。
Pending RecordのDiscard後もDocumentはDirtyのままとし、Scene切替またはProject終了の判断画面へ戻す。

DiscardはMemory上のEditorDocumentを閉じるだけで、正本FileやRecovery Fileを削除しない。Recovery削除は別の明示Intentとする。

### Project Hub and Editor Process Boundary

Project HubとEditorは別Processとする。Project HubはProject一覧、作成、登録、互換性表示、Editor起動要求を所有し、
Scene編集状態を所有しない。Editor Processは一つの`ProjectWorkspaceSession`を所有する。

Process境界ではC++ Object、STL Container、生Pointer、Native Handleを渡さない。`EditorLaunchRequest`は次を値として渡す。

- 正規化済みProject Descriptor Locator
- 期待する`ProjectId`
- Engine Compatibility識別子
- 任意の初期Scene Locator
- Launch Protocol Version

Editorは起動後に同じ`Cue.Project` ParserでDescriptorを再読込し、Project IDとCompatibilityを再検証する。失敗時は部分Sessionを
公開せず、診断可能なExit Resultを返す。Project HubはProcess終了を監視し、正常終了、起動失敗、異常終了のいずれでも再操作可能に戻る。

M12では複数Editor Processによる同一Project編集、Live IPC、Project Lock Protocolを決定しない。

`Cue.ProjectHub`は`Cue.Foundation`、`Cue.IO`、`Cue.Project`だけへ依存するApplication Moduleとする。
Platform Compositionは、Project Locatorの正規化と合成、RootのOpen、Project ID供給だけを`ProjectHubPlatform`として注入する。
Root OpenはLocatorが存在しない場合を成功したNull Root、Access失敗を`Error`として区別し、ServiceがMissingとBrokenを混同しないようにする。

Project Hub ViewModelはRecent RegistryのIdentity、順序、Pin、Locator状態に、Descriptor表示名とCompatibility結果を合成した
Snapshotとする。Descriptor Parse、Blank Project生成、Compatibility判定、Recent永続化は既存`Cue.Project` APIを正本とし、
ViewModelまたはImGui Adapterへ再実装しない。BrokenはDescriptorを修復せずEntry単位で隔離し、他のProject操作を継続可能にする。
Recentからの除外はWorkspace Registryだけを更新し、Project Folderを削除するAPIをProject Hubへ設けない。

### Presentation Adapter

ImGui AdapterはEditor Coreから取得した不変ViewModelを描画し、User入力をSemantic Intentとして返す。

- Hierarchy Rowは`ObjectId`を保持する
- Inspector Fieldは`ComponentInstanceId`とStable `FieldId`を保持する
- WidgetのText Buffer、展開、FocusはPresentation Stateとする
- Error、Progress、Conflict、Close確認はCoreの状態を表示する
- File Dialog結果は未検証PathとしてControllerへ渡し、CoreがProject Root境界とDescriptorを再検証する

Adapterは`SceneDocument`、`RecentProjectRegistry`、Filesystem、Serializer、`RuntimeWorld`を直接変更しない。
ViewModelはProject／Scene Modelを再実装せず、表示に必要な値と操作可否だけを持つ。

### Error and Lifetime Contract

公開操作は`Result`を返し、失敗理由と対象Identityを診断へ含める。UI表示用日本語TextをCore Errorの正本にせず、
安定Error CategoryとContextからPresentationがMessageを生成する。

`ProjectWorkspaceSession`、`EditorDocument`、History、Recovery Workflowの所有権は一意とし、Global Singletonへ置かない。
公開View、Selection Span、ViewModel Snapshotの有効期間は次のMutationまでとし、長期保持にはStable IDを使用する。

### Headless Test Boundary

次をWindow、ImGui、D3D12 Device、Renderer、RuntimeHostなしで検証可能にする。

- Project Workspace SessionのOpen／失敗Rollback
- RevisionとDirty遷移
- Selection整合
- 全Command ValidationとStrong Failure
- Transaction Commit／Rollback
- Undo／RedoとHistory上限
- Save／Reload／External Conflict／Recovery／Close状態遷移
- Project Hub ServiceとEditor Launch Request生成
- Process終了Resultの解釈

Buildの依存検証で、`Cue.EditorCore`から`Cue.RHI`、D3D12、ImGuiへの依存、`Cue.EngineAssets`／`Cue.Renderer`の
Public伝播、およびRuntime Moduleから`Cue.EditorCore`への逆依存を拒否する。Create Primitive TestはCatalog外ID、
Mesh以外のComponent、Asset Field不一致をTransaction開始前に拒否することを検証する。

## Consequences

### Positive

- UIなしで編集と保存の主要状態遷移を再現できる
- Stable IDとRevisionによりUndo、Selection、Dirtyが同じ規則で整合する
- Transaction途中失敗がSceneへ部分適用されない
- Project HubとEditorの異常終了をProcess境界で隔離できる
- Tool UIのためにRuntime Graphics公開APIを拡張せずに済む
- 将来の別UI、Automation、Command Paletteが同じCoreを利用できる

### Trade-offs

- 完全CheckpointはMemory使用量とCopy Costが大きい
- Core、Workflow、Presentationを分離するため、単純なUI Callbackより型と状態遷移が増える
- Process境界ではProject情報を再検証するため、起動時I/Oが重複する
- Save UncertainとExternal Conflictを区別するため、UIに追加の判断経路が必要になる
- Windows上の非協調Writerに対する既存FileのAtomic Compare-and-PublishはM12で保証しない

### Mitigations

- HistoryをEntry数とByte数で制限し、将来の差分Checkpoint最適化を契約内で許可する
- ViewModelとIntentを小さく保ち、Scene／Project Modelの重複実装を禁止する
- すべての状態遷移をHeadless Testし、Presentation Testを描画とIntent変換へ限定する
- Process Launch ProtocolをVersion付き値Contractにし、ABI共有を避ける

## Rejected Alternatives

### ImGui CallbackからSceneDocumentを直接変更する

実装は短いが、Automation、Headless Test、Undo、失敗Rollback、別Presentationで同じ操作を再利用できないため採用しない。

### RuntimeWorldをEditorの編集正本にする

Runtime EntityはSession-localで、Slot再利用とRuntime Component Layoutが永続編集に適さないため採用しない。

### Dirtyを独立したboolで管理する

Undoで保存地点へ戻る場合、保存中の編集、Redo分岐で正しく復元できないため採用しない。

### CommandへRuntime Entity Pointerを保持する

Undo実行時までPointer寿命を保証できず、Scene再読込とRuntime再生成で無効になるため採用しない。

### Runtime SceneSnapshotをUndo Snapshotとして共用する

Runtime実体化に不要な未知Data、Editor復元情報、保存完全性を保持する契約ではないため採用しない。

### Project HubとEditorを一つのProcess／Global Stateにする

Editor起動失敗や異常終了がHubを巻き込み、Project選択へ戻れないため採用しない。

### Tool UIのためにCue.RHIまたはGame Rendererを拡張する

M12のHierarchy／Inspector／Project Hubは描画Runtime機能を必要とせず、EditorとRuntimeの依存を混在させるため採用しない。

## Implementation Sequence

1. `Cue.EditorCore`にEditorDocument、Revision、Selection、Close状態を実装する
2. Stable IDを対象とするScene編集Commandと完全Checkpointを実装する
3. Transaction型Undo／RedoとHistory上限を実装する
4. Atomic Save、Reload、External Conflict、RecoveryをEditor Workflowへ接続する
5. Project Hub ServiceとViewModelを実装する
6. ImGui Project Hub ShellをPresentation Adapterとして接続する
7. Hierarchy／InspectorをViewModel／Intent／Commandへ接続する
8. Project OpenからEditor再起動／再OpenまでのProcess Workflowを統合する
9. Headless、Process、手動UI WorkflowをM12 Completion Gateで検証する

## Follow-up

- M12 #157から#165で本契約を実装、統合、検証する
- Viewport Rendering、Play Mode、Scriptingは後続MilestoneのResearchを先行する
- 差分CheckpointやCommand Coalescingが必要な場合は、計測結果と復元同値Testを伴う別Issueで判断する
- 複数Editor同時編集、Project Lock、Crash-safe Persistent Undoは別Research Issueで扱う
- 非協調Writerを含む既存FileのCompare-and-Publishが必要な場合は、Windows Native境界をResearch Issueで決定する
