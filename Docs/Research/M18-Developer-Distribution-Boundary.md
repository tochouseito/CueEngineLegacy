# M18 Developer Distribution Boundary Research

- Date: 2026-09-21
- Issue: #365
- Milestone: #19
- Decision: ADR-0030

## Verified Starting Point

| Boundary | Current implementation | M18 gap |
| --- | --- | --- |
| Build definition | Root CMakeと3構成PresetがEngine／ToolをRepository内でBuildする | Install／Export、配布用Source Allowlist、配布Identity |
| Tool binaries | Project Hub、Editor、RuntimeHostをReleaseで生成できる | Version付きBundle、Entry Point検証、導入後の探索 |
| Engine libraries | Configuration別Static LibraryをBuild Treeへ出力する | 公開ABI／Binary SDK契約は未定 |
| Project build | M15～M17のBuild／Package／Shipping WorkflowがRepositoryまたは選択Engine Rootを使用する | Installed Engine Versionの選択と再現可能なSource SDK |
| Player package | ADR-0023／0024がRuntime Package、Monolithic Product、Trustを規定する | Developer Engine配布とのIdentity／Inventory分離 |
| Third-party | vcpkg Manifest、Baseline、Tool Pin、Notice、LicenseをRepositoryで管理する | Noticeを含む配布、Install Tree／Cache除外、初回Restore |
| Product trust | M17は`PublicDistributionReady = false`を明示する | Developer Toolの署名状態、Installer Trust、公開Channel |
| Recovery | Build／PackageはStagingとAtomic Publishを持つ | Version Install、Update、Rollback、Uninstall Journal |

現在のC++ LibraryはCompiler、CRT、Configuration、STL ABIを公開互換として固定していない。
このためM18はBinary SDKを先取りせず、Release Toolと必要なFirst-party Sourceを組み合わせた
`DeveloperSourceSdk`を採用する。Repository Cloneそのものは配布物にしない。

## Selected Flow

```text
clean Repository + pinned commit tree
  -> fixed source/tool/license allowlist
  -> staged DeveloperSourceSdk bundle
  -> canonical manifest + size/hash inventory
  -> bundle validation
  -> per-user install staging
  -> immutable version directory
  -> exclusive-install-lease-aware Release Tool launch probe
  -> durable probe-success marker
  -> installed-version registry
  -> Project Hub version selection
  -> explicit Engine Install Root handoff
  -> external dependency / project build workspace
  -> configuration-matched Editor Play / Shipping build
```

更新はSide-by-side Installで行い、既存Versionを上書きしない。Rollbackは以前のVersionを
再選択する。UninstallはEngine Versionだけを対象とし、Project、Recent Project、Preference、
Source Asset、Build Artifactを削除しない。

## Distribution Inventory

含めるもの:

- Release Project Hub、Editor、Release開発用RuntimeHost、First-party Installer Tool／`CueEngineInstallWorker.exe`
- Project Buildに必要なEngine Source、HLSL、CMake定義、Template
- VC++ Runtime不足時にも診断できるFirst-party静的Bootstrapと`Tools/Dependencies/RestoreVcpkg.ps1`
- `ThirdParty`のvcpkg Manifest／Configuration／Tool Pin、Notice、License Copy
- Engineの利用条件、Version、導入・復旧手順

含めないもの:

- `.git`、`.codex`、Build Tree、CTest出力、Cache、PDB、内部Test／Evidence
- `ThirdParty/.tools`、`ThirdParty/vcpkg_installed`、Download Cache
- Game Project、Source Asset、User Preference、Credential
- Player向けRuntime Package、`CueGameProduct.exe`、追加Game DLL

VC++ Runtimeは存在検査と案内だけをM18に含める。Redistributable Binaryの同梱、MSI／MSIX、
第三者Installer Framework、Code-signing Serviceは承認済み外部依存ではないため導入しない。
未署名Bundleは同一開発者のLocal Fixed Driveから明示許可された入力だけを受け付け、真正性ではなく
偶発破損の検出だけを保証する。

Installed VersionはManifest Inventoryを含めて不変とする。Dependency RestoreのTool／Install Tree、
Project Build Tree、Debug／DevelopmentのRuntimeHostはVersion外のDependency／Project Workspaceへ生成する。
Project Hubは検証済みInstall RootをEditorへ明示的に渡し、Installed ModeでBuild時埋込みのRepository Pathを
使用しない。Bundle Publisherはdirty Repositoryを拒否し、開始時のCommitを固定してAllowlist対象をCommit
TreeからMaterializeする。公開前にHEAD／Worktreeの不変性とStaging ByteのCommit Blob一致を再検証し、
`.git`を含めない代わりにDistribution ManifestのEngine Source Revision、`clean` Source State、Source
Inventory HashをShipping Provenanceとして使用する。Dependency Set IDはCanonical Manifest群と、Target
Triplet、Compiler／Toolset、CRT、Windows SDK、Host／Target Architectureを含むDependency Build Identityの
SHA-256へ固定し、ABIが異なる出力を別Rootへ分離する。ID単位のProcess間LeaseとStaging Publishで共有Rootの
並行Restoreを直列化する。

Install／Update／Rollback／UninstallのOperation JournalはVersion付きCanonical JSONとし、Operation ID、
Kind、単調なStage、Expected Registry Revision、Version／Bundle Identity、Manifest Digest、Worker Identityを
必須化する。未知Schema／Member、不正遷移、旧Worker不一致、破損はFail-closedでEvidenceへ隔離し、Migrationは
専用Issueで明示する。

## Implementation Issues to Create

1. #375 Canonical Identity、Source Revision／Inventoryを持つDistribution Manifest v1とAllowlistを実装する
2. #376 Release Tool／Source SDK／Third-party Noticeを公開し、Dependency出力をVersion外へ分離する
3. #377 Process間Writer排他、Schema v1 Registry、Probe-before-publish、Recoveryを実装する
4. #378 Version外Workerと共通Execution LeaseでUpdate、Rollback、安全な自己Uninstallを実装する
5. #379 Install Root明示引渡し、Configuration一致Host、`.git`非依存ProvenanceをProject Hub／Editorへ接続する
6. #380 VC++ Runtime／Git／Toolchain Prerequisite、License Inventory、配布禁止File監査を実装する
7. #381 M18 Completion GateでProcess E2E、3構成Build／CTest、失敗時保全を検証する

各IssueはSまたはMに限定し、永続ManifestとInstall RegistryのVersionは先行Issueで固定する。
Network Updater、Binary SDK、署名済み公開Installerを同じIssueへ混在させない。

## Risks to Verify

- Update失敗で旧Version、Project、User Dataを変更しない
- Registryだけが先行して未完成Versionを選択可能にしない
- 排他Install Leaseを保持したまま専用Probeが循環待ちせず完了する
- 新Schema Registryを旧Writerが上書きせず、破損RecoveryがProbe成功Markerまで再検証する
- CLIとProject Hubの並行Install／UninstallでRegistry Updateを失わない
- 起動検証とUninstallの間にTOCTOUでVersion Directoryを回収しない
- 対象Version自身のProcessから自己Uninstallして実行中Fileを削除しない
- 異なるEngine VersionのDLL／Library／Third-party Install Treeを一つのProcessへ混在させない
- Debug／Development Game ModuleをRelease RuntimeHostへLoadしない
- Dependency Restore、Project Build、Shipping Provenance生成でImmutable Versionを書き換えない
- 同じDependency Setへの並行RestoreでTool／Install Treeを部分公開しない
- Compiler／Toolset／CRT／Windows SDKが異なるDependency Binaryを同じDependency Rootで再利用しない
- Publisherの事前`clean`確認後に変化したWorktree Byteを記録済みCommitのSnapshotとして公開しない
- 未知または破損したOperation Journalを旧Workerが解釈してRegistryやVersionを変更しない
- Source Allowlistの拡大でTest、Credential、Build出力を配布しない
- Noticeの存在だけでなく、採用Versionと実Payloadが一致することを検証する
- Unsigned Local Bundleを公開署名済みまたはPublic Distribution Readyと表示しない

## Out of Scope

プレイヤー向けInstaller、Store配布、Network Download、Delta Patch、Background Update、
Machine-wide Install、Binary／Plugin SDK、実運用CertificateとPublic Trust Anchor。
