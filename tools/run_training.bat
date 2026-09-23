@echo off
setlocal

REM ============================================================
REM  PursuitAI - one-click training run
REM
REM    1. generate the training level if it is missing (headless)
REM    2. run SB3 PPO training over gRPC
REM
REM  Fully unattended: no editor window, no button to press.
REM  See docs\TUTORIAL.md for what all of this means.
REM ============================================================

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
set "UE=E:\Project\UE5\UE_5.7"
set "MAP=/Game/Maps/L_PursuitAITrain"
set "LEVEL=%PROJ%\Content\Maps\L_PursuitAITrain.umap"
set "STEPS=60000"

REM -- keep gRPC and pip away from any machine-local HTTP proxy --
set "NO_PROXY=127.0.0.1,localhost"
set "no_proxy=127.0.0.1,localhost"

REM -- Unreal prints UTF-8; match the console codepage before it runs --
chcp 65001 >nul

echo ============================================================
echo  PursuitAI training
echo  project : %PROJ%
echo  steps   : %STEPS%
echo ============================================================
echo.

if not exist "%PROJ%\.venv\Scripts\schola.exe" (
  echo [ERROR] .venv is missing or incomplete.
  echo         See docs\TUTORIAL.md section 2.
  exit /b 1
)

if not exist "%UE%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" (
  echo [ERROR] Unreal Engine not found at %UE%
  exit /b 1
)

if exist "%LEVEL%" goto level_ok

echo [1/2] generating training level ...
"%UE%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "%PROJ%\PursuitAI.uproject" ^
  -run=pythonscript -script="%PROJ%\tools\gen_training_level.py" ^
  -unattended -nosplash -nullrhi
if errorlevel 1 (
  echo.
  echo [ERROR] level generation failed.
  echo         See docs\TUTORIAL.md section 6.
  exit /b 1
)
echo [1/2] level generated.
goto levels_done

:level_ok
echo [1/2] training level already present, skipping generation.

:levels_done
echo.
echo [2/2] training PPO ...
echo       first run compiles the game first, so it takes a while.
echo.

pushd "%PROJ%"
".venv\Scripts\schola.exe" sb3 train ppo project "PursuitAI.uproject" ^
  --map %MAP% --headless ^
  --build-dir "%PROJ%\Build\Staging" ^
  --timesteps %STEPS% --enable-tensorboard --log-dir "%PROJ%\logs\tb" ^
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
echo    curve   : tools\open_tensorboard.bat
echo    model   : %PROJ%\checkpoints\ppo_final.zip
echo    ue log  : %PROJ%\Build\Staging\Windows\PursuitAI\Saved\Logs\PursuitAI.log
echo.
echo  Next: watch the trained policy drive the game with no Python at all
echo    tools\export_policy.bat      save it as checkpoints\policy.onnx
echo    tools\watch_inference.bat    open the window and watch it
echo ============================================================
endlocal
exit /b 0
