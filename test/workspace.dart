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
import 'userlib:user';    // Stage 1: the live user library, served SYNCHRONOUSLY from
// the SQLite image by a C++ tag handler (dart_win32/windart_userlib.cc) — no scratch
// file, the DB is the source of truth. Single isolate: mirrors + a source import
// coexist since the scanner tolerates a leading BOM. Accept rewrites the `userlib`
// rows + hot-reloads (ReloadSources re-invokes the handler), morphing live instances.

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
// The pane (container) client size, refreshed on every resize (kind 7). Tab
// layouts are computed from these so content reflows to fill the window.
int paneW = 1084;
int paneH = 712;
List<String> content = <String>[];        // ids of the current tab's content widgets
Set<String> persistentWidgets = <String>{};  // widgets that survive a tab switch (tab strip etc.)
StringBuffer wsLog = new StringBuffer();
final tabNames = const ['Workspace','Browser','Editor','Find','Docs','App','Debug','VM','Help','Game'];

Map<String, ClassMirror> classMirrors = <String, ClassMirror>{};
List<String> classNames = <String>[];
List<String> members = <String>[];
String currentClass = '';

// ── Categorized browser (item 4): library -> class -> (vars|methods) -> source ─
List<String> libraryNames = <String>[];                       // dart:core, dart:io, ...
Map<String, List<String>> classesInLib = <String, List<String>>{};  // library -> classes
Map<String, String> libOfClass = <String, String>{};         // class -> owning library
String currentLib = '';
List<String> libClasses = <String>[];                         // classes in currentLib
List<String> brVars = <String>[];                             // currentClass variables
List<String> brMethods = <String>[];                          // currentClass methods

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

// ── Mirror-driven declaration rendering (real signatures for VM classes) ──────
String _typeName(TypeMirror t) {
  if (t == null) return 'dynamic';
  try {
    var n = MirrorSystem.getName(t.simpleName);
    return n.isEmpty ? 'dynamic' : n;
  } catch (e) { return 'dynamic'; }
}

String _paramList(MethodMirror m) {
  try {
    // Group by kind so the WHOLE named set shares one {...} and the whole
    // optional-positional set shares one [...] — one brace per param produced
    // invalid Dart, e.g. Duration({int days}, {int hours}, ...).
    var req = <String>[], opt = <String>[], named = <String>[];
    for (var p in m.parameters) {
      var pn = MirrorSystem.getName(p.simpleName);
      var s = '${_typeName(p.type)} $pn';
      if (p.isNamed) named.add(s);
      else if (p.isOptional) opt.add(s);
      else req.add(s);
    }
    var parts = <String>[];
    parts.addAll(req);
    if (opt.isNotEmpty) parts.add('[${opt.join(', ')}]');
    if (named.isNotEmpty) parts.add('{${named.join(', ')}}');
    return parts.join(', ');
  } catch (e) { return ''; }
}

String _methodDecl(String cls, MethodMirror m) {
  var name = MirrorSystem.getName(m.simpleName);
  var stat = m.isStatic ? 'static ' : '';
  if (m.isConstructor) {
    var cn = '';
    try { cn = MirrorSystem.getName(m.constructorName); } catch (e) {}
    var full = cn.isEmpty ? cls : '$cls.$cn';
    return '$full(${_paramList(m)});';
  }
  var ret = _typeName(m.returnType);
  if (m.isGetter) return '$stat$ret get $name;';
  if (m.isSetter) {
    var base = name.endsWith('=') ? name.substring(0, name.length - 1) : name;
    return '${stat}set $base(${_paramList(m)});';
  }
  if (m.isOperator) return '$stat$ret operator $name(${_paramList(m)});';
  return '$stat$ret $name(${_paramList(m)});';
}

// The full class declaration from the live VM class mirror: real field types,
// constructor/accessor/method signatures (no bodies — the VM has no source for
// snapshot classes), grouped and sorted. Used by the Browser/Docs/Editor source
// panes for VM classes.
String classSketch(String name) {
  var cm = classMirrors[name];
  var sb = new StringBuffer();
  sb.writeln('// $name  (declaration from the live VM class mirror — signatures, no bodies)');
  if (cm == null) { sb.writeln('class $name {\n}'); return sb.toString(); }
  var sup = '';
  try {
    if (cm.superclass != null) sup = ' extends ' + MirrorSystem.getName(cm.superclass.simpleName);
  } catch (e) {}
  sb.writeln('class $name$sup {');
  var vars = <String>[], ctors = <String>[], acc = <String>[], meths = <String>[];
  cm.declarations.forEach((sym, d) {
    try {
      var nm = MirrorSystem.getName(sym);
      if (nm.isEmpty || nm.startsWith('_')) return;
      if (d is VariableMirror) {
        var stat = d.isStatic ? 'static ' : '';
        var fin = d.isFinal ? 'final ' : '';
        vars.add('  $stat$fin${_typeName(d.type)} $nm;');
      } else if (d is MethodMirror) {
        var line = '  ' + _methodDecl(name, d);
        if (d.isConstructor) ctors.add(line);
        else if (d.isGetter || d.isSetter) acc.add(line);
        else meths.add(line);
      }
    } catch (e) { /* one declaration's mirror threw — skip it, keep the rest */ }
  });
  vars.sort(); ctors.sort(); acc.sort(); meths.sort();
  if (vars.isNotEmpty)  { sb.writeln('  // fields');       for (var l in vars)  sb.writeln(l); }
  if (ctors.isNotEmpty) { sb.writeln('  // constructors'); for (var l in ctors) sb.writeln(l); }
  if (acc.isNotEmpty)   { sb.writeln('  // accessors');    for (var l in acc)   sb.writeln(l); }
  if (meths.isNotEmpty) { sb.writeln('  // methods');      for (var l in meths) sb.writeln(l); }
  sb.writeln('}');
  return sb.toString();
}

// ── On-disk SDK source (real bodies, not just mirror signatures) ──────────────
// The mirror browser can only show signatures — the running VM keeps no source
// for its snapshot classes. But the 1.24.3 SDK sources ARE on disk (the build's
// extracted tree, or the pristine quarry), so we read the real class/method text
// straight from the .dart files. First existing root wins, per file.
final List<String> _sdkRoots = <String>[
  'e:/windart/tree/sdk/lib',
  'e:/dart_origins/sdk-1.24.3/sdk/lib',
];

String _libDir(String lib) {
  if (lib == null || !lib.startsWith('dart:')) return '';
  var d = lib.substring(5);
  if (d.startsWith('_')) d = d.substring(1);       // dart:_internal -> internal
  return d;
}

// Skip a Dart string literal (single/triple, escapes) at i; return the index past it.
int _skipStr(String s, int i) {
  var n = s.length, q = s.codeUnitAt(i);
  var triple = i + 2 < n && s.codeUnitAt(i + 1) == q && s.codeUnitAt(i + 2) == q;
  if (triple) {
    i += 3;
    while (i + 2 < n && !(s.codeUnitAt(i) == q && s.codeUnitAt(i + 1) == q && s.codeUnitAt(i + 2) == q)) {
      if (s.codeUnitAt(i) == 0x5C) i++;
      i++;
    }
    return (i + 2 < n) ? i + 3 : n;
  }
  i++;
  while (i < n && s.codeUnitAt(i) != q && s.codeUnitAt(i) != 0x0A) {
    if (s.codeUnitAt(i) == 0x5C) i++;
    i++;
  }
  return (i < n && s.codeUnitAt(i) == q) ? i + 1 : i;
}

// Index of the '}' matching the '{' at `open`, skipping comments and strings.
int _matchBrace(String s, int open) {
  var n = s.length, depth = 0, i = open;
  while (i < n) {
    var c = s.codeUnitAt(i);
    if (c == 0x2F && i + 1 < n) {                    // '/'
      var d = s.codeUnitAt(i + 1);
      if (d == 0x2F) { i += 2; while (i < n && s.codeUnitAt(i) != 0x0A) i++; continue; }
      if (d == 0x2A) { i += 2; while (i + 1 < n && !(s.codeUnitAt(i) == 0x2A && s.codeUnitAt(i + 1) == 0x2F)) i++; i += 2; continue; }
    }
    if (c == 0x27 || c == 0x22) { i = _skipStr(s, i); continue; }
    if (c == 0x7B) depth++;                          // {
    else if (c == 0x7D) { depth--; if (depth == 0) return i; }   // }
    i++;
  }
  return -1;
}

// The real on-disk source of `cls` from its dart: library file, or '' if not
// found (a native class with no .dart, or the source tree is absent).
String sdkClassSource(String cls) {
  var lib = libOfClass[cls];
  var sub = _libDir(lib);
  if (sub.isEmpty) return '';
  var decl = new RegExp('^[ \\t]*(abstract[ \\t]+)?class[ \\t]+' + cls + '[ \\t\\r\\n<{]', multiLine: true);
  for (var root in _sdkRoots) {
    var dir = new Directory('$root/$sub');
    if (!dir.existsSync()) continue;
    for (var ent in dir.listSync()) {
      if (ent is! File) continue;
      var f = ent as File;
      var path = f.path.replaceAll('\\', '/');
      if (!path.endsWith('.dart')) continue;
      String text;
      try { text = f.readAsStringSync(); } catch (e) { continue; }
      var m = decl.firstMatch(text);
      if (m == null) continue;
      var start = m.start;
      while (start < text.length) {
        var cc = text.codeUnitAt(start);
        if (cc == 0x20 || cc == 0x09 || cc == 0x0A || cc == 0x0D) start++; else break;
      }
      var open = text.indexOf('{', start);
      if (open < 0) continue;
      var close = _matchBrace(text, open);
      if (close < 0) continue;
      return '// $lib  ::  $cls        (real source: $path)\n' + text.substring(start, close + 1) + '\n';
    }
  }
  return '';
}

// The search anchor for a member declaration string (get/set/operator/name).
String _memberAnchor(String decl) {
  var d = decl.replaceAll(';', '').trim();
  var paren = d.indexOf('(');
  var head = (paren < 0 ? d : d.substring(0, paren)).trim();
  var gi = head.indexOf(' get ');
  if (gi >= 0) return 'get ' + head.substring(gi + 5).trim();
  var si = head.indexOf(' set ');
  if (si >= 0) return 'set ' + head.substring(si + 5).trim();
  var op = head.indexOf('operator ');
  if (op >= 0) return head.substring(op).trim();
  var parts = head.split(new RegExp('\\s+'));
  return parts.isEmpty ? '' : parts[parts.length - 1];
}

// Best-effort: just one member's real source, sliced from its class file.
String sdkMemberSource(String cls, String decl) {
  var full = sdkClassSource(cls);
  if (full.isEmpty) return '';
  var anchor = _memberAnchor(decl);
  if (anchor.isEmpty) return '';
  var idx = full.indexOf(anchor);
  if (idx < 0) return '';
  var ls = full.lastIndexOf('\n', idx);
  var start = ls < 0 ? 0 : ls + 1;
  var brace = full.indexOf('{', idx);
  var semi = full.indexOf(';', idx);
  int end;
  if (brace >= 0 && (semi < 0 || brace < semi)) {
    var close = _matchBrace(full, brace);
    end = (close < 0) ? (semi < 0 ? full.length - 1 : semi) : close;
  } else {
    end = (semi < 0) ? full.length - 1 : semi;
  }
  var head = '// $cls  ::  ' + decl.replaceAll(';', '').trim() + '   (real source)\n';
  return head + full.substring(start, end + 1) + '\n';
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
  // Remove EVERY non-persistent widget, not just the ones the per-tab `content` list
  // captured. A user app's build() (e.g. the Calculator) may create widgets that were
  // never tracked; if they are not destroyed here they remain as live Win32 child
  // windows behind the next tab and repaint on hover. Iterating ui.widgetIds (kept
  // accurate by ui.remove dropping the ticket-map entry) removes all of them.
  for (var id in ui.widgetIds.toList()) {
    if (!persistentWidgets.contains(id)) ui.remove(id);
  }
  content.clear();
}

void buildWorkspace() {
  var W = paneW, H = paneH;
  var edH = ((H - 58) * 0.40).round();     // editor ~40% of the pane height
  var doitY = 58 + edH + 8;
  var outlY = doitY + 34;
  var outY = outlY + 22;
  var outH = H - outY - 10;
  ui.label('ws_lbl', text: 'Workspace   -   type Dart, click Do It (evaluates against the live VM)', frame: <int>[12, 36, W - 24, 18]); track('ws_lbl');
  ui.editor('ws_editor', text: '(2 + 3) * 7', frame: <int>[12, 58, W - 24, edH]); track('ws_editor');
  ui.button('ws_doit', title: 'Do It', frame: <int>[12, doitY, 100, 28], onClick: doIt); track('ws_doit');
  ui.label('ws_hint', text: '(result appended to Output below)', frame: <int>[124, doitY + 4, 500, 18]); track('ws_hint');
  ui.label('ws_outl', text: 'Output', frame: <int>[12, outlY, 200, 18]); track('ws_outl');
  ui.editor('ws_output', frame: <int>[12, outY, W - 24, outH]); track('ws_output');
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

// Smalltalk-style member side: 0 = instance (non-static), 1 = class (static
// members + constructors/factories). The instance/class toggle filters the
// Variables + Methods panes.
int browserSide = 0;
String sideName() => browserSide == 0 ? 'instance' : 'class';

// The classic drill-down: Libraries | Classes | (Variables / Methods) | Source.
// Categories are dart:mirrors libraries; each column narrows the selection.
void buildBrowser() {
  var W = paneW, H = paneH;
  var listH = H - 68;
  var libX = 12,           libW = 176;
  var clsX = libX + libW + 8, clsW = 196;
  var memX = clsX + clsW + 8, memW = 236;
  var srcX = memX + memW + 8, srcW = W - srcX - 12;
  var memTop = 82;                                // member lists start below the toggle row
  var memListH = listH - 24;
  var varH = ((memListH - 22) * 0.42).round();    // variables pane ~top 42%
  var methLblY = memTop + varH + 4;
  var methY = memTop + varH + 22;
  var methH = memListH - varH - 22;               // methods pane fills the rest

  if (currentLib.isEmpty && libraryNames.isNotEmpty) {   // default category
    currentLib = libraryNames.contains('dart:core') ? 'dart:core' : libraryNames[0];
    libClasses = classesInLib[currentLib] ?? <String>[];
  }

  ui.label('br_libl', text: 'Libraries (${libraryNames.length})', frame: <int>[libX, 36, libW, 18]); track('br_libl');
  ui.list('br_libs', frame: <int>[libX, 58, libW, listH],
      rowCount: () => libraryNames.length, cellAt: (r) => libraryNames[r], onSelect: selectLibrary); track('br_libs');

  ui.label('br_ll', text: 'Classes (${libClasses.length})', frame: <int>[clsX, 36, clsW, 18]); track('br_ll');
  ui.list('br_classes', frame: <int>[clsX, 58, clsW, listH],
      rowCount: () => libClasses.length, cellAt: (r) => libClasses[r], onSelect: selectLibClass); track('br_classes');
  // A user-draggable divider between the Libraries and Classes panes (item 5).
  ui.splitter('br_split', orientation: 'vertical', frame: <int>[libX + libW + 2, 58, 6, listH],
      between: <String>['br_libs', 'br_classes']); track('br_split');

  var sn = sideName();
  ui.label('br_vl', text: currentClass.isEmpty ? 'Variables ($sn)' : 'Variables - $sn (${brVars.length})', frame: <int>[memX, 36, memW, 18]); track('br_vl');
  // Smalltalk-style instance/class side toggle over the Variables + Methods panes.
  var halfW = ((memW - 4) / 2).round();
  ui.button('br_inst', title: 'instance', frame: <int>[memX, 58, halfW, 22], onClick: () => setBrowserSide(0)); track('br_inst');
  ui.button('br_cls', title: 'class', frame: <int>[memX + halfW + 4, 58, memW - halfW - 4, 22], onClick: () => setBrowserSide(1)); track('br_cls');
  ui.list('br_vars', frame: <int>[memX, memTop, memW, varH],
      rowCount: () => brVars.length, cellAt: (r) => brVars[r], onSelect: selectBrVar); track('br_vars');
  ui.label('br_ml', text: currentClass.isEmpty ? 'Methods ($sn)' : 'Methods - $sn (${brMethods.length})', frame: <int>[memX, methLblY, memW, 18]); track('br_ml');
  ui.list('br_meths', frame: <int>[memX, methY, memW, methH],
      rowCount: () => brMethods.length, cellAt: (r) => brMethods[r], onSelect: selectBrMethod); track('br_meths');

  ui.label('br_sl', text: 'Source', frame: <int>[srcX, 36, srcW, 18]); track('br_sl');
  ui.editor('br_source', frame: <int>[srcX, 58, srcW, listH]); track('br_source');

  if (currentClass.isNotEmpty) {                 // restore member panes after a rebuild
    loadVarsMethods(currentClass);
    ui.set('br_vl', {'text': 'Variables - ${sideName()} (${brVars.length})'});
    ui.set('br_ml', {'text': 'Methods - ${sideName()} (${brMethods.length})'});
    ui.set('br_vars', {'rows': brVars.length});
    ui.set('br_meths', {'rows': brMethods.length});
    var real = sdkClassSource(currentClass);
    var src = real.isNotEmpty ? real : classSketch(currentClass);
    ui.set('br_source', {'text': wr(src)});
  }
}

// Split a class's declarations into variable + method rows (short signatures).
void loadVarsMethods(String cls) {
  brVars = <String>[]; brMethods = <String>[];
  var cm = classMirrors[cls];
  if (cm == null) return;
  var wantStatic = browserSide == 1;             // class side = static members + ctors/factories
  cm.declarations.forEach((sym, d) {
    try {
      var nm = MirrorSystem.getName(sym);
      if (nm.isEmpty || nm.startsWith('_')) return;
      if (d is VariableMirror) {
        if (d.isStatic != wantStatic) return;
        brVars.add((d.isStatic ? 'static ' : '') + (d.isFinal ? 'final ' : '') + _typeName(d.type) + ' ' + nm);
      } else if (d is MethodMirror) {
        var classSide = d.isStatic || d.isConstructor;
        if (classSide != wantStatic) return;
        brMethods.add(_methodDecl(cls, d));
      }
    } catch (e) {}
  });
  brVars.sort(); brMethods.sort();
}

// Instance/Class toggle: re-filter the current class's member panes by side.
void setBrowserSide(int side) {
  if (browserSide == side) return;
  browserSide = side;
  if (currentClass.isNotEmpty) loadVarsMethods(currentClass);
  ui.set('br_vl', {'text': currentClass.isEmpty ? 'Variables (${sideName()})' : 'Variables - ${sideName()} (${brVars.length})'});
  ui.set('br_ml', {'text': currentClass.isEmpty ? 'Methods (${sideName()})' : 'Methods - ${sideName()} (${brMethods.length})'});
  ui.set('br_vars', {'rows': brVars.length});
  ui.set('br_meths', {'rows': brMethods.length});
  ui.commit();
  print('BROWSE side=${sideName()} -> ${brVars.length} vars, ${brMethods.length} methods');
}

// Category pane: pick a library -> repopulate the Classes pane, clear the rest.
void selectLibrary(int r) {
  if (r < 0 || r >= libraryNames.length) return;
  currentLib = libraryNames[r];
  libClasses = classesInLib[currentLib] ?? <String>[];
  currentClass = ''; brVars = <String>[]; brMethods = <String>[];
  ui.set('br_ll', {'text': 'Classes (${libClasses.length})'});
  ui.set('br_classes', {'rows': libClasses.length});
  ui.set('br_vl', {'text': 'Variables'});
  ui.set('br_ml', {'text': 'Methods'});
  ui.set('br_vars', {'rows': 0});
  ui.set('br_meths', {'rows': 0});
  ui.set('br_source', {'text': ''});
  ui.commit();
  print('LIB: $currentLib -> ${libClasses.length} classes');
}

// Classes pane: pick a class within the current library.
void selectLibClass(int r) {
  if (r < 0 || r >= libClasses.length) return;
  var cls = libClasses[r];
  browseToClass(cls);
  var flat = classNames.indexOf(cls);
  if (flat >= 0 && !_browseNav) _browseRecord(flat);
  print('BROWSE: class $cls -> ${brVars.length} vars, ${brMethods.length} methods');
}

// Navigate to a class by NAME: sync the category + class panes, load its vars +
// methods, show its declaration. The shared entry point for the Classes pane,
// Find jumps, and toolbar Back/Forward/Home.
void browseToClass(String cls) {
  var lib = libOfClass[cls];
  if (lib != null && (lib != currentLib || libClasses.isEmpty)) {
    currentLib = lib;
    libClasses = classesInLib[lib] ?? <String>[];
    ui.set('br_ll', {'text': 'Classes (${libClasses.length})'});
    ui.set('br_classes', {'rows': libClasses.length});
  }
  currentClass = cls;
  loadVarsMethods(cls);
  ui.set('br_vl', {'text': 'Variables - ${sideName()} (${brVars.length})'});
  ui.set('br_ml', {'text': 'Methods - ${sideName()} (${brMethods.length})'});
  ui.set('br_vars', {'rows': brVars.length});
  ui.set('br_meths', {'rows': brMethods.length});
  var real = sdkClassSource(cls);                  // real on-disk bodies if we have them
  var src = real.isNotEmpty ? real : classSketch(cls);
  ui.set('br_source', {'text': wr(src)});
  ui.commit();
  ui.applySpans('br_source', lexDart(src));
}

// Member panes -> the Source pane shows the selected member's declaration.
void selectBrVar(int r) {
  if (r < 0 || r >= brVars.length) return;
  var src = '// $currentLib  ::  $currentClass\n${brVars[r]};\n';
  ui.set('br_source', {'text': wr(src)}); ui.commit();
  ui.applySpans('br_source', lexDart(src));
  print('BROWSE: var ${brVars[r]}');
}
void selectBrMethod(int r) {
  if (r < 0 || r >= brMethods.length) return;
  var real = sdkMemberSource(currentClass, brMethods[r]);   // real body if on disk
  var src = real.isNotEmpty ? real : '// $currentLib  ::  $currentClass\n${brMethods[r]}\n';
  ui.set('br_source', {'text': wr(src)}); ui.commit();
  ui.applySpans('br_source', lexDart(src));
  print('BROWSE: method ${brMethods[r]}');
}

// Flat-index entry (toolbar Back/Forward/Home history, Find jumps).
void selectClass(int r) {
  if (r < 0 || r >= classNames.length) return;
  browseToClass(classNames[r]);
  if (!_browseNav) _browseRecord(r);
  print('BROWSE: class ${classNames[r]}');
}

// ── Browser navigation (toolbar Home / Back / Forward) ────────────────────────
// A back/forward stack of class indices, cursor at the current position. A fresh
// selectClass (user click, Find jump) pushes; Back/Forward move the cursor and
// re-select WITHOUT re-recording (guarded by _browseNav).
List<int> browseHistory = <int>[];
int browseCursor = -1;
bool _browseNav = false;

void _browseRecord(int idx) {
  if (browseCursor >= 0 && browseCursor < browseHistory.length &&
      browseHistory[browseCursor] == idx) return;          // ignore a repeat
  if (browseCursor < browseHistory.length - 1) {
    browseHistory = browseHistory.sublist(0, browseCursor + 1);   // drop the forward tail
  }
  browseHistory.add(idx);
  browseCursor = browseHistory.length - 1;
}

void _browseGo(int idx) {                    // navigate without recording
  if (activeTab != 1) switchTab(1);
  _browseNav = true;
  selectClass(idx);
  _browseNav = false;
}

void browseBack() {
  if (activeTab != 1) switchTab(1);
  if (browseCursor > 0) { browseCursor--; _browseGo(browseHistory[browseCursor]); }
  print('NAV: back -> cursor $browseCursor/${browseHistory.length - 1}');
}

void browseForward() {
  if (activeTab != 1) switchTab(1);
  if (browseCursor < browseHistory.length - 1) {
    browseCursor++; _browseGo(browseHistory[browseCursor]);
  }
  print('NAV: forward -> cursor $browseCursor/${browseHistory.length - 1}');
}

void browseHome() {                          // Browser tab, reset to the top class
  if (activeTab != 1) switchTab(1);
  if (classNames.isNotEmpty) selectClass(0);   // records (default _browseNav == false)
  print('NAV: home -> ${classNames.isEmpty ? "" : classNames[0]}');
}

// The Editor is now pick-a-class-then-edit: a class-selector dropdown lists the
// user classes stored in the image plus the live VM classes (via mirrors).
String currentEditClass = 'Counter';
List<String> editorClassList = <String>[];

void buildEditorClassList() {
  var seen = new Set<String>();
  editorClassList = <String>[];
  var db = openImage();
  var rows = db.query('SELECT name FROM classes ORDER BY name', const []);
  db.close();
  for (var r in rows) { var n = r[0].toString(); if (seen.add(n)) editorClassList.add(n); }
  if (seen.add('Counter')) editorClassList.insert(0, 'Counter');     // always offer Counter
  for (var n in classNames) { if (seen.add(n)) editorClassList.add(n); }   // then VM classes
}

String editorSourceFor(String cls) {
  var db = openImage();
  var isUser = userClassNames.contains(cls);
  if (isUser) db.exec('CREATE TABLE IF NOT EXISTS userlib(name TEXT PRIMARY KEY, source TEXT)', const []);
  var rows = db.query('SELECT source FROM ' + (isUser ? 'userlib' : 'classes') + ' WHERE name = ?', [cls]);
  db.close();
  if (rows.isNotEmpty && rows[0][0].toString().isNotEmpty) return rows[0][0].toString();
  if (cls == 'Counter') {
    return 'class Counter {\n  int n = 0;\n  Counter bump() { n = n + 1; return this; }\n}';
  }
  if (classMirrors.containsKey(cls)) {
    loadMembers(cls);
    var real = sdkClassSource(cls);
    return real.isNotEmpty ? real : classSketch(cls);
  }
  return 'class $cls {\n}';
}

void loadEditorClass(String cls) {
  currentEditClass = cls;
  var src = editorSourceFor(cls);
  ui.set('ed_source', {'text': wr(src)});
  ui.set('ed_status', {'text': 'editing $cls'});
  ui.commit();
  ui.applySpans('ed_source', lexDart(src));
  print('EDITOR: selected class $cls');
}

void buildEditor() {
  if (editorClassList.isEmpty) buildEditorClassList();
  var W = paneW, H = paneH;
  var srcH = H - 92 - 100;                 // editor fills, leaving room for the footer
  var acceptY = 92 + srcH + 10;
  ui.label('ed_lbl', text: 'Editor   -   pick a class, read/edit its source; Accept saves it to the SQLite image', frame: <int>[12, 36, W - 24, 18]); track('ed_lbl');
  ui.label('ed_cl_lbl', text: 'Class:', frame: <int>[12, 62, 48, 18]); track('ed_cl_lbl');
  ui.popup('ed_class', items: editorClassList, frame: <int>[64, 58, 320, 260],
      onSelect: (i) { if (i >= 0 && i < editorClassList.length) loadEditorClass(editorClassList[i]); }); track('ed_class');
  ui.editor('ed_source', frame: <int>[12, 92, W - 24, srcH]); track('ed_source');
  ui.button('ed_accept', title: 'Accept', frame: <int>[12, acceptY, 100, 28], onClick: accept); track('ed_accept');
  ui.button('ed_revert', title: 'Revert', frame: <int>[118, acceptY, 90, 28], onClick: revert); track('ed_revert');   // W3: undo to prior version
  ui.label('ed_status', text: 'editing $currentEditClass', frame: <int>[216, acceptY + 4, W - 228, 18]); track('ed_status');
  ui.label('ed_note', text: 'Accept writes the user class to the SQLite image (userlib) + hot-reloads: live instances MORPH in place (state kept, new fields added). A C++ tag handler serves the source from the image - no file. Survives restart.', frame: <int>[12, acceptY + 36, W - 24, 18]); track('ed_note');
  ui.label('ed_img', text: 'image: $imgPath', frame: <int>[12, acceptY + 62, W - 24, 18]); track('ed_img');
  var src = editorSourceFor(currentEditClass);
  ui.set('ed_source', {'text': wr(src)});
  var idx = editorClassList.indexOf(currentEditClass);
  if (idx >= 0) ui.set('ed_class', {'index': idx});
  ui.commit();
  ui.applySpans('ed_source', lexDart(src));
}

// ── Stage 1: the live user library (DB-served, single isolate) ────────────────
// The user classes live in the image's `userlib` table. `import 'userlib:user'`
// (top) is served SYNCHRONOUSLY from that table by a C++ tag handler
// (dart_win32/windart_userlib.cc). Accept writes the edited source to `userlib`
// and hot-reloads; ReloadSources re-invokes the handler, re-reads the image, and
// morphs live instances in place. No scratch file — the DB is the source of truth.
final List<String> userClassNames = <String>['Counter'];   // the live user classes
dynamic liveCounter;                                        // a live instance to morph

// User-class source lives in the image's `userlib` table; the C++ userlib: tag
// handler (windart_userlib.cc) serves it to `import 'userlib:user'` — no file.

// Read the live instance's state via eval in the root library (sees userlib's Counter).
String liveState() {
  var n = wsEval('liveCounter == null ? "-" : liveCounter.n');
  // inc() is a METHOD, so it re-links on every morph and tracks the source version
  // (a field would keep its value across re-morphs — state preservation).
  var inc = wsEval('liveCounter == null ? "" : liveCounter.inc()');
  var s = 'live Counter.n=' + n;
  if (!inc.startsWith('ERR') && inc.isNotEmpty) s = s + '  inc()=' + inc;
  return s;
}

void accept() {
  var sel = ui.editorSelection('ed_source');
  var src = sel[2].toString().replaceAll('\r\n', '\n').replaceAll('\r', '\n').trim();
  var isUser = userClassNames.contains(currentEditClass);
  // Validate-before-save (syntax-check-first): compile the edit in isolation; only
  // clean source is written to the image + reloaded, so the reload never sees
  // uncompilable source (a bad reload's finalize would kill the isolate). Logical/
  // runtime errors are a separate matter — found by running, not by this scan.
  if (isUser) {
    var err = wsCheckSyntax(src, currentEditClass);
    if (err.isNotEmpty) {
      // Dart reports "'usercheck:N': error: line L pos P: msg\n  <src line>\n  ^".
      // Strip the internal URI to the bare location + message, and highlight the
      // offending line in the editor (SetFocus + select via editorSelectLine).
      var loc = new RegExp(r'line (\d+) pos (\d+): (.*)').firstMatch(err);
      var head = (loc != null)
          ? 'line ${loc.group(1)} pos ${loc.group(2)}: ${loc.group(3)}'
          : err.replaceAll('\r', ' ').replaceAll('\n', ' ');
      ui.set('ed_status', {'text': 'REJECTED - $currentEditClass: $head'});
      ui.commit();
      if (loc != null) ui.editorSelectLine('ed_source', int.parse(loc.group(1)) - 1);
      print('ACCEPT REJECTED (syntax): $currentEditClass: $head');
      return;   // do NOT write the image or reload
    }
  }
  var db = openImage();
  if (isUser) {
    db.exec('CREATE TABLE IF NOT EXISTS userlib(name TEXT PRIMARY KEY, source TEXT)', const []);
    // W3: version the outgoing source (multi-level undo). Before overwriting, save
    // what the image currently holds for this class, so Revert can step back through
    // the history of accepted (and therefore gate-passed, compilable) versions.
    db.exec('CREATE TABLE IF NOT EXISTS versions(id INTEGER PRIMARY KEY AUTOINCREMENT, '
        'ts TEXT, kind TEXT, name TEXT, source TEXT, label TEXT)', const []);
    var cur = db.query('SELECT source FROM userlib WHERE name = ?', [currentEditClass]);
    if (cur != null && cur.isNotEmpty && cur[0][0].toString().isNotEmpty) {
      db.exec('INSERT INTO versions(ts, kind, name, source, label) VALUES(?, ?, ?, ?, ?)',
          [new DateTime.now().toIso8601String(), 'userlib', currentEditClass, cur[0][0], 'pre-accept']);
    }
    db.exec('INSERT OR REPLACE INTO userlib(name, source) VALUES(?, ?)', [currentEditClass, src]);
  } else {
    db.exec('INSERT OR REPLACE INTO classes(name, source) VALUES(?, ?)', [currentEditClass, src]);
  }
  db.close();
  wsRequestUiReload();   // ReloadSources re-invokes the userlib: handler -> re-reads the image -> morph
  ui.set('ed_status', {'text': 'Accept: wrote image' + (isUser ? ' (userlib) -> reloading (morph)...' : ' -> reloading...')});
  ui.commit();
  new Timer(new Duration(milliseconds: 350), () {
    var st = wsUiReloadStatus();
    var live = isUser ? ('   ' + liveState()) : '';
    ui.set('ed_status', {'text': 'Accepted $currentEditClass  (reload: "$st")$live'});
    ui.commit();
    print('ACCEPT: $currentEditClass reload="$st"$live');
  });
}

// W3: how many older versions of a user class are on the undo stack.
int versionCount(String cls) {
  var db = openImage();
  var r = db.query('SELECT COUNT(*) FROM versions WHERE kind = ? AND name = ?', ['userlib', cls]);
  db.close();
  if (r == null || r.isEmpty) return 0;   // versions table not created yet
  return int.parse(r[0][0].toString());
}

// W3: revert the current user class to its most recent prior version and hot-reload
// (morph). Each Revert pops one entry off the undo stack, so repeated Reverts step
// further back. Only versions that were Accepted (and so passed the syntax gate) are
// on the stack, so the reload never sees uncompilable source.
void revert() {
  if (!userClassNames.contains(currentEditClass)) {
    ui.set('ed_status', {'text': 'Revert: $currentEditClass is not a live user class'});
    ui.commit();
    return;
  }
  var db = openImage();
  var v = db.query('SELECT id, source FROM versions WHERE kind = ? AND name = ? ORDER BY id DESC LIMIT 1',
      ['userlib', currentEditClass]);
  if (v == null || v.isEmpty) {
    db.close();
    ui.set('ed_status', {'text': 'Revert: no earlier version of $currentEditClass'});
    ui.commit();
    print('REVERT: $currentEditClass has no history');
    return;
  }
  var id = v[0][0];
  var oldSrc = v[0][1].toString();
  db.exec('INSERT OR REPLACE INTO userlib(name, source) VALUES(?, ?)', [currentEditClass, oldSrc]);
  db.exec('DELETE FROM versions WHERE id = ?', [id]);   // consume this undo step
  db.close();
  ui.set('ed_source', {'text': wr(oldSrc)});
  ui.commit();
  ui.applySpans('ed_source', lexDart(oldSrc));
  wsRequestUiReload();   // morph live instances to the reverted version
  ui.set('ed_status', {'text': 'Reverting $currentEditClass -> reloading (morph)...'});
  ui.commit();
  new Timer(new Duration(milliseconds: 350), () {
    var st = wsUiReloadStatus();
    var live = '   ' + liveState();
    ui.set('ed_status', {'text': 'Reverted $currentEditClass  (reload: "$st")$live   [${versionCount(currentEditClass)} older]'});
    ui.commit();
    print('REVERT: $currentEditClass reload="$st"$live older=${versionCount(currentEditClass)}');
  });
}

// W5: regenerate the boot snapshot from the VM's OWN gen_snapshot, then bake it into
// the image as a new versioned blob (W3 @prev preserved). The world rebuilds its own
// snapshot — no C++ build, no external tooling. Today this reproduces the core
// snapshot (the core libs are fixed in the binary); it becomes edit-capturing once
// core/app source moves into the image. Closes the DB-world loop:
// gen_snapshot -> .bin -> versioned blob -> boot.
void recreateSnapshot() {
  var exe = Platform.resolvedExecutable;                 // ...\dartui.exe
  var slash = exe.lastIndexOf('\\');
  if (slash < 0) slash = exe.lastIndexOf('/');
  var dir = exe.substring(0, slash + 1);                 // exe dir + trailing slash
  var gen = dir + 'gen_snapshot.exe';
  var vmBin = dir + 'vm_isolate_snapshot.bin';
  var isoBin = dir + 'isolate_snapshot.bin';
  print('RECREATE: running $gen (--snapshot_kind=core)');
  var r = Process.runSync(gen, <String>['--ignore_unrecognized_flags',
      '--snapshot_kind=core', '--vm_snapshot_data=' + vmBin,
      '--isolate_snapshot_data=' + isoBin]);
  print('RECREATE: gen_snapshot exit=${r.exitCode}');
  if (r.exitCode != 0) {
    print('RECREATE: FAILED: ${r.stderr}');
    return;
  }
  print('RECREATE: ' + wsBakeSnapshot());                // fresh .bin -> versioned blob
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

num _statVal(v) { try { return num.parse(v.toString()); } catch (e) { return 0; } }

// Feed the toolbar's live metric graph — total heap used (MB), sampled every second
// (always, regardless of the active tab, since the toolbar is always visible).
void pushToolbarMetric() {
  var s = wsVmStats();
  if (s == null || s.length < 9) return;
  var usedMB = (_statVal(s[0]) + _statVal(s[2])) / 1048576.0;
  var compiled = _statVal(s[6]).toInt();
  var codeK = (_statVal(s[8]) / 1024.0).round();
  wsPushToolbarMetric(usedMB.toDouble(),
      'heap ${usedMB.toStringAsFixed(1)} MB     compiled $compiled     code ${codeK}K');
}

// ── Find tab (T2): substring over classes + members -> jump to Browser ────────
List<String> findResults = <String>[];
void buildFind() {
  var W = paneW, H = paneH;
  ui.label('fd_lbl', text: 'Find   -   substring over classes and members (from the VM class table)', frame: <int>[12, 36, W - 24, 18]); track('fd_lbl');
  ui.field('fd_q', text: '', frame: <int>[12, 58, 300, 24], onEnter: doFind); track('fd_q');
  ui.button('fd_go', title: 'Find', frame: <int>[320, 57, 80, 26], onClick: doFind); track('fd_go');
  ui.label('fd_rl', text: 'Matches (click a result to open it in the Browser)', frame: <int>[12, 92, W - 24, 18]); track('fd_rl');
  ui.list('fd_results', frame: <int>[12, 114, W - 24, H - 124],
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
  var W = paneW, H = paneH;
  var listH = H - 68;
  var detX = 324, detW = W - detX - 12;
  ui.label('dc_lbl', text: 'Docs   -   the VM class reference (select a class to see its members)', frame: <int>[12, 36, W - 24, 18]); track('dc_lbl');
  ui.list('dc_classes', frame: <int>[12, 58, 300, listH],
      rowCount: () => classNames.length, cellAt: (r) => classNames[r], onSelect: selectDocsClass); track('dc_classes');
  ui.label('dc_dl', text: 'Members', frame: <int>[detX, 36, detW, 18]); track('dc_dl');
  ui.editor('dc_detail', frame: <int>[detX, 58, detW, listH]); track('dc_detail');
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
  var W = paneW, H = paneH;
  ui.label('hp_lbl', text: 'Help   -   WINDART workspace', frame: <int>[12, 36, W - 24, 18]); track('hp_lbl');
  ui.editor('hp_text', frame: <int>[12, 58, W - 24, H - 68]); track('hp_text');
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
  // Responsive layout: source editor fills the left column down to the button row
  // (pinned to the bottom); the stack / eval / transcript fill the right column.
  var W = paneW, H = paneH;
  var leftW = (W * 0.56).round();
  var rightX = leftW + 24, rightW = W - rightX - 12;
  var btnY = H - 44;                       // button row + status pinned to the bottom
  var srcH = btnY - 84 - 8;                // editor: y=84 down to just above the buttons

  ui.label('db_lbl', text: 'Debug   -   in-process debugger (classic embedder API) on a spawned target isolate',
      frame: <int>[12, 36, W - 24, 18]); track('db_lbl');
  ui.label('db_target', text: 'target: debug_target.dart      breakpoint: line $kDbgBreakLine (int factorial)      * = breakpoint   > = paused line',
      frame: <int>[12, 58, W - 24, 18]); track('db_target');

  ui.editor('db_src', frame: <int>[12, 84, leftW, srcH]); track('db_src');
  ui.button('db_over', title: 'Run / Step Over', frame: <int>[12, btnY, 130, 28], onClick: () => debugRun(0)); track('db_over');
  ui.button('db_into', title: 'Step Into', frame: <int>[150, btnY, 96, 28], onClick: () => debugRun(1)); track('db_into');
  ui.button('db_out', title: 'Step Out', frame: <int>[254, btnY, 96, 28], onClick: () => debugRun(2)); track('db_out');
  ui.button('db_resume', title: 'Resume', frame: <int>[358, btnY, 96, 28], onClick: () => debugRun(3)); track('db_resume');

  var stackH = ((btnY - 82) * 0.42).round();
  ui.label('db_sl', text: 'Call stack (top frame first)', frame: <int>[rightX, 60, rightW, 18]); track('db_sl');
  ui.list('db_stack', frame: <int>[rightX, 82, rightW, stackH],
      rowCount: () => dbgStackLines.length, cellAt: (r) => dbgStackLines[r], onSelect: (r) {}); track('db_stack');

  var evalY = 82 + stackH + 12;
  ui.label('db_el', text: 'Evaluate in frame:', frame: <int>[rightX, evalY, 130, 18]); track('db_el');
  ui.field('db_eval', text: 'n * n', frame: <int>[rightX + 132, evalY - 2, rightW - 132, 24]); track('db_eval');
  ui.label('db_evalout', text: '(result appears here)', frame: <int>[rightX, evalY + 28, rightW, 18]); track('db_evalout');

  var logLY = evalY + 56;
  var logTop = logLY + 22;
  ui.label('db_ll', text: 'Session transcript', frame: <int>[rightX, logLY, rightW, 18]); track('db_ll');
  ui.editor('db_log', frame: <int>[rightX, logTop, rightW, btnY - logTop - 8]); track('db_log');
  ui.label('db_status', text: 'click Run / Step Over to start a debug session',
      frame: <int>[rightX, btnY + 4, rightW, 18]); track('db_status');

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

// ── Game tab (D3D11 game pane): run a gp game in its OWN isolate ──────────────
// The plug-in-app-off-the-UI-isolate model: the game runs in a spawned isolate that
// flushes ['draw', cmds] frames over a port; we gpOpen on the first `gpopen` command,
// then gpApply each frame and pull-pace the next. Mirrors test/game_live.dart.
final gpGames = const ['coindash', '13_invaders', '13_invaders_hlsl', '15_brickout', '12_copper', 'tiletest', 'rgbatest', 'fonttest', 'abc'];
String gameSel = 'coindash';
const int gpW = 424, gpH = 240;              // logical game size (the engine letterboxes)
ReceivePort gameRp;
SendPort gameCtl;
Isolate gameIso;
bool gameOpened = false, gameDone = false;
int gameFrames = 0;

void gameTick() { if (gameCtl != null && !gameDone) gameCtl.send(<dynamic>[<dynamic>[], 0]); }
void gameSchedule() {
  if (!gameDone && activeTab == 9) new Timer(new Duration(milliseconds: 16), gameTick);
}

void stopGame() {
  gameDone = true;
  if (gameRp != null) { gameRp.close(); gameRp = null; }
  gameCtl = null;
  if (gameIso != null) { try { gameIso.kill(priority: Isolate.immediate); } catch (e) {} gameIso = null; }
  if (gameOpened) { gpClose(); gameOpened = false; }
}

void startGame(String name) {
  stopGame();
  gameSel = name;
  gameDone = false; gameOpened = false; gameFrames = 0;
  var rp = new ReceivePort();
  gameRp = rp;
  rp.listen((msg) {
    if (gameDone || rp != gameRp || msg is! List || msg.isEmpty) return;   // stale/late frame
    if (msg[0] == 'port') { gameCtl = msg[1]; gameTick(); return; }
    if (msg[0] != 'draw') return;
    var cmds = msg[1];
    if (!gameOpened) {
      var first = (cmds is List && cmds.isNotEmpty && cmds[0] is List && cmds[0].isNotEmpty) ? cmds[0][0] : null;
      if (first == 'gpopen') {
        var o = cmds[0];
        gpOpen(o[1], o[2], o[3], o[4], 0);
        gameOpened = true;
        gameSchedule();
        return;                                   // the gpopen frame just opens; invite the next
      }
      gpOpen(gpW, gpH, gpW, gpH, 0);
      gameOpened = true;
    }
    gpApply(cmds);                                // apply + render_present + swapchain Present
    gameFrames++;
    gameSchedule();
  });
  Isolate.spawnUri(Uri.parse('demos/$name.dart'), <String>['$gpW', '$gpH'], rp.sendPort)
      .then((iso) { gameIso = iso; })
      .catchError((e) { print('GAME: spawn error $name: $e'); });
}

void buildGame() {
  var W = paneW, H = paneH;
  ui.label('gm_lbl', text: 'Game   -   pick a game; it runs in its OWN isolate, flushing frames to the D3D11 pane',
      frame: <int>[12, 36, W - 24, 18]); track('gm_lbl');
  ui.list('gm_list', frame: <int>[12, 60, 180, H - 96],
      rowCount: () => gpGames.length, cellAt: (r) => gpGames[r],
      onSelect: (r) { if (r >= 0 && r < gpGames.length) startGame(gpGames[r]); }); track('gm_list');
  ui.label('gm_status', text: 'running: $gameSel', frame: <int>[12, H - 30, 180, 18]); track('gm_status');
  var gx = 204;
  ui.game('gp', frame: <int>[gx, 60, W - gx - 12, H - 72]); track('gp');
  ui.commit();
  startGame(gameSel);
}

void buildTab(int i) {
  if (activeTab == 9 && i != 9) stopGame();   // leaving the Game tab -> stop the game isolate
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
    case 9: buildGame(); break;
    default: buildPlaceholder(i); break;
  }
  ui.commit();
  wsSetStatus('${tabNames[i]}     |     WINDART   -   image: $imgPath');
}

void switchTab(int i) {          // programmatic (self-test): set the strip + rebuild
  ui.set('tabs', {'tab': i});
  buildTab(i);
}

// Coalesce a burst of WM_SIZE events (a continuous border drag fires many per
// second) into ONE relayout on the trailing edge — a full tab teardown/rebuild
// per event flickers the RichEdit controls. paneW/paneH track the latest size
// immediately so a mid-drag tab switch still lays out at the current bounds.
Timer _resizeTimer = null;
int _resizeW = 0, _resizeH = 0;
void onResizeCoalesced(int w, int h) {
  if (w <= 0 || h <= 0) return;
  _resizeW = w; _resizeH = h;
  paneW = w; paneH = h;
  if (_resizeTimer != null) _resizeTimer.cancel();
  _resizeTimer = new Timer(new Duration(milliseconds: 60), () {
    _resizeTimer = null;
    relayout(_resizeW, _resizeH);
  });
}

// Reflow for a new pane (container) size (kind-7 resize). Re-place the tab strip
// across the new width, then rebuild the active tab so its size-parameterized
// layout fills the new bounds. buildTab commits; the strip 'place' rides along.
void relayout(int w, int h) {
  if (w <= 0 || h <= 0) return;
  paneW = w; paneH = h;
  ui.place('tabs', <int>[0, 0, w, 26]);
  buildTab(activeTab);
  print('RESIZE: pane ${w}x$h -> tab $activeTab relaid out');
}

void snap(String name) {
  var e = ui.snapshot('e:/windart/build/$name.png');
  print('SNAP: $name ${e.isEmpty ? "OK" : "ERR:$e"}');
}

// ── menu / toolbar commands (polled from the host queue) ──────────────────────
// The host's menu bar + icon toolbar push Dart-routed command ids; we poll them
// (wsMenuPoll) and map ids -> actions. Ids MUST match win_host.h (WinHostCommand).
void menuDoIt() {
  if (activeTab != 0) switchTab(0);
  var code = ui.textOf('ws_editor');
  if (code == null) return;
  code = code.replaceAll('\r', ' ').replaceAll('\n', ' ').trim();
  if (code.isEmpty) return;
  var result = wsEval(code);
  wsLog.writeln('$code   =>   $result');
  ui.set('ws_output', {'text': wr(wsLog.toString())});
  ui.commit();
  print('MENU-DOIT: $code => $result');
}

void dispatchMenu(int id) {
  if (id >= 200 && id <= 208) { switchTab(id - 200); print('MENU: tab ${id - 200}'); return; }
  switch (id) {
    case 140: case 141: menuDoIt(); break;                 // Do It / Print It
    case 148:                                              // Refresh
      buildTab(activeTab); if (activeTab == 7) refreshVM(); break;
    case 130:                                              // New Class
      switchTab(2);
      currentEditClass = 'NewClass';
      ui.set('ed_source', {'text': wr('class NewClass {\n  \n}')});
      ui.set('ed_status', {'text': 'new class (edit + Accept to create)'});
      ui.commit();
      break;
    case 131: buildEditorClassList(); switchTab(2); break; // Open (refresh class list)
    case 132: if (activeTab != 2) switchTab(2); accept(); break;   // Save
    case 145: browseHome(); break;                         // Home -> Browser, top class
    case 146: browseBack(); break;                         // Back (browser history)
    case 147: browseForward(); break;                      // Forward (browser history)
    case 149: switchTab(3); break;                         // Find
    case 150: switchTab(1); break;                         // Browse -> Browser
    default: break;
  }
  print('MENU: dispatched cmd $id');
}

void pollMenu() {
  var id = wsMenuPoll();
  while (id >= 0) { dispatchMenu(id); id = wsMenuPoll(); }
}

// ── main ─────────────────────────────────────────────────────────────────────
main(List<String> args) {
  var selftest = args.contains('selftest');
  var bake = args.contains('bake');   // W1: write the on-disk snapshot into the image

  // Browser data: the VM's class table, grouped by library (the Browser's
  // categories). classMirrors/classNames stay flat for Find/Docs/nav.
  for (var lib in currentMirrorSystem().libraries.values) {
    var lname;
    try { lname = lib.uri.toString(); }
    catch (e) { lname = MirrorSystem.getName(lib.simpleName); }
    var inLib = <String>[];
    lib.declarations.forEach((sym, decl) {
      if (decl is ClassMirror) {
        var name = MirrorSystem.getName(decl.simpleName);
        if (name.isNotEmpty && !name.startsWith('_')) {
          classMirrors[name] = decl;
          inLib.add(name);
          libOfClass.putIfAbsent(name, () => lname);
        }
      }
    });
    if (inLib.isNotEmpty) {
      inLib.sort();
      classesInLib[lname] = inLib;
      libraryNames.add(lname);
    }
  }
  libraryNames.sort();
  classNames = classMirrors.keys.toList()..sort();

  // The workspace image.
  var home = Platform.environment['USERPROFILE'];
  var dir = new Directory(home + '\\.windart');
  if (!dir.existsSync()) dir.createSync(recursive: true);
  imgPath = (dir.path + '\\workspace.sqlite').replaceAll('\\', '/');
  var db = openImage(); db.close();   // ensure the image + table exist

  // Stage 1: a live user-class instance to morph on Accept (single isolate).
  liveCounter = new Counter()..bump()..bump()..bump();   // n = 3
  print('STAGE1: liveCounter created (Counter.n=${liveCounter.n})');

  ui = new Ui.pane(1084, 740);
  // Size the initial layout to the real pane (Win_surfaceSize reads the container's
  // client rect); fall back to the defaults if it is not ready.
  var w0 = ui.width, h0 = ui.height;
  if (w0 > 0) paneW = w0;
  if (h0 > 0) paneH = h0;
  ui.title('WINDART Workspace   -   a live Windows Dart IDE');
  ui.tabs('tabs', items: tabNames, frame: <int>[0, 0, paneW, 26], onSelect: (i) => buildTab(i));
  ui.onResize(onResizeCoalesced); // reflow on window resize (kind 7), debounced
  persistentWidgets = ui.widgetIds.toSet();  // the tab strip (+ any chrome) survives switches
  buildTab(0);
  ui.commit();
  uiReady();

  // Headless one-shot commands (bake / export / import): the image work runs after
  // the UI is up but BEFORE the periodic timers, so the fast quit does not race the
  // 150 ms pollMenu tick during the first wake (that race left the isolate teardown
  // exiting -1). One Timer dispatches whichever command was asked for, then quits.
  var oneShot = null;
  if (bake) {
    // W1: bake the on-disk snapshot .bin into the image; the NEXT boot loads it
    // from the DB blob (win_host fallback chain).
    oneShot = () => print('BAKE: ' + wsBakeSnapshot());
  } else if (args.contains('rollback-snapshot')) {
    // W3: promote the @prev snapshot back to current (roll back the last bake).
    oneShot = () => print('ROLLBACK: ' + wsRollbackSnapshot());
  } else if (args.contains('recreate-snapshot')) {
    // W5: regenerate the boot snapshot via gen_snapshot, then versioned-bake it.
    oneShot = recreateSnapshot;
  } else {
    // W2 git bridge: export <dir> projects the image to loose files; import <dir>
    // builds <dir>/world.sqlite from them (never the live image).
    var wExport = args.indexOf('export');
    var wImport = args.indexOf('import');
    if (wExport >= 0 && wExport + 1 < args.length) {
      var dir = args[wExport + 1];
      oneShot = () => print('EXPORT: ' + wsExportWorld(dir));
    } else if (wImport >= 0 && wImport + 1 < args.length) {
      var dir = args[wImport + 1];
      oneShot = () => print('IMPORT: ' + wsImportWorld(dir, dir + '/world.sqlite'));
    }
  }
  if (oneShot != null) {
    // Exit via dart:io exit(0) rather than hostQuit()/WM_CLOSE: the GUI teardown on
    // the fast one-shot path reports process exit -1, which would make a release
    // script think import-world failed. exit(0) is a clean, deterministic 0.
    new Timer(new Duration(milliseconds: 300), () { oneShot(); exit(0); });
    return;   // a one-shot command starts no periodic timers
  }

  new Timer.periodic(new Duration(seconds: 1), (_) => refreshVM());
  new Timer.periodic(new Duration(milliseconds: 150), (_) => pollMenu());   // menu/toolbar
  new Timer.periodic(new Duration(seconds: 1), (_) => pushToolbarMetric());  // toolbar graph
  pushToolbarMetric();

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
    // Item 4: categorized drill-down — library -> class -> vars/methods -> source.
    new Timer(new Duration(milliseconds: t), () {
      switchTab(1);
      var li = libraryNames.indexOf('dart:io');
      if (li >= 0) selectLibrary(li);
      print('CATBROWSE: dart:io -> ${libClasses.length} classes');
    }); t += 450;
    new Timer(new Duration(milliseconds: t), () {
      var ci = libClasses.indexOf('File');
      if (ci < 0 && libClasses.isNotEmpty) ci = 0;
      if (ci >= 0) selectLibClass(ci);
    }); t += 450;
    new Timer(new Duration(milliseconds: t), () { snap('browser_categorized'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { if (brMethods.isNotEmpty) selectBrMethod(0); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { snap('browser_member'); }); t += 450;
    // P3: instance/class side toggle — Duration has a rich class side (static
    // consts + ctor) and a full instance side (accessors/operators/methods).
    new Timer(new Duration(milliseconds: t), () { browseToClass('Duration'); snap('browser_instanceside'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { setBrowserSide(1); snap('browser_classside'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { setBrowserSide(0); }); t += 300;
    // P4: real on-disk SDK source — the Source pane shows actual bodies, not
    // just mirror signatures. Capture the class source and one method body.
    new Timer(new Duration(milliseconds: t), () { browseToClass('Duration'); snap('browser_realsource'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () {
      var mi = 0;
      for (var i = 0; i < brMethods.length; i++) { if (brMethods[i].contains('abs(')) { mi = i; break; } }
      selectBrMethod(mi); snap('browser_realmethod');
    }); t += 450;
    // Item 5: draggable splitter — capture the divider at rest, then after a drag.
    new Timer(new Duration(milliseconds: t), () { switchTab(1); selectClass(richIdx); }); t += 500;
    new Timer(new Duration(milliseconds: t), () { snap('splitter_before'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () {
      var tk = ui.ticketOf('br_split');
      if (tk != null) wsDragWidget(tk, 120, 0);    // drag the divider 120px right
      print('SPLIT: dragged br_split ticket=$tk');
    }); t += 450;
    new Timer(new Duration(milliseconds: t), () { snap('splitter_after'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { switchTab(0); doIt(); snap('tab_workspace'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { switchTab(2); snap('tab_editor'); }); t += 450;
    // Item 3: a VM class in the Editor shows the full declaration incl. real
    // method signatures (return + parameter types), not just member names.
    new Timer(new Duration(milliseconds: t), () {
      switchTab(2);
      var i = editorClassList.indexOf('Duration');
      if (i >= 0) ui.set('ed_class', {'index': i});
      loadEditorClass('Duration');
    }); t += 500;
    new Timer(new Duration(milliseconds: t), () { snap('editor_vmclass'); }); t += 450;
    // Stage 1: LIVE MORPH in the IDE. Counter v1 (n; bump+1) -> v2 (add `int step
    // = 7`; bump += step). Accept rewrites user_lib.dart from the image + reloads;
    // the live liveCounter (n=3) KEEPS n=3 and GAINS step=7 (state kept, field added).
    new Timer(new Duration(milliseconds: t), () {
      switchTab(2);
      var ci = editorClassList.indexOf('Counter');
      if (ci >= 0) ui.set('ed_class', {'index': ci});
      loadEditorClass('Counter');
      ui.set('ed_status', {'text': 'before Accept -> ' + liveState()});
      ui.commit();
    }); t += 500;
    new Timer(new Duration(milliseconds: t), () { snap('editor_counter_v1'); }); t += 400;
    new Timer(new Duration(milliseconds: t), () {
      var v2 = 'class Counter {\n  int n = 0;\n  int inc() => 7;\n  Counter bump() { n = n + inc(); return this; }\n}';
      ui.set('ed_source', {'text': wr(v2)});
      ui.commit();
      currentEditClass = 'Counter';
      accept();   // -> image + user_lib.dart -> reload -> morph; accept reads liveState after 350ms
    }); t += 950;
    new Timer(new Duration(milliseconds: t), () { snap('editor_morph'); }); t += 450;
    // W3 (versioning): a second Accept (step=11) leaves the step=7 version on the
    // undo stack; Revert pops it and morphs the live instance back to step=7.
    new Timer(new Duration(milliseconds: t), () {
      var v3 = 'class Counter {\n  int n = 0;\n  int inc() => 11;\n  Counter bump() { n = n + inc(); return this; }\n}';
      ui.set('ed_source', {'text': wr(v3)});
      ui.commit();
      currentEditClass = 'Counter';
      accept();
    }); t += 950;
    new Timer(new Duration(milliseconds: t), () { currentEditClass = 'Counter'; revert(); }); t += 950;
    new Timer(new Duration(milliseconds: t), () { snap('editor_revert'); }); t += 450;
    // Validated gate (syntax-check-first): a BAD edit is REJECTED before any write
    // or reload — wsCheckSyntax catches it, so the live instance AND the DB are
    // untouched and the next boot stays safe.
    new Timer(new Duration(milliseconds: t), () {
      ui.set('ed_source', {'text': wr('class Counter {\n  int n = 0\n  this is not valid dart\n}')});
      ui.commit();
      currentEditClass = 'Counter';
      accept();   // -> wsCheckSyntax rejects; no DB write, no reload
    }); t += 600;
    new Timer(new Duration(milliseconds: t), () { snap('editor_reject'); }); t += 450;
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
    // Tab-clear regression: RE-VISIT App (a 2nd build) then switch away. The keypad
    // must NOT bleed onto the next tab (ui.remove now drops ids from the ticket map,
    // so buildApp's before/after widgetIds diff tracks the keypad on every visit).
    new Timer(new Duration(milliseconds: t), () { switchTab(6); }); t += 350;   // Debug
    new Timer(new Duration(milliseconds: t), () { switchTab(5); }); t += 350;   // App AGAIN
    new Timer(new Duration(milliseconds: t), () {
      switchTab(1);                        // -> Browser (gappy: any keypad bleed shows)
      var kcount = ui.widgetIds.where((s) => s.length >= 1 && s[0] == 'k').length;
      print('REVISIT: keypad widgets after leaving App = $kcount (0 = destroyed, not hidden)');
    }); t += 250;
    new Timer(new Duration(milliseconds: t), () { snap('tab_revisit'); }); t += 450;  // Browser — must be clean
    new Timer(new Duration(milliseconds: t), () { switchTab(4); selectDocsClass(docIdx); snap('tab_docs'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { switchTab(8); snap('tab_help'); }); t += 450;
    // Debug: open the tab, run one scripted debug session (breakpoint -> pause ->
    // stack -> frame-eval -> step -> resume -> complete), let it settle, snapshot.
    new Timer(new Duration(milliseconds: t), () { switchTab(6); debugRun(0); }); t += 1800;
    new Timer(new Duration(milliseconds: t), () { snap('tab_debug'); }); t += 450;
    // Polish captures: FULL window (frame + menu bar + toolbar + client). Build the
    // tab in one tick, snapshot in the NEXT (a message-loop cycle so removed widgets'
    // areas repaint/erase before PrintWindow — no stale pixels from a taller tab).
    new Timer(new Duration(milliseconds: t), () { dispatchMenu(202); }); t += 450;   // -> Editor (via menu id)
    new Timer(new Duration(milliseconds: t), () {
      var e = wsSnapshotFull('e:/windart/build/polish_editor.png');
      print('SNAP: polish_editor ${e.isEmpty ? "OK" : "ERR:$e"}');
    }); t += 450;
    new Timer(new Duration(milliseconds: t), () { switchTab(1); }); t += 450;        // -> Browser (rich overview)
    new Timer(new Duration(milliseconds: t), () {
      var e = wsSnapshotFull('e:/windart/build/polish_overview.png');
      print('SNAP: polish_overview ${e.isEmpty ? "OK" : "ERR:$e"}');
    }); t += 450;

    // ── Item 1 proof: fire commands through the REAL toolbar path ─────────────
    // wsFireCommand synthesizes the exact WM_COMMAND an icon-toolbar button posts
    // (lParam == toolbar HWND), so this exercises WndProc -> OnMenuCommand ->
    // host queue -> pollMenu -> dispatchMenu — the whole chain, not just Dart.
    var navA = classNames.indexOf('Duration'); if (navA < 0) navA = 0;
    var navB = classNames.indexOf('StringBuffer');
    if (navB < 0) navB = (classNames.length > 1 ? 1 : 0);
    // Do-It from the toolbar: set a distinctive expression, then fire CMD_DOIT.
    new Timer(new Duration(milliseconds: t), () {
      switchTab(0); ui.set('ws_editor', {'text': '111 + 222'}); ui.commit();
    }); t += 450;
    new Timer(new Duration(milliseconds: t), () { wsFireCommand(140); pollMenu(); }); t += 450; // CMD_DOIT
    new Timer(new Duration(milliseconds: t), () { snap('toolbar_doit'); }); t += 450;
    // Browser Back from the toolbar: reset history, visit two classes, fire CMD_BACK.
    new Timer(new Duration(milliseconds: t), () {
      switchTab(1);
      browseHistory = <int>[]; browseCursor = -1;   // deterministic proof
      selectClass(navA); selectClass(navB);
      print('NAV setup: A=${classNames[navA]} B=${classNames[navB]} hist=$browseHistory cur=$browseCursor');
    }); t += 450;
    new Timer(new Duration(milliseconds: t), () { wsFireCommand(146); pollMenu(); }); t += 450; // CMD_BACK -> class A
    new Timer(new Duration(milliseconds: t), () { snap('toolbar_back'); }); t += 450;
    // Home from the toolbar: fire CMD_HOME -> Browser top class.
    new Timer(new Duration(milliseconds: t), () { wsFireCommand(145); pollMenu(); }); t += 450; // CMD_HOME
    new Timer(new Duration(milliseconds: t), () { snap('toolbar_home'); }); t += 450;

    // ── Item 2 proof: resize the real OS window; the Browser reflows to fill ──
    new Timer(new Duration(milliseconds: t), () { switchTab(1); selectClass(navA); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { wsResizeWindow(1460, 940); }); t += 500;  // WM_SIZE -> onResize
    new Timer(new Duration(milliseconds: t), () { snap('resize_large'); }); t += 500;
    new Timer(new Duration(milliseconds: t), () { wsResizeWindow(820, 560); }); t += 500;
    new Timer(new Duration(milliseconds: t), () { snap('resize_small'); }); t += 500;
    new Timer(new Duration(milliseconds: t), () { wsResizeWindow(1100, 800); }); t += 450;  // restore

    // ── Game tab: spawn a gp game in its OWN isolate, let it render frames into
    // the D3D11 pane, capture the HONEST frames (gpSnap = offscreen RT, gpSnapPresent
    // = on-screen swapchain — both independent of PrintWindow), then drive the
    // selector to a second game, then leave (stopGame must shut the isolate cleanly).
    new Timer(new Duration(milliseconds: t), () { switchTab(9); print('GAME: -> Game tab, spawning $gameSel'); }); t += 2200;
    new Timer(new Duration(milliseconds: t), () {
      print('GAME: $gameSel frames=$gameFrames opened=$gameOpened');
      var a = gpSnap('e:/windart/build/game_pane.png');
      var b = gpSnapPresent('e:/windart/build/game_present.png');
      print('GAME: gpSnap ${a.isEmpty ? "OK" : "ERR:$a"}  gpSnapPresent ${b.isEmpty ? "OK" : "ERR:$b"}');
      snap('tab_game');
    }); t += 450;
    // Selector proof + tile-layers on-screen: switch to tiletest via startGame,
    // capture BOTH offscreen (gpSnap) and the live swapchain (gpSnapPresent).
    new Timer(new Duration(milliseconds: t), () { startGame('tiletest'); print('GAME: -> tiletest'); }); t += 2200;
    new Timer(new Duration(milliseconds: t), () {
      print('GAME: tiletest frames=$gameFrames');
      var a = gpSnap('e:/windart/build/game_tiletest.png');
      var b = gpSnapPresent('e:/windart/build/game_tiletest_present.png');
      print('GAME: tiletest gpSnap ${a.isEmpty ? "OK" : "ERR:$a"}  present ${b.isEmpty ? "OK" : "ERR:$b"}');
    }); t += 450;
    // Leaving the Game tab must stop the isolate cleanly (no crash, no bleed).
    new Timer(new Duration(milliseconds: t), () { switchTab(1); print('GAME: left Game tab -> stopGame done=$gameDone'); }); t += 450;
    new Timer(new Duration(milliseconds: t), () { snap('tab_after_game'); }); t += 450;

    new Timer(new Duration(milliseconds: t), () { print('SELFTEST: done'); hostQuit(); });
  }
  // else: stay open — a real interactive application.
}
