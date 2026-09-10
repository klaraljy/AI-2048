@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

rem ===========================================================================
rem  AI-2048 一键启动：双击本文件即可开始玩。
rem
rem  真正的启动逻辑在 tools\launch.mjs —— 放在那里是因为批处理在被强制结束
rem  （用户直接叉掉窗口）时收尾语句根本不会执行，引擎会留在后台。
rem  Node 的 exit / SIGINT 处理可靠得多，能把子进程收干净。
rem
rem  本文件只负责：切代码页、检查 node、把控制权交给启动器、别让窗口闪退。
rem
rem  注意：本文件必须保存为 **UTF-8 无 BOM + CRLF**。
rem    - 无 BOM 且开头 chcp 65001 → 中文正常（GBK 反而乱码，已实测）
rem    - 必须是 CRLF：用 LF 时 cmd 会把命令从中间劈开，
rem      报出 'rlevel' is not recognized 这种莫名其妙的错
rem ===========================================================================

where node >nul 2>nul
if errorlevel 1 (
  echo.
  echo   [错误] 找不到 node。请安装 Node.js，或把它加入 PATH。
  echo.
  pause
  exit /b 1
)

node tools\launch.mjs
set "EXITCODE=%ERRORLEVEL%"

echo.
if not "%EXITCODE%"=="0" (
  echo   启动器以代码 %EXITCODE% 结束。
  echo.
)
pause
endlocal
exit /b %EXITCODE%
