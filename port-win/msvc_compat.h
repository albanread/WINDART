// WINDART MSVC compatibility shim (Sprint S1).
//
// Force-included (cl /FI) ahead of every translation unit in the VM-core build,
// before any Dart or system header. Its job is to smooth over deltas between
// the MSVC 2015/2017 that Dartium/dart.exe originally compiled under and the
// MSVC 19.50 (VS 2026) we build with now — WITHOUT editing the pristine source
// quarry.
//
// Keep this MINIMAL and evidence-driven: add a shim only when a concrete
// compile error demands it, and comment why. An empty shim is the goal.
#ifndef WINDART_MSVC_COMPAT_H_
#define WINDART_MSVC_COMPAT_H_

#if defined(_MSC_VER)

// (intentionally empty for the first build — populated from the error burn-down)

#endif  // _MSC_VER

#endif  // WINDART_MSVC_COMPAT_H_
