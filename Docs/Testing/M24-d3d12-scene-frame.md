# M24 D3D12 Fixed Scene Frame

Issue #355 adds a deliberately narrow GPU presentation path for the standalone
Scene work in ADR-0029. `PresentationContext::present_scene_frame` accepts only
non-Native value data: a clear color, a row-major row-vector World-to-Clip
matrix, and a borrowed span of at most 4096 Cube Local-to-World matrices. The
caller owns the span storage until the call returns. Invalid counts or
non-finite values are rejected before Frame Command recording or Fence advance.

The D3D12 Presentation Context owns the fixed Pipeline, Built-in Cube geometry,
and one Upload constant buffer per existing Frame Slot. It initializes them on
the first Scene Frame, writes a slot only after that slot's reuse Fence wait,
and releases them only after the normal GPU-idle or Device Removal cleanup
condition. A Scene Frame records Clear and Cube draws into the same Command
List, then uses the existing single Execute／Present／Signal sequence. The
Clear-only `present_frame` behavior remains unchanged. Failed lazy initialization
also runs the existing Backend Device Removal and DRED classification path.

`SceneFrame.hlsl` is first-party source compiled by the selected Windows SDK
`fxc.exe` at build time into generated C headers. Runtime products do not need
the shader source or a newly introduced load-time shader compiler DLL. The
fixed Pass intentionally has no Depth attachment and does not cull faces;
Depth, back-face Cull, and resize-specific validation belong to Issue #356.

The process tests cover an offscreen WARP Scene pixel and clear corner, invalid
input rejection, Scene／Clear Fence progression, unclassified Device Removal
during lazy initialization, and the existing Clear and 300-Frame paths.

Validation on 2026-09-21: full Debug and Development builds and all 280 CTest
entries passed in each configuration. The full Release build passed; 276 CTest
entries passed and four configuration-defined InfoQueue／DRED tests were skipped.
The Release Scene Frame process imports `d3d12.dll` and `dxgi.dll`, but no new
shader compiler DLL. The first sandboxed Debug run had ten filesystem／SDK
access failures and one stale dependency expectation; the former passed when
rerun with the required access and the latter passed after updating the RHI
dependency report. All three final configuration runs completed without
failures. `git diff --check` passed.

The generated shader headers share one path across configurations in a single
multi-config build tree. Sequential builds and separate CI jobs are covered;
simultaneously building different configurations in that same tree is not
validated and should use separate build directories if needed.
