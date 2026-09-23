# v2：PPO 直接控制 ACharacter（对齐官方 ScholaExamples Tag 结构）

> 2026-09-21 定案。参考工程已克隆到 `refs/ScholaExamples`（tag v2.1.0，配套 Schola 2.1 / UE 5.5–5.7）。
> 本文档是 v2 的实施蓝图；v1 球环境（`APursuitAIEnv`）保留不删。

## 0. 为什么走官方结构

官方 Tag 示例（3v1 追捕）已经把"PPO 控制 ACharacter"这条路走通，且与本项目版本组合完全匹配。
全部构件**已在本地插件里**，零插件改动：

| 官方构件 | 位置 | 作用 |
|---|---|---|
| `UMovementInputActuator` | `Plugins/Schola/Source/ScholaInteractors/Public/Actuators/MovementInputActuator.h` | AddMovementInput 型执行器，X/Y/Z 轴可开关，速度上下界定义动作空间 |
| `URayCastSensor` | `.../ScholaInteractors/Public/Sensors/RayCastSensor.h` | 射线感知（36 射线 360°），后期城市避障用 |
| `ATagAgent : ACharacter + IAgent` | `refs/ScholaExamples/Source/ScholaExamples/Public/Tag/TagAgent.h` | 角色即 Agent：Define/Observe/Act 按枚举传感器/执行器组件构建，训练与推理字典键自动对齐 |
| `ATagEnvironment : AGymConnectorManager + IMultiAgentScholaEnvironment` | `refs/.../Tag/TagEnvironment.h` | 奖励/Reset/Step/撞墙判定/回合生命周期的官方范本 |
| `TagTrain / TagTrainVec / TagInference` 三地图 | `refs/.../Content/Examples/Tag/Maps/` | 训练图 / 并行训练图 / ONNX 推理图分离的官方推荐结构 |
| Runner/Tagger BP + `DirectionDistanceObserver` BP 组件 | `refs/.../Content/Examples/Tag/Blueprints/` | 角色蓝图挂传感器/执行器组件；方向+距离观测（局部坐标，利于换图迁移） |

## 1. 官方结构 → PursuitAI v2 映射

| 官方 Tag | PursuitAI v2 | 说明 |
|---|---|---|
| Tagger（追击者，PPO 控制） | `APursuitChaserAgent : ACharacter + IAgent`（新） | 挂 `UMovementInputActuator`（X/Y 开、**Z 关**）+ 观测组件；皮肤 = RPGHeroSquad BP |
| Runner（逃跑者，PPO 控制） | 规则逃跑者（v1 的 Flee 策略移植为脚本驱动同一个 Character） | 项目第一阶段只训追击者，此处**有意偏离**官方 3v1 双方皆训 |
| 撞墙判定（`TaggerWallHitReward`） | 采纳：Character `Hit` 事件 → 步内小惩罚 | 参考 TagEnvironment.cpp 的 `HandleAgentHit` |
| 捕获判定 | 胶囊中心水平距离 ≤ CatchRadius（沿用 v1 数值 85） | Z 同层容差保留 |
| NavMesh Reset（官方 `bUseNavMeshReset`） | **不用**：空地训练图 + 圆形竞技场随机出生（沿用 per-episode seed） | 保持 v1 的可复现出生序列设计 |
| 三地图分离 | `L_PursuitCharTrain`（空地）/（并行版后置）/ `Demonstration`（推理+演示） | 城市先只做推理舞台，不做训练图 |

## 2. 确定性重建（v1 的核心资产必须保住）

CharacterMovement 是 dt 驱动，v1 的"逐字可复现"依赖以下措施，**全部要实测验证**：

1. **固定帧率**：训练/推理均以固定 dt 运行（启动参数 `-UseFixedFrameRate` + `t.MaxFPS=<n>`，
   或 `DefaultEngine.ini` 的 `bUseFixedFrameRate`；n 建议 30，与 Schola step=1 tick 对齐）。
2. **每回合播种保留**：`Seed + EpisodeIndex` 的出生序列设计原样迁移。
3. **验收判据**：同一模型、同一 seed、跑两遍 → episode 日志的出生位置与捕获步数逐字一致；
   双窗对比的 `-PursuitStartAt` 闸门逻辑照旧适用。
4. 已知风险：动画 tick、物理 substepping、帧率漂移。动画是纯表现层，确认不回写位置即可。

## 3. 奖励设计（起点，训练时再调）

沿用 v1 shaping + 官方撞墙惩罚：

```
reward = k_dist * (prev_dist - dist)      // 距离差 shaping（v1 同款）
       + CatchReward   (捕获时，+10 量级)
       + StepPenalty   (每步 -0.005，参考官方 TaggerStepPenalty)
       + WallHitPenalty(步内撞墙 -0.02，参考官方 TaggerWallHitReward)
       + 回合超时截断（MaxSteps，参考官方 2000）
```

## 4. 实施清单（顺序即依赖）

1. [ ] `APursuitChaserAgent`（ACharacter + IAgent）：枚举组件构建 Define/Observe/Act，照抄 TagAgent 的键对齐规则
2. [ ] 观测组件：方向+距离（目标在角色局部坐标下的方向/距离，对齐官方 DirectionDistanceObserver）+ 自身速度；RayCast 后置
3. [ ] 规则逃跑者：Flee 脚本驱动同一 Character 类（挂 Agent 的手动模式）
4. [ ] `L_PursuitCharTrain` 空地训练图（gen 脚本：圆形边界墙 + 无楼 + PlayerStart）
5. [ ] 固定帧率 + 确定性验收（双跑逐字一致）
6. [ ] `tools/run_training_char.bat`（--timesteps 参数化 + `--enable-checkpoints --save-freq`）
7. [ ] `tools/export_policy.bat` 输出名参数化 + `tools/onnx_add_tanh.py`（v1 遗留缺口，v2 同样需要）
8. [ ] 吞吐实测：steps/s 报表，决定并行 simulator 数量
9. [ ] 0 / 100k / 500k 三档训练（0 档用 `--timesteps 0 --save-final-policy` 验证，不行则写未训练导出脚本）
10. [ ] 双窗对比片（左 RANDOM BASELINE / 右 PPO 各档），`portfolio_pair.py` 传 `-PursuitModel=` + 标签

## 5. 与 v1 的关系

- v1 球环境与 60k 旧模型：保留归档，不删不改；v1 的 0/100k/500k 训练**取消**（模型在 v2 语义下作废）。
- v1 的管线资产全部继承：per-episode 播种、双窗闸门录制、`analyze_clip.py` 体检、`-PursuitModel=` 推理加载。
- v1 球 vs v2 Character 的对比本身就是作品集素材（abstract → embodied 的演进叙事）。

## 6. 风险与对策

| 风险 | 对策 |
|---|---|
| 固定帧率下仍不可复现（动画/物理干扰） | 先关动画 tick 验证；逐项排查；最坏情况记录偏差量并改用"统计对比"口径 |
| 训练吞吐不足 | 空地图 + 关动画；实测 steps/s 后决定 `--num-workers`（Schola 2.1 支持多进程） |
| 角色卡进边界墙/几何缝 | 空地图只有圆形边界墙；撞墙惩罚 + 超时截断兜底；城市推理图先只用开阔区域 |
| 未训练模型近静止（SB3 小增益初始化） | 主对比用 RANDOM BASELINE（v1 已定案），未训练 ONNX 仅作彩蛋 |

## 7. 多智能体训练接入方案（2026-09-21 定案，对应路线图步骤 11）

**目标**：士兵（RPGHero）与狗（AnimalHero）一样接 PPO，对齐官方 Tag 示例（3 Tagger 追 1 Runner，
Tagger 共享一个 policy）。演示侧士兵已落地（`SupportAgent`，见 2026-09-21 日志），训练侧按下面走。

**现状与差距**：env 是 `ISingleAgentScholaEnvironment`（单 agent 三签名）；Tag 示例
`ATagEnvironment` 是 `IMultiAgentScholaEnvironment`，接口差异全部集中在三个 Implementation 的
In/Out 容器（`FInteractionDefinition&` → `TMap<FString, FInteractionDefinition>&` 等），
agent 以字符串 ID 寻址（如 `Tagger_0` / `Runner_0`）。

**改动清单（估算：env 侧为主动作，agent 侧近乎零）**：
1. `APursuitCharEnv` 换 `IMultiAgentScholaEnvironment`，三个 Implementation 改 TMap 签名；
   内部逻辑不变，把单 agent 的 obs/action/reward 打包进 `chaser`（狗）/`support`（士兵）两个 ID 桶。
   观察模式路径（RunWatchedEpisode）不受影响——它不走 Schola 接口。
2. 士兵 obs/act 规格与狗完全同构（这正是 v2 的设计起点）：TargetSensor 指向 evader +
   CharMoveInput(2) + JumpInput(1) → 天然可共享同一 policy，无需维度对齐工作。
3. 奖励：两只 chaser 各自算 shaping（到 evader 的距离差）+ 抓捕奖；任一 chaser 抓到即双方
   terminated（Tag 同款）。注意 watched 侧已实现"任一抓到即结算"（2026-09-21，日志含 caught_by），
   训练侧 Step 需同样按"最近 chaser 判抓"。
4. 训练脚本：`schola sb3 train` 里两个 agent ID 映射到同一个 policy slug（官方 Tag 教程做法）；
   checkpoint/导出/ONNX 推理不变（仍是一个 policy 文件）。
5. 顺序依赖：先跑完单 chaser 3 维 500k 基线（步骤 1），再切多智能体——基线是多智能体版本的
   对照锚点，动作/观测语义对单 chaser 保持不变。

**不做**：异构策略（士兵/狗各训各的）、Runner 也接入训练（evader 仍规则 AI，见 v2 目标边界）。

### 7.1 城市训练落地记录（2026-09-21，方向验证跑）

- **训练地图**：`Demonstration_Train` = Demonstration 的副本 + 烘焙 env/两 agent（`tools/gen_char_city_level.py`，
  幂等）。env 位置 = 实测广场 (-250,0,70)，`bStartOnCityStage=True` 烘焙了 `-PursuitStage=city` 的全部行为
  （出生探测/广场限径/拴绳），Schola 训练命令行零额外参数。
- **跳台**：bCityStage 时 C++ 生成 3 个 75cm 平台（固定偏移 (200,200)/(-260,180)/(-80,-300)，160x160，
  RF_Transient）。高度设计：MaxStepHeight 45 上不去、CatchHeightTolerance 70 地面够不着、JumpZVelocity
  顶点 ~90 跳得上——跳跃是唯一通路。
- **结构性修复**：APursuitCharEnv 此前无 RootComponent → SetActorLocation 静默失效、烘焙变换存不住，
  城市场地中心实际一直在 (0,0,0)。加 USceneComponent 根后 plaza 语义首次真正生效。
- **管线**：`tools/run_training_char_city.bat <steps> <save_freq>`；录像 `tools/record_char_city.py`（复用
  record_city 机制，ready 标记 = episode 1 started，杀窗按 PCD3D 匹配——按项目名杀会误伤含 PursuitAI
  标题的 VS 窗口，2026-09-21 实测误杀过）。
- **50k 首跑结果**：管线全通；7 回合 0 抓捕（15s 超时），跳跃 493 次（policy 高频跳，方向未收敛）；
  explained_variance 0.885→0.322。判读：任务在城市里显著更难（建筑阻挡 + 跳台），50k 远不够，
  方向性结论=管线/奖励/观测正确，需 500k 级别训练。25k 中途检查点 ONNX 导出缺双键名适配
  （单 'action' 输出，onnx_add_tanh 拒绝猜配对）——后续需要时再补 split 逻辑。
- **存档**：checkpoints/ppo_v2c_{0k,25k,50k}.zip + policy_v2c_{0k,50k}.onnx；视频
  logs/portfolio/char_city_{0k,50k}_30s.mp4。

### 7.2 500k 正式训练 + 士兵默认隐藏（2026-09-21 深夜）

- **士兵默认不生成**：watch 里脚本士兵站着不动、看着像坏道具（用户反馈），`PursuitPlayGameMode`
  新增 `bSpawnSupportAgent`（默认 false，`-PursuitSupportAgent` 显式开启）。env 对 null SupportAgent
  即"无第二追击者"，抓捕/计分不受影响。士兵站着不动的根因**未查**——多智能体接入（§7）后它会以
  训练 agent 回归，脚本士兵届时删除。
- **500k 训练**：`run_training_char_city.bat 500000 50000`，50k 实测 ≈10.5 min，500k 估 ≈100-110 min。
  完成后：导出最终 ONNX + 录 0k/500k 对比视频。

### 7.3 无射线版 500k 结果与墙探测修复（2026-09-22 凌晨）

- **无射线 500k 实测 ≈18 min**（staged sim 复用 + 455 fps），但 **mean_reward 全程 -13.8~-20.9 无趋势，
  0 抓捕，回合全超时（~8000 步 = 15 sim-s）**。注意：距离塑形（0.005/cm）、dt 缩放罚、15 sim-s 回合墙
  全都在且正确——问题不在奖励。
- **根因判读：策略对障碍全盲**。TargetSensor 只有目标方位/距离/双速度，城市里"目标直前方"常被楼挡住，
  撞墙罚是唯一的墙信息（事后信号，无梯度指向）。开阔场地 500k 能学、城市 500k 学不动，差异即此。
- **修复：TargetSensor 5→8 维**，新增 3 根前方墙探测（-45°/0°/+45°，owner yaw 系）：球扫
  ECC_WorldStatic（与 env ProbePath 同语义，所见=所挡）、忽略双方 agent、600cm 量程（WallProbeDistanceCm）、
  读数=可通行比例（1=通到底）。旧 5 维模型全部作废（ppo_v2c_500k.zip / policy_v2c_norays_500k.onnx
  为无射线版存档）。
- **20k 冒烟**：端到端通过（525 fps，回合日志正常，无空间不匹配报错）。正式 500k（含导 ONNX
  policy_v2c_rays_500k.onnx + 录像 + 训练后自动关机）后台链执行。
- 工具坑：`export_policy.bat` 第一参数必须裸文件名（内部拼 checkpoints\ 前缀）；bat 经 PowerShell
  `*>&1` 重定向时 stderr 会被工具报成 failed（实际成功判据 = bat 尾部 Done 行 / ppo_final.zip 时间戳）。
