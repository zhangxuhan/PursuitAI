@echo off
setlocal

rem ---------------------------------------------------------------------------
rem Launch PursuitAI in watch mode.
rem
rem No Python, no training: the environment skips its gRPC connector and drives
rem the chase itself with a built-in greedy policy, while laying out its own
rem floor, lighting and top-down camera. Nothing has to be placed in the editor.
rem
rem Close the window (or press Alt+F4) to stop.
rem ---------------------------------------------------------------------------

set "UE_ROOT=E:\Project\UE5\UE_5.7"
set "PROJECT=E:\Project\UE5+ai\PursuitAI.uproject"
set "MAP=/Game/Maps/L_PursuitAITrain"
set "EDITOR=%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe"

if not exist "%PROJECT%" (
  echo [watch_demo] project not found: %PROJECT%
  goto :fail
)

if not exist "%EDITOR%" (
  echo [watch_demo] editor not found: %EDITOR%
  goto :fail
)

echo [watch_demo] compiling...
call "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" PursuitAIEditor Win64 Development -Project="%PROJECT%" -WaitMutex -NoUBA
if errorlevel 1 (
  echo [watch_demo] build failed - fix the compile errors and try again
  goto :fail
)

echo [watch_demo] launching - close the window to stop
start "" "%EDITOR%" "%PROJECT%" %MAP% -game -windowed -resx=1280 -resy=720 -PursuitDemo

exit /b 0

:fail
echo [watch_demo] aborted
exit /b 1
