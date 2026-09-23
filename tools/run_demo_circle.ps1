# Launch the independent circular-arena portfolio demo. No training or checkpoint writes.
# Usage: powershell -ExecutionPolicy Bypass -File tools/run_demo_circle.ps1 [-View Arena|Follow]
param(
    [ValidateSet('Arena', 'Follow')][string]$View = 'Arena',
    [int]$MaxEpisodes = 0,
    [int]$QuitAfterSeconds = 0,
    [int]$Seed = 20260923,
    [string]$EngineRoot = $env:UE_ENGINE_ROOT
)

$projectRoot = Split-Path $PSScriptRoot -Parent
$projectFile = Join-Path $projectRoot 'PursuitAI.uproject'
if ([string]::IsNullOrWhiteSpace($EngineRoot)) {
    throw 'Set UE_ENGINE_ROOT to your Unreal Engine 5.7 installation, or pass -EngineRoot.'
}
$editorExe = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$modelFile = Join-Path $projectRoot 'models\rep3_sb3_exact.onnx'
$levelFile = Join-Path $projectRoot 'Content\Maps\L_PursuitCharDemoCircle.umap'

foreach ($required in @($projectFile, $editorExe, $modelFile, $levelFile)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing required file: $required" }
}

$camera = if ($View -eq 'Arena') {
    '-PursuitWatchAt=0,0,3300 -PursuitWatchFOV=75'
} else {
    '-PursuitWatchHeight=2300 -PursuitWatchPitch=-82 -PursuitWatchFOV=72'
}

$arguments = ('"{0}" /Game/Maps/L_PursuitCharDemoCircle -game -fullscreen -nosplash -NoSound ' +
    '-PursuitCharInference -PursuitCharModel="{1}" ' +
    '-PursuitCharMaxEpisodes={2} -PursuitCharQuitAfter={3} -PursuitCharSeed={4} ' +
    '-PursuitPaneLabel=TRAINED_PPO_rep3 {5}') -f $projectFile, $modelFile, $MaxEpisodes, $QuitAfterSeconds, $Seed, $camera

$process = Start-Process -FilePath $editorExe -ArgumentList $arguments -PassThru
Write-Output "Demo launched: view=$View pid=$($process.Id) map=/Game/Maps/L_PursuitCharDemoCircle"
