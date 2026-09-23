# Generic headless pythonscript runner. The editor must be closed (it locks the .umap).
# Pure ASCII on purpose: PowerShell 5.1 parses .ps1 as GBK unless a UTF-8 BOM is present.
#
# Uses a raw .NET Process rather than Start-Process: Start-Process with
# -RedirectStandardOutput throws "An item with the same key has already been added.
# Key: PATH" when the parent environment holds both PATH and Path.
param(
    [Parameter(Mandatory=$true)][string]$Script,
    [Parameter(Mandatory=$true)][string]$Log,
    [string]$Project = "E:\Project\UE5+ai\PursuitAI.uproject",
    [string]$Engine  = "E:\Project\UE5\UE_5.7",
    [int]$TimeoutSec = 600
)

$ErrorActionPreference = "Continue"
$exe = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $exe)) { Write-Output "MISSING_EXE $exe"; exit 2 }

$argLine = '"{0}" -run=pythonscript -script="{1}" -unattended -nosplash -nullrhi -NoSound -stdout -abslog="{2}"' -f $Project, $Script, $Log

Write-Output ("EXE=" + $exe)
Write-Output ("SCRIPT=" + $Script)
Write-Output ("LOG=" + $Log)

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
Write-Output "PYSCRIPT_DONE"
