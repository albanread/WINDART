// WINDART scaffolding — not from upstream.
//
// dart:io's zlib/gzip Filter natives live in bin/filter.cc, which #includes
// "zlib/zlib.h". We have not vendored zlib yet (deferred to S3; see the zlib
// stub in runtime/third_party/zlib/zlib.h), so filter.cc is excluded from the
// dart_io target. But bin/io_natives.cc still lists the four Filter_* natives in
// its resolver table, so their symbols must exist at link time.
//
// This file provides throwing stubs for exactly those four, mirroring upstream
// bin/filter_unsupported.cc (whose bodies these are) and the SSL-off posture of
// bin/secure_socket_unsupported.cc. Upstream's filter_unsupported.cc is gated by
// `#if defined(DART_IO_DISABLED)` — which would disable ALL of dart:io — so we
// carry the bodies here unguarded instead.
//
// To lift: vendor zlib into runtime/third_party/zlib, re-add bin/filter.cc to
// the dart_io source list, and delete this file.
#include "bin/builtin.h"
#include "bin/dartutils.h"
#include "include/dart_api.h"

namespace dart {
namespace bin {

void FUNCTION_NAME(Filter_CreateZLibInflate)(Dart_NativeArguments args) {
  Dart_ThrowException(DartUtils::NewInternalError(
      "ZLibInflater and Deflater not supported in this WINDART build "
      "(zlib deferred to S3)"));
}


void FUNCTION_NAME(Filter_CreateZLibDeflate)(Dart_NativeArguments args) {
  Dart_ThrowException(DartUtils::NewInternalError(
      "ZLibInflater and Deflater not supported in this WINDART build "
      "(zlib deferred to S3)"));
}


void FUNCTION_NAME(Filter_Process)(Dart_NativeArguments args) {
  Dart_ThrowException(DartUtils::NewInternalError(
      "ZLibInflater and Deflater not supported in this WINDART build "
      "(zlib deferred to S3)"));
}


void FUNCTION_NAME(Filter_Processed)(Dart_NativeArguments args) {
  Dart_ThrowException(DartUtils::NewInternalError(
      "ZLibInflater and Deflater not supported in this WINDART build "
      "(zlib deferred to S3)"));
}

}  // namespace bin
}  // namespace dart
