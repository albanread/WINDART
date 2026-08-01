# W2 — export-world / import-world (the git bridge)

Goal (user, DB-world): "a tidy release without losing git tracking of the apps."
A single `world.sqlite` is tidy but a binary blob (bad for git diffs/merges); loose
files are diffable but untidy. W2 bridges the two: **develop against git-tracked
loose files; build the one-file release with import-world; export-world commits
live edits back.** The `world.sqlite` is a build artifact (gitignore it); the loose
files are the tracked source of truth.

## Mechanism (host layer — binary-safe)

The workspace sqlite binding is text-only (`column_text` / `bind_text`), which
truncates a binary snapshot blob at its first NUL. So the projection lives in C++:

- **`windart_world_io.cc` (NEW, owned)** —
  `windart_export_world(outDir)` dumps the image to loose files;
  `windart_import_world(inDir, outDbPath)` builds a fresh DB from them.
- **Natives** `Workspace_exportWorld` / `Workspace_importWorld` →
  `wsExportWorld(dir)` / `wsImportWorld(inDir, outDb)` (win.dart).
- **Commands** `dartui workspace.dart export <dir>` / `import <dir>` — one-shot:
  run the image op after the UI is up, then `exit(0)` (dart:io) for a deterministic
  headless exit (the GUI WM_CLOSE teardown is for interactive runs).
- **CMakeLists.txt** — adds `windart_world_io.cc`.

## Layout under `<dir>`

```
src/userlib/<name>.dart   userlib(name, source)  — the live user classes
src/classes/<name>.dart   classes(name, source)  — VM-class source sketches
blobs/<safeKey>.bin       blobs(key, kind, data) — snapshot (+ future assets)
blobs/index.txt           "key|kind|file" per blob — exact key/kind fidelity
world.manifest.txt        schema version + row counts (informational)
```

Text tables scan `*.dart` (name = filename). Blobs use `index.txt` (a simple
pipe-delimited line per blob) so key + kind round-trip exactly and binary bytes are
read/written whole. No JSON parser needed.

## Verification (all captured, exit 0)

Seeded `userlib.Counter` with a distinctive `bump` (adds 5), baked the snapshot, then:

1. **Export** — `export <dir>`: `userlib=1 classes=4 blobs=2`; tree has
   `src/userlib/Counter.dart`, `src/classes/*.dart`, `blobs/snapshot_{vm,isolate}.bin`
   (+ index + manifest); exported `.bin` sizes equal the on-disk `.bin`.
2. **Import** — `import <dir>` builds `<dir>/world.sqlite`; querying the original
   image vs the imported DB shows **identical** rows: `userlib.Counter srcLen=76`,
   all 4 class sketches, both blobs (`845382` + `241081` bytes) — byte-exact.
3. **Boot the imported world** — swapped `<dir>/world.sqlite` over the image (backup
   + restore): boot logged `booting from DB image blob`, and
   `STAGE1: Counter.n=15` (3 bumps x 5) — the seeded source round-tripped AND the
   snapshot blob boots. selftest exit 0.

## Scope / notes

- **ASCII paths** (narrow Win32). Build/USERPROFILE dirs are ASCII; a UTF-8/wide
  variant is a later refinement.
- **Key convention** `<kind>:<name>` (e.g. `snapshot:vm`); the `index.txt` carries
  key+kind verbatim, so the convention is documented, not required for round-trip.
- **Never point `import` at the live image** — it deletes + recreates the target.
  The command targets `<dir>/world.sqlite`.
- **Tables handled**: userlib, classes, blobs. Adding a table (meta/versions in W3)
  is a small addition to export/import.
- **Exit codes**: all commands exit 0; a prior "-1" was a PowerShell
  `2>&1 | Select-String` stderr-promotion artifact, not the process code.

Owned files only: `dart_win32/windart_world_io.cc` (new),
`dart_win32/windart_workspace_natives.cc`, `dart_win32/win_natives.cpp`,
`dart_win32/win.dart`, `CMakeLists.txt`, `test/workspace.dart`. No `tree/` change.
