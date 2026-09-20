# M23 Input Routing and Editor Camera Control Completion Gate

## Result

2026-09-21 時点で、M23 の先行 Issue #341、#342、#343、#345 は Closed。
本 Gate は入力抽象層を新設し直さず、既存の `Cue.Input` と `Cue.Input.Windows` を
ToolHost、DebugView、Play Runtime へ接続した範囲を検証する。

| Acceptance Gate | Result | Evidence |
| --- | --- | --- |
| 先行 Issue の Close | Pass | #341、#342、#343、#345 は Closed。#345 は PR #349 で `Rebuild` へ Merge |
| Portable Input の依存方向 | Pass | `Engine/Tests/Input/CMakeLists.txt` が `Cue.Input` の Target Link と Public Header の禁止依存を Configure 時に検査 |
| Win32 Adapter の分離 | Pass | `Cue.Input.Windows` が Win32 Message を Portable Event へ変換。`Cue.Input.Windows.MessageSink` と Public Header Test が 3 構成で成功 |
| Routing と ADR の一致 | Pass | ADR-0028 の FIFO、UI／Modal、DebugView、GameView、Play Session の優先順を `Cue.Editor.ImGui.PlayInputRouting`、`Cue.EditorCore.PlaySession` で検証 |
| Focus／Overflow／Capture 解除 | Pass | `Cue.Input.State`、`Cue.Input.Windows.MessageSink`、`Cue.Editor.ImGui.PlayInputRouting`、`Cue.EditorCore.PlaySession` が FocusLost、DeviceReset、Queue Overflow、押下解除、再 Play 時の古い入力破棄を検証 |
| 実 Window の Debug Camera 操作 | Pass（限定） | Debug 構成の ProjectHub から一時的な Project Copy を Editor で開き、DebugView 内の Mouse Wheel で Cube の表示が変化することを目視確認。Runtime 側 Panel 上の Scroll では Camera が変化しないことも確認 |
| 3 構成 Build | Pass | PR #349 の Windows CI run `35539440666` で Debug／Development／Release の Build and Test が全て成功 |
| 全 CTest／差分検査 | Pass | #345 の同一実装 Tree で Debug／Development は各 276 件成功。Release は 276 件登録中 272 件成功、既定 4 件 Skip、失敗 0。Release の初回並列実行では既存 `Cue.Platform.Windows.Process` が一度 Timeout したが、単体再実行と全件逐次再実行は成功。`git diff --check` 成功 |
| 未実行検証と Risk 記録 | Pass | 下記参照 |

## Verified Tree and Commands

- #345 実装 Commit: `7d1fb66c59fc1d40f14d1fe4bb36525654a97886`
- PR #349 Merge Commit: `91be49e301921a06325a4c8afa4927fc6ec08068`
- CI: [PR #349](https://github.com/tochouseito/CueEngine/pull/349)、[run 35539440666](https://github.com/tochouseito/CueEngine/actions/runs/35539440666)
- `cmake --preset windows-vs2026`
- `cmake --build --preset windows-vs2026-debug --parallel`
- `ctest --preset windows-vs2026-debug --output-on-failure`
- `cmake --build --preset windows-vs2026-development --parallel`
- `ctest --preset windows-vs2026-development --output-on-failure`
- `cmake --build --preset windows-vs2026-release --parallel`
- `ctest --preset windows-vs2026-release --output-on-failure`
- Release Timeout 後は対象 Test の単体再実行と全 CTest の逐次再実行を実施
- `git diff --check`

## Manual Window Check Boundary

実 Window 確認には既存 Project の `CueProject.json` と Default Scene を一時 Directory へ
Copy した Project を使用した。元 Project の Scene は変更していない。Editor と ProjectHub は
確認後に終了した。一時 Project と ProjectHub の最近使った Project 登録は保持している。

## Not Run and Remaining Risks

- 実 Window では Mouse Wheel の Dolly のみ確認した。Right Drag の Yaw／Pitch、Middle Drag の Pan、Focus 喪失、Play 中の GameView 入力は自動 Test 対象だが実 Window では未確認。
- Release の初回並列 CTest で既存の Platform Process Test が Timeout した。再現性と同時実行負荷の原因は未確定で、逐次 Test と CI は成功した。
- Raw Input、Cursor Lock、Window 外の連続 Mouse Look、Gamepad、IME、Input Mapping、Rebinding、Keyboard Fly Camera、Multi-thread Input は M23 Scope 外。
- 長時間の Editor 操作、別 Machine／別 Windows Version、複数 DPI／Monitor、GPU Device Removal 下の操作は未確認。

## Next Action

M23 Close 後、M24 の Research Issue で Runtime Scene Data の Version 拡張、Standalone Renderer と
RHI の GPU Resource／Fence 所有、Dynamic／Static Package 境界を先に確定する。
