// WINDART S7.2 — the code editor + the live Do-It gesture. A RichEdit editor,
// lexDart syntax highlighting via Win_editorApplySpans, and Do-It: evaluate the
// editor's content against the LIVE VM (Dart_EvaluateExpr) and show the result.
//
// Two headless proofs: (a) a syntax-coloured snippet in the editor (PNG), and
// (b) captured Do-It results to stdout (expressions evaluated against the VM).
//   dartui.exe workspace_editor.dart e:/windart/build/workspace_editor.png
import 'dart:win';
import 'dart:async';

// ── lexDart — ported VERBATIM from workspace.dart:1024-1102 (platform-neutral).
// Produces flat [start, len, kind, ...] runs: 1 keyword, 2 string, 3 comment,
// 4 number, 5 type, 0 default. The applySpans contract is byte-identical to Cocoa.
final Set<String> _dartKeywords = new Set<String>.from(<String>[
  'abstract','as','assert','async','await','break','case','catch','class','const',
  'continue','default','deferred','do','dynamic','else','enum','export','extends',
  'external','factory','false','final','finally','for','get','if','implements',
  'import','in','is','library','new','null','operator','part','rethrow','return',
  'set','static','super','switch','sync','this','throw','true','try','typedef',
  'var','void','while','with','yield','bool','int','double','num',
]);
bool _isDigit(int c) => c >= 0x30 && c <= 0x39;
bool _isHex(int c) => _isDigit(c) || (c >= 0x41 && c <= 0x46) || (c >= 0x61 && c <= 0x66);
bool _isUpper(int c) => c >= 0x41 && c <= 0x5A;
bool _isAlpha(int c) => _isUpper(c) || (c >= 0x61 && c <= 0x7A);
bool _isIdentStart(int c) => _isAlpha(c) || c == 0x5F || c == 0x24;
bool _isIdentPart(int c) => _isIdentStart(c) || _isDigit(c);

List<int> lexDart(String s) {
  var out = <int>[];
  var n = s.length, i = 0;
  while (i < n) {
    var c = s.codeUnitAt(i);
    if (c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D) { i++; continue; }
    if (c == 0x2F && i + 1 < n) {
      var d = s.codeUnitAt(i + 1);
      if (d == 0x2F) {
        var st = i; while (i < n && s.codeUnitAt(i) != 0x0A) i++;
        out..add(st)..add(i - st)..add(3); continue;
      }
      if (d == 0x2A) {
        var st = i; i += 2;
        while (i + 1 < n && !(s.codeUnitAt(i) == 0x2A && s.codeUnitAt(i + 1) == 0x2F)) i++;
        i = (i + 1 < n) ? i + 2 : n;
        out..add(st)..add(i - st)..add(3); continue;
      }
    }
    if (c == 0x27 || c == 0x22) {
      var st = i, q = c;
      var triple = i + 2 < n && s.codeUnitAt(i + 1) == q && s.codeUnitAt(i + 2) == q;
      if (triple) {
        i += 3;
        while (i + 2 < n && !(s.codeUnitAt(i) == q && s.codeUnitAt(i + 1) == q && s.codeUnitAt(i + 2) == q)) {
          if (s.codeUnitAt(i) == 0x5C) i++;
          i++;
        }
        i = (i + 2 < n) ? i + 3 : n;
      } else {
        i++;
        while (i < n && s.codeUnitAt(i) != q && s.codeUnitAt(i) != 0x0A) {
          if (s.codeUnitAt(i) == 0x5C) i++;
          i++;
        }
        if (i < n && s.codeUnitAt(i) == q) i++;
      }
      out..add(st)..add(i - st)..add(2); continue;
    }
    if (_isDigit(c)) {
      var st = i;
      if (c == 0x30 && i + 1 < n && (s.codeUnitAt(i + 1) == 0x78 || s.codeUnitAt(i + 1) == 0x58)) {
        i += 2; while (i < n && _isHex(s.codeUnitAt(i))) i++;
      } else {
        while (i < n) {
          var d = s.codeUnitAt(i);
          if (_isDigit(d) || d == 0x2E || d == 0x65 || d == 0x45 || d == 0x5F) i++; else break;
        }
      }
      out..add(st)..add(i - st)..add(4); continue;
    }
    if (_isIdentStart(c)) {
      var st = i; i++;
      while (i < n && _isIdentPart(s.codeUnitAt(i))) i++;
      var word = s.substring(st, i);
      var kind = _dartKeywords.contains(word) ? 1 : (_isUpper(c) ? 5 : 0);
      out..add(st)..add(i - st)..add(kind); continue;
    }
    i++;
  }
  return out;
}

main(List<String> args) {
  var out = args.isNotEmpty ? args[0] : 'e:/windart/build/workspace_editor.png';

  var ui = new Ui.pane(1084, 740);
  ui.title('WINDART Workspace  -  editor + live Do It');
  var tabNames = ['Workspace', 'Browser', 'Editor', 'Find', 'Docs', 'App',
                  'Debug', 'VM', 'Help'];
  ui.tabs('tabs', items: tabNames, frame: <int>[0, 0, 1084, 26]);
  ui.label('el',
      text: 'Workspace  -  edit Dart, Do It evaluates against the live VM',
      frame: <int>[12, 34, 900, 18]);
  ui.editor('editor', frame: <int>[12, 56, 1060, 320]);
  ui.label('ol', text: 'Output  (Do It results from Dart_EvaluateExpr)',
      frame: <int>[12, 386, 900, 18]);
  ui.editor('output', frame: <int>[12, 408, 1060, 296]);
  ui.commit();
  uiReady();

  var log = new StringBuffer();
  void doit(String expr) {
    ui.set('editor', {'text': expr});          // put the expression in the editor
    ui.commit();
    var sel = ui.editorSelection('editor');    // whole buffer (no selection) == the expr
    var result = wsEval(sel[2]);               // -> Dart_EvaluateExpr against the live VM
    print('DOIT: ${sel[2]}  =>  $result');
    log.writeln('${sel[2]}   =>   $result');
    ui.set('output', {'text': log.toString().replaceAll('\n', '\r')});
    ui.commit();
  }

  // A snippet with a comment, keywords, types, a string, and numbers.
  var snippet =
      '// A little Dart class, highlighted by lexDart -> Win_editorApplySpans\n'
      'class Point {\n'
      "  final int x, y;\n"
      "  String label = 'the origin';\n"
      '  num distance() => 3.14159 * (x + y) + 0xFF;\n'
      '  static bool near(Point p) => p.x < 10 && p.y < 10;\n'
      '}';

  new Timer(new Duration(milliseconds: 500), () {
    // Proof (b): live Do-It against the running VM.
    doit('(2 + 3) * 7');
    doit('new List.filled(3, 9).fold(0, (a, b) => a + b)');
    doit("'windart'.toUpperCase()");
    doit('new List.generate(6, (i) => i * i)');

    // Proof (a): the syntax-coloured editor (RichEdit uses \\r line breaks).
    ui.set('editor', {'text': snippet.replaceAll('\n', '\r')});
    ui.commit();
    ui.applySpans('editor', lexDart(snippet));

    new Timer(new Duration(milliseconds: 250), () {
      var e = ui.snapshot(out);
      print('WS: SNAP editor -> $out ${e.isEmpty ? "OK" : "ERR:$e"}');
      hostQuit();
    });
  });
}
