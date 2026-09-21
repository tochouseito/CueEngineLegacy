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
retains partial native resources until the existing Backend Device Removal and
DRED classification has completed, then releases them. Device Removal is the
primary error and the Scene creation failure remains its cause.

`SceneFrame.hlsl` is first-party source compiled by the selected Windows SDK
`fxc.exe` from the selected Windows SDK root at build time into generated C
headers. The SDK root can be supplied with `CMAKE_WINDOWS_KITS_10_DIR` or
`WindowsSdkDir`; otherwise it is read from the Windows Kits registry. Runtime products do not need
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

PR review follow-up: the lazy Scene initialization Device Removal test now
checks primary code 52, its retained Scene creation cause, and one DRED attempt.
The SDK compiler path is derived directly from the selected SDK root/version on
every configure, without a cached executable search. During follow-up Debug
testing, `Cue.Editor.Workflow.ProcessRoundTrip` once failed with Windows
`Access is denied` while renaming its test project; it passed when run alone
and in the next full 280-test run. This intermittent unrelated test failure
has not been attributed to the Scene change.
