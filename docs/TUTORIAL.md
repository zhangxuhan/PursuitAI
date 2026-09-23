# PursuitAI 教程 — 从零到「训练 → 导出 → 引擎内推理」

> 面向第一次接触本项目的人。读完这一篇，你能自己把整条链路跑一遍，
> 直到**关掉 Python，让训练好的模型在 Unreal 里自己飞**。
>
> 当前状态：**第 4 轮已完成**。3D 连续控制的追击任务、训练与推理共用的契约层、
> ONNX 引擎内推理全部跑通。

---

## 0. 一句话原理

**Unreal 里跑「游戏世界」，Python 里跑「神经网络」。两边用一个明文协议（gRPC）对话。**

- Unreal 每走一帧，把**观测**（追击者与目标的位置）发给 Python；
- Python 用当前策略算出**动作**（三维方向），发回 Unreal；
- Unreal 执行动作、算出**奖励**，下一帧再发新的观测。

神经网络只看到「观测 → 动作 → 奖励」这三个数字，完全不知道 Unreal 的存在。这就是标准的
强化学习闭环（Gym 风格）。

```
                    ┌──────────────── Unreal 进程 ────────────────┐
                    │                                             │
   ┌──────────┐     │   APursuitAIEnv  (一个 AActor)               │
   │ 关卡      │────►│     是「环境」：定义观测/动作空间、算奖励、判终止  │
   │ L_Pursuit│     │     是「连接器管理器」：持有 URPCGymConnector  │
   │ AITrain  │     │     也是「智能体」：实现 IAgent（推理侧用）      │
   └──────────┘     └───────────────────┬─────────────────────────┘
                                        │
                             gRPC (127.0.0.1:8000)
                     观测 / 奖励 / done ──►│◄── 动作
                                        │
                    ┌───────────────────┴─────────────────────────┐
                    │ Python 进程（.venv）                          │
                    │   schola.sb3.env  ──► Gymnasium 兼容包装       │
                    │   Stable-Baselines3 PPO ──► PyTorch 策略网络    │
                    └─────────────────────────────────────────────┘
```

**这里没有一行代码是自己写的通信协议**：gRPC 桥、Gym 包装、PPO 算法、并行/评测/ONNX 导出
全部来自 AMD Schola 插件 + Stable-Baselines3。我们要写的只是**那个「游戏世界」**。

### 第 4 轮多出来的一件事：训练完的模型不需要 Python

上面那条 gRPC 线是**训练**用的。训练结束后把策略导出成 ONNX，Unreal 用引擎自带的 NNE
直接跑它 —— 此时 Python 完全不在场：

```
   checkpoints/ppo_final.zip  ──(schola sb3 export)──►  checkpoints/policy.onnx
                                                                │
                                            APursuitAIEnv 从磁盘读字节
                                            UNNEPolicy + USimpleStepper
                                                                │
                                                    -PursuitInference 跑起来
```

这就是「部署」两个字的具体含义，也是第 9 轮演示视频要拍的东西。

---

## 1. 这个环境在做什么

一个**会飞的追击者**在一个盒子里抓一个静止的靶子。

```
        +Z
         ^        ArenaHalfHeight = 250
         |
         |     ·  [T] 目标（静止，任意位置）
         |   ·
         | ·
    [A] ●───────────────────────► 动作：三维方向向量，长度被夹到 1
         |
         +------------------------► +X / +Y     ArenaHalfSize = 500
```

| 项 | 值 | 说明 |
|---|---|---|
| 观测 | `Box(6)` | `[0..2]` = `(目标 − 追击者) / 1000`；`[3..5]` = `自身位置 / 500`。都在 [−1, 1] 内 |
| 动作 | `Box(3)` | 三维方向，每个分量 ∈ [−1, 1]。长度先夹到 1，再乘 `MoveStep = 50` cm |
| 场地 | 1000 × 1000 × 500 cm | X/Y 是 ±500，Z 是 ±250（是个扁盒子，不是立方体） |
| 奖励 | 每步 | 靠近给正分、走远给负分：`(上一步距离 − 这一步距离) / 50` |
| 奖励 | 抓到 +10 | 距离 ≤ `CatchRadius = 50` cm |
| 奖励 | 超时 −1 | `MaxSteps = 300` 步还没抓到 |
| 终止 | | 抓到 → 成功；300 步 → 截断 |

**为什么动作是连续的、目标却是静止的？** 这是刻意的取舍。

- **连续动作**是因为二维离散动作（上下左右）在三维里会立刻退化成「先走 X 再走 Y 再走 Z」的
  阶梯运动，学出来的是三条直线拼起来的折线，不是追击。
- **靶子静止**是因为这一轮要证明的是**链路**而不是「AI 有多聪明」。靶子不动，那么高成功率
  **只可能**说明观测/动作/奖励的约定完全正确。会跑的靶子（规则 AI 逃跑者）是后面的事。

### 领域随机化

每一集开局，追击者和目标的位置都是**在整个盒子里随机撒的**（含 Z 轴），并且保证两者相距
至少 `CatchRadius × 4`。所以模型学到的是「从任意位置飞过去」，
而不是背下某一条固定路线。

---

## 2. 前置条件（已经装好了，仅供核对）

| 组件 | 位置 / 版本 |
|---|---|
| Unreal Engine | `E:\Project\UE5\UE_5.7`（5.7.4） |
| 本项目 | `E:\Project\UE5+ai` |
| Python 虚拟环境 | `<project>\\.venv`（3.12.10；torch 2.11.0+cu128 / SB3 2.9.0 / tensorboard 2.21.0） |
| Schola 插件 | `<project>\\Plugins\Schola`（v2.1.1） |

自检（想确认没坏就跑这两条）：

```powershell
& "<project>\\tools\check_env.ps1"
& "<project>\\tools\check_python_env.ps1"
```

> **两条铁律，忘了就会出怪错：**
> 1. **不要移动或改名 `Plugins\Schola`。** Python 包是 editable 安装，直接指向那个目录，
>    一动 `import schola` 立刻失效。
> 2. **不要删 `Saved\UnrealBuildTool\BuildConfiguration.xml`。** 那里面关掉了本机有问题的
>    UBA 构建加速器，删了会立刻链接失败（详见第 6 节）。

---

## 3. 操作步骤

### 方式 A：一键脚本（推荐）

```
第 1 步  双击 <project>\\tools\run_training.bat
第 2 步  双击 <project>\\tools\export_policy.bat
第 3 步  双击 <project>\\tools\watch_inference.bat
```

第 1 步跑完，`checkpoints\ppo_final.zip` 是训练好的策略；
第 2 步把它转成 `checkpoints\policy.onnx`；
第 3 步开一个 1280×720 的窗口，**不启动任何 Python**，让这个 ONNX 模型自己驱动游戏。

**只想先看看训练过程**：第 1 步换成 `tools\watch_training.bat`。
它和 `run_training.bat` 是同一次训练，只是**开着窗户跑**，能看见两颗粒子在里面追。

```
双击 <project>\\tools\watch_training.bat          20000 步
tools\watch_training.bat 50000                           5 万步
```

训练曲线用 `tools\open_tensorboard.bat`，浏览器打开 <http://localhost:6006>。

**全程不需要打开编辑器，不需要点任何按钮。**

> 实测：61440 步 ≈ 150 秒（400 步/秒）、`explained_variance 0.996`、`mean_steps` 个位数、0 行错误。
> 带窗口会慢一点（约 300 步/秒），细节见下面「看训练过程」一节。

**每次启动都有一段「看起来卡住了」的时间，这是正常的。** `schola` 在跑训练之前会先做一遍
`BuildCookRun`（编译 + Cook，Cook 是大头），几分钟起步。这段时间里**既没有窗口也没有进度输出**，
`logs\visual_train.log` 也是空的 —— 不是死了。Cook 完窗口才会弹出来，然后才开始刷 episode。
想确认它真的在干活，看这两个进程：

```powershell
Get-CimInstance Win32_Process -Filter "Name='dotnet.exe'" | Select ProcessId, CommandLine
# 出现 BuildCookRun ... -cook -FastCook 就是在 Cook
```

### 方式 B：手动三条命令

**第 1 步 — 生成训练关卡**（幂等，重复跑也没事）

```bash
"E:/Project/UE5/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" \
  "<project>/PursuitAI.uproject" -run=pythonscript \
  -script="<project>/tools/gen_training_level.py" \
  -unattended -nosplash -nullrhi
```

成功标志：日志里出现 `spawned environment actor: PursuitAIEnv_0`
（关卡若已存在会打印 `environment actor already present (1)`）。
生成到 `Content/Maps/L_PursuitAITrain.umap`。

**第 2 步 — 训练**

```bash
cd "E:/Project/UE5+ai"
NO_PROXY="127.0.0.1,localhost" ./.venv/Scripts/schola.exe sb3 train ppo project \
  "<project>/PursuitAI.uproject" \
  --map /Game/Maps/L_PursuitAITrain \
  --headless \
  --build-dir "<project>/Build/Staging" \
  --timesteps 60000 \
  --save-final-policy --checkpoint-dir "<project>/checkpoints" \
  --enable-tensorboard --log-dir "<project>/logs/tb" \
  --disable-eval --no-pbar
```

参数逐个解释：

| 参数 | 作用 | 能不能省 |
|---|---|---|
| `project` | 用哪个模拟器。会**自动编译 + 启动独立进程**，所以能无人值守 | 换 `editor` 就必须每次手动点 Play |
| `--map` | 加载哪个关卡 | 不能省 |
| `--headless` | 无窗口，给进程传 `-nullRHI`。引擎没有渲染器，环境也就自动不搭景 | 想看画面就**别加**，这就是 `watch_training.bat` 和这边的唯一区别 |
| `--fps` | 固定**物理时间步长**（源码原话：*Fixed FPS ... if None no fixed timestep is used*） | 本环境没有物理，加不加都一样，实测不影响速度 |
| `--build-dir` | 编译产物放哪 | **强烈建议给**。不给会落到 `%TEMP%`（C 盘） |
| `--timesteps` | 总训练步数。6 万步 ≈ 2.5 分钟 | 可以调 |
| `--save-final-policy` | 训练结束保存策略 | **别省**。默认是 `False`，省了就等于白训 |
| `--checkpoint-dir` | 策略存哪。默认是 `ckpt` | 建议给，写进 `checkpoints\` 好找 |
| `--enable-tensorboard` / `--log-dir` | 写曲线数据 | 可以省 |
| `--disable-eval` / `--no-pbar` | 不额外评测 / 不刷进度条 | 可以省 |
| `NO_PROXY=...` | 绕开本机的本地代理 | **别省**，否则会有 502 告警 + tensorboard 装不上 |

> **`--export-onnx` 故意没加。** 它会在**训练中途**导出，而那时策略还在 GPU 上，
> `torch.export` 会撞上 `Unhandled FakeTensor Device Propagation for aten.mm.default`
> 直接崩掉整轮训练。导出放到第 3 步单独做，问题就消失了。原因见 `export_policy.bat` 注释。

**第 3 步 — 导出 ONNX**

```bash
NO_PROXY="127.0.0.1,localhost" ./.venv/Scripts/schola.exe sb3 export \
  --policy-checkpoint-path "<project>/checkpoints/ppo_final.zip" \
  --output-path "<project>/checkpoints/policy.onnx" \
  --algorithm PPO
```

成功后会得到一个约 280 KB 的 `policy.onnx`。

---

## 4. 怎么看结果

结果分散在两处，**都要看**。

### Python 侧（训练进度）— 控制台输出

```
| time/fps                 | 403      |   ← 每秒环境步数。几百是正常值
| time/total_timesteps     | 61440    |   ← 累计步数
| train/explained_variance | 0.996    |   ← 价值函数拟合得多好，越接近 1 越好
| rewards/mean_reward      | 16.7     |   ← 每集平均奖励
| rewards/mean_steps       | 8.7      |   ← 每集平均步数，越小说明越快抓到
```

### Unreal 侧（策略行为）— `Build\Staging\Windows\PursuitAI\Saved\Logs\PursuitAI.log`

```
LogScholaTraining: CollectEnvironments(): Collected Environments PursuitAIEnv_0   ← 环境被发现了
LogPursuitAI: PursuitAIEnv: training. arena=500x500x250 ... obs=Box(6) action=Box(3)
LogPursuitAI: PursuitAIEnv: episode 1     TIMEOUT in 300 steps, travelled 10927 cm  ← 一开始乱走
LogPursuitAI: PursuitAIEnv: episode 4625  CAUGHT  in 6   steps, travelled 300   cm  ← 后来直奔目标
```

带窗口跑时第一行会多两个字：`training (visual)`，说明场景这次是搭起来的（见下）。
无窗口的就是 `training`，既没有场景也没有渲染开销。

一句话判断标准：

- **`Collected Environments` 有东西** → 环境被发现了
- **日志里 `Fatal` / `Error` 出现 0 次** → 没炸
- **episode 的「步数」和「移动距离」在变小** → 真的在学
- **`explained_variance` 接近 1** → 策略收敛了

### 看画面 — 四种运行方式

同一个关卡、同一个 Actor。两件互相独立的事决定它怎么跑：

- **谁在开飞机** —— 驱动模式：`-PursuitDemo` / `-PursuitInference` / 都不给就是 Train（Python）
- **有没有画面** —— schola 那边给不给 `--headless`

| 你要干什么 | 脚本 | 窗口 | 谁在开飞机 | 需要 |
|---|---|---|---|---|
| 正式训练（最快） | `run_training.bat` | 无 | Python 里的 PPO | .venv |
| **看训练过程** | `watch_training.bat` | 有 | Python 里的 PPO | .venv |
| 看内置演示 | `watch_demo.bat` | 有 | 内置贪心策略 | 什么都不用 |
| 看训练成果 | `watch_inference.bat` | 有 | 导出的 ONNX 模型，引擎内跑 | `policy.onnx` |

命令行开关总共就六个，可以自由组合：

| 开关 | 含义 |
|---|---|
| `-PursuitDemo` | 换成 Demo 驱动 |
| `-PursuitInference` | 换成 Inference 驱动 |
| `-PursuitVisual` | 强制打开画面 |
| `-PursuitDebug` / `-PursuitNoDebug` | 强制开 / 关调试显示（默认跟着画面走） |
| `-PursuitModel=<路径>` | 换一个 ONNX（不给就用 `checkpoints\policy.onnx`） |
| `-PursuitSeed=<整数>` | 钉死出生点随机流，用来复现同一局 |
| （都不给） | 走 Train 驱动 |

**画面这一条不靠开关判断，是引擎自己判断的。** `--headless` 会让独立进程拿到
`-nullRHI`，此时 `FApp::CanEverRender()` 为 false，环境就不建场景；反过来只要这次
启动有渲染器（`-WINDOWED`），训练也照样把地板、灯光、相机搭起来。
所以「看训练」不需要任何新参数，**去掉 `--headless` 就是全部改动**。
`-PursuitVisual` 是留给「编辑器里点 Play 跑训练」这种没法改命令行的场景的强制开关。

**`-PursuitSeed` 是为了回答一个问题：无渲染训练和有渲染的演示，动作对得上吗？**

对得上，而且是结构上保证的：移动就是 `AgentPos += Direction × MoveStep` 的纯位置算术
（`ApplyActionVector`），**不读 `DeltaSeconds`、不读帧率、不走物理**，一步位移恒为 `MoveStep`。
唯一吃 dt 的函数是 `StepWatched`，它只管演示／推理的「每秒放几步」，不碰几何。
所以帧率只决定放得多快，不决定放出来是什么。

但这句话在没有种子的时候**无法验证**：`Seed` 默认 0，而 0 意味着 `FMath::Rand()`，
两次启动本来就不是同一局。钉上种子之后，「同一份二进制、同一张图、只差渲染」的两次运行
每一集都应该逐字相同 —— 这是可以 diff 出来的，不是靠信。

反过来，三件事会让它失效，动手之前先想清楚：

- 换成 `ACharacter` / `CharacterMovementComponent`，或引入任何 Chaos 物理：那是 cm/s × dt，
  两边 dt 不同立刻发散。这也是移动层保持位置式步进、不用角色移动组件的第二个理由。
- 让**逻辑**（不只是绘制）去读 `FApp::CanEverRender()` 或 viewport。
- 拿训练的 rollout 直接跟推理演示对比：训练是从高斯里**采样**，推理取均值，
  同种子也不该逐帧重合。要比就两边都跑 `-PursuitInference`。

**实测（2026-09-19，本机）**：同一份 DLL、同一张图、`-PursuitInference -PursuitSeed=7`，
一次 `-nullrhi`、一次 `-windowed`，各跑 150 秒：

```
headless  63 集          窗口  62 集
diff：前 62 集逐字相同
  watched episode  1  CAUGHT in 10 steps, travelled 500 cm, reward 19.863
  watched episode  2  CAUGHT in  6 steps, travelled 300 cm, reward 15.953
  ...
  watched episode 62  CAUGHT in  5 steps, travelled 250 cm, reward 14.836
```

差的只有窗口那轮被杀早了的第 63 集（150 秒到点时正好差一集），不是数值分歧。
`travelled` 是每步距离的浮点累加、`reward` 是每步 shaping 的浮点累加，
任何一处 dt 依赖都会立刻把这两列拉开 —— 62 集逐字相同，就是"对得上"的证据。

同一次核查还确认了 `-nullrhi` 下的 Train 模式启动行是 `scenery off, debug view off`，
也就是无渲染训练确实不建场景、不开调试面板（watch 两类模式则会强制开，因为它们的用途就是给人看）。

**看训练过程**（先看这个）：

```
双击 <project>\\tools\watch_training.bat
```

会先编译游戏再启动。窗口里绿球追红球，同时控制台刷 episode 统计。
参数只有步数一个。

> 实测节奏（同一台机器、20000 步那次）：**59 秒跑了 130 集，约 300 步/秒**。
> episode 1 是 `TIMEOUT 300 步 / reward −1.2`，59 秒后 episode 130 是
> `CAUGHT 17 步 / reward +10.9` —— 学习过程本身在窗口里就能看出来：
> 一开始满场乱飞，后来直奔目标。
> 无窗口的 headless 大约是 400 步/秒，所以画画只吃掉约四分之一的吞吐，比想象中便宜。
>
> **这里没有调速旋钮，是故意的。** Schola 的 `--fps` 看着像限速，其实不是 ——
> 它设的是**物理固定时间步长**（源码注释：*Fixed FPS ... if None no fixed timestep is used*），
> 这个环境没有物理，所以它一点也不会拖慢训练（实测 `--fps 20` 照样跑 300 步/秒）。
> 想看**每一次决策**而不是整轮训练，用 `watch_demo` / `watch_inference` ——
> 那两个有 `DemoStepsPerSecond` 节流（默认每秒 6 步）和每集停顿。

> 训练时的画面和观战时的画面**不完全一样**：观战有节流（`DemoStepsPerSecond`，默认每秒 6 步）
> 和每集结束的停顿，所以看得清每一次决策；训练没有节流 —— 步进节奏归 Python 训练器管，
> 画面只是「这一帧渲染时模拟恰好停在哪」。速度拉高之后会看到目标在集与集之间跳，
> 那是训练真实的模样，不是 bug。想看每一次决策，用 `watch_demo` / `watch_inference`。

**Demo 模式**（不需要模型，就是有个东西能动）：

```
双击 <project>\\tools\watch_demo.bat
```

**Inference 模式**（看训练成果，没有 Python）：

```
双击 <project>\\tools\watch_inference.bat
```

窗口里能看到的东西：

| 画面元素 | 含义 |
|---|---|
| 绿色小球 | 追击者（智能体），半径 25 cm |
| 红色小球 | 目标。**半径正好等于捕获半径 50 cm** —— 绿球整个钻进红球就算抓到 |
| 灰蓝地板 | 1000 × 1000 cm，摆在场地的**底面**（Z = −250） |
| 地上的暗斑 | 球投下的影子。**球是会飞的**，所以影子离球有一段距离 —— 那正是高度 |
| 透视视角 | 故意斜着架，正交俯视会把 Z 轴压平成一个点 |

一集结束后停 0.75 秒再开下一集，方便看清刚才那一秒发生了什么。
想看得更慢，可以在编辑器里把 `DemoStepsPerSecond` 从 6 调到 1（见下）。

也可以在**编辑器里**看，好处是能在 Details 面板里实时调参数：

```bash
"E:/Project/UE5/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe" \
  "<project>/PursuitAI.uproject" /Game/Maps/L_PursuitAITrain -PursuitDemo
```

打开后点绿色三角 Play 即可。调试显示是自动开的（见下一节）；要临时关掉它，
命令行加 `-PursuitNoDebug`。属性面板上的 `bDrawDebug` 只是只读快照 ——
它由 BeginPlay 根据「这次启动能不能渲染」算出来，手改无效，否则一个存进关卡的值
会让之后每次启动都意外地不显示。

> **注意**：驱动模式**只能从命令行给**，属性面板上改不了。这是故意的 —— 如果在训练关卡上
> 把它存成了 Demo，之后每一次训练都会静默失败（环境不连 Python 了）。

### 画面里的调试显示

只要画面开着，HUD 和场景标记就自动打开。左上角是面板，另外每个球头顶有一行标签。
不用任何设置，训练、推理、演示三种模式都有。

```
PursuitAI   |   PPO Training - SB3 over gRPC, weights live in Python
Episode 128   Step 243 / 300   312 steps/s   1.2 episodes/s
Reward   step +0.014   total +0.720   closing +48 cm
Episodes   41 caught / 41 done   best +12.41   mean(last 20) +9.83
Distance 842 cm   Target visible YES
Observation (the policy's input)   rel +0.42 -0.77 +0.11   |   self +0.05 -0.31 -0.18
Action   x +0.83  y -0.21  z +0.04   |a| 0.86   off-axis 19 deg   step 43 of 50 cm
legend   green = move action   yellow = to target   ray = line of sight   ring = capture
```

| 行 | 是什么 | 怎么用 |
|---|---|---|
| 第一行 | 谁在开飞机，**以及权重在哪** | 见下面那一节 |
| Episode / Step | 当前第几集、这一集走到第几步 | 「几步抓到」一眼可见 |
| steps/s | 每秒实际推进的步数，**每秒采样一次算出来的，不是配置值** | 判断这次是不是真在跑；对比窗口 vs headless 的吞吐差 |
| Reward step / total | 这一步的奖励 / 这一集累计 | step 里含 ±10 的终结奖励，所以抓到的那一步会跳到 +10 以上 |
| closing | 这一步把距离缩短了多少 cm | 负值 = 正在远离目标 |
| best / mean(20) | 历史最好一集 / 最近 20 集平均 | **判断「有没有学会」就看这一行** |
| Distance / Target visible | 直线距离；视线是否被挡住 | 视线是真 trace，不是标志位 |
| Observation | 策略这一步收到的 6 维输入，和训练用的是同一个函数 | 数字对不上就是契约层出了问题 |
| Action | 输出方向、模长、与目标的夹角、这一步实际走了多少 | `off-axis` 就是「瞄得准不准」 |
| legend | 颜色对照 | 录视频时给观众看 |

场景里的标记：

| 标记 | 含义 |
|---|---|
| 绿色箭头 | 这一步实际移动的方向和距离（肉眼长度 = 位移 × 4） |
| 黄色箭头 | 指向目标，**固定 4 步长**。和绿箭头等长同向 = 全速直冲 |
| 红线段 | 接在绿箭头末端 —— 被场地边界吃掉的位移。**出现红就是撞墙** |
| 绿色射线 | 从追击者到目标的视线 |
| 红色射线 | 视线被挡住，射线停在挡住它的地方 |
| 橙色圆环 + 线框球 | 捕获范围（等于红球半径） |
| `CHASER #0  R +16.00` | 追击者编号 + 这一集累计奖励 |
| `TARGET  665 cm` | 目标的直线距离 |
| 灰线框 | 场地边界，也就是把动作裁掉的那三条线 |

两点要知道，否则会以为调试显示坏了：

- **红色射线现在不会出现。** 视线是真实的 `LineTraceByChannel`，但场上除了地板没有任何
  可阻挡物。启动日志会打印一行
  `N visible collidable actor(s) besides this one: <名字>` —— 现在那个 N 是 1，指的是
  `GameplayDebuggerCategoryReplicator_0`，引擎自己的调试簿记 actor，它没有几何体，
  射线打不到它。所以日志把名字一起打出来，而不是只报一个数字。
  往场地里放个障碍物，这条射线不用改一行代码就会变红，`Target visible` 也会跟着变 `NO`。
- **抓捕那一瞬间绿球看不见，不是 bug。** 捕获半径 50 cm 而绿球半径 25 cm，所以抓到的那一刻
  绿球整个在红球内部。红球背光的下半部分会黑到 `(4,2,2)` —— 那也不是加了什么黑球，
  是同一颗球没被照亮的那半边。

### 看不到「神经网络的思考」——这件事必须说清楚

HUD 上每一个数字都是**策略的输入、输出和得分**：

- 输入 = 那 6 维观测
- 输出 = 那个 3 维动作
- 得分 = reward

**没有一格显示网络内部发生了什么。** 权重根本不在这台进程里，它们在哪看第一行：

| 第一行写的是 | 权重在哪 | 谁在更新它 |
|---|---|---|
| `PPO Training - SB3 over gRPC, weights live in Python` | Python / PyTorch 进程，每个 batch 都在改 | 训练器 |
| `PPO Inference - ONNX on NNERuntimeORTCpu` | 冻结的 `policy.onnx`，引擎内前向 | 没人，只推理 |
| `Scripted greedy - no model at all` | 没有权重，就是几行 `(目标 - 自己).GetSafeNormal()` | —— |

所以「它学没学会」不能靠盯着一帧动作下结论。三个真能回答这个问题的东西：

1. **曲线** —— `tools\open_tensorboard.bat`，看 `episode_reward`、`explained_variance`
2. **行为变化** —— HUD 上 `41 caught / 41 done` 和 `mean(last 20)` 在往上走，
   以及窗口里「一开始满场乱飞，后来直奔目标」
3. **off-axis** —— 训练早期是随机角度，收敛后稳定在很小的值

反过来说，只看一帧动作就说「它想包抄」，那一定是在编故事：单步动作里没有意图，
意图只在几百集的统计里。

---

## 5. 文件都是干什么的

```
<project>\\
├─ PursuitAI.uproject                      项目描述（含启用的插件，含 NNERuntimeORT）
├─ Content\Maps\L_PursuitAITrain.umap      训练关卡（脚本生成，已入库）
├─ Source\PursuitAI\
│   ├─ PursuitAIEnv.h / .cpp               ★ 环境本体。要改游戏规则就改这里
│   ├─ PursuitAI.Build.cs                  模块依赖（多了 ScholaNNE / ScholaInferenceUtils / NNE）
│   └─ PursuitAI.cpp / .h                  模块入口
├─ Plugins\Schola\                         Schola 插件（不动；改动会导致重编）
├─ checkpoints\                            训练产物（不入库）
│   ├─ ppo_final.zip                       SB3 策略
│   └─ policy.onnx                         ★ 引擎内推理读的就是它
├─ tools\
│   ├─ run_training.bat                    ★ 一键训练（含保存策略）
│   ├─ watch_training.bat                  ★ 一键训练「带窗口」（同一轮训练，能看见）
│   ├─ export_policy.bat                   ★ 一键导出 ONNX
│   ├─ watch_demo.bat                      ★ 一键观战（内置贪心策略）
│   ├─ watch_inference.bat                 ★ 一键观战（跑训练好的模型）
│   ├─ open_tensorboard.bat                一键看曲线
│   ├─ gen_training_level.py               ★ 无头生成关卡
│   ├─ record_window.py                    ★ 录 mp4（不需要 OBS）；`--still x.png` 抓单帧
│   ├─ inspect_frame.py                    ★ 从截图里查「画面上到底有什么」
│   ├─ check_env.ps1 / check_python_env.ps1 环境自检
│   ├─ gen_project_files.ps1               生成 .sln（要在 VS 里写代码时用）
│   └─ smoke_headless.ps1                  冒烟测试
├─ docs\                                   文档（本文件也在里面）
├─ Saved\UnrealBuildTool\BuildConfiguration.xml   ★ 关掉 UBA（已入库，别删）
├─ Build\Staging\...                       编译产物
├─ logs\                                   训练日志 + TensorBoard 数据（不入库）
└─ .venv\                                  Python 环境（不入库）
```

**想改游戏，改 `PursuitAIEnv.cpp` 就够了。** 具体改哪里：

| 想改什么 | 改哪个函数 |
|---|---|
| 观测里放什么信息 | `BuildObservation` |
| 动作怎么解释 / 怎么移动 | `ApplyActionVector` |
| 奖励怎么给 | `Step_Implementation`（`OutAgentState.Reward`） |
| 什么时候算结束 | `Step_Implementation`（`bTerminated` / `bTruncated`） |
| 开局怎么摆位 | `RandomizePositions` |
| 场地大小、步数上限等 | `PursuitAIEnv.h` 顶部的 `UPROPERTY` |
| 观战时的布景（地板/灯/相机） | `SetupDemoScene` |
| 观战时的自动策略 | `WriteGreedyAction` |

改完**不用手动编译**，训练命令里的 `project` 模拟器会自动重编。

### 唯一一处需要小心的设计：共享契约层

`PursuitAIEnv.cpp` 里这几个私有函数**各只有一份实现**，训练路径和推理路径都要穿过它们：

```
DefineSpaces()        观测/动作空间的唯一描述处
BuildObservation()    观测的唯一构造处
ApplyActionVector()   移动的唯一实现
BeginEpisode()        开局的唯一重置处
```

**为什么值得专门说：** Schola 的**训练侧**和**推理侧**是两套互不相干的抽象
（训练侧实现 `IBaseScholaEnvironment`，由 gRPC 连接器驱动；推理侧实现 `IAgent`，
由步进器 + 本地策略驱动），插件本身不提供任何保证让它们一致。如果两边各写一份，
就会出现「观测顺序悄悄不一样 → 训练时 99% 成功率，部署后原地转圈」这种最难查的故障。
把它们收敛到一份，这种漂移就从「可能发生」变成「不可能发生」。

对应地，代码里有两条容易踩的坑，已经在源码注释里写清楚了：

- 推理侧的步进器**不会**自动重置环境（训练侧连接器会）。所以 `Act_Implementation`
  里必须自己判断本集是否结束并重置。
- `SimpleStepper.h` 直接 include 会撞上 `windows.h` 的 `#define GetObject GetObjectW`，
  必须在 include 前 `#undef GetObject`。

---

## 6. 出问题怎么办

| 症状 | 原因 | 处理 |
|---|---|---|
| 编译报 `Unable to build while Live Coding is active` | **有 `UnrealEditor.exe` 开着本项目**，Live Coding 占着构建 | 关掉再编；或者在编辑器里按 `Ctrl+Alt+F11` 让它自己热编译。**注意这条错误下 UBT 很快就返回，别被「3 秒就编译完」骗了 —— 一定去日志里确认 `Result:` 那行**。<br>注意：不只是编辑器，**观战窗口（watch_demo / watch_inference）也是 `UnrealEditor.exe`，同样会起 Live Coding 服务**。所以**编译前把所有 UE 窗口一起关掉** |
| 满屏 `SetFileInformationByHandle ... Access is denied`，然后 `LNK1136` / `LNK1201` / `C1083` | **UBA** 构建加速器在本机坏了 | 确认 `Saved\UnrealBuildTool\BuildConfiguration.xml` 里 `<bAllowUBAExecutor>false</bAllowUBAExecutor>`。如果 `.lib`/`.pdb` 已经变成 **0 字节**，**手工删掉它们**再重编 |
| 训练报 `Unhandled FakeTensor Device Propagation for aten.mm.default` | 用了 `--export-onnx`，训练中途在 GPU 上导出 | 去掉 `--export-onnx`，训练完用 `tools\export_policy.bat` 单独导 |
| 训练完 `checkpoints\` 里什么都没有 | 没给 `--save-final-policy`（默认 `False`） | 加上它，或直接用 `tools\run_training.bat` |
| `watch_inference.bat` 提示找不到模型 | 还没导出 | 先 `run_training.bat`，再 `export_policy.bat` |
| 推理窗口里绿球发神经 / 原地转圈 | ONNX 的观测顺序和训练时不一致 | 检查是不是绕过共享契约层改了 `BuildObservation` |
| `HTTP proxy handshake ... response code 502` | 本机 `HTTP_PROXY` 指向本地代理，`NO_PROXY` 没设 | 命令前加 `NO_PROXY=127.0.0.1,localhost` |
| `tensorboard not installed. Disabling tensorboard logging` | 装 tensorboard 时被上面的代理挡了 | `NO_PROXY=... ./.venv/Scripts/python.exe -m pip install tensorboard` |
| `Collected 0 environment(s)` | 关卡里没有环境 Actor | 重跑第 3 节第 1 步生成关卡 |
| `No module named schola.cli` | 用法不对 | 用 `.venv/Scripts/schola.exe`，别用 `python -m schola.cli` |
| 训练日志警告 `trying to run PPO on the GPU ... MlpPolicy` | SB3 的常规提示 | **无害，忽略** |
| `watch_training.bat` 启动后好几分钟没窗口、日志也空 | 在跑 `BuildCookRun`（编译 + Cook），Cook 是大头，这段时间没有输出 | 正常，等着。确认在干活：`Get-CimInstance Win32_Process -Filter "Name='dotnet.exe'"` 里能看到 `BuildCookRun ... -cook -FastCook` |
| 截图/录屏录到的不是游戏画面 | `ImageGrab` / 录屏工具抓的是**屏幕**，窗口被别的窗口挡住了 | 把遮挡的窗口挪开或最小化；`tools/record_window.py` 同样受这条限制 |
| 画面里多出一颗灰色小球，永远停在中间 | 引擎的 `ADefaultPawn` 自带一个球网格，关卡里没有 PlayerStart 时它就生成在原点 | 已经处理（观战模式第一帧会把它藏起来），日志里能看到 `hid the engine's default pawn` |
| 每颗球下面有两个影子 | 两盏方向光都在投影 | 已经处理（只有主光投影，填充光不投影） |
| 球一片惨白、或者一半黑一半白，看不出颜色 | 场地里没有 PostProcessVolume，**曝光不会自适应** —— 画面亮度直接等于「反照率 × 灯光强度」，增益太高就把球推到裁切 | 已经处理（把灯光强度压到增益 ≈ 1）。要再调就动那两个 `SpawnSun` 的强度：**反照率和灯光强度要一起想**，只改一个必然跑偏 |
| 加了调试显示后 HUD 里 `steps/s` 是 0 | 采样窗口比两次 tick 的间隔还短，采到一个合法的空档 | 采样窗口是 1 秒（试过 0.5 秒，会在被系统节流的窗口上稳定显示 0）。窗口在前台时不会出现 |

---

## 7. 后面还要做什么

| 轮次 | 要做的事 | 为什么 |
|---|---|---|
| ~~4~~ | ~~3D + 连续动作 + 训练与推理共用契约层~~ | **已完成** |
| ~~7~~ | ~~导出 ONNX，用 Unreal 的 NNE 在引擎里直接推理~~ | **已完成**（提前到第 4 轮做掉了） |
| ~~—~~ | ~~调试显示：HUD 面板 + 场景标记（动作箭头、目标方向、视线射线、捕获环、头顶 ID/累计奖励）~~ | **已完成**（本轮） |
| **5** | **把静止靶换成会跑的靶**，做成真正的 1v1；奖励加时间惩罚 | 现在证明的是链路，还不能叫「追击 AI」。纯距离塑形容易退化成贴墙绕圈，必须加时间惩罚 |
| 6 | 调 PPO 超参把它跑收敛；并行多环境提速 | 现在只是「能跑」 |
| 8 | 视觉升级：场地线框、拖尾、命中特效 | 演示视频需要 |
| 9 | 录演示视频 + 写技术报告 | 对外展示 |
| 10 | 可选：多智能体共享策略 / 模仿学习 / StateTree 接推理 | 加难度 |

> 第 5 轮开始前可以先加**障碍物**：视线射线和 `Target visible` 已经接好，
> 场上出现第一个可碰撞物体时它们会自动开始工作，不用改代码。

关于画面：训练时无窗口是刻意的（为了吞吐），想**看**就用两个 watch 脚本。

### 录演示视频

素材已经齐了，录一段只要一条命令（**窗口保持可见，别被别的窗口挡住** —— 它是录屏，
不是录窗口）：

```bash
cd "E:/Project/UE5+ai"
./.venv/Scripts/python.exe tools/record_window.py --seconds 20 --fps 14 \
  --output logs/inference_demo.mp4
```

想拍得好看一点，有三个旋钮：

- **调慢节奏**：`DemoStepsPerSecond` 默认 6 步/秒，一集 1~2 秒就结束了，太快看不清。
  改小到 2~3，让每一次决策都看得见。
- **换个角度**：`SetupDemoScene` 里那几行相机参数（`CamDistance` / 俯角 / `FieldOfView`）。
- **要不要调试覆盖层**：默认是开着的，而且它对视频其实是加分项（观众能直接看到
  action、reward、抓捕范围）。想要干净画面就在命令行加 `-PursuitNoDebug`，
  也就是 `tools\watch_inference.bat` 里那句末尾加一个开关。

只想确认画面长什么样、不想要一段视频，用同一个脚本抓一帧就够（它会先把窗口提到前台，
不然抓到的可能是压在它上面的那个窗口）：

```bash
./.venv/Scripts/python.exe tools/record_window.py --still logs/shot.png
```

画面看起来不对时，别靠眼睛猜，用 `tools/inspect_frame.py` 把截图变成数字：

```bash
./.venv/Scripts/python.exe tools/inspect_frame.py logs/hero.png --margin-top 190
```

它会列出所有「比地面暗」的物体（位置、大小、平均颜色），以及一条可选的亮度剖面 ——
**亮度剖面从不超过周围地面 = 影子；有更亮的地方 = 实体网格**。第 4 轮那三个画面 bug
（引擎默认 Pawn 的幽灵球、双份影子、遮住球的地板）全是靠这个查出来的。

---

## 8. 更细的文档

| 文件 | 内容 |
|---|---|
| `docs/SCHOLA_INTERFACE.md` | Schola C++ 接口速查：三种环境接口、Space/Point 对应、三种模拟器、生命周期回调顺序，以及 §8 **推理侧接口**（`IAgent` vs `IBaseScholaEnvironment`、ONNX 的两条加载路径、运行时名字、tick 频率陷阱） |
| `docs/PROJECT_PLAN.md` | 轮次路线图 + 每轮的完整执行结果与判据 |
| `docs/HANDOFF.md` | 轮次交接快照、事实边界、下一轮清单 |
| `docs/ENVIRONMENT_AUDIT.md` | 最初的环境审计报告 |
