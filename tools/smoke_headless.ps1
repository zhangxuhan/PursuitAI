# Headless smoke test: boot the editor with no RHI, run the Schola status command, quit.
# Pure ASCII on purpose: PowerShell 5.1 parses .ps1 as GBK unless a UTF-8 BOM is present.
#
# Notes / gotchas learned the hard way on this machine:
#  - Do NOT use Start-Process with -RedirectStandardOutput: it throws
#    "An item with the same key has already been added. Key: PATH" when the parent
#    environment contains both PATH and Path. Use a raw .NET Process instead.
#  - Do NOT write  '"-x="' + $v + '"'  inside an array literal: the comma operator binds
#    tighter than '+', so it silently splits into three elements. Use -f formatting.
param(
    [string]$Project = "E:\Project\UE5+ai\PursuitAI.uproject",
    [string]$Engine  = "E:\Project\UE5\UE_5.7",
    [string]$LogDir  = "E:\Project\UE5+ai\logs",
    [string]$ExecCmds = "PursuitAI.ScholaStatus,QUIT_EDITOR",
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = "Continue"
$exe = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $exe)) { Write-Output "MISSING_EXE $exe"; exit 2 }
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$abslog = Join-Path $LogDir "headless_smoke.log"
if (Test-Path $abslog) { Remove-Item $abslog -Force -ErrorAction SilentlyContinue }

$argLine = '"{0}" -nullrhi -unattended -nosplash -NoSound -NoLogTimes -ExecCmds="{1}" -stdout -abslog="{2}"' -f $Project, $ExecCmds, $abslog

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
    Start-Sleep -Seconds 2
} else {
    Write-Output ("EXITCODE=" + $proc.ExitCode)
}

# Give the async readers a moment, then persist the captured streams.
Start-Sleep -Seconds 1
$so = ""
$se = ""
try { $so = $outTask.Result } catch {}
try { $se = $errTask.Result } catch {}
[System.IO.File]::WriteAllText((Join-Path $LogDir "headless_stdout.txt"), $so, (New-Object System.Text.UTF8Encoding $false))
[System.IO.File]::WriteAllText((Join-Path $LogDir "headless_stderr.txt"), $se, (New-Object System.Text.UTF8Encoding $false))

Write-Output ("STDOUT_LEN=" + $so.Length)
Write-Output ("ABSLOG_EXISTS=" + (Test-Path $abslog))
Write-Output "SMOKE_DONE"
