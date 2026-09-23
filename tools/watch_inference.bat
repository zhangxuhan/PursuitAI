@echo off
setlocal

rem ---------------------------------------------------------------------------
rem Watch the *trained* policy drive the chase, with no Python in the loop.
rem
rem Difference from watch_demo.bat:
rem   watch_demo.bat       built-in greedy policy   - needs nothing
rem   watch_inference.bat  the exported ONNX model - needs checkpoints\policy.onnx
rem
rem If the model is missing, run run_training.bat once and then
rem export_policy.bat. See docs\TUTORIAL.md section 4.
rem
rem Close the window (or press Alt+F4) to stop.
rem ---------------------------------------------------------------------------

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
set "UE=E:\Project\UE5\UE_5.7"
set "MAP=/Game/Maps/L_PursuitAITrain"
set "EDITOR=%UE%\Engine\Binaries\Win64\UnrealEditor.exe"
set "MODEL=%PROJ%\checkpoints\policy.onnx"

if not exist "%PROJ%\PursuitAI.uproject" (
  echo [watch_inference] project not found under %PROJ%
  goto :fail
)

if not exist "%EDITOR%" (
  echo [watch_inference] editor not found: %EDITOR%
  goto :fail
)

if not exist "%MODEL%" (
  echo.
  echo [watch_inference] no exported model at:
  echo                   %MODEL%
  echo.
  echo   Train one first :  tools\run_training.bat
  echo   Then export it  :  tools\export_policy.bat
  echo.
  goto :fail
)

echo [watch_inference] compiling...
call "%UE%\Engine\Build\BatchFiles\Build.bat" PursuitAIEditor Win64 Development -Project="%PROJ%\PursuitAI.uproject" -WaitMutex -NoUBA
if errorlevel 1 (
  echo [watch_inference] build failed - fix the compile errors and try again
  goto :fail
)

echo [watch_inference] model : %MODEL%
echo [watch_inference] launching - close the window to stop
start "" "%EDITOR%" "%PROJ%\PursuitAI.uproject" %MAP% -game -windowed -resx=1280 -resy=720 -PursuitInference

exit /b 0

:fail
echo [watch_inference] aborted
exit /b 1
