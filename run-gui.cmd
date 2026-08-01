@echo off
REM ============================================================================
REM  WINDART - start the workspace IDE in interactive GUI mode.
REM
REM  Double-click this file, or run it from any shell. It locates dartui.exe and
REM  the workspace app relative to itself, so it works no matter where WINDART
REM  lives or where you run it from.
REM
REM  In the window: click the "Game" tab, then Space / Up to play COIN DASH.
REM
REM  Any extra arguments pass straight through to dartui, e.g.
REM      run-gui.cmd selftest        (headless self-test capture pass, then quit)
REM ============================================================================
setlocal
set "ROOT=%~dp0"
set "EXE=%ROOT%build\dartui.exe"
set "APP=%ROOT%test\workspace.dart"

if not exist "%EXE%" (
  echo [WINDART] dartui.exe not found at:
  echo             "%EXE%"
  echo [WINDART] Build it first:
  echo             powershell -ExecutionPolicy Bypass -File "%ROOT%port-win\build.ps1"
  echo.
  pause
  exit /b 1
)

REM Run from test\ so a game's Isolate.spawnUri('demos/...') resolves.
cd /d "%ROOT%test"
echo [WINDART] launching the IDE  ^(Game tab: Space/Up to play COIN DASH^) ...
start "WINDART" "%EXE%" "%APP%" %*
endlocal
