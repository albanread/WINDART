// WINDART W1 — snapshot-as-blob. The core boot snapshot lives IN the SQLite image
// (the `blobs` table), so the whole "world" is one file. Two host-layer entry
// points, no VM change:
//
//   windart_read_snapshot_blob()      — boot path. Called from win_host.cpp
//     (windart_load_snapshots_from_disk) BEFORE Dart_Initialize; returns a blob
//     from the image or NULL (-> caller falls back to the on-disk .bin, then the
//     baked-in array). The fallback chain is: DB blob -> .bin -> baked.
//   windart_bake_snapshots_to_image() — bake path. Reads the two .bin next to the
//     exe and writes them into the image as 'snapshot:vm'/'snapshot:isolate'.
//     Driven from Dart via the Workspace_bakeSnapshot native ("Bake snapshot ->
//     image"), the seed of the future in-app Recreate-Snapshot.
//
// The image is %USERPROFILE%\.windart\workspace.sqlite — the same file the userlib:
// handler and workspace.dart use. See port-win/WORLD_DB_DESIGN.md (W1).
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>
#include <sqlite3.h>

// %USERPROFILE%\.windart\workspace.sqlite — matches workspace.dart imgPath and the
// userlib: tag handler.
static bool WindartImagePath(char* out, size_t n) {
  const char* home = getenv("USERPROFILE");
  if (home == NULL) return false;
  snprintf(out, n, "%s\\.windart\\workspace.sqlite", home);
  return true;
}

// Read one snapshot blob (key 'snapshot:vm' / 'snapshot:isolate') from the image
// into a malloc'd, process-lifetime buffer — the VM does not copy snapshot data, so
// the buffer must outlive the isolate (intentionally leaked, like the .bin path).
// Returns NULL when the image / `blobs` table / row is absent, so the caller falls
// back cleanly. Read-only; never creates the DB.
extern "C" unsigned char* windart_read_snapshot_blob(const char* key,
                                                     unsigned int* out_size) {
  *out_size = 0;
  char path[1024];
  if (!WindartImagePath(path, sizeof(path))) return NULL;
  sqlite3* db = NULL;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
    if (db != NULL) sqlite3_close(db);
    return NULL;
  }
  unsigned char* result = NULL;
  sqlite3_stmt* st = NULL;
  if (sqlite3_prepare_v2(db, "SELECT data FROM blobs WHERE key = ?", -1, &st,
                         NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
      const void* data = sqlite3_column_blob(st, 0);
      int n = sqlite3_column_bytes(st, 0);
      if (data != NULL && n > 0) {
        result = static_cast<unsigned char*>(malloc(static_cast<size_t>(n)));
        if (result != NULL) {
          memcpy(result, data, static_cast<size_t>(n));
          *out_size = static_cast<unsigned int>(n);
        }
      }
    }
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  return result;
}

// ── bake ─────────────────────────────────────────────────────────────────────
static unsigned char* WindartReadFile(const wchar_t* path, DWORD* out_size) {
  *out_size = 0;
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 0x7fffffffLL) {
    CloseHandle(h);
    return NULL;
  }
  DWORD n = static_cast<DWORD>(sz.QuadPart);
  unsigned char* buf = static_cast<unsigned char*>(malloc(n));
  if (buf == NULL) { CloseHandle(h); return NULL; }
  DWORD off = 0, got = 0;
  BOOL ok = TRUE;
  while (off < n && (ok = ReadFile(h, buf + off, n - off, &got, NULL)) != FALSE &&
         got > 0) {
    off += got;
  }
  CloseHandle(h);
  if (ok == FALSE || off != n) { free(buf); return NULL; }
  *out_size = n;
  return buf;
}

static int WindartWriteBlob(sqlite3* db, const char* key,
                            const unsigned char* data, int n) {
  sqlite3_stmt* st = NULL;
  int rc = sqlite3_prepare_v2(
      db, "INSERT OR REPLACE INTO blobs(key, kind, data) VALUES(?, 'snapshot', ?)",
      -1, &st, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_blob(st, 2, data, n, SQLITE_STATIC);   // valid until step returns
  rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

// Bake the two on-disk .bin (next to the exe) into the image `blobs` table. Writes a
// human-readable status into msg; returns 0 on success, nonzero on failure.
extern "C" int windart_bake_snapshots_to_image(char* msg, int msgSz) {
  wchar_t exe[MAX_PATH];
  DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) {
    snprintf(msg, msgSz, "bake: cannot resolve exe path");
    return 1;
  }
  while (len > 0 && exe[len - 1] != L'\\' && exe[len - 1] != L'/') len--;
  exe[len] = L'\0';
  wchar_t vmPath[MAX_PATH], isoPath[MAX_PATH];
  wcscpy_s(vmPath, MAX_PATH, exe);   wcscat_s(vmPath, MAX_PATH, L"vm_isolate_snapshot.bin");
  wcscpy_s(isoPath, MAX_PATH, exe);  wcscat_s(isoPath, MAX_PATH, L"isolate_snapshot.bin");
  DWORD vmSz = 0, isoSz = 0;
  unsigned char* vmBuf = WindartReadFile(vmPath, &vmSz);
  unsigned char* isoBuf = WindartReadFile(isoPath, &isoSz);
  if (vmBuf == NULL || isoBuf == NULL) {
    if (vmBuf != NULL) free(vmBuf);
    if (isoBuf != NULL) free(isoBuf);
    snprintf(msg, msgSz, "bake: .bin not found next to exe (regenerate the snapshot)");
    return 1;
  }
  char path[1024];
  if (!WindartImagePath(path, sizeof(path))) {
    free(vmBuf); free(isoBuf);
    snprintf(msg, msgSz, "bake: no USERPROFILE");
    return 1;
  }
  sqlite3* db = NULL;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
      SQLITE_OK) {
    if (db != NULL) sqlite3_close(db);
    free(vmBuf); free(isoBuf);
    snprintf(msg, msgSz, "bake: cannot open image for write");
    return 1;
  }
  sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS blobs(key TEXT PRIMARY KEY, kind TEXT, data BLOB)",
               NULL, NULL, NULL);
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  int rc = WindartWriteBlob(db, "snapshot:vm", vmBuf, static_cast<int>(vmSz));
  if (rc == SQLITE_OK)
    rc = WindartWriteBlob(db, "snapshot:isolate", isoBuf, static_cast<int>(isoSz));
  if (rc == SQLITE_OK) sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  else sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
  sqlite3_close(db);
  free(vmBuf); free(isoBuf);
  if (rc != SQLITE_OK) {
    snprintf(msg, msgSz, "bake: write failed (sqlite rc=%d)", rc);
    return 1;
  }
  snprintf(msg, msgSz, "baked snapshot -> image blobs (vm=%lu iso=%lu bytes)",
           static_cast<unsigned long>(vmSz), static_cast<unsigned long>(isoSz));
  return 0;
}
