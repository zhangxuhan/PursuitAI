@echo off
setlocal

rem ---------------------------------------------------------------------------
rem Train WITH the game window open, so the chase is visible while it learns.
rem
rem Difference from run_training.bat: that one passes --headless (which becomes
rem -nullRHI inside Unreal), this one does not, so the engine renders.
rem
rem The scenery needs no extra flag. The environment turns it on whenever the
rem launch can actually draw something - --headless is the only thing that takes
rem it away. So this is the same training run, with a window.
rem
rem Measured on this machine: headless trains at roughly 400 steps/s, this at
rem roughly 300. The renderer costs about a quarter of the throughput, not most
rem of it - the scene is two spheres and a plane.
rem
rem There is no speed knob here on purpose. Schola's --fps looks like one but is
rem not: it sets a *fixed timestep* for physics, and this environment has no
rem physics, so it changes nothing about how fast training goes. To watch a
rem single decision rather than the whole run, use watch_demo / watch_inference,
rem which throttle to DemoStepsPerSecond.
rem
rem Usage:
rem   tools\watch_training.bat             20000 steps
rem   tools\watch_training.bat 50000       50k steps
rem
rem Close the window (or Ctrl+C in this console) to stop early.
rem A finished checkpoint is only written when the run reaches the end.
rem ---------------------------------------------------------------------------

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
set "UE=E:\Project\UE5\UE_5.7"
set "MAP=/Game/Maps/L_PursuitAITrain"
set "LEVEL=%PROJ%\Content\Maps\L_PursuitAITrain.umap"
set "EDITOR=%UE%\Engine\Binaries\Win64\UnrealEditor.exe"

set "STEPS=%~1"
if "%STEPS%"=="" set "STEPS=20000"

rem -- keep gRPC and pip away from any machine-local HTTP proxy --
set "NO_PROXY=127.0.0.1,localhost"
set "no_proxy=127.0.0.1,localhost"

rem -- Unreal prints UTF-8; match the console codepage before it runs --
chcp 65001 >nul

echo ============================================================
echo  PursuitAI training, window open
echo  project : %PROJ%
echo  steps   : %STEPS%
echo ============================================================
echo.

if not exist "%PROJ%\.venv\Scripts\schola.exe" (
  echo [ERROR] .venv is missing or incomplete. See docs\TUTORIAL.md section 2.
  exit /b 1
)

if not exist "%LEVEL%" (
  echo [ERROR] training level missing.
  echo         Run tools\run_training.bat once - it generates the level headlessly.
  exit /b 1
)

echo [1/1] training ...
echo       the game rebuilds and re-cooks first, so the launch takes a few minutes.
echo       one log line per finished episode appears below.
echo.

pushd "%PROJ%"
".venv\Scripts\schola.exe" sb3 train ppo project "PursuitAI.uproject" ^
  --map %MAP% ^
  --build-dir "%PROJ%\Build\Staging" ^
  --timesteps %STEPS% --no-display-logs ^
  --enable-tensorboard --log-dir "%PROJ%\logs\tb" ^
  --save-final-policy --checkpoint-dir "%PROJ%\checkpoints" ^
  --disable-eval --no-pbar
set "RC=%ERRORLEVEL%"
popd

echo.
if not "%RC%"=="0" (
  echo [ERROR] training failed with exit code %RC%
  echo         See docs\TUTORIAL.md section 6.
  exit /b %RC%
)

echo ============================================================
echo  Done.
echo    curve : tools\open_tensorboard.bat
echo    model : %PROJ%\checkpoints\ppo_final.zip
echo.
echo  Next: tools\export_policy.bat  then  tools\watch_inference.bat
echo ============================================================
endlocal
exit /b 0
