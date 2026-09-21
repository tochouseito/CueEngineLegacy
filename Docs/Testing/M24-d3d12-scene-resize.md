# M24 Scene Depth Resize and Recovery (#363)

The Scene Pass remains owned by the Presentation Context. A zero-size Resize
suspends Frame acceptance without retiring Depth or DSV. A same-size Restore
resumes acceptance with the same Depth resource. A size-changing Resize first
uses the existing Frame Fence to prove GPU idle, then retires the old Scene
Pass; the next Scene Frame creates Depth and DSV at the new Swap Chain size.
The Production probe compares the non-owning Depth identity across suspension
and same-size Restore, checks that an actual size change clears that identity,
and verifies the new dimensions after Scene submission.

An isolated Process test uses the actual Presentation on WARP and an available
hardware adapter. It copies the Back Buffer into a probe-owned Readback before
Present, then maps it only after the existing Resize or Shutdown fence wait has
proved GPU completion. At 640x360 and again after a real Resize to 672x384,
the test checks the clear corner, a far-only Cube, and both draw orders with a
near Cube that must win the Depth test. The capture hook exists only with
`BUILD_TESTING=ON` and does not add a public Presentation API. Separate
offscreen Scene Pass probes also cover 64- and 96-pixel extents.
Distribution builds must use `BUILD_TESTING=OFF`; Release alone does not remove
the probe hooks when the test-enabled CMake configuration is used.

A Process-separated fault probe injects a synthetic `E_OUTOFMEMORY` at Depth
creation and at DSV Heap creation after a successful Resize. These faults are
scoped to one Presentation Context and compiled only with `BUILD_TESTING=ON`.
The fault is cleared before retry. The probe checks the
specific error, absence of partial Scene native objects, no additional Frame
Submit, and successful re-creation on the next Scene Frame. It does not claim
to reproduce a real out-of-memory event or a GPU completion failure.

The existing GPU-unproven Resize probe now submits a Scene Frame before fault
injection and verifies that Depth/DSV, Back Buffers, RTVs, command resources,
and Backend owners remain retained in `Unavailable`. Broader Device Removal,
Signal failure, and Scene lifetime fault matrices remain in #364. Actual
window appearance and complete Standalone workflow remain in M24's integration
and completion issues.
