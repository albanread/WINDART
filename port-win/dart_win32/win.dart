// dart:win — WINDART's native Windows bridge (the view-server client side).
//
// Mirrors dart:cocoa's SHAPE (macdart/cocoa/cocoa.dart) but DELETES the dynamic
// selector bridge: there is no `noSuchMethod` -> objc_msgSend, no `import
// dart:mirrors`, no `_send`, no @encode classifier, and no Accept-time
// selector-lint natives (cocoa.dart:62-78, :326-387) — because there is no
// selector to misspell. The Windows surface is a FIXED catalog of concrete ops
// (S4_GUI_HOST_DESIGN.md §5). Wired like dart:io/dart:cocoa; natives bound by
// WinNativeLookup.
library dart.win;

// ── Workspace liveness primitives — REUSED AS-IS (cocoa.dart:12-52) ─────────
// (Bodies are workspace_natives.cc / the embedder patch — OS-neutral, unchanged.)
int    processId()                 native "Win_getpid";
String wsEval(String src)          native "Workspace_eval";
String wsReload()                  native "Workspace_reload";
String wsCheckSyntax(String src, String className) native "Workspace_checkSyntax";
String wsBakeSnapshot()            native "Workspace_bakeSnapshot";
String wsRollbackSnapshot()        native "Workspace_rollbackSnapshot";
String wsExportWorld(String dir)                 native "Workspace_exportWorld";
String wsImportWorld(String inDir, String outDb) native "Workspace_importWorld";
List   wsVmStats()                 native "Workspace_vmStats";
String wsRequestUiReload()         native "Workspace_requestUiReload";
String wsUiReloadStatus()          native "Workspace_uiReloadStatus";
String wsUiReady()                 native "Workspace_uiReady";
int    wsMenuPoll()                native "Workspace_menuPoll";        // menu/toolbar cmd id, -1 = none
String wsFireCommand(int id)       native "Workspace_fireCommand";     // synth toolbar WM_COMMAND (test)
String wsResizeWindow(int w, int h) native "Workspace_resizeWindow";  // resize main window (test)
String wsDragWidget(int ticket, int dx, int dy) native "Workspace_dragWidget"; // synth splitter drag (test)
String wsSnapshotFull(String path) native "Workspace_snapshotFull";   // full-window PNG (menu+toolbar)

// ── Debugger (T4) — the classic in-process embedder debug API, driven from the
// Debug tab. Arm BEFORE spawning the target isolate; poll for the captured
// session (stack / paused line / frame-eval / step / done). ─────────────────
// stepKind: 0 step-over, 1 step-into, 2 step-out, 3 resume (no step) at the hit.
String wsDebugArm(int breakLine, String evalExpr, int stepKind) native "Workspace_debugArm";
List   wsDebugPoll()                              native "Workspace_debugPoll";
void   wsDebugDone()                              native "Workspace_debugDone";

// ── View-server natives (the op catalog, win_natives.cpp §2.2) ──────────────
int    _surfaceOpenPane(int w, int h)                 native "Win_surfaceOpenPane";
int    _surfaceOpenWindow(String title, int w, int h) native "Win_surfaceOpenWindow";
void   _surfaceClose(int surface)                     native "Win_surfaceClose";
String _surfaceApply(int surface, int gen, List cmds) native "Win_surfaceApply";
List   _surfaceSize(int surface)                      native "Win_surfaceSize";
String _widgetText(int ticket)                        native "Win_widgetText";
List   _editorSelection(int ticket)                   native "Win_editorSelection";
void   _editorSelectLine(int ticket, int line)        native "Win_editorSelectLine";
List   _measureText(String s, String font)            native "Win_measureText";
void   _editorApplySpans(int ticket, List runs)       native "Win_editorApplySpans";

// Canvas (S5): replay a demo draw-command list to a Direct2D target; PNG readback.
void   _canvasDraw(int ticket, List cmds)             native "Win_canvasDraw";
String _canvasSnap(int ticket, String path)           native "Win_canvasSnap";
String _surfaceSnapshot(int surface, String path)     native "Win_surfaceSnapshot";
void   _hostQuit()                                    native "Win_hostQuit";
void   _setStatus(String s)                           native "Win_setStatus";
void   _pushToolbarMetric(double v, String label)     native "Win_pushToolbarMetric";

// Game pane (S6): the Direct3D 11 retro engine — the same 7-native shape as
// dart:cocoa (gp_natives_win.cpp). A whole frame of gp verbs crosses in one
// _gpApply; _gpSnap reads honest offscreen pixels to a PNG.
int    _gpOpen(int w, int h, int ww, int wh, int mode) native "Win_gpOpen";
void   _gpClose()                                      native "Win_gpClose";
String _gpApply(List cmds)                             native "Win_gpApply";
String _gpSnap(String path)                            native "Win_gpSnap";
String _gpSnapPresent(String path)                     native "Win_gpSnapPresent";
List   _gpStat()                                       native "Win_gpStat";
void   _gpFullscreen(int on)                           native "Win_gpFullscreen";

// Dialogs.
String _openFileDialog(String optsJson)               native "Win_openFileDialog";
String _saveFileDialog(String optsJson)               native "Win_saveFileDialog";
int    _messageBox(String title, String body, int kind) native "Win_messageBox";

// ── The view-server client — `Ui` ───────────────────────────────────────────
// The `build(ui)` API is byte-identical to APP_PANE_PLAN.md §3; only the backing
// native differs. Layout (grid/row/column) is pure Dart arithmetic producing
// absolute top-left frames — the wire carries absolute frames only, no flip
// (APP_PANE_PLAN.md §6, S4_GUI_HOST_DESIGN.md §4.5). Handlers are closures held
// HERE, keyed by widget id; the native side holds only the integer ticket.
class Ui {
  final int _surface;          // native surface handle
  int _gen = 0;                // generation, bumped each rebuild
  final List _cmds = <dynamic>[];             // pending batch
  final Map<String, int> _ticketOf = <String, int>{};   // id -> ticket
  final Map<int, Function> _handlers = <int, Function>{}; // ticket -> handler
  Function _onResize;          // void f(int w, int h) — surface (kind 7) events
  Function _onClose;           // void f()             — surface (kind 8) events
  // Tickets == Win32 child control-ids, carried in WM_COMMAND's LOWORD(wParam),
  // so they must be < 65536 and disjoint from the host's menu-command id block
  // (101..120). Start high enough to clear it.
  int _nextTicket = 0x100;

  // WINDART S4 slice: on construction, become the active Ui and ensure the
  // native callback dispatcher is registered, so a materialized widget's event
  // round-trips to this Ui's handlers. (The design notes _activeUi is per-Ui in
  // the real workspace; flat is fine for the single-surface slice.)
  Ui._(this._surface) {
    _activeUi = this;
    _ensureDispatch();
  }

  /// Open the app on an embedded pane, or a real window. The app cannot tell
  /// them apart (APP_PANE_PLAN.md §2).
  factory Ui.pane(int w, int h)   => new Ui._(_surfaceOpenPane(w, h));
  factory Ui.window(String t, int w, int h) => new Ui._(_surfaceOpenWindow(t, w, h));

  int get width  { var wh = _surfaceSize(_surface); return wh[0]; }
  int get height { var wh = _surfaceSize(_surface); return wh[1]; }

  // ── Describe (batched; flushed by build()'s caller via commit()) ──────────
  void clear()             { _cmds.add(['clear']); _ticketOf.clear(); _handlers.clear(); }
  void title(String text)  { _cmds.add(['title', text]); }
  void focus(String id)    { _cmds.add(['focus', id]); }

  void label(String id, {String text: '', List frame}) =>
      _add('label', id, {'text': text}, frame);

  void field(String id, {String text: '', List frame, String align: 'left',
                         bool readOnly: false, Function onText, Function onEnter}) {
    var t = _add('field', id, {'text': text, 'align': align, 'readOnly': readOnly}, frame);
    if (onText != null)  _handlers[t] = onText;   // kind 1
    if (onEnter != null) _handlers[t] = onEnter;  // kind 5 (see _winDispatch)
  }

  void button(String id, {String title: '', List frame, Function onClick}) {
    var t = _add('button', id, {'title': title}, frame);
    if (onClick != null) _handlers[t] = onClick;  // kind 0
  }

  void checkbox(String id, {String title: '', bool checked: false, List frame,
                            Function onToggle}) {
    var t = _add('checkbox', id, {'title': title, 'checked': checked}, frame);
    if (onToggle != null) _handlers[t] = onToggle; // kind 6
  }

  void popup(String id, {List items: const [], List frame, Function onSelect}) {
    var t = _add('popup', id, {'items': items}, frame);
    if (onSelect != null) _handlers[t] = onSelect; // kind 4
  }

  /// A data-driven list (LVS_OWNERDATA, pull-based — like NSTableView + onTable,
  /// cocoa.dart:185). rowCount()/cellAt(row)/onSelect(row) are pulled by the host.
  void list(String id, {List frame, int rowCount(), String cellAt(int row),
                        void onSelect(int row)}) {
    var t = _add('list', id, {'rows': rowCount == null ? 0 : rowCount()}, frame);
    _handlers[t] = new _ListSource(rowCount, cellAt, onSelect); // kinds 2/3/4
  }

  /// The code editor (RichEdit bring-up -> Direct2D, S4_GUI_HOST_DESIGN.md §4.7).
  void editor(String id, {String text: '', List frame, Function onText}) {
    var t = _add('text', id, {'text': text}, frame);
    if (onText != null) _handlers[t] = onText;
  }

  /// A Direct2D canvas (S5). `frame` = [x,y,w,h]; w,h size the render target.
  /// Demos push ['draw', cmds] over an isolate port; the UI isolate replays them
  /// via canvasDraw() and can PNG-snapshot via canvasSnap().
  void canvas(String id, {List frame}) {
    var w = (frame != null && frame.length > 2) ? frame[2] : 0;
    var h = (frame != null && frame.length > 3) ? frame[3] : 0;
    _add('canvas', id, {'w': w, 'h': h}, frame);
  }

  /// A D3D11 game pane that LIVES on screen (T3). `frame` = [x,y,w,h]; the child
  /// HWND created here becomes the target of the game engine's DXGI swapchain, so
  /// every gpApply frame is presented into it. Open the engine (gpOpen) after the
  /// widget is committed; the swapchain binds on the first present.
  void game(String id, {List frame}) {
    var w = (frame != null && frame.length > 2) ? frame[2] : 0;
    var h = (frame != null && frame.length > 3) ? frame[3] : 0;
    _add('game', id, {'w': w, 'h': h}, frame);
  }

  /// Replay a demo draw-command list against canvas [id]'s Direct2D target.
  void canvasDraw(String id, List cmds) => _canvasDraw(_ticketOf[id], cmds);

  /// Encode canvas [id]'s current pixels to a PNG file. Returns "" or an error.
  String canvasSnap(String id, String path) => _canvasSnap(_ticketOf[id], path);

  /// Unified surface snapshot (S6): PNG-readback this surface's canvas (and,
  /// once it exists, its game pane). The one primitive the S7 regression loop uses.
  String snapshot(String path) => _surfaceSnapshot(_surface, path);

  /// A labeled group frame (S7). `frame` = [x,y,w,h].
  void box(String id, {String title: '', List frame}) =>
      _add('box', id, {'title': title}, frame);

  /// A user-draggable pane divider (P2). `orientation` 'vertical' (a vertical bar
  /// that moves horizontally, IDC_SIZEWE) or 'horizontal' (moves vertically,
  /// IDC_SIZENS). `between: [leftId, rightId]` names the two neighbor widgets the
  /// drag resizes. Dragging is handled ENTIRELY in C++ (no per-move round-trip);
  /// the neighbors are resolved by id at drag time, so declare it after them.
  void splitter(String id, {String orientation: 'vertical', List frame,
                            List between: const []}) {
    var l = between.length > 0 ? between[0] : '';
    var r = between.length > 1 ? between[1] : '';
    _add('splitter', id, {'orientation': orientation, 'left': l, 'right': r}, frame);
  }

  /// A tab strip (S7 tab-deck bring-up). Selecting a tab dispatches onSelect(i);
  /// the chrome then clear()s + re-describes the active tab's widgets.
  void tabs(String id, {List items: const [], List frame, Function onSelect}) {
    var t = _add('tabs', id, {'items': items}, frame);
    if (onSelect != null) _handlers[t] = onSelect;   // kind 4
  }

  /// The editor/field's current selection: [start, len, text] (S7 catalog gap 1;
  /// the Do-It-on-selection pull).
  List editorSelection(String id) => _editorSelection(_ticketOf[id]);
  /// Select (highlight) a 0-based line in an editor — used to point at the line a
  /// rejected Accept failed on (the syntax-check gate).
  void editorSelectLine(String id, int line) => _editorSelectLine(_ticketOf[id], line);

  /// Register a surface resize handler: `f(int w, int h)` fires with the new
  /// client size whenever the host window/pane is resized (kind 7). The handler
  /// recomputes + re-places the layout; pending describes are auto-committed.
  void onResize(void f(int w, int h)) { _onResize = f; }

  /// Register a surface close handler (kind 8): `f()` on an explicit window close.
  void onClose(void f()) { _onClose = f; }

  void set(String id, Map props)  { _cmds.add(['set', id, props]); }
  void place(String id, List xywh) { _cmds.add(['place', id, xywh]); }
  void remove(String id) {
    _cmds.add(['remove', id]);
    var t = _ticketOf.remove(id);         // keep widgetIds accurate: a user app's tab
    if (t != null) _handlers.remove(t);   // clearing diffs widgetIds before/after its
  }                                       // build(); a stale id breaks re-visit removal


  int _add(String kind, String id, Map props, List frame) {
    var ticket = _nextTicket++;
    _ticketOf[id] = ticket;
    // The ticket rides in the 'add' command so the materializer creates the
    // control with control-id == ticket == this handler key (the single mapping
    // that makes WM_COMMAND -> ticket -> _winDispatch route to onClick).
    _cmds.add(['add', kind, id, ticket, props]);
    if (frame != null) _cmds.add(['place', id, frame]);
    return ticket;
  }

  /// The native ticket (== Win32 control-id) bound to a described widget id.
  int ticketOf(String id) => _ticketOf[id];

  /// All widget ids currently described on this surface (used to track the ids a
  /// user app's build() adds, so a tab switch can remove exactly them).
  List<String> get widgetIds => _ticketOf.keys.toList();

  /// True if there are queued describe commands awaiting commit().
  bool get hasPending => _cmds.isNotEmpty;

  /// Flush the described batch to the native materializer atomically. Returns ""
  /// or the first error (best-effort, like gpApply). The UI isolate calls this
  /// after build() returns.
  String commit() {
    var err = _surfaceApply(_surface, ++_gen, _cmds);
    _cmds.clear();
    return err;
  }

  /// The current text of a field/editor (pulled by its onText/onEnter handler;
  /// mirrors Cocoa's onTextChange handler reading sender.stringValue).
  String textOf(String id) => _widgetText(_ticketOf[id]);

  /// Colour an editor with syntax runs: flat [start, len, kind, …]
  /// (1 keyword, 2 string, 3 comment, 4 number, 5 type). Direct port of
  /// applySpans (cocoa.dart:282). Offsets are UTF-16 units (Dart string indices).
  void applySpans(String editorId, List runs) =>
      _editorApplySpans(_ticketOf[editorId], runs);

  void close() => _surfaceClose(_surface);
}

class _ListSource {
  final Function rowCount, cellAt, onSelect;
  _ListSource(this.rowCount, this.cellAt, this.onSelect);
}

// ── The single dispatcher every native callback funnels through ─────────────
// The exact counterpart of _cocoaDispatch (cocoa.dart:133-143), extended with
// kinds 5-8. Called by win_callbacks.cpp's Dispatch(). A dead ticket returns null
// (fails closed). Registered once via _registerWinDispatch.
//   kind: 0 click · 1 text · 2 rowCount · 3 cell(arg=row) · 4 select(arg=row)
//         5 enter · 6 toggle(arg=checked) · 7 resize(arg=w<<32|h) · 8 close
void _registerWinDispatch(Function f) native "Win_registerCallbackDispatch";
bool _dispatchRegistered = false;

// One global registry the dispatcher routes through. In the real UI isolate this
// is per-Ui (the running app); shown flat here for clarity.
Ui _activeUi;

dynamic _winDispatch(int ticket, int kind, int arg) {
  if (_activeUi == null) return null;
  // Pull kinds (row count / cell text) return a value and MUST NOT commit — they
  // fire during PrintWindow/paint, where an apply would be wrong.
  if (kind == 2 || kind == 3) {
    var h = _activeUi._handlers[ticket];
    if (kind == 2) return (h is _ListSource) ? h.rowCount() : 0;
    return (h is _ListSource) ? h.cellAt(arg) : '';
  }
  // Surface-level events (7 resize, 8 close) are keyed by the SURFACE ticket, not
  // a widget's — route them to the Ui's own handlers before the per-widget lookup
  // (which would fail closed on the surface ticket). arg for resize packs w,h.
  if (kind == 7) {
    var f = _activeUi._onResize;
    if (f != null) {
      f(arg >> 32, arg & 0xFFFFFFFF);
      if (_activeUi.hasPending) _activeUi.commit();
    }
    return null;
  }
  if (kind == 8) {
    var f = _activeUi._onClose;
    if (f != null) { f(); if (_activeUi.hasPending) _activeUi.commit(); }
    return null;
  }
  var h = _activeUi._handlers[ticket];
  if (h == null) return null;                      // fail closed (stale ticket)
  switch (kind) {
    case 0: h(); break;                             // click
    case 1: h(); break;                             // text changed (handler pulls textOf)
    case 5: h(); break;                             // field enter
    case 6: h(arg != 0); break;                     // toggle(checked)
    case 4:                                          // select(row/index)
      if (h is _ListSource) { h.onSelect(arg); } else { h(arg); }
      break;
    default: return null;                           // (7 resize / 8 close handled above)
  }
  // The app-pane model: a handler DESCRIBES changes (ui.set/add/...); auto-commit
  // them so the effect is visible without the app calling commit() itself.
  if (_activeUi != null && _activeUi.hasPending) _activeUi.commit();
  return null;
}

void _ensureDispatch() {
  if (!_dispatchRegistered) { _registerWinDispatch(_winDispatch); _dispatchRegistered = true; }
}

/// Forget every callback (a surface teardown). Stale tickets then fail closed in
/// _winDispatch — but note the native side must ALSO DestroyWindow the owned
/// controls (win_callbacks.cpp / ViewServer::ClearSurface), unlike Cocoa's
/// weakly-held targets (cocoa.dart:156-158, S4_GUI_HOST_DESIGN.md §3.2).
void disposeCallbacks() { _activeUi?._handlers?.clear(); }

/// Tell the host the UI is up (so a later UI-isolate error is survivable rather
/// than fatal — win_host.cpp §1.4). Call once after the first surface commits.
void uiReady() => wsUiReady();

/// Ask the host to close cleanly (post WM_CLOSE). For headless/one-shot runs.
void hostQuit() => _hostQuit();

/// Set the native status-bar text (the bottom band).
void wsSetStatus(String s) => _setStatus(s);

/// Push one sample into the toolbar's live metric graph, with a value label.
void wsPushToolbarMetric(double v, String label) => _pushToolbarMetric(v, label);

// ── Game pane (S6) — Dart-facing ────────────────────────────────────────────
/// Open the D3D11 game engine at logical [w]x[h] over a [ww]x[wh] world (world
/// defaults to the viewport). [mode] 0 = retained sprite/indexed stack, 1 =
/// direct framebuffer (§6b, deferred). Returns a non-zero handle; throws on
/// failure. Renders offscreen — gpSnap reads it back independently of any window.
int gpOpen(int w, int h, [int ww, int wh, int mode = 0]) =>
    _gpOpen(w, h, ww == null ? w : ww, wh == null ? h : wh, mode);
void gpClose() => _gpClose();
/// Apply one whole frame's gp verb list atomically; "" or the first error.
String gpApply(List cmds) => _gpApply(cmds);
/// PNG-snapshot the offscreen (logical pixels, screen line 0 at the TOP). "" or error.
String gpSnap(String path) => _gpSnap(path);
/// PNG-snapshot the LIVE on-screen swapchain backbuffer (the letterboxed present
/// image the game widget shows). Requires a `game` widget (T3). "" or error.
String gpSnapPresent(String path) => _gpSnapPresent(path);
/// [open, framesPresented, w, h, fullscreen, direct, stride].
List gpStat() => _gpStat();
void gpFullscreen(bool on) => _gpFullscreen(on ? 1 : 0);

// ── Dialogs (Dart-facing) ───────────────────────────────────────────────────
String openFile([String optsJson = '{}']) => _openFileDialog(optsJson);
String saveFile([String optsJson = '{}']) => _saveFileDialog(optsJson);
int    messageBox(String title, String body, [int kind = 0]) =>
    _messageBox(title, body, kind);

// ── Key-state poll — REUSED SHAPE (cocoa.dart:207-223) ──────────────────────
void _keyWatch()        native "Win_keyWatch";
void _keyCapture(int on) native "Win_keyCapture";
List _keyState()        native "Win_keyState";

/// Install the key poller (idempotent; on Windows a no-op — GetAsyncKeyState
/// polls directly — but kept so portable game Dart is unchanged).
void keyWatch() => _keyWatch();
/// While on, plain keys are consumed so a game owns the keyboard; Alt/Ctrl
/// shortcuts pass through. (S4_GUI_HOST_DESIGN.md §3.3.)
void keyCapture(bool on) => _keyCapture(on ? 1 : 0);
/// `[downKeycodes, modifierFlags]` — RIGHT NOW. The keycodes are the SAME macOS
/// virtual keycodes the portable game Dart already uses (left 123, right 124,
/// down 125, up 126, space 49, A 0, D 2); the C side remaps VK_ -> macOS so this
/// Dart needs no change (S4_GUI_HOST_DESIGN.md §3.3, delta 1).
List keyState() => _keyState();

// ── SQLite image store — REUSED AS-IS (cocoa.dart:286-304, sqlite_natives.cc) ─
int    _sqlOpen(String path)                   native "Sqlite_open";
void   _sqlClose(int db)                        native "Sqlite_close";
String _sqlExec(int db, String sql, List p)     native "Sqlite_exec";
List   _sqlQuery(int db, String sql, List p)    native "Sqlite_query";

class Db {
  final int _h;
  const Db._(this._h);
  factory Db.open(String path) => new Db._(_sqlOpen(path));
  bool get isOpen => _h != 0;
  String exec(String sql, [List params = const []]) => _sqlExec(_h, sql, params);
  List   query(String sql, [List params = const []]) => _sqlQuery(_h, sql, params);
  void   close() => _sqlClose(_h);
}
