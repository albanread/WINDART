# The World-in-a-DB design — one image, git-tracked, tidy release

Vision (user): hold **everything the running program needs** in one SQLite image —
user source, the boot snapshot, icons, sounds, shaders — with **`export-world` /
`import-world`** commands that project the image to/from loose files. The **release
is one tidy file**; **git still tracks the apps** (as the exported loose files).
Boot-critical pieces are **versioned** so there's always a last-good to roll back to.

This is the Smalltalk **image** model, with a filesystem projection for git. It's the
natural convergence of what's already built:

- **Stage 1** already puts user source in the image (`userlib`) and serves it into the
  live VM via a C++ tag handler — no loose file.
- **Stage 2** already boots the snapshot from a *loadable buffer* (a `.bin`) instead of
  the baked-in C array — a DB **blob** is just a different buffer for the same override.
- The **BOM + longjmp** fixes mean DB-materialized source loads and errors cleanly.

So "everything in the DB" is mostly *plumbing each asset's loader to read the image
first*, plus the export/import bridge and versioning.

## The world schema (one SQLite image)

```sql
-- user + app source (Stage 1 has `userlib`; generalise to any unit)
CREATE TABLE source  (uri TEXT PRIMARY KEY, body TEXT);          -- 'userlib:user', 'app:calc', ...
-- binary assets: the snapshot, icons, sounds, shaders, images
CREATE TABLE blobs   (key TEXT PRIMARY KEY, kind TEXT, data BLOB);
--   key='snapshot:vm' / 'snapshot:isolate'; kind='snapshot'
--   key='icon:goBack' kind='icon'; key='sound:zap' kind='sound'; ...
-- append-only history for rollback (boot-critical resilience)
CREATE TABLE versions(id INTEGER PRIMARY KEY AUTOINCREMENT, ts TEXT, label TEXT,
                      table_name TEXT, row_key TEXT, old_body TEXT);
-- world metadata: schema version, the VM/SDK build hash the snapshot matches, HEAD version
CREATE TABLE meta    (key TEXT PRIMARY KEY, value TEXT);
```

(`userlib` from Stage 1 folds into `source` as `uri='userlib:user'`.)

## 1. Snapshot-as-blob (extends Stage 2, small)

`windart_load_snapshots_from_disk` (win_host.cpp) already overrides
`dart::bin::vm_snapshot_data` / `core_isolate_snapshot_data` before `Dart_Initialize`.
Generalise its source to a **fallback chain**:

1. `blobs` where `key IN ('snapshot:vm','snapshot:isolate')` in the image (read-only
   sqlite3, already linked) — the world's snapshot;
2. else the on-disk `.bin` next to the exe (Stage 2);
3. else the baked-in array.

All-or-nothing + the version guard (`meta.vm_build` vs the binary's snapshot version)
carry over from Stage 2. Process-lifetime buffers. **A bad blob → clean fallback**, so
boot never bricks. This is the piece that makes the snapshot live *in* the world.

## 2. Assets-in-DB (incremental)

Each asset loader gains a **DB-first path**, mirroring the icon/sound/shader loaders'
current file/embedded source:

- **Icons** (toolbar): `win_toolbar` reads `blobs` `kind='icon'` before the embedded SVGs.
- **Sounds** (SFX): `gp_audio` reads `kind='sound'`.
- **Shaders**: `gp_engine` reads `kind='shader'`.

Order by value/effort — icons first (visible), then sounds, then shaders. Each is a
small, isolated change (query the image; fall back to the embedded default).

## 3. export-world / import-world (the git bridge)

The crux of "tidy release *without* losing git tracking":

- **`dartui --export-world <dir>`** — project the image to loose, diffable files:
  - `source` rows → `<dir>/src/<uri-as-path>.dart`
  - `blobs` snapshots → `<dir>/snapshot/vm_isolate_snapshot.bin`, `isolate_snapshot.bin`
  - `blobs` assets → `<dir>/assets/<kind>/<name>`
  - `<dir>/world.manifest.json` — schema version, `meta`, and the file inventory.
- **`dartui --import-world <dir>`** — build a fresh `world.sqlite` from those loose files.

Workflow that keeps both properties:
- **Develop**: edit live in the IDE (writes the image) → `--export-world` → **git commit**
  the loose files. Or edit the loose files in git → `--import-world` → run.
- **Release**: `--import-world` from the git-tracked files → **one** `world.sqlite` → ship.

So git tracks `src/*.dart`, `assets/*`, and `snapshot/*.bin` (a ~1 MB binary blob is fine
in git; or track only the source + a "recreate snapshot" step). The `world.sqlite` itself
is a build artifact / user data — **gitignored**. Diffs/merges happen on the loose files;
the release is the single tidy image.

Implementation: a CLI flag dispatched in `bin/main.cc` (a `--export-world`/`--import-world`
branch that runs a C++ sqlite3 + file-I/O routine before the UI host), or a small Dart
tool run under `dartui` with the flag. C++ keeps it usable headless (CI release builds).

## 4. Versioning + rollback (boot-critical resilience)

"Boot is important — always have a last-good to roll back to."

- Every image write (Accept, snapshot bake, asset update) appends a `versions` row with
  the **old** body/blob-ref and a timestamp/label. Append-only = full history.
- `meta.HEAD` names the current good version; boot serves HEAD.
- **Rollback** = restore a prior `versions` row (source or snapshot) and move HEAD.
- The Stage-1 **validate-before-save** gate already keeps `source` compilable; versioning
  adds *time-travel* on top (undo a logically-bad-but-compilable change).
- For the **snapshot** specifically: a "recreate snapshot" (Stage 3) writes a *new*
  snapshot blob as a new version; if the next boot with it fails the version guard, the
  loader falls back to the previous snapshot version — the boot can't be bricked.

## 5. Recreate-snapshot = bake the world (Stage 3, larger)

With §1 (snapshot-as-blob) + §4 (versioning): a "Recreate Snapshot" action runs
`gen_snapshot` over the current image source, stores the result as a new `snapshot:*`
blob version, and boots from it next launch. This is the "save/bake the image" operation
— it's what lets edited **core** classes (frozen in today's snapshot) become persistent,
and what keeps boot fast. Guardrails: keep the prior snapshot version; validate on next
boot; roll back on failure.

## Staged plan

- **W1 — snapshot-as-blob** (extend Stage 2's override to read the image first). Small,
  high-symbolism (the snapshot joins the world).
- **W2 — export-world / import-world** (the git bridge). Medium; unblocks the "tidy
  release + git tracking" goal immediately, even before assets move.
- **W3 — versioning + rollback** (the `versions` table + HEAD + a rollback command).
- **W4 — assets-in-DB** (icons → sounds → shaders, incremental).
- **W5 — recreate-snapshot** (bake the world; needs W1+W3; the Stage-3 capstone).

## Risks / notes

- **Snapshot↔binary match**: a snapshot blob only boots on a matching VM build — carry
  `meta.vm_build` and gate on it (Stage 2 already reports the mismatch cleanly). Export
  the build hash alongside the blob so import-world can warn.
- **Round-trip fidelity**: `export-world`→`import-world` must be lossless — a golden test
  (export, import, diff the image) guards it.
- **Blob size / git**: the snapshot is ~1 MB. Tracking it in git is acceptable, but the
  cleaner long-term option is to track *only source + assets* and regenerate the snapshot
  via W5 on release, so git holds no large binary.
- **One image, many concerns**: keep `source` / `blobs` / `versions` cleanly separated so
  a corrupt asset can't take down source loading, and vice versa.
- **Concurrency**: the C++ handler opens the image read-only; writers (Accept, bake) use
  their own connection. `SQLITE_THREADSAFE=1` is already set.
