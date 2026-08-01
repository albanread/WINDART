# Stage 1 design v3 — single isolate, DB-served user source (the BOM postmortem)

Goal (unchanged): edit a user class → **Accept** → the change is **(a) live** (a running
instance morphs, InstanceMorpher + Become) and **(b) persistent** (survives restart).

> **v3 supersedes v1 (user-worker) and v2 (mirror-worker).** Both were built to avoid a
> "mirrors + a source-imported library abort at load" constraint. That constraint was
> **false** — the aborts were a UTF-8 **BOM** in one stale test file (`counter_scratch.dart`).
> With the scanner now BOM-tolerant, mirrors and a user source library coexist in ONE
> isolate (proven: 16 libs / 800 classes reflected + the imported class constructed), and
> the live morph runs. No worker isolate, no cross-isolate protocol, no reload fork.

## What the spikes established (all now empirical)

- Secondary source imports work: the demos import `pixmap.dart`/`gamepane.dart`→`abc.dart`
  and render live; a direct `import` + construct works (`P1/P2 loaded`).
- Mirrors + a BOM-free import in one isolate: `libs=16 classes=800, new U().inc().v=43`.
- The morph: `workspace_morph.dart` → instance keeps `n=3`, gains `step=7`, new behavior.
- The one trigger was the BOM (`ef bb bf`) → `allocation.cc:37`; fixed in `scanner.cc`.

## Architecture v3 — single isolate

- **One isolate** (`workspace.dart`, mirrors and all). It **imports the user library**
  whose source is **served from the SQLite image by a C++ tag handler** (below). Live user
  instances are ordinary objects; user classes can drive `ui.*` (editable Calculator).
- **Do-It stays synchronous** (`wsEval` on the root library sees the imported user classes).
- **The Browser keeps `dart:mirrors`** for SDK classes; user classes get a `user:` category
  read from the DB (P4 renderer). No worker, no data-shipping.

## Directive B — source↔DB management in C++, deep in the VM

Per the user: the user-source ↔ DB path lives in the **C++/embedder layer**, not Dart-side
file juggling. A custom library tag handler serves the user library from the image:

- **`WindartUserTagHandler`** (owned C++): installed via `Dart_SetLibraryTagHandler`
  (patch `main.cc`, wrapping the stock `Loader::LibraryTagHandler`). For the `userlib:`
  scheme it serves source **synchronously** from the SQLite image (a dedicated read-only
  `sqlite3` connection — `sqlite3.c` is already linked; `SQLITE_THREADSAFE=1`), the same
  shape `DartColonLibraryTagHandler` uses for `dart:` (`loader.cc:782`): `kCanonicalizeUrl`
  → return the `userlib:` URI unchanged; `kImportTag`/`kScriptTag` → `Dart_LoadLibrary` with
  the DB source; `kSourceTag` → `Dart_LoadSource`. Every other URI (`dart:`/`package:`/
  `file:`) delegates to the stock handler unchanged.
- Because reload re-invokes the isolate's tag handler (`isolate_reload.cc:628`), re-reading
  the (now-updated) DB row on Accept morphs the live instances — **no scratch file at all**,
  which is exactly the DB-first model. The scanner BOM fix means even a BOM-carrying DB row
  or hand-edited file loads.

## Flows

- **Accept** → write the edited source to `classes(name,source)` → `wsRequestUiReload()` →
  off-stack `PerformUiReload` → `ReloadSources(force)` → tag handler re-serves the DB row →
  instances morph → status surfaced. **Validated gate**: on reload `ERR`, the DB write is
  rolled back (keep last-good) so the DB never holds unbootable source; boot always builds
  the world from the DB.
- **Do-It (user)** → `wsEval(expr)` in the root library (sees user classes); synchronous.
- **Registry** → a `reg` map top-level (`reg["c1"] = new Counter()`); an Instances view lists
  it so morphs are observable.
- **Browser** → SDK via mirrors + a `user:` category from the DB (P4 source render).

## Product rules (carried from the gap review — still apply)

- Editor partitions **User** (editable, Accept on) vs **VM/SDK** (read-only, "Copy as user
  class…" forces a non-shadowing rename); the handler rejects names colliding with SDK exports.
- Rename/delete = transactional DB row ops; removed-class reload drops orphaned `reg` entries.
- Find/Docs extended to user classes from the DB (or explicitly deferred).
- Do-It mirror-expressions stay valid (single isolate — no regression).

## Build plan (effort ~M–L; the C++ handler is the delicate part)

- **B1 — user tag handler (C++)**: `WindartUserTagHandler` serving one `userlib:` library
  from the image + install hunk in `main.cc`; a dedicated read-only sqlite3 connection to
  the image. Spike: `import 'userlib:user'` of a DB-stored class loads + constructs.
- **B2 — Accept/reload/registry (Dart)**: workspace imports `userlib:user`; `reg`; Accept =
  DB write → reload → morph → status; validated gate; Instances view.
- **B3 — Browser/Editor user surface (Dart)**: `user:` category, Editor partition +
  copy-as-user-class, rename/delete, Find/Docs.

## Appendix — the postmortem

- The `allocation.cc:37` abort that spawned the entire two-isolate line of design was a
  UTF-8 BOM in `counter_scratch.dart`, mishandled by 1.24.3's scanner. `workspace_morph`
  ("the proven morph") imported it, so it too crashed — the "proof" hadn't actually run in
  this build. Fix: skip a leading U+FEFF in `Scanner::Reset` (committed).
- Lesson kept: verify a load-bearing "it crashes" claim by isolating the *file*, not just
  the *feature* — a single stale byte-order mark drove ~two design rounds and two review
  workflows before a bare-minimum repro (`dart.exe` + one import) exposed it.
