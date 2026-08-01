class Counter {
  int n = 0;
  Counter bump() { n = n + 1; return this; }
}
