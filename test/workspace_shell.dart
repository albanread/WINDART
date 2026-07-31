// WINDART S7 slice-1 — the workspace IDE SHELL with a read-only class browser.
// Renders the tab-deck chrome + a class browser (classes | members | source)
// populated from the VM's OWN class table via dart:mirrors (the SQLite image is
// deferred — read-only from the class table is the first-slice proof). Verified
// headlessly by Win_surfaceSnapshot (PrintWindow -> PNG).
//
//   dartui.exe workspace_shell.dart e:/windart/build/workspace_shell.png
import 'dart:win';
import 'dart:mirrors';
import 'dart:async';

main(List<String> args) {
  var out = args.isNotEmpty ? args[0] : 'e:/windart/build/workspace_shell.png';

  // ── read-only class-browser data from the VM class table (via mirrors) ──────
  var classMirrors = <String, ClassMirror>{};
  for (var lib in currentMirrorSystem().libraries.values) {
    lib.declarations.forEach((sym, decl) {
      if (decl is ClassMirror) {
        var name = MirrorSystem.getName(decl.simpleName);
        if (name.isNotEmpty && !name.startsWith('_')) classMirrors[name] = decl;
      }
    });
  }
  var classes = classMirrors.keys.toList()..sort();

  var members = <String>[];
  void loadMembers(String cls) {
    members = <String>[];
    var cm = classMirrors[cls];
    if (cm == null) return;
    cm.declarations.forEach((sym, d) {
      var nm = MirrorSystem.getName(sym);
      if (nm.isEmpty || nm.startsWith('_')) return;
      var pfx = 'var ';
      if (d is MethodMirror) {
        if (d.isConstructor) { pfx = 'new '; }
        else if (d.isGetter) { pfx = 'get '; }
        else if (d.isSetter) { pfx = 'set '; }
        else { pfx = 'fn  '; }
      }
      members.add(pfx + nm);
    });
    members.sort();
  }
  // Show the class with the most public members first, so the members pane is
  // visibly populated (mirrors.declarations lists only directly-declared members).
  var shown = classes.isNotEmpty ? classes[0] : '-';
  var best = -1;
  classMirrors.forEach((name, cm) {
    var c = 0;
    cm.declarations.forEach((s, d) {
      var nm = MirrorSystem.getName(s);
      if (nm.isNotEmpty && !nm.startsWith('_')) c++;
    });
    if (c > best) { best = c; shown = name; }
  });
  loadMembers(shown);

  // ── the shell chrome (materialized in the workspace window) ────────────────
  var ui = new Ui.pane(1084, 740);
  ui.title('WINDART Workspace  -  read-only class browser');
  var tabNames = ['Workspace', 'Browser', 'Editor', 'Find', 'Docs', 'App',
                  'Debug', 'VM', 'Help'];
  ui.tabs('tabs', items: tabNames, frame: <int>[0, 0, 1084, 26],
      onSelect: (i) => print('WS: tab ${tabNames[i]}'));

  ui.label('cl', text: 'Classes (${classes.length})',
      frame: <int>[12, 36, 260, 18]);
  ui.list('classes', frame: <int>[12, 56, 260, 648],
      rowCount: () => classes.length,
      cellAt: (r) => classes[r],
      onSelect: (r) {
        loadMembers(classes[r]);
        ui.set('ml', {'text': 'Members of ${classes[r]} (${members.length})'});
        ui.set('members', {'rows': members.length});
        ui.commit();
        print('WS: class ${classes[r]} -> ${members.length} members');
      });

  ui.label('ml', text: 'Members of $shown (${members.length})',
      frame: <int>[284, 36, 340, 18]);
  ui.list('members', frame: <int>[284, 56, 340, 648],
      rowCount: () => members.length,
      cellAt: (r) => members[r],
      onSelect: (r) => print('WS: member ${members[r]}'));

  ui.box('src', title: 'Source (read-only)', frame: <int>[636, 32, 436, 672]);
  ui.label('status',
      text: 'WINDART workspace shell  -  ${classes.length} classes from the VM '
          'class table (read-only; SQLite image deferred)',
      frame: <int>[12, 712, 1060, 18]);
  ui.commit();
  uiReady();

  // Give the controls a moment to paint, then snapshot the window + quit.
  new Timer(new Duration(milliseconds: 700), () {
    var e = ui.snapshot(out);
    print('WS: SNAP -> $out ${e.isEmpty ? "OK (${classes.length} classes, "
        "${members.length} members of $shown)" : "ERR:$e"}');
    hostQuit();
  });
}
