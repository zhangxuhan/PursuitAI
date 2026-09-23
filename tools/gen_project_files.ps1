# Generate Visual Studio solution for the project.
# UE 5.7 removed Engine\Build\BatchFiles\GenerateProjectFiles.bat, so UBT is invoked
# directly through the engine's bundled .NET runtime.
# Pure ASCII on purpose (PowerShell 5.1 reads .ps1 as GBK unless there is a UTF-8 BOM).
param(
    [string]$Project = "E:\Project\UE5+ai\PursuitAI.uproject",
    [string]$Engine  = "E:\Project\UE5\UE_5.7",
    [string]$LogFile = "E:\Project\UE5+ai\logs\gen_project_files.log"
)

$ErrorActionPreference = "Continue"
$dotnet = Join-Path $Engine "Engine\Binaries\ThirdParty\DotNet\8.0.412\win-x64\dotnet.exe"
$ubt    = Join-Path $Engine "Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll"

if (-not (Test-Path $dotnet)) { Write-Output "MISSING_DOTNET $dotnet"; exit 2 }
if (-not (Test-Path $ubt))    { Write-Output "MISSING_UBT $ubt"; exit 2 }

$argLine = '"{0}" -ProjectFiles -Project="{1}" -Game -Engine -Progress' -f $ubt, $Project

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName               = $dotnet
$psi.Arguments              = $argLine
$psi.UseShellExecute        = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true
$psi.CreateNoWindow         = $true
$psi.WorkingDirectory       = (Split-Path $Project -Parent)

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi
[void]$proc.Start()
$ot = $proc.StandardOutput.ReadToEndAsync()
$et = $proc.StandardError.ReadToEndAsync()
[void]$proc.WaitForExit(600000)

$so = ""; $se = ""
try { $so = $ot.Result } catch {}
try { $se = $et.Result } catch {}
[System.IO.File]::WriteAllText($LogFile, ($so + "`r`n=== STDERR ===`r`n" + $se), (New-Object System.Text.UTF8Encoding $false))

Write-Output ("EXITCODE=" + $proc.ExitCode)
Write-Output ("SLN=" + (Test-Path "E:\Project\UE5+ai\PursuitAI.sln"))
Write-Output "GEN_DONE"
