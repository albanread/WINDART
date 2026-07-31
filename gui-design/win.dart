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
List   wsVmStats()                 native "Workspace_vmStats";
String wsRequestUiReload()         native "Workspace_requestUiReload";
String wsUiReloadStatus()          native "Workspace_uiReloadStatus";
String wsUiReady()                 native "Workspace_uiReady";

// ── View-server natives (the op catalog, win_natives.cpp §2.2) ──────────────
int    _surfaceOpenPane(int w, int h)                 native "Win_surfaceOpenPane";
int    _surfaceOpenWindow(String title, int w, int h) native "Win_surfaceOpenWindow";
void   _surfaceClose(int surface)                     native "Win_surfaceClose";
String _surfaceApply(int surface, int gen, List cmds) native "Win_surfaceApply";
List   _surfaceSize(int surface)                      native "Win_surfaceSize";
String _widgetText(int ticket)                        native "Win_widgetText";
List   _measureText(String s, String font)            native "Win_measureText";
void   _editorApplySpans(int ticket, List runs)       native "Win_editorApplySpans";

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
  int _nextTicket = 1;

  Ui._(this._surface);

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

  void set(String id, Map props)  { _cmds.add(['set', id, props]); }
  void place(String id, List xywh) { _cmds.add(['place', id, xywh]); }
  void remove(String id)          { _cmds.add(['remove', id]); }

  int _add(String kind, String id, Map props, List frame) {
    var ticket = _nextTicket++;
    _ticketOf[id] = ticket;
    _cmds.add(['add', kind, id, props]);
    if (frame != null) _cmds.add(['place', id, frame]);
    return ticket;
  }

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
  var h = _activeUi._handlers[ticket];
  if (h == null) return null;                      // fail closed (stale ticket)
  switch (kind) {
    case 0: h(); return null;                       // click
    case 1: h(); return null;                       // text changed (handler pulls textOf)
    case 5: h(); return null;                       // field enter
    case 6: h(arg != 0); return null;               // toggle(checked)
    case 4:                                          // select(row/index)
      if (h is _ListSource) { h.onSelect(arg); } else { h(arg); }
      return null;
    case 2: return (h is _ListSource) ? h.rowCount() : 0;   // row count (int)
    case 3: return (h is _ListSource) ? h.cellAt(arg) : ''; // cell text (String)
    case 7: /* resize: re-run build() with new bounds (APP_PANE_PLAN.md §7) */ return null;
    case 8: /* window close: stop the app */ return null;
    default: return null;
  }
}

void _ensureDispatch() {
  if (!_dispatchRegistered) { _registerWinDispatch(_winDispatch); _dispatchRegistered = true; }
}

/// Forget every callback (a surface teardown). Stale tickets then fail closed in
/// _winDispatch — but note the native side must ALSO DestroyWindow the owned
/// controls (win_callbacks.cpp / ViewServer::ClearSurface), unlike Cocoa's
/// weakly-held targets (cocoa.dart:156-158, S4_GUI_HOST_DESIGN.md §3.2).
void disposeCallbacks() { _activeUi?._handlers?.clear(); }

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
