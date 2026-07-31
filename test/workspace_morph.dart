// WINDART S7.3 — the morphing hot-reload capstone (the second Smalltalk gesture,
// Accept). Make a Counter, increment it (n=3); then EDIT the class (add a field)
// + Accept -> the host hot-reloads at the pump top -> the SAME live instance
// morphs: it KEEPS n=3 and GAINS the new field. This is "more live than MACVM":
// a structural class change stays live (InstanceMorpher/Become), no restart.
import 'dart:win';
import 'dart:io';
import 'dart:async';
import 'counter_scratch.dart';

dynamic gc;   // a top-level: survives the reload, holding the live instance

main(List<String> args) {
  // ── v1: make an instance and run it ─────────────────────────────────────────
  gc = new Counter();
  gc.bump();
  gc.bump();
  gc.bump();
  print('MORPH: v1  gc.n = ${gc.n}   (expect 3)');

  // ── Accept: edit the class (add `int step`) by rewriting the scratch source ──
  var scratch = new File('e:/windart/test/counter_scratch.dart');
  scratch.writeAsStringSync(
      'class Counter {\n'
      '  int n = 0;\n'
      '  int step = 7;\n'                               // <-- NEW FIELD
      '  Counter bump() { n = n + step; return this; }\n'  // <-- new logic too
      '}\n');
  print('MORPH: Accept -> counter_scratch.dart rewritten (added `int step = 7`)');

  // Ask the HOST to hot-reload the isolate at the pump top (no Dart frames live —
  // an isolate cannot reload the frames it stands on). windart_request_ui_reload
  // -> PerformUiReload -> Dart_WorkspaceReloadSources(true) -> InstanceMorpher.
  wsRequestUiReload();

  new Timer(new Duration(milliseconds: 500), () {
    print('MORPH: reload status = "${wsUiReloadStatus()}"');
    // The SAME instance, morphed. Read its fields against the reloaded (v2) library.
    print('MORPH: after reload  gc.n    = ${wsEval("gc.n")}    (expect 3 — KEPT across the shape change)');
    print('MORPH: after reload  gc.step = ${wsEval("gc.step")}    (NEW field, morphed into the live instance)');
    // The new method logic is live too: bump now adds `step`.
    wsEval('gc.bump()');
    print('MORPH: after one more v2 bump  gc.n = ${wsEval("gc.n")}   (3 + step)');
    hostQuit();
  });
}
