import 'dart:mirrors';
main() {
  var n = 0, sample = <String>[];
  for (var lib in currentMirrorSystem().libraries.values) {
    lib.declarations.forEach((sym, decl) {
      if (decl is ClassMirror) {
        var name = MirrorSystem.getName(decl.simpleName);
        if (name.isNotEmpty && !name.startsWith('_')) { n++; if (sample.length < 8) sample.add(name); }
      }
    });
  }
  print('MIRROR_TEST: $n classes; sample: ${sample.join(", ")}');
}
