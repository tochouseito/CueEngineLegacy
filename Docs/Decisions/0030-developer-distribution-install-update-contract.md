# ADR-0030: Developer Distribution, Install, Update, and Rollback Contract

- Status: Accepted
- Date: 2026-09-21
- Decision Owners: CueEngine Project
- Relates to: ADR-0019、ADR-0022、ADR-0023、ADR-0024

## Context

M18は、ゲーム製品ではなくCueEngineを利用する開発者向けにEditor、Project Hub、
Engine Source、Build定義、承認済み第三者Dependencyの再現可能な配布と導入を提供する。
現在のRepositoryはCMakeを正本とし、Release Toolを生成できるが、Install／Export定義、
Version付き配布Manifest、更新、Rollback、Uninstallの契約を持たない。

EngineのC++公開境界、静的Library、Compiler／CRT ABIは安定化されていないため、現在の
LibraryをBinary SDKとして配布すると、Compiler、Configuration、Iterator Debug Level、
第三者Library Versionの組合せを互換契約として固定してしまう。一方、Repository全体を
そのまま配布すると、`.git`、Test、Build出力、Cache、内部Evidenceまで製品境界へ混入する。

M17の`ShippingProduct`はプレイヤー向け製品のTrust契約であり、M18の開発者向けEngine
配布とは別物である。M18は実運用Certificate、Online Revocation、外部Trust Anchorを
持たないため、公開署名済み配布を意味しない。

## Decision

### Distribution Kind

- M18は`DeveloperSourceSdk`というVersion付き配布Kindを導入する
- 配布物はRelease構成の`CueProjectHubTool.exe`、`CueEditorTool.exe`、開発支援Toolと、
  ProjectをBuildするためのFirst-party Engine Source／HLSL／CMake定義を含む
- Engineの`.lib`を公開Binary SDKとして契約せず、Project Buildは配布済みSourceを
  選択中ToolchainとConfigurationで再Buildする
- ADR-0022の`Cue.GameModule.Abi` version 1 C互換DLL境界、ABI Header、Version Query、
  Compatibility Metadataは既存の安定接続契約として維持する。延期するのはEngine C++
  `.lib`のBinary SDKとPlugin SDKであり、配布済みDynamic RuntimeHostとSource SDKから
  BuildしたGame Moduleの接続契約はM18でも検証する
- Debug／Development／ReleaseはProject Buildの選択肢として維持するが、配布する固定Tool
  自体はReleaseとする。Game Moduleを動的Loadする`CueRuntimeHost.exe`はModuleとConfigurationを
  一致させる必要があるため、Debug／Development用HostはInstalled Source SDKからProject Build Rootへ
  Buildして使用する。Release Hostへ異なるConfigurationのModuleをLoadしない
- `CueRuntimeHost.exe`は開発用Dynamic実行に必要な場合だけTool Payloadへ含める。
  プレイヤー向け`CueGameProduct.exe`とRuntime PackageはDeveloper Source SDKへ含めない
- Installed CMake Package、`find_package(CueEngine)`、Plugin SDK、安定Binary ABIは、
  明示的なABI VersionとCompatibility Matrixを決める別ADRまで提供しない

### Canonical Bundle Layout

配布Bundleは次の論理Layoutを持つ。生成されたVisual Studio Solution、Build Tree、
vcpkg Install Tree、Source Control Metadataは含めない。

```text
CueEngine-<version>-windows-x64/
  CueEngineDistribution.json
  Bin/
    CueEngineBootstrap.exe
    CueProjectHubTool.exe
    CueEditorTool.exe
    CueRuntimeHost.exe
    CueEngineInstallerTool.exe
    CueEngineInstallWorker.exe
  Engine/
    Source/
    Documents/
  CMake/
  Templates/
  Tools/
    Dependencies/RestoreVcpkg.ps1
  ThirdParty/
    vcpkg.json
    vcpkg-configuration.json
    vcpkg-tool.json
    THIRD_PARTY_NOTICES.md
    Licenses/
  LICENSES/
```

`CueEngineDistribution.json`はVersion付きCanonical JSONとし、Bundle Identity、Engine
Version、Engine Source Revision、`clean`に固定したEngine Source State、Source Inventory Hash、
Dependency Definition ID、Publisher Build Identity、Host OS／Architecture、
最低Toolchain、Entry Point、全Payload FileのRole、Size、SHA-256を記録する。Manifest自身、
署名用予約File、DirectoryはInventoryへ含めない。
未知Role、重複Path、非Canonical Path、Root外参照、未登録File、Size／Hash不一致を拒否する。

Engine Versionは先頭Zeroを持たない`MAJOR.MINOR.PATCH`のCanonical ASCIIに限定し、Bundle IDは
lowercase canonical UUID v4に固定する。Version Directory名は検証済み値から
`v<MAJOR>.<MINOR>.<PATCH>--<uuid>`として生成し、入力文字列をPathへ直接連結しない。生成後のPathを
Canonical化し、`Versions`直下の単一要素であることを再検証する。

Dependency Definition IDはCanonical化したvcpkg Manifest／Configuration／Tool Pinだけから生成する
64文字のlowercase SHA-256 hexとし、Bundleおよび導入先で不変とする。導入先がToolchainを選択した後、
Target Triplet、Host／Target Architecture、Compiler Vendor／Full Version／Toolset、CRT Linkage／Version、
Windows SDK Target VersionをCanonical Dependency Build Identityとして取得し、Definition IDとBuild
IdentityのCanonical結合をSHA-256したDependency Root IDを導出する。外部Rootの完了MarkerはDefinition ID、
Build Identity、Root IDを記録し、一項目でも異なるBinaryを再利用しない。検証前の値をPathへ使用せず、
生成したDirectoryが`Dependencies`直下の単一要素であることをCanonical化後に再検証する。

Source SDK Publisherは開始時にGit HEADを固定し、Tracked／Untracked変更のないWorktreeだけを入力として
受け付ける。Source、HLSL、CMake、Script、Template、Document、LicenseなどSource Control管理Payloadは、
Live Worktreeの後続状態からCopyせず、記録したCommit TreeのBlobをMaterializeしてStagingへ生成する。
各FileのCommit Blob一致を検証し、Source Inventory Hashへ集約する。

`Bin`配下の生成BinaryはCommit Blob検証の対象にしない。同じ固定Commitから隔離されたBuild RootでRelease
TargetをBuildし、Target名、Configuration、Compiler／Toolset、CRT、Windows SDK、Host／Target Architectureを
Publisher Build IdentityとしてManifestへ記録する。生成Binaryは期待Target／PE Architecture／Inventory Roleと
Size／SHA-256を検証し、`builtFromRevision`で固定Commitへ結び付ける。Source管理Payloadと生成Binaryの検証を
同じ規則で代用しない。

Inventory生成後にHEAD、Index、Tracked／Untracked状態を再読込して開始時と変化していれば公開を拒否する。
Repository Rootからの場当たり的な再帰Copyは行わず、検証済みRevision、`clean` Source State、Source
Inventory HashをManifestへ記録する。配布物からProject SourceやUser Dataへ書き戻さない。

### Third-Party and Toolchain

- `ThirdParty/THIRD_PARTY_NOTICES.md`と採用License Copyを必ず配布する
- `Tools/Dependencies/RestoreVcpkg.ps1`をRepositoryと同じ相対Pathで配布Allowlistへ含め、
  `CMake/CueVcpkgToolchain.cmake`の診断が指す明示Restore Entry PointをBundle内で有効にする
- vcpkg Manifest、Registry Baseline、Tool Pinは配布するが、`ThirdParty/.tools`、
  `ThirdParty/vcpkg_installed`、Download Cacheは配布しない
- 初回Buildは明示Dependency Restoreを使用し、取得元、Version、Hash、LicenseをRepositoryと同じ
  Control Planeで検証する。Restore Scriptは`Dependency Root ID`で分離した明示`Tool Root`と
  `Install Root`を必須入力とし、Installed Version配下への出力を拒否する
- vcpkg Tool、Download、Install Treeは`%LOCALAPPDATA%/CueEngine/Dependencies/<dependency-root-id>/`
  配下またはProjectが明示した外部Workspaceへ配置し、Immutable VersionのInventoryを変更しない。
  CMake Toolchainへ`VCPKG_ROOT`と`VCPKG_INSTALLED_DIR`を明示的に渡す
- 共有Dependency Rootの初回RestoreはDependency Root IDごとのProcess間排他Leaseを取得し、
  Operation固有StagingへTool／Install Treeを生成・検証してから同一Volume Renameで公開する。
  公開済みRootはDefinition ID、Build Identity、Root ID、Pinを持つ完了Markerを再検証してImmutable再利用する。
  Configure／Buildは共有Leaseを保持し、Manifest自動Installを無効化して公開Rootへ書き戻さない。異なる
  依存定義またはBuild Identityは新しいRoot IDへ分離する
- 新しいLibrary、Installer Framework、Archive Library、署名ToolをM18の暗黙依存にしない。
  導入が必要なら対象、用途、License、Version、取得元、配布影響を提示してUser承認を得る
- Windows SDK、CMake、MSVC、Git for Windows 2.44.0以上はDeveloper PrerequisiteとしてVersion診断する。
  vcpkg ToolはPin済みRestoreで外部Dependency Rootへ取得する。Toolchain SourceやBinaryをEngine
  Bundleへ複製しない

### Install Root and Ownership

初期実装は管理者権限を要求しないPer-user Installとする。

```text
%LOCALAPPDATA%/CueEngine/
  Versions/v<MAJOR>.<MINOR>.<PATCH>--<uuid>/
  State/InstalledVersions.json
  Operations/
    Workers/<worker-version>/CueEngineInstallWorker.exe
  Dependencies/<dependency-root-id>/
  Logs/
```

- `Versions`配下はInstall完了後にImmutableとする
- Project、Recent Project、Editor Preference、Cache、Build ArtifactはInstall Root外に置き、
  Uninstall対象にしない
- RegistryへMachine-wideな所有権を作らず、初期版は`schemaVersion: 1`、単調増加`revision`、
  Version Entryを持つCanonical JSONで保持する。Readerは対応外Majorと未知MemberをFail-closedで拒否し、
  新しいSchemaを旧Writerで上書きしない。破損時だけVersion Manifest、Payload完了Marker、
  Probe成功Markerの三つが同じBundle／Manifest Digestを示すVersionからRegistry v1を明示Recoveryし、
  元FileをEvidenceとして退避する。Probe成功MarkerがないVersionはSelectableへ復活させず隔離または
  明示再Probeする。意味変更はMigration Issueと新Schemaで行う
- Project HubはInstalled Version RegistryからVersionを列挙し、Project Compatibilityと一致する
  Editor Entry Pointを明示選択する。単一の可変`current` Directoryへ依存しない
- Process起動前に選択VersionのManifestとEntry Point Inventoryを再検証する
- Install RootごとにProcess間Control Lockを一つ、VersionごとにExecution Lease Fileを一つ持つ。
  Registry WriterはControl Lockの排他Lease、起動側は短時間の共有Control Leaseを使用する
- Project Hubは選択Version Rootを`--engine-install-root`とDistribution IdentityでEditorへ渡す。
  EditorとBuild ServiceはManifest検証済みRootから`Engine/Source`、`CMake`、Templateを実行時解決し、
  Build時に埋め込まれたRepository絶対PathをInstalled Modeで使用しない。Build Tree、生成物、vcpkg出力は
  ProjectまたはPer-user Workspaceへ置き、Installed Versionへ書き戻さない

### Install Transaction

`CueEngineInstallerTool.exe`はFirst-partyの薄いCLIとし、同じInstall ServiceをProject Hubからも
利用できるようにする。入力はLocal Bundle Rootと操作種別だけとし、Network Download、Store、
自己更新はM18に含めない。

1. Bundle Manifest、Canonical表現、Inventory、Host／Toolchain互換を読取専用で検証する
2. Operation IDごとのInstall Root内StagingへPayloadをCopyする
3. Stagingの全Fileを再Hashし、Entry PointのPE Architectureを検証する
4. Payload完了Markerを最後に耐久書込みする
5. 同一Volume上のRenameでVersion Directoryを公開する
6. 公開済みVersion DirectoryのRelease Toolを専用Install Probe Modeで起動し、失敗時はRegistryへ追加せず隔離する
7. Probe成功後、Bundle IDとManifest Digestを持つProbe成功Markerを耐久書込みする
8. Probe成功Marker検証後にだけInstalled Version RegistryをAtomic Replaceし、VersionをSelectableにする

失敗時はStagingだけを隔離または削除し、既存VersionとRegistryを変更しない。同じBundle IDの
再実行は内容が一致すれば冪等成功、不一致なら改ざんまたは衝突として拒否する。

専用Install Probe Modeは通常のProject Hub起動経路を使用せず、Installerが保持する排他Control Leaseの
所有下でだけ実行する。InstallerはOperation Journal、Version Identity、Manifest Digestと結び付いた
継承HandleをChildへ渡し、Childは共有Control Leaseや未公開Registry Entryを再取得しない。Probeは
Manifest検証済みVersion Rootの読取とRelease Tool自己診断だけを行い、RegistryやProject状態を変更しない。

Install、Update、Rollback、Uninstall、Registry RecoveryはProcess間Control Lockの排他Leaseを
操作開始から最終Registry Publishまで保持する。Lease取得後にRegistryを再読込し、単調増加する
Registry Revisionと期待Revisionを照合してから変更する。別Processが更新済みなら古いSnapshotを
上書きせず再試行またはConflict Errorとする。Abandoned WriterはOperation JournalとVersion Directoryを
再検証してからRecoveryする。Atomic ReplaceだけをProcess間排他の代用にしない。

Operation Journalは`schemaVersion: 1`のCanonical JSONとし、Operation ID、Operation Kind、単調なStage、
Expected Registry Revision、対象Version／Bundle Identity、Manifest Digest、Worker Identityを必須Memberとして
記録する。Reader／Workerは対応外Schema、未知Member、欠落Member、非Canonical表現、後退または不正なStage
遷移をFail-closedで拒否し、旧Writerが新Schemaを上書きしない。各Stageは耐久書込みとAtomic Replace後にだけ
進め、Workerは自身が対応するSchemaとOperation Kindだけを再開する。破損または非互換JournalはEvidenceとして
Quarantineし、Payload／Registryを推測で変更しない。意味変更と移行は専用Issueで新Schemaと明示Migrationを
定義し、暗黙Upgradeしない。

Journal v1のStageは「最後に完了した耐久副作用」を表し、次の表以外の値と遷移を許可しない。

| Operation Kind | 許可する単調Stage遷移 | Stageが証明する耐久副作用 |
| --- | --- | --- |
| `install`／`update` | `prepared` → `payloadStaged` → `versionPublished` → `probeSucceeded` → `registryPublished` → `completed` | Journal作成 → Staging完了Marker → Version Rename → Probe成功Marker → Selectable Registry Publish → Staging／Journal Cleanup完了 |
| `rollback` | `prepared` → `selectionPublished` → `completed` | Journal作成 → 既存Version選択のRegistry Publish → Journal Cleanup完了 |
| `uninstall` | `prepared` → `removalBlocked` → `versionQuarantined` → `registryEntryRemoved` → `completed` | Journal作成 → `pendingRemoval` Registry Publish → Version Quarantine Rename → Registry Entry削除Publish → Quarantine削除とCleanup完了 |
| `registryRecovery` | `prepared` → `candidatesValidated` → `registryPublished` → `completed` | Journal作成 → Manifest／Payload／Probe Marker候補検証 → Registry再構築Publish → Journal Cleanup完了 |

Writerは副作用を耐久化して再読込検証した後だけ次StageをAtomic Replaceする。副作用後かつStage更新前にCrashした
場合、Recoveryは現在Stageの直後に期待されるFile／Marker／RegistryだけをOperation IdentityとDigestで照合し、
完全一致時だけ同じ副作用を冪等完了してStageを進める。欠落、別Identity、想定外の先行副作用、複数候補、
Stage後退を検出した場合は再開もRollbackも推測せず、Journalと対象をEvidenceへ隔離して明示診断する。

### Update, Rollback, and Uninstall

- Updateは既存Versionへの上書きPatchではなく、新しいImmutable VersionのSide-by-side Installとする
- 新Versionは検証と起動Probeの成功後に選択可能にし、旧Versionを自動削除しない
- RollbackはProject Hubで以前のInstalled Versionを再選択する操作であり、Payloadを逆Patchしない
- Editor／Tool起動は共有Control Leaseを取得し、RegistryとManifestを検証した後、対象Versionの
  共有Execution Leaseを取得する。取得後にRegistryを再確認してからControl Leaseを解放し、
  Execution Lease HandleをChild Processへ継承してProcess終了まで保持する
- Uninstallは排他Control Leaseのもとで対象Versionを新規起動不可にし、同じVersionの排他Execution
  Leaseを取得できた場合だけDirectoryを回収する。既存の共有LeaseがあればBusyとして回収しない
- 自分自身を含むVersionのUninstallは対象Version内のProcessから直接削除しない。Install時にInventory検証して
  Distribution Manifestの`installWorker` Roleから`Operations/Workers`へHash検証後に配置したVersion外の
  `CueEngineInstallWorker.exe`へOperation Journalと起動Process Handleを渡し、
  起動側が終了して共有Leaseを解放した後にWorkerが排他Control／Execution Leaseを取得する。WorkerはRegistryを
  `pendingRemoval`へAtomic Publishして新規起動を止め、Versionを同一VolumeのQuarantineへRenameし、Registryから
  Entryを削除する。各段階をJournalからRollbackまたは再開できる場合だけQuarantineを最終削除する
- 最後の互換Version、使用中Version、未完了Operationを無確認で削除しない
- Project、Source Asset、Recent Registry、Editor Preference、Build／Package成果物は削除しない
- Crash後はOperation Journalと完了Markerから、未公開Stagingの回収またはRegistry再構築を行う

Network Channel、Delta Patch、Background Updater、強制更新、Telemetryは別Milestoneまで導入しない。

### VC++ Runtime and Signing

- 現在のEngine ToolとShipping ProductはMSVC Dynamic Runtimeを前提とする。M18 Installerは
  必要なVC++ Runtimeの存在とArchitectureを検査し、不足時は診断可能なErrorで停止する
- Bundleの最初のEntry PointはEngine LibraryへLinkしないFirst-party `CueEngineBootstrap.exe`とし、
  `/MT`で自己完結させる。BootstrapはHost Architecture、VC++ Runtime、InstallerのInventoryを検査し、
  Runtimeが利用可能な場合だけ`CueEngineInstallerTool.exe`を起動する。BootstrapはCRT Security更新時に
  再Build／再配布する。通常のEditor、Project Hub、RuntimeHost、Shipping Productは`/MD`を維持する
- Microsoft VC++ Redistributable BinaryをRepositoryまたはBundleへ同梱しない。将来同梱する場合は、
  正確なVersion、Microsoftの再配布条件、取得元、署名、Silent Install、Reboot、更新責任を提示し、
  User承認を得る
- Authenticode未署名BundleとToolは`LocalDeveloperOnly`として扱う
- `LocalDeveloperOnly`のSize／SHA-256は偶発破損とOperation整合性だけを検出し、Publisher真正性を
  保証しない。受付対象は同一開発者が管理するLocal Fixed Drive上で明示選択したBundleに限定する
- CLIは`--allow-unsigned-local`の明示指定、Project Hubは同等の確認なしに未署名BundleをInstallしない。
  UNC／Remote Drive、Mark-of-the-WebがInternet／Restricted ZoneのBundle、自動Download結果は拒否する
- `CueEngineBootstrap.exe`も未署名Bundleの一部であるため信頼起点ではない。M18はNetwork配布や第三者から
  受領したBundleを安全にする機能を提供しない
- Test用Self-signed CertificateやBuild時Hashだけで`PublicDistributionReady`へ昇格しない
- 公開Channelには実運用Certificate、Timestamp、Online Revocation、署名済みManifest、
  許可Publisherを強制する外部Trust Anchor、署名済みInstallerの実機検証が必要である

### Failure and Compatibility Policy

- Engine Version、Bundle ID、Manifest Version、Host ArchitectureをInstall Identityに含める
- Manifest Readerは対応Major以外を拒否し、未知Memberを黙って破棄しない
- 新EngineでProject変換が必要な場合、旧Versionを保持したまま明示Migrationを行う。
  Install／UpdateがProject Dataを自動変更しない
- Installer、Project Hub、Editorを同じVersion Directoryから起動し、異なるBundleのLibraryや
  Third-party DLLを検索Pathから混在させない
- Source Control Metadataを含まないInstalled SDKからShipping ProductをBuildする場合、検証済みDistribution
  ManifestのEngine Source Revision、`clean` Source State、Source Inventory HashをProvenanceとして使用する。
  Repository Modeは従来どおりGit HEAD／Dirty Stateを検証し、Installed Modeは`.git`を要求せず、
  `clean`以外のSource StateまたはManifest Inventory不一致を拒否する
- Editor Playは選択Configurationと同じGame Module、RuntimeHost、Engine Buildを同じ外部Build Rootから使う。
  固定Release Tool PayloadはDebug／Development ModuleのHostとして代用しない
- LogにはSecret、User Source内容、Credentialを記録せず、Operation ID、Path分類、Error Code、
  検証段階を記録する

## Alternatives

| Option | 利点 | 代償 | Decision |
| --- | --- | --- | --- |
| Source SDK + Release Tools | 現在のABIを固定せずProject Buildを再現できる | Build ToolchainとDependency Restoreが必要 | 採用 |
| Configuration別Binary SDK | 導入後のBuildが速い | Compiler／CRT／STL ABI、Symbol、Patch互換を今決める必要がある | M18では不採用 |
| Repository Cloneを配布 | 実装が少ない | Git履歴、Test、Cache、内部Fileが製品境界へ混入する | 不採用 |
| Version Directoryを上書き更新 | Disk使用量が少ない | Crash時に旧成功状態を失いRollbackできない | 不採用 |
| MSI／MSIX／第三者Installer Framework | OS統合が強い | 新しい外部Tool、署名、Machine-wide状態、更新Policyが必要 | User承認を伴う後続候補 |
| vcpkg Install Treeを同梱 | 初回Buildが速い | Toolchain／ABI固有BinaryとLicense Inventoryが肥大化する | 不採用 |

## Verification

- Allowlist外File、Path Traversal、重複、欠落、Size／Hash、非Canonical Manifestを拒否する
- Dirty Repository、Publisher実行中のHEAD／Worktree変更、Commit Blobと不一致なSource管理SnapshotからのBundle生成を拒否する
- 生成BinaryのSource Revision／Publisher Build Identity／Target／PE Architecture／Inventory不一致を拒否する
- 非Canonical Dependency Definition／Root ID、Toolchain／Compiler／CRT Build Identity不一致、Dependencies Root外Pathを拒否する
- Staging失敗、Copy失敗、Hash不一致、Registry Publish失敗で旧Versionを維持する
- Install、同一Bundle再実行、Side-by-side Update、Rollback、Uninstall、Crash RecoveryをProcess Testする
- 排他Control Lease中の専用Probeが完了し、通常起動経路が同じ状態では待機することをProcess Testする
- Probe前Crashから未Probe VersionをSelectableへ復活させず、Probe成功MarkerだけをRecovery対象にする
- Operation Journalの未知Schema／Member、Kind別v1列挙外Stage／遷移、旧Worker互換性違反、破損をFail-closedで拒否する
- 各Journal Stage間へCrashを注入し、完全一致する直後の副作用だけを冪等再開して想定外状態を隔離する
- 同一Dependency Root IDの並行Restoreを直列化し、失敗Stagingと公開済みImmutable Rootを混在させない
- Project／User Data／Recent RegistryがUpdateとUninstallで不変であることを確認する
- Release Tool起動、異なるWorking Directory、Unicode／Long Pathを確認する
- VC++ Runtime不足、Toolchain不一致、Architecture不一致を診断する
- Git for Windows不足／Version不一致、Dependency出力先がImmutable Version配下の場合を診断する
- Installed RootからDebug／Development／ReleaseそれぞれのConfiguration一致HostをBuild・起動する
- `.git`なしInstalled RootからManifest由来Provenanceを持つShipping ProductをBuildする
- Distribution InventoryにBuild Tree、`.git`、PDB、Test、vcpkg Install Tree、Credential、
  Player Productが混入しないことを確認する
- Third-party NoticeとLicense、Manifest Pinが存在し、Inventoryに登録されることを確認する
- Debug／Development／Release Build、全CTest、`git diff --check`を実行する

## Deferred

安定Binary SDK、Plugin SDK、Network Download、Delta Update、Background Updater、Store配布、
Machine-wide Install、File Association、実運用Code Signing、Public Trust Anchor、Telemetry。
