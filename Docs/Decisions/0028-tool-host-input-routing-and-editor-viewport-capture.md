# ADR-0028: Tool Host Input Routing and Editor Viewport Capture

## Status

Accepted

## Context

M14は、Platform非依存のKeyboard／Mouse Event、固定容量FIFO、Frame Snapshotを`Cue.Input`へ、Win32 Message変換を`Cue.Input.Windows`へ実装した。`Cue.Input`は標準Libraryだけへ依存し、`Cue.Input.Windows`は`Cue.Input`と`Cue.Platform.Windows`だけへ依存する。

一方、Windows D3D12 Tool HostはWin32 MessageをDear ImGui Backendへ直接渡しており、Editor ToolへPortable Inputを通知していない。そのため、DebugView Camera操作とPlay Runtime入力を追加するには、既存Input抽象層を重複させず、Tool Host、ImGui、Editor Viewport、Runtime Session間の所有権とFrame順を決める必要がある。

## Decision

### Existing Input Module Is Authoritative

新しいInput Moduleは追加しない。次を正本として再利用する。

- `Cue.Input`: `InputEvent`、`InputEventQueue`、`InputState`、`FrameInputSnapshot`
- `Cue.Input.Windows`: `WindowsInputMessageSink`
- `Cue.Platform.Windows`: Windowと単一`WindowsMessageSink`接続境界

依存方向は次に限定する。

```text
Cue.Input.Windows ------> Cue.Input
         |
         +--------------> Cue.Platform.Windows

Cue.ToolHost.WindowsD3D12 --> Cue.Input.Windows / Cue.Input
Cue.EditorTool -------------> Cue.Input
Cue.Runtime ----------------> Cue.Input
```

`Cue.Input`はPlatform、Windows、Editor、Renderer、RHI、D3D12、ImGuiへ依存しない。

### Tool Host Ownership

一つのWindows D3D12 Tool Host Sessionは、同じWindow Owner Thread上で次を所有する。

- 一つの`InputEventQueue`
- 一つのEditor Host用`InputState`
- 一つの`WindowsInputMessageSink`
- 一Frame分のPortable `InputEvent`借用Viewを保持する固定容量Storage

`WindowsInputMessageSink`はDear ImGui Win32 Message Sinkをdownstreamに持つDecoratorとする。認識したNative Messageは、先にPortable EventとしてFIFOへ格納し、その後ImGui Backendへ転送する。Window Lifecycle Eventは従来どおり`Cue.Platform.Windows`が所有し、Input Eventを`WindowEvent`へ追加しない。

Tool HostはQueue Overflowを独自に解釈せず、M14の`DeviceReset`挿入契約をそのまま使用する。

### Tool Host Input Frame View

Tool Hostの公開CallbackへWin32型を追加しない。Tool Clientは一Frameにつき一回、次を借用Viewとして受け取る。

- 全Portable EventのFIFO順View
- 全Event適用後の`FrameInputSnapshot`
- Dear ImGuiが現在Frameで要求するKeyboard／Mouse Capture値

Event ViewとSnapshot参照は、次回Input Frame通知またはTool Host終了までだけ有効とする。Clientは参照やSpanを保持せず、次Frame以降に必要な値だけをCopyする。

Tool Host用`InputState`にはImGui Captureを適用しない。これはEditor Composition RootがDebugView、GameView、一般Tool UI、Play Runtimeの優先順位を決めるために、Capture前のPortable状態を必要とするためである。ImGui Capture値は別のRouting入力として通知する。

### Frame Order

一Frameの順序を次に固定する。

1. Windows Message Pumpを実行する
2. `WindowsInputMessageSink`がPortable FIFOへ格納し、同じMessageをImGui Backendへ転送する
3. ImGui Backendの`NewFrame`と`ImGui::NewFrame`を実行する
4. Tool Host用`InputState::begin_frame({})`を実行する
5. FIFOを一度だけDrainし、Event Viewへ保持しながら`InputState`へ順番どおり適用する
6. ImGuiの`WantCaptureKeyboard`／`WantCaptureMouse`をPortable `InputCapture`へ変換する
7. Tool ClientへInput Frame Viewを通知する
8. Tool ClientがEditor UIを構築し、Viewport CaptureとRuntime Routingを決定する
9. Scene SurfaceとImGuiを描画してPresentする

最小化中もMessage Pumpは継続する。描画Frameを提出しない期間のEventは次に提出するInput Frameで処理するが、`FocusLost`、`DeviceReset`、Queue Overflowにより押下状態を安全に解放する。

### Editor Routing Priority

Editor Composition Rootは、Portable EventとCapture情報を次の優先順位で解釈する。

1. `FocusLost`と`DeviceReset`はCaptureに関係なくEditor状態とRunning Runtime Sessionの双方へ配送する
2. Modal、Text Input、一般Tool UIが占有するKeyboard／MouseはPlay Runtimeへ配送しない
3. 操作中のDebugViewはMouseを占有し、Editor専用DebugCameraだけを更新する
4. 操作中のGameViewはGame入力としてPlay Runtimeへ配送できる
5. どのViewportも占有しない未Capture入力だけをPlay Runtimeへ配送する

GameView内の描画領域がHoverまたは操作中なら、一般UI向けのMouse CaptureよりGameViewを優先する。GameView WindowがFocusされている間は一般UI向けKeyboard CaptureよりGameViewを優先する。ただしModalとText Inputは常に優先してRuntime入力を遮断する。DebugViewのMouse占有はGameViewより優先する。Focus復帰に必要な`FocusGained`も、Focus喪失とResetと同様にCaptureに関係なく配送する。

Play開始FrameにTool Hostへ到着していたEventは、新しく生成したRuntime Sessionへ引き継がない。Play停止中のEventはFrame末尾で破棄し、再開時にはSession-local Input Stateを新規生成する。Runtime Frameには配送Eventだけでなく合成した`InputCapture`も渡し、UIへ占有が移ったときに既存押下を解放する。

M23の最初の実装はTool HostからPortable Input Frameを通知するところまでとする。DebugCamera更新とPlay Runtime Routingは別Issueで追加する。

### Debug Camera Input

DebugCameraはEditor Sessionが所有し、Sceneへ保存しない。初期操作は、DebugViewがHoverまたは操作中の場合だけ、Portable Mouse状態から次を行う。

- Right Mouse Drag: Yaw／Pitch
- Middle Mouse Drag: Pan
- Mouse Wheel: Dolly

Focus喪失、View非表示、対応Button解放時は操作状態を解除する。Pitch Clampと有限値検査を行い、NaNまたはInfをCamera Poseへ保存しない。

Raw Input、OS Cursor Lock、無制限Mouse Lookはこの契約へ含めない。初期実装はClient Area内のPortable Mouse位置とFrame Deltaを使用する。

### Thread and Lifetime Contract

Native Message変換、Queue Drain、`InputState`更新、Tool Client通知、Viewport RoutingはWindow Owner Threadだけで実行する。別ThreadへEvent View、Snapshot参照、ImGui Capture値の参照を渡さない。

Play RuntimeはSession-localなInput QueueとInput Stateを引き続き所有する。EditorはPortable値だけをRuntimeへCopyし、Tool HostまたはImGui ObjectのPointerをRuntimeへ渡さない。

## Rejected Alternatives

### Add Another Editor Input Module

M14のPortable Event、State、Windows Adapterと責務が重複し、同じKey変換とFocus Reset契約が分岐するため採用しない。

### Read ImGuiIO Directly in Renderer or DebugCamera

RendererをImGuiへ依存させ、Headless Testと将来Platform Backendを妨げるため採用しない。

### Use GetAsyncKeyState or Native Mouse APIs in EditorTool

Message順、Focus、Capture、Test入力が分離し、Platform固有型がComposition境界を越えるため採用しない。

### Apply ImGui Capture Inside Tool Host InputState

一般UIとGameView／DebugViewの意味をTool Hostが判断できず、Editorが必要とする未Capture状態を失うため採用しない。

### Introduce Raw Input Before Viewport Routing

Relative InputとCursor Policyを先に固定し、最小のDebugCamera操作よりScopeが拡大するため採用しない。

## Consequences

- 既存`Cue.Input`をEditor Toolでも再利用し、RuntimeとEditorでPortable Key／Mouse契約を共有できる
- Tool Hostは`Cue.Input.Windows`へ依存するが、公開APIにWin32型を追加しない
- EditorはImGui CaptureとViewport Captureを一箇所で判断できる
- 一Frame分のEventを固定容量Storageへ一度CopyするCostが増える
- 初期Camera操作はWindow境界で移動量が途切れ、Raw InputやCursor Lockは後続Issueが必要になる
- Gamepad、IME、Text Input、Input Mapping Asset、Rebinding UIは未対応のまま残る

## Verification

- Tool Host Public Header単体Compile
- CMake Target Link入力検査
- Win32 Keyboard／Mouse／Focus MessageがPortable EventとImGui downstreamへ同順で届くTest
- 一FrameごとのPressed／Released／Repeat／Mouse Delta Reset Test
- Focus喪失とQueue OverflowのDevice Reset Test
- DebugView Capture中にPlay RuntimeへMouse入力が届かないTest
- Debug／Development／Release BuildとCTest
- 実WindowでDebugView Camera操作、Focus喪失、Play中Routingを確認する
