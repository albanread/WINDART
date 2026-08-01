// WINDART Stage 1 — the `userlib:` library tag handler. Serves the user library
// SYNCHRONOUSLY from the SQLite workspace image (the DB is the source of truth),
// so `import 'userlib:user'` in the workspace loads the user classes with NO
// scratch file. Installed in front of the stock Loader::LibraryTagHandler
// (bin/main.cc, under DART_UI_HOST); every non-userlib: URL is delegated to it
// unchanged. Reload re-invokes the handler, so Accept — which rewrites the
// `userlib` rows — morphs live instances. See port-win/STAGE1_DESIGN.md.
//
// This is Directive B: the source<->DB management lowered into the C++/VM layer.
// The scanner BOM fix (runtime/vm/scanner.cc) means DB source loads whether or
// not any row carries a byte-order mark.
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sqlite3.h>

#include "include/dart_api.h"
#include "bin/loader.h"

namespace dart {
namespace bin {

// The factory-default user library — used when the image has no `userlib` rows
// yet (self-bootstrapping: the very first load runs before Dart seeds the table,
// so the handler must still return a valid library for `import 'userlib:user'`).
static const char* kUserlibDefault =
    "class Counter {\n"
    "  int n = 0;\n"
    "  Counter bump() { n = n + 1; return this; }\n"
    "}\n";

// Concatenate every `userlib` row's source into one library string. Returns a
// malloc'd C string (caller frees). Falls back to kUserlibDefault when the table
// is empty/absent. Path: %USERPROFILE%\.windart\workspace.sqlite — matches
// workspace.dart's imgPath. Read-only; never creates the DB (Dart owns writes).
static char* WindartUserlibSource() {
  std::string out =
      "// GENERATED from the SQLite image by the WINDART userlib: tag handler.\n"
      "// The live user classes — edit via the Editor + Accept (rewrites the image).\n";
  bool got = false;
  const char* home = getenv("USERPROFILE");
  if (home != NULL) {
    char path[1024];
    snprintf(path, sizeof(path), "%s\\.windart\\workspace.sqlite", home);
    sqlite3* db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
      sqlite3_stmt* st = NULL;
      if (sqlite3_prepare_v2(db, "SELECT source FROM userlib ORDER BY name", -1,
                             &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
          const unsigned char* s = sqlite3_column_text(st, 0);
          if (s != NULL && s[0] != '\0') {
            out += reinterpret_cast<const char*>(s);
            out += "\n\n";
            got = true;
          }
        }
        sqlite3_finalize(st);
      }
      sqlite3_close(db);
    }
  }
  if (!got) out += kUserlibDefault;
  char* result = static_cast<char*>(malloc(out.size() + 1));
  if (result != NULL) memcpy(result, out.c_str(), out.size() + 1);
  return result;
}

// The isolate's library tag handler: intercept `userlib:`, delegate all else to
// the stock loader (dart:/package:/file: unchanged). Declared (block-scope) and
// installed in bin/main.cc under DART_UI_HOST.
Dart_Handle WindartUserTagHandler(Dart_LibraryTag tag,
                                  Dart_Handle library,
                                  Dart_Handle url) {
  const char* url_string = NULL;
  if (Dart_IsString(url) &&
      !Dart_IsError(Dart_StringToCString(url, &url_string)) &&
      url_string != NULL && strncmp(url_string, "userlib:", 8) == 0) {
    if (tag == Dart_kCanonicalizeUrl) {
      return url;                       // userlib: URIs are already canonical
    }
    if (tag == Dart_kImportTag) {
      char* src = WindartUserlibSource();
      Dart_Handle source = Dart_NewStringFromCString(src != NULL ? src : "");
      if (src != NULL) free(src);
      return Dart_LoadLibrary(url, url, source, 0, 0);
    }
    return Dart_NewApiError("userlib: supports 'import' only");
  }
  return Loader::LibraryTagHandler(tag, library, url);
}

}  // namespace bin
}  // namespace dart
