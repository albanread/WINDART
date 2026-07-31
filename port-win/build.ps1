# WINDART S1 build driver.
#
# Activates the MSVC 2026 x64 toolchain (vcvars64), configures the VM-core CMake
# project with Ninja, and builds it — teeing all output to build\build.log.
#
# Usage (from any normal PowerShell):
#   powershell -ExecutionPolicy Bypass -File e:\windart\port-win\build.ps1
#   ...\build.ps1 -Clean          # wipe the build dir first (fresh configure)
#   ...\build.ps1 -Target dart_engine
#
# cl.exe is only on PATH after vcvars64, so we run cmake+ninja INSIDE the same
# `cmd /c` that sources vcvars (the env does not survive back to PowerShell).
[CmdletBinding()]
param(
  [switch]$Clean,
  [string]$Target = "",
  [string]$Config = "Debug"
)

$ErrorActionPreference = "Stop"

$VcVars = "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"
$SrcDir = "e:\windart\port-win"
$BuildDir = "e:\windart\build"
$LogFile = Join-Path $BuildDir "build.log"

if (-not (Test-Path $VcVars)) {
  throw "vcvars64.bat not found at: $VcVars"
}

if ($Clean -and (Test-Path $BuildDir)) {
  Write-Host "build.ps1: cleaning $BuildDir"
  Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# Build the ninja target argument (empty = default = all).
$NinjaTarget = ""
if ($Target -ne "") { $NinjaTarget = " $Target" }

# One cmd invocation: vcvars -> cmake configure -> ninja build, chained with
# `&&` so a failed step aborts the rest. MUST be a SINGLE-LINE command string:
# PowerShell does not pass a multi-line here-string (with `^` continuations)
# through to `cmd /c` correctly — cmd misparses it and runs nothing. `-k 0`
# keeps ninja going after an error so a failing build yields a full burn-down.
$Inner = "call `"$VcVars`" && " +
         "cmake -G Ninja -S `"$SrcDir`" -B `"$BuildDir`" -DCMAKE_BUILD_TYPE=$Config && " +
         "ninja -C `"$BuildDir`" -k 0$NinjaTarget"

# Redirect the WHOLE chain to the log INSIDE cmd (parenthesized group), for two
# reasons: (1) cmd's `>` writes ANSI, so build.log stays grep-able — PowerShell's
# own `>` would write UTF-16; (2) all native output (incl. vcvars64's benign
# `vswhere.exe` stderr line) goes to the file, so PowerShell never promotes it to
# a terminating NativeCommandError. $LASTEXITCODE is then ninja's real exit code.
$Full = "( $Inner ) > `"$LogFile`" 2>&1"

Write-Host "build.ps1: configuring + building ($Config) -> $LogFile"
$ErrorActionPreference = "Continue"
& cmd /c $Full
$rc = $LASTEXITCODE
# Echo the tail so the console still shows the result.
if (Test-Path $LogFile) { Get-Content -Path $LogFile -Tail 30 }

Write-Host "build.ps1: exit code $rc (full log: $LogFile)"
exit $rc
