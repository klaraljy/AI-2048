@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

rem ===========================================================================
rem  AI-2048 one-click launcher. Double-click this file to play.
rem
rem  This file MUST stay pure ASCII. That is not a style preference.
rem
rem  cmd.exe executes a .bat by seeking through the file by BYTE OFFSET, and it
rem  re-seeks after every command. The "chcp 65001" above switches the console
rem  to UTF-8. Once that happens, cmd's offset arithmetic and the file's
rem  multi-byte UTF-8 sequences disagree: it lands mid-character and starts
rem  executing fragments of otherwise harmless "rem" lines as commands.
rem
rem  Real symptom observed:
rem      'vel'' is not recognized as an internal or external command
rem      '<fragment of a Chinese comment>' is not recognized ...
rem
rem  Those fragments came from comments in THIS header. Keeping the file ASCII
rem  removes the whole failure class: no multi-byte sequences, nothing for the
rem  offset arithmetic to get wrong, and it behaves the same under every code
rem  page and locale.
rem
rem  The same reasoning already applies to the sibling lesson in the project
rem  notes about LF line endings, which produced the equally cryptic
rem  "'rlevel' is not recognized". Line endings here are CRLF.
rem
rem  The Chinese explanation of what this script does lives in
rem  tools\launch.mjs, which is JavaScript and has no such restriction.
rem ===========================================================================

where node >nul 2>nul
if errorlevel 1 (
  echo.
  echo   [ERROR] Node.js not found. Install Node.js, or add it to PATH.
  echo.
  pause
  exit /b 1
)

rem All real work happens in launch.mjs: it starts the engine and the static
rem server, opens the browser, and -- importantly -- cleans up both child
rem processes on exit. A .bat cannot do that reliably, because when the user
rem force-closes the window its remaining statements never run and the engine
rem is left behind. Node's exit / SIGINT handling is dependable.
node tools\launch.mjs
set "EXITCODE=%ERRORLEVEL%"

echo.
if not "%EXITCODE%"=="0" (
  echo   Launcher exited with code %EXITCODE%.
  echo.
)
pause
endlocal
exit /b %EXITCODE%
