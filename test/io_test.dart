// WINDART S3 — dart:io validation (V1 / Dart 1.24.3).
// Exercises Platform, synchronous File write/read/length/delete, Directory
// listing, and stdout/stderr. Run: dart.exe io_test.dart
import 'dart:io';

main() {
  // --- Platform ---
  print('platform.operatingSystem = ${Platform.operatingSystem}');
  print('platform.numberOfProcessors = ${Platform.numberOfProcessors}');
  print('platform.pathSeparator = "${Platform.pathSeparator}"');
  print('platform.executable nonEmpty = ${Platform.executable.isNotEmpty}');

  // --- File write / read / length ---
  var path = 'e:/windart/test/_io_probe.txt';
  var f = new File(path);
  f.writeAsStringSync('windart io works\nsecond line\n');
  var content = f.readAsStringSync();
  print('file readback = "${content.trim().replaceAll("\n", " | ")}"');
  print('file lengthSync = ${f.lengthSync()}');
  print('file existsSync = ${f.existsSync()}');

  // --- Directory listing ---
  var dir = new Directory('e:/windart/port-win');
  var names = dir
      .listSync()
      .map((e) => e.path.split(new RegExp(r'[\\/]')).last)
      .toList();
  names.sort();
  print('port-win entries = ${names.length}; first5 = ${names.take(5).join(", ")}');

  // --- stdout / stderr ---
  stdout.writeln('stdout.writeln works');
  stderr.writeln('stderr.writeln works (goes to stderr)');

  // --- delete the probe file this test created ---
  f.deleteSync();
  print('file deleted, existsSync = ${f.existsSync()}');

  print('IO_TEST_OK');
}
