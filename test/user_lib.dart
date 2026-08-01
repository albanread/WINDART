// WINDART user library (Stage 1) — the live, editable user classes.
// GENERATED from the SQLite image by the Editor's Accept, which rewrites this
// file and hot-reloads; live instances morph in place (state kept, new fields
// added). Ships with a default Counter — edit it in the Editor tab and Accept
// to watch a live instance morph. (Order-B: the C++ DB tag handler replaces
// this file later; see port-win/STAGE1_DESIGN.md.)
class Counter {
  int n = 0;
  Counter bump() { n = n + 1; return this; }
}
