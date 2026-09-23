@echo off
setlocal

REM ============================================================
REM  PursuitAI - open TensorBoard on the training curves
REM
REM  Serves logs\tb at http://localhost:6006
REM  Leave this window open while you look at the page.
REM  Ctrl+C here stops the server.
REM ============================================================

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
set "LOGDIR=%PROJ%\logs\tb"
set "PORT=6006"

REM -- TensorBoard's data server is a local socket; keep the proxy out of it --
set "NO_PROXY=127.0.0.1,localhost"
set "no_proxy=127.0.0.1,localhost"

if not exist "%PROJ%\.venv\Scripts\python.exe" (
  echo [ERROR] .venv is missing. See docs\TUTORIAL.md section 2.
  exit /b 1
)

if not exist "%LOGDIR%" (
  echo [ERROR] no TensorBoard data yet at:
  echo         %LOGDIR%
  echo         Run tools\run_training.bat first. The curve is only written
  echo         when the training command includes --enable-tensorboard.
  exit /b 1
)

echo ============================================================
echo  TensorBoard
echo    data : %LOGDIR%
echo    page : http://localhost:%PORT%
echo ============================================================
echo  Ctrl+C in this window stops the server.
echo.

start "" "http://localhost:%PORT%"
"%PROJ%\.venv\Scripts\python.exe" -m tensorboard.main --logdir "%LOGDIR%" --port %PORT%
endlocal
