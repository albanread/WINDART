# P4 — on-disk SDK source viewer + an Accept honesty correction

## The source viewer (real bodies, not just mirror signatures)

The Browser reflects the LIVE VM via `dart:mirrors`, which exposes a class's
*shape* (name, supertype, field types, method signatures) but NOT source text —
the VM carries no source for its snapshot classes. So the panes showed one-liners
(`Duration abs();`). The actual 1.24.3 SDK source IS on disk, so P4 reads the real
class/method text straight from the `.dart` files.

- `_sdkRoots` — `e:/windart/tree/sdk/lib` (the build's extracted copy, primary) then
  `e:/dart_origins/sdk-1.24.3/sdk/lib` (the pristine quarry, fallback). First
  existing root wins, per file. **Read-only** — the quarry stays git-clean.
- `sdkClassSource(cls)` — `libOfClass[cls]` → `_libDir` (dart:core → core, …) → scan
  the lib dir's `.dart` files for `^\s*(abstract\s+)?class <Cls>[ \t\r\n<{]`, then
  brace-match (`_matchBrace`, comment/string-aware via `_skipStr`) to slice the whole
  class. Returns `'' ` when there's no `.dart` (native/patch class) → caller falls
  back to `classSketch` (the signature sketch).
- `sdkMemberSource(cls, decl)` — best-effort: `_memberAnchor` derives a search anchor
  from the decl (`get X` / `set X` / `operator …` / trailing name), finds it in the
  class source, and slices that member (brace-match or `;`).
- Wired into: `browseToClass` + the browser restore block (class source), `selectBrMethod`
  (method body), and `editorSourceFor`'s VM-class branch. Each prefers the real source,
  falls back to the sketch.

Proof (self-test, viewed):
- `browser_realsource.png` — Duration: header cites `tree/sdk/lib/core/duration.dart`;
  body shows `class Duration implements Comparable<Duration> {`, the `static const`
  constants with real initializer values.
- `browser_realmethod.png` — `abs()`: its doc comment + the real body
  `Duration abs() => new Duration._microseconds(_duration.abs());`.

## Accept honesty correction

Investigated "Accept updates the live image" and found it overstated for the Editor tab:

- `accept()` writes the edited source to the SQLite `classes` table — **real**, survives
  restart. Then it calls `Dart_WorkspaceReloadSources` → `isolate->ReloadSources`, which
  reloads the **root script + its on-disk imports** through the tag handler. It does NOT
  read the SQLite blob.
- The edited class (Counter) is **never imported into the running isolate** (workspace.dart:13
  deliberately avoids it; `userClassSource()` is defined but never called). So there is no
  live instance to morph, and the reload recompiles the unchanged `workspace.dart` — a no-op
  w.r.t. the edit.
- Net: Accept SAVES (persist + re-display on restart) but does NOT make the edit live. The
  genuine morphing reload (InstanceMorpher + Become) is real and proven **only** in the
  standalone `test/workspace_morph.dart` (S7.3), which imports an on-disk `counter_scratch.dart`,
  **rewrites that file**, reloads, and a live instance actually gains a field.

Fixed the misleading UI text (`ed_lbl`, `ed_note`, the Accept status) to say Accept SAVES to
the image and does NOT live-reload, pointing at `workspace_morph.dart` for the real thing.

**Open offer (not built):** wire the Editor to the workspace_morph mechanism — keep a live
imported scratch class + instance, and on Accept rewrite the scratch FILE (+ SQLite) then
reload → a genuine "edit + Accept → live morph" in-app. Larger change; deferred pending a
go-ahead.

test/workspace.dart only (runtime script; no rebuild).
