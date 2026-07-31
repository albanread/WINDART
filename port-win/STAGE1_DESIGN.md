# Stage 1 design v2 — the inverted split: live + persistent user code in the UI isolate

Goal: edit a user class in the workspace, **Accept**, and the change is **(a) live** (a
running instance morphs, InstanceMorpher + Become) and **(b) persistent** (survives
restart) — genuinely, replacing the current save-to-DB-only Accept.

> **v2 supersedes v1.** v1 evicted *user code* into a mirrors-free worker isolate. A
> 4-agent adversarial review found the split inverted the wrong way; see the appendix
> for the v1 postmortem. v2 evicts *mirrors* instead.

## The constraint (unchanged, empirically pinned down)

`dart:mirrors` + a **secondary source-imported library** abort at isolate load
(`allocation.cc:37` StackResource assert), before `main()` runs. The review sharpened
the mechanism: it is NOT "reflecting over a source-loaded library" — the boot walk
reflects the source-loaded **root** library (`workspace.dart`, incl. `Calculator`)
every boot without incident. The trigger is mirrors *coexisting with a secondary
source import at load time* (n=1: `counter_scratch.dart`; a 30-min matrix spike
bounds the residual). So mirrors and user libraries cannot share an isolate — but we
get to choose which one moves.

## Why invert: the mirrors inventory

An exhaustive inventory of every `dart:mirrors` use in `workspace.dart` (boot walk
:1099-1121, `loadVarsMethods`, `classSketch`/`_typeName`/`_paramList`/`_methodDecl`,
`buildEditorClassList`, `doFind`, Docs, the instance/class toggle) found **only static
metadata** — names, kinds, signatures. There is no `reflect()`, no `InstanceMirror`,
no mirror invoke anywhere. The Debug tab uses natives + spawnUri; the VM tab uses
`wsVmStats`. So 100% of the Browser/Find/Docs data can be **precomputed strings**,
and the UI isolate loses nothing by dropping `import 'dart:mirrors'`.

## Architecture v2

- **UI isolate** (`workspace.dart`, mirrors-free): the GUI, the Editor, Do-It
  (synchronous, unchanged), **and the live user library** — a statically imported,
  DB-materialized `user_lib.dart`. Live user instances are ordinary objects here;
  user classes may even drive `ui.*` (the Calculator can finally become an editable
  user class — v1 forfeited that forever).
- **Mirror worker** (stateless, mirrors-only, no user code, no GUI): computes the
  class catalog — `libraryNames`/`classesInLib`/`libOfClass`, per-class two-side
  member lists, signature sketches, Find's member index — and **writes it to the
  SQLite image**. The UI reads the cached catalog every boot; the worker re-runs only
  when the SDK/snapshot changes (it is a pure function of the core snapshot). It can
  be a `spawnUri` isolate or even an out-of-process `dart.exe` run — it is stateless,
  so its failure mode is benign (stale/absent catalog, refresh retried).
- **Spawn-under-debug** (unchanged): the T4 `debug_target` pattern remains the
  debugger story, now also for user classes (run a spawned copy under the debugger).

The catalog moving **into the image DB** is the point where this converges with the
source-image direction: the image now holds user source (authoritative), the SDK
class catalog (cache), and later the baked snapshot association (Stage 3).

## The reload path (the decisive advantage)

Accept = write DB → re-materialize `user_lib.dart` → `wsRequestUiReload()` →
host-deferred `PerformUiReload` (off-stack, after `Dart_HandleMessages`) →
`force_reload` re-reads root + imports → user instances morph. Every link is already
exercised in production TODAY: `accept()` calls `wsRequestUiReload` on every Accept
and the whole UI isolate (workspace + dart:win) survives the force-reload;
`workspace_morph.dart` proves the live-instance morph (kept state + new field) on a
mirrors-free root importing a rewritten source lib. **No unproven VM behavior on the
critical path. No new C++.**

Reload preserves existing top-level state (proven: `gc` survived in the morph
capstone; the running IDE survives its Accept-reloads) — so the UI, the widget
registry, and a `reg` instance registry all persist across Accepts.

## The Accept gate (critical — a bad Accept must never poison boot)

Under v2 the user lib is imported by the IDE's root script, so unbootable user source
would take the IDE down at boot. The rule that prevents it:

1. Accept: rewrite the scratch from the candidate source → `wsRequestUiReload`.
2. Reload is atomic: on `ERR` (cancel + rollback — confirmed clean in
   `isolate_reload.cc` Rollback), **revert the scratch** from the previous source and
   do **not** write the DB; surface the error in the Editor status.
3. Only on reload success is the DB updated. **The DB never holds source that has not
   survived a reload.**
4. Boot always re-materializes the scratch **from the DB** (never trusts a stale
   scratch), so a crash mid-Accept cannot leave a poisoned compile input.

## Product rules (from the gaps review — apply regardless of split)

- **Editor partition**: User classes = editable, Accept enabled. VM/SDK classes =
  read-only viewer (Accept disabled) + a "Copy as user class…" gesture that forces a
  non-shadowing rename. The materializer rejects names colliding with imported-library
  exports (an Accepted `Duration` shadow was both incoherent and uncompilable).
- **Do-It**: stays local + synchronous (the routing problem of v1 vanishes). One
  honest regression: mirror-expressions in Do-It (`currentMirrorSystem()...`) no
  longer work — the catalog data is precomputed instead; state it in Help.
- **Registry + visibility**: a `reg` map top-level (Do-It: `reg["c1"] = new
  Counter()`), plus an Instances view (Browser `user:` pane or Workspace section)
  enumerating `reg` with `toString()` — morphs must be observable.
- **Rename/delete**: transactional row ops (rename = insert-new + delete-old); define
  reload behavior for removed classes (orphaned instances dropped from `reg` with a
  warning). Today the DB only ever grows — add delete.
- **Find/Docs**: extend to user classes from the DB (names + source scan), or state
  the deferral explicitly.
- **Browser `user:` category**: user classes listed from the DB; source pane renders
  DB source via the P4 text renderer; member lists via a light source parse
  (signature-fidelity not required for user code — we have the real source).

## Spikes before building (all cheap, total ~0.5–1d)

- **S-a**: `Isolate.spawnUri` of a script importing `dart:mirrors` (the one untested
  combination for the worker). Fallback if it fails: out-of-process `dart.exe` worker
  writing to the image DB — B survives either outcome.
- **S-b**: the abort matrix (mirrors × {file import, spawnUri}) to bound the
  `allocation.cc:37` mechanism beyond n=1.
- **S-c**: one-liner — `wsEval('new Counter()')` constructor resolution in the root
  library (the morph capstone proved method calls; cover constructors).

## Build plan (effort ~M, zero new C++)

- **B1 — mirror worker + catalog**: move the boot walk + `classSketch`/
  `loadVarsMethods`/member-index into a worker script; write the catalog to the
  image DB (versioned by SDK build); UI boots from the cached catalog.
- **B2 — Browser refactor**: Browser/Find/Docs/Editor consume the precomputed maps;
  drop `import 'dart:mirrors'` from `workspace.dart`. (Mechanical — the call sites
  already consume lists/strings.)
- **B3 — live user lib**: static import of the materialized `user_lib.dart`; the
  validated Accept pipeline above; `reg` + Instances view; Editor partition +
  copy-as-user-class; rename/delete.

## Appendix: v1 postmortem (what the review established)

- v1's Approach 2 ("embedder takes over the spawnUri'd user isolate's loop") is
  **unimplementable as written**: a spawnUri isolate is permanently pool-attached
  (`message_handler.cc:105-126`; `pool_` cleared only at shutdown), `PostMessage`
  spawns the pool task *before* the notify callback fires, and an outside
  `Dart_EnterIsolate` races into the "Multiple mutators" FATAL
  (`dart_api_impl.cc:1395-1404`) — or, release-mode, a discarded-return null-`Thread`
  entry (`isolate.h:919-933`). The real variant required an embedder-created isolate
  and serialized user compute onto the UI thread.
- v1's Approach 1 (self-reload) was actually *viable*: upstream ships it as the
  `--reload-every` stress mode (`runtime_entry.cc:1687-1734`), and failure modes are
  clean cancel + rollback. But under v2 the entire fork is moot.
- `wsReload` has zero call sites in the repo — every real reload goes through the
  deferred host path. v1's "proven in-repo" claim was true of the native's existence,
  not its exercise.
- Corrections: dart:win per-isolate registration is `dartutils.cc:698-701` (v1 cited
  293-300); T1's "reflecting over a non-snapshot library crashes" mechanism note is
  refuted (the boot walk does exactly that every boot) — the trigger is the
  secondary-import coexistence, as above.
