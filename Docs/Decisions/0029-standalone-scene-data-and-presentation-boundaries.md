# ADR-0029: Standalone Scene Data and Presentation Boundaries

- Status: Accepted
- Date: 2026-09-21
- Decision Owners: CueEngine Project
- Relates to: ADR-0023、ADR-0024、ADR-0026、ADR-0027

## Context

M24 は Package 内の Startup Scene から Main Camera と Built-in Cube を読み、
Standalone RuntimeHost の Window に最小の Scene 描画を行う。
現在の Runtime Scene Data v1 は Component が空であることを必須とし、
Standalone の Presentation は固定色 Clear／Present だけを行う。
Editor は Camera／Mesh を CPU 所有の `RenderSnapshot` へ抽出するが、
Editor の GPU 描画は ToolHost が所有する。ToolHost／ImGui を Runtime に取り込まない。

Runtime Scene Data の永続契約、GPU Resource の所有と Fence、Dynamic／Static
Package の Trust を同時に変更するため、実装より先に境界を決める。

## Decision

### Runtime Scene Data v2

- ADR-0023 の v1 の意味と Reader を変更しない。v1 は空 Component の Scene として引き続き読む
- Writer は Component がない場合は v1 を出力し、サポート対象 Component がある場合だけ v2 を出力する
- Runtime Project Data と Package Manifest の Schema Version は変更しない。Manifest の既存 Size／Hash と Scene Identity の検証を維持する
- v2 は v1 の Object、Hierarchy、Active、Transform の Member と順序を維持し、`components` に限って次の固定形を許す

```json
{"instanceId":"12345678-1234-4abc-8def-1234567890ab","typeId":"70000000-0000-4000-8000-000000000001","schemaVersion":1,"fields":[{"fieldId":1,"value":true},{"fieldId":2,"value":60},{"fieldId":3,"value":0.1},{"fieldId":4,"value":1000}]}
```

- `instanceId` と `typeId` は lowercase UUID v4、`schemaVersion` と `fieldId` は正の JSON 整数とする
- Component は Instance ID の Byte 辞書順、Field は Field ID 昇順に固定する。重複、未知 Member、未知 Field、未知 Type／Version、Opaque Component／Field は拒否する
- v2 が許す Type は `Cue.Renderer.Camera` と `Cue.Renderer.Mesh` の各 Schema Version 1 だけとする。一 Object に同じ Type を複数置かない
- v2 は Source Scene と同じ最大 4096 Object、各 Object 最大 2 Component、最大 4096 Cube Draw とする。v1 の既存 Object 上限は変更しない
- Camera は Field 1 の Boolean `isMain`、Field 2～4 の有限 binary64 `verticalFovDegrees`／`nearPlane`／`farPlane` を正確に一つずつ持つ。Renderer の有限 binary32 変換後も `0 < fov < 180`、`0 < near < far` を満たす
- Mesh は Field 1 の Asset Reference `cue://engine/mesh/cube` だけを持つ。Source Path、一般 Asset Database、任意 Mesh を参照しない
- Number は locale 非依存の最短 round-trip 表現とし、既存の UTF-8／BOM なし／LF 終端、上限、Canonical Member 順を守る
- Reader は Game Module 接続前に First-party Core＋Renderer の一時 Schema Registry／Value Schema Registry を構築し、全 Member と値を検証して `KnownComponentData` を含む所有 Snapshot を構築する。同じ Version の Publisher で再生成した Byte 列との完全一致を確認する。未知 Data を黙って破棄しない
- Authoring Scene から Runtime Scene Data への変換は一方向であり、v2 から Authoring Scene を復元する API は追加しない

v2 の Object は永続 Object／Component ID と Runtime 実体化に必要な値だけを保持する。
`SceneSnapshot` の Runtime 入力は検証済み Component を所有するよう拡張し、
`SceneInstance` が既存 `RuntimeComponentBuilder` で World へ実体化する。
Source Scene の名前、未知 Extension、Editor 状態、Undo 履歴は Runtime Data へ含めない。

### Composition and Trust

- Package Publisher は保存済み Startup Scene の不変 Snapshot を一度だけ受け取り、v2 のサポート対象を公開前に検証する。失敗時は部分 Package を公開しない
- RuntimeHost は Executable 相対の Package Root、Manifest Inventory／Hash／Size、Identity、Dynamic／Static 固有 Trust を先に検証する。未知・改ざん・非 Canonical v2 を Game Module 呼出前に拒否する
- Reader の一時 Registry は Snapshot 構築後に破棄できる。Snapshot は Stable ID と所有値を保持し、Registry 世代 Token や Pointer を保持しない。Session 開始時には別の Registry を Composition Root で構築する
- Package Reader と Publisher は共通の First-party Camera／Mesh Schema ID と Value 契約を使う。Dynamic と Static で Runtime Scene Data の意味を分岐しない
- RuntimeHost の Composition Root は Core Schema と Renderer Schema を Seal 前に登録し、Renderer Runtime System と Component Builder Factory を Session 開始前に登録する。Game Module に同じ Type ID を上書きさせない
- RuntimeHost Application が `RenderSnapshotStore` を Session と System より長く所有する。Runtime World の Entity／Component Pointer は Process の描画境界へ渡さず、GameView と同じ CPU `RenderSnapshot` の所有値だけを渡す
- Main Camera が 0 件または複数件なら ADR-0027 と同じ診断 Clear とし、別 Camera を暗黙選択しない。Package 内の不正な投影値は Load を拒否し、実行中に不正値が生じた場合は Renderer の診断可能な Error を保持して Frame を失敗させる
- Shipping Link Closure に必要な `Cue.Renderer` と `Cue.EngineAssets` を明示し、ImGui／ToolHost／Editor、追加の Game DLL、Source Asset を含めない

### M24 Presentation Ownership

- `Cue.Renderer` は World から CPU Snapshot への抽出を所有し、GPU Native Handle は保持しない
- RuntimeHost の薄い変換層は Snapshot から固定の Cube Draw と Camera Matrix を作り、Scene Frame の同期呼出中だけ RHI へ借用する。未知 Mesh を黙って描画対象から外さない
- `Cue.RHI` の非 Native な固定 Scene Frame 入力は Camera Matrix、Instance Matrix、固定色 Clear と初期 Cube Geometry に必要な値に限定する。Renderer／Scene／World 型、Win32／D3D12 型、所有 Pointer を公開しない
- Clear 専用 `present_frame` は既存 Smoke と v1 互換のため残す。Scene 対応 Context へ別の限定 Frame 投入経路を設け、Scene 非対応 Context は状態変更前に拒否する
- `Cue.RHI.D3D12` の Presentation Context が Back Buffer、Command List、Queue、Fence と併せて M24 固定 Pass の Pipeline、Cube GPU Buffer、D32 Depth／DSV、Frame Slot 別 256-byte 整列 Constant を所有する。ToolHost の Resource と Code は共有・コピーしない
- 一 Frame は `PRESENT → RENDER_TARGET → Clear／Depth Clear／Draw → PRESENT` を一つの Command List に記録し、既存の一回 Submit／Present／Signal 経路へ戻る。裏面 Cull、Depth Write と `LESS`、左手座標・Winding は ADR-0011／0026／0027 に従う
- Frame 入力の Size、有限値、Instance 上限、Mesh 対応、Context 状態は記録開始前に検証し、黙って切り詰めない。借用値と Native Command List を呼出後に保持しない
- Resize は既存 Fence による GPU Idle 証明後に Depth／DSV を退役・再生成する。0 Size 中は投入を延期する。生成失敗、Device Removal、Signal 失敗は既存 Context の `DeviceRemoved`／`Unavailable` と診断・解放契約を弱めない
- Shader は First-party HLSL を Windows SDK の Toolchain で Build 時に Compile し、Bytecode を最終 Binary へ組み込む。製品起動時の `d3dcompiler` DLL Load、Shader Source File、外部 Shader Compiler Package を追加しない

この固定 Pass は M24 の Cube 描画だけを対象とする。汎用 Material、Shader、Resource、
Command Encoder API を先取りせず、将来の Renderer Backend へ拡張するときに別 ADR で見直す。

## Alternatives

| Option | 利点 | M24 での代償 | Decision |
| --- | --- | --- | --- |
| 既存 Presentation の所有域へ固定 Scene Pass を追加 | Fence、Barrier、Resize、Device Removal の正本を一箇所に維持 | RHI に M24 限定の描画語彙が入る | 採用 |
| `Cue.Renderer.D3D12` へ Native Frame Recording Lease を貸す | GPU Pass の層が明確 | Device／Command List／RTV／Fence の借用・失効 API と例外時回収を新設する | M24 では延期 |
| 汎用 RHI Resource／Command Encoder を先に導入 | 後続描画へ拡張しやすい | Buffer、Texture、Pipeline、Descriptor、同期を要件前に固定する | 採用しない |
| ToolHost の Cube 描画を Runtime へ複製 | 初期コード量が少ない | ImGui 所有と同期が分岐し、旧 Code の実質的な移植になる | 採用しない |

v1 の空 `components` の意味を変更する案は、既存 Package の互換契約を破るため採用しない。
Source `.cuescene` を Runtime で読む案は Authoring／Runtime 境界を破る。
Camera と Mesh だけを別の無 Version File へ重複保存する案も、Startup Scene の正本と
Manifest Inventory を分岐させるため採用しない。

Unity、Unreal Engine、Godot は Authoring と Player の分離、および描画抽出の比較対象にとどめる。
本 Decision は CueEngine の v1 Package 互換、既存 Fence 所有、単一 Cube の要件から決め、
外部 Engine の Source、Format、Backend API は採用しない。

## Verification

- v1 Empty Component Package の読取・起動回帰、v2 Camera／Cube の Writer／Reader／Canonical 再出版一致
- 未知 Type／Field／Version、Opaque Data、重複、順序違反、値不正、Tamper、Hash／Size 不一致の fail-closed
- Dynamic／Static の同一 Scene Snapshot と Main Camera 0／1／複数件の診断
- Hardware／WARP の Scene Pixel、Depth、裏面 Cull、Resize／Minimize／Restore、Frame Fence 再利用、InfoQueue／DRED
- Device Removal、Resize 失敗、Signal 失敗、GPU 完了未証明時の Resource 保持と Shutdown
- Shipping Product の Link Closure、PE Import／Loader Allowlist、Package File Inventory と Source 非参照
- Debug／Development／Release Build、全 CTest、`git diff --check`、実 Window で Cube 描画

## Deferred

Material、Lighting、Texture、Project Mesh、Asset Import／Cook、Sound、Effect、Physics、
ECS 改良、Scripting／Hot Reload、汎用 RHI Command API、Multi-thread Rendering は対象外。
