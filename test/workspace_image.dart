// WINDART S7.3 — the workspace SQLite image (persistence proof).
// User class SOURCE lives in a SQLite image at %USERPROFILE%\.windart\workspace.sqlite
// (the source of truth, loaded over the VM snapshot at boot, written by Accept).
//   dartui.exe workspace_image.dart define  <png>   # define Foo/Bar, write to image
//   dartui.exe workspace_image.dart browse  <png>   # (a "restart") load from image
import 'dart:win';
import 'dart:io';
import 'dart:async';

main(List<String> args) {
  var mode = args.isNotEmpty ? args[0] : 'browse';
  var png = args.length > 1 ? args[1] : 'e:/windart/build/workspace_userclass.png';

  var home = Platform.environment['USERPROFILE'];
  var dir = new Directory(home + '\\.windart');
  if (!dir.existsSync()) dir.createSync(recursive: true);
  var imgPath = (dir.path + '\\workspace.sqlite').replaceAll('\\', '/');

  var db = new Db.open(imgPath);
  print('IMAGE: open $imgPath -> isOpen=${db.isOpen}');
  db.exec('CREATE TABLE IF NOT EXISTS classes(name TEXT PRIMARY KEY, source TEXT)');

  if (mode == 'define') {
    db.exec('INSERT OR REPLACE INTO classes(name, source) VALUES(?, ?)',
        ['Foo', 'class Foo {\n  int answer = 42;\n  String greet() => "hello from the image";\n}']);
    db.exec('INSERT OR REPLACE INTO classes(name, source) VALUES(?, ?)',
        ['Bar', 'class Bar {\n  final List items = [];\n  void add(x) { items.add(x); }\n}']);
    db.exec('INSERT OR REPLACE INTO classes(name, source) VALUES(?, ?)',
        ['Widget', 'class Widget {\n  num x = 0.0, y = 0.0;\n  bool visible = true;\n}']);
    print('IMAGE: Accept -> wrote Foo, Bar, Widget to the image');
  }

  // Load the user classes from the image (source of truth).
  var rows = db.query('SELECT name, source FROM classes ORDER BY name');
  var names = rows.map((r) => r[0]).toList();
  print('IMAGE: user classes loaded from image: ${names.join(", ")}');

  var ui = new Ui.pane(1084, 740);
  ui.title('WINDART Workspace  -  the SQLite image (mode=$mode)');
  var tabNames = ['Workspace', 'Browser', 'Editor', 'Find', 'Docs', 'App',
                  'Debug', 'VM', 'Help'];
  ui.tabs('tabs', items: tabNames, frame: <int>[0, 0, 1084, 26]);
  ui.label('ul',
      text: 'User Classes  (persisted in %USERPROFILE%\\.windart\\workspace.sqlite  '
          '-  ${names.length})',
      frame: <int>[12, 34, 980, 18]);
  ui.list('userclasses', frame: <int>[12, 56, 300, 420],
      rowCount: () => names.length,
      cellAt: (r) => names[r],
      onSelect: (r) => print('user class ${names[r]}'));
  ui.label('sl', text: 'Source (from the image)', frame: <int>[324, 34, 400, 18]);
  ui.editor('src', frame: <int>[324, 56, 748, 420]);
  ui.set('src', {
    'text': (rows.isNotEmpty ? rows[0][1] : '(no user classes yet)')
        .replaceAll('\n', '\r')
  });
  ui.label('status',
      text: 'mode=$mode  -  the image is loaded over the VM snapshot at boot and '
          'survives restart (Accept writes source here).',
      frame: <int>[12, 490, 1060, 18]);
  ui.commit();
  uiReady();

  new Timer(new Duration(milliseconds: 500), () {
    var e = ui.snapshot(png);
    print('IMAGE: SNAP -> $png ${e.isEmpty ? "OK (${names.length} user classes)" : "ERR:$e"}');
    db.close();
    hostQuit();
  });
}
