// WINDART S2 milestone probe — a trivial Dart 1.x (V1) program.
// Exercises: isolate startup, core-lib bootstrap, the JIT compiling main(),
// and the print() embedder path to stdout.
main() {
  print('hello, windart');
  int sum = 0;
  for (int i = 1; i <= 100; i++) {
    sum += i;
  }
  print('sum 1..100 = $sum');
}
