@echo off
setlocal

REM ============================================================
REM  PursuitAI v2 - one-click character training run
REM
REM    1. generate the character training level if it is missing (headless)
REM    2. run SB3 PPO training over gRPC, checkpointing every SAVE_FREQ
REM
REM  Usage:
REM    tools\run_training_char.bat                 60000 steps, checkpoints every 100k
REM    tools\run_training_char.bat 500000          half a million steps
REM    tools\run_training_char.bat 500000 100000   explicit checkpoint interval
REM
REM  The agents are real ACharacters moved by CharacterMovementComponent, so the
REM  run MUST be fixed-frame-rate for reproducibility (bUseFixedFrameRate below);
rem  that flag is baked into the simulator config, not passed here.
REM
REM  Fully unattended: no editor window, no button to press.
REM ============================================================

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
set "UE=E:\Project\UE5\UE_5.7"
set "MAP=/Game/Maps/L_PursuitCharTrain"
set "LEVEL=%PROJ%\Content\Maps\L_PursuitCharTrain.umap"

set "STEPS=%~1"
if "%STEPS%"=="" set "STEPS=60000"
set "SAVE_FREQ=%~2"
if "%SAVE_FREQ%"=="" set "SAVE_FREQ=100000"

REM -- keep gRPC and pip away from any machine-local HTTP proxy --
set "NO_PROXY=127.0.0.1,localhost"
set "no_proxy=127.0.0.1,localhost"
set "PYTHONIOENCODING=utf-8"

REM -- Unreal prints UTF-8; match the console codepage before it runs --
chcp 65001 >nul

echo ============================================================
echo  PursuitAI v2 training (character agents)
echo  project    : %PROJ%
echo  steps      : %STEPS%
echo  checkpoint : every %SAVE_FREQ% steps
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

echo [1/2] generating character training level ...
"%UE%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "%PROJ%\PursuitAI.uproject" ^
  -run=pythonscript -script="%PROJ%\tools\gen_char_train_level.py" ^
  -unattended -nosplash -nullrhi
if errorlevel 1 (
  echo.
  echo [ERROR] level generation failed.
  exit /b 1
)
echo [1/2] level generated.
goto levels_done

:level_ok
echo [1/2] training level already present, skipping generation.

:levels_done
echo.
echo [2/2] training PPO ...

pushd "%PROJ%"
".venv\Scripts\schola.exe" sb3 train ppo project "PursuitAI.uproject" ^
  --map %MAP% --headless ^
  --build-dir "%PROJ%\Build\Staging" ^
  --timesteps %STEPS% --enable-tensorboard --log-dir "%PROJ%\logs\tb_char" ^
  --save-final-policy --checkpoint-dir "%PROJ%\checkpoints" ^
  --enable-checkpoints --save-freq %SAVE_FREQ% ^
  --disable-eval --no-pbar
set "RC=%ERRORLEVEL%"
popd

echo.
if not "%RC%"=="0" (
  echo [ERROR] training failed with exit code %RC%
  exit /b %RC%
)

echo ============================================================
echo  Done.
echo    curve      : tools\open_tensorboard.bat  (log dir logs\tb_char)
echo    final model: %PROJ%\checkpoints\ppo_final.zip
echo    interval ckpts: checkpoints\ppo_<N>_steps.zip
echo.
echo  Next:
echo    tools\export_policy.bat ppo_final.zip policy_v2.onnx
echo    then record the pair clip with portfolio_pair.py.
echo ============================================================
endlocal
exit /b 0
