# PursuitAI — 开发环境要求

> 本文档记录本项目构建与训练所依赖的工具链版本。内容来自一次本机实测审计（UE 5.7.4 + Schola v2.1.1 组合可编译、可训练、可导出 ONNX），已移除审计时的主机身份、磁盘与内存部件编号等信息。

## A. 实测通过的组合

| 组件 | 版本 | 备注 |
|---|---|---|
| Unreal Engine | **5.7.4**（build `5.7.4-51494982`） | UE 5.8 不在 Schola 2.1.x 测试矩阵内，不要用 |
| AMD Schola 插件 | **v2.1.1** | 兼容矩阵：Schola 2.1.x ↔ UE 5.5–5.7 / Python 3.10–3.12 |
| Python | **3.12.x** | Schola 支持 3.10–3.12；本项目用 3.12 |
| PyTorch | **≥ 2.7.0，cu128 轮子** | Blackwell（sm_120）从 PyTorch 2.7.0 起才有官方预编译 kernel |
| Stable-Baselines3 | 随 Schola `[sb3]` extra 钉版本 | 不要自行指定版本 |
| Visual Studio | **2022 17.14+**（含 `NativeDesktop` / `NativeGame` 工作负载） | Epic 官方：UE 5.6 起推荐 17.14 |
| Windows SDK | **10.0.22621.0** 或更高 | 最低 10.0.19041.0 |
| MSVC 工具集 | v143 / **14.44.35207+** | Epic 官方最低 14.38.33130 |
| Windows | 11（64 位） | 不需要 Hyper-V / WSL / Docker |
| ONNX Runtime | 不需要装 | UE 侧走内置 NNE（`ScholaNNE`），Python 侧由 `schola sb3 export` 产出 |

## B. 安装顺序与注意点

1. 先装 VS 2022（含 C++ 桌面/游戏工作负载）与 Windows 11 SDK，再装 UE 5.7.4。
2. 把 `Plugins/Schola/` 放好后编译项目；插件是纯 C++ 源码，依赖 `EnhancedInput`、`StateTree`、`GameplayStateTree` 三个引擎插件，首次编译请留足 20–40 分钟。
3. Python 侧**强制使用 venv**，并且所有 pip 调用都用 venv 里的绝对路径解释器，避免命中 Windows Store 的 Python stub。
4. PyTorch 必须显式指定 cu128 索引，否则默认 PyPI 会装到 CPU-only 轮子：

```powershell
& .\.venv\Scripts\python.exe -m pip install torch --index-url https://download.pytorch.org/whl/cu128
```

## C. 已知风险与规避

| ID | 风险 | 应对 |
|---|---|---|
| R1 | UE 与 Schola 版本不匹配（装成 5.8 会出界） | 锁死 UE 5.7.4 + Schola v2.1.1；换引擎版本必须重编项目 |
| R2 | MSVC 工具集差一档时报 `No required compiler toolchain found` | VS Installer 里把 MSVC v143 升到最新后重编 |
| R3 | GPU 与 PyTorch CUDA 版本不匹配（`no kernel image is available`） | 用 cu128 轮子；装完断言 `torch.cuda.is_available()` |
| R4 | 内存吃满（编辑器 + 多实例并行） | 并行训练从 2–4 个 UE 实例起步，实测后再加 |
| R5 | Python 依赖互相踩版本 | 只用 venv；先装 `[sb3]`，不要装 `[all]` |
| R6 | Schola 插件编译失败 | 先确认三个依赖插件已启用；失败时看 `Saved/Logs/UnrealBuildTool`；不改 Schola 核心源码 |
| R7 | 训练数据与 checkpoint 占盘 | `checkpoints/` 与 `logs/` 设保留策略 |

## D. 事实边界

- 表中"实测通过"指本仓库代码在上述组合下完成过编译、训练、评估与 ONNX 导出；不同机器上的构建耗时和训练结果不保证一致。
- 审计阶段的主机信息、磁盘与内存部件编号属于私人信息，已从本文档移除。
- 未验证项：UE 5.7 的实际安装体积、Schola 在其他机器上的编译时间。
