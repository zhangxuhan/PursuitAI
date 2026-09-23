# 训练前后对比：可行性审计 + 对照台方案

> 状态：**§10 的 4 件事已拍板（A 方案），管道已实现并跑通最小验证**；**训练尚未跑**。
> 结论：方案**大体成立**，但审计挖出 **2 个会让对比失效的硬问题** 和 **3 个必须先知道的约束**。
> 不先解决第 4 节那两个，录出来的片子会**把"管道问题"剪成"训练结论"**。

---

## 0. 拍板结果（2026-09-21，A 方案）

| # | 问题 | 拍板 |
|---|---|---|
| 1 | "训练前"用哪个 | **随机策略 `-PursuitRandom`**。未训练 ONNX **不做主画面**（SB3 输出层 gain=0.01，实测 0.11 cm/步，冻住的点会被读成程序故障）；可**附 5 秒彩蛋**说明这件事 |
| 2 | 目标策略 | **Static**（`-PursuitStatic`）。两侧目标位置、出生点、环境状态完全一致。Flee **单独录**，不混进主对比 |
| 3 | 画面形式 | **先双窗**。左 `RANDOM BASELINE`、右 `TRAINED PPO 60K / ONNX`。四宫格暂不做，等双窗验证完再决定要不要补 Random/5k/20k/60k 学习过程 |
| 4 | 配色 | **改**。目标方向=黄，策略动作=蓝，实际轨迹=绿，捕获范围=红，捕获成功=大字 `CAUGHT`；**动作箭头不得与视线/轨迹同为绿色** |

**主片的准确表述（已固定，不得改写）：**

> 相同城市布景、相同出生状态、相同静止目标，左侧使用随机策略基线，右侧使用训练后的 PPO 策略。
> 城市在本阶段作为视觉演示环境，不参与障碍观测与碰撞训练。

### 0.1 本轮落地了什么（最小验证）

| 项 | 位置 | 状态 |
|---|---|---|
| 随机基线驾驶模式 | `EPursuitDriveMode::Random` + `-PursuitRandom`，**独立于 `Rng` 的 `ActionRng`**，均匀采球面（不是立方体，否则对角线方向会被多采 ~1.9 倍） | ✅ |
| 轨迹（绿） | `Trail` / `TrailMax` / `-PursuitTrailMax=`，`ApplyActionVector` 里 push，`BeginEpisode` 里清空 | ✅ |
| 配色 | 动作箭头 绿→**蓝**；被墙吃掉的那段 红→**橙**；捕获环/球 橙→**红**；新增**绿**色轨迹折线；图例同步改写 | ✅ |
| 大字 CAUGHT | `EpisodeMessage` + `MessageRemaining`，与定格同一时刻起落 | ✅ |
| 窗内烧标签 | `-PursuitPaneLabel=<text>`；**空格必须写成下划线**（`FParse::Value` 无论分隔符开关如何都在空格处截断），代码里换回空格 | ✅ |
| 同时开始 | `-PursuitStartAt=<unix epoch>` 闸门 + `WAITING FOR GO`；闸门期间**不累积**时间，慢的那一窗不会带着 backlog 起跑 | ✅ |
| 对照台脚本 | `tools/portfolio_pair.py`：选不被置顶窗压的位置 → 逐窗启动 → 按"不是左窗的那个"识别右窗 → 摆放 → **看像素**确认有画面 → 等闸门 → **单次并集抓屏** | ✅ |
| 一致性开关 | `-PursuitRate=` / `-PursuitMaxSteps=` / `-PursuitSeed=` | ✅ |
| **每回合重新播种** | `EpisodeIndex`；`Rng.Initialize(Seed + EpisodeIndex)`、`ActionRng.Initialize(Seed + 1000003 + EpisodeIndex)`；新增 `episode N started agent=... target=... gap=...` 日志 | ✅（**审计时漏掉的第三个硬问题**，见下） |

### 0.1.1 🔴 补一条：出生序列会随回合数分叉（实测，不是推理）

30 秒验证片的实测：右窗（贪心）**跑了 14 个回合**，左窗（随机）**还在第 1 个回合里**。
而 `Rng` 只在 BeginPlay 播种一次、之后顺序消耗 → **第 2 个回合起两窗的出生点就不同**，
"两侧出生点完全一致"只在第 1 回合成立。这不是 Static/Flee 能救的，是"流被消费的速率不同"。

修法：**按回合序号播种**。第 N 回合的出生布局与"对面跑得多快"无关。
实测两窗 episode 1 逐字一致：`agent=(186, 30, -21)  target=(307, -154, -134)  gap=247 cm`。

### 0.2 验证结果（`logs/portfolio/pair_verify_30s.mp4`，1904×720，30 s）

| 检查 | 结果 |
|---|---|
| 两窗像素确认 | mean 54.0 / 55.2，都不是黑屏 |
| `analyze_clip` 左右两半 | **都 PASS**（luma ≈ 53；flat 0.64，退化线 0.88） |
| 右窗（SCRIPTED GREEDY） | 30 s 内 **14 次 CAUGHT**，每回合 4–8 步 |
| 左窗（RANDOM BASELINE） | **0 次**，仍在第一个 300 步回合里 |
| 轨迹 | 左窗绿色像素 1337 → 8607（轨迹在累积）；右窗 1079 → 2773（直取目标） |
| 同时开始 | 两窗同一 epoch 解冻，片头 2 秒是两边完全相同的静止姿态 |

> 这一版右窗是**脚本贪心**而不是训练后的 PPO —— 训练还没跑，`checkpoints/policy.onnx`
> 是 Box(6)、与当前 Box(9) 不匹配。本片验的是**管道**：双窗、同时开始、出生一致、配色、轨迹、CAUGHT。

### 0.3 还没做的（下一轮）

1. **训练 60k**（§7 命令）→ `ppo_final.zip`；
2. `tools/export_all_ckpt.py` 批量导出 + `tools/onnx_add_tanh.py` 补 `Tanh`（🔴-1，不补则箭头方向最大偏 14.5°）；
3. ~~**城市舞台**~~ → **✅ 已做（2026-09-21，见 0.4）**；
4. 未训练 ONNX 的 5 秒彩蛋（要有同一次训练产出的 Box(9) 模型才有意义）。

### 0.4 城市舞台（2026-09-21 完成）

对比台**整体搬进 `Demonstration.umap`**，此后所有录制任务都以城市为舞台。不改编好的关卡、不建第二个
GameMode——`-PursuitEnvBox` 让 Demonstration 的 `PursuitPlayGameMode` 变成 env 的宿主：跳过追兵/玩法循环/玩法
HUD，改 spawn 一个 `APursuitAIEnv` 全权接管画面。

| 新开关 | 作用 |
|---|---|
| `-PursuitEnvBox` | PlayGameMode 跳过玩法搭建，spawn env；默认 pawn 降级为引擎隐身球；`-PursuitRunSeconds` 自退出仍生效 |
| `-PursuitStage=city` | env 不建自备地板与双灯（城市有自己的阳光），相机改**垂直俯视**（ arena 正上方 2900cm、FOV 34——与单窗 god 相机同一套实测参数，任何街道都无遮挡） |
| `-PursuitArenaAt=X,Y,Z` | 竞技场中心（逗号分隔，`FParse` 关分隔符截断）。仿真状态保持中心相对——观测、钳制、出生全部按 ArenaCenter 归一，换舞台不改任务语义 |
| `-PursuitArenaHeight=<cm>` | 板层半高（下限 10cm）；双窗用 60cm，球贴着路面 |

舞台坐标来自 `tools/inspect_city_stage.py`（无头实测）：最干净空地中心 **(-250, 0)**、地面 z=10、
距 PlayerStart 80cm，48 个地面遮挡物都不进 1150cm 见方的缓冲区。`tools/portfolio_pair.py`
默认地图已改为 Demonstration 并自动带上这组开关（`STAGE_FLAGS`），`--map` 可换回训练台。

验证片：`logs/portfolio/pair_city_30s.mp4`。

---

## 1. 一句话结论

| | 结论 |
|---|---|
| "训练前后对比"这件事 | **能做，而且比预想的更干净** —— ONNX 导出的是**确定性均值**，推理路径里**一个随机源都没有**，所以整段对比是**逐比特可复现**的 |
| 但"训练前 = 未训练神经网络" | **🔴 不成立**。实测未训练策略的动幅度是 **0.11 cm/步**（满舵 50），左窗会是一个**冻住的点**。必须改用**随机策略**做"训练前" |
| 但"导出的 ONNX = 训练出的策略" | **🔴 不成立**。导出图**漏了 tanh**，行动作方向最大偏 **14.5°** —— 而"箭头是否一致"正是你想秀的那个指标 |
| 你列的 9 条一致性 | **7 条天然满足**，2 条需加命令行开关（动作频率、最大步数），1 条**必须换目标策略**才能字面满足（目标位置），1 条需新增机制（同时开始） |
| 你列的 6 项叠加层 | **4 项已存在**（只有 2 项的颜色和你说的不同），缺 **1 项轨迹** 和 **1 项 CAUGHT 文字** |
| 你能拿到的最强指标 | `off-axis N deg`（动作方向与目标方向的夹角）**已经在 HUD 里逐帧在算** —— 建议加"回合平均"变成曲线 |

---

## 2. 审计：你列的 9 条"必须一致"

以 `APursuitAIEnv`（`Source/PursuitAI/`）为准，逐条核。

| # | 要求 | 现状 | 判定 |
|---|---|---|---|
| 1 | 相同地图 | 两进程各加载一次 `L_PursuitAITrain`；关卡里**只有 1 个 env actor**，地板/灯光/相机都由代码按 `ArenaHalfSize` 算出来 | ✅ 天然满足 |
| 2 | 相同出生位置 | `RandomizePositions()`（`.cpp:750`）**只用 `Rng`**，`-PursuitSeed=N` 已能固定（`.cpp:219`） | ✅ 但见下方 **⚠️-2** |
| 3 | 相同目标位置 | **出生点相同，但逃跑目标的位置是 agent 的函数** → 第 1 步之后必然分叉 | ⚠️ **只有 `-PursuitStatic` 能字面满足**，见 **⚠️-3** |
| 4 | 相同随机种子 | `-PursuitSeed=<n>`，已实测 nullrhi/windowed 逐字一致 | ✅ |
| 5 | 相同动作频率 | `DemoStepsPerSecond`（`.h:226`，默认 6）**目前无法从命令行覆盖** | ⚠️ 需加 `-PursuitRate=` |
| 6 | 相同最大步数 | `MaxSteps`（`.h:171`，默认 300）**同样不可覆盖** | ⚠️ 需加 `-PursuitMaxSteps=` |
| 7 | 相同摄像机角度 | 由 `ArenaHalfSize` 纯公式推导（`.cpp:1111-1118`），与进程无关 | ✅ 天然满足（窗口宽高比要一致） |
| 8 | 相同推理模式 | 若**两边都是 `-PursuitInference`**（只换 onnx 路径）→ 完全一致 | ✅ **推荐这条路**；若用"随机策略 vs ONNX"则字面不成立（见 **🔴-2** 的取舍） |
| 9 | 同时开始 | **没有任何机制** | ❌ 需加启动闸门 `-PursuitStartAt=<epoch>` |

---

## 3. 审计：你列的 6 项叠加层

好消息：`APursuitAIEnv::DrawDebug()`（`.cpp:1379`）**已经画了一大半**。

| 你要的 | 现状 | 位置 / 颜色 | 动作 |
|---|---|---|---|
| 黄色线：目标方向 | ✅ **已有** | `.cpp:1423` `FColor(255,210,0)`，长度 4×MoveStep，从 agent 出发 | 无 |
| 蓝色箭头：模型动作方向 | ⚠️ **已有但是绿色** | `.cpp:1435` `FColor(60,255,60)`，且**与视线绿线撞色** | **改蓝** `FColor(60,120,255)` |
| 绿色轨迹：实际路线 | ❌ **完全没有** | —— | **新增** `TArray<FVector>` 折线，`BeginEpisode` 清空，上限 ~300 点 |
| 红色圆环：捕获范围 | ⚠️ **已有但是橙色** | `.cpp:1447` 环 + `.cpp:1449` 线框球，`FColor(255,120,0)` | **改红** `FColor(230,40,40)`，球用同色更淡 |
| 目标头顶标记 | ✅ **已有** | `.cpp:1458` `TARGET   xxx cm`（红），另有 `CHASER #0  R +x.xx`（绿） | 无 |
| 捕获时显示 CAUGHT | ⚠️ **只有 0.75 s 定格**，env 里**没有 CAUGHT 文字** | `DemoEpisodePause`（`.h:230`）；文字版只在 `APursuitPlayGameMode` 里 | **新增** `Message` + 倒计时 + 大字 |

**两条你没提但应该加的：**

- **`-PursuitPaneLabel=<text>`** —— 在窗内左上角烧一行 `UNTRAINED` / `TRAINED 60k`。
  两个窗口标题**完全相同**（`PursuitAI （64-位 …）`），不烧字就只能在 ffmpeg 里后期叠，而后期叠字要额外保证对齐。
- **`-PursuitTrailStyle=<line|dots>`** —— 折线在 6 步/秒下太稀疏（每条腿 50 cm），点阵更能读出"路径形状"。试一版再定。

---

## 4. 🔴 两个会让对比失效的硬问题

### 🔴-1 导出的 ONNX **漏了 tanh** —— "箭头一致性"这个指标被污染

`Plugins/Schola/.../core/model.py`：`make_box_output()` 是 **identity**（原样返回）。实测证据：

```
predict(deterministic=True)   = [ 1.0000  -1.0000   0.1999]   ← tanh 压过之后
action_net raw (导出走的这条) = [ 3.1362  -1.7599   0.1999]   ← 这就是 ONNX 的输出
```

`policy.onnx` 的计算图（**5 个节点，无 Tanh**）：

```
Gemm → Clip(hardtanh) → Gemm → Clip(hardtanh) → Gemm(action)
```

**影响量化**：引擎侧 `ApplyActionVector` 只做 `GetClampedToMaxSize(1.0)`（按整矢量等比缩放，**不改方向**）。
拿上面这个样本算：

| 路径 | 缩放后方向 |
|---|---|
| 原始 logits 直接 clamp | `(0.871, -0.489, 0.056)` |
| 先 tanh 再 clamp（**SB3 的真实行为**） | `(0.720, -0.682, 0.143)` |

两者夹角 ≈ **14.5°**。也就是说：**一份瞄得完全正确、且决心拉满的策略，HUD 会写 `off-axis 14 deg`。** 你最有说服力的那个指标，量的是导出 bug，不是策略。

> **修法**（不碰 Schola 源码）：新增 `tools/onnx_add_tanh.py`，导出后给图**追加一个 `Tanh` 节点**（约 25 行 `onnx` API）。这与 SB3 的 `predict(deterministic=True)` 完全等价。
> 注意：tanh **不会**救 🔴-2（`tanh(0.002) ≈ 0.002`），两件事互相独立。

### 🔴-2 未训练的 SB3 策略**不会"乱走"，它几乎不动**

你的方案写"0步：完全乱走"。**实测不成立。**

镜像 Schola `train.py` 的构造方式（`net_arch pi=[256,256] vf=[256,256]`、ReLU）跑 200 个随机观测：

| seed | \|logits\| 均值 | 换算步距（MoveStep=50） | 300 步回合总漂移 |
|---|---|---|---|
| 0 | **0.0022** | **0.11 cm/步** | **~33 cm** |
| 7 | **0.0016** | 0.08 cm/步 | ~25 cm |

竞技场是 **1000 cm**。也就是说左窗是**一个几乎静止的点**（整个回合漂不到场地直径的 4%）。

**机制在源码里**（`stable_baselines3/common/policies.py:617-622`）：

```python
module_gains = {
    self.features_extractor: np.sqrt(2),
    self.mlp_extractor:      np.sqrt(2),
    self.action_net:         0.01,   # ← SB3 故意把输出层初始得极小
    self.value_net:          1,
}
```

这是 SB3 的**刻意设计**（初期动作接近零，避免初始乱跳）。后果：

1. 蓝箭头短到看不见（`.cpp:1435` 的箭头尺寸 `max(|Δ|*4*0.25, 12)` 只剩 12 单位的残桩）；
2. HUD 的 `off-axis` 在零矢量附近做 `GetSafeNormal()` → **数值疯狂抖动，无意义**；
3. 观众会把它读成"**管道坏了**"，而不是"策略没训练"——这比不做还糟。

> **修法（推荐）**：新增 `-PursuitRandom` 驾驶模式——每步从**独立于 `Rng` 的** `FRandomStream`（用同一个 `-PursuitSeed` 播种）采一个**满舵随机方向**。
> - 左窗变成真正意义上的混沌乱走，蓝箭头满舵乱指，`off-axis ≈ 90°` 且剧烈摆动 —— **正是你要的画面**。
> - 出生序列不受影响（见 §2 的 ⚠️-2），8 条不变量仍严格成立。
> - 代价：第 8 条"相同推理模式"**字面上不成立**（左边是基线、不是 ONNX）。**如实标注即可**——"随机策略基线 vs 训练后策略"本来就是最标准的对照。
>
> **不建议**把未训练 logits 乘个系数放大：那等于**伪造**一个任何真实策略都不会产生的行为。
> **但值得加录一条**：未训练 ONNX 那一窗留作"彩蛋"，配一句话——"未训练的 SB3 策略不是乱走，是几乎不动（gain=0.01 初始化），这本身是个值得知道的事实"。这比强行包装成"乱走"诚实得多，也更显功底。

---

## 5. ⚠️ 三个必须先知道的约束

### ⚠️-3 逃跑目标会让"相同目标位置"在**第 2 步就失效**

`ComputeEscapeDirection()`（`.cpp:676`）算的是"**离 agent 最远的方向**"，`StepTarget()`（`.cpp:712`）再按 `TargetEvadeGain`/`TargetEvadeTurnRate` 打弯。所以**目标的位置是 agent 轨迹的函数**——两个 agent 一动就不一样，第 1 步之后目标必然分叉。

这不是 bug，是闭环系统的本性。但你的第 3 条要求要成立，只有一条路：

```
-PursuitStatic     目标站住不动 → 整个环境与 agent 完全无关
                  → 两窗看到的是同一个世界，画面里唯一的差别就是那根蓝箭头
```

**这是最强控制变量的版本，也字面满足"相同目标位置"。** 建议：

- **主对比用 `-PursuitStatic`**（"同样的世界，只有策略不同"）；
- 另录一段 `-PursuitFlee` 作第二段素材，**明确说明"初始条件相同、之后目标会因策略不同而分叉"**。

> 顺带一个潜伏问题：`StepTarget()` 用 `DemoStepsPerSecond` 算目标转向的 `RInterpConstantTo`（`.cpp:734-736`）——
> 意思是**目标的转弯速率依赖"观战速率"**，而不是仿真速率。Static 下无关紧要，
> Flee 下会让"观战"与"训练"里的逃跑者行为不一致。**已记录，本轮不改。**

### ⚠️-4 Schola 的 SB3 CLI **没有 `--seed`**

`grep -rn seed schola/scripts/` 只在 `minari` 里有，SB3 的 settings 里**没有**。
而 SB3 侧（`base_class.py:566`）：

```python
def set_random_seed(self, seed=None):
    if seed is None:
        return          # ← 什么都不做
```

所以 `seed=None` 时**完全不设种**，策略初始化取进程的全局 torch RNG → **每次训练结果都不同**。

两个后果：
1. **训练本身不可复现**（要复现得在进程内先 `torch.manual_seed(N)`，CLI 给不了）；
2. **step-0 模型与训练运行的"血缘"无法通过 CLI 保证**（得让两个进程在构造策略前拥有相同的全局 torch RNG 状态）。

> **这恰好被 🔴-2 化解了**：既然 step-0 那一窗本来就冻住、不能当"训练前"，我们就不需要它的血缘。
> **检查点阶梯（5000/20000/60000）全部来自同一次训练运行 → 血缘天然一致。** ✅
> 若以后确实想要严格血缘，可以写一个 ~20 行的包装脚本在调 `main()` 前设 `torch/numpy/random` 三个种子（**不改 Schola 源码**），本轮不做。

### ⚠️-5 `--export-onnx` 在本机是**坏的**，必须事后导出

`tools/export_policy.bat` 头部已经记着：训练中导出会让 `torch.export` 踩到
`Unhandled FakeTensor Device Propagation for aten.mm.default` 并**把训练搞崩**。

> 所以检查点流程必须是：**训练只存 `.zip`** → 训练结束后**逐个** `schola sb3 export`（CPU）→ `.onnx`。
> 需要一个 `tools/export_all_ckpt.py` 批量循环。**绝不加 `--export-onnx`。**

---

## 6. 对照台架构：为什么用"单次抓屏"而不是 ffmpeg 拼接

你要求"同时开始"。**两个独立进程 + 分别录 + ffmpeg `hstack`** 有个根本弱点：
两次录制各有各的帧节拍，对齐只能靠"看起来同步"。

**改用：两个窗口并排摆好，只抓一次屏幕的并集区域。**

```
屏幕 1920×1080
┌────────────────────┬────────────────────┐
│  左窗 960×720       │  右窗 960×720       │
│  -PursuitRandom     │  -PursuitInference  │
│  (-PursuitStatic)   │  (-PursuitStatic)   │
│  同 seed / 同步率 / 同步数 / 同相机         │
└────────────────────┴────────────────────┘
        ↑ 一次 GDI 抓屏抓 (0,0,1920,720) → 直接落一个 mp4
```

**同一帧里天然同步**，不存在对齐问题、不需要 `hstack`、不需要担心帧数不等。

**"同时开始"的机制**：新增 `-PursuitStartAt=<unix epoch>`。两个进程起来后**冻结**（不推步、不走定格倒计时），
并在画面上打 `WAITING FOR GO`；启动器在 T−1 s 开始抓屏，到 T 两窗同时解冻。
→ 视频**开头 1 秒是两窗完全相同的静止姿态**，这本身就是"同时开始"的视觉证明（而且是免费得到的）。

**四宫格（中间态）**：同样逻辑，2×2 摆 4 个 960×540 窗口，并集正好铺满 1920×1080。
⚠️ 4 个 `UnrealEditor.exe -game` 同时跑（各约 1.5–3 GB + 一个渲染主线程）有资源风险；
**退路**：分两批各录 2 窗，再 `ffmpeg xstack` 拼——因为仿真是确定性的、且都用同一个 epoch 闸门，按步数对齐是精确的。

---

## 7. 检查点阶梯：你的 0/5000/20000/60000 怎么落地

Schola **自带** `--enable-checkpoints --checkpoint-dir --save-freq`（走 SB3 的 `CheckpointCallback`），
文件名形如 `ppo_20000_steps.zip`。

```bash
NO_PROXY="127.0.0.1,localhost" ./.venv/Scripts/schola.exe sb3 train ppo project \
  "<project>/PursuitAI.uproject" \
  --map /Game/Maps/L_PursuitAITrain --headless \
  --build-dir "<project>/Build/Staging" \
  --timesteps 60000 \
  --enable-checkpoints --checkpoint-dir "<project>/checkpoints/sweep" \
  --save-freq 5000 --name-prefix-override ppo \
  --save-final-policy \
  --enable-tensorboard --log-dir "<project>/logs/tb" \
  --disable-eval --no-pbar
```

**不加 `--export-onnx`**（⚠️-5）。这会产出 **12 个检查点**（5000…60000）+ `ppo_final.zip`。

| 你要的 | 实际落地 | 说明 |
|---|---|---|
| `checkpoint_000000.zip` | **没有，且不需要** | `CheckpointCallback` 从 `save_freq` 起才存；且它冻住不能当"训练前"（🔴-2） |
| `checkpoint_005000.zip` | `ppo_5000_steps.zip` | 第一次 PPO 更新后的样子 |
| `checkpoint_020000.zip` | `ppo_20000_steps.zip` | |
| `checkpoint_060000.zip` | `ppo_final.zip`（60000 步） | |

**四宫格取 4 个**：建议 `final(60k)` / `20000` / `5000` / `Random` —— 把"随机"放进四宫格，
四格讲的就是**完整学习过程**；只取 3 个模型 + 1 个随机会缺"中间态"的说服力。

---

## 8. 需要改的代码（清单）

估算 **~150 行**，全部集中在 `APursuitAIEnv`，**不动 Schola**。

### `Source/PursuitAI/PursuitAIEnv.h/.cpp`

| # | 改动 | 量 |
|---|---|---|
| 1 | `TArray<FVector> Trail` + `-PursuitTrailMax=<n>`；`ApplyActionVector` 里 push、`BeginEpisode` 里清空 | ~15 |
| 2 | `DrawDebug`：轨迹折线；动作箭头 **绿→蓝**；捕获环/球 **橙→红** | ~30 |
| 3 | `Message` / `MessageRemaining` / `bLastEpisodeCaught`；`FinishWatchedEpisode` 置位；`StepWatched` 倒计时；`DrawHud` 大字 `CAUGHT` / `TIMEOUT` | ~30 |
| 4 | `EPursuitDriveMode::Random` + `-PursuitRandom`；`WriteRandomAction()` 用**独立** `FRandomStream` | ~25 |
| 5 | `-PursuitStartAt=<epoch>` 启动闸门（`StepWatched` 里冻结 + `WAITING FOR GO` 提示） | ~15 |
| 6 | `-PursuitRate=` / `-PursuitMaxSteps=` / `-PursuitPause=` 三个覆盖 | ~12 |
| 7 | `-PursuitPaneLabel=<text>` 窗内左上角烧字 | ~10 |
| 8 | 回合平均离轴角 `off-axis` 滚动均值 + HUD 一行 | ~20 |

### `tools/`（不改引擎）

| # | 脚本 | 作用 |
|---|---|---|
| 9 | `tools/onnx_add_tanh.py` | 给导出的 ONNX 追加 `Tanh`，修 🔴-1 |
| 10 | `tools/export_all_ckpt.py` | 批量 `schola sb3 export` 遍历 `checkpoints/sweep/*.zip` |
| 11 | `tools/portfolio_pair.py` | 预检置顶窗 → 启 2/4 进程（各自 `-abslog`）→ 按新出现的 HWND 摆窗 → 等 epoch 闸门 → **单次并集抓屏** → 收尾杀进程并还原置顶窗 |

---

## 9. 录制环境的风险（本机实测过）

1. **🔴 `桌面歌词` 是 TOPMOST 窗口，位于 `(1364,346,1980,520)`**，正好压住右窗。
   GDI 抓屏**抓的是屏幕**，被盖住就录进去（已经踩过一次，视频右上角出现一块日文）。
   → 抓屏前枚举所有 `WS_EX_TOPMOST` 窗口，**与录制区相交的先把游戏窗挪开**。
   `tools/record_city.py` 已经把这步做进去了；**全屏的** TOPMOST（NVIDIA Overlay、
   Program Manager）跳过，因为它们覆盖一切、把它们当障碍会导致无处可放窗口，何况它们不绘制。
2. **两个游戏窗口标题完全相同**（都是 `PursuitAI （64-位 Development PCD3D_SM5）`）。
   → 不能按标题定位。做法：先启左窗、记下**新出现的** HWND 并摆位；再启右窗、抓**下一个新出现的** HWND。
3. **两个进程会抢写同一份 `Saved/Logs/PursuitAI.log`** → 各自加 `-abslog=<独立路径>`。
   这条对**单进程**也重要，理由见下面第 6 条。
4. **动画/渲染节拍不吃 dt 的部分**：真值仿真（`AgentPos = ClampToArena(AgentPos + Dir*MoveStep)`）是纯位置算术，与帧率无关，
   两窗推进的**步数**一致 → 画面同步。这一点之前已实测过（nullrhi 与 windowed 逐字一致）。
5. **🔴 PNG 抓帧会比请求的帧率慢一半，于是视频变成 2 倍速。**
   实测：请求 20 fps，`ImageGrab`+PNG 存的真实速度是 **10.1 fps**（600 帧花了 59.1 s），
   而旧代码把 600 帧按 **20 fps** 编码 → `duration = 30 s`。
   **一份 59 秒的动作被压成 30 秒播**，看起来像物理出问题，其实是录制管道的锅。
   → 已改 `tools/record_window.py`：`png` 路径改为**按实测有效帧率编码**；
   默认引擎换成 **`ffmpeg -f gdigrab`**（抓屏与编码同一个进程，帧带墙钟时间戳，
   **文件时长就是录制时长**，机器跟不上时掉帧而不是整体加速）。
6. **🔴 窗口出现 ≠ 场景可见，录到的是加载期黑屏。**
   `UnrealEditor.exe -game` **先建窗口再加载关卡**，Cartoon City 是 5.7 MB BuiltData 的流式关卡，
   中间那段是很长的黑屏。实测：按"窗口存在"就开录，30 s 的 720p clip 只有 **60 KB 纯黑**
   （`mean = [0,0,0]`，约 40 种颜色）。
   → 改等**日志里 game mode 自己的 BeginPlay 行**（`PursuitPlay:`），而不是等窗口或睡固定时长；
   同时每次运行用**独立 `-abslog`**——共用一份日志时上一轮的标记会让这个等待**立刻通过**（陈旧文本）。

---

## 10. 需要你拍板的 4 件事

| # | 问题 | 我的建议 |
|---|---|---|
| 1 | **"训练前"用哪个？** (a) 随机策略 `-PursuitRandom`（不满足第 8 条字面要求，但视觉最强）/ (b) 未训练 ONNX（9 条全满足，但**冻住**） | **(a) 做主线**，(b) 作为"彩蛋"附一句解释 |
| 2 | **目标策略** Static 还是 Flee？ | **主对比 Static**（字面满足"相同目标位置"），Flee 另录一段 |
| 3 | **做双窗还是四宫格？** | 先双窗（稳），再加四宫格；四宫格有 4 实例资源风险，可退成两批 + `xstack` |
| 4 | **要不要修那两处叠加层颜色**（箭绿→蓝、环橙→红）？ | 要。不改的话你要求的"蓝箭头/红圆环"对不上，且绿箭头与视线绿线撞色 |

**另需确认**：录像舞台**只能是 Cartoon City 的 `Demonstration.umap`**（项目约定，见 §12）。
这对本方案的**训练侧**有一个必须正视的后果——`APursuitAIEnv` 与城市地图是两条不同的路径，
不能直接互换。§12 把边界和两条出路写清楚了。

---

## 11. 建议推进顺序

1. 拍板 §10 的 4 件事。
2. 改代码（§8 的 1–8），编一次，**用 `-PursuitStatic -PursuitRandom` 空跑一遍**验证冻结/解冻、轨迹、CAUGHT 都对。
3. 写 `tools/onnx_add_tanh.py`，并用 `logs/_preflight_onnx.py` 的同一套断言复验"ONNX == predict(deterministic)"。
4. 跑训练（§7 的命令，~几分钟），产出 12 个 `.zip`。
5. 批量导出 13 个 `.onnx`（含 `ppo_final`），**每个都过一遍 tanh**。
6. 录双窗主对比 → 数指标（日志现成 + off-axis 曲线）→ 再做四宫格。
7. 指标拉不开差距就先解决训练，**不要用剪辑弥补结论**。

---

## 12. 录像舞台：`Cartoon_City_Free/Maps/Demonstration.umap`

> **项目约定：以后所有内容都基于这张图录。** 不是"优先"，是"只"。
> `L_PursuitAITrain` 是训练装置：它在世界原点自建一块素地面、自建灯光、自建正交相机，
> 录出来是一段灰盒子。第一次录的视频就是它，那是错的场景。

### 12.1 这条约定对**可玩内容**没有代价

城市地图走 `APursuitPlayGameMode` + `APursuitCharacter`（真 `ACharacter` +
`CharacterMovementComponent` + 碰撞），已经具备：4 个追逐者、`CHASER n  xxx cm` 头顶标签、
`CAUGHT by a chaser - n time(s) so far` 大字、`-PursuitAutoPlay` 自演奏、`-PursuitRunSeconds` 自退出。
障碍场与三座 100/200/300 cm 的塔**也在这张图上**（`tools/gen_city_obstacles.py`）。
一条命令录完：

```bash
./venv/Scripts/python.exe tools/record_city.py --seconds 30 --fps 30
```

它按顺序做五件事：启动城市自演奏 → 等窗口 → **等关卡加载完**（轮询本次独立日志里的
`PursuitPlay:`）→ 绕开置顶窗口 → gdigrab 实时录制 → 按标题关窗。
`--still <path>` 只抓一帧（同样的启动与摆窗），`--keep-open` 录完不关。

### 12.2 这条约定对**训练侧**有一个必须正视的边界

`APursuitAIEnv` 现在**跑不了城市地图**，原因有三条，都是结构性的：

| # | 事实 | 位置 |
|---|---|---|
| 1 | 它自建场景，且**写死世界原点**：地板 `SetWorldLocation(0, 0, -ArenaHalfHeight)`、灯光 `SpawnActor`、相机按 `ArenaHalfSize` 算相对位置 | `.cpp:1069-1118` |
| 2 | 移动是**纯位置算术** `ClampToArena(AgentPos + Dir*MoveStep)`，**从不查地图碰撞** | `.cpp` 移动段 |
| 3 | 观测是 `Box(9)`，**没有任何障碍信息** | `.cpp:65` |

第 2 条是关键：把 env 放进城市，追逐者会**穿墙直走**——渲染完美，碰撞不参与。
第 3 条意味着即使不穿墙，策略也**学不到绕障**（它看不见障碍）。
所以"把对比搬到城市"不是改个地图路径，是要给环境加能力。

### 12.3 两条出路

**出路 A（快，几小时）——把城市当布景，对比仍然有意义。**
把 `APursuitAIEnv` 放进城市的某个广场，**隐藏它自建的地板与灯光**，让城市当地面与背景。
追逃仍是原点那块 36 m 见方的平地追逐（穿墙、不加障碍），但：
- `off-axis`（动作方向 vs 目标方向）这个核心指标**完全有效**，它只依赖相对位置与相对速度；
- 出生点/种子/步数/相机（改成固定视角）全部可控；
- 画面里是卡通城市，不是灰盒子 —— 满足"所有内容基于 Demonstration"的形式要求。

**代价要如实说**：这个版本的对比**不能声称"策略学会了绕障"**，因为场景里没有会挡人的东西。
台词只能是"同样的世界，只有策略不同 —— 看箭头是否对齐"。

**出路 B（真，新工作流）——让环境具备城市能力。** 需要：
1. 观测扩容（`Box(9)` → `Box(9+N)`）：加前向/侧向的障碍距离条，或几条射线；
2. 移动改为**带扫掠的位移**（`SetActorLocation(..., bSweep=true)`）或对 `ECC_WorldStatic` 做前探，
   保持"位置式、不吃 dt、可复现"这条底线（**这是本项目最贵的一条约束**，见 §2 第 1 条注释）；
3. 训练地图换成城市（或一个城市风格的训练关卡），并重训 —— 旧模型全部作废。
这是一条**独立的工作流**（"在城市里训练"），不是录制环节的改动。

### 12.4 建议

- **短期**：走 A —— 先把"训练前后对比"这条片子做出来（因为它本来就是要证明"策略学会了直取目标"，
  这个结论在平地上完全成立，而且**更干净**：没有障碍这个混淆变量）。
- **中期**：走 B，作为作品集里**另一段**内容（"同一个策略，在有障碍的地图里"）。
- **两段的舞台都是城市**，只是 A 的城市不参与物理。**片中要标清楚是哪一种**，
  否则观众会以为追逐者在绕楼。

---


| 文件 | 作用 |
|---|---|
| `logs/_preflight_onnx.py` | 不启 UE 的预检：ONNX 是否等于 `predict(deterministic=True)`；未训练策略的动作幅度 |
| `docs/PORTFOLIO_RECORDING.md` | 上一轮的录像方案（第 0 节记录了 Box(6)/Box(9) 不匹配那次事故） |
| `docs/PLAY_MAP.md` | 跳跃/障碍/卡死的实测记录 |
| `checkpoints/policy.onnx` | 现有模型（**Box(6)，与当前 Box(9) 不匹配，跑不动**） |
