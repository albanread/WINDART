// WINDART — interactive live workspace launcher (stays open; no hostQuit).
// Type Dart in the top editor, click "Do It", and it evaluates against the
// running VM via Dart_EvaluateExpr, showing the result below.
import 'dart:win';
import 'dart:async';

// lexDart — ported verbatim (syntax runs: 1 kw, 2 str, 3 comment, 4 num, 5 type).
final Set<String> _kw = new Set<String>.from(<String>[
  'abstract','as','assert','async','await','break','case','catch','class','const',
  'continue','default','do','dynamic','else','enum','export','extends','external',
  'factory','false','final','finally','for','get','if','implements','import','in',
  'is','library','new','null','operator','part','rethrow','return','set','static',
  'super','switch','this','throw','true','try','typedef','var','void','while','with',
  'yield','bool','int','double','num','String','List','Map',
]);
bool _dig(int c) => c >= 0x30 && c <= 0x39;
bool _up(int c) => c >= 0x41 && c <= 0x5A;
bool _al(int c) => _up(c) || (c >= 0x61 && c <= 0x7A);
bool _ids(int c) => _al(c) || c == 0x5F || c == 0x24;
bool _idp(int c) => _ids(c) || _dig(c);
List<int> lexDart(String s) {
  var out = <int>[]; var n = s.length, i = 0;
  while (i < n) {
    var c = s.codeUnitAt(i);
    if (c==0x20||c==0x09||c==0x0A||c==0x0D){i++;continue;}
    if (c==0x2F && i+1<n && s.codeUnitAt(i+1)==0x2F){var st=i;while(i<n&&s.codeUnitAt(i)!=0x0A)i++;out..add(st)..add(i-st)..add(3);continue;}
    if (c==0x27||c==0x22){var st=i,q=c;i++;while(i<n&&s.codeUnitAt(i)!=q&&s.codeUnitAt(i)!=0x0A){if(s.codeUnitAt(i)==0x5C)i++;i++;}if(i<n&&s.codeUnitAt(i)==q)i++;out..add(st)..add(i-st)..add(2);continue;}
    if (_dig(c)){var st=i;while(i<n){var d=s.codeUnitAt(i);if(_dig(d)||d==0x2E||d==0x78||d==0x58||(d>=0x61&&d<=0x66)||(d>=0x41&&d<=0x46))i++;else break;}out..add(st)..add(i-st)..add(4);continue;}
    if (_ids(c)){var st=i;i++;while(i<n&&_idp(s.codeUnitAt(i)))i++;var w=s.substring(st,i);var k=_kw.contains(w)?1:(_up(c)?5:0);out..add(st)..add(i-st)..add(k);continue;}
    i++;
  }
  return out;
}

Ui ui;
void highlight() => ui.applySpans('input', lexDart(_currentInput));
String _currentInput = '';

void doIt() {
  var sel = ui.editorSelection('input');           // whole buffer == what to eval
  var expr = sel[2].toString().trim();
  if (expr.isEmpty) return;
  String result;
  try { result = wsEval(expr).toString(); }
  catch (e) { result = 'error: $e'; }
  _out.writeln('$expr\r   =>   $result\r');
  ui.set('output', {'text': _out.toString()});
  ui.commit();
  print('DOIT: $expr => $result');
}
final StringBuffer _out = new StringBuffer();

main(List<String> args) {
  ui = new Ui.pane(1084, 760);
  ui.title('WINDART  -  live Dart workspace');
  ui.tabs('tabs', items: ['Workspace','Browser','Editor','Find','Docs','App','Debug','VM','Help'],
          frame: <int>[0,0,1084,26]);
  ui.label('hint', text: 'Type Dart below and click  Do It  -  it evaluates against the running VM.',
           frame: <int>[12,34,900,18]);
  _currentInput =
      "// Live! Edit this and click Do It.\r"
      "new List.generate(8, (i) => i * i)";
  ui.editor('input', text: _currentInput, frame: <int>[12,58,1060,240], onText: (t){ _currentInput = t; });
  ui.button('doit', title: 'Do It', frame: <int>[12,308,120,30], onClick: doIt);
  ui.label('ol', text: 'Output  (results from Dart_EvaluateExpr, live)', frame: <int>[12,348,900,18]);
  ui.editor('output', frame: <int>[12,370,1060,376]);
  ui.commit();
  uiReady();

  new Timer(new Duration(milliseconds: 350), () {
    highlight();
    doIt();                                          // one sample eval so it's not empty
    var e = ui.snapshot(args.isNotEmpty ? args[0] : 'e:/windart/build/workspace_live.png');
    print('LIVE: window up, snapshot ${e.isEmpty ? "OK" : "ERR:$e"} — staying open (interactive).');
    // NO hostQuit — the window stays open and the Do It button is live.
  });
}
