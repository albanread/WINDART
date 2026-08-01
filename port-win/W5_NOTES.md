# W5 — recreate-snapshot ("bake the world")

Goal (user/design): a "Recreate Snapshot" action that rebuilds the boot snapshot and
stores it as a new versioned blob — the capstone that closes the DB-world loop.

## Finding that shaped the scope

The boot snapshot is a **core** snapshot: `gen_snapshot --snapshot_kind=core` bakes the
core libs + dart:win (CMakeLists.txt §"gen_snapshot"). It does NOT include userlib —
that is served live by the C++ `userlib:` tag handler and already persists in the image
(Stage 1). So "recreate-snapshot" is framed as **the world regenerating its own boot
snapshot from the VM's own gen_snapshot**, then a versioned bake.

## Mechanism (Dart-only — no new native, no rebuild)

`recreate-snapshot` command (workspace.dart):

1. `Process.runSync(gen_snapshot.exe, --snapshot_kind=core, --vm_snapshot_data=…,
   --isolate_snapshot_data=…)` — regenerate the two `.bin` next to the exe (found via
   `Platform.resolvedExecutable`).
2. `wsBakeSnapshot()` (the W1 native) — bake the fresh `.bin` into the image as
   `snapshot:vm`/`snapshot:isolate`, preserving the previous as `@prev` (W3).

So it reuses everything already built: gen_snapshot (the build tool), the bake +
`@prev` versioning (W1/W3), and boots via the W1 fallback chain.

## Verification (exit 0)

- `recreate-snapshot` -> `gen_snapshot exit=0`, `baked snapshot -> image blobs
  (vm=845478 iso=241136 bytes)`.
- Standalone check: gen_snapshot reproduces the snapshot byte-for-byte
  (845478/241136 == the on-disk .bin) — deterministic for a given binary.
- Next boot -> `booting from DB image blob (current)` (the regenerated blob) + full
  selftest, STAGE1 Counter.n=3.

## Honest scope

- **Self-hosting + loop closure**: the running system rebuilds its own boot snapshot
  with no C++ build and no external tooling, versioned + rollback-able. This is the
  capstone capability.
- **Reproduces today**: because the core libs are fixed in the binary, the regenerated
  snapshot equals the current one — recreate does not yet *capture edits*. It becomes
  edit-capturing when core/app source moves into the image (a script/app-JIT snapshot
  that imports userlib), which is the natural next step of the "everything in the DB"
  vision.

## The DB-world, closed

- **W1** snapshot lives in the image (blob boot, fallback chain).
- **W2** export/import-world (the git bridge).
- **W3** versioning + rollback (source undo; snapshot last-good).
- **W5** the world regenerates its own snapshot (gen_snapshot -> versioned blob -> boot).

(W4 — assets in the DB — remains, and is independent/incremental.)

Owned files: `test/workspace.dart`. No native, no `tree/` change.
