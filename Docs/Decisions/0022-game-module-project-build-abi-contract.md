# ADR-0022: Game Module, Project Build, and ABI Contract

- Status: Accepted
- Date: 2026-09-08
- Decision Owners: CueEngine Project

## Context

M15では、Project固有のC++ SourceをCMakeでBuildし、再利用可能なGame Module Artifactとして扱う。
EditorはBuildを開始、Cancel、再実行し、LogとArtifactを表示するが、Build ToolやNative Processを直接操作しない。

ADR-0003はC++20、MSVC、Debug／Development／ReleaseのBuild構成を決定した。
ADR-0013はProject共有Data、Machine固有Workspace、Generated、Savedを分離した。
ADR-0015はCompiler型名と登録順から独立したSchema Identityを決定した。
ADR-0021はProject Scopeが不変System Factory集合を持ち、各Runtime Application SessionがSystem Instanceを所有することを決定した。

現行の`Cue.RuntimeHost`はEngine Source Tree内で構築される単一Executableであり、Project固有Sourceを入力に持たない。
現行ECSのComponent Storageは`World::register_component<T>`でC++型へCompile時にBindingされる。
この状態でProject SourceをEngine Targetへ直接追加すると、ProjectごとにEngine Hostを再Linkし、Engine SourceとGame Sourceの
責務、生成物、失敗時の保全境界が混在する。一方、C++ Class、STL Container、Exception、所有PointerをそのままDLLへ公開すると、
Compiler、Runtime Library、Configuration、Allocation Ownerの差が未定義動作または互換性事故になる。

本ADRは、Game Module Target、Engineとの依存方向、最小ABI、登録順とLifetime、Project SourceとBuild Outputの配置、
3構成の互換性、Editor Build Serviceの責務、失敗Artifactの扱いを決定する。
Game Moduleの実装、Hot Reload、Scripting、Asset Build／Cook、Runtime Packaging、ECS Storage改良は決定しない。

## Prior and Legacy Reference

### Current Rebuild Contracts

- CMakeを唯一の正式Build定義とし、生成されたIDE Projectを正本にしない
- RuntimeはEditor、ImGui、Platform固有型へ依存しない
- `SchemaRegistryBuilder`はCoreとProjectの定義を集約した後に一度だけSealする
- `RuntimeSystemRegistry`はSystem ID、Phase、Order、Dependency、登録順から実行順を固定する
- 一つのRuntime Application SessionがSystem Instanceを一意所有し、Startの逆順でStopする
- Project ScopeのFactoryはSession-local Mutable Stateを所有しない
- Project Root外のPathを暗黙に読書きせず、GeneratedとSavedを共有Sourceから区別する

本ADRはこれらを置換せず、Project DLLをProject Scopeへ接続する境界を追加する。

### Legacy CueEngine

旧CueEngineはScript DLLをBuildし、C ABIと関数Pointer Tableを介してEngine機能へ接続することで、
Engine本体を再BuildせずProject Codeを読込む問題を解いていた。DLL内で生成したObjectをDLL側関数で破棄し、
ABI Versionを検査する考え方も持っていた。

一方、ABIは多くのComponent、Object、Gameplay操作を一つのVersionへ追加し続け、変更影響と互換性検証の範囲が大きくなった。
Editor Build、Staging、PDB、Reload、Runtime APIが近接し、通常BuildとHot Reloadの所有境界も理解しにくかった。

新CueEngineでは単一Exportと明示Version、C互換値、同一Module内の生成／破棄という原則だけを現在要件から再設計する。
旧ABIの型、関数、Version値、Data Layout、Loader、Build Scriptはコピー、移植、改名、部分抽出しない。

検証は新規HeaderのC／C++単体Compile、誤Version／Configuration／Architectureの拒否、登録順、
SessionごとのInstance分離、Load／Unload順、Artifact保全Testで行う。

### TheatriaEngine

TheatriaEngineはTarget種別をBuild定義から生成し、DLLを別名へStagingしてNative Loaderで読込む短いIteration経路を持つ。
ただし、Machine固有PathをBuild定義へ含める構成と、通常起動、Editor状態、Hot Reloadを同時に扱うLoaderは、
再現可能なProject Buildと最小M15 Scopeには適さない。

CMakeからTargetを構成する点だけを参考にし、Generator、Loader、Path、Reload実装は使用しない。

## Current Requirements

- Project固有C++ SourceをEngine Sourceと物理的、論理的に分離する
- Projectを一つのGame Module DLLへBuildし、M16の汎用RuntimeHostから接続可能にする
- CMakeを唯一の正式Build定義とし、Visual Studio Solutionを生成物として扱う
- Game ModuleからEditor、ImGui、Project Hub、Platform Windows、RHI、D3D12へ依存しない
- RuntimeからGame Module実装へCompile時依存しない
- DLL境界にSTL、C++ Virtual Interface、Exception、所有Pointer、Allocatorを公開しない
- Schema、Component宣言、Systemの登録入口と順序を固定する
- Debug／Development／Release、Architecture、ABI Versionの不一致をLoad前または登録前に拒否する
- Project Display NameをTarget名、File名、Export Symbol、C++ Identifierの正本にしない
- Build失敗、Cancel、Validation失敗で以前の成功Artifactを破壊しない
- M15でHot Reload、Asset Build、Runtime Packaging、任意Project Component Storageを導入しない

## Reference Comparison

| Reference | 参考にする点 | CueEngineで採用しない点 |
| --- | --- | --- |
| Unreal Engine | ModuleごとのSource、Public／Private境界、Build RuleからのTarget構成、Monolithic／Modularの選択 | `UObject`、Reflection、Build.cs、Module API Macro、Engine全体のModule Graphを移植しない |
| Unity | Native Plug-inを単純なC InterfaceとC linkageで接続する原則 | Managed Runtime、P/Invoke、Unity固有Rendering Plug-in Eventを導入しない |
| Godot | 単一Entry Symbol、C Interface、Version付きAPI、Configuration／Platform別Library指定 | GDExtension API、Variant、Object Binding、`.gdextension`形式を移植しない |
| SOL-AVES | Global相当とWorld相当のLifetimeを分け、依存と破棄順を明示する考え方 | Service Container、型Lookup、Job System、Updater Graph、ECS変更をM15へ導入しない |
| Legacy CueEngine | Version検査、C ABI、DLL側生成物をDLL側で破棄する原則 | 肥大化したGameplay ABI、Hot Reload、旧型、旧Loader、旧Build Scriptを使用しない |
| TheatriaEngine | Project Codeを独立TargetとしてBuildする短い導線 | Machine固有Path、通常BuildとHot Reloadの混在、既存Source生成実装を使用しない |

References:

- [Unreal Engine Modules](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-modules)
- [Unreal Build Tool](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-build-tool-in-unreal-engine)
- [Unreal Engine Module API Specifiers](https://dev.epicgames.com/documentation/en-us/unreal-engine/module-api-specifiers-in-unreal-engine)
- [Unity Native plug-ins](https://docs.unity3d.com/2023.2/Documentation/Manual/NativePlugins.html)
- [Godot What is GDExtension?](https://docs.godotengine.org/en/stable/engine_details/engine_api/gdextension/what_is_gdextension.html)
- [Godot `.gdextension` file](https://docs.godotengine.org/en/stable/engine_details/engine_api/gdextension/gdextension_file.html)
- [Godot GDExtension interface JSON](https://docs.godotengine.org/en/stable/engine_details/engine_api/gdextension/gdextension_interface_json_file.html)
- [Microsoft C Run-Time Library selection](https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library)
- [Microsoft Linker Tools Error LNK2038](https://learn.microsoft.com/en-us/cpp/error-messages/tool-errors/linker-tools-error-lnk2038)
- [CEDiL: 「SOL-AVES」の高性能なランタイムを構成するアーキテクチャ](https://cedil.cesa.or.jp/cedil_sessions/view/3297)
- [TheatriaEngine](https://github.com/tochouseito/TheatriaEngine)

参考EngineはModule化とIterationを改善する一方、DLL数、Export管理、互換性検証、Tooling Complexityを増やす。
CueEngineはM16のStandalone Packageが必要とする一つのProject Moduleだけを導入し、汎用Plugin Systemへ拡張しない。

## Decision

### Delivery Model

Project固有CodeはWindows x64のShared Library Target `CueGameModule`としてBuildする。
Artifact名は固定の`CueGameModule.dll`とし、Project Display NameをFile名またはC++ Symbolへ変換しない。
PDB等の付随ArtifactはConfigurationとToolchainが生成する別FileとしてInventoryへ記録する。

EngineはProjectごとに`Cue.RuntimeHost`を再Compileしない。M15はGame ModuleをBuild Artifactとして生成、検証、記録するまでとし、
M16のPackage Publisherが汎用`CueRuntimeHost.exe`、Game Module、Runtime Data、Manifestを一つのPackageへ配置する。
M16のRuntimeHostはManifestに列挙されたGame ModuleだけをProcess開始時に読込み、停止完了後にUnloadする。

M15とM16は一Project、一Game Module、一RuntimeHost Processを基本形とする。複数Game DLL、Editor Plugin、Runtime Plugin、
動的追加、Hot Reload、同一Process内のProject切替は導入しない。

Static Linkだけを正式経路にしない。Static LinkではProjectごとにRuntimeHost Executableを再Link、複製する必要があり、
M16の共通Host、Package Manifest、Game Module Artifactという境界を維持できないためである。

### Target and Dependency Direction

依存方向は次のとおりとする。

```text
Project Source
  `-- CueGameModule.dll
        `-- Cue.GameModule.Abi headers

CueRuntimeHost.exe
  |-- Cue.GameModule.Loader.Windows
  |-- Cue.GameModule.Adapter
  `-- Cue.Runtime -> Cue.GameCore / Cue.Scene / Cue.Schema / Cue.Input / Cue.Foundation

Cue.Editor.Tool
  `-- Cue.Build -> Cue.Platform.Process.Windows / Cue.Project / Cue.Foundation
```

`Cue.GameModule.Abi`はC互換Headerだけを公開し、EngineのC++ LibraryへLinkしない。
Game Moduleは`Cue.Runtime`、`Cue.GameCore`、`Cue.Schema`のC++ HeaderまたはBinaryへ直接依存しない。
Host側のFirst-party AdapterだけがC ABI値を既存の`TypeDescriptor`、`RuntimeSystemDescriptor`、
`RuntimeSystem`へ変換する。Runtime ModuleはLoader、DLL Handle、Editor Build Serviceを参照しない。

Windows DLL読込みはPlatform Adapterへ置く。`HMODULE`、`LoadLibraryW`、`GetProcAddress`、Native Error Codeを
Portable ABI Header、Runtime、Editor Coreへ公開しない。

### Project Source and Build Layout

Project Root内の役割を次のように定める。

| Path | Role | Source of Truth | Overwrite Policy |
| --- | --- | --- | --- |
| `Source/Game` | User所有のFirst-party Game Source | Yes | 既存FileをGeneratorが上書きしない |
| `Source/Game/CMakeLists.txt` | Game Module Target定義 | Yes | 初回生成後はUser所有とする |
| `CMakeLists.txt` | Project BuildのRoot定義 | Yes | 初回生成後はUser所有とする |
| `CMakePresets.json` | 3構成の共有Preset定義 | Yes | Schemaを検証し、既存Fileを上書きしない |
| `Generated/Build/<workspace-key>` | CMake Binary Tree、生成IDE Project、Compiler中間物 | No | 互換なToolchain入力では再利用する |
| `Generated/Build/Locks/<workspace-key>.lock` | Binary TreeのProcess間Exclusive Build Lock | No | Binary Tree Cleanup対象に含めない |
| `Generated/Build/Candidates/<operation-id>` | 成功Processから収集した未公開Artifact | No | 検証後にPublishまたは破棄できる |
| `Saved/Build/Operations/<operation-id>` | Build Log、Plan、Environment、Result Snapshot | No | 診断Retention Policyで管理する |
| `Generated/Artifacts/<configuration>/Versions/<artifact-id>` | M16 Publisher入力となる不変の成功Artifact集合 | No | 一意Directoryとして一度だけ公開する |
| `Generated/Artifacts/<configuration>/Current.json` | 現在の成功Artifact IDとInventoryを指す小さいManifest | No | 新しい成功Versionの検証後だけAtomic Replaceする |

`Assets/Source`はAsset Authoring用であり、C++ Sourceを置かない。`Assets/Runtime`はAsset Pipeline出力用であり、
Game DLL、PDB、Build Logを置かない。Machine固有Engine Source／Binary LocationはCMake引数またはUser Workspace設定から渡し、
共有`CMakeLists.txt`、`CMakePresets.json`、`CueProject.json`へ絶対Pathを書込まない。

`workspace-key`はGenerator、Architecture、Toolset、Engine Build Policyの互換入力から決定的に作る。
同じKeyのCMake Binary TreeはIncremental Buildへ再利用し、入力が変わった場合は別KeyへConfigureする。
Operation IDをBinary TreeのIdentityにしないため、通常の再Buildで全Objectを毎回作り直さない。

M15の初期Keyは`windows-vs2026-x64-msvc-<major>.<minor>.<patch>.<build>-policy-<version>-<configuration>`とする。
`<version>`はGame Module ABI、C++ Language Level、Runtime Library等をまとめたEngine Build PolicyのVersionであり、初期値は`1`とする。
Configurationは`debug`、`development`、`release`のいずれかとする。CompilerのMachine固有PathやProject RootはKeyへ含めず、
同一Versionの互換Toolset Installation間でBinary Treeを再利用する。Keyが変わる場合は`Generated/Build/<workspace-key>`も必ず変え、
異なるLockで同じBinary Treeを保護する状態を作らない。

Project Generatorが共有`CMakePresets.json`へ記録する`binaryDir`はCommand Lineから直接Presetを使う場合の既定値であり、
Editor Build ServiceのBinary Tree正本にはしない。CMake RunnerはConfigure時に`--preset <preset> -B <plan-binary-directory>`を渡して
PresetのGenerator／Cache設定を使いながら出力先を必ずPlanへ一致させ、Build時はBuild Presetを使わず
`--build <plan-binary-directory> --config <configuration> --target CueGameModule`を使用する。これによりLock、Incremental Build、
Candidate収集が参照するTreeを一つに固定する。

同じ`workspace-key`を使用するConfigure、Build、Binary Tree Cleanupは、対応するLock FileのOffset `0`、Length `1`へ
`LockFileEx`のExclusive Lockを取得し、全Process、全Build Service Instanceで直列化する。Build OperationはConfigure開始前に取得し、
Build Process終了後、そのOperation固有Candidate Directoryへの全Artifact Copy、Handle Close、Size／Hash取得が完了するまで保持する。
同じKeyの別OperationはLock取得をCancel／Timeout可能な待機として扱い、Lockなしで共有Binary Treeを使用しない。異なるKeyは並行できる。
Process CrashではOSのHandle CloseによりLockを解放するが、次のOperationは残存Binary Treeを成功済みと仮定せずConfigureから再検証する。
Binary Tree Cleanupも同じLockを取得できなければ実行しない。

### Build Profile Wire Format

Build ProfileはEditor Workspaceが再利用するUser選択であり、UTF-8 JSONのVersion付き永続形式とする。初期Schemaは次の3 Memberを
それぞれちょうど一つ要求し、Member順には依存しない。

```json
{
  "schemaVersion": 1,
  "configuration": "Development",
  "target": "GameModule"
}
```

`schemaVersion`はJSON整数`1`、`configuration`は`Debug`、`Development`、`Release`のいずれか、`target`は
`GameModule`だけを受理する。欠落、重複、未知Member、型不一致、未知列挙値、未知または新しいSchema Versionは拒否し、推測して読まない。
Build ProfileはBuild ArtifactやProject Sourceではなく再生成可能なUser設定であるため、v1では自動Migrationを行わない。読込み拒否時は
既定値へ暗黙Fallbackせず、UIが再選択と明示保存を促す。将来Versionを追加する場合は、旧VersionをMigrationするか再生成させるかを
先行ADRで決め、Writer／Reader／拒否経路のTestを同時に更新する。

Exclusive Build LeaseはCandidate Snapshot確定後に解放し、その後でCandidate検証とArtifact StoreのExclusive Mutation Leaseを取得する。
Build LeaseとArtifact Leaseを同時保持せず、全First-party入口でこの順序を固定してProcess間Deadlockを避ける。

Generatorは空Project生成時に全共有SourceとBuild定義をStaging Rootへ構築し、検証後だけProject Rootとして公開する。
既存Projectへの不足File追加はCreate-onlyとし、File単位の存在と内容Hashを検査してから実行する。
既存User Source、CMake定義、Presetを自動Migrationまたは再生成名目で上書きしない。

### ABI Surface

Game Module ABI v1は一つの固定Export Symbol `cue_game_module_query`から開始する。
ExportはHostが要求するABI VersionとHostが確保した出力構造体を受け取り、
互換な`CueGameModuleApiV1` Function Tableまたは安定Error Codeを返す。

ABI HeaderはC11とC++20の両方からInclude可能にし、次の規則を満たす。

- `extern "C"`はC++ Compile時だけ適用する
- 整数は`uint8_t`、`uint32_t`、`uint64_t`、`int32_t`、`int64_t`等の固定幅型を使用する
- `bool`、`wchar_t`、`size_t`、Compiler依存Enum幅、Reference、Template、RTTI、Virtual Classを使用しない
- 文字列はUTF-8の`const char*`と`uint64_t`長を持つ借用Viewとし、NUL終端を要求しない
- UUID型Identityは16 byte値として渡し、Compiler型名、Pointer値、登録順をIdentityにしない
- 可変長入力はPointerと`uint64_t`件数を持つ借用Arrayとし、呼出終了までだけ有効とする
- 全公開構造体は`structSize`と対象Versionを持ち、Hostは不足Sizeを拒否し、未知Tailを読まない
- Function Tableの予約Fieldは0またはNullを要求し、追加はVersionまたはSizeでNegotiationする
- `std::string`、`std::vector`、`std::span`、`std::unique_ptr`、`Result`、`Error`、`std::function`を公開しない
- C++ Exception、SEH、`longjmp`を境界越しのError通知に使用しない
- Calling ConventionとSymbol visibilityはMacroで一箇所に固定する

Module API TableはDLLが所有し、HostはDLL Load中だけ借用する。HostはTableの値を検証済みFactoryへCopyできるが、
Function PointerをDLL Unload後に呼ばない。v1はGame ModuleからHost機能を任意に呼ぶHost API Tableを公開しない。
後続のGameplay APIは必要機能だけを列挙したVersion付きTableとして追加し、既存v1構造体の予約領域へ暗黙追加しない。

生成、登録、Start、Update、Stop等の失敗可能なABI関数は安定した数値Resultを返す。
Hostは全出力Handleと出力構造体をNullまたは0へ初期化してからCallbackを呼ぶ。Module HandleまたはSystem Stateの
生成Callbackは成功時だけ完全構築済みHandleの所有権を出力へ移し、失敗時は出力をNullのまま維持して
Module内部の部分構築ResourceをすべてRollbackする。Queryと登録Callbackも失敗時に部分的な出力所有権をHostへ移さない。
System StateとModule Handleの破棄Callbackだけは戻り値を持たない失敗不能操作とし、同じ入力へ一度だけ呼ぶ。
破棄Callbackは所有物を完全に解放して正常復帰するか、契約違反を検出したModule自身がProcessをFail-fast終了する。
Exception、部分解放状態、再試行要求をHostへ返してはならず、Hostは破棄失敗を回復可能Errorへ変換してDLLをUnloadしない。
詳細診断はUTF-8の借用Viewとして同じ呼出中だけ返し、Hostが即時Copyする。
Game Module内のAllocationはGame Moduleが解放し、Host内のAllocationはHostが解放する。片側で生成したObject、Buffer、
String、Array、File Handleを他方の`free`、`delete`、Destructorで破棄しない。

### Registration Entries

`CueGameModuleApiV1`は次の責務を持つCallbackを固定順で提供する。

1. Project ScopeのModule Handleを生成する
2. Schema TypeとTombstoneを登録する
3. Component宣言を登録する
4. Runtime System Factory定義を登録する
5. Project ScopeのModule Handleを破棄する

Schema登録は`TypeId`、診断名、連続Schema Version、Field ID、Field診断名、Reserved Field IDをC互換Descriptorで渡す。
HostはCallback中に全値をFirst-party所有の一時Registration BatchへCopyし、Callback成功後だけ既存
`SchemaRegistryBuilder`へ追加する。Callback失敗、Descriptor不正、Copy失敗ではBatch全体を破棄し、Builderを変更しない。
登録後に借用Pointerを保持しない。Core Schemaを先に登録し、Game SchemaとTombstoneをModule登録順で受理した後、
全衝突を検査してSchema Registryを一度だけSealする。

Component登録はStable Type IDとComponent用途の宣言をSchema登録へ関連付ける。
M15 ABI v1はComponentのC++ Object Layout、Constructor、Move、Destructor、生Pointer、Storage Pointer、Query Viewを公開しない。
現行`World::register_component<T>`はCompile時C++型Bindingであるため、任意Project Componentを動的Storageへ登録する機能は
ECS Storage／ABIの別Researchを先行させる。M15では宣言の重複、対応Schema欠損、Stable ID衝突を診断できるところまでとし、
Component StorageやSerializationを追加しない。

System登録はStable UTF-8 ID、`PreUpdate`／`Update`／`PostUpdate`、`int32_t` Order、Dependency ID Array、
System State生成／破棄、Start／Update／Stop Callbackを渡す。HostはDescriptorを即時Copyし、CallbackとModule Handleへの
非所有参照を保持するFirst-party FactoryをProject Scopeへ構築する。Composition Rootが所有する
`ProjectGameModuleConnection`だけがModule Handleの一意Ownerとなり、全Factory、Adapter、System Stateより長く生存する。
System登録もHost所有の一時Batchへ集約し、Callbackと全Descriptorの検証成功後だけFactory集合として公開する。
FactoryはModule Handleを破棄せず、SessionごとにHost側`RuntimeSystem` Adapterを一つ生成する。
AdapterがDLL側System Stateを一意所有し、Adapterの破棄処理が失敗不能なDLL側破棄Callbackの一回実行を包含する。
System StateをAdapterと別のOwnerへ置かず、Adapterより先に独立破棄しない。

Game System ABI v1のStart／StopはSystem Stateだけを受け、UpdateはFrame Indexと符号付き64-bit NanosecondのTiming値だけを値で受ける。
`World`、`RuntimeWorld`、`StructuralCommandBuffer`、Entity Pointer、Component Pointer、Service Locatorを公開しない。
Start／Update／Stopを通した任意ECS操作とGameplay APIはM16 Gateの条件ではなく、後続ResearchでVersion付きHost APIとして追加する。
これによりM15でECS設計を変更せず、ABI v1を旧版の広大なGameplay APIへ拡大しない。

### Registration Order and Lifetime

Standalone ProcessとEditor Processは一つのProjectだけをGame Moduleへ接続する。順序は次のとおりとする。

1. ManifestまたはBuild Artifact MetadataからPath、Hash、Size、Architecture、Configuration、ABI VersionをLoad前に検証する
2. Windows Loader AdapterがDLLをLoadし、固定Entry Symbolだけを解決する
3. EntryへHost ABI Versionを渡し、Module自己報告値、Module API Table、Project Identityを登録前に検証する
4. Project ScopeのModule Handleを生成する
5. Engine Core Schemaを`SchemaRegistryBuilder`へ登録する
6. Game ModuleのSchema、Tombstone、Component宣言を登録する
7. 全SchemaとComponent宣言を検証し、Schema RegistryをSealする
8. Game ModuleのSystem Factory定義を登録順でProject ScopeへCopyする
9. Runtime Application SessionごとにFactoryからSystem StateとHost Adapterを生成する
10. 既存規則どおりRegistryをSealし、Phase、Order、登録順でStart／Update／逆順Stopする
11. 全SessionのStop完了後に各Adapterを破棄し、その処理内で所有System Stateを破棄した後、Factoryを破棄する
12. Project ScopeのModule HandleをDLL側Callbackで破棄し、最後にDLLをUnloadする

Schema Registry Seal失敗またはSystem Factory登録失敗ではSessionを開始しない。途中まで生成したHost所有値とModule Handleを
逆順で破棄し、DLLをUnloadする。System Start失敗以降はADR-0021のSession RollbackとCleanup契約へ従う。

Module DLLはModule Handle、Factory、Adapter、System State、Callbackのいずれかが生存する間Unloadしない。
Game Moduleを接続するComposition RootのThreadをProject Scope Owner Threadとする。DLL Load、Query、Module Handle生成、
Schema／Component／Factory登録、Module Handle破棄、DLL UnloadはこのThreadだけで行う。Game Module ABI v1を使用する
Runtime Application Sessionは同じProject Scope Owner Threadで生成、更新、停止、破棄し、System Lifecycle Callbackも同Threadだけで呼ぶ。
Game Moduleを使用しないSessionにはこの追加制約を適用せず、ADR-0021のOwner Thread契約に従う。

ModuleはCallback Context、借用String、借用Array、HostのOpaque ContextをCallback終了後に保持しない。
M15／M16ではBackground ThreadをGame Module ABIから開始しない。`DllMain`とC++静的初期化では、Thread生成、File IO、
Engine Callback、Module外Resource取得、永続Mutable状態の公開を行わない。外部Metadata不一致はDLL Load前に拒否するが、
Module自己報告値だけの不一致はDLL初期化後、Module Handle生成と登録の前に拒否し、即座にUnloadする。

### Toolchain, Runtime Library, and Configuration Compatibility

初期対応Matrixを次に固定する。

| Property | Supported Contract |
| --- | --- |
| Host OS | Windows |
| Architecture | x64 |
| Language | C++20 for Project Source、C11-compatible public ABI |
| Compiler family | Engineが記録した対応MSVC Toolset |
| Debug | Debug Host + Debug Game Module、MSVC Debug DLL Runtime |
| Development | Development Host + Development Game Module、MSVC DLL Runtime |
| Release | Release Host + Release Game Module、MSVC DLL Runtime |

Debug／Development／Releaseを相互に混在させない。Game Module Artifact Metadataは少なくともABI Version、Project ID、
Engine Compatibility、Configuration、Architecture、Compiler family、MSVC Toolset Identityを持つ。
LoaderはDLL Entryを呼ぶ前にManifest Metadataを検査し、Entry呼出後にもModule報告値との一致を検査する。

Game Module Artifact MetadataのFile名をVersion Directory直下の`CueGameModule.metadata.json`、初期`schemaVersion`を`1`とし、
Wire形式を次のJSON Objectへ固定する。例示値を除くMember名、型、必須性はこの形を正本とする。

```json
{
  "schemaVersion": 1,
  "artifactId": "01234567-89ab-4cde-8f01-23456789abcd",
  "projectId": "12345678-1234-4abc-8def-1234567890ab",
  "engineCompatibility": {
    "minimum": "0.1.0",
    "maximumExclusive": null
  },
  "abiVersion": 1,
  "configuration": "Debug",
  "architecture": "x64",
  "compilerFamily": "msvc",
  "msvcToolset": {
    "compilerVersion": 1944,
    "fullVersion": 194435123,
    "build": 0
  },
  "runtimeLibrary": "DebugDll",
  "iteratorDebugLevel": 2,
  "moduleFile": "CueGameModule.dll",
  "entrySymbol": "cue_game_module_query"
}
```

全Memberを必須かつNon-nullとするが、`engineCompatibility.maximumExclusive`だけはJSON StringまたはNullを許可する。
Top-level、`engineCompatibility`、`msvcToolset`の未知Memberと重複Keyを拒否する。`artifactId`と`projectId`は小文字Canonical
UUID v4文字列、Engine Versionは`major.minor.patch`の非負整数3要素とし、Project Descriptorの既存契約に従う。
`abiVersion`はJSON整数`1`、`configuration`は`Debug`、`Development`、`Release`、`architecture`は`x64`、
`compilerFamily`は`msvc`だけをv1で許可する。`msvcToolset.compilerVersion`、`fullVersion`、`build`はそれぞれ
`_MSC_VER`、`_MSC_FULL_VER`、`_MSC_BUILD`の`0`以上`9007199254740991`以下のJSON整数とする。
`runtimeLibrary`はDebugの`DebugDll`またはDevelopment／Releaseの`Dll`、`iteratorDebugLevel`は対応する`_ITERATOR_DEBUG_LEVEL`の
JSON整数とする。`moduleFile`と`entrySymbol`は上記固定文字列から変更しない。

Writerは上記Member順のUTF-8、BOMなし、LF、末尾改行ありで出力する。ReaderはMember順と意味を持たない空白には依存しない。
ReaderはMetadata自身を`Current.json`のInventoryでSize／Hash検証してからParseし、Artifact ID、Project ID、Configuration、
Architecture、Compatibility、ABI、Toolset、Runtime LibraryをHostと照合した後だけDLLをLoadする。未知、欠落、破損した
Metadata VersionをMigrationせず拒否し、対応するEngineとConfigurationで再Buildして再生成する。

C ABIでAllocator所有権を分離しても、異なるConfigurationやToolsetの組合せを暗黙に互換とは扱わない。
Generated Project CMakeはEngineが公開するBuild Policy Targetを使用し、DebugではDebug DLL Runtime、
Development／ReleaseではDLL Runtimeを選択する。`_MSC_VER`、Runtime Library、Iterator Debug Level等の不一致を
Link時またはArtifact Validationで診断し、LNK2038を場当たり的なCompiler Option無効化で回避しない。

Toolchain Versionの許容Rangeは#220のEnvironment ValidationでEngine Build Metadataから決定する。
初期契約は次の半開区間とする。

| Tool | Minimum | Maximum Exclusive | Compatibility Unit |
| --- | --- | --- | --- |
| CMake | Engine Configure時の`CMAKE_MINIMUM_REQUIRED_VERSION` | 次のMajor Version | 同一Major |
| Visual Studio／MSBuild | Engineを生成したVisual Studio Majorの先頭 | 次のMajor Version | 同一Major |
| MSVC Compiler | EngineをCompileした`_MSC_VER`のMajor／Minor先頭 | 次のMinor Version | 同一Major／Minor |
| Windows SDK | Engineが選択した4要素Version | 最終要素を1増やしたVersion | 選択Version完全一致 |

CMakeは同一Major内のPreset／Command Line互換、Visual Studioは同一Generator Major、MSVCは同一`_MSC_VER`が表す
Toolset／Runtime互換を前提とし、MSVCのServicing Patchは許容する。Windows SDKは実際に選択したHeaderとx64 Libraryの組を
検証するため完全一致とする。これより広いRangeを暗黙に採用せず、次のMajor、MSVC Minor、SDK Versionへ対応するときは
EngineをそのToolchainで再構成、再検証してBuild Metadataを更新する。

この選択は既知のServicing Updateを許容しつつ、未検証のGenerator、Compiler ABI、SDK Header／Library差を拒否する代わりに、
新しいToolchainをInstallしただけでは既存Engine Binaryが対応済みにならない。EditorはUnsupportedとUnknownを区別し、対応Engineでの
再Configure／Buildまたは互換Toolchainの選択を修復案として示す。

本ADRは特定Visual Studio Install Path、Windows SDK Install Root、CMake Install Pathを共有Projectへ固定しない。

### Editor Build Service Boundary

Editor UIは`Cue.Build`のApplication Serviceへ型付きBuild Requestを渡すだけとする。
ServiceはToolchain検証、Plan生成、Process実行、Stage遷移、Log、Cancel、Artifact Publishを所有する。
`GameBuildService`は生成ThreadをOwner Threadとし、Build開始、Retry、完了待機をOwner Threadへ限定する。
状態SnapshotとCancel要求は別Threadから利用できる。終了時は進行中OperationへCancelを通知し、WorkerとChild Processの完了を待つ。
Artifact Publisherにも同じCancellationを借用で渡し、不可逆な`Current.json`更新前まで取消を監視させる。
取消を受理したPublisherはArtifact Inventoryを返さず、以前のLatest Successful Artifactを維持する。
一方、`Current.json`のCommit後に到着した取消は確定済みArtifactを巻き戻さず、そのOperationの成功を優先する。

```text
ImGui Build UI
  -> Build Request
  -> Game Build Service
       -> Toolchain Validation
       -> Immutable Build Plan
       -> CMake Runner
       -> Child Process Service
       -> Artifact Validation / Publish
  <- State Snapshot / Log Event / Result / Artifact Inventory
```

UIはCMake、MSBuild、`CreateProcessW`、Job Object、Filesystem Publishを直接呼ばない。
Build ServiceはEditorDocument、Selection、ImGui Context、RuntimeWorldを所有せず、Game ModuleをLoadまたは実行しない。
M15ではBuild完了後のHot Reload、Editor Play自動再起動、Runtime Session差替えを行わない。

Build RequestはProject Root、Configuration、Target、Operation IDを検証し、不変Build Planへ変換する。
Process RunnerへShell Command文字列ではなくExecutable PathとArgument Vectorを渡す。
Machine固有EnvironmentはAllowlistで構成し、Credentialや無関係なEnvironmentをPlan、Log、診断Bundleへ保存しない。

### Artifact Publication and Failure Contract

各Build Operationは互換KeyのBinary Treeを再利用し、独立したLog、Result、Candidate Artifact領域を持つ。
ConfigureまたはBuild開始時に以前の成功Artifactを削除、切詰め、上書きしない。

CMake Process成功後、要求Targetの出力をOperation固有Candidate領域へCopyし、そのSnapshotだけを検証する。
Binary Tree内の中間出力は成功Artifactの正本にせず、失敗Buildで更新されても公開済みSnapshotへ影響させない。

CandidateはProcess成功だけで公開せず、次をすべて検証する。

- 要求Configuration、Architecture、Targetと一致する
- 必須DLLが存在し、通常FileでRoot境界内にある
- Sizeが0ではなく、Hashを取得できる
- ABI、Project、Engine Compatibility、Toolset Metadataが一致する
- Artifact InventoryのFile名、相対Path、Size、Hashが確定している

検証成功後、Candidate集合を`Generated/Artifacts/<configuration>/Versions/<artifact-id>`へ一意な不変Directoryとして公開する。
既存Version Directoryを置換せず、同じArtifact IDが存在する場合は不一致として拒否する。Directory公開と全File再検証の後、
Artifact ID、相対Path、Size、Hashを持つ小さい`Current.json`だけをADR-0014のRegular File Atomic Replaceで更新する。

`Current.json`はMilestone間で共有するVersion付き永続Manifestであり、初期`schemaVersion`を`1`とする。Readerは
`schemaVersion == 1`だけを受理し、未知Version、新しいVersion、欠落Versionを推測して読まない。`Current.json`は再生成可能な
Build出力なので、互換性のないVersionをIn-place Migrationせず、対応するEngineとConfigurationでGame Moduleを再Buildして再生成する。

Schema v1のWire形式は次のJSON Objectへ固定する。例示値を除くMember名、型、必須性はこの形を正本とする。

```json
{
  "schemaVersion": 1,
  "artifactId": "01234567-89ab-4cde-8f01-23456789abcd",
  "configuration": "Debug",
  "files": [
    {
      "path": "CueGameModule.dll",
      "sizeBytes": 123456,
      "hashAlgorithm": "sha256",
      "contentHash": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    },
    {
      "path": "CueGameModule.metadata.json",
      "sizeBytes": 1024,
      "hashAlgorithm": "sha256",
      "contentHash": "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
    }
  ]
}
```

全Memberを必須かつNon-nullとし、Top-levelとFile Entryの未知Member、重複Key、空の`files`を拒否する。
`schemaVersion`はJSON整数`1`、`artifactId`は小文字Canonical UUID v4文字列で、参照先Version Directory名と完全一致させる。
`configuration`は`Debug`、`Development`、`Release`のいずれかで、Artifact StoreのDirectory名と一致させる。
`files`はInventory Entry Objectの配列である。`path`はVersion Directory相対のUTF-8文字列、`sizeBytes`は
`0`以上`9007199254740991`以下のJSON整数とする。Game Module DLL自身の`sizeBytes`は`0`を拒否する。
`hashAlgorithm`は文字列`sha256`だけを許可し、`contentHash`はFileの先頭から末尾までの未変換Byte列に対するSHA-256 Digestを
小文字64桁のHexで表す。改行、BOM、Text Encodingを正規化しない。`path`は`/`区切りの正規化済み相対Pathとし、`..`、絶対Path、
重複Entry、WindowsのCase-insensitive比較で衝突するEntryを拒否する。`files`は`path`のUTF-8 Byte列による昇順で保存する。
Writerは上記Member順のUTF-8、BOMなし、LF、末尾改行ありで出力する。ReaderはJSONのMember順と意味を持たない空白には依存しない。
Windows実装はOSのCNGを利用できるが、Manifest上のAlgorithmとByte表現を変更しない。
`files`には`path == "CueGameModule.dll"`と`path == "CueGameModule.metadata.json"`のEntryをそれぞれちょうど一つ必須とし、
いずれかの省略、Case違い、重複をReaderで拒否する。DLLのSize／Hashはこの必須Entryだけを正本とする。

M16 Publisherは後述のRead Leaseを取得して`Current.json`を一度読み、指定Version Directoryだけを入力にし、全HashとSizeを
再検証してCopy完了までLeaseを保持する。
このVersion Snapshotは再生成可能なPublisher入力であり、RuntimeがProjectのGenerated Rootから直接Loadする契約ではない。
失敗、Cancel、Timeout、Editor終了、Metadata不一致ではCandidateを成功として公開せず、以前の成功SnapshotとInventoryを保持する。
失敗OperationのLog、Plan、Environment Report、Stage Resultは`Saved/Build/Operations/<operation-id>`へ診断可能な範囲で残す。

`Current.json`更新結果はADR-0014のOutcomeを失敗理由から分離して扱う。

- `Committed`: 新しいManifestをCurrentとして確定し、Build Operationを成功にできる
- `NotPublished`: Build Operationを失敗とし、以前のCurrentが維持されているものとして扱う
- `PublishedButDurabilityUnknown`: Build Operationを成功にせず、同じMutation Lease内で`Current.json`を再読込し、Schema、参照先、
  Inventory、Size、Hashを再検証する。検証できた可視Manifestを現在の選択として診断へ記録し、旧Current維持とは報告しない。
  再読込または検証に失敗した場合はCurrent不明としてPublisher利用を停止し、旧版または新版を推測しない

通常のBuild成功通知、Recent状態、Package開始は`Committed`だけから自動継続する。`PublishedButDurabilityUnknown`で可視な新版を
M16 Publisherへ渡すには、再度Read Leaseを取得して同じ検証を通し、利用者へDurability不明を明示した再試行操作を要求する。

Artifact Retention、古いBinary Tree削除、Diagnostic Bundle上限は後続Issueで決めるが、User SourceとLatest Successful Artifactを
Cleanup対象へ含めない。

Build Publish、Cleanup、M16 Publisher読取りは、Project IdentityとConfigurationごとのArtifact Storeが発行するProcess間共有の
Project Scope Leaseを必須とする。Windows実装は`Generated/Artifacts/<configuration>/Access.lock`の固定Byte Rangeに対する
`LockFileEx`を使用し、Shared Read LockまたはExclusive Mutation Lockを全Process、全Artifact Store Instanceで共有する。
全実装がLockする範囲はOffset `0`、Length `1`の先頭1 byteとし、Shared Readでは`LOCKFILE_EXCLUSIVE_LOCK`を指定せず、
Exclusive Mutationでは指定する。同じPathをCreate-or-openしてHandle生存中はFileを置換または削除しない。
Lock FileはVersion Cleanup対象にせず、Process終了時はOSによるHandle CloseでLockを解放する。
Version Directory公開から`Current.json`更新とOutcome再検証までは一つのExclusive Mutation Leaseで直列化する。Cleanupも同じ
Exclusive Mutation Leaseを取得し、Current参照先を削除しない。PublisherはShared Read Leaseを`Current.json`読取り前に取得し、
参照先のHash検証とPackage CandidateへのCopyが完了するまで保持する。Exclusive Mutation Leaseは既存Read Leaseの終了を待つため、
読取り中のVersionをCleanupできない。Directory公開直後の未参照VersionもPublish Lease中はCleanupから保護される。
全First-party Build／Publisher入口はこのArtifact Storeを経由し、Lease取得失敗時に同期なしのFilesystem操作へFallbackしない。
CleanupはExclusive Mutation Lease取得後に`Current.json`のSchema、全Field、参照先Inventoryを完全検証する。ManifestがMissing、
読取り不能、破損、未知Version、参照先不一致のいずれかなら、公開済みVersion Directoryを一つも削除せずFail-closedで終了する。
`Current.json`がMissingかつVersionsが空の場合だけ、削除なしの成功とする。
CleanupはBuild成功条件にせず、失敗しても成功Artifactの正本を失わせない。

### Build Diagnostic Bundle Wire Format

Build Diagnostic Bundleは、Build失敗を再現・共有するための再生成可能な診断Snapshotである。Project Source、Credential、
永続User設定の正本ではない。Directory名は任意だが、Directory全体をSibling Stagingへ完成させてから一度だけRenameし、
部分Bundleを最終Destinationとして公開しない。同じDestinationを上書きせず、失敗時は呼出しが所有するStagingだけをRollbackする。

Schema v1はUTF-8、BOMなし、LF、末尾改行ありのJSONとLog Fileで構成する。Writerは次の固定Member順でCanonical JSONを出力し、
Readerは各Memberの欠落、重複、未知Member、型不一致、未知列挙値、末尾Dataを拒否する。`manifest.json`はFile名、収集状態、
Byte数、欠損理由を含み、Canonical表現とのByte完全一致を要求する。収集済みPayloadはManifestのByte数と実Fileを照合した後、
個別Schemaも検証する。`entries`は`artifact.json`、`environment.json`、`plan.json`、`result.json`、`stages.json`、
`stderr.log`、`stdout.log`のPath昇順へ固定し、位置ごとの一致も検証する。

| File | Schema v1の必須内容 |
| --- | --- |
| `manifest.json` | `schemaVersion`、`operationId`、終端`state`、固定順の`entries`。各Entryは`path`、`collected`、`sizeBytes`、`missingReason` |
| `plan.json` | `schemaVersion`、`projectRoot`、`presetName`、`workspaceKey`、`binaryDirectory`、`candidateDirectory`、`operationDirectory`、`artifactStoreDirectory`、`targetName` |
| `environment.json` | `schemaVersion`、`support`、Engine Source／Binary Root、対応Configuration列、選択Tool列、診断列 |
| `stages.json` | `schemaVersion`と、`stage`、`outcome`、任意`exitCode`からなる完了Stage列 |
| `result.json` | `schemaVersion`、Manifestと一致する終端`state`、Domain／Code／Context／任意Native Errorを持つ診断列 |
| `artifact.json` | `schemaVersion`と、Operation ArtifactまたはLatest Successful Artifact。各ArtifactはID、Configuration、File Path／Size／Hashを持つ |
| `stdout.log` | Redaction済み標準出力Chunkの連結Byte列 |
| `stderr.log` | Redaction済み標準エラーChunkの連結Byte列 |

`environment.json`の選択Toolは`kind`、`path`、`root`、任意`version`、`architecture`、`available`を持つ。
Environment診断は`code`、任意`tool`、`support`、`path`、`summary`、`repairHint`を持つ。Schema v1の`kind`と`tool`は
`CMake = 0`、`VisualStudio = 1`、`MsvcCompiler = 2`、`WindowsSdk = 3`の安定した数値値だけを許可する。
Schema v2はこの列挙へ`Git = 4`だけを追加する。WriterはSchema v2を生成し、Readerは既存Schema v1を読み取るが、
Schema v1内の`Git = 4`をVersion偽装として拒否する。他のPayloadはSchema v1を維持する。
`code`は`UnsupportedHostArchitecture = 0`、`MissingTool = 1`、`UnknownToolIdentity = 2`、`UnsupportedTool = 3`、
`AmbiguousTool = 4`、`MissingEngineSource = 5`、`MissingEngineBinary = 6`だけを許可する。
`support`は`supported`、`unsupported`、`unknown`、`architecture`は`unknown`、`x64`、`x86`、`arm64`、
対応Configurationは`Debug`、`Development`、`Release`の既知値だけを許可する。
`tool`がない診断はJSON `null`とする。Build Operation `state`は`Succeeded`、`Failed`、`Cancelled`、`TimedOut`に対応する
`succeeded`、`failed`、`cancelled`、`timedOut`だけを許可し、実行中または未知値を保存しない。
Stageは`configure`または`build`、Outcomeは`succeeded`、`failed`、`cancelled`、`timedOut`だけを許可する。
成功はExit Code 0、失敗は1から4294967295までを必須とし、CancelとTimeoutはExit Codeを持たない。
Artifact Snapshotは`BuildArtifactInventory`と同じlowercase UUID v4、Configuration、安全な相対Path、64文字lowercase
SHA-256、File Size上限、Path昇順、ASCII case-insensitive重複拒否、必須DLL／Metadata非空制約をReaderにも適用する。
`operationArtifact`はOperation Stateが`succeeded`の場合だけ必須とし、それ以外では禁止する。過去に成功した
`latestSuccessfulArtifact`は現在Operationの終端Stateに関係なく任意とする。

絶対PathはProject Root、Engine Root、選択Tool、Environment診断、およびCallerが明示したMappingをTokenへ置換する。
未Mappingの絶対Pathを暗黙に追加せず、MappingのNative Prefixは4,096 byte、Tokenは64 byteを上限とする。NUL、Path区切りを含む
Token、空Prefixを拒否し、借用入力を所有BufferへCopyする前に検証する。Credential名やEnvironment全体は収集しない。
明示Mappingと自動Mapping候補の合計件数は設定上限を所有Vector確保前に適用する。各File数、File単位Byte数、Bundle総Byte数を
Serialization前から適用し、上限超過時に部分出力を公開しない。
JSONとLogを含む全FileはStrict UTF-8として生成前と読込時に検証し、Overlong Encoding、Surrogate、範囲外Scalar、
不完全Sequenceを拒否する。LogはOS Read Chunk境界ではなく同じStreamの連結後Byte列を検証・Token化する。Native Tool
出力がUTF-8でない場合は推測変換せず、そのBundle Exportを失敗として報告する。
Log先頭のUTF-8 BOMは除去し、CRLFと単独CRはLFへ正規化し、空Logを含め末尾LFを保証する。ReaderはBOM、CR、
末尾LF欠落を拒否する。Bundle Source Directory自身と配下Entryは、WindowsではExtended-length Native Pathへ
変換して`FILE_ATTRIBUTE_REPARSE_POINT`を明示検査し、Tagの種類に関係なく追跡せず拒否する。列挙とFile読込も
同じNative Pathを使用し、WriterのDestination／Stagingも含め`MAX_PATH`を超える有効な絶対Pathを短いPathと同じ契約で扱う。

`schemaVersion`は全JSONで整数`1`だけを受理する。v1 Bundleは診断用Snapshotであり、Readerは未知Versionを推測読込または
In-place Migrationしない。Schema追加、Member意味変更、列挙値変更、Redaction契約変更は先行ADRで新Versionと互換性方針を決め、
Writer、全Payload Reader、Canonical Manifest、拒否経路Testを同時に更新する。旧Bundleの利用が必要なら、元Engine Versionで読込み、
新Versionへ明示Exportする別Toolを設計し、通常Readerへ暗黙Migrationを入れない。

### Error and Diagnostic Contract

Game Module ABIは安定数値ResultとUTF-8診断だけを返す。HostはModule Errorを`Cue.Foundation`のErrorへ変換し、
ABI Version、Project ID、Configuration、Stage、System ID等の値をContextへCopyする。DLL内部Pointer、Native Handle、
Exception Object、Call Stack ObjectをErrorへ保存しない。

ABI CallbackからExceptionまたはSEHが境界外へ出た場合はContract違反として扱う。通常の回復可能Errorへ変換して継続せず、
既存Fatal PolicyでProcessを停止する。Build失敗はRuntime Fatalではなく、Build Operationの失敗として既存成功Artifactを保持する。

診断にはSource本体、Credential、Environment全体を既定で含めない。絶対User PathはUIで必要な範囲だけ保持し、
Export時は#226のRedaction契約へ従う。

## Consequences

### Positive

- Engine SourceをProjectごとに再Buildせず、一つのGame Module Artifactとして扱える
- Runtime、Editor、Project Codeの依存方向が一方向になる
- DLL境界のAllocator、STL、Exception、Compiler固有C++ ABI事故を限定できる
- Build失敗と以前の成功Artifactを明確に分離できる
- M16がManifestからRuntimeHost、Game Module、Runtime Dataを決定的に起動できる
- Hot Reloadと大規模Gameplay ABIをM15から分離できる

### Trade-offs

- C ABI Descriptor、Host Adapter、Metadata、Compatibility検証が必要になる
- Static Linkだけの構成よりTargetとArtifactが一つ増える
- C++型を直接渡せないため、Game APIはVersion付きHost Functionとして個別設計する必要がある
- M15 ABI v1だけでは任意Project ComponentをRuntime ECSへ格納、Query、編集できない
- ABIを変更するたびにVersionと互換性Testが必要になる
- Artifact Publish、Cleanup、Publisher読取りをProject Scope Leaseで同期する必要がある

### Mitigations

- ABI v1を登録とLifecycleの最小面積へ限定し、予約APIを先回りして追加しない
- Public ABI HeaderをCとC++の双方で単体Compileする
- Module内生成／Module内破棄、Host内生成／Host内破棄をTestで固定する
- Runtime System AdapterとLoaderをFirst-party Targetへ隔離する
- Gameplay APIと任意Component Storageは実要件を伴うResearch Issueから追加する
- M15 GateでAsset Pipeline、Hot Reload、ECS変更が差分へ含まれないことを確認する

## Rejected Alternatives

### ProjectごとのRuntimeHostへGame SourceをStatic Linkする

DLL ABIは不要になるが、ProjectごとにEngine Hostを再Linkし、共通RuntimeHost ArtifactとM16 Package Manifestの境界が崩れる。
Build時間と配布物の重複も増えるため、M15の正式経路には採用しない。

### C++ Virtual InterfaceとSTLをDLLへ公開する

Compiler、Runtime Library、Iterator Debug Level、Exception、Allocator、Object Layoutの一致を暗黙前提にする。
所有権と破棄責務を局所的に証明できないため採用しない。

### Game ModuleをEditor Processへ直接LoadしてHot Reloadする

Build、Editor、Session、DLL、Thread、PDB、失敗RollbackのLifetimeを同時に固定する必要がある。
M15のBuild機能完成を遅らせるため採用しない。

### ABI v1へECS、Renderer、Sound、Effect、Physicsの全操作を入れる

未確定SubsystemとData LayoutをABIへ固定し、旧CueEngineと同様にVersion更新範囲を肥大化させる。
ユーザー方針にも反するため採用しない。

### Engine C++ LibraryをGame DLLへ直接Linkする

既存C++型、Template、所有権がDLL境界へ漏れ、Engine内部RefactorをABI変更にする。
Game ModuleはC互換ABI Headerだけへ依存させるため採用しない。

### 任意Service ContainerをGame Moduleへ公開する

依存、Lifetime、Thread、利用可能APIを実行時Lookupまで隠し、ADR-0021の明示Compositionに反するため採用しない。

### 失敗Build開始時に前回Outputを消去する

CancelまたはCompiler Errorで最後に動作したArtifactを失う。Operation単位のCandidateとAtomic Publishを使用するため採用しない。

## Validation Contract

M15で次を検証する。

- Blank Project Generatorが`Source/Game`とCMake正本をProject Root内へ生成する
- 既存User Source、CMake定義、Presetを上書きしない
- 生成ProjectをDebug／Development／ReleaseでConfigure、Buildできる
- C11とC++20のTranslation UnitがABI Headerを単体Includeできる
- ABI Public HeaderがSTL、Exception、C++ Class、Windows型を公開しない
- 誤った外部MetadataのProject ID、Architecture、Configuration、ToolsetをDLL Load前に拒否する
- `CueGameModule.metadata.json` v1の全必須Memberと型をWriter／Readerで一致させ、未知VersionをLoad前に拒否する
- Module自己報告のABI Version、Project ID、Configuration不一致をModule Handle生成と登録の前に拒否する
- Schema、Component宣言、System Factoryを固定順で登録し、失敗時に逆順Cleanupする
- Module Handle／System State生成失敗時に出力をNullのまま維持し、Module内の部分Resourceを残さない
- 二つのSessionが別のDLL側System Stateを持ち、片方の停止が他方へ影響しない
- `ProjectGameModuleConnection`だけがModule Handleを所有し、Factoryは長寿命の非所有参照だけを持つ
- System StateとModule Handleを生成したDLL側Callbackで一度だけ破棄する
- Adapter破棄がSystem State破棄を包含し、失敗不能破棄Callback以外の経路で解放しない
- Callback、Factory、System State生存中にDLLをUnloadしない
- Project ScopeとGame Module使用Sessionの全Callbackを一つのOwner Threadへ限定する
- UIなしでBuild Request、Plan、Cancel、Retry、Artifact Publishを検証できる
- CancelとTimeoutを区別し、Process TreeとHandleを残さない
- 同じ`workspace-key`のConfigure、Build、Candidate収集、Binary Tree CleanupをProcess間Exclusive Build Leaseで直列化する
- Build Lease解放後だけArtifact Leaseを取得し、二つのLeaseを同時保持しない
- 不変Version Directory公開後に`Current.json`だけをAtomic Replaceする
- `Current.json`の`schemaVersion == 1`だけを受理し、未知Versionを拒否して再Buildで再生成する
- Schema v1のTop-levelとFile Entryについて、Member名、型、必須性、Artifact ID、Configuration、PathをWriterとReaderで一致させる
- Schema v1のInventory Hashを、未変換File Byte列に対する小文字64桁SHA-256としてWriterとReaderで一致させる
- `files`が`CueGameModule.dll`と`CueGameModule.metadata.json`をそれぞれちょうど一つ含まなければ拒否する
- `NotPublished`では以前のCurrentが変わらず、`PublishedButDurabilityUnknown`では可視Manifestを再読込・再検証して
  Current選択とBuild失敗診断を一致させる
- 複数Process、複数Artifact Store Instance間でPublishとCleanupをExclusive Mutation Leaseにより直列化し、M16 Publisherの
  Shared Read Lease中は参照Versionを削除しない
- 全Windows実装が同じ`Access.lock`のOffset `0`、Length `1`を`LockFileEx`でLockする
- `Current.json`を完全検証できないCleanupは全Versionを保持してFail-closedで終了する
- Runtime、Game Module、Build CoreからEditor／ImGuiへの逆依存がない
- M15差分にAsset Pipeline、Hot Reload、ECS Storage改良が含まれない

M16でManifestに列挙されたGame ModuleのHash、Size、Compatibilityを検証し、Package移動後もCurrent Directoryへ依存せず
Load、Startup、Stop、UnloadできることをProcess Testで追加確認する。

## Implementation Sequence

1. #219でGame Source、CMake Workspace Generator、ABI v1 Header、最小Game Entryを実装する
2. #220でCMake、Visual Studio、MSVC、Windows SDK、Engine Build Metadataを検出、検証する
3. #221でArgument Vector、Log Capture、Cancel、Timeout、Process Tree終了を持つWindows Child Process境界を実装する
4. #222でBuild Request、Immutable Plan、Debug／Development／Release Profileを実装する
5. #223でCMake Configure／Build RunnerとStage Resultを実装する
6. #224でBuild Operation、Log、Artifact Inventory、Latest Successful Artifactを所有するServiceを実装する
7. #225でEditor Build UI、Console、Cancel、Retry、Artifact表示を実装する
8. #226でPath Redactionと上限を持つBuild Diagnostic Bundleを実装する
9. #227で3構成Build、Process／Cancellation Test、手動Editor Workflow、Artifact保全を完了判定する

## Follow-up

- #219から#227で本契約のBuild側を実装、統合、検証する
- #233でPackage ManifestからGame ModuleをLoadし、登録、Session実行、Stop、Unloadを接続する
- 任意Project ComponentのStorage、Query、SerializationはECS改良を再開する際のResearch Issueで決定する
- GameplayからWorldを操作するHost APIは具体的な最小Game要件を伴う別Research IssueでVersion化する
- Scripting、Hot Reload、Game Rendering、Sound、Effect、Physics、Asset Pipelineは本ADRで先行実装しない
