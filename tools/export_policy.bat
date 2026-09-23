@echo off
setlocal

rem ---------------------------------------------------------------------------
rem Export a saved SB3 checkpoint to ONNX for in-engine inference, then append
rem the missing Tanh squash (tools/onnx_add_tanh.py - see docs/COMPARISON_PLAN.md,
rem gap RED-1: without it the action direction is off by up to 14.5 degrees).
rem
rem Usage:
rem   tools\export_policy.bat                     exports checkpoints\ppo_final.zip
rem   tools\export_policy.bat other.zip           exports checkpoints\other.zip
rem   tools\export_policy.bat other.zip out.onnx  writes checkpoints\out.onnx
rem
rem Why a separate step from training:
rem   "schola sb3 train --export-onnx" exports *in the middle of* training, while the
rem   policy is still on the GPU. torch.export then trips over tensors on two devices
rem   ("Unhandled FakeTensor Device Propagation for aten.mm.default") and the run dies.
rem   Exporting afterwards loads the checkpoint on the CPU, where it is a non-issue.
rem ---------------------------------------------------------------------------

for %%I in ("%~dp0..") do set "PROJ=%%~fI"

set "CKPT=%~1"
if "%CKPT%"=="" set "CKPT=ppo_final.zip"

rem Output name: explicit second argument, else derived from the checkpoint name,
rem so exporting 0k/100k/500k in a row can no longer overwrite one fixed policy.onnx.
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%~n1.onnx"

set "SRC=%PROJ%\checkpoints\%CKPT%"
set "RAW=%PROJ%\checkpoints\%~n1.raw.onnx"
set "DST=%PROJ%\checkpoints\%OUT%"

rem -- keep pip and any gRPC traffic away from a machine-local HTTP proxy --
set "NO_PROXY=127.0.0.1,localhost"
set "no_proxy=127.0.0.1,localhost"
set "PYTHONIOENCODING=utf-8"

if not exist "%PROJ%\.venv\Scripts\schola.exe" (
  echo [export_policy] .venv is missing or incomplete. See docs\TUTORIAL.md section 2.
  exit /b 1
)

if not exist "%SRC%" (
  echo.
  echo [export_policy] checkpoint not found:
  echo                 %SRC%
  echo.
  echo   Run tools\run_training_char.bat first. If you trained with a different
  echo   name, pass it:  tools\export_policy.bat mymodel.zip
  echo.
  exit /b 1
)

echo [export_policy] in  : %SRC%
echo [export_policy] out : %DST%
echo.

pushd "%PROJ%"
".venv\Scripts\schola.exe" sb3 export --policy-checkpoint-path "%SRC%" --output-path "%RAW%" --algorithm PPO
set "RC=%ERRORLEVEL%"
if "%RC%"=="0" (
  rem Third arg renames the declared output(s) to the actuator keys. schola names the
  rem output `CharMoveInput` for final saves but plain `action` for mid-training
  rem checkpoint zips; the engine's dict action binding looks up by that key, so an
  rem unmatched name silently disables the policy. Since the jump actuator (v2.1) the
  rem action space is a dict {CharMoveInput:2, JumpInput:1}, so BOTH keys are passed;
  rem onnx_add_tanh.py pairs them with the graph outputs in order and Tanh-squashes each.
  ".venv\Scripts\python.exe" tools\onnx_add_tanh.py "%RAW%" "%DST%" CharMoveInput:2,JumpInput:1
  set "RC=%ERRORLEVEL%"
  del "%RAW%" 2>nul
)
popd

echo.
if not "%RC%"=="0" (
  echo [export_policy] export failed with exit code %RC%
  exit /b %RC%
)

if not exist "%DST%" (
  echo [export_policy] export reported success but %DST% is missing
  exit /b 1
)

echo [export_policy] done. Watch it drive:  tools\watch_inference.bat
endlocal
exit /b 0
