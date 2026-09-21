# M24 Standalone Runtime Minimal Scene Rendering Completion Gate

## Result

2026-09-21 時点で、M24 の先行 Issue #351、#353、#354、#355、#356、#357、#358、#363、#364 は Closed。
ADR-0029 の Runtime Scene Data v1 互換、Camera／Built-in Cube 限定の v2、Renderer Snapshot、
Dynamic／Static Package Trust、D3D12 固定 Scene Pass の所有権を統合検証した。

| Acceptance Gate | Result | Evidence |
| --- | --- | --- |
| 先行 Issue と ADR の一致 | Pass | #351、#353～#358、#363、#364 は Closed。ADR-0029 の Accepted 契約と実装 Issue の Scope を照合 |
| Runtime Scene Data／Trust | Pass | v1 Empty Component、v2 Canonical Camera／Cube、未知 Type／Field／Version、重複、順序、Tamper、Hash／Size、Dynamic／Static Trust を `Cue.Package.*`、`Cue.RuntimeHost.*` で検証 |
| Renderer／RuntimeHost 接続 | Pass | Runtime World から所有値 `RenderSnapshot` を抽出し、Main Camera 0／1／複数件と Cube Count を Process Test で確認 |
| Hardware／WARP Scene Pass | Pass | Cube Scene Pixel、D32 Depth、Depth Resize、Back-face Cull、Swap Chain Resize Pixel を Hardware／WARP の両方で実行 |
| Dynamic／Static Package E2E | Pass（Test Product） | Dynamic `CueRuntimeHost.exe` とRuntimeHost本体をStatic LinkしたRelease Test Productが同じv2 Startup Sceneを読込み、WARP CTestとHardware手動ProbeでScene Pixelを検証 |
| Shipping Link Closure | Pass（Scene E2Eは限定） | 生成Project ProductのImport Allowlist、Game DLL／Editor／ImGui／Shader Compiler DLL不在は既存Release Process Testで検証。実生成Productのv2 Scene Pixel E2Eは未実行 |
| 実 Window の起動と終了 | Pass（自動） | RuntimeHost が実 Win32 Window を `show()` 後、Sceneを描画。Render Smokeは300 Frame、Package SmokeはWindow Close Event、Scene Pixel ProbeはReadback成功と正常終了を検証 |
| 3 構成 Build／CTest | Pass | Debug 298/298、Development 298/298、Release 293成功・既定5 Skip・失敗0。全Target Build成功 |
| 差分検査／Risk記録 | Pass | `git diff --check` 成功。未実行検証と残るRiskを下記へ記録 |

## Verified Tree and Commands

- Verification base: `f325eaf78a2597f154d7971d998c4bf5127997ab`
- Branch: `codex/issue-359-m24-completion-gate`
- `cmake --build --preset windows-vs2026-debug --parallel 4`
- `ctest --preset windows-vs2026-debug --output-on-failure`
- `cmake --build --preset windows-vs2026-development --parallel 4`
- `ctest --preset windows-vs2026-development --output-on-failure`
- `ctest --preset windows-vs2026-development -R "^Cue.Editor.Workflow.ProcessRoundTrip$" --output-on-failure`
- `ctest --preset windows-vs2026-development --output-on-failure`
- `cmake --build --preset windows-vs2026-release --parallel 4`
- `ctest --preset windows-vs2026-release --output-on-failure`
- Dynamic Release Packageの `CueRuntimeHost.exe --package-scene-smoke-test hardware`
- Static-link Release Test Productの `CueGameProduct.exe --package-scene-smoke-test hardware`
- `git diff --check`

Repository 指定の `scripts/codex_build.ps1` はこの Checkout に存在しないため、正式な CMake Preset を直接使用した。
NuGet restore と第三者 Library の追加／更新は行っていない。

### Manual Hardware Package Probe Preparation

Hardware Package Probeは、Release CTestが生成する無視対象の`out`配下のFixtureだけを使用した。
`Cue.RuntimeHost.Package.Process`は終了時にDynamic Sceneをv1 Empty Sceneへ戻し、
`Cue.RuntimeHost.StaticScenePackage.Process`は改ざん拒否の確認としてStatic Scene末尾へ`x`を追加する。
そのため、次の手順で両FixtureをCTest時と同じCanonical v2 Camera／Cube Sceneへ復元してから実行した。

1. Repository Rootから次を実行し、Release Fixtureを作り直した。
   `ctest --preset windows-vs2026-release -R "^Cue.RuntimeHost.(Package|StaticScenePackage).Process$" --output-on-failure`
2. Static Scene
   `out/build/windows-vs2026/Engine/Tests/RuntimeHost/RuntimeStaticScenePackage/Release/RelocatedStaticPackage/Data/Scenes/51234567-89ab-4cde-8f01-23456789abcd.cueruntime.json`
   の末尾1 byte `x`だけを除去した。復元後は853 byte、SHA-256
   `1583c29fd070c0e1da10579479af1f2caefa201f73333e61bb39881f8b0d4c9d`となり、同Directoryの
   `CuePackage.json`にCTestが記録した`startupSceneRuntimeData`のSize／Hashと一致することを確認した。
3. 復元したStatic SceneをDynamic Scene
   `out/build/windows-vs2026/Engine/Tests/RuntimeHost/RuntimePackageProcess/Release/RelocatedPackage/Data/Scenes/51234567-89ab-4cde-8f01-23456789abcd.cueruntime.json`
   へCopyした。Dynamic `CuePackage.json`の`startupSceneRuntimeData`を853 byteと上記SHA-256へ更新し、
   実行前に実Fileと一致することを確認した。
4. Dynamic Probeは
   `out/build/windows-vs2026/Engine/Tests/RuntimeHost/RuntimePackageProcess/Release/UnrelatedWorkingDirectory`
   をWorking Directoryとして、
   `../RelocatedPackage/CueRuntimeHost.exe --package-scene-smoke-test hardware`を実行した。
5. Static Probeは
   `out/build/windows-vs2026/Engine/Tests/RuntimeHost/RuntimeStaticScenePackage/Release/UnrelatedWorkingDirectory`
   をWorking Directoryとして、
   `../RelocatedStaticPackage/CueGameProduct.exe --package-scene-smoke-test hardware`を実行した。

両ProbeはNVIDIA GeForce RTX 3060でExit Code 0となり、`MainCamera=Ready, MeshCount=1`、
`Mode=Scene, CubeCount=1`、`Runtime Package Scene Pixel Probe: Passed`、`WindowClosed FrameCount=1`を出力した。
Fixtureの復元は`out`配下だけへ行い、Source、Test、配布物定義には変更を加えていない。

## Local Validation Results

- Debug: 全Target Build成功、298/298 Test成功、244.24秒
- Development: 全Target Build成功、298/298 Test成功、112.09秒
- Release: 全Target Build成功、293 Test成功、既定5 Test Skip、失敗0、255.50秒
- Hardware Adapter: Scene Pixel、Depth、Depth Resize、Back-face Cull、Swap Chain Resize Pixel、RuntimeHost Device／Presentation／Render／Resize Smoke成功
- WARP: 同じScene／RuntimeHost検証に成功
- Dynamic Package: v1 Diagnostic Clear、v2 Main Camera／Cube Scene、Main Camera Missing／Multiple診断、WARP／Hardware Scene Pixel Readback成功
- Static-link Test Package: Release Test ProductのWARP／Hardware v2 Scene Pixel Readback、Tamper拒否、Import Allowlist成功
- 生成Project Shipping Product: Build／起動、Link／Import Closureを既存E2Eで確認。v2 Scene Pixelは未実行
- 実Window: Rendering SmokeとPackage Scene SmokeがProductionのWindow作成／表示／描画／停止経路を実行

ReleaseでSkipされた既存Testは、Release構成でDebug Layer／InfoQueue／DRED Test Supportを無効にする条件による。

- `Cue.RHI.D3D12.FrameCommand.InfoQueue300`
- `Cue.RHI.D3D12.RtvHeap.InfoQueue`
- `Cue.RHI.D3D12.SwapChain.InfoQueue`
- `Cue.RHI.D3D12.SwapChain.SceneDirectPresentDeviceRemoved`
- `Cue.RHI.D3D12.SwapChain.DeviceRemovalDredFailure`

## Window and Pixel Verification Boundary

RuntimeHost Process Testは非Nativeな模擬Windowではなく、`Cue.Platform.Windows`の実Windowを作成して表示する。
`--render-smoke`はHardware／WARPで300 Frameを同じPresent経路へ投入する。Dynamic／Static Packageの
`--package-scene-smoke-test`はMain CameraとCubeを含むDynamic PackageとStatic-link Test Packageを
WARP／Hardwareで起動し、最終Back BufferからClear角画素とScene画素をReadbackして、Clearだけではないことを
検証する。Package Smokeは実Windowへ
Close Eventを発行し、`WindowClosed`としてRuntime Application、Presentation、Backendを停止する。

人間による今回の目視確認は行っていない。自動GateはWindow表示APIの成功、実Swap Chain描画、Pixel Readback、
Close Event、正常終了を一続きで検証する。

## Scope Audit

- Material、Lighting、Texture、Project Mesh、Asset Import／Cookを追加していない。
- Sound、Effect、Physics、ECS改良、Scripting／Hot Reloadを追加していない。
- 汎用RHI Resource／Command Encoder、Multi-thread Renderingを先取りしていない。
- RendererからEditor／ImGuiへの依存、Runtime World PointerのGPU側保持を追加していない。
- 新規第三者LibraryまたはVersion更新を行っていない。
- 旧CueEngine、外部Engine、SampleからSource Codeをコピー、移植、部分抽出していない。

## Existing Problems and Remaining Risks

- Development全CTestの最初の昇格実行で、`Cue.Editor.Workflow.ProcessRoundTrip`がProject Directory Renameの
  `Access denied`により1回失敗した。単体再実行と全CTest再実行では成功し、Debug／Releaseでも再現しなかった。
  Scene描画を呼ばない既存Workflow経路だが、外部ProcessまたはFile Handleの一時保持原因は未特定である。
- Sandbox内の非昇格Development実行ではWindows SDK Registry／Temp DirectoryへのAccess Deniedが発生した。
  同一Treeの昇格実行は全件成功しており、製品Failureではなく検証環境制約として扱う。
- Hardware検証は現在の一台のWindows x64／D3D12 Adapterに限定される。別GPU Vendor、Driver、Windows Build、
  Multi-monitor／DPI、Remote Desktop、長時間Soakは未確認。
- 実Window経路は自動Pixel Gateで検証したが、人間による見た目、入力操作、Alt+Tab、最小化中の長時間動作は未確認。
- Static Scene E2EのExecutableはRuntimeHost CoreをStatic LinkしたTest Productであり、Project Generatorの
  `cue_add_shipping_product`と`BUILD_TESTING=OFF`から生成した実Productではない。実生成Productは既存E2Eで
  Build／起動／Link Closureを確認したが、v2 Camera／Cube PackageとPixel Readbackは未実行である。
- Device Removal、Signal／Resize失敗、GPU完了未証明時のResource保持はSynthetic Fault Injectionで検証した。
  PIX／DREDを伴う実GPU Hang、TDR、Driver Reset Captureは実施しておらず、実障害時の診断完全性は未確認である。
- ReleaseでSkipされたDebug Layer／InfoQueue／DRED経路はDebug／Developmentで実行済みだが、Release Binaryでは未実行。
- M24固定PassはBuilt-in Cube専用であり、Material、Texture、任意Mesh、Lightingの性能や拡張性を保証しない。

## Next Action

M24 Close後は、現在のRoadmap順にM25 Scripting FoundationのResearchから開始し、ABI、所有権、寿命、
Reload失敗時のRollback、Editor／Runtime境界を実装前に固定する。
