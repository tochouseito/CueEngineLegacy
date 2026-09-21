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
- Debug／Development／ReleaseはProject Buildの選択肢として維持するが、配布するTool
  自体はReleaseとする
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
Version、Host OS／Architecture、最低Toolchain、Entry Point、全Payload FileのRole、
Size、SHA-256を記録する。Manifest自身、署名用予約File、DirectoryはInventoryへ含めない。
未知Role、重複Path、非Canonical Path、Root外参照、未登録File、Size／Hash不一致を拒否する。

Source SDK Publisherは固定Allowlistから不変SnapshotをStagingへCopyし、全Inventoryを
検証した後だけBundleを公開する。Repository Rootからの場当たり的な再帰Copyは行わない。
配布物からProject SourceやUser Dataへ書き戻さない。

### Third-Party and Toolchain

- `ThirdParty/THIRD_PARTY_NOTICES.md`と採用License Copyを必ず配布する
- `Tools/Dependencies/RestoreVcpkg.ps1`をRepositoryと同じ相対Pathで配布Allowlistへ含め、
  `CMake/CueVcpkgToolchain.cmake`の診断が指す明示Restore Entry PointをBundle内で有効にする
- vcpkg Manifest、Registry Baseline、Tool Pinは配布するが、`ThirdParty/.tools`、
  `ThirdParty/vcpkg_installed`、Download Cacheは配布しない
- 初回Buildは既存の明示Dependency Restoreを使用し、取得元、Version、Hash、Licenseを
  Repositoryと同じControl Planeで検証する
- 新しいLibrary、Installer Framework、Archive Library、署名ToolをM18の暗黙依存にしない。
  導入が必要なら対象、用途、License、Version、取得元、配布影響を提示してUser承認を得る
- Windows SDK、CMake、MSVC、vcpkg ToolはDeveloper PrerequisiteとしてVersion診断する。
  Toolchain SourceやBinaryをEngine Bundleへ複製しない

### Install Root and Ownership

初期実装は管理者権限を要求しないPer-user Installとする。

```text
%LOCALAPPDATA%/CueEngine/
  Versions/<engine-version>-<bundle-id>/
  State/InstalledVersions.json
  Operations/
  Logs/
```

- `Versions`配下はInstall完了後にImmutableとする
- Project、Recent Project、Editor Preference、Cache、Build ArtifactはInstall Root外に置き、
  Uninstall対象にしない
- RegistryへMachine-wideな所有権を作らず、初期版はVersion RegistryをCanonical JSONで保持する
- Project HubはInstalled Version RegistryからVersionを列挙し、Project Compatibilityと一致する
  Editor Entry Pointを明示選択する。単一の可変`current` Directoryへ依存しない
- Process起動前に選択VersionのManifestとEntry Point Inventoryを再検証する
- Install RootごとにProcess間Control Lockを一つ、VersionごとにExecution Lease Fileを一つ持つ。
  Registry WriterはControl Lockの排他Lease、起動側は短時間の共有Control Leaseを使用する

### Install Transaction

`CueEngineInstallerTool.exe`はFirst-partyの薄いCLIとし、同じInstall ServiceをProject Hubからも
利用できるようにする。入力はLocal Bundle Rootと操作種別だけとし、Network Download、Store、
自己更新はM18に含めない。

1. Bundle Manifest、Canonical表現、Inventory、Host／Toolchain互換を読取専用で検証する
2. Operation IDごとのInstall Root内StagingへPayloadをCopyする
3. Stagingの全Fileを再Hashし、Entry PointのPE Architectureを検証する
4. 完了Markerを最後に耐久書込みする
5. 同一Volume上のRenameでVersion Directoryを公開する
6. Installed Version RegistryをAtomic Replaceする
7. Release Toolの起動Probeが成功したVersionだけをSelectableにする

失敗時はStagingだけを隔離または削除し、既存VersionとRegistryを変更しない。同じBundle IDの
再実行は内容が一致すれば冪等成功、不一致なら改ざんまたは衝突として拒否する。

Install、Update、Rollback、Uninstall、Registry RecoveryはProcess間Control Lockの排他Leaseを
操作開始から最終Registry Publishまで保持する。Lease取得後にRegistryを再読込し、単調増加する
Registry Revisionと期待Revisionを照合してから変更する。別Processが更新済みなら古いSnapshotを
上書きせず再試行またはConflict Errorとする。Abandoned WriterはOperation JournalとVersion Directoryを
再検証してからRecoveryする。Atomic ReplaceだけをProcess間排他の代用にしない。

### Update, Rollback, and Uninstall

- Updateは既存Versionへの上書きPatchではなく、新しいImmutable VersionのSide-by-side Installとする
- 新Versionは検証と起動Probeの成功後に選択可能にし、旧Versionを自動削除しない
- RollbackはProject Hubで以前のInstalled Versionを再選択する操作であり、Payloadを逆Patchしない
- Editor／Tool起動は共有Control Leaseを取得し、RegistryとManifestを検証した後、対象Versionの
  共有Execution Leaseを取得する。取得後にRegistryを再確認してからControl Leaseを解放し、
  Execution Lease HandleをChild Processへ継承してProcess終了まで保持する
- Uninstallは排他Control Leaseのもとで対象Versionを新規起動不可にし、同じVersionの排他Execution
  Leaseを取得できた場合だけDirectoryを回収する。既存の共有LeaseがあればBusyとして回収しない
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
- Staging失敗、Copy失敗、Hash不一致、Registry Publish失敗で旧Versionを維持する
- Install、同一Bundle再実行、Side-by-side Update、Rollback、Uninstall、Crash RecoveryをProcess Testする
- Project／User Data／Recent RegistryがUpdateとUninstallで不変であることを確認する
- Release Tool起動、異なるWorking Directory、Unicode／Long Pathを確認する
- VC++ Runtime不足、Toolchain不一致、Architecture不一致を診断する
- Distribution InventoryにBuild Tree、`.git`、PDB、Test、vcpkg Install Tree、Credential、
  Player Productが混入しないことを確認する
- Third-party NoticeとLicense、Manifest Pinが存在し、Inventoryに登録されることを確認する
- Debug／Development／Release Build、全CTest、`git diff --check`を実行する

## Deferred

安定Binary SDK、Plugin SDK、Network Download、Delta Update、Background Updater、Store配布、
Machine-wide Install、File Association、実運用Code Signing、Public Trust Anchor、Telemetry。
