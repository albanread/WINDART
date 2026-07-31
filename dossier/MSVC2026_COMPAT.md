# MSVC 2026 Compatibility Dossier — Dart 1.24.3 VM (Windows x64 JIT)

**Target:** compile `e:\dart_origins\sdk-1.24.3\runtime` under MSVC 19.50 (Visual Studio 2026), C++14/17, Windows x64, JIT (`DART_PRECOMPILED_RUNTIME` **undefined**).
**Author:** compatibility analyst (read-only recon). **Consumer:** build engineer porting the CMake build.
**Method:** static reading of the sources that a Win-x64 JIT build actually compiles (`vm/*.cc|*.h` minus `*_test/_arm*/_ia32*/_mips*/_dbc*/simulator`, keeping `*_x64*`/`*_win*`/portable; `platform/*`; `bin/*_win.cc` + portable bin; `include/*`), plus the original GYP/GN Windows build config.

**Bottom line up front:** Dart 1.24.3 already carried a *substantial* MSVC compatibility layer (it targeted VS2013–VS2015). The three things that will actually bite MSVC 2026 are (1) two hand-rolled CRT macros in `platform/globals.h` that were correct for VS2013 but are now **wrong and fragile** (`snprintf`→`_snprintf`, `strtoll`→`_strtoi64`), (2) deprecation errors from `strncpy`/`sscanf`/`fopen`/`getenv` unless `_CRT_SECURE_NO_WARNINGS` is set, and (3) the giant translation units needing `/bigobj`, which the original build never enabled. Everything else is mostly already handled — see §5.

---

## 1. Preprocessor defines the build needs

Dart **already defines** the windows.h-taming macros inside `platform/globals.h:14-54` *before* it includes `<windows.h>`:
`WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `NOKERNEL`, `NOUSER`, `NOSERVICE`, `NOSOUND`, `NOMCX`, `UNICODE`/`_UNICODE`. That protection only holds for translation units that reach `<windows.h>` *through* `platform/globals.h`. **Set the critical ones on the command line too**, so any TU that pulls `<windows.h>` via a system/third-party header first is still safe.

| Define | Required? | Evidence / why |
|---|---|---|
| `NOMINMAX` | **YES (command-line)** | Not for `min(a,b)` macro calls — there are none — but because the VM has *member functions and locals named `min`/`max`* that a leaked windows.h `min`/`max` macro would destroy: e.g. `range->min()`, `range->max()`, `RangeBoundary min = range->min();` (`vm/flow_graph_range_analysis.cc:122-124, 511, 540`). Already in `globals.h:20-22` and in the original build (`build/config/win/BUILD.gn:175-177`, `build/config/BUILDCONFIG.gn:231`). Keep it global. |
| `WIN32_LEAN_AND_MEAN` | **YES (command-line)** | `globals.h:16-18` + `build/config/win/BUILD.gn:165-167`. Cuts winsock1/ole/shell to avoid double-include vs the explicit `<winsock2.h>` at `globals.h:50`. |
| `_CRT_SECURE_NO_WARNINGS` | **YES — add** | The VM core calls C4996-deprecated `strncpy`/`sscanf`/`fopen`/`getenv` in compiled files (see §3). Original build used the older spelling `_CRT_SECURE_NO_DEPRECATE` (`build/config/compiler/BUILD.gn:440`, gyp `_HAS_EXCEPTIONS=0` block). Add **both** `_CRT_SECURE_NO_WARNINGS` and `_CRT_SECURE_NO_DEPRECATE` (they gate slightly different header versions). |
| `_CRT_NONSTDC_NO_DEPRECATE` | **Optional / defensive** | The `bin/*_win.cc` layer already uses the MSVC underscore spellings (`_open`, `_fileno`, `_O_WRONLY` in `bin/file_win.cc:42-54, 303-306`), so bare POSIX names are largely absent from *Windows-compiled* code. Add it only if a portable bin file trips C4996 on a POSIX alias. Low priority. |
| `_WINSOCK_DEPRECATED_NO_WARNINGS` | **Likely YES for bin/** | `bin/socket_win.cc`/`socket_base_win.cc` use classic winsock (`inet_ntoa`/`gethostbyname`-era) that UCRT flags. Not in the core VM. Add if building the `bin` socket layer. Cheap to add pre-emptively. |
| `_USE_MATH_DEFINES` | **NO** | Grep for `M_PI|M_E|M_LN2|...` across `runtime/` = **no matches**. Dart never uses the POSIX math constants. Do not add. |
| `NOGDI` | **NO (optional/defensive)** | Not defined originally and the build worked. Note: without it, `<wingdi.h>` *is* pulled in and its `GetObject`→`GetObjectW` macro renames Dart's own `static bool GetObject(...)` at `vm/service.cc:3836` (and its registration at `:4260`) — but consistently within the TU, so it compiles. `ERROR`/`Rectangle`/`TextOut` bare identifiers: none found. Add `NOGDI` only if you later hit a phantom `GetObjectW` link error; otherwise skip. |
| `_ENABLE_EXTENDED_ALIGNED_STORAGE` | **NO** | No `std::aligned_storage`/`alignas`/`std::align` anywhere. Dart aligns via `__declspec(align(8|16))` (`vm/globals.h:88-89`, `ALIGN8`/`ALIGN16`). Not needed. |
| `UNICODE`/`_UNICODE` | **YES** | `globals.h:44-47` + `build/config/win/BUILD.gn:153-158`. The Win bin layer is wchar-based (`os_win.cc:82-88` `WideCharToMultiByte`). Keep. |
| `_HAS_EXCEPTIONS=0` | **Match original, but watch STL** | Original sets it (`tools/gyp/configurations_msvs.gypi:19`, `build/config/compiler/BUILD.gn:441`) and compiles with exceptions **off** (no `/EHsc`, `ExceptionHandling:0`). See §6 — modern MS-STL headers pulled by `kernel_to_il.cc` (`<set>/<vector>/<map>`) may object; have the `/EHsc` fallback ready. |
| `DART_PRECOMPILED_RUNTIME` / `DART_PRECOMPILER` | **Leave UNDEFINED** | JIT config. Both undefined selects the full JIT VM (`NOT_IN_PRECOMPILED(code)` expands to `code`, `globals.h:147-151`). Defining neither is correct; they are mutually-exclusive guards (`globals.h:139-145`). |

**Windows-version floor (from the original build, keep it):** `_WIN32_WINNT=0x0603`, `WINVER=0x0603`, `NTDDI_VERSION=0x06030000` (Win 8.1) — `build/config/win/BUILD.gn:28-33, 16`. Safe on any modern Windows SDK; raise to `0x0A00` only if you want Win10 APIs.

---

## 2. How Dart detects compiler / OS / arch (and the correct JIT-x64 select)

All detection lives in `platform/globals.h`; `vm/globals.h` just includes it (`vm/globals.h:11`).

- **OS:** `_WIN32` ⇒ `HOST_OS_WINDOWS 1` (`globals.h:109-112`). When no `TARGET_OS_*` is pre-set, `HOST_OS_WINDOWS` ⇒ `TARGET_OS_WINDOWS 1` (`globals.h:392-393`). No action — auto-selects correctly.
- **Arch:** `_M_X64 || __x86_64__` ⇒ `HOST_ARCH_X64 1` + `ARCH_IS_64_BIT 1` (`globals.h:207-209`). With no `TARGET_ARCH_*` pre-set, `HOST_ARCH_X64` ⇒ `TARGET_ARCH_X64 1` (`globals.h:319-320`). MSVC's `_M_X64` fires automatically for an x64 target. **Do not** hand-define `TARGET_ARCH_X64`; let it auto-derive (defining it *and* building x86 by accident trips the host/target mismatch `#error` at `globals.h:332-341`).
- **Compiler:** every compiler split keys off `_MSC_VER` vs `__GNUC__` and `#error`s otherwise (`globals.h:266-309`: `DART_FORCE_INLINE`=`__forceinline`, `DART_NOINLINE`=`__declspec(noinline)`, `DART_NORETURN`=`__declspec(noreturn)`, `DART_PRETTY_FUNCTION`=`__FUNCSIG__`). All valid on MSVC 19.50.
- **Version gates that matter:** `_MSC_VER < 1800` (VS2013) fully gates out `platform/c99_support_win.h:8` (its whole body, including a *second* `strtoll` macro) — **inert on 2026, good**. `_MSC_VER < 1900` is the right guard to *add* to the two live macros in §3.

**Minimal correct define set for Windows x64 JIT:** platform/window defines from §1 + arch/OS auto-derived. You do **not** need to define `TARGET_ARCH_X64`, `TARGET_OS_WINDOWS`, or `DART_PRECOMPILED_RUNTIME=0` (absence = JIT). Do define `DEBUG` xor `PRODUCT` per build flavor (`globals.h:123-137`).

---

## 3. CRT / STL breakage

### 3a. THE headline bug — two obsolete CRT macros in `platform/globals.h` (fix first)

```
globals.h:669  #if defined(HOST_OS_WINDOWS)
globals.h:670  #define snprintf _snprintf
globals.h:671  #define strtok_r strtok_s
globals.h:672  #endif
...
globals.h:426  #ifdef _MSC_VER
globals.h:427  #define strtoll _strtoi64
globals.h:428  #endif
```

These were correct for VS2013 (no conforming `snprintf`, no `strtoll`). Under MSVC 2026 they are **both wrong and dangerous**:

1. **Correctness:** the UCRT (VS2015+) has a standards-conforming `snprintf`. `_snprintf` is the *legacy* function with different semantics — it returns **-1** on truncation and does **not** null-terminate. Several core call sites use the C99 "measure with `(NULL, 0)`" idiom, which `_snprintf` breaks (returns -1 ⇒ `len+1 == 0` allocation): `bin/builtin.cc:66-68`, `bin/dartutils.cc:312-314, 472-474`, `bin/loader.cc:811-813`, `bin/extensions.cc:137`. `snprintf` is also used in the always-compiled assert core `platform/assert.cc:25`. Letting the real `snprintf` through *fixes latent bugs*.
2. **Compile fragility (macro vs STL):** `#define snprintf _snprintf` / `#define strtoll _strtoi64` are object-like text substitutions. If any TU includes `platform/globals.h` and *then* an STL/UCRT C++ header that emits `using std::snprintf;` / `using std::strtoll;` (`<cstdio>`, `<cstdlib>` and everything that transitively pulls them — `<string>`, `<algorithm>`, `<sstream>`), the `using` becomes `using std::_snprintf;` / `using std::_strtoi64;` → **`error C2039: '_snprintf' is not a member of 'std'`**. Google-style "system headers first" ordering avoids it in most existing files (e.g. `vm/kernel_to_il.cc:5` includes `<set>` before its project headers), but ordering is not enforced and the semantic bug above is order-independent.

Both macros are **load-bearing** today (Windows uses them): `strtoll(` appears in compiled VM at `vm/flags.cc:298` and `lib/integers.cc:202`; `snprintf(` throughout `bin/` and `platform/assert.cc:25`.

**FIX (source edit, smallest safe change):** guard each with a version test so modern MSVC uses the real CRT:
```c
#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif
// and
#if defined(_MSC_VER) && _MSC_VER < 1900
#define strtoll _strtoi64
#endif
```
`strtok_r`→`strtok_s` (`globals.h:671`) is **fine** — keep it (semantics match, `strtok_s` is current).
**Cheap test:** compile a 3-line TU that does `#include "platform/globals.h"` then `#include <string>` then `int main(){}`. If it errors on `std::_snprintf`/`std::_strtoi64`, the guard is needed (it is).

### 3b. Deprecated-but-present CRT calls (C4996) — need `_CRT_SECURE_NO_WARNINGS`

Used in **compiled core VM** files, so with `/WX` (which the original build sets, §6) they are hard errors:
- `strncpy`: `vm/zone.cc:240,254,266,270`, `vm/uri.cc:346,379,385`, `vm/object.cc:20083`, `vm/flags.cc:360,368`, `vm/dart_api_impl.cc:399`, `vm/il_printer.cc:60,428`, `vm/profiler_service.cc:334`, `vm/compiler.cc:253`.
- `sscanf`: `lib/vmservice.cc:338`.
- `fopen`: `vm/proccpuinfo.cc:27,42` (Linux-only TU — ignore for Win), `bin/gen_snapshot.cc:1200,1229`.
- `getenv`: `bin/platform_*` (non-Win TUs). The **Windows** platform layer avoids `getenv`.
- `_vsnprintf`/`_vscprintf`: `vm/os_win.cc:323,315,331` — these underscore forms are also C4996-flagged.

**FIX:** define `_CRT_SECURE_NO_WARNINGS` (+ `_CRT_SECURE_NO_DEPRECATE`) globally. No source edits needed. (Alternatively suppress `/wd4996`, but the define is cleaner and is what modern Dart does.)

### 3c. Removed/renamed STL — **all clear**

Verified absent across `runtime/`: `std::unary_function`/`binary_function`, `std::auto_ptr`, `std::bind1st`/`bind2nd`/`ptr_fun`/`mem_fun`, `std::random_shuffle`. Dynamic-exception-specifications (`throw()`/`throw(std::…)`): none. The `register` keyword: only in the ARM-only `vm/signal_handler.h:105` (`register int arg0 asm("r0")`), which is **not compiled** for x64 — no action. So the classic "C++17 removed it" landmines do not apply to this subset.

### 3d. `platform/inttypes_support_win.h` — typedef + PRI-macro redefinition risk

`inttypes_support_win.h:8-15` *typedefs* `int8_t…uint64_t` from `signed __int8` etc., and `:18-25` `#define`s `PRIdPTR`/`PRIxPTR`/`PRId64`… . On modern toolchains `<cstdint>`/`<inttypes.h>` (pulled transitively by `<windows.h>` and the STL) define the same names. Typedef-to-same-type is legal in C++, so the types are fine. The **`PRIxxx` macros can warn C4005 "macro redefinition"** if `<inttypes.h>`/`<cinttypes>` was seen first — which under `/WX` is fatal. The original build **anticipated exactly this**: `build/config/compiler/BUILD.gn:527` disables it — `"/wd4005",  # Redefinition of macros for PRId64 etc.`. **Keep `/wd4005`.**

### 3e. `platform/math.h` function-like math macros

`math.h:14-17` `#define isinf(val) std::isinf(val)` (+ `isnan`/`signbit`/`isfinite`). It includes `<cmath>` first (`:12`) then defines the macros, so `std::isinf` resolves. Self-consistent and standard; low risk on MSVC 2026. Only breaks if some *other* header declares a member/function literally named `isinf` after this — none observed. No action, but if `/permissive-` surfaces a `<cmath>` conflict, this is the suspect.

---

## 4. MSVC-specific pragmas / attributes

- **Compiler intrinsics used (all valid on MSVC 19.50, x64):** `_BitScanReverse64`/`_BitScanForward64`/`_byteswap_*` via `<intrin.h>` (`platform/utils_win.h:8-51`); `_InterlockedCompareExchange64`/`InterlockedIncrement64`/`InterlockedExchangeAdd64` (`vm/atomic_win.h:18-138`); `__debugbreak`, `_ReturnAddress` (`vm/os_win.cc:251,264`); `_set_abort_behavior`, `_strtoi64`, `localtime_s`, `_tzset`, `QueryPerformanceCounter` (`os_win.cc`). No changes.
- **Attribute portability is already handled:** `DART_FORCE_INLINE`/`DART_NOINLINE`/`DART_NORETURN`/`DART_UNUSED`/`DART_PRETTY_FUNCTION` all have `_MSC_VER` arms (`globals.h:266-309`); `DART_UNUSED` is a no-op on MSVC (`globals.h:285-289`) — fine. `PRINTF_ATTRIBUTE` is defined to **empty** on non-GCC/Clang (`globals.h:693-695`) — MSVC does no printf format-checking, correct.
- **Alignment:** `ALIGN8`/`ALIGN16` map to `__declspec(align(8|16))` on Windows (`vm/globals.h:87-93`). Valid.
- **Inline asm:** `COPY_FP_REGISTER` on Win-x64 is defined to *not* use inline asm (MSVC x64 forbids it) — it returns the stack pointer instead (`vm/globals.h:113-116`). The IA-32 `__asm { mov fp, ebp }` arm is not compiled for x64. Good — no MSVC-x64 inline-asm problem.
- **`#pragma warning`:** Dart uses **none** in `runtime/*.cc` (grep = 0 hits). All warning control is via build flags (§6), so your CMake owns it entirely — nothing hidden in sources.
- **Export macro:** `DART_EXPORT` = `extern "C" __declspec(dllexport)` only when `DART_SHARED_LIB` is defined, else plain `extern "C"` (`include/dart_api.h:42-46`). For a static-lib/exe JIT build, **leave `DART_SHARED_LIB` undefined**. `include/dart_api.h:34-41` also self-typedefs `int8_t…` under `_WIN32` (same benign redefinition note as §3d).

---

## 5. Shims already present (don't reinvent) vs. gaps for 2026

**Already present (2017-era, still correct):**
- windows.h taming: `WIN32_LEAN_AND_MEAN`/`NOMINMAX`/`NOUSER`/… (`globals.h:14-54`).
- windows.h symbol un-pollution: `#undef PARITY_EVEN/PARITY_ODD/near` (`vm/globals.h:15-17`), `#undef OVERFLOW` (`vm/globals.h:21`).
- 64-bit literal suffixes: `DART_INT64_C(x)=x##I64` on MSVC (`globals.h:417-423`).
- printf format specifiers for MSVC: `PRIdPTR="Id"`, `PRId64="I64d"` (`inttypes_support_win.h:18-25`).
- atomics, bit-scan, byteswap, timezone, high-res clock, `_vscprintf`-based `VSNPrint` (`os_win.cc:313-345` — note it *correctly* uses `_vscprintf` to measure, unlike the `snprintf` idiom in §3a).
- `c99_support_win.h` — a whole pre-VS2013 math/`strtoll` shim, **correctly gated `_MSC_VER < 1800`** so it's inert now.
- Build-flag foreknowledge of MSVC noise: `/wd4005` (PRI redefinition), `/wd4091`, `/wd4351`, `/wd4312/4838/4172/4311/4477` (`build/config/compiler/BUILD.gn:519-530`; gyp `4351` at `configurations_msvs.gypi:76,109,142`).

**Gaps for 2026 (what the engineer must add):**
1. Guard the two obsolete macros `snprintf`/`strtoll` with `_MSC_VER < 1900` (§3a) — *the* required source edit.
2. `_CRT_SECURE_NO_WARNINGS` for the `strncpy`/`sscanf` core calls (§3b).
3. `/bigobj` for the giant TUs (§6) — **never present** in the original build.
4. Explicit `/std:c++17` — original relied on the compiler default (§6).
5. Decide `/EHsc` vs `_HAS_EXCEPTIONS=0` under modern MS-STL (§6).

---

## 6. Recommended MSVC flag set

Starting point that mirrors the working 2017 config, adjusted for 2026:

| Flag | Recommendation | Rationale |
|---|---|---|
| `/std:c++17` | **Set explicitly** | Original set no `/std` (`/std:` absent everywhere) and rode the VS2015/2017 default. MSVC 19.50's default may not be C++14; the code is C++11/14-era and clean under C++17. Pin it. Avoid `/std:c++20`+ initially (more conformance surface). |
| `/bigobj` | **ADD (high confidence)** | Not in the original build at all. `vm/object.cc` is **23,362 lines** (single TU); `vm/dart_api_impl.cc` 6,896; `vm/kernel_to_il.cc` 6,839. Heavier 2026 inlining/template expansion very plausibly exceeds the 65,279-section COFF limit → `fatal error C1128`. Cheap insurance; apply project-wide. |
| `/MP` | **ADD** | Faster parallel compile of ~580 TUs. Cosmetic, not correctness. |
| `/EHsc` vs exceptions-off | **Start exceptions-OFF to match** (`_HAS_EXCEPTIONS=0`, no `/EHsc`) | Dart uses `setjmp`/`longjmp` (`LongJumpScope`) for error handling and deliberately disabled EH (`configurations_msvs.gypi:19,57,87`; `build/config/compiler/BUILD.gn:441`). **Fallback:** if modern MS-STL headers pulled by `kernel_to_il.cc` (`<set>/<vector>/<map>/<string>`) refuse to compile with `_HAS_EXCEPTIONS=0` (a known 17.x friction point), switch to `/EHsc` and drop `_HAS_EXCEPTIONS=0`. Cheap test: compile `kernel_to_il.cc` alone both ways. |
| `/Zc:__cplusplus` | **ADD** | Makes `__cplusplus` report the true standard; harmless and forestalls any third-party header (double-conversion) that checks it. |
| `/permissive-` | **DO NOT add initially** | This is 2012–2017 code written under MSVC's permissive default; expect two-phase-name-lookup / dependent-base issues in the template-heavy headers. CMake+MSVC does **not** add `/permissive-` by default, so simply don't opt in. Revisit only if a conformance pass is desired, budgeting for `this->`/qualification fixes. |
| `/WX` | **Match original, but stage it** | Original treats warnings as errors for non-x86 (`build/config/compiler/BUILD.gn:515-517`). Recommend building **without `/WX` first** to get a full warning inventory, then re-enable with the suppression list below. |
| Warning suppressions to carry | `/wd4005 /wd4091 /wd4351 /wd4312 /wd4838 /wd4172 /wd4311 /wd4477` | Exactly the set the original build already needed (`build/config/compiler/BUILD.gn:521-529`, gyp `4351`). `/wd4005` is *required* if `/WX` is on (PRI-macro redefinition, §3d). |
| Runtime library | `/MT` (release) `/MTd` (debug) | Static CRT, matching original (`configurations_msvs.gypi:59,90,124`; `build/config/compiler/BUILD.gn:431-436`). Keep consistent across all objects and the embedder. |
| Preprocessor (recap §1) | `NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _CRT_SECURE_NO_DEPRECATE _HAS_EXCEPTIONS=0 _WIN32_WINNT=0x0603 WINVER=0x0603` | |

Optional to consider if warnings become noisy under 2026: `/wd4996` (redundant once `_CRT_SECURE_NO_WARNINGS` is set), `/wd4267`/`/wd4244` (size_t↔int narrowing — very common in this codebase; likely appear once `/WX` is on).

---

## 7. Prioritized "first 10 fixes" (highest error-clearing leverage)

1. **Guard `#define snprintf _snprintf`** in `platform/globals.h:670` with `#if defined(_MSC_VER) && _MSC_VER < 1900`. Clears the `std::_snprintf` C2039 class of errors across every TU that includes an STL header after globals.h, and fixes the latent `(NULL,0)`-measure bug. *(source edit)*
2. **Guard `#define strtoll _strtoi64`** in `platform/globals.h:427` the same way. Same collision class via `<cstdlib>`. *(source edit)*
3. **Define `_CRT_SECURE_NO_WARNINGS` + `_CRT_SECURE_NO_DEPRECATE`** globally — clears C4996 on `strncpy`/`sscanf`/`_vsnprintf` in `zone.cc`, `uri.cc`, `object.cc`, `flags.cc`, `dart_api_impl.cc`, `il_printer.cc`, `os_win.cc`, `lib/vmservice.cc`. *(build flag)*
4. **Add `/bigobj`** project-wide — pre-empts `C1128` on `vm/object.cc` (23k lines) and the kernel/api TUs. *(build flag)*
5. **Define `NOMINMAX` + `WIN32_LEAN_AND_MEAN` on the command line** (not only via globals.h) — protects `range->min()/max()` member calls in `flow_graph_range_analysis.cc` for any TU that reaches windows.h through a system header first. *(build flag)*
6. **Set `/std:c++17` explicitly** and don't add `/permissive-` — stops the build from inheriting an unexpected default standard or conformance mode. *(build flag)*
7. **Keep `/wd4005`** (and the rest of the original suppression list) if you enable `/WX` — the PRI-macro redefinition in `inttypes_support_win.h:18-25` is otherwise fatal. *(build flag)*
8. **Leave `DART_PRECOMPILED_RUNTIME`/`DART_PRECOMPILER`/`DART_SHARED_LIB` undefined**, and do **not** hand-define `TARGET_ARCH_X64`/`TARGET_OS_WINDOWS` — let `globals.h` auto-derive the JIT-x64 config from `_M_X64`/`_WIN32`. *(build config)*
9. **Build once without `/WX`** to capture the true warning set (expect C4267/C4244 size_t↔int narrowing to dominate), then re-enable `/WX` with targeted `/wd` additions. *(process)*
10. **Compile `vm/kernel_to_il.cc` in isolation both with and without `_HAS_EXCEPTIONS=0`** to decide the exceptions story early (it's the heaviest STL consumer: `<set>/<vector>/<map>`); pick `/EHsc` if the no-exceptions STL path fails. *(process / cheap probe)*

---

## Uncertainties & cheap tests

- **snprintf/strtoll STL collision is order-dependent.** It *will* fire for any TU that includes `platform/globals.h` before an STL header emitting the `using`. Google-style ordering hides it in most existing files, but the guarded-macro fix (#1/#2) removes the risk unconditionally, so just apply it rather than auditing include order. Test: the 3-line TU in §3a.
- **`_HAS_EXCEPTIONS=0` under 2026 MS-STL** — genuinely uncertain; MS has been eroding support. Probe with fix #10.
- **`/bigobj` necessity** — high-confidence-likely, not proven without a compile. It is harmless to add unconditionally, so add it rather than wait for `C1128`.
- **`NOGDI`** — not required for compilation (the `GetObject`→`GetObjectW` rename at `service.cc:3836` is self-consistent), but if a link stage reports an unresolved/duplicated `GetObjectW`, add `NOGDI`.
- **`/permissive-` fallout** — not quantified here (would need a conformance compile). Recommendation is to avoid it, so it should not block the port.

*All paths under `e:\dart_origins\sdk-1.24.3\`. This dossier modified no source.*
