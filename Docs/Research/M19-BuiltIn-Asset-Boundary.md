# M19 Built-in Asset Boundary Research

- Date: 2026-09-21
- Issue: #366
- Milestone: #20
- Decision: ADR-0031

## Verified Starting Point

| Boundary | Current implementation | M19 gap |
| --- | --- | --- |
| Identity | Cubeは`cue://engine/mesh/cube`、Revision 1 | Namespace検証、Catalog、Kind別解決 |
| Geometry | `Cue.EngineAssets`の静的24頂点／36 Index、Position＋Normal | Plane／Sphere、共通DescriptorとGeometry Test |
| Authoring | EditorのCube生成がMesh ComponentへCube IDを保存 | Catalog駆動Primitive生成と未対応IDの抑止 |
| Runtime Data | Runtime Scene v2がCameraとCubeだけをCanonical保存 | v1／v2維持、複数Primitive用v3 |
| CPU Renderer | `RenderMesh::Cube`だけをSnapshotへ抽出 | Mesh Kind拡張と未知Kind Error |
| GPU Renderer | ToolHost／D3D12 Runtime Passが固定Cube Geometryを所有 | Plane／Sphere Resource、Depth／Cull／Fence回帰 |
| Product | Cube DataをStatic Libraryから最終ConsumerへLinkする | Built-in Reference Closure、未使用Payload除外Evidence |
| Asset Pipeline | 未実装 | M28でVirtual Asset／Cook Graphへ接続する移行条件 |

Cubeの既存ID、Geometry Revision、形状、Triangle集合、Bounds、Windingは将来のEngine Versionでも
変更しない。変更が必要なら旧IDを維持して新IDを追加する。IDは正確に2 SegmentのCanonical文法で
検証する。旧CueEngineや外部EngineはPrimitiveの機能比較に限り、Source、Mesh Data、Format、
生成Algorithmをコピー／移植しない。

## Selected Primitive and Resolution Model

```text
Authoring Create Primitive
  -> typed Built-in Catalog ID
  -> saved Mesh Component Asset Reference
  -> Runtime Scene v2 Cube or v3 Primitive
  -> validated Built-in Descriptor
  -> CPU RenderSnapshot Mesh Kind
  -> RHI Scene Frame Mesh Kind
  -> Backend-owned GPU Mesh Resource
```

M19の追加優先順位はPlane、Sphereとする。Quad等は利用要件が未確定のため追加しない。
未知ReferenceはRuntimeでCubeへ置換せず、Build／Packageを失敗させる。Editor向けError表示は
非永続の診断であり、Material／Texture IDをM19で予約しない。

## Runtime and Product Inclusion

- CubeだけのRuntime Scene v2は互換維持する
- Plane／Sphereを含む場合だけRuntime Scene v3を使用する
- Editor／開発Runtimeは全M19 Primitiveを利用できる
- Shipping ProductはScene Reference Closureから使用ID／Revisionを生成Build入力へ固定する
- Primitive Payloadを個別所有し、Catalog Metadataから全Payloadを常時強参照しない
- M28は同じID／Revision／ClosureをCook Graphへ移し、Project Meta Fileを作らない

## Implementation Issues to Create

1. #383 Built-in Mesh Catalog、Canonical ID、Typed Resolution、Cube回帰を実装する
2. #384 Plane／Sphere Revision 1 Geometryと完全なTopology Testを実装する
3. #385 Editor Create Primitive、Save／Reload／Undo／RedoをCatalogへ接続する
4. #386 Runtime Scene v3、Renderer Snapshot、RHI／D3D12 Primitive描画を実装する
5. #387 Shipping Built-in Reference Closure、未使用Payload除外、M28移行Metadataを実装する
6. #388 M19 Completion GateでEditor／Dynamic／Static、Hardware／WARP、3構成を検証する

各IssueはSまたはMに限定する。永続形式とGPU Resource所有を同じIssueで暗黙変更せず、
Runtime Scene v3とRHI Primitive入力は先行Issueの契約に従って段階的に接続する。

## Risks to Verify

- 既存Cube SceneとRuntime Scene v2のByte互換を壊さない
- 公開済みStable IDの形状、Winding、Bounds、Revisionを変更しない
- Project AssetがEngine予約NamespaceをOverrideしない
- Editorの診断FallbackをSceneまたはPackageへ保存しない
- Catalog列挙がShipping Productの全Payload強参照にならない
- M28移行時にStable IDをFile Pathへ置換しない

## Out of Scope

Material、Texture、Lighting、Particle、Physics、UV／Tangent、Project Asset Import、
Cooker本体、Runtime Bundle Format、Streaming、Compression、Patch。
