# M24 Standalone Scene Boundary Research

- Date: 2026-09-21
- Issue: #351
- Milestone: #25
- Decision: ADR-0029

## Verified Starting Point

| Boundary | Current implementation | M24 gap |
| --- | --- | --- |
| Package Writer | `Engine/Source/Package/Private/RuntimeData.cpp` の `make_scene_data` は Component 非空を拒否 | Camera／Mesh を含む Runtime Scene Data v2 |
| Package Reader | `Engine/Source/RuntimeHost/RuntimePackage.cpp` の `parse_runtime_scene` は空 `components` だけ受理し、同じ Publisher で Canonical 再検証 | v1 維持と v2 の厳格な解析・再出版 |
| Runtime Snapshot | `Engine/Source/Scene/Public/Cue/Scene/Instantiation.h` の `RuntimeSceneObjectData` は Core Transform まで | 検証済み Component の所有 |
| Runtime Composition | `Engine/Source/RuntimeHost/GameModuleQueryProvider.cpp` は Core Schema のみ Seal | Renderer Schema／System／Builder の登録 |
| CPU Render Data | `Engine/Source/Renderer/Private/RendererRuntimeSystem.cpp` は Editor で利用する `RenderSnapshot` を生成 | RuntimeHost Application が同じ Snapshot 契約を保持・公開 |
| GPU Presentation | `Engine/Source/RHI/Public/Cue/RHI/PresentationContext.h` は Clear Color のみ。D3D12 Context が Frame／Fence／Resize を所有 | Scene Frame を既存 Submit／Present／Signal 経路へ組み込む |
| Shipping | `Engine/Source/ShippingProduct/CMakeLists.txt` は Renderer／EngineAssets を Link Closure に含めない | Static Product と Dynamic Host の同一描画機能 |

Editor の Cube GPU 描画は `ToolHost.cpp` に閉じている。RHI の Resource 所有者を
増やすためにこれを移植せず、ADR-0029 の最小 Pass を First-party Code として新規実装する。

## Data and Lifetime Flow

```text
Saved Authoring Scene
  -> immutable SceneSnapshot
  -> versioned Canonical Runtime Scene Data
  -> Package Inventory / Trust verification
  -> verified Runtime SceneSnapshot
  -> Runtime World + Renderer System
  -> owned CPU RenderSnapshot
  -> borrowed Scene Frame values
  -> RHI D3D12 Presentation owner
  -> Back Buffer / GPU Fence / Present
```

Dynamic と Static は Package 検証、Runtime Data、Runtime Application、CPU Snapshot、
GPU Presentation を共有し、Game Module Query Provider と Trust Policy だけを分ける。
Reader は Package の外へ Source Asset を探しに行かない。
Package Reader は Game Module 接続より前に一時的な Core＋Renderer Registry を作って
v2 Component を検証する。検証済み Snapshot は Registry Pointer を保持しない。

## Implementation Issues to Create

1. Runtime Scene Data v2 の Camera／Mesh 限定 Writer、Reader、Runtime Snapshot、v1 互換と改ざん拒否 Test
2. RuntimeHost の Renderer Schema／System／Builder、`RenderSnapshotStore`、Dynamic／Static Composition
3. RHI D3D12 の最小 Scene Frame 入力と Build 時 Shader、Cube GPU Resource、基本描画 Test
4. RHI D3D12 Scene Pass の Depth／裏面 Cull、Resize／Removal／Fence 寿命と Fault Test
5. RuntimeHost の Snapshot→Scene Frame 変換と Startup Scene 描画 Loop
6. Dynamic／Static Package の Hardware／WARP Process Test、Shipping Trust／Import 回帰
7. M24 Completion Gate：3 構成 Build／CTest、実 Window 確認、未実行検証と Risk の Evidence

各 Issue は前段の成果に依存する。特に Scene Data v2 を Reader の受理拡大だけで
先に有効化せず、Publisher、Canonical 再検証、Trust Test を同じ Issue で閉じる。

## Risks to Verify

- Runtime Scene Data の意味を Manifest の Hash 成功だけで保証しない。Schema と Canonical
  再検証、Type／Field／Version allowlist を別に行う
- GPU 完了を証明できない Frame／Resize／Shutdown で Back Buffer、Depth、Cube Buffer、
  Constant を早期解放しない
- D3D12 Pass の新しい Load-time DLL を Shipping Product へ持ち込まない
- v1 Empty Scene の起動と固定色 Clear Smoke を維持する
- Editor の Main Camera 選択と Standalone の選択条件を分岐させない
- 実 Window の見た目だけで Depth／Cull／WARP／Dynamic／Static の Gate を代用しない

## Out of Scope

一般 Asset Database、Project Mesh、Material、Lighting、Shader Graph、汎用 RHI、
Sound、Effect、Physics、ECS 改良、Scripting／Hot Reload。
