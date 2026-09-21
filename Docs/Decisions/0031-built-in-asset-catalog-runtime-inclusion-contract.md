# ADR-0031: Built-in Asset Catalog, Revision, and Runtime Inclusion Contract

- Status: Accepted
- Date: 2026-09-21
- Decision Owners: CueEngine Project
- Relates to: ADR-0011、ADR-0013、ADR-0026、ADR-0029
- Supersedes: ADR-0026のCube以外を延期した範囲と、ADR-0029のRuntime Mesh Allowlistを拡張する場合の規則

## Context

`Cue.EngineAssets`は`cue://engine/mesh/cube`の不変CPU Geometryを所有し、EditorのCreate Cube、
Renderer Snapshot、Runtime Scene v2、D3D12固定Scene Passが同じIdentityを使用している。
一方、Built-in Assetを列挙・解決するCatalog、Cube以外のPrimitive、Project AssetとのNamespace分離、
既定値／解決失敗時の扱い、未使用Built-in Assetの製品収録規則は未決定である。

M28ではAsset Database、Import、Cook、Runtime Bundleを導入する予定だが、Built-in Assetは
Source Fileを持たず、Engine Versionとともに提供される。M19でFile Pathベースの仮Identityや
暗黙Fallbackを追加すると、M28で同一性、Cook Closure、製品Inventoryを移行できない。

## Decision

### Reserved Namespace and Typed Identity

- `cue://engine/`をEngine所有Assetの予約Namespaceとする
- IDはlowercase ASCIIの`cue://engine/<kind>/<name>`とし、Prefixに続くSegmentを`kind`と
  `name`の正確に2個へ固定する。各Segmentは1～64文字、先頭を`[a-z]`、2文字目以降を
  `[a-z0-9-]`とし、末尾`-`を許可しない。空Segment、追加Slash、Backslash、Colon、空白、
  制御文字、Query、Fragment、`.`、`..`、Percent Encoding、末尾Slash、Unicode別表現を許可しない
- Project Asset、Import済みAsset、Pluginは`cue://engine/`を発行またはOverrideできない
- Asset IDはFile Pathではなく、Engine Version内で安定した意味を表す
- 一つの万能Asset基底型を導入せず、`BuiltInMeshDescriptor`等のKind別の値型と解決APIを使用する
- DescriptorはStable ID、Kind、Geometry Revision、Display Name、Bounds、Capabilityを所有し、
  Pointer所有権やRenderer／D3D12型を公開しない
- Geometry ViewはProcess寿命の不変Storageを借用し、Allocationなし、Thread-safe Read-onlyとする

不正ID、未知ID、Kind不一致、無効Revisionは区別できる`Cue.EngineAssets` Errorとして返す。
未知IDをCube、空Mesh、先頭Assetへ黙って置換しない。

### Primitive Set and Priority

M19の実装順は次のとおりとする。

1. Cube: 既存`cue://engine/mesh/cube`、Geometry Revision 1を変更しない
2. Plane: `cue://engine/mesh/plane`、Geometry Revision 1
3. Sphere: `cue://engine/mesh/sphere`、Geometry Revision 1

Quad、Capsule、Cylinder、Cone、Grid、Camera／Light Icon、Collider表示は、実際のEditor／Physics／
UI要件が決まるまで追加しない。

Plane Revision 1は原点中心、一辺1.0のXZ平面、Y=0、Normal +Y、BoundsはX／Zが
`[-0.5, 0.5]`、Yが0、4頂点、6個の16-bit Index、+Y側から見て反時計回りとする。

Sphere Revision 1は原点中心、半径0.5、16 Slice、8 StackのUV Sphere topologyとし、
極を共有、各中間Ringは16頂点、合計114頂点、672個の16-bit Indexを持つ。Positionを正規化した
方向をSmooth Normalとし、SeamとPoleのTriangle順をRevision契約としてTestする。

Revision 1のVertexはPositionとNormalだけを持つ。UV、Tangent、Material Slotを暗黙追加しない。
必要になった場合は新Revisionまたは新しいCook済みRuntime Representationを別ADRで決定する。

### Revision and Compatibility

- 公開済みStable IDが表すGeometryの形状、頂点Attribute、Triangle集合、Bounds、Windingと
  Geometry RevisionはEngine VersionやCompatibility範囲を越えて変更しない。変更が必要な場合は
  `cube-v2`等の新しいStable IDを追加し、旧IDとPayloadを互換読込みのため維持する
- Geometry RevisionはAsset Referenceへ毎回保存しない。Stable IDとRevisionの対応を不変にすることで、
  既存Authoring SceneのIDだけから常に同じRevisionを解決できる。Runtime PackageのEngine Versionと
  Schema Versionは利用可能なID集合と実装互換を追加で固定する
- 頂点配列、Face配列、Index配列の順序は、同じPosition／Normalを持つ頂点と同じ外向きTriangle集合を
  保つ限りRevision内の実装詳細とする。Index列のByte一致を永続形式またはABIとして要求しない
- M28 Asset DatabaseはBuilt-in DescriptorをRead-only Virtual Assetとして同じIDで列挙し、
  Project Meta FileやImport Sourceを生成しない
- Asset Database Cacheを削除してもBuilt-in IdentityとRevisionは変化しない

### Authoring, Runtime Data, and Rendering

- EditorのCreate PrimitiveはCatalogから明示選択したIDをMesh Componentへ保存する
- PrimitiveをAuthoring Sceneへ生成する機能は、保存、Runtime Data、Renderer、Standaloneが同じIDを
  解決できる段階までUIへ公開しない
- Cubeだけを許すRuntime Scene v2は変更しない
- Plane／Sphereを含むSceneはRuntime Scene v3として公開し、v1／v2 ReaderとCanonical Writerを維持する
- v3のMesh Component構造はv2と同じStable Asset Reference Fieldを使用し、Allowlistだけを
  M19 Catalogへ拡張する。未知ID、Kind不一致、未対応RevisionはGame Module接続前に拒否する
- CPU `RenderSnapshot`とRHI Scene FrameはMesh Kindを明示し、未知Kindを黙ってSkipしない
- D3D12 BackendがGPU Mesh ResourceとFence寿命を所有し、Editor ToolHostとRuntimeHostが
  Native Resourceを共有しない

### Default and Error Resources

- Mesh Componentに暗黙のDefault Meshを設定しない。Primitive生成操作が選択したIDを明示保存する
- Asset Reference未設定は「描画対象なし」、未知／破損Referenceは「解決Error」として区別する
- Build、Cook、Runtime Packageは未知AssetをError Resourceへ置換せずFail-closedとする
- Editorは将来、保存されない診断表示としてError Resourceを使用できるが、そのIDをSceneやPackageへ
  永続化しない。現在Material／Texture契約がないため、Magenta MaterialやChecker TextureのStable IDを
  M19で先取りしない
- Default Material、White／Black Texture、Normal Texture、Missing TextureはMaterial／Texture要件の
  ResearchでStable ID、Color Space、Format、Cook規則を決めてから追加する

### Product Inclusion and M28 Migration

- Editor／Project Hub向けDeveloper Toolは、Authoring用Built-in Catalogと全M19 Primitiveを含める
- Dynamic開発RuntimeはIterationのため対応Catalogを含めてよい
- Release Shipping ProductはStartup Sceneと到達可能Runtime DataからBuilt-in Asset Reference Closureを作り、
  使用されたID／RevisionだけをProduct Build入力へ渡す
- EngineAssetsのPayloadはPrimitiveごとに分離し、Shipping Productの生成Selectionが参照しないPayloadを
  Link ClosureまたはRuntime Bundleへ含めない。単一の全Asset Registryから全Payloadを強参照しない
- Product Metadata／EvidenceへBuilt-in ID、Revision、Storage Kindを記録し、Scene Referenceとの一致を検証する
- M28 Cooker導入後は、同じReference ClosureをCook GraphのEngine-owned Source Nodeとして表現する。
  Stable ID、Geometry Revision、Scene Dataの意味を変更せず、Compiled PayloadからRuntime Bundleへ移行できる
- Built-in Assetは再生成可能なEngine Payloadであり、Project Source、Import Source、User編集対象にしない

M19時点でLoose Asset Fileは追加しない。Compiled PayloadからBundleへ移す判断は、M28のBundle Format、
Compression、Streaming、Patch単位、Platform別Cook要件と同時に行う。

## Alternatives

| Option | 利点 | 代償 | Decision |
| --- | --- | --- | --- |
| Typed Built-in Catalog | Project Assetと分離しつつ列挙・解決できる | KindごとのAPIが必要 | 採用 |
| Project AssetsへPrimitive FileをCopy | 一般Assetと同じように編集できる | Identity重複、Project汚染、更新競合が生じる | 不採用 |
| 未知MeshをCubeへFallback | Sceneを表示し続けられる | Build／Runtimeの破損を隠す | 不採用 |
| 全Built-in Payloadを全製品へ収録 | Buildが単純 | 未使用Assetと将来のTexture等が製品を肥大化する | Editor／開発Runtimeだけ許可 |
| M19からLoose Runtime Bundle | M28へ近い | Bundle、Cook、Patch契約を先取りする | 不採用 |

## Verification

- Catalog IDのCanonical性、予約Namespace、重複、Kind不一致、未知IDをTestする
- Cube Revision 1の頂点数、Index数、Position／Normal、Triangle集合、Bounds／Windingを回帰固定する。
  Face順、頂点順、Index列のByte一致は固定しない
- Plane／Sphereの頂点数、Index数、範囲、非縮退、Bounds、Normal、Windingを検証する
- Editor Create／Save／Reload／Undo／RedoとRuntime Scene v1／v2互換、v3 Canonical Round-tripを検証する
- GameView、DebugView、Dynamic RuntimeHost、Static Productで各Primitiveを描画する
- Hardware／WARPの画素、Depth、裏面Cull、Resize、Device Removalを回帰確認する
- Shipping Productで使用PrimitiveだけがEvidenceに現れ、未使用PayloadとSource Assetが含まれないことを確認する
- Debug／Development／Release Build、全CTest、`git diff --check`を実行する

## Deferred

Material、Texture、UV／Tangent、Lighting、Particle、Physics Primitive、Asset Import／Cook、
Runtime Bundle Format、Streaming、Compression、Patch、Project Asset Database全体。
