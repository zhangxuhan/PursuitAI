@echo off
setlocal

rem ---------------------------------------------------------------------------
rem Watch the v2 CHARACTER environment live: top-down camera over the C++ arena
rem rig, on-screen pane label / episode-step lines, whatever driver you name.
rem
rem Usage:
rem   tools\watch_char.bat                          greedy built-in baseline
rem   tools\watch_char.bat -PursuitCharRandom       random baseline
rem   tools\watch_char.bat -PursuitCharInference    ONNX at checkpoints\policy.onnx
rem   tools\watch_char.bat -PursuitCharInference -PursuitCharModel=checkpoints\policy_v2_100k.onnx
rem
rem Everything you pass is forwarded to the game untouched. Close the window to stop.
rem ---------------------------------------------------------------------------

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
set "UE=E:\Project\UE5\UE_5.7"
set "EDITOR=%UE%\Engine\Binaries\Win64\UnrealEditor.exe"

set "ARGS=%*"
if "%ARGS%"=="" set "ARGS=-PursuitCharGreedy"

rem A drive mode must ALWAYS reach the env: without one it stays at the Train default,
rem whose Tick is a no-op outside training - no camera, no episodes, a silent first-
rem person pawn view (2026-09-21 watch report). The env now also self-heals this case,
rem but the tool keeps the explicit default so the log stays clean.
set "HASDRIVE=0"
for %%A in (%ARGS%) do (
  echo %%A | findstr /C:"PursuitCharGreedy" >nul && set "HASDRIVE=1"
  echo %%A | findstr /C:"PursuitCharRandom" >nul && set "HASDRIVE=1"
  echo %%A | findstr /C:"PursuitCharInference" >nul && set "HASDRIVE=1"
  echo %%A | findstr /C:"PursuitCharTrain" >nul && set "HASDRIVE=1"
)
if "%HASDRIVE%"=="0" set "ARGS=%ARGS% -PursuitCharGreedy"

rem City stage: -PursuitCharEnvBox (pair it with -PursuitStage=city) runs the char env
rem on the Demonstration map's plaza instead of the grey-box training arena.
set "MAP=/Game/Maps/L_PursuitCharTrain"
for %%A in (%ARGS%) do (
  echo %%A | findstr /C:"PursuitCharEnvBox" >nul && set "MAP=/Game/Cartoon_City_Free/Maps/Demonstration"
)

if not exist "%PROJ%\PursuitAI.uproject" (
  echo [watch_char] project not found under %PROJ%
  exit /b 1
)

rem A custom -PursuitCharModel= may point anywhere, so only the DEFAULT model path is
rem pre-checked; a wrong custom path still fails loudly in the log and falls back to
rem the greedy baseline (the env does that itself).
set "CUSTOM=0"
for %%A in (%ARGS%) do (
  echo %%A | findstr /C:"PursuitCharModel=" >nul && set "CUSTOM=1"
)

set "DEFAULTMODEL=%PROJ%\checkpoints\policy.onnx"
if "%CUSTOM%"=="0" echo %ARGS% | findstr /C:"PursuitCharInference" >nul
if "%CUSTOM%"=="0" if not errorlevel 1 if not exist "%DEFAULTMODEL%" (
  echo.
  echo [watch_char] no exported model at:
  echo              %DEFAULTMODEL%
  echo   Train one first :  tools\run_training_char.bat
  echo   Then export it  :  tools\export_policy.bat ppo_final.zip policy.onnx
  echo   Or name one     :  tools\watch_char.bat -PursuitCharInference -PursuitCharModel=checkpoints\policy_v2_100k.onnx
  echo.
  exit /b 1
)

echo [watch_char] args : %ARGS%
echo [watch_char] launching - close the window to stop
start "" "%EDITOR%" "%PROJ%\PursuitAI.uproject" %MAP% -game -windowed -resx=1280 -resy=720 %ARGS%

endlocal
exit /b 0
