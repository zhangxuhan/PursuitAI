@echo off
setlocal

REM ============================================================
REM  PursuitAI v2 - CITY character training run
REM
REM  Same pipeline as run_training_char.bat, but the sim runs on
REM  Demonstration_Train (a duplicate of the demo map with the env
REM  + agents baked in; see tools\gen_char_city_level.py). The env
REM  actor carries bStartOnCityStage=True, so no stage flags are
REM  needed on the command line - spawn probing, the plaza-capped
REM  spawn radius, the evader leash and the 75 cm jump platforms
REM  are all baked into the level.
REM
REM  Usage:
REM    tools\run_training_char_city.bat 50000 25000
REM      -> 50k steps, checkpoints every 25k (+ ppo_final.zip)
REM
REM  NOTE: the first run on this map cooks the city content and is
REM  noticeably slower than a greybox run. Later runs reuse it.
REM ============================================================

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
set "UE=E:\Project\UE5\UE_5.7"
set "MAP=/Game/Cartoon_City_Free/Maps/Demonstration_Train"

set "STEPS=%~1"
if "%STEPS%"=="" set "STEPS=50000"
set "SAVE_FREQ=%~2"
if "%SAVE_FREQ%"=="" set "SAVE_FREQ=25000"

REM -- keep gRPC and pip away from any machine-local HTTP proxy --
set "NO_PROXY=127.0.0.1,localhost"
set "no_proxy=127.0.0.1,localhost"
set "PYTHONIOENCODING=utf-8"

chcp 65001 >nul

echo ============================================================
echo  PursuitAI v2 training (CITY stage)
echo  project    : %PROJ%
echo  map        : %MAP%
echo  steps      : %STEPS%
echo  checkpoint : every %SAVE_FREQ% steps
echo ============================================================
echo.

if not exist "%PROJ%\.venv\Scripts\schola.exe" (
  echo [ERROR] .venv is missing or incomplete.
  exit /b 1
)

if not exist "%PROJ%\Content\Cartoon_City_Free\Maps\Demonstration_Train.umap" (
  echo [ERROR] %MAP% is missing.
  echo         Generate it first: see tools\gen_char_city_level.py header.
  exit /b 1
)

pushd "%PROJ%"
".venv\Scripts\schola.exe" sb3 train ppo project "PursuitAI.uproject" ^
  --map %MAP% --headless ^
  --build-dir "%PROJ%\Build\Staging" ^
  --timesteps %STEPS% --enable-tensorboard --log-dir "%PROJ%\logs\tb_char_city" ^
  --save-final-policy --checkpoint-dir "%PROJ%\checkpoints" ^
  --enable-checkpoints --save-freq %SAVE_FREQ% ^
  --disable-eval --no-pbar %~3
set "RC=%ERRORLEVEL%"
popd

echo.
if not "%RC%"=="0" (
  echo [ERROR] training failed with exit code %RC%
  exit /b %RC%
)

echo ============================================================
echo  Done. checkpoints\ppo_final.zip = step %STEPS% policy.
echo ============================================================
endlocal
exit /b 0
