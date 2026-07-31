# S7 — Re-hosting `workspace.dart` onto the view-server

The concrete plan for Sprint **S7** (`SPRINTS.md` §S7): re-host the 4,853-LOC IDE
chrome (`MACDARTV1/macdart/cocoa/workspace/workspace.dart`) from
Dart-calling-AppKit onto the native Win32/Direct2D **view-server** built in S4.
Read-only analysis; this is a spec, not a rewrite.

**Sources analysed** (all read-only): `workspace.dart` (4,853 lines, 133-ish
`dart:cocoa` sites), `workspace/language.dart` (786), `cocoa.dart` (the current
bridge), and the target: `gui-design/S4_GUI_HOST_DESIGN.md`, `gui-design/win.dart`
(the `Ui` client), `gui-design/win_view.h` (the materializer model),
`arch-notes/S4_design_decisions.md`, `MACDARTV1/APP_PANE_PLAN.md`,
`MACDARTV1/WORKSPACE_PLAN.md`.

**The one-line finding.** The chrome divides cleanly. Roughly **60% is
platform-neutral logic** (the language-isolate orchestration, the debugger's
vm-service client, the lexer/formatter/compile-gate, the control plane, the demo
pacer) that ports with an import swap and a handful of accessor changes. Roughly
**30% is UI materialization** (`build*` functions + widget helpers) that is
rewritten from "create NSView, set frame, addSubview, wire target-action" to
"append a `['add'…]`/`['place'…]` command and `commit()`." Roughly **10% is
deleted** (the `objc_msgSend` selector lint, and the chrome's own app-pane
NSView materializer, which the C++ ViewServer now owns). **Three things force
net-new ViewServer capability the S4 protocol does not yet expose** — the
tabless tab deck, the draggable split panes, and the syntax-highlighting editor
— and they are the S7 risk, ranked in §4.

---

## 1. The two-isolate model on Windows

The macOS split survives the port unchanged in shape, because both OSes impose
the same rule for different reasons (`S4_GUI_HOST_DESIGN.md` §0): AppKit refuses
`NSWindow` off the main thread; Win32 delivers a window's messages only to the
thread that created it. So:

| | macOS today | Windows (S7) |
|---|---|---|
| **UI isolate** | thread 0, `[NSApp run]` pump, services `Dart_HandleMessages` via `CFRunLoopSource` | `DART_UI_HOST` thread, `GetMessageW` pump, services `Dart_HandleMessages` via `WM_WINDART_WAKE` (`win_host.cpp`) |
| runs | `main()`, `buildChrome`, all callbacks, control plane, the surfaces | identical |
| **Language isolate** | `Isolate.spawnUri` from a scratch copy, respawnable | identical (portable `dart:isolate`) |
| runs | user code, the image, morphing reload, debugger target | identical |

### What in the two files is UI-isolate vs language-isolate

- **`workspace.dart` is the UI isolate, entirely.** It is what S7 re-hosts.
- **`language.dart` is the language isolate, entirely, and is UI-agnostic**
  already. Its only coupling to the platform is one import
  (`language.dart:7` `import 'dart:cocoa'`) used for exactly four platform-neutral
  primitives — `wsEval`, `wsReload`, `Db`, `wsVmStats` — every one of which
  `dart:win` re-exports with the identical signature (`win.dart:15-20, 212-224`).
  **The port of `language.dart` is one line:** `import 'dart:cocoa'` →
  `import 'dart:win'`. Its `AppSurface` class (`language.dart:627-711`) pushes
  plain-list `['appui', …]` messages over a `SendPort` and never touches the
  platform — it is already a view-server client by construction.

### Does anything break under the view-server model?

The chrome moves from *manipulating* live handles to *describing* widgets and
reading events back. Four consequences, each with a resolution:

1. **The `defer` bounce is still required, unchanged** (`workspace.dart:49-102`).
   The comment there explains that AppKit callbacks arrive via `Dart_InvokeClosure`
   straight out of an ObjC IMP — *not* as an isolate message — so an `async`
   handler's microtask queue never drains and the handler silently does nothing.
   On Windows the identical hazard exists: `win_callbacks.cpp`'s `Dispatch()` also
   calls the Dart dispatcher via `Dart_InvokeClosure` (`win.dart:151`,
   `S4_GUI_HOST_DESIGN.md` §3). So `initEvents`/`defer`/`sel` port **verbatim**;
   every `onClick`/`onSelect` still routes through `defer(...)`.

2. **Synchronous reads become pulls, not property gets.** Today the chrome reads
   a live control synchronously — `gEditor.string().UTF8String()`
   (`workspace.dart:1106,1135`), `gFindField.stringValue()...` (`:999`),
   `gEditor.selectedRange()` (`:1114`), `b.frame()` (`:1784`). The view-server
   holds the widgets in C++; Dart holds only tickets. These become native pull
   ops: `Ui.textOf(id)` → `Win_widgetText` (`win.dart:129`), and any geometry
   read (`frame()`, `selectedRange()`, `bounds()`) needs an equivalent pull op
   **that the S4 catalog only partly provides** (`Win_measureText` and
   `Win_surfaceSize` exist; a per-widget `frame`/selection read does not — see
   §6, open question). The `frames`/`apptree` control verbs (`:1775,:1850`) that
   dump widget geometry are the main consumers and can be reduced to what the app
   describes rather than what the OS lays out.

3. **`reloadData()` disappears; row counts are pulled.** The chrome calls
   `gClassTable.reloadData()` after mutating a backing list (26 `reloadData`
   sites). Under `LVS_OWNERDATA` the list is virtual: Dart tells the surface "this
   list's row count changed" via a `set` command, and the host pulls
   `rowCount()`/`cellAt(row)` back through kinds 2/3 (`win.dart:94-98,170-171`).
   So `reloadData()` → `ui.set(id, {'rows': gBrClasses.length})`.

4. **`rebuildUi` / `disposeCallbacks` ownership changes** (`workspace.dart:4080-4125`,
   `:4108`). AppKit held action targets weakly, so the chrome's teardown just
   cleared the ticket map. Win32 controls are **owned**: the teardown must
   `DestroyWindow` them. This is already the ViewServer's job
   (`win_view.h:66-69` `ClearSurface`, `S4_design_decisions.md` §2), so the Dart
   side simplifies to `ui.clear()` + `disposeCallbacks()`; the C++ side owns the
   destroy. The **rebuild-ticket trap** (`APP_PANE_PLAN.md` §7 — a popped-out
   window surviving a rebuild with dead buttons) applies to the App tab and is
   handled by the retained-spec replay that already exists (`appRematerialise`,
   `workspace.dart:3925-3930`).

**Nothing in the model breaks.** The chrome was *already* architected as "one
isolate builds views, one runs code, message-passing between" — the S7 change is
narrower than the mac port's, because the chrome stops holding native handles and
starts holding ids, and the code that did that is a bounded ~30%.

---

## 2. Widget inventory — the complete kind list the chrome needs

Every widget KIND `workspace.dart` instantiates, the AppKit class it uses, the
S4 Win32/D2D backing (`S4_GUI_HOST_DESIGN.md` §4.4), and whether the ViewServer
already plans it (an enum member in `win_view.h:21-24`, body a skeleton) or it is
**net-new** for S7. Counts are `Cocoa.cls("…")` sites in `workspace.dart`.

| Chrome widget (AppKit) | used for (workspace.dart) | `dart:win` kind | Win32 backing (bring-up) | ViewServer status |
|---|---|---|---|---|
| `NSWindow` (:316) | the one workspace window | surface `win` | `WS_OVERLAPPEDWINDOW` (host) | ✅ planned (`OpenWindow`) |
| `NSView` container (:255,:336,:530,:603,:3639) | toolbar band, tab bodies, browser panes, app pane | *(no widget — see §4.3)* | child `WS_CHILD` group / language-side frames | ⚠️ **net-new** (deck/group) |
| `NSTabView` type 6 (:353) | the 9-tab tabless deck | *(no widget — see §4.3)* | show/hide child panes, or clear+re-describe | ⚠️ **net-new** (deck) |
| `NSButton` (:132,:216,:3739) | toolbar icons, ~60 action buttons | `button` | `BUTTON BS_PUSHBUTTON` | ✅ planned |
| `NSButton` (icon/template img) (:132-143) | 8 toolbar view-switchers | `button` + `image` prop | `BUTTON BS_BITMAP` / owner-draw | ⚠️ partial (no image prop on button) |
| `NSTextField` non-editable (:246) | labels, status lines, metrics cells | `label` | `STATIC SS_LEFT` | ✅ planned |
| `NSTextField` editable (:975,:2660,:3745) | Find field, Eval field, app fields | `field` | `EDIT` | ✅ planned |
| `NSTextView` in `NSScrollView` (:230) | editor, transcript, docs, dbg source, browser source | `text` (editor) | **RichEdit** `MSFTEDIT_CLASS` | ✅ planned (hardest — §4.1) |
| `NSTableView` single-col (:501) | browser 4 panes, Find, dbg stack/locals, help | `list` `LVS_OWNERDATA` | `SysListView32` | ✅ planned |
| `NSPopUpButton` (:2013,:2642,:3618) | editor/app/isolate pickers | `popup` | `COMBOBOX CBS_DROPDOWNLIST` | ✅ planned |
| `NSBox` custom-fill (:118,:182,:188) | textured toolbar, mem usage bar | `box` (+ fill colour) | `BUTTON BS_GROUPBOX` / D2D panel | ✅ planned (fill/pattern extra) |
| `NSSplitView` (:199) | browser + debug + docs draggable panes | *(no widget — see §4.2)* | draggable divider + min-size | ⚠️ **net-new** (splitter) |
| `NSImageView` (:3161) | the Demos canvas | `canvas` / `image` | child + D2D `Win_canvasBlit` | ✅ planned (S5 body) |
| `NSMenu`/`NSMenuItem` (:1203,:1226,:1257) | menu bar (7 menus) | menus (host) | `HMENU` + `WM_COMMAND` | ✅ planned (host) |
| `NSOpenPanel`/`NSSavePanel` (:2132-2133) | Open…/Save File…/File In | dialog ops | `IFileOpenDialog`/`IFileSaveDialog` | ✅ planned (`Win_openFileDialog`) |
| `NSIndexSet` (:894,:3575) | programmatic row selection | list `set`/`select` cmd | `ListView_SetItemState` | ⚠️ partial (no "select row N" cmd) |
| `NSApplication`/`NSProcessInfo` (:325,:1255) | activate, app name | host | — | ✅ host |
| `NSColor`/`NSFont` (:158-160,:213) | fonts + muted/secondary colours | `font`/`color` props | DWrite / `SetFont` | ✅ partial (prop set) |
| `NSBezierPath`/`NSImage`/`NSData`/`NSDictionary` (:3202,:3234) | demo draw + snapshot PNG | canvas (S5) / snap op | D2D geometry + WIC | ✅ S5 scope |

**Net-new ViewServer work S7 forces on S4 (add these `case`s / capabilities):**

1. **A container/deck concept** — the 9 tabs and the ~15 sub-panes are `NSView`
   containers today. The protocol is flat (one surface, absolute frames). Either
   (a) add a `group`/`pane-child` widget kind that is a `WS_CHILD` HWND other
   widgets can be parented to and shown/hidden as a unit, or (b) keep flat and
   let the chrome clear+re-describe the active tab's widgets on every `switchTab`
   (§4.3). Decide in S7 — recommendation (b) for bring-up.
2. **A `splitter`** (§4.2) — or ship fixed panes (compute frames language-side,
   no drag) for bring-up and defer draggable dividers.
3. **`button` image prop** — so the 8 toolbar view-switchers can be icons; trivial
   (text buttons work meanwhile — the socket already addresses them by title).
4. **A "select row N" list command + a per-widget geometry read** — small
   `set`/pull additions (`NSIndexSet` selection, `frame()`/`selectedRange()`).

Everything else in the table is a `case` the S4 materializer already anticipates
(`win_view.h:21-24`) — S7 fills the skeleton body, it does not invent the kind.

---

## 3. The re-host mechanics — `dart:cocoa` idioms → `describe-widget` protocol

The mechanical translation is four rules, then four worked sketches.

**The four rules.**
- **create+frame+addSubview → one `add`+`place`.**
  `Cocoa.cls("NSButton").alloc().initWithFrame(f); …; parent.addSubview(b)`
  becomes `ui.button(id, title:…, frame:f, onClick:…)` — which appends
  `['add','button',id,props]` + `['place',id,f]` to the batch (`win.dart:76-79,110-116`).
  A `buildChrome` that today issues N `addSubview` calls becomes N `ui.*` calls
  then one `ui.commit()` (`win.dart:121-125`) — the whole build materialises
  atomically, like `gpApply`.
- **target-action → a closure keyed by ticket.** `onAction(b, fn)`
  (`cocoa.dart:162`) → the `onClick:` argument; the closure is held Dart-side in
  `Ui._handlers[ticket]` (`win.dart:78`), the native side holds only the ticket,
  and the click round-trips `WM_COMMAND → ticket → _winDispatch(kind 0)`.
- **table data-source → a pull list.** `onTable(t, rowCount, cellAt, onSelect)`
  (`cocoa.dart:185`) → `ui.list(id, rowCount:…, cellAt:…, onSelect:…)`
  (`win.dart:94-98`); `LVS_OWNERDATA` pulls kinds 2/3/4 exactly as `NSTableView`
  pulled its data source. `reloadData()` → `ui.set(id,{'rows':n})`.
- **`NSTextStorage` spans → `applySpans`.** `applySpans(tv, lexDart(text))`
  (`cocoa.dart:282`, `workspace.dart:1106`) → `ui.applySpans(editorId, runs)`
  (`win.dart:134`) → `Win_editorApplySpans` → RichEdit `EM_SETCHARFORMAT` per run.
  **The Dart-facing contract and the `lexDart` producer are byte-identical** — the
  flat `[start,len,kind,…]` list and the kind numbers (1 keyword … 5 type) are
  unchanged, so the entire highlighter (`lexDart`, `highlightView`,
  `workspace.dart:1024-1109`) ports verbatim.

### Sketch A — a toolbar view-switcher (`iconButton`, workspace.dart:131-147)

```
// BEFORE (dart:cocoa)                          // AFTER (dart:win / Ui)
var b = Cocoa.cls("NSButton").alloc()           ui.button("Workspace",
    .initWithFrame(frame);                          title: "Workspace",
b.setTitle(title); b.setBordered(false);            frame: [8,6,36,32],
var img = Cocoa.cls("NSImage").alloc()              onClick: (_) => defer(() => switchTab(0)));
    .initWithContentsOfFile(...svg);            // (icon via a later `image` prop;
if (!img.isNil){ img.setTemplate(true);        //  ships as a text button meanwhile —
  b.setImage(img); b.setImagePosition(1);}      //  the control plane already
parent.addSubview(b);                           //  addresses it by title, :1975)
gButtons[title] = b;
gTargets.add(onAction(b, (s)=>defer(...)));
```
The `gButtons`/`gTargets` bookkeeping (keep-alive maps) **disappears** — the
ViewServer owns the HWND and the `Ui` owns the handler map.

### Sketch B — the class-browser table (`tableIn`+`onTable`, workspace.dart:497-518,633-634)

```
// BEFORE                                        // AFTER
gClassTable = tableIn(classPane, frame);         ui.list("classes", frame: frame,
gTargets.add(onTable(gClassTable,                     rowCount: () => gBrClasses.length,
  () => gBrClasses.length,                            cellAt:  (r) => gBrClasses[r].toString(),
  (r) => gBrClasses[r].toString(),                    onSelect: sel(selectClass));
  sel(selectClass)));                            // …and on data change:
gClassTable.reloadData();                        ui.set("classes", {"rows": gBrClasses.length});
```
`gBrClasses`/`selectClass` and the whole browser state machine
(`selectCategory`/`selectClass`/`filterMembers`/`selectMemberRec`,
`workspace.dart:678-748`) are **unchanged logic** — only the four `tableIn`
constructions and their `reloadData` calls move to the protocol.

### Sketch C — the editor with live highlighting (workspace.dart:364-366,1104-1109)

```
// BEFORE                                        // AFTER
gEditor = scrolledTextView(ws, frame, true);     ui.editor("editor", frame: frame,
anchorScroll(gEditor, …);                            onText: () => highlight());
gTargets.add(onTextChange(gEditor,               // highlight() unchanged except the sink:
    (s) => highlight()));
void highlight() =>                              void highlight() => ui.applySpans("editor",
  applySpans(gEditor,                                lexDart(ui.textOf("editor")));
    lexDart(gEditor.string().UTF8String()));
```
`lexDart` is untouched; `gEditor.string().UTF8String()` (a synchronous handle
read) becomes `ui.textOf("editor")` (a `Win_widgetText` pull), and
`currentCode()`'s `selectedRange()` (`:1114`) needs a selection-read pull op
(§6). The RichEdit backend makes the span apply real; the Dart is oblivious.

### Sketch D — the App pane materializer collapses into the protocol

Today the chrome is *itself* an app-materializer: it receives `['appui', …]`
pushes from the language isolate and builds NSViews (`onAppPush`/`appApply`/
`appAdd`/`appSet`/`appRemove`, `workspace.dart:3694-3779`, ~120 lines of
`Cocoa.cls("NSButton")…`). Under the view-server, **the ViewServer materialises
app widgets in C++**, so this collapses to a near pass-through: the app-pane's
`['appui']` vocabulary (`['add',kind,id,props]`/`['set']`/`['remove']`,
`APP_PANE_PLAN.md` §4) is *already the same vocabulary* as `Win_surfaceApply`'s
(`S4_GUI_HOST_DESIGN.md` §4.2). `appApply` becomes:

```
void onAppPush(List msg) => _appSurface.applyRaw(msg[3]);   // forward to the app surface
```
plus the coordinate normalisation `_appFrame`/`_appAlign` (`:3662-3677`), which
the protocol's top-left-no-flip rule (`S4_GUI_HOST_DESIGN.md` §2.1) actually
*deletes* (Cocoa needed the height-flip; Win32 child coords are already top-left).
So ~120 lines of NSView materialization become ~10 lines of forwarding + the
ViewServer owns it. This is the single biggest LOC win of the whole sprint.

### LOC accounting (of the 4,853)

| bucket | approx lines | what happens |
|---|---|---|
| **Logic — unchanged** | ~2,900 | debugger vm-service client (`:2328-3037`, ~700); lexer+formatter (`:1024-2326` core, ~250); compile gate + watchdog + spawn/respawn (`:1377-1618, 4257-4560`, ~500); control-plane `handle()` (`:1630-1999`, most of ~370); help/demo pacer *logic* (`:3084-3596`, ~250); browser/editor/find/app *state machines*; `defer`/`ask`/`askQuiet` |
| **UI materialization — rewritten** | ~1,250 | `buildChrome`+helpers (`:104-400`, ~300); the 8 `build*Tab` fns (~450); `tableIn`/`browserPane`/`paneButtons`/`splitView` (~120); `buildMenu` (~125); `rebuildUi`/`snapshot` (~70); the widget-poking bodies of ~30 control verbs |
| **Deleted** | ~300 | `cocoaLint`+`_cocoaTokens`+`_reconSelector`+`_lintSend` (`:4379-4531`, ~150 — no selectors to lint on Windows, `win.dart:6-9`); the app-pane NSView materializer (`appApply`/`appAdd`/…, ~120, collapses per Sketch D); `autoreleasePool`, `setSelectorAction` responder-chain trick (`:1235-1243`, replaced by host menu `WM_COMMAND`) |
| **Import/accessor churn** | ~400 (touched, not rewritten) | `import 'dart:cocoa'`→`'dart:win'`; ~40 synchronous handle reads → pull ops; `Cocoa gX`→`String gXId` global re-typing; 26 `reloadData()`→`set(rows:)` |

Net: **~30% rewritten, ~6% deleted, the rest logic that ports with churn.**

---

## 4. The hard parts, ranked

### 4.1 The syntax-highlighting code editor — the crux (same as the mac risk #1)

Five text surfaces use `NSTextView`: the Workspace editor (`gEditor`), the
Editor-tab class editor (`gEdText`), the browser source pane (`gBrowserSrc`), and
three read-only ones — transcript (`gTranscript`), docs (`gHelpText`), debug
source (`gDbgSrc`). The read-only three ship on RichEdit read-only trivially. The
**editing three need syntax spans**, and that is the hard widget
(`S4_GUI_HOST_DESIGN.md` §4.7, `WINDOWS_PORTING_PLAN.md` §5, §9-risk-1).

- **Bring-up: RichEdit** (`MSFTEDIT_CLASS`), `EM_SETCHARFORMAT` per run behind
  `Win_editorApplySpans`. The `applySpans` contract (`win.dart:131-135`) hides the
  backend from Dart, so `lexDart` + `highlightView` are byte-for-byte unchanged.
  **Watch:** RichEdit's UTF-16 offsets must match Dart string indices — they do
  (both UTF-16, the same win the mac port banked, `WORKSPACE_PLAN.md` §4), so the
  span offsets need no remap. RichEdit's own undo/selection/`EM_EXSETSEL` model
  is quirky; the `clearUndo` discipline (`workspace.dart:310-313`) maps to
  `EM_EMPTYUNDOBUFFER`.
- **Finished: Direct2D/DirectWrite custom view** — full control, "pure DirectX,"
  but a substantial build (text layout, caret, selection, scroll, IME,
  hit-test). The `S4_design_decisions.md` §1 verdict is **RichEdit through S7,
  D2D deferred to polish** — do not build the D2D editor first.
- **Selection reads** (`currentCode` `:1113-1119`, `dbgCaretLine` `:2743-2752`,
  `_selectLine` `:4689-4701`) need a selection get/set pull op the S4 catalog
  does not yet have. Small, but on the S7 critical path (Do-It-on-selection is a
  headline gesture).

### 4.2 The class browser — 5 panes, 4 data sources, nested split views

`buildBrowserTab` (`workspace.dart:560-640`) is the densest UI in the file: two
nested `NSSplitView`s (`vsplit` vertical over `hsplit` of 4 columns), four
`NSTableView` data sources (Categories/Classes/Variables/Methods), per-column
`+/−` button pairs anchored with autoresize masks, an instance/class toggle, and
the Comment/Definition/Source mode row over the editable source pane. Two forces:

- **The split views have no protocol equivalent.** There is no `splitter` kind
  (`win_view.h:21-24`). Four split views exist across the chrome (browser
  vsplit+hsplit, debug split+rsplit, docs split, `workspace.dart:565-566,2672,2681,3471`),
  each with `setPosition`/`adjustSubviews`/`setSplitMinSize`. **Recommendation:**
  for S7 bring-up, drop draggable dividers — compute the pane frames
  language-side (absolute `[x,y,w,h]`, which the layout is already expressed in)
  and ship fixed panes. Draggable splitters become either a net-new `splitter`
  widget (a divider HWND that reports drag deltas → Dart re-describes child
  frames) or a D2D custom divider, deferred to polish. Note the
  `setSplitMinSize` native (`cocoa.dart:267-274`) exists *specifically* because
  the divider callback fires per drag-frame and must not round-trip into Dart —
  so a Dart-reflowing splitter reintroduces exactly the latency that native was
  built to avoid. This argues for a self-contained C++ splitter, not a Dart one.
- **The 4 data sources are `list` widgets** and port cleanly (Sketch B). The
  browser *state machine* (`selectCategory`→`selectClass`→`filterMembers`→
  `selectMemberRec`, the `gBr*` globals, `updateSourcePane`, `browserAccept`,
  `:678-970`) is **pure logic, unchanged** — it manipulates Dart lists and issues
  `ask()` calls to the language isolate; only the `reloadData`/`setString`/
  `selectRowIndexes` sinks change.

### 4.3 The tabless tab deck — no "card container" in the protocol

`gTabView` is an `NSTabView` of **type 6 (NoTabsNoBorder)** with 9 tab bodies
(`workspace.dart:353-388`); the toolbar buttons are the visible tab bar and
AppKit owns the view swap/clip/repaint. `switchTab(i)` (`:277-304`) calls
`selectTabViewItemAtIndex` and AppKit shows one body, hides the rest. **The flat
view-server protocol has no group/deck.** Options:

- **(b, recommended for bring-up) clear + re-describe.** On `switchTab`, the
  chrome `ui.clear()`s the content region and re-describes only the active tab's
  widgets. All tab state (editor buffer, browser selection, dbg breakpoints)
  already lives in Dart globals, so re-describing is stateless and cheap — and
  the chrome already tears down and rebuilds freely (`rebuildUi`, `:4080`). This
  fits the `['clear']`/`['add']` vocabulary with zero new C++.
- **(a) a `group`/child-pane kind** — a `WS_CHILD` HWND per tab that other
  widgets parent to, shown/hidden as a unit (`ShowWindow`). Cleaner runtime
  (widgets persist across tab switches, no rebuild) but net-new ViewServer
  capability (parenting, per-group show/hide). Defer to polish unless (b)'s
  per-switch rebuild proves too flickery.

The transcript dock (`gTranscript`, always visible below the deck, `:391-396`)
and the toolbar band stay described once; only the deck region churns.

### 4.4 The debugger tab — easy chrome over hard-but-portable logic

Counter-intuitively **low risk**: the debugger's engine — the vm-service
WebSocket client, JSON-RPC, breakpoint anchoring to `(decl, offset)`, the token
table line resolution, conditional breakpoints, the pause/step state machine
(`workspace.dart:2328-3037`, ~700 lines) — is **`dart:io` + `dart:developer`,
zero AppKit, ports verbatim.** Only `buildDebugTab`'s ~10 widgets
(`:2637-2701` — a popup, 8 buttons, an eval field, source pane, 2 tables in split
views) re-host, exactly like any other tab. The one dependency: it needs the
vm-service reachable on Windows (`ws://127.0.0.1:8181/ws`) — a launch-flag
concern, not a chrome one.

### 4.5 Live metrics + demo canvas + toolbar texture — cosmetic sub-risks

- **Metrics cluster** (`buildMetricsCluster`/`buildMemBar`, `:154-194`): a
  right-anchored MEM/JIT/CODE/GC readout with a 2px used/capacity bar drawn as
  two coloured `NSBox`es. The labels are `label` widgets (fine); the coloured bar
  wants either a `progress` control or a small D2D fill. Ship as text-only for
  bring-up; the `renderMetrics` logic (`:452-470`) is unchanged (it pulls
  `vmstats` over the port).
- **Demo canvas** (`gDemoView`/`renderDemo`, `:3076-3264`): `NSImageView` +
  `NSBezierPath` replay → the S5 Direct2D `canvas` + `Win_canvasBlit`. This is
  **S5 scope, not S7** — S7 just describes a `canvas` widget where the image view
  was. The demo *pacer* (`gDrawClock`, pull ticks, `keyState`, `:3084-3430`) is
  unchanged logic.
- **Textured toolbar + icon buttons**: pattern-image `NSBox` and template SVG
  icons. Ship as a plain box + text buttons; D2D texture/icons are polish.

---

## 5. A phased S7 plan — incrementally runnable slices

Each phase ends at something you can launch and drive over the control plane, so
S7 is never a 4,853-line big-bang. The order front-loads the shell and the
cheapest-to-materialise tabs, defers the editor and the debugger.

**S7.0 — Port `language.dart` + the neutral logic (no UI).** Swap
`import 'dart:cocoa'`→`'dart:win'` in `language.dart`; confirm `wsEval`/`wsReload`/
`Db`/`wsVmStats` resolve. Port the platform-neutral halves of `workspace.dart`
that have no widgets: `lexDart`/formatter, `splitTopLevel`/name parsing,
`ask`/`askQuiet`/watchdog/`spawnLanguage`, `compileCheck`/`guardedAccept` (point
`_analyzeBinary` at `dart.exe`), `registerServiceExtensions`. **Exit:** the
control plane answers `ping`; a headless `doit`/`accept` round-trips to the
language isolate with no window yet (or a bare window). Delete `cocoaLint`.

**S7.1 — Shell + tabs (the deck).** `buildWindow`/`buildChrome`/`buildMenu`, the
toolbar view-switchers (text buttons), the tabless deck via clear+re-describe
(§4.3), the transcript dock, `switchTab`, `log`/`repaint`. **Exit:** `dartui.exe`
opens the workspace window; the 8 toolbar buttons switch tabs; the transcript
logs; `tab N`/`menuclick`/`click <title>` drive it. (This is the S4 exit
milestone scaled to the real chrome.)

**S7.2 — The Workspace tab, read-then-write.** The editor as a RichEdit `text`
widget with `applySpans` highlighting (§4.1), Do It / Print It / Clear / Accept.
Wire `currentCode` via a selection-read pull op. **Exit:** type Dart, Print It →
result in the transcript; Accept → the language isolate hot-reloads (proves the
editor + the compile gate + the whole liveness loop on Windows).

**S7.3 — A read-only class browser.** The 4 `list` data sources + the source pane
(read-only first), fixed panes (no draggable dividers, §4.2), the
category→class→member state machine. **Exit:** browse User App + world classes,
select a class, see members and source. Then flip the source pane editable +
`browserAccept` for member/class edits.

**S7.4 — Editor tab, Find tab, Docs tab.** All three are lists + text + a popup +
buttons — the same kinds already standing. Editor tab adds the file dialogs
(`Win_openFileDialog`, `IFileOpenDialog`). Docs adds the help indexer isolate
(unchanged logic). **Exit:** Load/Save-to-Image, Open…/Save File…/File In, Format,
Analyze; Find + Senders; searchable help.

**S7.5 — App tab (unifies with S4).** The App pane becomes one `Ui` surface the
language isolate drives through the collapsed `onAppPush` forwarder (Sketch D).
This is where the chrome and the user-app view-server become literally the same
code path. **Exit:** install + run an `apps/` example; `apptree`/`appclick`/
`appget` drive it; Accept morphs the running app and re-runs `build()`.

**S7.6 — Debugger tab.** Materialise the ~10 widgets; the vm-service client is
already ported. **Exit:** Attach, breakpoint, pause/step, evaluate-in-frame,
against the language isolate, with the window live.

**S7.7 — Polish.** Draggable splitters (net-new `splitter` or D2D divider), the
D2D custom editor, toolbar texture + icon buttons, the metrics usage bar,
`rebuildUi` re-materialise + the ticket-trap test (`APP_PANE_PLAN.md` §7), the
Demos canvas (gated on S5/S6 landing).

---

## 6. Open questions for the architect

1. **Tab deck: clear+re-describe vs a `group` widget kind?** (§4.3) Bring-up
   wants (b) clear+re-describe (zero new C++, fits `['clear']`/`['add']`). A
   persistent child-pane `group` is nicer at runtime but net-new ViewServer
   capability. Recommendation: (b) now, revisit if per-switch rebuild flickers.

2. **Splitters: fixed panes now, and then a C++ splitter or a D2D divider?**
   (§4.2) The protocol has no splitter and the `setSplitMinSize` history
   (`cocoa.dart:267-274`) proves the divider callback must not round-trip into
   Dart — so a draggable splitter should be **self-contained C++**, not a
   Dart-reflowing one. Confirm fixed panes are acceptable for the S7 functional
   milestone, splitters as polish.

3. **The editor backend commitment.** RichEdit through S7, D2D custom editor as
   polish (per `S4_design_decisions.md` §1) — confirm S7 is allowed to ship on
   RichEdit and is not blocked on the D2D editor. The `Win_editorApplySpans`
   contract makes the later swap invisible to Dart, so this is a scheduling
   decision, not an architectural one.

4. **The missing pull ops.** The chrome reads widget state synchronously in ~40
   places. The S4 catalog has `Win_widgetText`/`Win_measureText`/`Win_surfaceSize`
   but **not** a per-widget **frame/geometry read** (`b.frame()`, `bounds()`,
   `:1784,:3657`) nor a **text-selection get/set** (`selectedRange`/
   `setSelectedRange`, `:1114,:2744,:4699`). Selection get/set is on the S7.2
   critical path (Do-It-on-selection). Add these to `win.dart`/`win_natives.cpp`
   as `Win_widgetSelection`/`Win_setSelection` and a `Win_widgetFrame`. Decide
   whether the geometry reads are even needed post-port (much of `frames`/
   `apptree` reflects what the app *described*, which Dart already knows).

5. **SQLite image on Windows — the `Db` natives are stubbed.** `language.dart`
   opens the image at boot (`language.dart:38-43`) via `Db`/`Sqlite_*`
   (`win.dart:211-224`), and `WINDOWS_PORTING_PLAN.md` §7 lists `sqlite_natives.cc`
   as reused-as-is with a bundled SQLite amalgamation — but **is that native
   linked into `dartui.exe` yet?** If not, the image path degrades to
   in-memory-only (accepts don't persist across launches) until it is. The image
   is the source of truth for the whole Accept/respawn model, so this is a real
   S7 dependency, not polish. Path also moves: `~/.macdart/workspace.sqlite`
   (`workspace.dart:4836`) → `%LOCALAPPDATA%\windart\workspace.sqlite`, and
   `Platform.environment['HOME']` → a Windows-aware home (`USERPROFILE`/
   `LOCALAPPDATA`) in both files.

6. **Does any chrome piece need a capability the protocol lacks?** Two beyond the
   deck/splitter/selection above: (a) the **snapshot** verb
   (`snapshot`, `:1620-1628`) uses `bitmapImageRepForCachingDisplayInRect` to PNG
   the window offscreen — the headless drive-and-see loop the whole project rests
   on (`WORKSPACE_PLAN.md` §2); Windows needs an equivalent (`WM_PRINT`/
   `PrintWindow` to a DIB, or a D2D render-target readback) exposed as a native op
   — **it is not in the S4 catalog** and the regression harness depends on it. (b)
   The **`image`/icon prop on `button`** for toolbar icons (cosmetic).

7. **`Platform.script.resolve(...)` asset/demo/app paths.** The chrome resolves
   `assets/`, `demos/`, `apps/`, `help/indexer.dart`, and the SDK lib for help
   relative to the script (`:107,:3266,:3510,:3934`). Under a relocatable
   `windart` bundle (`WINDOWS_PORTING_PLAN.md` §8 W6) these need a defined layout;
   confirm the bundle layout before S7.4 (Docs/help) and S7.5 (Apps menu).

---

## The three hardest S7 risks (summary)

1. **The syntax-highlighting code editor (§4.1).** The crux, identical to the mac
   port's #1 risk. RichEdit gets a working editor fast but is quirky on
   undo/selection and is not the IDE's look; the D2D custom view is a large build.
   Mitigated by the `Win_editorApplySpans` contract making the backend swap
   invisible to Dart, and by staging RichEdit→D2D. On the critical path because
   three tabs and Do-It-on-selection depend on it.

2. **Structural widgets the protocol doesn't have — the tab deck and the split
   panes (§4.2, §4.3).** The chrome's whole spatial structure is `NSTabView` +
   nested `NSSplitView` + `NSView` containers, and the flat view-server has none
   of them. Bring-up answer (clear+re-describe deck; fixed language-side panes) is
   sound and needs no new C++, but the *finished* form (persistent groups,
   draggable self-contained splitters) is net-new ViewServer capability that must
   be scoped, and the `setSplitMinSize` history warns the splitter must stay out
   of the Dart round-trip.

3. **The load-bearing infrastructure that isn't a widget: the SQLite image and
   the offscreen snapshot (§6.5, §6.6).** Both are stubbed/absent on Windows
   today and both are foundational — the image *is* the Accept/persist/respawn
   source of truth, and the offscreen PNG snapshot is the headless drive-and-see
   loop the entire regression discipline depends on. Neither is "chrome," so both
   are easy to under-scope, and S7 cannot actually be verified the project's own
   way (`snap` → `Read` the image) until the snapshot native exists.
