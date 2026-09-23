# PursuitAI — 项目计划

## 0. 项目定位

用 Unreal Engine C++ + AMD Schola，做一个**可训练、可评测、可部署**的追击 / 躲避游戏 AI 环境。
第一阶段只训练**追击者**（Learner），逃跑者由**规则 AI** 驱动。

- 不做世界模型、不从零实现 PPO、不自实现 Gym 接口、不自实现 UE↔Python 通信协议
- 不做高清图像 Observation、不在第一阶段同时训练双方、不做战斗 / 动画 / 联机 / 大地图
- 策略：**最大化复用 Schola 官方基础设施**（UE 环境接口、Gymnasium 兼容、SB3、RLlib、并行环境、无渲染训练、评测、Minari、ONNX/NNE 推理）

---

## 1. 九轮路线图

| 轮次 | 目标 | 状态 |
|---|---|---|
| **1** | 本机环境审计与技术确认 | **已完成** |
| **2** | 创建最小 C++ 项目并接入 Schola | **已完成** |
| **3** | 自建最小 C++ Schola 环境 + 打通 gRPC 训练闭环 | **已完成** |
| 4 | 实现自定义 1v1 环境 + 规则 AI 逃跑者 | 未开始 |
| 5 | 接入 Stable-Baselines3 PPO | 未开始 |
| 6 | 评测 / 并行训练 / 无渲染训练 | 未开始 |
| 7 | ONNX 导出 + Unreal 运行时推理（NNE） | 未开始 |
| 8 | 可选：多智能体共享 Policy + 模仿学习 | 未开始 |
| 9 | 作品整理、演示视频、技术报告 | 未开始 |

> **第 3 轮路线修正（2026-09-19，用户决策）**：原计划"复刻官方 Tag 蓝图示例"改为 **纯 C++ 自建最小环境**。
> 理由：Tag 示例是 16 个 `.bp` 蓝图文本，需在编辑器里手工粘贴搭建，不可脚本化、不可版本控制友好地 diff，
> 且第 4 轮真正要写的是 1v1 追击，蓝图形态复用价值低。C++ 环境可提交、可 diff、可被第 4 轮直接扩写成正式的 1v1 骨架。
> 用户经 AskUserQuestion 选择 **"C++ 自建环境（推荐）"**。

---

## 2. 第 1 轮结论摘要

- **结论**：本机大部分条件已就位（VS2022 完整工作负载、RTX 5080 + 591.86 驱动、Git + LFS、E: 盘 2.4 TB 空闲），但**有两项硬阻塞**：
  1. **没有安装任何 UE5**（只有 UE 4.27.2）→ 必须安装 UE **5.7.4**
  2. **Windows SDK 只有 10.0.18362.0**，低于 Epic 官方 UE5 最低要求 10.0.19041.0，也低于推荐值 10.0.22621.0 → 必须补装
- **版本选型确认**：Schola v2.1.1 官方测试矩阵为 UE 5.5–5.7 / Python 3.10–3.12 → **UE 5.7.4 + Python 3.12.10（本机已有）** 是当前唯一稳妥组合。UE 5.8 虽已发布，但**出界，不用**。
- **计划修正 1**：原计划写 Python 3.11，但本机已有 3.12.10 且在 Schola 支持区间内 → **建议直接用 3.12**，省一次安装（3.11 用 `uv` 也能随时补）。
- **计划修正 2**：Schola 官方示例**没有 "Basic"**。官方 examples 只有三项：**Tag**（3v1 多智能体追击）、StateTree RL、XArm5（机器人）；而且**仓库内没有可直接打开的 `.uproject`**，示例形态是"文档教程 + 可复制粘贴的蓝图文本"。第 3 轮应改成：Tag 教程 + Schola 自带 Python 侧连通性验证。
- **未验证项**（装完引擎才能定论）：MSVC 14.44.35207 是否被 UE 5.7 接受、是否需要补 .NET 8、UE 5.7 实际占用体积。

详见 `ENVIRONMENT_AUDIT.md`。

---

## 3. 已确认决策（2026-09-19，用户答复）

| # | 事项 | 决策 | 落地状态 |
|---|---|---|---|
| D1 | 项目根目录 | **`E:\Project\UE5+ai`**（复用原空工作区） | 已生效。三份文档即位于 `<project>\\docs\` |
| D2 | Unreal Engine 5.7 | **安装中**，路径 **`E:\Project\UE5\UE_5.7`** | 用户手动安装中（2026-09-19 14:36 起，下载已完成 8.1 GB 时审计，引擎文件尚未落盘） |
| D3 | Windows 11 SDK 10.0.22621.0 | **由我直接安装** | 通过 VS Installer `--add Microsoft.VisualStudio.Component.Windows11SDK.22621` 执行中 |
| D4 | Python | **用本机已有的 3.12.10** | 已生效。不安装 3.11 |
| D5 | 后续缺失环境的安装策略 | **可由我直接安装**；但**尽量装在工作目录内，不装 C 盘** | 生效。例外：Windows SDK 这类工具链组件必须留在 `C:\Program Files (x86)\Windows Kits\10`（VS/UBT 通过注册表 `KitsRoot10` 定位），不可搬移 |
| D6 | 文档维护 | 结论统一写进 `docs/`；本地工作笔记不入库 | 已建立 `docs/` 目录，含计划、教程、环境要求与评估记录 |

---

## 4. 第 2 轮执行计划（精确版）

> 前提：D1–D6 已确认；UE 5.7.4 与 Windows SDK 已安装完成。
> 变量约定：
> - `PROJ` = 项目根目录（默认 `E:\Project\UE5+ai`）
> - `UE` = `E:\Project\UE5\UE_5.7`
> - `PY` = `%LOCALAPPDATA%\Programs\Python\Python312\python.exe`

### 4.1 目录结构（目标形态）

```
<project>\\
├─ .git\                     # 本地仓库（不建远程）
├─ .gitignore                # UE 标准 + .venv / logs / checkpoints
├─ .gitmodules               # 若 Schola 用 submodule 方式
├─ PursuitAI.uproject
├─ Config\
│  ├─ DefaultEngine.ini
│  ├─ DefaultGame.ini
│  └─ DefaultEditor.ini
├─ Content\                  # 先留空
├─ Plugins\
│  └─ Schola\                # Schola v2.1.1（插件根 = 仓库根）
├─ Source\
│  ├─ PursuitAI.Target.cs
│  ├─ PursuitAIEditor.Target.cs
│  └─ PursuitAI\
│     ├─ PursuitAI.Build.cs
│     ├─ PursuitAI.h / .cpp      # 模块入口
│     └─ (后续轮次：Environment / Trainer / Agent 相关类)
├─ docs\                     # 本目录（HANDOFF / ENVIRONMENT_AUDIT / PROJECT_PLAN）
├─ .venv\                    # Python 虚拟环境（git 忽略）
├─ checkpoints\              # 训练产物（git 忽略）
├─ logs\                     # TensorBoard / 训练日志（git 忽略）
└─ datasets\                 # Minari 轨迹（第 8 轮，git 忽略）
```

### 4.2 逐步操作

**Step 1 — 建目录 + 本地 git**
```powershell
New-Item -ItemType Directory -Force -Path "E:\Project\UE5+ai" | Out-Null
Set-Location "E:\Project\UE5+ai"
git init -b main
```
> 只 `git init`，**不** `git remote add`，**不** push。

**Step 2 — 生成项目骨架（二选一）**

- **方案 A（推荐，最稳）**：Graphical
  用 Epic Launcher 启动 UE 5.7 → **Games → Blank** → **C++** → 名称 `PursuitAI` → 路径 `E:\Project` → 取消勾选 Starter Content → Create。
  编辑器会自动生成 `.uproject` / `Source/` / `Config/` 并编译一次。

- **方案 B（可脚本化，便于审计）**：手写最小骨架
  需要我生成的 5 个文件：`PursuitAI.uproject`、`Source/PursuitAI.Target.cs`、`Source/PursuitAIEditor.Target.cs`、`Source/PursuitAI/PursuitAI.Build.cs`、`Source/PursuitAI/PursuitAI.cpp`
  `.uproject` 内容要点：
  ```json
  {
    "FileVersion": 3,
    "EngineAssociation": "5.7",
    "Category": "",
    "Description": "",
    "Modules": [
      { "Name": "PursuitAI", "Type": "Runtime", "LoadingPhase": "Default" }
    ],
    "Plugins": [
      { "Name": "Schola", "Enabled": true },
      { "Name": "EnhancedInput", "Enabled": true },
      { "Name": "StateTree", "Enabled": true },
      { "Name": "GameplayStateTree", "Enabled": true }
    ]
  }
  ```

**Step 3 — 放入 Schola 插件 v2.1.1**

方式一（普通 clone，最简单）：
```powershell
git clone --branch v2.1.1 --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/Schola.git "<project>\\Plugins\Schola"
Remove-Item -Recurse -Force "<project>\\Plugins\Schola\.git"
```

方式二（submodule，便于日后升级）：
```powershell
git -C "E:\Project\UE5+ai" submodule add -b v2.1.1 https://github.com/GPUOpen-LibrariesAndSDKs/Schola.git Plugins/Schola
```

> 插件根目录**就是仓库根目录**（含 `Schola.uplugin`、`Source/`、`Resources/`、`Config/`）。
> C++ 依赖（gRPC / protobuf / absl）已内置在 `Source/ThirdParty`，**不需要**跑 `windows_dependencies.bat`。
> `Schola.uplugin` 无 `EngineVersion` 字段，不会因引擎小版本号被拒；但插件是源码，**必须重编项目**。

**Step 4 — Python 虚拟环境**
```powershell
$PY = "%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
& $PY -m venv "<project>\\.venv"
& "<project>\\.venv\Scripts\python.exe" -m pip install --upgrade pip
```
> 必须用**绝对路径**调用，避免命中 `%LOCALAPPDATA%\Microsoft\WindowsApps\python.exe` 这个 Store stub。

**Step 5 — 装 PyTorch（cu128，先装）**
```powershell
& "<project>\\.venv\Scripts\python.exe" -m pip install torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cu128
```
> RTX 5080 = sm_120，**必须** cu128 及以上；用默认 PyPI 会装成 CPU 版。

**Step 6 — 装 Schola Python 包**
```powershell
& "<project>\\.venv\Scripts\python.exe" -m pip install -e "<project>\\Plugins\Schola\Resources\python[sb3]"
```
> 先只装 `[sb3]`，**不装 `[all]`** —— RLlib / Minari 留到第 8 轮，避免现在就把依赖解析复杂化。
> 装完立刻复查 torch 是否被降级/换成 CPU 轮子（见 4.4 断言）。

**Step 7 — 生成 VS 工程文件**
```powershell
& "E:\Project\UE5\UE_5.7\Engine\Build\BatchFiles\GenerateProjectFiles.bat" -project="<project>\\PursuitAI.uproject" -game -engine
```

**Step 8 — 编译（首次约 20–40 分钟）**
```powershell
& "E:\Project\UE5\UE_5.7\Engine\Build\BatchFiles\Build.bat" PursuitAIEditor Win64 Development -Project="<project>\\PursuitAI.uproject" -WaitMutex
```
> 所有调用都用**绝对路径**，不依赖 PATH（本机 PATH 里没有任何 Epic/Unreal 条目）。

**Step 9 — 最小运行测试**
```powershell
# 9a 打开编辑器
& "E:\Project\UE5\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" "<project>\\PursuitAI.uproject"

# 9b 无渲染冒烟（第 6 轮会正式用，本轮先确认能起）
& "E:\Project\UE5\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" "<project>\\PursuitAI.uproject" -nullrhi -unattended -nosplash
```

### 4.3 成功判据（必须全部满足）

| # | 判据 | 检查方式 |
|---|---|---|
| S1 | `Build.bat` 退出码 0 | 命令返回码 |
| S2 | 产出 `<project>\\Binaries\Win64\UnrealEditor-PursuitAI.dll` | 文件存在 |
| S3 | 编辑器能打开项目、无崩溃 | 目视 + `Saved\Logs\PursuitAI.log` 无 `Fatal` |
| S4 | 项目设置 → Plugins 里 **Schola 显示已启用** | 编辑器 UI |
| S5 | 编辑器日志中**没有** "Schola ... built for another version of Unreal Engine" 告警 | 日志 grep |
| S6 | `python -c "import schola; print(schola.__version__)"` 成功 | venv 内执行 |
| S7 | `schola --help` 能列出子命令（`sb3` / `rllib` / `compile-proto` 等） | venv 内执行 |
| S8 | Torch 断言：`torch.cuda.is_available()==True`、设备名含 `RTX 5080`、capability `(12, 0)`、版本带 `+cu128` | 见 4.4 |

### 4.4 Torch 断言脚本

```powershell
& "<project>\\.venv\Scripts\python.exe" -c @"
import torch
print('torch      :', torch.__version__)
print('cuda build :', torch.version.cuda)
print('available  :', torch.cuda.is_available())
print('device     :', torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'N/A')
print('capability :', torch.cuda.get_device_capability(0) if torch.cuda.is_available() else 'N/A')
"@
```
期望输出示例：`2.x.x+cu128` / `12.8` / `True` / `NVIDIA GeForce RTX 5080` / `(12, 0)`

若 `torch.__version__` 不含 `+cu128`，或 `available` 为 `False` → 执行补救：
```powershell
& "<project>\\.venv\Scripts\python.exe" -m pip install --force-reinstall --no-deps torch --index-url https://download.pytorch.org/whl/cu128
```

### 4.5 失败判据（命中任一 → 停止并上报，不自行改 Schola 源码）

| # | 症状 | 含义 | 处置 |
|---|---|---|---|
| F1 | UBT 报 `No required compiler toolchain found in ...14.44.35207` | MSVC 工具集差一档（风险 R2 命中） | VS Installer 把 MSVC v143 升到最新，重试 |
| F2 | UBT 报找不到 Windows SDK / `windows.h` | SDK 补装未生效 | 复查 VS Installer 组件，或改 `BuildConfiguration.xml` 指定 SDK 版本 |
| F3 | 报 `Schola ... built for another version of Unreal Engine` | 引擎版本不符 | 确认引擎确为 5.7.x；清 `Binaries/` `Intermediate/` 后全量重编 |
| F4 | 报缺 `EnhancedInput` / `StateTree` / `GameplayStateTree` | 依赖插件未启用 | 在 `.uproject` 的 `Plugins` 段补上并重编 |
| F5 | `pip install` 依赖解析失败 | Python 依赖冲突（风险 R5） | 重建 `.venv`；退回逐个装；不改 Schola `pyproject.toml` |
| F6 | 编辑器崩溃 / Schola 模块未加载 | 插件编译或加载失败 | 读 `Saved\Logs\PursuitAI.log` + `UnrealBuildTool` 日志 |

### 4.6 本轮明确不做

- 不搭任何 1v1 环境、不写 Agent / Trainer 逻辑
- 不创建规则 AI 逃跑者
- 不接 SB3、不训练、不导出 ONNX
- 不建 GitHub 远程仓库、不 push

---

### 4.7 第 2 轮执行结果（2026-09-19）

**结论：第 2 轮完成，S1–S8 全部通过。**

| # | 判据 | 结果 | 证据 |
|---|---|---|---|
| S1 | `Build.bat` 成功 | ✅ | `logs/build_round2_nouba.log` → `Result: Succeeded`，`Total time in Parallel executor: 78.05 seconds` |
| S2 | 产出 `UnrealEditor-PursuitAI.dll` | ✅ | `Binaries\Win64\UnrealEditor-PursuitAI.dll`（91,136 B）+ `.pdb` 58.7 MB |
| S3 | 编辑器能打开项目、无崩溃 | ✅ | `logs/headless_smoke.log` → `LogInit: Display: Running engine for game: PursuitAI` … `LogExit: Exiting.`，无 `Fatal` |
| S4 | Schola 显示已启用 | ✅ | 同日志第 270 行 `LogPluginManager: Mounting Project plugin Schola` |
| S5 | 无引擎版本不匹配告警 | ✅ | 全日志 grep `built for another version` = 0 命中 |
| S6 | `import schola` 成功 | ✅（判据微调） | `schola.__version__` **不存在**；正确写法是 `schola --version` → `2.1.0` |
| S7 | `schola --help` 列出子命令 | ✅ | `build-docs` / `compile-proto` / `minari` / `rllib` / `sb3` |
| S8 | Torch CUDA 断言 | ✅ | `2.11.0+cu128` / cuda build `12.8` / `available=True` / `NVIDIA GeForce RTX 5080` / capability `(12, 0)` / matmul 实算通过 |

**额外确认**

- Schola 9 个模块全部产出 DLL（`Plugins\Schola\Binaries\Win64\`）：
  `Schola` / `ScholaEditor` / `ScholaNNE` / `ScholaProtobuf` / `ScholaTraining` /
  `ScholaInferenceUtils` / `ScholaInteractors` / `ScholaImitation` / `ScholaStateTree`
- 运行时自检命令可用：`PursuitAI.ScholaStatus` → `schola_module_loaded=true schola_plugin_version=2.1.0`
- `.sln` 已生成（`PursuitAI.sln`，172 KB），入口是 `tools/gen_project_files.ps1`
- Python 侧实际版本：`stable-baselines3 2.9.0` / `gymnasium 1.3.0` / `grpcio 1.84.0` / `protobuf 6.33.6` / `onnx 1.23.0` / `onnxscript 0.7.2`
- `schola sb3` 自带三个子命令：`train` / `eval` / **`export`（"Convert a StableBaselines 3 policy to ONNX for Unreal Engine"）** → 第 7 轮的 ONNX 导出不用自己写，直接用官方 CLI
- `.venv` 体积 **4.5 GB**（torch cu128 占绝大部分），已 gitignore

**版本号注意**：git tag 是 `v2.1.1`（commit `fa4743b`，2026-05-26），但插件描述符 `Schola.uplugin` 的 `VersionName` 和 Python 包版本都写的是 **`2.1.0`** —— AMD 没有为这个 patch 提版本号。日志里看到 `2.1.0` 属于正常，不代表装错。

**本轮新增的两个坑（已写进 `tools/` 与记忆）**

| # | 症状 | 根因 | 处置 |
|---|---|---|---|
| P1 | 编译报 `fatal error C1083: 无法打开预编译头文件 ... SharedPCH....h.pch: 拒绝访问`，同时满屏 `SetFileInformationByHandle (FileDispositionInfo) failed ... (Access is denied.)` | **UBA（Unreal Build Accelerator）** 的文件替换被拒，`.pch` 从未落盘。日志里 `[NoUba]` 重试的动作反而全部成功 | `Build.bat ... -NoUBA`。已验证 79 秒编完 221 个动作。注：此现象出现在 WorkBuddy 沙箱内执行的进程树上；在 VS / 普通 cmd 里大概率不需要 `-NoUBA`，但本机 + 沙箱组合下必须加 |
| P2 | 无法生成 `.sln` | **UE 5.7 删除了 `Engine\Build\BatchFiles\GenerateProjectFiles.bat`** | 直接调 UBT：`dotnet.exe UnrealBuildTool.dll -ProjectFiles -Project=<uproject> -Game -Engine -Progress`，封装在 `tools/gen_project_files.ps1` |

**新增/变更文件**

| 文件 | 说明 |
|---|---|
| `tools/check_env.ps1` | 引擎/VS/SDK/Python/GPU/磁盘一次性自检 |
| `tools/check_python_env.ps1` | S6–S8 断言（写临时 `.py` 再执行，不用 `python -c`） |
| `tools/gen_project_files.ps1` | 生成 `.sln`（UE 5.7 无 GenerateProjectFiles.bat） |
| `tools/smoke_headless.ps1` | 无渲染启动冒烟 + `-ExecCmds` + 超时自杀 |
| `PursuitAI.sln` | 已生成，gitignore |
| `Binaries/` `Intermediate/` `DerivedDataCache/` `Saved/` | 编译产物，gitignore |

---

## 5. 第 3 轮执行结果（2026-09-19）

### 5.1 结论

**第 3 轮完成：已建成一个完全自研的 C++ Schola 环境，并跑通 "UE 出环境 → gRPC → SB3 PPO 训练 → 回灌动作" 的完整闭环。**
全程**无需手工打开编辑器**（关卡由 Python 脚本无渲染生成），复现命令不超过 2 条。

### 5.2 交付物

| 文件 | 类型 | 说明 |
|---|---|---|
| `Source/PursuitAI/PursuitAIEnv.h` | 新增 | `APursuitAIEnv : public AGymConnectorManager, public ISingleAgentScholaEnvironment`。同时是"连接器管理器"和"环境本体"——挂在关卡里即自动被发现 |
| `Source/PursuitAI/PursuitAIEnv.cpp` | 新增 | 环境全部实现：观测构造、动作解码、奖励、随机化、调试绘制 |
| `Source/PursuitAI/PursuitAI.Build.cs` | 修改 | 依赖从 `Schola`/`Projects` 扩为 + `ScholaTraining` + `ScholaProtobuf` |
| `tools/gen_training_level.py` | 新增 | 无头生成训练关卡 `/Game/Maps/L_PursuitAITrain` 并 spawn 环境 actor，**幂等** |
| `tools/run_training.bat` | 新增 | **一键训练**（自动补关卡 + 跑 PPO），纯 ASCII + CRLF |
| `tools/open_tensorboard.bat` | 新增 | **一键看曲线**（TensorBoard @ localhost:6006） |
| `Content/Maps/L_PursuitAITrain.umap` | 新增 | 训练关卡本体，已入库（8.4 KB） |
| `Saved/UnrealBuildTool/BuildConfiguration.xml` | 新增 | 项目级关闭 UBA（见 5.5 P1）。**已 `git add -f` 强制入库** |
| `docs/SCHOLA_INTERFACE.md` | 新增 | Schola C++ 接口速查（三种环境接口、Space/Point 对应、三种模拟器、生命周期） |
| `docs/TUTORIAL.md` | 新增 | 新手教程：原理、操作步骤、看点、故障速查、后续路线 |
| `PursuitAI.uproject` | 修改 | 启用 `PythonScriptPlugin` + `EditorScriptingUtilities`（供无头关卡生成） |

### 5.3 环境契约（第 4 轮 1v1 的骨架）

```
        +X
         ^
         |        [T] target
         |      .
         |    .
         |  [A] agent ---> 动作 0 停 / 1 +X / 2 -X / 3 +Y / 4 -Y
         +-------------> +Y
```

| 项 | 值 |
|---|---|
| Agent ID | `"SingleAgent"`（Schola 对单智能体环境硬编码，见 `SingleAgentEnvironmentInterface.h`） |
| Observation | `Box(2)` —— 归一化的 `(target − agent) / ArenaHalfSize`，分量 ∈ [−1, 1] |
| Action | `MultiDiscrete([5])` —— 0 停 / 1 +X / 2 −X / 3 +Y / 4 −Y |
| Reward | 每步 `(上一步距离 − 本步距离) / MoveStep`（塑形）；捕获 **+10**；超时 **−1** |
| 终止 | 距离 ≤ `CatchRadius`(50cm) → `bDone`；`MaxSteps`(300) 用尽 → 截断 |
| 场地 | 正方形 `[−500, +500]` cm，出界即 clamp |
| 可调 UPROPERTY | `ArenaHalfSize` / `MoveStep` / `CatchRadius` / `MaxSteps` / `GoalReward` / `TimeoutPenalty` / `Seed` / `bDrawDebug` / `bLogEpisodes` |

> 本轮的 target 是**静止**的。这是刻意的：跑通了只可能是链路对，不可能是"AI 碰巧聪明"。
> 第 4 轮把 target 换成规则逃跑者，环境类本身不需要改接口，只改 `Step` 里的 target 更新。

### 5.4 复现步骤（两条命令）

```bash
# 1. 无头生成训练关卡（幂等，可重复跑）
"E:/Project/UE5/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" \
  "<project>/PursuitAI.uproject" -run=pythonscript \
  -script="<project>/tools/gen_training_level.py" -unattended -nosplash -nullrhi

# 2. 训练（自动 cook + 启动独立进程 + 连 gRPC + PPO）
cd "E:/Project/UE5+ai"
NO_PROXY="127.0.0.1,localhost" ./.venv/Scripts/schola.exe sb3 train ppo project \
  "<project>/PursuitAI.uproject" \
  --map /Game/Maps/L_PursuitAITrain --headless \
  --build-dir "<project>/Build/Staging" \
  --timesteps 60000 --enable-tensorboard --log-dir "<project>/logs/tb" \
  --disable-eval --no-pbar
```

- 用 **`project` 模拟器**（而非 `editor`）：它会自动 cook + 启动独立 `PursuitAI.exe`，因此**整个训练无人值守**。
  参数上 `use_cached_build=False`（不复用缓存），但实测若 `--build-dir` 指向的目录里已有产物则会跳过重编。
- `--headless` 对应独立进程的 `-nullRHI`，无窗口。
- **必须显式给 `--build-dir`**：不给的话 Schola 会退到 `%TEMP%\schola_build_<Project>`（即 **C 盘**），
  既违反"产物留在工作目录"的约定，也不利于复用缓存。

### 5.5 本轮新增的两个坑

| # | 症状 | 根因 | 处置 |
|---|---|---|---|
| **P1** | 编译报 `LNK1136: invalid or corrupt file` / `LNK1201`，且 `UnrealEditor-PursuitAI.lib` / `.pdb` 变成 **0 字节**；日志满屏 `SetFileInformationByHandle (FileDispositionInfo) failed … (Access is denied.)` | 还是 **UBA**：第 2 轮的 `-NoUBA` 只是命令行补丁，**`project` 模拟器内部调 UBT 时没有任何地方能传 `-NoUBA`** | 改为**项目级**配置 `Saved/UnrealBuildTool/BuildConfiguration.xml` → `<bAllowUBAExecutor>false</bAllowUBAExecutor>`。先手工删掉被写坏的 0 字节产物再重编。⚠️ 有效键是 `bAllowUBAExecutor`；`bUseUBA` **不是合法键**（UBT 会报 invalid child element），`bAllowUBALocalExecutor` 在 5.7 已废弃 |
| **P2** | C2398 窄化错误，`FVector2D` 赋给 `TArray<float>` | UE5 的 `FVector2D`/`FVector` 是**双精度**，`Box.Values` 是 `TArray<float>` | 所有参与观测/奖励的算术显式 `static_cast<float>` |

**附带发现（非致命，但影响日志干净度）**

本机有机器级环境变量 `HTTP_PROXY` / `HTTPS_PROXY` = `http://127.0.0.1:50684`（本地代理），而 `NO_PROXY` **未设置**。
后果有两个：
1. gRPC 建连时日志里出现一条 `HTTP proxy handshake with ipv4:127.0.0.1:50684 failed: … returned response code 502`（训练照样成功）；
2. `pip install tensorboard` 一开始被这个代理挡住，报"tensorboard not installed. Disabling tensorboard logging"。

→ 处置：跑训练前 `NO_PROXY=127.0.0.1,localhost`（已写进 5.4 的复现命令）。

### 5.6 成功判据

| # | 判据 | 结果 | 证据 |
|---|---|---|---|
| R3-1 | 环境 C++ 编译通过，无窄化/链接错误 | ✅ | `logs/build_round3_cfg_test.log` |
| R3-2 | 关卡可无头生成，环境 actor 落在关卡里 | ✅ | `logs/gen_level.log` → `spawned environment actor: PursuitAIEnv_0` |
| R3-3 | UE 侧自动发现环境 | ✅ | `LogScholaTraining: UAbstractGymConnector::CollectEnvironments(): Collected Environments PursuitAIEnv_0` |
| R3-4 | 环境定义被 Python 侧正确读取 | ✅ | `LogPursuitAI: PursuitAIEnv: InitializeEnvironment -> obs=Box(2)[-1,1], action=MultiDiscrete(5)`；连接器 = `RPCGymConnector` |
| R3-5 | gRPC 链路可通 | ✅ | 设 `NO_PROXY` 后无任何代理告警；4625 个 episode 正常往返 |
| R3-6 | PPO 训练跑完且策略确实在学 | ✅ | 见 5.7：成功率 99.74%，移动量 10927 cm → 300 cm |
| R3-7 | TensorBoard 可写 | ✅ | `logs/tb/PPO_0/events.out.tfevents.1789801169.<host>.11940.0` |

### 5.7 训练结果

**首次（`logs/train_round3.log`，无 TensorBoard）**

- 独立进程构建到 `Build/Staging/Windows/PursuitAI/Binaries/Win64/PursuitAI.exe`，启动 PID 14168
- PPO 跑在 `cuda`，**20480 timesteps / 49 s / fps 411**，`explained_variance 0.989`、`value_loss 0.062`
- UE 侧 `episode` 日志显示明确学习曲线：
  - 第 1 集：`TIMEOUT`，用满 300 步，累计移动 **11854 cm**
  - 第 24 集：`CAUGHT`，仅 48 步，累计移动 **1950 cm**
  - 总计 **CAUGHT 861 / TIMEOUT 8**（成功率 ≈ 99.1%）
- 移动距离从 ~11854 cm 收敛到 ~1950 cm，说明策略从"乱走到超时"变成"直奔目标"（最短路径理论值约 1900 cm 量级，取决于随机初始距离）

**第二次（`logs/train_round3_tb.log` + `logs/tb/`，带 TensorBoard、设 `NO_PROXY`）**

| 指标 | 值 |
|---|---|
| 总步数 | **61440** |
| 墙钟 | 152 s（PPO 段），**fps 403** |
| `explained_variance` | **0.996** |
| `mean_reward` | 16.7（min 13.6 / max 26.6） |
| `mean_steps` | **8.7 步/集** |
| 进程 | 独立进程 PID 30492 |
| **Fatal / Error 行数** | **0** |
| TensorBoard 事件 | `logs/tb/PPO_0/events.out.tfevents.1789801169.<host>.11940.0` |

**UE 侧 episode 统计（同一次运行）**

| 项 | 值 |
|---|---|
| 环境发现 | `Collected Environments PursuitAIEnv_0` |
| 空间定义 | `obs=Box(2)[-1,1], action=MultiDiscrete(5)` |
| 连接器 | `RPCGymConnector` |
| 总 episode | **4625** |
| **成功 / 超时** | **CAUGHT 4613 / TIMEOUT 12 → 成功率 99.74%** |
| 起始表现 | ep1 `TIMEOUT` 300 步 / 移动 10927 cm |
| 收敛表现 | ep4625 `CAUGHT` **6 步** / 移动 **300 cm** |

> 学到的策略就是最优解：在 50 cm/步 的离散动作下，直接沿曼哈顿路径贴向目标，
> 6~9 步内完成、移动量恰好等于初始曼哈顿距离。没有任何多余绕路。

**代理修复已验证**：设了 `NO_PROXY=127.0.0.1,localhost` 后，日志里
`HTTP proxy handshake … 502` **完全消失**，只剩下 SB3 那条无害的 GPU 提示。

### 5.8 补充记录

- `tensorboard 2.21.0` 已装入 `.venv`（事件目录 `logs/tb/`，已 gitignore）
- Python 侧新增依赖：`tensorboard` / `absl-py` / `markdown` / `werkzeug` / `tensorboard-data-server`
- **`project` 模拟器不放 `--build-dir` 时会落到 `%TEMP%\schola_build_PursuitAI`（C 盘）**。
  实测该目录已有产物时会跳过重编（本次 exe 复用，未重编），但路径不可控、且在 C 盘，
  因此复现命令里一律显式指定 `--build-dir`。

### 5.9 本轮明确不做

- 未写规则 AI 逃跑者（第 4 轮）
- 未做 1v1（第 4 轮）
- 未写多智能体接口（第 8 轮）
- 未导出 ONNX / 未做推理（第 7 轮）
- **未修改 Schola 任何源码**；**未建远程仓库、未 push、未上传**

---

