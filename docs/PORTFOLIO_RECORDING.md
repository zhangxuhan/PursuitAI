# 作品集：训练前后对比录像方案

> 结论先行：**管道基本都在了，但"已训练模型"那一环是坏的（见第 0 节），必须重训。**
> 真正耗时的只有训练本身，而按已有实测（61440 步 / 152 s），20 万步约 8 分钟。
>
> 🔴 **2026-09-20 后续审计推翻了本文件第 2 节的推荐做法**，并给出了完整的对照台设计：
> 见 **`docs/COMPARISON_PLAN.md`**。要点：
> ① 导出的 ONNX **漏了 tanh**，行动作方向最大偏 14.5°；
> ② **未训练 SB3 策略不是"乱走"而是几乎不动**（实测 0.11 cm/步，满舵 50），
> 所以本文第 2 节的"零梯度未训练模型当训练前"**视觉上不成立**，应改用随机策略基线。

---

## 0. 🔴 2026-09-20 实测发现：现有 ONNX 与观测空间不匹配，推理路径已静默失效

第一次真正录视频（`preview_inference_test.mp4`）时暴露出来的。**画面会渲染、HUD 会画、
不崩溃、不报红字弹窗** —— 唯一的症状是**场景完全不动**：
`Step 0/300`、`0.0 steps/s`、`0 caught / 0 done`，四帧（t=0/6.5/13/19.5s）HUD 逐字相同。

引擎日志里每一步都刷同一行错误（本次 400 帧录下来 1246 次）：

```
LogNNERuntimeORT: Error: Binding input tensor 0 size does not match size given by
                        tensor descriptor (got 36, expected 24).
LogScholaNNE:     Error: NNEPolicy::Think(): Model inference failed
LogScholaInferenceUtils: Error: USimpleStepper::Step(): Policy failed to think!
```

**36 字节 = 9 个 float，24 字节 = 6 个 float。** 两处独立证据一致：

| 来源 | 观测维度 |
|---|---|
| `policy.onnx` 的图输入 | `['batch_size', 6]` |
| `ppo_final.zip` 的 `observation_space` | `Box(..., (6,))` |
| 当前 `APursuitAIEnv::Define()` | **Box(9)** |

即：这份策略是**观测还是 Box(6) 的时代**训练并导出的；
后来观测扩展成 **Box(9)**（加上相对速度那一步），
**ONNX 从没重新导出** → 引擎侧推理从那次改动起就一直失败。

影响面（三件事要一起改口径）：

1. 第 3 轮"成功率 99.74%"是 **Box(6) 时代**的实测，仍然有效，但**不能用当前代码复现**。
2. 作品集第一段"训练后"片段**现在拿不到** —— 必须重训出一个 9 维模型再导出。
3. **教训（值得写进流程）**：观测维度是引擎与模型之间的隐式契约，
   目前**没有任何机制拦住"引擎改了观测、模型没跟"**。
   静态图片（`--still`）**永远发现不了这个 bug**（场景不动，单帧照样好看）——
   **只有录视频才能发现**。建议在 `Define()` 旁边加一条断言或启动自检：
   加载 ONNX 后比对 `obs 维度 × 4 == input tensor bytes`，不符就直接报错退出。

> 复现命令：`tools/watch_inference.bat`，然后看 `Saved/Logs/PursuitAI.log` 里
> `Binding input tensor 0 size does not match` 的行数。

---

## 1. 现状盘点：已经有什么

| 能力 | 在哪 | 状态 |
|---|---|---|
| 三种驱动模式 | `EPursuitDriveMode`：`Train` / `Inference` / `Demo` | ✅ |
| 引擎内 ONNX 推理 | `UNNEPolicy` + `USimpleStepper`，无 Python | ✅ |
| 已训练模型 | `checkpoints/policy.onnx`（284 KB，**6 维输入**） | ❌ **与当前 Box(9) 不匹配，跑不动**（见第 0 节） |
| 未训练模型 | —— | ❌ **缺这一环** |
| 训练产物 | `checkpoints/ppo_final.zip` | ✅ |
| 导出 SB3 → ONNX | `tools/export_policy.bat` | ✅ |
| 逃跑者策略 | `-PursuitFlee` / `-PursuitStatic`（`TargetPolicy`） | ✅ A/B 实测过 |
| 确定性复现 | `-PursuitSeed=<n>` | ✅ 实测 nullrhi/windowed 逐字一致 |
| 干净画面 | `-PursuitVisual` / `-PursuitNoDebug` | ✅ |
| 录像 | `tools/record_window.py` → mp4（Pillow GDI + ffmpeg） | ✅ |
| 观战入口 | `watch_demo.bat`（脚本策略）/ `watch_inference.bat`（已训练） | ✅ |

**两个逻辑缺口，不是一个**（原版只写了第二个）：

1. **现有 ONNX 与观测空间不匹配**（第 0 节）→ 必须先**重训一轮**才能拿到"训练后"片段。
2. `Demo` 是**手写的贪心策略**，不是"未训练的神经网络"。
   拿它跟训练好的策略对比，讲的是"脚本 vs 学习"，**不是"训练前后"**。
   作品集里这个区别会被追问，所以必须补上真正的 step-0 模型。

两者可以合并成一次动作：**重训**（产出 9 维的"训练后"模型）+
**零梯度保存**（产出 9 维的"训练前"模型），见第 2 节。

---

## 2. 补缺：未训练模型（约 20 行）

> 🔴 **2026-09-20 实测修正：这条路线保留为"技术记录"，但不要用作"训练前"那一窗。**
>
> 做法本身可行：SB3 可以在**不训练**的情况下直接保存初始化后的权重：
>
> ```python
> from stable_baselines3 import PPO
> model = PPO("MlpPolicy", env, seed=0, verbose=1)
> model.save("checkpoints/policy_untrained")   # 零梯度步
> ```
>
> **但它不会"乱走"。** 实测（镜像 Schola 的 `net_arch pi/vf=[256,256]` + ReLU，
> 200 个随机观测）：`|logits|` 均值 **0.0022**，换算 **0.11 cm/步**（满舵 50），
> 一个 300 步回合总共只漂 **~33 cm**，而场地是 **1000 cm** —— 画面里就是一个**冻住的点**。
>
> 机制在 `stable_baselines3/common/policies.py:617-622`：SB3 把 `action_net`
> 输出层用 **ortho init gain=0.01** 初始化（刻意让初期动作接近零）。`tanh` 也救不了
> （`tanh(0.002) ≈ 0.002`）。
>
> 后果不止"不好看"：蓝箭头会短到只剩 12 单位的残桩，
> 而 HUD 的 `off-axis` 是在零矢量附近做 `GetSafeNormal()` → **数值乱抖、无意义**。
> 观众会把它读成"管道坏了"。
>
> **→ 改用随机策略基线 `-PursuitRandom` 当"训练前"**，设计见 `docs/COMPARISON_PLAN.md` §4/§6。
> 未训练 ONNX 那一窗仍值得录一条，但**换一种讲法**：
> "未训练的 SB3 策略不是乱走，是几乎不动 —— 这本身就是个值得知道的事实"。

为什么这样最干净：

- **观测/动作空间必然一致** —— `Define()` 是唯一描述空间的地方，
  所以"训练时一套布局、推理时另一套"这个经典静默故障在这里**结构上不可能发生**。
- 两次录制**只差权重**，架构、种子、起点、相机全部相同。这是唯一能经受追问的控制变量。
- 不需要改引擎代码，也不需要新的 DriveMode —— 用 `-PursuitInference` +
  换 `InferenceModelPath` 指向未训练那个 onnx 即可。

---

## 3. 三段式录像结构

### 第一段：核心对比（最有说服力）

未训练 ONNX **vs** 已训练 ONNX，同种子、同起点、同相机、同回合数。

指标（全部能从现有日志里数出来，不用新增埋点）：

| 指标 | 来源 |
|---|---|
| 捕获步数 中位数 / 最小 / 最大 | 每回合结束那行日志 |
| 成功率（CAUGHT / 总回合） | 同上 |
| 超时率 | 同上 |
| 每回合行走距离 | 已有（第 3 轮实测用过） |

画面：两段各 20 s，ffmpeg `hstack` 左右并排，左上角各打一行字。

### 第二段：难度阶梯（解释"训练到底学没学到东西"）

对手从 `Static`（静止靶）→ `Flee`（会跑的靶）两档，展示策略在更难环境下的退化。

参考数据（已实测，可直接引用）：

| | Static | Flee |
|---|---|---|
| 捕获步数 中位数 | 9 | **18** |

这一段能讲的东西：**静止靶上学会的东西不会迁移到会跑的目标** ——
这正是"第一阶段先做规则逃跑者"的理由，也是项目设计里最该被看到的一步。

### 第三段：泛化失败（加分项，面试最容易被追问的点）

把在 `L_PursuitAITrain`（1000×1000 空地）上训出的策略，**拿到 Cartoon City 上跑**。

**预期它会失败。** 理由：9 维观测是 `[相对位置, 自身位置, 相对速度]`，
**里面没有任何障碍信息**，策略在训练时从没见过墙。

这段比一个漂亮的成功案例更值钱 —— 因为它证明的是：

- 知道自己的观测空间边界在哪
- 能说清"为什么不行"：缺的是障碍感知（射线/占用网格），不是训练不够
- 知道要改什么：观测维度从 9 扩到 9 + N 条射线，重训

> 诚实提醒：如果第三段调不好，**宁可删掉也不要剪成"看起来能跑"**。
> 作品集里一个被看穿的过度包装，代价远大于少一段。

---

## 4. 决定成败的几个技术点

1. **确定性配对是前提。** 两次录制必须同种子同起点。`-PursuitSeed=<n>` 已有，
   且已实测「一次 `-nullrhi`、一次 `-windowed`」逐字一致 —— 基础是可靠的。
2. **录制时窗口必须在最前。** `record_window.py` 走 GDI 抓屏，被遮挡就录到别的东西
   （脚本会自己前置窗口，但录制中别切走、别让弹窗盖上来）。
3. **HUD 建议留着。** `-PursuitNoDebug` 能出干净画面，但作品集里**留 debug 面板更好**：
   奖励、距离、步数直接摊在屏幕上，观众不用信作者的话，自己看得见数字在变。
4. **`DemoStepsPerSecond` 要调。** 默认 `6` 是给人看单步决策的，太慢；
   作品集调到 **12–20**，否则一个回合要放十几秒。
5. **编码统一。** 两段录制必须同 `--fps`（建议 30 或 60），否则并排时不同步。
6. **`-PursuitRunSeconds=<n>` 可自杀退出**，适合无人值守批量录，不用手点关窗。

---

## 5. 录制命令骨架

```bash
# 1. 未训练策略（换 onnx 路径即可）
UnrealEditor.exe PursuitAI.uproject /Game/Maps/L_PursuitAITrain -game -windowed \
  -resx=1280 -resy=720 -PursuitInference -PursuitFlee -PursuitSeed=7 -PursuitVisual

# 2. 单独一个进程录像（窗口须在最前）
./.venv/Scripts/python.exe tools/record_window.py --seconds 20 --fps 30 \
  --output logs/portfolio/before.mp4

# 3. 已训练策略：同一条命令，只把模型换成 checkpoints/policy.onnx
```

---

## 6. 工作量

| 项 | 量级 |
|---|---|
| 未训练模型导出脚本 | 小（~20 行，复用现有 export 流程） |
| 录制包装 bat（两次录制 + 文件名规约） | 小 |
| 左右并排拼接 | 小（ffmpeg `hstack` 一条命令） |
| 调 `DemoStepsPerSecond` / 挑种子 / 挑回合 | 中（这是"挑出好看的 20 秒"，需要试几轮） |
| **真实训练一轮** | 20 万步 ≈ **8 分钟**（按 61440 步 / 152 s 外推） |

---

## 7. 建议的推进顺序

1. **先补未训练模型**（约 20 行）—— 没有它，第一段对比不成立。
2. **录前后两段**，同种子，只差权重。
3. **数指标**（日志现成）。如果训练前后指标拉不开差距，先解决训练，
   再谈录像 —— 录像不能替代结论。
4. 补第二段（Static / Flee 阶梯）。
5. 第三段（泛化失败）作为可选加分项，做不出来就明确不做。
