@echo off
setlocal

rem ---------------------------------------------------------------------------
rem Pin the playable game mode onto the Cartoon City "Demonstration" map.
rem
rem This is a one-time, idempotent step: it writes the per-level GameMode override
rem (World Settings -> GameMode Override) so that opening or playing the
rem Demonstration map spawns the player and the chasers. It does NOT touch the RL
rem training map (L_PursuitAITrain), which keeps its own setup and its own game mode.
rem
rem The editor must be CLOSED when you run this: it holds a lock on the .umap and
rem the save fails with Error Code 32 otherwise.
rem ---------------------------------------------------------------------------

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
set "UE=E:\Project\UE5\UE_5.7"
set "CMD=%UE%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"

if not exist "%PROJ%\PursuitAI.uproject" (
  echo [set_demo_map] project not found under %PROJ%
  goto :fail
)
if not exist "%CMD%" (
  echo [set_demo_map] editor not found: %CMD%
  goto :fail
)

echo [set_demo_map] pinning PursuitPlayGameMode onto Demonstration ...
"%CMD%" "%PROJ%\PursuitAI.uproject" -run=pythonscript ^
  -script="%PROJ%\tools\set_demo_gamemode.py" -unattended -nosplash -nullrhi
if errorlevel 1 (
  echo [set_demo_map] FAILED - if the editor has this map open, close it first
  goto :fail
)

echo [set_demo_map] done. The Demonstration map now starts the playable scene.
exit /b 0

:fail
echo [set_demo_map] aborted
exit /b 1
