# Schola C++ 接口速查（v2.1.1 实测）

> 来源：直接读 `Plugins/Schola/Source/` 源码，非文档推测。第二轮/第三轮验证。
> 目的：后续每轮写环境代码时不用再翻插件源码。

---

## 1. 三种环境接口，选一个实现

| 接口 | 头文件 | agent ID | 用法 |
|---|---|---|---|
| `ISingleAgentScholaEnvironment` | `ScholaTraining/Public/Environment/SingleAgentEnvironmentInterface.h` | **硬编码 `"SingleAgent"`** | 单智能体，SB3 走这条 |
| `IMultiAgentScholaEnvironment` | `.../MultiAgentEnvironmentInterface.h` | 自己填，每个 agent 一个 string | RLlib 多智能体 |
| `ICppOnlyMultiAgentEnvironment` | `.../CppOnlyMultiAgentEnvironmentInterface.h` | 自己填 | 纯 C++，不对蓝图暴露（StateTree 用） |

三者共同基类：`IBaseScholaEnvironment`（`EnvironmentInterface.h`），内部统一走 `IScholaEnvironment`。

**关键：单智能体的 agent ID 是写死的 `"SingleAgent"`**，见 `SingleAgentEnvironmentInterface.h` 的
`Execute_InitializeEnvironment` / `Execute_Reset` / `Execute_Step`——它们用 `FindOrAdd(FString("SingleAgent"))`。
Python 侧拿到的 agent id 就是它，不要另外起名字。

### 必须实现的方法（`BlueprintNativeEvent`，所以带 `_Implementation` 后缀）

```cpp
// 单智能体版签名
virtual void InitializeEnvironment_Implementation(FInteractionDefinition& OutAgentDefinition) override;
virtual void SeedEnvironment_Implementation(int InSeed) override;
virtual void SetEnvironmentOptions_Implementation(const TMap<FString, FString>& InOptions) override;
virtual void Reset_Implementation(FInitialAgentState& OutAgentState) override;
virtual void Step_Implementation(const FInstancedStruct& InAction, FAgentState& OutAgentState) override;
```

多智能体版把「单个 ref」换成「`TMap<FString, ...>`」，方法名相同。

- `SeedEnvironment` / `SetEnvironmentOptions` **只在 Python 侧显式传了 seed/options 时才被调用**，可以空实现。
- `Reset` 里除了返回观测，也可以填 `Info`。
- `Step` 里填 `Reward` / `bTerminated` / `bTruncated` / `Observations`。

---

## 2. Space 与 Point 的对应

**Space 描述"范围"（在 `InitializeEnvironment` 里定义），Point 装"实际数值"（在 `Reset`/`Step` 里填）。**

| 语义 | Space 类型（`Spaces/`） | 构造 | Point 类型（`Points/`） | 取值字段 |
|---|---|---|---|---|
| 连续向量 | `FBoxSpace` | `Add(float Low, float High)` — 每个维度调一次 | `FBoxPoint` | `TArray<float> Values`（+ `TArray<int> Shape`） |
| 单离散 | `FDiscreteSpace` | `Add(int)` | `FDiscretePoint` | `int Value` |
| 多离散 | `FMultiDiscreteSpace` | `Add(int DimSize)` — 每个维度调一次 | `FMultiDiscretePoint` | `TArray<int> Values` |
| 多二值 | `FMultiBinarySpace` | — | `FMultiBinaryPoint` | — |
| 字典 | `FDictSpace` | — | `FDictPoint` | — |

典型写法：

```cpp
// InitializeEnvironment 内
TInstancedStruct<FSpace>& ObsInst = OutAgentDefinition.ObsSpaceDefn;
ObsInst.InitializeAs<FBoxSpace>();
FBoxSpace& Obs = ObsInst.GetMutable<FBoxSpace>();
Obs.Add(-1.f, 1.f);   // dim 0
Obs.Add(-1.f, 1.f);   // dim 1

TInstancedStruct<FSpace>& ActInst = OutAgentDefinition.ActionSpaceDefn;
ActInst.InitializeAs<FMultiDiscreteSpace>();
FMultiDiscreteSpace& Act = ActInst.GetMutable<FMultiDiscreteSpace>();
Act.Add(5);           // 单个维度，5 个选项

// BuildObservation 内
OutObservation.InitializeAs<FBoxPoint>();
FBoxPoint& Box = OutObservation.GetMutable<FBoxPoint>();
Box.Values = { X, Y };
Box.Shape  = { 2 };   // 显式设 Shape，别只设 Values
```

`FInteractionDefinition` 还有个 `FString AgentType`：非空时可用于**多智能体共享 policy**（第 8 轮会用到）。

### ⚠️ 动作 payload 的类型容错

`FInstancedStruct` 里装的**具体 Point 类型由训练侧序列化决定**，可能与 Space 声明的类型不完全一致
（官方 `BasicTestEnvironment` 就是拿 `FMultiDiscreteSpace(3)` 配 `FDiscretePoint`）。
**用 `GetPtr<T>()` 逐个探测，不要直接 `Get<T>()`**，否则类型不匹配会崩：

```cpp
if (const FMultiDiscretePoint* Multi = InAction.GetPtr<FMultiDiscretePoint>()) { /* Multi->Values[0] */ }
else if (const FDiscretePoint* Discrete = InAction.GetPtr<FDiscretePoint>())  { /* Discrete->Value  */ }
else { /* 记 warning，当作无效动作 */ }
```

---

## 3. 环境怎么被发现：必须是 Actor，放关卡里

`UAbstractGymConnector::CollectEnvironments`（`ScholaTraining/Private/.../AbstractGymConnector.cpp`）：

```cpp
UGameplayStatics::GetAllActorsWithInterface(GetWorld(), UBaseScholaEnvironment::StaticClass(), TempEnvArray);
```

→ **扫描当前世界里所有实现了该接口的 Actor**。

`PrepareEnvironments` 按接口类型分派到 `TScholaEnvironment<T>` 包装器；认不出接口就只打 warning 并跳过。

**结论：**
- 环境必须是 **AActor**（不能是普通 UObject / Subsystem）。
- 关卡里必须存在这个 Actor，否则 `Collected 0 environment(s)`，Python 侧拿不到 agent。
- 一个 actor 可以既是环境又是连接管理器（官方 Tag 的做法：`TagEnvironment` 继承 `AGymConnectorManager` 并实现多智能体接口）。

---

## 4. 连接管理器与连接器

```cpp
// ScholaTraining/Public/TrainingUtils/GymConnectorManager.h
UCLASS()
class AGymConnectorManager : public AActor
{
    UPROPERTY(EditAnywhere, Instanced, BlueprintReadWrite, Category="Schola|Training")
    UAbstractGymConnector* Connector = nullptr;

    virtual void Tick(float) override;   // Connector->Step()
protected:
    virtual void BeginPlay() override;   // Connector->CollectEnvironments(...) + Init(...)
};
```

实现（`GymConnectorManager.cpp`）就 7 行逻辑：

```cpp
void AGymConnectorManager::BeginPlay()
{
    Super::BeginPlay();
    if (Connector)
    {
        TArray<TScriptInterface<IBaseScholaEnvironment>> Environments;
        Connector->CollectEnvironments(Environments);
        Connector->Init(Environments);   // -> PrepareEnvironments -> InitializeEnvironment
    }
}
void AGymConnectorManager::Tick(float DeltaTime) { Super::Tick(DeltaTime); if (Connector) Connector->Step(); }
```

### gRPC 连接器

| 项 | 值 |
|---|---|
| 类 | **`URPCGymConnector`**（`ScholaProtobuf` 模块，`UCLASS(EditInlineNew)`） |
| 头 | `ScholaProtobuf/Public/GymConnectors/gRPC/gRPCGymConnector.h` |
| 默认地址 | `FRPCServerSettings::Address = "127.0.0.1"` |
| 默认端口 | `FRPCServerSettings::Port = **8000**` |
| 命令行覆盖 | `-ScholaPort=<n>`（`FParse::Value`，`gRPCGymConnector.cpp:48`） |
| 禁用自动起脚本 | `-ScholaDisableScript` |

C++ 里可直接在构造函数建默认连接器（无需在编辑器 Details 里配）：

```cpp
Connector = CreateDefaultSubobject<URPCGymConnector>(TEXT("ScholaConnector"));
```

`bRunScriptOnPlay` 默认 **false** → UE 侧只监听端口，由 Python 侧主动连接。这正是我们要的模式。

---

## 5. 三种 simulator：决定"要不要人工点 Play"

位置：`Resources/python/schola/core/simulators/unreal/`

| CLI 子命令 | Python 类 | 管理 UE 进程 | 人工操作 | 备注 |
|---|---|---|---|---|
| `editor` | `UnrealEditor` = `ExternalSimulator` | **否** | **必须手动点绿色三角启动 PIE**，每次都要 | 只连接已运行的编辑器 |
| `executable` | `UnrealExecutable` | 是（`subprocess.Popen`） | 无 | 需先打包；支持 `-nullRHI` |
| `project` | `UnrealProject` | 是（先 cook/package 再起） | 无 | `use_cached_build=False` → **每次训练都全量 build** |

`UnrealExecutable::make_args()` 实际拼的参数：
`-UNATTENDED` +（`-nullRHI` 或 `-WINDOWED`）+ `[map]` + `-LOG` +（`-BENCHMARK -FPS=n`）+ `-ScholaDisableScript` + `-Schola<key>=<value>`

→ **要无人值守训练：先打包一次，走 `executable`；或直接用 `project`。**

### CLI 形态

```bash
schola sb3  train {ppo,sac} {editor,executable,project} [...]
schola rllib train {ppo,sac,impala,appo} {editor,executable,project} [...]
```

`executable` 要 `--executable-path <PATH>`；`project` 要 `--uproject-path <PATH>`；
公共开关：`--port` / `--headless` / `--fps` / `--map /Game/...` / `--display-logs` / `--disable-script`。

---

## 6. 最小 C++ 环境骨架

```cpp
UCLASS()
class APursuitAIEnv : public AGymConnectorManager, public ISingleAgentScholaEnvironment
{
    GENERATED_BODY()
public:
    APursuitAIEnv();                       // RootComponent + CreateDefaultSubobject<URPCGymConnector>

protected:
    virtual void InitializeEnvironment_Implementation(FInteractionDefinition& OutAgentDefinition) override;
    virtual void SeedEnvironment_Implementation(int InSeed) override;      // 可空
    virtual void SetEnvironmentOptions_Implementation(const TMap<FString,FString>&) override; // 可空
    virtual void Reset_Implementation(FInitialAgentState& OutAgentState) override;
    virtual void Step_Implementation(const FInstancedStruct& InAction, FAgentState& OutAgentState) override;
};
```

本项目实测可用的实现见 `Source/PursuitAI/PursuitAIEnv.{h,cpp}`。

**Build.cs 依赖**（缺一不可）：

```csharp
PrivateDependencyModuleNames.AddRange(new string[] { "Schola", "ScholaTraining", "ScholaProtobuf", "Projects" });
```

- `Schola` → Space / Point / 数据类型
- `ScholaTraining` → 环境接口 + `AGymConnectorManager`
- `ScholaProtobuf` → `URPCGymConnector`（会连带引入 `gRPC`、protobuf 第三方头，编译变慢是正常的）
- `Projects` → 读插件版本（`PursuitAI.ScholaStatus` 自检命令用）

---

## 7. 生命周期（一次训练里各回调的顺序）

```
关卡加载
  └─ APursuitAIEnv::BeginPlay
       └─ AGymConnectorManager::BeginPlay
            ├─ Connector->CollectEnvironments()   // 扫世界，找到本 actor
            └─ Connector->Init(Envs)
                 ├─ PrepareEnvironments()          // 包成 TScholaEnvironment<T>
                 ├─ env->InitializeEnvironment()   // ← 定义 obs/action space
                 ├─ 起 gRPC server（Port=8000 或 -ScholaPort=N）
                 ├─ Publish(TrainingDefinition)     // Python 侧拉取 agent 定义
                 └─ 等 GymConnectorStartRequest
每帧 Tick
  └─ Connector->Step()
       ├─ 阻塞等 Python 的 StateUpdate（动作 或 reset 指令）
       ├─ HandleStep / HandleReset
       │    └─ env->Step() 或 env->Reset()
       └─ SubmitState() → 观测/奖励/done 回给 Python
```

`EConnectorStatus`：`NotStarted` → `Running`（收到 start request 后）→ `Closed` / `Error`。
`EAutoResetType` 默认 `SameStep` → 一个 episode 结束时**同一步内自动 reset**，环境不需要自己处理。
（这条只对**训练侧**成立。推理侧没有任何东西替你复位，见下。）

---

## 8. 推理侧：训练和推理是两套独立的抽象

官方指南：`Plugins/Schola/Docs/Sphinx/guides/setting_up_inference.rst`

**这是最容易踩空的地方**：Schola 的训练接口和推理接口**没有任何继承或适配关系**，
是两套平行设计。只实现 `IBaseScholaEnvironment` 的话，训练完的模型在引擎里**没有任何东西会去驱动它**。

| | 训练侧 | 推理侧 |
|---|---|---|
| 接口 | `IBaseScholaEnvironment` | `IAgent`（`Schola/Public/Agent/AgentInterface.h`）|
| 方法 | `InitializeEnvironment` / `SeedEnvironment` / `SetEnvironmentOptions` / `Reset` / `Step` | `GetStatus` / `SetStatus` / `Define` / `Observe` / `Act` |
| 驱动者 | `AGymConnectorManager` + gRPC | `USimpleStepper`（或 `UPipelinedStepper`）+ `UNNEPolicy` |
| 模块 | `ScholaTraining` | `ScholaNNE` + `ScholaInferenceUtils` |
| 谁管 episode 复位 | **连接器**（`EAutoResetType::SameStep`） | **你自己**，在 `Act` 里 |
| 谁算奖励 | 你，在 `Step` 里 | 没人算，推理不需要 |

### 三件套

1. **Agent** —— 实现 `IAgent` 的任意 UObject（Actor / Component 都行）
2. **Policy** —— `UNNEPolicy`，加载 `UNNEModelData` 并在 NNE 上跑
3. **Stepper** —— `USimpleStepper`（同步；推理慢时用 `UPipelinedStepper` 重叠流水）

`USimpleStepper::Step()` 干的事就三行：

```cpp
for (Agent : Agents) IAgent::Execute_Observe(Agent, Observation);
Policy->BatchedThink(Observations, Actions);
for (Agent : Agents) IAgent::Execute_Act(Agent, Actions[i]);
```

注意 `TArray<TScriptInterface<IAgent>>` —— **stepper 收的是数组，所以多个 agent 可以共享同一个 policy**。

### ONNX 从哪来

训练时加 `--export-onnx`（默认 `False`！不加就什么都不留），或事后转换：

```bash
schola sb3 export --policy-checkpoint-path <ckpt.zip> --output-path <out.onnx> --algorithm PPO
```

`--algorithm` 的 choices：`PPO, A2C, SAC, TD3, DDPG, DQN`。

### ONNX 怎么进引擎 —— 两条路

**A. 内容浏览器导入**：把 `.onnx` 拖进去 → 变成 `UNNEModelData` 资源 → `LoadObject`

**B. 运行时从磁盘读（推荐给自动化）**：`UNNEModelData::Init` 是**公开且运行时可用**的，不是 editor-only：

```cpp
TArray<uint8> Bytes;
FFileHelper::LoadFileToArray(Bytes, *OnnxPathOnDisk);
UNNEModelData* Model = NewObject<UNNEModelData>(this);
Model->Init(TEXT("onnx"), TConstArrayView64<uint8>(Bytes.GetData(), Bytes.Num()));
```

第一个参数是**文件扩展名**，ONNX runtime 靠它挑 producer。
选 B 的话整条 训练 → 导出 → 运行 完全无头，没有编辑器步骤，也不需要往版本库塞二进制资源。

### 运行时名

| 名字 | 说明 |
|---|---|
| `NNERuntimeORTCpu` | CPU 推理 |
| `NNERuntimeORTDml` | DirectML，DX12 GPU |

需要引擎插件 **`NNERuntimeORT`**（`Engine/Plugins/NNE/NNERuntimeORT`，支持 Win64）在 `.uproject` 里启用。
`Build.cs` 依赖加 `ScholaNNE` + `ScholaInferenceUtils` + `NNE`。

### 推理时的 tick 率是个坑

stepper 是**每帧**跑的。直接挂到 `Tick` 上，60fps 就意味着智能体每秒走 60 步 —— 一个 episode 几帧就结束，
**肉眼完全看不见**。要录视频或者旁观，必须自己按时间累加节流。

### ⚠️ 坑：`SimpleStepper.h` 有 Windows 宏冲突

直接 include 这个头会报：

```
error C2039: "GetObjectW": 不是 "TScriptInterface<IAgent>" 的成员
```

因为 `windows.h` 有 `#define GetObject GetObjectW`，把 `TScriptInterface::GetObject()` 改写了。
插件自己的模块靠 include 顺序躲过去了，外部模块躲不掉。不改插件源码的绕过方式：

```cpp
#ifdef GetObject
#undef GetObject
#endif
#include "Steppers/SimpleStepper.h"
```

### ⚠️ 坑：`TInstancedStruct<T>` 和 `FInstancedStruct` 不是继承关系

两者**内存布局相同但类型无关**，靠 `reinterpret_cast` 互转，不能直接赋值或传参。
`IAgent::Observe` 给的是 `FInstancedStruct&`，而训练侧 `FAgentState::Observations` 是
`TInstancedStruct<FPoint>`。用 `Common/InstancedStructUtils.h` 的：

```cpp
BuildObservation(ToTypedInstancedStruct<FPoint>(OutObservations));  // FInstancedStruct -> 有类型
BuildObservation(ToUntypedInstancedStruct(OutObservation));         // 有类型 -> FInstancedStruct
```

### 想同时支持训练和推理，就把两套接口都实现在同一个 Actor 上

这是本项目的做法（见 `PursuitAIEnv.h` 顶部注释）。要点是**两边共用同一份实现**：

- `DefineSpaces()` —— 唯一描述空间的地方，`IAgent::Define` 和 `InitializeEnvironment` 都调它
- `BuildObservation()` —— 唯一算观测的地方，`Observe` / `Reset` / `Step` 都调它
- `ApplyActionVector()` —— 唯一移动代码，`Act` / `Step` 都调它

否则推理侧会照着训练侧的逻辑再写一遍，两边一改就悄悄不同步 —— **这是 train/deploy 分裂最典型的静默失败**：
模型在训练时看到的观测布局和推理时喂进去的不是同一个，策略看起来"完全没学会"，但两边都不报错。

### 其他出口

| 用法 | 模块 / 类 |
|---|---|
| 接进 StateTree 当行为树节点 | `ScholaStateTree`：`StateTreeTask_StepInference` + `StateTreeEvaluator_RLDecision` |
| 人类示范 / 模仿学习 | `ScholaImitation`：`ImitationPlayerController` 录操作，环境接口 `IImitationScholaEnvironment` |
| 多智能体共享一个模型 | `USimpleStepper::Init(TArray<IAgent>, Policy)` |
| 搬出 UE | ONNX 是通用格式：Unity(Sentis)、Godot、onnxruntime-web、纯 Python |

