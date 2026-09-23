# PursuitAI 环境自检脚本
# 用法： powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\check_env.ps1
# 输出可直接阅读；加 -OutFile <path> 同时写文件。
param([string]$OutFile = "")

$ErrorActionPreference = 'SilentlyContinue'
$L = New-Object System.Collections.Generic.List[string]
function W([string]$m) { $script:L.Add($m) }
function Cap([string]$label, [scriptblock]$sb) {
  $r = & $sb 2>&1 | Out-String
  if ([string]::IsNullOrWhiteSpace($r)) { W ("  " + $label + " : (空)") } else { W ("  " + $label + " : " + $r.Trim()) }
}

$PROJ = "E:\Project\UE5+ai"
$UE   = "E:\Project\UE5\UE_5.7"
$PY   = "%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
$VENV = Join-Path $PROJ ".venv\Scripts\python.exe"

W ("===== PursuitAI 环境自检  " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss") + " =====")
W ""

W "--- 1. 项目与引擎路径 ---"
W ("  PROJ  " + $PROJ + "  exists=" + (Test-Path $PROJ))
W ("  UE    " + $UE   + "  exists=" + (Test-Path $UE))
$bv = Join-Path $UE "Engine\Build\Build.version"
if (Test-Path $bv) {
  $j = Get-Content $bv -Raw -Encoding UTF8 | ConvertFrom-Json
  W ("  UE 版本: " + $j.MajorVersion + "." + $j.MinorVersion + "." + $j.PatchVersion + "  changelist=" + $j.Changelist + "  branch=" + $j.BranchName)
} else { W "  UE Build.version: 不存在（引擎未装完）" }
W ("  UnrealEditor.exe  " + (Test-Path (Join-Path $UE "Engine\Binaries\Win64\UnrealEditor.exe")))
W ("  Build.bat         " + (Test-Path (Join-Path $UE "Engine\Build\BatchFiles\Build.bat")))
W ""

W "--- 2. 插件与工程文件 ---"
W ("  Plugins\Schola    " + (Test-Path (Join-Path $PROJ "Plugins\Schola\Schola.uplugin")))
W ("  *.uproject        " + ((Get-ChildItem $PROJ -Filter "*.uproject" -ErrorAction SilentlyContinue | Measure-Object).Count) + " 个")
W ("  Source\ 存在      " + (Test-Path (Join-Path $PROJ "Source")))
W ""

W "--- 3. Visual Studio / 工具链 ---"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
  Cap "VS 实例" { & $vswhere -all -products * -property installationVersion }
  Cap "VS 路径" { & $vswhere -all -products * -property installationPath }
}
$inc = "C:\Program Files (x86)\Windows Kits\10\Include"
if (Test-Path $inc) {
  $sdks = (Get-ChildItem $inc -Directory | Select-Object -ExpandProperty Name) -join ", "
  W ("  Windows SDK: " + $sdks)
} else { W "  Windows SDK: 目录不存在" }
$msvc = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC"
if (Test-Path $msvc) { W ("  MSVC: " + ((Get-ChildItem $msvc -Directory | Select-Object -ExpandProperty Name) -join ", ")) }
else { W "  MSVC: 目录不存在" }
Cap "dotnet SDK" { & dotnet --list-sdks }
W ""

W "--- 4. Python ---"
W ("  系统 Python312  " + (Test-Path $PY))
Cap "python -V" { & $PY -V }
W ("  venv  " + $VENV + "  exists=" + (Test-Path $VENV))
if (Test-Path $VENV) {
  Cap "venv python" { & $VENV -V }
  Cap "torch" { & $VENV -c "import torch;print(torch.__version__, '| cuda', torch.version.cuda, '| avail', torch.cuda.is_available(), '|', (torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'N/A'))" }
  Cap "schola" { & $VENV -c "import schola;print(getattr(schola,'__version__','?'))" }
  Cap "stable_baselines3" { & $VENV -c "import stable_baselines3 as s;print(s.__version__)" }
}
W ""

W "--- 5. GPU ---"
Cap "nvidia-smi 摘要" { & nvidia-smi --query-gpu=name,driver_version,memory.total,compute_cap --format=csv,noheader }
W ""

W "--- 6. 资源 ---"
$cs = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
W ("  内存 总 " + [math]::Round($cs.TotalPhysicalMemory/1GB,2) + " GB / 可用 " + [math]::Round($os.FreePhysicalMemory/1MB,2) + " GB")
Get-CimInstance Win32_LogicalDisk -Filter "DriveType=3" | ForEach-Object {
  W ("  " + $_.DeviceID + " 剩余 " + [math]::Round($_.FreeSpace/1GB,1) + " GB")
}
W ""

W "--- 7. 训练产物目录 ---"
foreach ($d in @("checkpoints","logs","datasets","docs")) {
  W ("  " + $d.PadRight(12) + (Test-Path (Join-Path $PROJ $d)))
}
W ""
W "===== 自检结束 ====="

$text = $L -join "`r`n"
Write-Output $text
if ($OutFile) { [System.IO.File]::WriteAllText($OutFile, $text, (New-Object System.Text.UTF8Encoding $false)) }
