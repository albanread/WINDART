// WINDART — the consolidated interactive workspace IDE (T1). ONE persistent
// dartui application whose tab strip switches content: Workspace (editor + live
// Do It), Browser (VM class table via mirrors, class->members->source), Editor
// (edit a user class + Accept -> morphing hot-reload via the SQLite image), VM
// (live Dart_WorkspaceVmStats counters). Find/Docs/App/Help are placeholders; the
// Debug tab is deferred (T4). The app STAYS OPEN (event-driven) unless run with
// the `selftest` arg, which drives each tab and snapshots it.
import 'dart:win';
import 'dart:io';
import 'dart:async';
import 'dart:isolate';   // T4: spawn the debug target isolate
import 'dart:mirrors';
// NB: do NOT statically import a from-source user library (e.g. counter_scratch.dart)
// here — reflecting over a source-loaded library via dart:mirrors trips a VM
// StackResource assert (allocation.cc:37). The Browser needs mirrors, so the live
// user class / instance-morph demo lives in the standalone workspace_morph.dart
// (S7.3, mirrors-free); this app's Editor tab does persist + Accept-reload.

// ── lexDart (verbatim from workspace.dart) — syntax runs for the source panes ─
final Set<String> _kw = new Set<String>.from(<String>[
  'abstract','as','assert','async','await','break','case','catch','class','const',
  'continue','default','deferred','do','dynamic','else','enum','export','extends',
  'external','factory','false','final','finally','for','get','if','implements',
  'import','in','is','library','new','null','operator','part','rethrow','return',
  'set','static','super','switch','sync','this','throw','true','try','typedef',
  'var','void','while','with','yield','bool','int','double','num']);
bool _dg(int c) => c >= 0x30 && c <= 0x39;
bool _hx(int c) => _dg(c) || (c >= 0x41 && c <= 0x46) || (c >= 0x61 && c <= 0x66);
bool _up(int c) => c >= 0x41 && c <= 0x5A;
bool _al(int c) => _up(c) || (c >= 0x61 && c <= 0x7A);
bool _is0(int c) => _al(c) || c == 0x5F || c == 0x24;
bool _is1(int c) => _is0(c) || _dg(c);
List<int> lexDart(String s) {
  var o = <int>[]; var n = s.length, i = 0;
  while (i < n) {
    var c = s.codeUnitAt(i);
    if (c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D) { i++; continue; }
    if (c == 0x2F && i + 1 < n) {
      var d = s.codeUnitAt(i + 1);
      if (d == 0x2F) { var st = i; while (i < n && s.codeUnitAt(i) != 0x0A) i++; o..add(st)..add(i-st)..add(3); continue; }
      if (d == 0x2A) { var st = i; i += 2; while (i+1 < n && !(s.codeUnitAt(i)==0x2A && s.codeUnitAt(i+1)==0x2F)) i++; i = (i+1<n)?i+2:n; o..add(st)..add(i-st)..add(3); continue; }
    }
    if (c == 0x27 || c == 0x22) {
      var st = i; i++;
      while (i < n && s.codeUnitAt(i) != c && s.codeUnitAt(i) != 0x0A) { if (s.codeUnitAt(i)==0x5C) i++; i++; }
      if (i < n && s.codeUnitAt(i)==c) i++; o..add(st)..add(i-st)..add(2); continue;
    }
    if (_dg(c)) {
      var st = i;
      if (c==0x30 && i+1<n && (s.codeUnitAt(i+1)==0x78||s.codeUnitAt(i+1)==0x58)) { i+=2; while (i<n && _hx(s.codeUnitAt(i))) i++; }
      else { while (i<n) { var d=s.codeUnitAt(i); if (_dg(d)||d==0x2E||d==0x65||d==0x45||d==0x5F) i++; else break; } }
      o..add(st)..add(i-st)..add(4); continue;
    }
    if (_is0(c)) { var st=i; i++; while (i<n && _is1(s.codeUnitAt(i))) i++; var w=s.substring(st,i); var k=_kw.contains(w)?1:(_up(c)?5:0); o..add(st)..add(i-st)..add(k); continue; }
    i++;
  }
  return o;
}

// ── state ────────────────────────────────────────────────────────────────────
Ui ui;
int activeTab = 0;
List<String> content = <String>[];        // ids of the current tab's content widgets
StringBuffer wsLog = new StringBuffer();
final tabNames = const ['Workspace','Browser','Editor','Find','Docs','App','Debug','VM','Help'];

Map<String, ClassMirror> classMirrors = <String, ClassMirror>{};
List<String> classNames = <String>[];
List<String> members = <String>[];
String currentClass = '';

String imgPath;

// ── helpers ──────────────────────────────────────────────────────────────────
String wr(String s) => s.replaceAll('\n', '\r');   // Dart \n -> RichEdit break

void loadMembers(String cls) {
  members = <String>[];
  var cm = classMirrors[cls];
  if (cm == null) return;
  cm.declarations.forEach((sym, d) {
    var nm = MirrorSystem.getName(sym);
    if (nm.isEmpty || nm.startsWith('_')) return;
    var p = 'var ';
    if (d is MethodMirror) {
      if (d.isConstructor) { p = 'new '; }
      else if (d.isGetter) { p = 'get '; }
      else if (d.isSetter) { p = 'set '; }
      else { p = 'fn  '; }
    }
    members.add(p + nm);
  });
  members.sort();
}

String classSketch(String name) {
  var cm = classMirrors[name];
  var sb = new StringBuffer();
  sb.writeln('// $name  (declaration sketch from the live VM class mirror)');
  var sup = '';
  if (cm != null && cm.superclass != null) {
    sup = ' extends ' + MirrorSystem.getName(cm.superclass.simpleName);
  }
  sb.writeln('class $name$sup {');
  for (var m in members) { sb.writeln('  $m;'); }
  sb.writeln('}');
  return sb.toString();
}

Db openImage() {
  var db = new Db.open(imgPath);
  db.exec('CREATE TABLE IF NOT EXISTS classes(name TEXT PRIMARY KEY, source TEXT)');
  return db;
}

// The user class source shown in the Editor tab: from the image if present, else
// a default. (Persisted by Accept; loaded over the snapshot at boot.)
String userClassSource() {
  var db = openImage();
  var rows = db.query('SELECT source FROM classes WHERE name = ?', ['Counter']);
  db.close();
  if (rows.isNotEmpty && rows[0][0].toString().isNotEmpty) return rows[0][0].toString();
  return 'class Counter {\n  int n = 0;\n  int step = 1;\n  Counter bump() { n = n + step; return this; }\n}';
}

// ── tab content builders ─────────────────────────────────────────────────────
void track(String id) => content.add(id);

void clearContent() {
  for (var id in content) { ui.remove(id); }
  content.clear();
}

void buildWorkspace() {
  ui.label('ws_lbl', text: 'Workspace   -   type Dart, click Do It (evaluates against the live VM)', frame: <int>[12, 36, 900, 18]); track('ws_lbl');
  ui.editor('ws_editor', text: '(2 + 3) * 7', frame: <int>[12, 58, 1060, 220]); track('ws_editor');
  ui.button('ws_doit', title: 'Do It', frame: <int>[12, 286, 100, 28], onClick: doIt); track('ws_doit');
  ui.label('ws_hint', text: '(result appended to Output below)', frame: <int>[124, 290, 500, 18]); track('ws_hint');
  ui.label('ws_outl', text: 'Output', frame: <int>[12, 322, 200, 18]); track('ws_outl');
  ui.editor('ws_output', frame: <int>[12, 344, 1060, 360]); track('ws_output');
  ui.set('ws_output', {'text': wr(wsLog.toString())});
}

void doIt() {
  var sel = ui.editorSelection('ws_editor');
  var code = sel[2].toString().replaceAll('\r', ' ').trim();
  if (code.isEmpty) return;
  var result = wsEval(code);
  wsLog.writeln('$code   =>   $result');
  print('DOIT: $code => $result');
  ui.set('ws_output', {'text': wr(wsLog.toString())});
  ui.commit();
}

void buildBrowser() {
  ui.label('br_cl', text: 'Classes (${classNames.length})', frame: <int>[12, 36, 260, 18]); track('br_cl');
  ui.list('br_classes', frame: <int>[12, 58, 260, 646],
      rowCount: () => classNames.length, cellAt: (r) => classNames[r], onSelect: selectClass); track('br_classes');
  var ml = currentClass.isEmpty ? 'Members' : 'Members of $currentClass (${members.length})';
  ui.label('br_ml', text: ml, frame: <int>[284, 36, 340, 18]); track('br_ml');
  ui.list('br_members', frame: <int>[284, 58, 340, 646],
      rowCount: () => members.length, cellAt: (r) => members[r], onSelect: (r) {}); track('br_members');
  ui.label('br_sl', text: 'Source', frame: <int>[636, 36, 400, 18]); track('br_sl');
  ui.editor('br_source', frame: <int>[636, 58, 436, 646]); track('br_source');
  if (currentClass.isNotEmpty) {
    var src = classSketch(currentClass);
    ui.set('br_source', {'text': wr(src)});
  }
}

void selectClass(int r) {
  if (r < 0 || r >= classNames.length) return;
  currentClass = classNames[r];
  loadMembers(currentClass);
  ui.set('br_ml', {'text': 'Members of $currentClass (${members.length})'});
  ui.set('br_members', {'rows': members.length});
  var src = classSketch(currentClass);
  ui.set('br_source', {'text': wr(src)});
  ui.commit();
  ui.applySpans('br_source', lexDart(src));
  print('BROWSE: class $currentClass -> ${members.length} members');
}

void buildEditor() {
  ui.label('ed_lbl', text: 'Editor   -   edit a user class + Accept (persists to the SQLite image, reloads the isolate)', frame: <int>[12, 36, 1000, 18]); track('ed_lbl');
  ui.editor('ed_source', frame: <int>[12, 58, 720, 380]); track('ed_source');
  ui.button('ed_accept', title: 'Accept', frame: <int>[12, 446, 120, 28], onClick: accept); track('ed_accept');
  ui.label('ed_status', text: 'edit the class above, then Accept', frame: <int>[148, 450, 900, 18]); track('ed_status');
  ui.label('ed_note', text: 'Accept writes the source to the image (survives restart) + hot-reloads. Live-instance morph: see workspace_morph.dart (S7.3).', frame: <int>[12, 486, 1040, 18]); track('ed_note');
  ui.label('ed_img', text: 'image: $imgPath', frame: <int>[12, 512, 1000, 18]); track('ed_img');
  var src = userClassSource();
  ui.set('ed_source', {'text': wr(src)});
  ui.commit();
  ui.applySpans('ed_source', lexDart(src));
}

void accept() {
  var sel = ui.editorSelection('ed_source');
  var src = sel[2].toString().replaceAll('\r\n', '\n').replaceAll('\r', '\n').trim();
  var db = openImage();
  db.exec('INSERT OR REPLACE INTO classes(name, source) VALUES(?, ?)', ['Counter', src]);
  db.close();
  wsRequestUiReload();
  ui.set('ed_status', {'text': 'Accept: persisted to image + requested reload...'});
  ui.commit();
  new Timer(new Duration(milliseconds: 300), () {
    ui.set('ed_status', {'text': 'Accepted -> image updated + reloaded ("${wsUiReloadStatus()}")'});
    ui.commit();
    print('ACCEPT: persisted Counter, reload = "${wsUiReloadStatus()}"');
  });
}

final vmLabels = const ['heap new used (B)','heap new capacity (B)','heap old used (B)',
  'heap old capacity (B)','scavenges (new GC)','mark-sweeps (old GC)',
  'functions compiled','functions optimized','generated code bytes'];

void buildVM() {
  ui.label('vm_lbl', text: 'VM   -   live counters (Dart_WorkspaceVmStats, refreshed every second)', frame: <int>[12, 36, 700, 18]); track('vm_lbl');
  for (var i = 0; i < vmLabels.length; i++) {
    ui.label('vm_k$i', text: vmLabels[i] + ':', frame: <int>[24, 70 + i * 30, 260, 18]); track('vm_k$i');
    ui.label('vm_v$i', text: '...', frame: <int>[300, 70 + i * 30, 320, 18]); track('vm_v$i');
  }
  refreshVM();
}

void refreshVM() {
  if (activeTab != 7) return;
  var stats = wsVmStats();
  for (var i = 0; i < vmLabels.length && i < stats.length; i++) {
    ui.set('vm_v$i', {'text': stats[i].toString()});
  }
  ui.commit();
}

// ── Find tab (T2): substring over classes + members -> jump to Browser ────────
List<String> findResults = <String>[];
void buildFind() {
  ui.label('fd_lbl', text: 'Find   -   substring over classes and members (from the VM class table)', frame: <int>[12, 36, 900, 18]); track('fd_lbl');
  ui.field('fd_q', text: '', frame: <int>[12, 58, 300, 24], onEnter: doFind); track('fd_q');
  ui.button('fd_go', title: 'Find', frame: <int>[320, 57, 80, 26], onClick: doFind); track('fd_go');
  ui.label('fd_rl', text: 'Matches (click a result to open it in the Browser)', frame: <int>[12, 92, 700, 18]); track('fd_rl');
  ui.list('fd_results', frame: <int>[12, 114, 760, 590],
      rowCount: () => findResults.length, cellAt: (r) => findResults[r], onSelect: openFindResult); track('fd_results');
}
void doFind() {
  var q = ui.textOf('fd_q').toLowerCase().trim();
  findResults = <String>[];
  if (q.isNotEmpty) {
    for (var c in classNames) { if (c.toLowerCase().contains(q)) findResults.add('class   ' + c); }
    for (var c in classNames) {
      var cm = classMirrors[c];
      cm.declarations.forEach((sym, d) {
        var nm = MirrorSystem.getName(sym);
        if (nm.isNotEmpty && !nm.startsWith('_') && nm.toLowerCase().contains(q)) {
          findResults.add(c + '.' + nm);
        }
      });
      if (findResults.length > 500) break;
    }
  }
  ui.set('fd_rl', {'text': 'Matches: ${findResults.length}  (click a result to open it in the Browser)'});
  ui.set('fd_results', {'rows': findResults.length});
  ui.commit();
  print('FIND: "$q" -> ${findResults.length} matches');
}
void openFindResult(int r) {
  if (r < 0 || r >= findResults.length) return;
  var m = findResults[r];
  var cls = m.startsWith('class   ') ? m.substring(8) : m.split('.')[0];
  var idx = classNames.indexOf(cls);
  if (idx < 0) return;
  switchTab(1);            // Browser
  selectClass(idx);
  print('FIND: open "$m" -> Browser class $cls');
}

// ── App tab (T2): a user app (Calculator) materialized in the app pane ────────
// Inlined (NOT imported — a source-loaded library + dart:mirrors crashes the VM,
// see T1). It uses only the Ui view-server API; its onClick handlers do the
// arithmetic and ui.set the display, which _winDispatch auto-commits.
class Calculator {
  var acc = 0.0;
  var pending;
  var display = '0';
  var fresh = true;
  build(u) {
    u.field('d', text: display, frame: <double>[8.0, 40.0, 272.0, 32.0], align: 'right', readOnly: true);
    var keys = ['7','8','9','/','4','5','6','*','1','2','3','-','0','.','=','+'];
    for (var i = 0; i < keys.length; i++) {
      var k = keys[i];
      u.button('k$k', title: k,
          frame: <double>[8.0 + (i % 4) * 68.0, 80.0 + (i ~/ 4) * 46.0, 64.0, 40.0],
          onClick: () => press(k, u));
    }
    u.button('kC', title: 'C', frame: <double>[8.0 + 4 * 68.0, 80.0, 64.0, 40.0], onClick: () => reset(u));
    u.label('hint', text: 'a live user app: buttons run Dart in the workspace (App-pane model)', frame: <int>[8, 274, 500, 16]);
  }
  press(String k, u) {
    if (k == '.' || (k.compareTo('0') >= 0 && k.compareTo('9') <= 0)) {
      if (fresh) { display = (k == '.') ? '0.' : k; fresh = false; }
      else if (k != '.' || !display.contains('.')) { display = display + k; }
    } else if (k == '=') { acc = _apply(_value()); display = _format(acc); pending = null; fresh = true; }
    else { acc = _apply(_value()); display = _format(acc); pending = k; fresh = true; }
    u.set('d', {'text': display});
    print('CALC: key $k -> display $display');
  }
  reset(u) { acc = 0.0; pending = null; display = '0'; fresh = true; u.set('d', {'text': display}); }
  _value() => double.parse(display, (_) => 0.0);
  _apply(double v) {
    if (pending == null) return v;
    if (pending == '+') return acc + v;
    if (pending == '-') return acc - v;
    if (pending == '*') return acc * v;
    if (pending == '/') return (v == 0.0) ? 0.0 : acc / v;
    return v;
  }
  _format(double v) {
    if (v == v.roundToDouble() && v.abs() < 1e15) return v.round().toString();
    return v.toString();
  }
}
Calculator calc;
void buildApp() {
  // The Calculator owns the pane from y=40 down; put the workspace's own caption
  // in the empty area to the RIGHT of the keypad so it clears both the tab strip
  // and the app's widgets.
  ui.label('app_lbl', text: 'App   -   a live user app (Calculator) running', frame: <int>[360, 44, 700, 18]); track('app_lbl');
  ui.label('app_lbl2', text: 'inside the workspace; the buttons are Dart closures.', frame: <int>[360, 66, 700, 18]); track('app_lbl2');
  var before = ui.widgetIds.toSet();
  if (calc == null) calc = new Calculator();
  calc.build(ui);
  for (var id in ui.widgetIds) { if (!before.contains(id)) track(id); }
}

// ── Docs tab (T2): a class/member reference from the VM class table ───────────
void buildDocs() {
  ui.label('dc_lbl', text: 'Docs   -   the VM class reference (select a class to see its members)', frame: <int>[12, 36, 900, 18]); track('dc_lbl');
  ui.list('dc_classes', frame: <int>[12, 58, 300, 646],
      rowCount: () => classNames.length, cellAt: (r) => classNames[r], onSelect: selectDocsClass); track('dc_classes');
  ui.label('dc_dl', text: 'Members', frame: <int>[324, 36, 400, 18]); track('dc_dl');
  ui.editor('dc_detail', frame: <int>[324, 58, 748, 646]); track('dc_detail');
}
void selectDocsClass(int r) {
  if (r < 0 || r >= classNames.length) return;
  var cls = classNames[r];
  loadMembers(cls);
  var src = classSketch(cls);
  ui.set('dc_dl', {'text': 'Members of $cls (${members.length})'});
  ui.set('dc_detail', {'text': wr(src)});
  ui.commit();
  ui.applySpans('dc_detail', lexDart(src));
}

// ── Help tab (T2): about / usage / keybindings ────────────────────────────────
void buildHelp() {
  ui.label('hp_lbl', text: 'Help   -   WINDART workspace', frame: <int>[12, 36, 900, 18]); track('hp_lbl');
  ui.editor('hp_text', frame: <int>[12, 58, 1060, 646]); track('hp_text');
  var help =
      'WINDART  -  a live Windows Dart workspace (Dart 1.24.3 JIT, native Win32 + Direct2D)\n'
      '\n'
      'TABS\n'
      '  Workspace  -  type a Dart expression, click Do It to EVALUATE it against the live VM\n'
      '                (Dart_EvaluateExpr). The result is appended to the Output pane.\n'
      '  Browser    -  browse the running VM\'s classes (via dart:mirrors): Classes -> Members\n'
      '                -> Source (a declaration sketch, syntax-highlighted).\n'
      '  Editor     -  edit a user class source; Accept persists it to the SQLite image\n'
      '                (%USERPROFILE%\\.windart\\workspace.sqlite, survives restart) + hot-reloads.\n'
      '  Find       -  substring search over class and member names; click a result to open it.\n'
      '  Docs       -  the class reference: pick a class, read its members.\n'
      '  App        -  a live user app (Calculator) materialized in the app pane; its buttons\n'
      '                are Dart closures that run in the workspace and update the display.\n'
      '  VM         -  live VM counters (Dart_WorkspaceVmStats): heap, GC, JIT compile stats.\n'
      '  Debug      -  the vm-service debugger (deferred to a later slice).\n'
      '\n'
      'GESTURES\n'
      '  Do It   -  evaluate the selection (or the whole editor) as a Dart expression, live.\n'
      '  Accept  -  commit a class definition: persist to the image + hot-reload; live instances\n'
      '             morph across a class-shape change (keep state, gain new fields).\n'
      '\n'
      'This whole IDE is ONE dartui.exe process: the VM you are inspecting is the VM running it.';
  ui.set('hp_text', {'text': wr(help)});
}

// ── Debug tab (T4): the in-process debugger ───────────────────────────────────
// Debugs a spawned target isolate (debug_target.dart) via the classic embedder
// debug API: set a line breakpoint, run, PAUSE at it, show the call stack + the
// current line marked in the source, evaluate an expression in the paused frame,
// step, and resume to completion. The step buttons pick the step mode (over/into/
// out) or plain resume; each Run drives one scripted breakpoint->pause->stack->
// eval->step->resume session (the buttons re-run with that mode).
const int kDbgBreakLine = 15;          // `var acc = 1;` in debug_target.dart
String dbgSource = '';                 // the target's source (read at build)
List<String> dbgStackLines = <String>[];
Timer dbgPollTimer;
bool dbgRunning = false;
int dbgPausedLine = -1;

String _dbgTargetPath() {
  try {
    var dir = new File.fromUri(Platform.script).parent.path;
    return dir + Platform.pathSeparator + 'debug_target.dart';
  } catch (e) { return 'debug_target.dart'; }
}

void _dbgLoadSource() {
  try { dbgSource = new File(_dbgTargetPath()).readAsStringSync(); }
  catch (e) { dbgSource = '// could not read debug_target.dart: $e'; }
}

// Render the source with a '*' on the breakpoint line and '>' on the paused line.
String _dbgRenderSource() {
  var lines = dbgSource.split('\n');
  var sb = new StringBuffer();
  for (var i = 0; i < lines.length; i++) {
    var n = i + 1;
    var mark = (n == dbgPausedLine) ? '>' : (n == kDbgBreakLine ? '*' : ' ');
    sb.write(mark);
    sb.write(n < 10 ? '  $n| ' : (n < 100 ? ' $n| ' : '$n| '));
    sb.write(lines[i]);
    sb.write('\n');
  }
  return sb.toString();
}

void buildDebug() {
  if (dbgSource.isEmpty) _dbgLoadSource();
  dbgPausedLine = -1;
  ui.label('db_lbl', text: 'Debug   -   in-process debugger (classic embedder API) on a spawned target isolate',
      frame: <int>[12, 36, 1040, 18]); track('db_lbl');
  ui.label('db_target', text: 'target: debug_target.dart      breakpoint: line $kDbgBreakLine (int factorial)      * = breakpoint   > = paused line',
      frame: <int>[12, 58, 1040, 18]); track('db_target');

  ui.editor('db_src', frame: <int>[12, 84, 620, 500]); track('db_src');
  ui.button('db_over', title: 'Run / Step Over', frame: <int>[12, 592, 130, 28], onClick: () => debugRun(0)); track('db_over');
  ui.button('db_into', title: 'Step Into', frame: <int>[150, 592, 96, 28], onClick: () => debugRun(1)); track('db_into');
  ui.button('db_out', title: 'Step Out', frame: <int>[254, 592, 96, 28], onClick: () => debugRun(2)); track('db_out');
  ui.button('db_resume', title: 'Resume', frame: <int>[358, 592, 96, 28], onClick: () => debugRun(3)); track('db_resume');

  ui.label('db_sl', text: 'Call stack (top frame first)', frame: <int>[644, 60, 428, 18]); track('db_sl');
  ui.list('db_stack', frame: <int>[644, 82, 428, 220],
      rowCount: () => dbgStackLines.length, cellAt: (r) => dbgStackLines[r], onSelect: (r) {}); track('db_stack');

  ui.label('db_el', text: 'Evaluate in frame:', frame: <int>[644, 312, 130, 18]); track('db_el');
  ui.field('db_eval', text: 'n * n', frame: <int>[776, 310, 200, 24]); track('db_eval');
  ui.label('db_evalout', text: '(result appears here)', frame: <int>[644, 340, 428, 18]); track('db_evalout');

  ui.label('db_ll', text: 'Session transcript', frame: <int>[644, 368, 300, 18]); track('db_ll');
  ui.editor('db_log', frame: <int>[644, 390, 428, 194]); track('db_log');
  ui.label('db_status', text: 'click Run / Step Over to start a debug session',
      frame: <int>[644, 592, 428, 18]); track('db_status');

  ui.set('db_src', {'text': wr(_dbgRenderSource())});
  ui.commit();
  ui.applySpans('db_src', lexDart(_dbgRenderSource()));
}

// Arm the debugger with the chosen step mode, spawn the target isolate, and poll
// the captured session, refreshing the stack / paused line / eval / transcript.
void debugRun(int stepKind) {
  if (activeTab != 6 || dbgRunning) return;
  dbgRunning = true;
  dbgStackLines = <String>[];
  dbgPausedLine = -1;
  var expr = ui.textOf('db_eval');
  if (expr == null || expr.trim().isEmpty) expr = 'n * n';
  var modeName = const ['Step Over', 'Step Into', 'Step Out', 'Resume'][stepKind];
  wsDebugArm(kDbgBreakLine, expr.trim(), stepKind);
  ui.set('db_status', {'text': 'running ($modeName) ...'});
  ui.set('db_evalout', {'text': '(result appears here)'});
  ui.set('db_stack', {'rows': 0});
  ui.commit();

  var rp = new ReceivePort();
  rp.listen((m) { wsDebugDone(); });

  Isolate
      .spawnUri(Uri.parse('debug_target.dart'), <String>[], rp.sendPort)
      .catchError((e) {
        ui.set('db_status', {'text': 'spawn error: $e'});
        ui.commit();
        dbgRunning = false;
      });

  var ticks = 0;
  dbgPollTimer = new Timer.periodic(new Duration(milliseconds: 100), (t) {
    ticks++;
    var st = wsDebugPoll();
    var done = st[0] == true;
    // st: [done, log, curLine, stack, evalResult, hit]
    dbgPausedLine = (st[2] is int) ? st[2] : -1;
    var stackStr = st[3].toString();
    dbgStackLines = stackStr.isEmpty ? <String>[] : stackStr.split('\n')
        .where((s) => s.trim().isNotEmpty).toList();
    ui.set('db_stack', {'rows': dbgStackLines.length});
    ui.set('db_log', {'text': wr(st[1].toString())});
    if (st[5] == true) ui.set('db_evalout', {'text': 'eval "$expr"  =>  ${st[4]}'});
    ui.set('db_src', {'text': wr(_dbgRenderSource())});
    ui.commit();
    ui.applySpans('db_src', lexDart(_dbgRenderSource()));
    if (done || ticks > 60) {
      t.cancel();
      dbgRunning = false;
      ui.set('db_status', {'text': done ? 'session complete ($modeName)' : 'timed out'});
      ui.commit();
      rp.close();
    }
  });
}

// ── Debug tab (deferred to T4): a placeholder panel ───────────────────────────
void buildPlaceholder(int i) {
  ui.label('ph_lbl', text: '${tabNames[i]}', frame: <int>[12, 40, 400, 22]); track('ph_lbl');
  ui.label('ph_note',
      text: 'The Debug tab (a vm-service client / breakpoints / stepping) is deferred to T4 — after the game pane (T3), per the user\'s priority order.',
      frame: <int>[12, 72, 1040, 18]); track('ph_note');
  ui.label('ph_note2',
      text: 'The vm-service protocol is already portable; this pane will host the debugger UI in a later slice.',
      frame: <int>[12, 98, 1040, 18]); track('ph_note2');
}

void buildTab(int i) {
  activeTab = i;
  clearContent();
  switch (i) {
    case 0: buildWorkspace(); break;
    case 1: buildBrowser(); break;
    case 2: buildEditor(); break;
    case 3: buildFind(); break;
    case 4: buildDocs(); break;
    case 5: buildApp(); break;
    case 6: buildDebug(); break;
    case 7: buildVM(); break;
    case 8: buildHelp(); break;
    default: buildPlaceholder(i); break;
  }
  ui.commit();
}

void switchTab(int i) {          // programmatic (self-test): set the strip + rebuild
  ui.set('tabs', {'tab': i});
  buildTab(i);
}

void snap(String name) {
  var e = ui.snapshot('e:/windart/build/$name.png');
  print('SNAP: $name ${e.isEmpty ? "OK" : "ERR:$e"}');
}

// ── main ─────────────────────────────────────────────────────────────────────
main(List<String> args) {
  var selftest = args.contains('selftest');

  // Browser data: the VM's class table.
  for (var lib in currentMirrorSystem().libraries.values) {
    lib.declarations.forEach((sym, decl) {
      if (decl is ClassMirror) {
        var name = MirrorSystem.getName(decl.simpleName);
        if (name.isNotEmpty && !name.startsWith('_')) classMirrors[name] = decl;
      }
    });
  }
  classNames = classMirrors.keys.toList()..sort();

  // The workspace image.
  var home = Platform.environment['USERPROFILE'];
  var dir = new Directory(home + '\\.windart');
  if (!dir.existsSync()) dir.createSync(recursive: true);
  imgPath = (dir.path + '\\workspace.sqlite').replaceAll('\\', '/');
  var db = openImage(); db.close();   // ensure the image + table exist

  ui = new Ui.pane(1084, 740);
  ui.title('WINDART Workspace   -   a live Windows Dart IDE');
  ui.tabs('tabs', items: tabNames, frame: <int>[0, 0, 1084, 26], onSelect: (i) => buildTab(i));
  buildTab(0);
  ui.commit();
  uiReady();

  new Timer.periodic(new Duration(seconds: 1), (_) => refreshVM());

  if (selftest) {
    // Pick a class with rich members for the Browser snapshot.
    var rich = classNames.isNotEmpty ? classNames[0] : '';
    var best = -1;
    classMirrors.forEach((name, cm) {
      var c = 0;
      cm.declarations.forEach((s, d) { var nm = MirrorSystem.getName(s); if (nm.isNotEmpty && !nm.startsWith('_')) c++; });
      if (c > best) { best = c; rich = name; }
    });
    var richIdx = classNames.indexOf(rich);
    var docIdx = richIdx;
    for (var name in const ['Duration','DateTime','Uri','StringBuffer','List','Object']) {
      var i = classNames.indexOf(name); if (i >= 0) { docIdx = i; break; }
    }

    var t = 400;
    new Timer(new Duration(milliseconds: t), () { switchTab(1); selectClass(richIdx); snap('tab_browser'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { switchTab(0); doIt(); snap('tab_workspace'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { switchTab(2); snap('tab_editor'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { switchTab(7); refreshVM(); snap('tab_vm'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { switchTab(3); ui.set('fd_q', {'text': 'Codec'}); ui.commit(); doFind(); snap('tab_find'); }); t += 450;
    // App: build the keypad in one tick; press + snapshot in the NEXT (a full
    // message-loop cycle between materialize and PrintWindow so every freshly
    // created button has painted at least once before the snapshot).
    new Timer(new Duration(milliseconds: t), () {
      switchTab(5);
      var keys = ui.widgetIds.where((s) => s.length >= 1 && s[0] == 'k').length;
      print('APP: keypad built, $keys key widgets');
    }); t += 450;
    new Timer(new Duration(milliseconds: t), () {
      calc.press('7', ui); calc.press('+', ui); calc.press('5', ui); calc.press('=', ui);   // -> 12
      ui.commit();
      snap('tab_app');
    }); t += 450;
    new Timer(new Duration(milliseconds: t), () { switchTab(4); selectDocsClass(docIdx); snap('tab_docs'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { switchTab(8); snap('tab_help'); }); t += 450;
    // Debug: open the tab, run one scripted debug session (breakpoint -> pause ->
    // stack -> frame-eval -> step -> resume -> complete), let it settle, snapshot.
    new Timer(new Duration(milliseconds: t), () { switchTab(6); debugRun(0); }); t += 1800;
    new Timer(new Duration(milliseconds: t), () { snap('tab_debug'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { print('SELFTEST: done'); hostQuit(); });
  }
  // else: stay open — a real interactive application.
}
