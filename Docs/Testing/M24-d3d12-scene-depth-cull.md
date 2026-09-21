# M24 D3D12 Scene Depth and Back-face Cull (#356)

The fixed Scene Pass owns a D32 depth texture and one DSV heap in the existing
Presentation Context. The texture is created in `DEPTH_WRITE`, stays in that
state, and is cleared to 1.0 on every Scene Frame before Cube draws. The
Pipeline uses depth write and `LESS`. The built-in Cube's outward winding from
ADR-0026 is rendered with back-face culling and clockwise screen-space front
faces (`FrontCounterClockwise = FALSE`). Clear-only frames do not create or use
Depth.

Scene resources are initialized on the first Scene Frame and released only
after the existing GPU-idle proof or Device Removal cleanup contract. A real
size-changing Resize releases the old Scene Pass after the idle proof and
before retiring old Back Buffers. The next Scene Frame lazily creates Depth at
the new Swap Chain dimensions. A zero-size suspension or same-size restore
does not retire the resource.

Offscreen pixel probes cover WARP and available hardware adapters. The depth
case first proves that the far rotated Cube is visible by itself, then checks
both draw orders: near-to-far must preserve the near pixel and far-to-near
must replace the far pixel. Together these detect a missing second draw as
well as missing Depth Test. The cull case places the camera inside the Cube and
checks that the center remains the clear color. A production Presentation
test checks Depth/DSV ownership and exact dimensions before Resize, after
retirement, and after the next Scene Frame.

Detailed Resize/Minimize/Restore fault injection remains #363. GPU failure
retention and DRED owner diagnostics remain #364.
