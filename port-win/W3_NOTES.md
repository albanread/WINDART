# W3 — versioning + rollback

Goal (user): "The core files needed to boot also need versioning so we have the last
successful version to roll back to; user-level apps just need to be editable and
fixable, but boot is important." Two halves, matching that split.

## W3a — source versioning + Revert (user apps: editable & fixable)

Dart-only (workspace.dart); the write path is Dart, and the C++ userlib: handler
already re-reads the image on reload.

- **`versions(id, ts, kind, name, source, label)`** table. Every Accept, before it
  overwrites a userlib class, appends the image's current source for that class.
- **Revert** (a button beside Accept) pops the most recent prior version back into
  userlib and hot-reloads (morph); repeated Reverts step further back. Only accepted
  (gate-passed, compilable) versions are on the stack, so a Revert never reloads bad
  source. `versionCount()` reports "[N older]".
- Verified: Accept `inc()=>7` -> Accept `inc()=>11` -> Revert. `inc()` is a METHOD,
  so it re-links on each morph and tracks the source version (7 -> 11 -> 7) while
  `n=3` state is preserved; editor_revert.png shows the reverted source.

## W3b — snapshot last-good + rollback (boot-critical)

Host layer (windart_snapshot_blob.cc + win_host.cpp).

- **bake keeps `@prev`**: before overwriting `snapshot:vm`/`snapshot:isolate`, the
  current pair is copied to `snapshot:vm@prev`/`snapshot:isolate@prev` — the last-good
  to fall back to. No-op on the first bake.
- **win_host fallback chain**: `WindartTryBlobPair` tries the **current** pair, then
  the **@prev** pair, each ALL-OR-NOTHING + version-guarded, before the on-disk .bin
  and the baked array. So a bad/stale current snapshot boots from the previous one
  instead of aborting: **current -> @prev -> .bin -> baked**.
- **`rollback-snapshot`** (`windart_rollback_snapshot` native + command) promotes
  `@prev` back to current, so the next boot loads the previous snapshot.
- Verified (exit 0): two bakes leave all four blobs (`snapshot:{vm,isolate}` +
  `@prev`); corrupting the CURRENT `snapshot:vm` boots
  `from DB image blob (last-good)` (fell back to @prev, no abort); `rollback-snapshot`
  then boots `from DB image blob (current)` again — the corrupt current was repaired.

## Scope / notes

- **@prev = the previous current** (one level, not a full snapshot history). It is the
  "last snapshot", which is the last-good in the normal workflow (you bake from a
  working build). A confirmed-good promotion (only after a successful boot) is a later
  refinement; likewise a deeper snapshot history if needed.
- **Closes the W1 caveat** partially: a bad snapshot now has a DB-resident fallback
  (@prev), not only the on-disk .bin / baked array.
- Source history is unbounded (append-only, text — cheap); a retention cap is a later
  refinement if it ever matters.

Owned files: W3a — `test/workspace.dart`. W3b — `dart_win32/windart_snapshot_blob.cc`,
`dart_win32/win_host.cpp`, `dart_win32/windart_workspace_natives.cc`,
`dart_win32/win_natives.cpp`, `dart_win32/win.dart`, `test/workspace.dart`. No `tree/`
change.
