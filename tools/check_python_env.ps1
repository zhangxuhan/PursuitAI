# Verify the Python side of the Schola toolchain (round 2 criteria S6 / S7 / S8).
#
# Gotchas learned the hard way on this machine:
#  - Do NOT pipe a multi-line here-string into `python -c $snippet`: PowerShell flattens
#    the newlines and Python raises "SyntaxError: '(' was never closed".
#    Always write the snippet to a real .py file and execute that file.
#  - .ps1 must stay ASCII-only: PowerShell 5.1 parses the file as GBK unless a UTF-8 BOM
#    is present, and non-ASCII bytes then produce bogus syntax errors.
param(
    [string]$Venv    = "<project>\\.venv",
    [string]$LogDir  = "<project>\\logs"
)

$ErrorActionPreference = "Continue"
$py  = Join-Path $Venv "Scripts\python.exe"
$cli = Join-Path $Venv "Scripts\schola.exe"
$out = Join-Path $LogDir "py_envcheck.txt"
$tmp = Join-Path $env:TEMP "pursuitai_pycheck"
if (-not (Test-Path $tmp)) { New-Item -ItemType Directory -Path $tmp -Force | Out-Null }

$L = New-Object System.Collections.Generic.List[string]
if (-not (Test-Path $py)) { Write-Output "MISSING_VENV_PYTHON $py"; exit 2 }

function Run-Py([string]$name, [string]$code) {
    $script:file = Join-Path $tmp ($name + ".py")
    [System.IO.File]::WriteAllText($script:file, $code, (New-Object System.Text.UTF8Encoding $false))
    $script:L.Add("--- " + $name + " ---")
    $r = & $py $script:file 2>&1 | Out-String
    $script:L.Add($r.Trim())
}

$L.Add("=== python ===")
$L.Add((& $py -V 2>&1 | Out-String).Trim())

$L.Add("")
$L.Add("=== S6: import schola ===")
Run-Py "s6" @'
import schola, sys
print("schola.__version__ =", getattr(schola, "__version__", "n/a"))
print("schola.__file__    =", getattr(schola, "__file__", "n/a"))
print("python             =", sys.version.split()[0])
'@

$L.Add("")
$L.Add("=== SB3 / gymnasium ===")
Run-Py "sb3" @'
import stable_baselines3, gymnasium
print("stable_baselines3 =", stable_baselines3.__version__)
print("gymnasium         =", gymnasium.__version__)
'@

$L.Add("")
$L.Add("=== S8: torch / CUDA ===")
Run-Py "s8" @'
import torch
print("torch      :", torch.__version__)
print("cuda build :", torch.version.cuda)
print("available  :", torch.cuda.is_available())
if torch.cuda.is_available():
    print("device     :", torch.cuda.get_device_name(0))
    print("capability :", torch.cuda.get_device_capability(0))
    print("arch list  :", torch.cuda.get_arch_list())
    a = torch.randn(1024, 1024, device="cuda")
    b = torch.randn(1024, 1024, device="cuda")
    c = (a @ b).sum().item()
    print("matmul ok  :", isinstance(c, float))
else:
    print("device     : N/A")
    print("capability : N/A")
'@

$L.Add("")
$L.Add("=== S7: schola --help ===")
if (Test-Path $cli) {
    $env:PYTHONIOENCODING = "utf-8"
    $L.Add((& $cli --help 2>&1 | Out-String).Trim())
} else {
    $L.Add("MISSING_CLI $cli")
}

$L.Add("")
$L.Add("=== schola --version ===")
$L.Add((& $cli --version 2>&1 | Out-String).Trim())

$L.Add("")
$L.Add("=== schola sb3 --help ===")
$L.Add((& $cli sb3 --help 2>&1 | Out-String).Trim())

[System.IO.File]::WriteAllText($out, ($L -join "`r`n"), (New-Object System.Text.UTF8Encoding $false))
Write-Output ("OUT=" + $out)
Write-Output "PYCHECK_DONE"
