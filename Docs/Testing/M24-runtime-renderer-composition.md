# M24 RuntimeHost Renderer Composition

Issue #354 implements the CPU extraction boundary in ADR-0029. The RuntimeHost
composition root registers Core and Renderer schema types before sealing its
registry, then registers the Renderer system and component builders before the
Runtime Application Session starts. Dynamic and Static packages use this same
composition; neither path depends on Editor or ImGui.

`RuntimeHostApplication::render_snapshot()` lends a `const RenderSnapshot&` to
the owner thread. The application owns its `RenderSnapshotStore` longer than the
session and systems. The borrowed reference must not survive `advance_frame`,
`stop`, or application destruction. Its data owns values and stable IDs rather
than World, Entity, or Component pointers. The Renderer system clears the store
when it stops, including rollback after a later system fails to start.

The Package process test publishes canonical v2 scenes with no main Camera,
one main Camera, and multiple main Cameras. It checks the extracted status and
Mesh count, repeats a successful launch after shutdown, injects a Game Module
start failure after Renderer start, and launches successfully again. The
dependency and query-provider tests cover the Runtime-only link boundary and
duplicate Renderer type rejection.

Validation on 2026-09-21: Debug, Development, and Release full builds succeeded;
each configuration ran 277 CTest entries without failures. The four Release
InfoQueue/DRED entries are configuration-defined skips. `git diff --check`
succeeded. This issue does not draw the Snapshot to a GPU surface; the Scene
Frame and RuntimeHost bridge belong to later M24 issues.
