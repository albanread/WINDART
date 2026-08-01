# W1 — snapshot-as-blob (boot the core snapshot from the SQLite image)

Goal (user, DB-world): "hold the initial snapshot as a blob in the db… all loose
files in one database." W1 makes the boot snapshot live IN the image, so the whole
world is one file. It extends Stage 2's swappable-buffer override — no VM change
("we are not changing the shape of the Dart VM").

## Mechanism (host/embedder layer only)

Fallback chain in `windart_load_snapshots_from_disk` (win_host.cpp), tried before
`Dart_Initialize`:

1. **DB image blob** — `blobs` rows `snapshot:vm` + `snapshot:isolate` in
   `%USERPROFILE%\.windart\workspace.sqlite`;
2. **on-disk `.bin`** next to the exe (Stage 2);
3. **baked-in arrays** (`snapshot_gen.cc`).

- **`windart_snapshot_blob.cc` (NEW, owned)** —
  `windart_read_snapshot_blob(key)` reads a blob into a malloc'd process-lifetime
  buffer (the VM does not copy snapshot data) or returns NULL (→ fall back);
  `windart_bake_snapshots_to_image()` writes the two on-disk `.bin` into the `blobs`
  table (`sqlite3_bind_blob`). Read-only for boot; read-write only for bake.
- **`win_host.cpp`** — tries the blob first; ALL-OR-NOTHING + a **version guard**:
  override only if BOTH blobs are present AND version-compatible with this build.
- **Bake** — `Workspace_bakeSnapshot` native → `wsBakeSnapshot()` (win.dart);
  driven from Dart via `dartui test\workspace.dart bake` (the seed of the future
  in-app "Recreate Snapshot", W5).
- **CMakeLists.txt** — adds `windart_snapshot_blob.cc` to `dart_win32`.

## The version guard (why a stale blob cannot brick)

A snapshot is `[16-byte Snapshot header][version (no null)][features (null-term)]
[data]` (Snapshot::kHeaderSize = 2·int64; verified in runtime/vm/snapshot.cc
`VerifyVersionAndFeatures`). `WindartSnapshotCompatible` compares the candidate
blob's `[16 .. features-null]` against the **baked array's** same region. The baked
arrays are always present and are the authoritative "this build's version", so no
VM-internal symbol is needed. Comparing version+features (and STOPPING before the
data) means a legitimately re-baked snapshot — same VM build, new classes/data —
still matches, while a wrong-version blob is rejected and we fall back instead of
letting `Dart_CreateIsolate` abort on it.

**Scope (honest):** the guard is VM-*compatibility* (won't brick), not content-
*freshness*. A blob with the same VM version but stale content (e.g. an old
`win.dart`) is VM-compatible and will boot — so **re-bake after changing snapshot
content**, exactly as Stage 2's `.bin` must be regenerated. True provenance/rollback
is W3 (versioning).

## Verification (all captured, exit 0)

1. **Boots from the blob** — `bake` (booted from `.bin`, wrote blobs
   vm=845146/iso=240952) → next boot logs
   `snapshot: booting from DB image blob (vm=845146 iso=240952 bytes)`; full selftest
   passes, Stage 1 morph (`step=7`) + gate (`line 2 pos 12`) intact.
2. **Absent blob → `.bin`** — the bake run itself (blobs not yet present) logged
   `booting from on-disk .bin`.
3. **Wrong-version blob → `.bin` (no brick)** — overwrote `snapshot:vm` with a
   wrong-version string → boot logged `booting from on-disk .bin`, selftest exit 0
   (guard rejected the blob, fell back — no abort). Re-bake → `DB image blob` again.

Owned files only: `dart_win32/windart_snapshot_blob.cc` (new), `dart_win32/win_host.cpp`,
`dart_win32/windart_workspace_natives.cc`, `dart_win32/win_natives.cpp`,
`dart_win32/win.dart`, `CMakeLists.txt`, `test/workspace.dart`. No `tree/` change,
no patch hunk (host layer only).
