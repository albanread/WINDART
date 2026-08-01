// WINDART W2 — export-world / import-world: the git bridge. Project the SQLite
// image to/from loose, diffable files, so the RELEASE is one tidy file while git
// still TRACKS the apps. Host layer (sqlite3 + Win32 file I/O), driven from Dart
// (Workspace_exportWorld / Workspace_importWorld). See WORLD_DB_DESIGN.md (W2).
//
// Why C++ and not Dart: the workspace sqlite binding is text-only (column_text /
// bind_text), which truncates a binary snapshot blob at its first NUL. Blobs must
// round-trip byte-exact, so the whole projection lives here.
//
// Layout under <dir>:
//   src/userlib/<name>.dart   userlib(name, source)  — the live user classes
//   src/classes/<name>.dart   classes(name, source)  — VM-class source sketches
//   blobs/<safeKey>.bin       blobs(key, kind, data) — snapshot (+ future assets)
//   blobs/index.txt           "key|kind|file" per blob (exact key/kind fidelity)
//   world.manifest.txt        schema version + row counts (informational)
//
// ASCII paths (narrow Win32) — the build/USERPROFILE dirs are ASCII; a UTF-8/wide
// variant is a later refinement. Convention: a blob key is "<kind>:<name>".
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <sqlite3.h>

static const int kWorldSchema = 1;

static bool WindartImagePath(char* out, size_t n) {
  const char* home = getenv("USERPROFILE");
  if (home == NULL) return false;
  snprintf(out, n, "%s\\.windart\\workspace.sqlite", home);
  return true;
}

// Create a directory and every missing parent (narrow ASCII paths).
static void WindartMakeDirs(const char* path) {
  char buf[1024];
  size_t len = strlen(path);
  if (len >= sizeof(buf)) return;
  memcpy(buf, path, len + 1);
  for (size_t i = 0; i < len; i++) {
    if (buf[i] == '/' || buf[i] == '\\') {
      char c = buf[i];
      buf[i] = '\0';
      if (i > 0 && buf[i - 1] != ':') CreateDirectoryA(buf, NULL);
      buf[i] = c;
    }
  }
  CreateDirectoryA(buf, NULL);
}

static bool WindartWriteBytes(const char* path, const void* data, DWORD n) {
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  bool ok = (n == 0) || (WriteFile(h, data, n, &wrote, NULL) && wrote == n);
  CloseHandle(h);
  return ok;
}

static unsigned char* WindartReadBytes(const char* path, DWORD* out_n) {
  *out_n = 0;
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 0x7fffffffLL) {
    CloseHandle(h);
    return NULL;
  }
  DWORD n = static_cast<DWORD>(sz.QuadPart);
  unsigned char* buf = static_cast<unsigned char*>(malloc(n ? n : 1));
  if (buf == NULL) { CloseHandle(h); return NULL; }
  DWORD off = 0, got = 0;
  BOOL ok = TRUE;
  while (off < n && (ok = ReadFile(h, buf + off, n - off, &got, NULL)) != FALSE &&
         got > 0) {
    off += got;
  }
  CloseHandle(h);
  if (ok == FALSE || off != n) { free(buf); return NULL; }
  *out_n = n;
  return buf;
}

static void WindartListFiles(const char* dir, const char* pattern,
                             std::vector<std::string>* names) {
  char glob[1024];
  snprintf(glob, sizeof(glob), "%s\\%s", dir, pattern);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(glob, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      names->push_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

// ── export ─────────────────────────────────────────────────────────────────────
static int WindartExportTextTable(sqlite3* db, const char* sql, const char* outDir,
                                  const char* sub) {
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s\\src\\%s", outDir, sub);
  WindartMakeDirs(dir);
  int count = 0;
  sqlite3_stmt* st = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char* name = sqlite3_column_text(st, 0);
      const unsigned char* src = sqlite3_column_text(st, 1);
      int srcLen = sqlite3_column_bytes(st, 1);
      if (name == NULL) continue;
      char path[1024];
      snprintf(path, sizeof(path), "%s\\%s.dart", dir,
               reinterpret_cast<const char*>(name));
      WindartWriteBytes(path, src != NULL ? reinterpret_cast<const char*>(src) : "",
                        static_cast<DWORD>(srcLen > 0 ? srcLen : 0));
      count++;
    }
    sqlite3_finalize(st);
  }
  return count;
}

extern "C" int windart_export_world(const char* outDir, char* msg, int msgSz) {
  char dbPath[1024];
  if (!WindartImagePath(dbPath, sizeof(dbPath))) {
    snprintf(msg, msgSz, "export: no USERPROFILE");
    return 1;
  }
  sqlite3* db = NULL;
  if (sqlite3_open_v2(dbPath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
    if (db != NULL) sqlite3_close(db);
    snprintf(msg, msgSz, "export: cannot open image");
    return 1;
  }
  WindartMakeDirs(outDir);
  int nUser = WindartExportTextTable(
      db, "SELECT name, source FROM userlib ORDER BY name", outDir, "userlib");
  int nClass = WindartExportTextTable(
      db, "SELECT name, source FROM classes ORDER BY name", outDir, "classes");

  char blobDir[1024];
  snprintf(blobDir, sizeof(blobDir), "%s\\blobs", outDir);
  WindartMakeDirs(blobDir);
  std::string index;
  int nBlob = 0;
  sqlite3_stmt* st = NULL;
  if (sqlite3_prepare_v2(db, "SELECT key, kind, data FROM blobs ORDER BY key", -1,
                         &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char* key = sqlite3_column_text(st, 0);
      const unsigned char* kind = sqlite3_column_text(st, 1);
      const void* data = sqlite3_column_blob(st, 2);
      int n = sqlite3_column_bytes(st, 2);
      if (key == NULL) continue;
      std::string fname(reinterpret_cast<const char*>(key));
      for (size_t i = 0; i < fname.size(); i++)
        if (fname[i] == ':' || fname[i] == '/' || fname[i] == '\\') fname[i] = '_';
      fname += ".bin";
      char path[1024];
      snprintf(path, sizeof(path), "%s\\%s", blobDir, fname.c_str());
      WindartWriteBytes(path, data != NULL ? data : "",
                        static_cast<DWORD>(n > 0 ? n : 0));
      index += reinterpret_cast<const char*>(key);
      index += "|";
      index += (kind != NULL ? reinterpret_cast<const char*>(kind) : "");
      index += "|";
      index += fname;
      index += "\n";
      nBlob++;
    }
    sqlite3_finalize(st);
  }
  char idxPath[1024];
  snprintf(idxPath, sizeof(idxPath), "%s\\index.txt", blobDir);
  WindartWriteBytes(idxPath, index.c_str(), static_cast<DWORD>(index.size()));
  sqlite3_close(db);

  char manifest[512];
  snprintf(manifest, sizeof(manifest),
           "windart-world schema=%d\nuserlib=%d\nclasses=%d\nblobs=%d\n",
           kWorldSchema, nUser, nClass, nBlob);
  char mPath[1024];
  snprintf(mPath, sizeof(mPath), "%s\\world.manifest.txt", outDir);
  WindartWriteBytes(mPath, manifest, static_cast<DWORD>(strlen(manifest)));

  snprintf(msg, msgSz, "exported: userlib=%d classes=%d blobs=%d -> %s", nUser,
           nClass, nBlob, outDir);
  return 0;
}

// ── import ───────────────────────────────────────────────────────────────────────
static int WindartImportTextTable(sqlite3* db, const char* inDir, const char* sub,
                                  const char* table) {
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s\\src\\%s", inDir, sub);
  std::vector<std::string> files;
  WindartListFiles(dir, "*.dart", &files);
  char create[256];
  snprintf(create, sizeof(create),
           "CREATE TABLE IF NOT EXISTS %s(name TEXT PRIMARY KEY, source TEXT)",
           table);
  sqlite3_exec(db, create, NULL, NULL, NULL);
  char insert[256];
  snprintf(insert, sizeof(insert),
           "INSERT OR REPLACE INTO %s(name, source) VALUES(?, ?)", table);
  int count = 0;
  for (size_t i = 0; i < files.size(); i++) {
    std::string name = files[i];
    if (name.size() > 5 && name.substr(name.size() - 5) == ".dart")
      name = name.substr(0, name.size() - 5);
    char path[1024];
    snprintf(path, sizeof(path), "%s\\%s", dir, files[i].c_str());
    DWORD n = 0;
    unsigned char* data = WindartReadBytes(path, &n);
    if (data == NULL && n != 0) continue;
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, insert, -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, data != NULL ? reinterpret_cast<const char*>(data) : "",
                        static_cast<int>(n), SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_DONE) count++;
      sqlite3_finalize(st);
    }
    if (data != NULL) free(data);
  }
  return count;
}

extern "C" int windart_import_world(const char* inDir, const char* outDbPath,
                                    char* msg, int msgSz) {
  DeleteFileA(outDbPath);   // fresh build (do NOT point this at the live image)
  sqlite3* db = NULL;
  if (sqlite3_open_v2(outDbPath, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      NULL) != SQLITE_OK) {
    if (db != NULL) sqlite3_close(db);
    snprintf(msg, msgSz, "import: cannot create target DB");
    return 1;
  }
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  int nUser = WindartImportTextTable(db, inDir, "userlib", "userlib");
  int nClass = WindartImportTextTable(db, inDir, "classes", "classes");

  sqlite3_exec(
      db, "CREATE TABLE IF NOT EXISTS blobs(key TEXT PRIMARY KEY, kind TEXT, data BLOB)",
      NULL, NULL, NULL);
  int nBlob = 0;
  char idxPath[1024];
  snprintf(idxPath, sizeof(idxPath), "%s\\blobs\\index.txt", inDir);
  DWORD idxN = 0;
  unsigned char* idx = WindartReadBytes(idxPath, &idxN);
  if (idx != NULL) {
    std::string s(reinterpret_cast<const char*>(idx), idxN);
    free(idx);
    size_t pos = 0;
    while (pos < s.size()) {
      size_t nl = s.find('\n', pos);
      std::string line = s.substr(pos, (nl == std::string::npos ? s.size() : nl) - pos);
      pos = (nl == std::string::npos) ? s.size() : nl + 1;
      // strip a trailing CR (in case the index was written/edited with CRLF)
      if (!line.empty() && line[line.size() - 1] == '\r') line.resize(line.size() - 1);
      if (line.empty()) continue;
      size_t p1 = line.find('|');
      size_t p2 = (p1 == std::string::npos) ? std::string::npos : line.find('|', p1 + 1);
      if (p1 == std::string::npos || p2 == std::string::npos) continue;
      std::string key = line.substr(0, p1);
      std::string kind = line.substr(p1 + 1, p2 - p1 - 1);
      std::string file = line.substr(p2 + 1);
      char path[1024];
      snprintf(path, sizeof(path), "%s\\blobs\\%s", inDir, file.c_str());
      DWORD bn = 0;
      unsigned char* bd = WindartReadBytes(path, &bn);
      if (bd == NULL && bn != 0) continue;
      sqlite3_stmt* st = NULL;
      if (sqlite3_prepare_v2(
              db, "INSERT OR REPLACE INTO blobs(key, kind, data) VALUES(?, ?, ?)", -1,
              &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 3, bd != NULL ? bd : reinterpret_cast<const unsigned char*>(""),
                          static_cast<int>(bn), SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_DONE) nBlob++;
        sqlite3_finalize(st);
      }
      if (bd != NULL) free(bd);
    }
  }
  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  sqlite3_close(db);
  snprintf(msg, msgSz, "imported: userlib=%d classes=%d blobs=%d -> %s", nUser,
           nClass, nBlob, outDbPath);
  return 0;
}
