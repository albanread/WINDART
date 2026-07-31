// WINDART S3 — stdin validation. Reads piped lines, echoes them. V1.
import 'dart:io';

main() {
  int n = 0;
  String line;
  while ((line = stdin.readLineSync()) != null) {
    n++;
    print('read[$n]: $line');
  }
  print('STDIN_TEST_OK ($n lines)');
}
