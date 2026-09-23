@echo off
setlocal

rem ---------------------------------------------------------------------------
rem Play the chase on the Cartoon City "Demonstration" map.
rem
rem Nothing is trained here and no model is loaded. You are the one being chased
rem and the chasers are scripted, because the question this answers is "what does
rem the chase feel like to play" - which a training curve cannot answer. The RL
rem training path (L_PursuitAITrain + Schola) is a separate map and is launched by
rem tools\run_training.bat; this script never starts it.
rem
rem   WASD move    Space jump    Shift sprint    mouse look    F11 fullscreen
rem   Alt+F4 quit
rem
rem Any extra switches you pass to this script are forwarded to the game, so the
rem self-playing demo is:
rem
rem   tools\play.bat -PursuitAutoPlay
rem
rem and a capture with no HUD text on it is:
rem
rem   tools\play.bat -PursuitAutoPlay -PursuitNoHud
rem
rem The editor must be closed for the game-mode pin to run: it holds a lock on the
rem .umap while it has the level open, and the save then fails with Error Code 32.
rem
rem The map is also the project's default map (Config/DefaultGame.ini), so opening
rem the editor and pressing Play gives the same scene.
rem ---------------------------------------------------------------------------

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
set "UE=E:\Project\UE5\UE_5.7"
set "MAP=/Game/Cartoon_City_Free/Maps/Demonstration"
set "LEVEL=%PROJ%\Content\Cartoon_City_Free\Maps\Demonstration.umap"
set "EDITOR=%UE%\Engine\Binaries\Win64\UnrealEditor.exe"
set "CMD=%UE%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"

if not exist "%PROJ%\PursuitAI.uproject" (
  echo [play] project not found under %PROJ%
  goto :fail
)

if not exist "%EDITOR%" (
  echo [play] editor not found: %EDITOR%
  goto :fail
)

rem -- forward any extra switches (e.g. -PursuitAutoPlay, -PursuitNoHud) to the game --
set "ORIGINAL=%*"
set "EXTRA=%ORIGINAL:-regen=%"

rem -- ensure the Demonstration map uses the playable game mode (idempotent) --
rem Without this override the map would open with the engine default game mode and
rem nothing would spawn. Running it here means the scene is always correct, whether
rem you arrived via this script or via the editor's Play button (this writes the
rem override into the .umap, so a later editor Play sees it too).
echo [play] pinning playable game mode onto %MAP% ...
"%CMD%" "%PROJ%\PursuitAI.uproject" -run=pythonscript ^
  -script="%PROJ%\tools\set_demo_gamemode.py" -unattended -nosplash -nullrhi
if errorlevel 1 (
  echo [play] could not pin the game mode - if the editor has this map open, close it
  echo [play] first and re-run. The override may already be set from a previous run.
  goto :fail
)

echo [play] locating dotnet to drive UnrealBuildTool...
set "DOTNET="
rem Prefer the engine-bundled dotnet (any version folder under ThirdParty\DotNet) ...
for /d %%D in ("%UE%\Engine\ThirdParty\DotNet\*") do (
  if exist "%%D\win-x64\dotnet.exe" if not defined DOTNET set "DOTNET=%%D\win-x64\dotnet.exe"
)
rem ... otherwise fall back to a system dotnet on PATH (UE 5.7 needs the .NET 8 runtime).
if not defined DOTNET for /f "delims=" %%P in ('where dotnet 2^>nul') do ( if not defined DOTNET set "DOTNET=%%P" )
if not defined DOTNET (
  echo [play] could not find dotnet - the engine's bundled dotnet is missing and no system dotnet was found.
  echo [play] repair the engine via the Epic Games Launcher, or install the .NET 8 runtime, then re-run.
  goto :fail
)

echo [play] compiling (UBT via %DOTNET%)...
"%DOTNET%" "%UE%\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll" PursuitAIEditor Win64 Development -Project="%PROJ%\PursuitAI.uproject" -WaitMutex -NoUBA
if errorlevel 1 (
  echo [play] build failed - see C:\Users\%USERNAME%\AppData\Local\UnrealBuildTool\Log.txt
  goto :fail
)

echo [play] launching - close the window to stop
rem 1280x720 rather than something larger: a windowed game window bigger than the desktop hangs
rem off the edge of the screen, and on a 1280x720 desktop that means the arena is half missing.
rem Raise these if your display is bigger, or press F11 in game.
rem
rem -PursuitCamera=follow is this script's default and it is deliberately LAST. The camera
rem switch is read left to right and the first one wins, so putting it after %EXTRA% means a
rem caller can still override it - tools\play.bat -PursuitCamera=god - while a plain
rem tools\play.bat stays steerable. Without it a -game launch would pick the god camera, which
rem is the right default for a recording and the wrong one for a person holding the mouse.
start "" "%EDITOR%" "%PROJ%\PursuitAI.uproject" %MAP% -game -windowed -resx=1280 -resy=720 %EXTRA% -PursuitCamera=follow

exit /b 0

:fail
echo [play] aborted - read the messages above, and the UE logs in Saved\Logs
pause
exit /b 1
