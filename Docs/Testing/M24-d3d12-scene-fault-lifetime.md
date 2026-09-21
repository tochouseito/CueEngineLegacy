# M24 Scene GPU Fault and Resource Lifetime (#364)

The existing Presentation Context remains the sole owner of the fixed Scene
Pass. Its D32 Depth/DSV, Cube vertex/index buffers, and both Frame Slot constant
buffers are observed separately by test probes. The probes report only
non-owning presence and the Depth identity; they do not expose native handles
to Runtime or Renderer.

Process-separated WARP fault cases submit a Scene Frame before injecting a
reused-Fence wait failure, a Signal failure after native forwarding with GPU
completion hidden, or Device Removal at the following Present. Unproven GPU
completion must leave Presentation and Backend `Unavailable`: Shutdown fails
and the native owners and Depth identity remain retained. The isolated process
ends without invoking their destructors in those cases. Device Removal must
explicitly enable DRED in diagnostics-capable builds, set both owners to
`DeviceRemoved`, record DRED owner presence while Scene resources still exist,
and only then allow the safe removal cleanup to release them. Builds that do
not permit DRED, including Release, conditionally skip that probe. A synthetic
Device Removal proves state propagation and retention; it is not a substitute
for a real GPU hang capture.

An RTV rebuild fault after a submitted Scene Frame checks the opposite path:
the existing Resize Fence first proves GPU idle, then the Scene resources are
released before the failed RTV reconstruction shuts down Presentation. The
Clear-only RTV fault case remains registered separately.

WARP and available hardware run the Minimize/Restore/Resize Scene cycle with
the existing InfoQueue-enabled validation policy. The Presentation pixel probe
also checks the exact submitted and reused Fence slot after each of six Scene
Frames, three before and three after a real Resize, on both adapter policies.
Both paths require successful Presentation and Backend Shutdown. Hardware
tests skip only when no suitable adapter is available. Existing Clear and D3D12
fault tests remain part of the full CTest gate.
