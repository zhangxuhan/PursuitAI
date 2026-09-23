# Headless playable-map run for verification. Boots the editor in -game mode, no window,
# with a self-terminating window so the process never has to be killed from outside - an
# UnrealEditor.exe left in memory makes the next UBT build fail in three seconds with
# "Unable to build while Live Coding is active", which reads as a compile error.
#
# Pure ASCII on purpose: PowerShell 5.1 parses .ps1 as GBK unless a UTF-8 BOM is present.
param(
    [string]$Extra = "-PursuitAutoPlay -PursuitRunSeconds=120",
    [string]$Log   = "E:\Project\UE5+ai\Saved\Logs\city_run.log",
    [string]$Project = "E:\Project\UE5+ai\PursuitAI.uproject",
    [string]$Engine  = "E:\Project\UE5\UE_5.7",
    [int]$TimeoutSec = 420
)

$ErrorActionPreference = "Continue"
$exe = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor.exe"
if (-not (Test-Path $exe)) { Write-Output "MISSING_EXE $exe"; exit 2 }

$argLine = '"{0}" -game -nullrhi -unattended -nosplash -NoSound -stdout -abslog="{1}" {2}' -f $Project, $Log, $Extra

Write-Output ("EXE=" + $exe)
Write-Output ("ARGS=" + $argLine)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName               = $exe
$psi.Arguments              = $argLine
$psi.UseShellExecute        = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true
$psi.CreateNoWindow         = $true
$psi.WorkingDirectory       = (Split-Path $Project -Parent)

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi
[void]$proc.Start()

$outTask = $proc.StandardOutput.ReadToEndAsync()
$errTask = $proc.StandardError.ReadToEndAsync()

if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    Write-Output ("TIMEOUT after {0}s -> killing PID {1}" -f $TimeoutSec, $proc.Id)
    try { $proc.Kill() } catch {}
    [void]$proc.WaitForExit(15000)
} else {
    Write-Output ("EXITCODE=" + $proc.ExitCode)
}

Start-Sleep -Seconds 1
Write-Output "PLAYRUN_DONE"
