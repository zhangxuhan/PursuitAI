# 可玩追捕 Demo（默认场景：Cartoon City / Demonstration）

这份文档只讲一件事：**不训练、不加载模型，直接进项目操控一个角色、被 4 个 AI 追**。
要的是「十秒钟之内用手感判断这套追捕好不好玩」，这是训练曲线回答不了的问题。

> **默认场景已换成 Cartoon City 的 `Demonstration`**（一个手工搭建的城市场景），
> 不再是脚本生成的 `L_PursuitPlay` 竞技场。两者都能玩，区别见第 3 节末尾。

训练那一整套流程（`L_PursuitAITrain` + Python + 导出 + 引擎内推理）完全没动，见 `TUTORIAL.md`。

---

## 1. 怎么跑起来

### 一键（推荐）

```
tools\play.bat
```

它会做三件事：关卡不存在就先生成、编译编辑器目标、用 `-game` 起窗口。
关掉窗口即结束。

### 自定义开关

多余的参数会原样透传给游戏，所以不用改脚本：

| 命令 | 效果 |
| --- | --- |
| `tools\play.bat` | 正常玩，你操控红色玩家 |
| `tools\play.bat -PursuitAutoPlay` | **自动演示**：玩家自己逃跑、自己跳，你只负责看 |
| `tools\play.bat -PursuitNoHud` | 关掉屏幕左上角的调试文字，方便截图 |
| `tools\play.bat -PursuitAutoPlay -PursuitNoHud` | 干净的自播放画面 |
| `tools\play.bat -PursuitAutoPlay -PursuitRunSeconds=120` | 自动演示跑 120 秒后**自己退出** |
| `tools\play.bat -PursuitJumpTest -PursuitJumpTestSeconds=45` | **确定性跳跃测试**，见第 5 节第 7 条 |
| `tools\play.bat -PursuitCatchRadius=70` | 改水平捕获半径（默认 85） |
| `tools\play.bat -PursuitCatchHeight=0` | 改垂直"同层"容差（默认 70）；**0 = 关掉**这道判据 |
| `tools\play.bat -PursuitPlayerAt=900,2120,400` | 把玩家钉在指定坐标出生（**x,y,z**），用来复现"站在平台上"这类只在特定位置发生的现象 |
| `tools\play.bat -PursuitCamera=first` | 强制视角，见下面的「三个视角」 |
| `tools\play.bat -PursuitGodPitch=-88` | 上帝视角的俯角（默认 -82，**下界 -89.5**） |
| `tools\play.bat -PursuitGodYaw=90` | 上帝视角的方位角（默认 45，固定不转） |

后三个开关是给无头验证用的，理由见第 5 节第 8 条（进程要能自己结束）。
用 `tools\run_play_headless.ps1` 可以直接在无窗口下跑同一个组合：

```
tools\run_play_headless.ps1 -Extra "-PursuitAutoPlay -PursuitRunSeconds=150" -Log <日志路径>
```

### 在编辑器里玩

`PursuitAI.uproject` 的默认关卡现在是 **Cartoon City 的 `Demonstration`**（`Config/DefaultGame.ini`
里的 `EditorStartupMap` / `GameDefaultMap` 都指向它），打开编辑器直接按 Play 就是同一场景。
游戏模式 `APursuitPlayGameMode` 是以**逐关卡覆盖**的方式钉在 `Demonstration` 上的
（`tools/set_demo_gamemode.py`），所以训练图 `L_PursuitAITrain` 完全不受影响、看不到角色和追逐者。

> 想回到纯生成的竞技场？把 `tools\play.bat` 里的 `MAP` 改回 `/Game/Maps/L_PursuitPlay` 即可，
> 那份地图的生成脚本是 `tools/gen_play_level.py`，规则见第 3 节。

### 操作

| 按键 | 动作 |
| --- | --- |
| WASD | 移动 |
| 空格 | 跳 |
| Shift | 冲刺（620 → 980 cm/s） |
| 鼠标 | 转视角 |
| F11 | 全屏 |
| Alt+F4 | 退出 |

---

## 1.1 三个视角

`EPursuitCameraMode` 有四个取值，`-PursuitCamera=god|follow|first|auto` 覆盖：

| 模式 | 是什么 | 谁会拿到 |
| --- | --- | --- |
| `FirstPerson` | 眼睛高度、挂在角色上、鼠标转视角 | **编辑器进程（含 PIE）** —— 即 `Auto` 的默认 |
| `Follow` | 弹簧臂第三人称，在角色身后 420 cm | `tools\play.bat`（它尾部追加 `-PursuitCamera=follow`） |
| `God` | 脱离角色的上帝视角，高空俯视追逐 | **其它一切**（`-game`、打包），即 `Auto` 的默认 |

**`Auto` 的判据是 `GIsEditor && !FApp::IsGame()`** —— 编辑器里为真（PIE 也算），
`UnrealEditor.exe -game` 和打包为假。所以 `tools/record_city.py` 不用开口就拿到上帝视角，
而双击工程按 Play 的人拿到眼睛高度那台。

### 上帝视角的遮挡问题（这一节是本轮的全部内容）

城市里**最高楼 5739 cm**，`PlayerStart` 在约 30 m 宽的街道里。任何**有倾角**的相机，
只要镜头离角色够远，就一定会被附近的楼挡——这是结构性的，不是参数没调好。

- **水平前伸 = `距离 × cos(俯角)`**，这个量就是"楼有多少机会挡进来"：默认 2900 cm @ −82° → **404 cm**，
  所以约 3 m 内的楼会挡。
- 被挡时**只把角度变陡，绝不缩短手臂**。缩短手臂会沿同一条被挡的射线把相机拉回去，
  曾经把镜头停在它刚测到的那堵墙**背面**（整屏橙色）——因为手臂下限 1600 cm 比障碍物 719 cm 还远。
- 陡角阶梯 `{0, −3, −6, −8}` → **−82 / −85 / −88 / −89.5**，取**第一个解**。
  阶梯之所以要密：只有 −4/−8 两档时，30 s 录像里有 **11/31 秒**被顶到最平的 −89.5，
  三分之一的时间在垂直往下看，是城市最难看的角度。
- **−89.5 是可证明无遮挡的那一档**：前伸 `2900 × cos(89.5°) = 25 cm`，
  而角色胶囊半径是 **34 cm** → 镜头至少离墙面 **9 cm**，**无论角色贴着哪面墙，相机都不可能在墙里**。
  所以**不需要**"沿命中法线把镜头推出去"那种补丁（曾经有，已删）。
- 探针是**5 条射线**（中心 + 上下左右 130 cm）**取多数**，而不是 1 条：问的是"**主体**被挡住没有"，
  不是"射线有没有碰到东西"。单条射线会为红绿灯杆和棕榈树抖动（实测一次 25 s 的录像里有 3 s 给了红绿灯）。
  通道用 `ECC_Visibility`（城市墙面挡的就是它），角色自己靠 ignore actor 排除。
- 探针的偏移方向是**视图向量的反向**（`-ViewRotation.Vector()`）。**符号是要命的**：
  `FRotator::Vector()` 是前向向量，俯视时指向地面，沿它打就是把探针指向马路，
  第一厘米就命中最底层，日志里于是写着 `SM_road_001_578 挡住了` —— 它确实挡住了，因为探针就是指着它的。

参数与推导在 `PursuitCharacter.h` 的 `GodCameraPitch` / `GodCameraYaw` 注释里（含两次"推理出来的安全值"实测都拍到了墙）。

### 录像怎么验收

```bash
.venv/Scripts/python.exe tools/record_city.py --seconds 30 --camera god \
    --output logs/portfolio/god_chase_30s.mp4
.venv/Scripts/python.exe tools/analyze_clip.py logs/portfolio/god_chase_30s.mp4 --fps 2
```

`analyze_clip.py` 是必需的：本机把 PNG 读回来会得到 **"Content filtered"**，肉眼看逐帧不可行，
所以片子只能**量**。硬判据两条，都是真发生过的故障：
`flat`（单色桶占比，>0.88 = 镜头在墙里）和 `luma`（<12 = 关卡没加载的黑屏）。
`green`/`red` 两列只是**参考**，不是判据 —— Cartoon City 满图绿树红车，
实测绿最多 297000 px（那是公园不是角色）、红从没低过 900（那是车）。
参考值：合格的一条 **flat 峰值 0.32 / luma 最低 104**。

---

## 2. 场景里有什么

一个 36 m 见方的围墙场地，中段有四块长挡板、五根圆柱、四角各一个矮箱。
矮箱是能跳上去也能跳过去的，圆柱是给追逐者的「触须」避障发挥作用的。

- **红色 = 你**，620 cm/s 走、980 cm/s 冲刺，`bUseControllerRotationYaw`，
  相机是挂在弹簧臂上的第三人称跟随。
- **绿色 = 4 个追逐者**，575 cm/s（故意比玩家略慢一点 —— 完全同速的追逐者是一堵墙），
  由 `AAIController` 接管，逻辑在 `APursuitCharacter::TickChaser`：
  朝玩家转向 + `ProbeObstacle` 射线探障碍 + `SteerAroundObstacles` 绕开 +
  追逐者之间 `SeparationPush` 互相推散。
- 抓到就记一次数，1.5 秒后双方各自回到初始位置重新开始，不用重开地图。判定是**两个数**：

  | | 值 | 含义 |
  | --- | --- | --- |
  | 水平 `CatchRadius` | 85 | **表面间隙 ≤ 17 cm**。原先 130 看着像「隔一条街抓到你」，因为**两个胶囊半径都是 34** —— 130 是中心距，留给表面的只有 62 cm。85 才是「碰到」 |
  | 垂直 `CatchHeightTolerance` | 70 | **同层**才算抓到。站在高于 70 cm 的地方，地面上的追逐者抓不到你 |

  两个都能按次覆盖、不用重编：`tools\play.bat -PursuitCatchRadius=70`、
  `tools\play.bat -PursuitCatchHeight=0`（0 = 关掉高度判据，恢复旧的"无限高圆柱"行为）。
  垂直这条的来龙去脉见第 5 节第 14 条 —— 它是在"站在平台上还是被抓"那个 bug 上加的。
  ⚠️ `APursuitAIEnv::CatchRadius = 50` 是**另一套**（训练环境），不受这两个开关影响。

**追逐者会跳。** 不是「看见障碍就跳」，而是先量高度再决定：脚上方 45 cm 以内
（`MaxStepHeight`，胶囊本来就免费迈得过去）的不跳，高于 120 cm（`MaxJumpHeight`）的当墙绕开，
**只有中间这一段（45–120）才起跳**。这段区间的几何依据和实测数据见第 5 节第 7 条。

> **120 不是口味，是物理上限，而且 150 曾经是个陷阱。**
> 跳跃弧顶 = `JumpZVelocity² / (2 × 980 × GravityScale)` = 640² / (2 × 980 × 1.6) = **131 cm**。
> 跳跃门必须设在弧顶之下。设在之上，它就把「跳得上去」和「跳不上去」合并成了一类：
> AI 会**决定跳**、然后**跳不过去**、最后顶着障碍速度归零 —— 那是卡死，不是绕开。
> 原来的 150 让**整个 121–150 区间**都变成了这种陷阱，而第一版跳跃平台正好被摆在那里面。
> 所以高度在这里是**开关**不是难度：44 cm 看不见、46 cm 要跳、121 cm 是墙，
> 真正有意思的区间只有 **75 cm 宽**。
>
> 推论：想让平台变「高」，**加高度是错的杠杆**（过了 120 就变成墙，只教会 AI 绕开）。
> 正确做法是**叠** —— 见 `tools/gen_city_obstacles.py` 里那两座三层塔：
> 300 cm 的顶不是跳上去的，是三跳 100 cm 爬上去的。

HUD 左上角三行：操作提示、追逐者数量 / 被抓次数 / 最近距离 / 当前速度 / 是否着地、
以及「CAUGHT」提示。

> HUD 下面永远跟着一行极淡的灰字 `'DisableAllScreenMessages' to suppress`。
> 那不是警告，是引擎在**任何** on-screen debug 文字下面都会画的提示
> （`UnrealEngine.cpp`，颜色 0.05/0.05/0.05，所以看着像一块脏东西）。
> 想彻底去掉就得把 HUD 改成 canvas 绘制，不值得。

### 跳跃是怎么判定的

三根探针，各问一个问题，缺一根就会出现一类"AI 看不见障碍"的假象：

| 探针 | 高度（脚上方） | 问什么 |
| --- | --- | --- |
| 胸口梁 `AvoidTraceHeight` | 158 | 「有什么挡着不让走」—— 墙、楼、柱子在这里 |
| 低位梁 `LowAvoidTraceHeight` | **6** | 「脚踝高度有什么」—— 台阶唇、路缘、铺装接缝 |
| 跳跃面 `JumpFaceProbeHeight` | **90** | 「45–120 的跳跃带里有什么正面」 |

胸口梁单独用是不够的，而且**错得很有欺骗性**：一根 61 cm 的台阶完全位于它下方，
它照直飞过去报「前方畅通」，`SteerAroundObstacles` 于是说路是通的，
追逐者一头撞上去，移动组件拒绝爬过 `MaxStepHeight`，于是它**顶在原地速度归零、
嘴里报着「face 300 cm」**—— sincerely，因为它胸口前方 300 cm 内确实什么都没有。

低位梁为什么是 6 而不是 20：胶囊底部是**圆的**，它最低的接触发生在任何 20 cm 探针之下，
而城里 3–6 cm 的铺装接缝就足以把它卡死。6 而不是 0 则是因为贴地打会擦到地面本身。

跳跃面在 90 而不是在胸口：**要跳的东西全部位于胸口梁之下**。
用胸口梁去找障碍的「正面」，它会从一只腰高的箱子上方飞过去、打在后面的楼上，
于是跳跃逻辑拿到的是**另一栋楼的**高度。日志因此自洽而结论全错——
"face" 和 "top" 是在诚实地报告两个不同的物体。

---

## 2.1 主角与追逐方的生成配置

开局时由 `APursuitPlayGameMode::BeginPlay` 完成场景与角色的初始化（**不触发训练**）。
关键参数都在游戏模式的 `Pursuit|Play` 分类下，可直接在编辑器细节面板调，也都能在 `PursuitPlayGameMode.h` 里改默认值：

**主角（玩家，默认用 RPG Hero Squad 的 Tiny Hero）**
- 出生点逻辑 `ResolvePlayerSpawn()`：优先用关卡里的 `PlayerStart`；勾了 `bUseConfiguredSpawn`
  就忽略 `PlayerStart`、强制用 `PlayerSpawnLocation`；连 `PlayerStart` 都没有时退回
  `PlayerSpawnLocation` 并报警告（不会掉到世界原点）。
- `Demonstration` 实测只有 1 个 `PlayerStart`，位于 `(-330, 0, 162)`，追逐者就环绕这个点生成。

**追逐方（默认 RPGHero + AnimalHero 犬，轮流分配）**
- `ChaserCount`（默认 4）、`ChaserSpawnRadius`（默认 950 cm）决定环绕圈。
- `bCenterChasersOnPlayer`（默认开）：追逐者环绕**玩家出生点**而不是世界原点，
  这样在城市场景里玩家不在 (0,0) 也能正常开局。
- `ChaserHeroA` / `ChaserHeroB`（默认 `RPGHero` / `AnimalHero`）：偶数索引用 A、奇数用 B，
  玩家永远是 Tiny Hero（唯一带跳跃动画的骨架）。
- 行为逻辑在 `APursuitCharacter::TickChaser`：朝玩家转向 + `ProbeObstacle` 射线探障碍 +
  `SteerAroundObstacles` 绕开 + 追逐者之间 `SeparationPush` 互相推散；抓到（≤ `CatchRadius` 85 cm）
  计数后双方回初始位重开。

> 这套生成配置是**按关卡**做的，只作用于 `Demonstration`。训练图 `L_PursuitAITrain`
> 走的是 `APursuitAIEnv`，本文件第 6 节的关系不变。

---

## 3. 改地图：两条路和一个约定

先解掉一个可能的误解：这张地图**不是引擎 demo 改的，也不是手工摆的**。
`tools/gen_play_level.py` 从**空白关卡**起手（`new_level()`），一块一块搭出来 ——
几何体全是引擎自带的 `Cube` / `Cylinder`，材质是本项目生成的 `/Game/Materials/M_PursuitTint`。
它已经是正常的项目资产（`Content/Maps/L_PursuitPlay.umap`，没有被 `.gitignore` 排除），
跟着版本控制走。

### 路径 A：改脚本，重新生成（推荐）

`LAYOUT` 就是一张表，一行一个 actor：

```python
(   "PP_Crate_NE",  CUBE,  (1350.0, 1350.0, 75.0),  (3.0, 3.0, 1.5),  0.0)
#    ↑ 标签              ↑网格   ↑ 位置 (x, y, z)          ↑ 缩放          ↑ yaw
```

- **单位**：`Cube` / `Cylinder` 的边长都是 100 cm，所以**缩放值 = 米数**；位置单位是 cm。
- **地板** `PP_Floor` 是 40 m 见方、顶面在 z = 0 的板，所以其他物体的 z 从 0 起算。
- **围墙**是 36 m 见方（±1800），高 300。
- **出生点**改 `PLAYER_START` 和 `PLAYER_START_YAW`。
- 旋转一律用关键字参数（`unreal.Rotator(yaw=...)`），原因是第 5 节第 1 条。

改完重跑：

```
tools\play.bat -regen
```

`-regen` 是这个脚本**自己的**开关（不会透传给游戏），作用是强制重新生成再启动。
不加它的话，地图只在**缺失时**才生成 —— 改了脚本却什么都没变，就是这么来的。

### 路径 B：在编辑器里手摆

可以。规则见下。

### 约定只有一句：`PP_` 前缀

> **`PP_` 开头 = 生成器的地盘，别手改；其他 = 你的，脚本不碰。**

生成器重跑时删掉的是「**标签以 `PP_` 开头**，且属于 `StaticMeshActor` /
`DirectionalLight` / `SkyLight` / `SkyAtmosphere` / `PostProcessVolume` / `PlayerStart`
这几类」的 actor。于是：

- 你手摆的东西（默认标签 `StaticMeshActor_0` 这类）**会被保留**。
- 你手工挪过的 `PP_` 物体，下次重跑**会被还原到脚本里的坐标**。
  想永久改就改脚本，别在编辑器里拖。
- 手工摆的东西想交给生成器管，把标签改成 `PP_xxx` —— 那等于自愿接受删除和重建。

> **这条规则不是洁癖，是补洞。** 手摆的 actor 默认就是 `StaticMeshActor`，
> 恰好是生成器要重建的那个类，所以早期那份「只按类匹配」的删除逻辑
> 会在第一次重跑时**静默删掉你手搭的整个关卡**。
> 现在改成「类 + 前缀」双条件，并且**有测试**：
> `tools/verify_handmade_survives.py` 会建一个临时关卡，摆两个自己的 actor 和一个
> `PP_` 开头的诱饵，然后重跑生成器，检查该活的活、该删的删、场地数量不变。
> 实测 `RESULT PASS (0 failure(s))`。测试全程在临时关卡（`/Game/Maps/_VerifyHandmade`）上做，
> 不碰真地图，跑完自己删掉。

### 跑生成脚本前把编辑器关掉

UE 编辑器打开 `.umap` 时会**持有文件锁**，此时无头脚本保存会失败：

```
LogFileManager: Error: Error moving file '.../Content/Maps/L_PursuitPlay.umap' ...
LogSavePackage: Error: Error saving '.../Content/Maps/L_PursuitPlay.umap'
```

日志里的 `Error Code 32` 就是共享冲突。实测过一次：22:48 打开编辑器 → 22:50 跑生成 →
保存失败。材质实例（`MI_PlayFloor` / `MI_PlayObstacle`）同理写不进去，不过那只是颜色值，
是上次写好的，画面不受影响。

---

## 4. 角色资源：现在是什么，Fab 素材怎么换

### 现在用的是引擎自带的白模

`SKM_Manny_Simple` + 5 段动画（Idle / Walk_Fwd / Run_Fwd / Jump / Fall_Loop），
来自引擎的 `MoverExamples` 插件（`PursuitAI.uproject` 里已启用）。
好处是零美术成本、开箱即用；坏处是它就是个白模。

### 换成 Fab 上的 RPG Hero Squad

那份包（`fab.com/listings/fcc5b0f4-feb2-4868-8642-60d1a26fcdea`）是 **UE 格式**（不是 FBX），
走 Fab / Epic Launcher 分发，**需要在你自己的账号里点下载**，我这边没法替你拉。
**它现在不在本机磁盘上**（已确认 VaultCache 里没有）。

下载之后替换只是改属性，不用改结构 —— 这正是当初把资源路径写成
`TSoftObjectPtr` 的原因（`PursuitCharacter.h`）：

1. 编辑器里选中游戏模式或直接改 `PursuitCharacter.cpp` 构造里的默认值：
   `CharacterMesh` → 新的 `SK_xxx`；`IdleAnim / WalkAnim / RunAnim / JumpAnim / FallAnim` → 对应的动画序列。
2. 角色网格朝向：本工程假定骨架朝向 +Y，网格组件在构造里转了 -90°。
   如果新骨架朝向不同，改 `SetRelativeRotation` 那一行。
3. 胶囊尺寸（现在 34 × 88）按新角色身高调 `InitCapsuleSize`。
4. 起跑后看日志：正常会打印
   `PursuitCharacter: <名字> is a player - mesh SK_xxx, 5/5 clips, 620 cm/s`。
   `5/5` 变成 `4/5` 就说明某一段动画路径没对上。

**顺带一提**：本机 VaultCache 里已经有一份可直接用的角色内容 ——
`AnimeCharacters`（194 个 uasset，4 个 `SK_` 骨骼网格 + Animations 目录），
在 `C:\ProgramData\Epic\EpicGamesLauncher\VaultCache\AnimeCha9393f4511679V1\data\Content\`，
是解包好的 UE 内容，拷进 `Content\` 就能用。想先看效果的话拿它替换更快。

### 为什么颜色不是灰色就算成功

角色染色走的是本工程自己的材质 `/Game/Materials/M_PursuitTint`
（一个 `Color` 向量参数），由 `tools/gen_tint_material.py` 生成。

**不能**用引擎的 `BasicShapeMaterial`：它没有 `bUsedWithSkeletalMesh` 这个 usage flag，
它的实例挂到骨骼网格上会被运行时丢弃、退回默认材质，日志里只有一行
`missing bUsedWithSkeletalMesh=True! Default Material will be used in game.` ——
而染色代码本身「看起来完全正常」，读回参数也是对的。这是本工程踩过的一个坑。

---

## 5. 这一版踩过的坑（都值得记住）

1. **Python 的 `unreal.Rotator` 参数顺序是 (roll, pitch, yaw)，不是 C++ 的 (Pitch, Yaw, Roll)。**
   写成 `unreal.Rotator(0.0, 90.0, 0.0)` 想说「yaw 90」，实际是 **pitch 90**。
   后果：东西墙被放倒，`PlayerStart` 直接朝天 —— 而控制器旋转取自 `PlayerStart`，
   于是跟随相机钻进了玩家自己的胶囊里对着天空，**截图全黑**。
   现在所有调用一律用关键字参数 `unreal.Rotator(yaw=...)`，错不了。

2. **灯光配方要和 `APursuitAIEnv` 保持一致，不要自己发明。**
   本引擎版的曝光是**固定**的（场上没有 PostProcessVolume 时 `SceneView` 直接钉 Min=Max=1），
   所以**光强就是亮度**，没有自动曝光帮你归一化。项目现有标定是
   主光 0.4 lux + 无阴影补光 0.1 lux（地板落在 157,158,160 附近），照抄它，
   两张地图的截图才可比。想调先把亮度重新量一遍。也不要加 SkyAtmosphere：0.4 lux 的太阳
   照不亮天空，而那只是给正在排查的场景又加了一个变量。

   > **诚实说明**：第一版用的是 3.2 lux + SkyLight + SkyAtmosphere，当时截图全黑，
   > 但那**不是灯光的问题**——是 `PlayerStart` 朝天（见上一条）。两个改动是一起落的，
   > 所以「到底是谁把画面变亮的」这件事**没有单独测过**。3.2 lux 是标定的 8 倍，
   > 相机修好之后大概率会过曝成白色——"大概率"就是这里不写具体数字的原因。

3. **`MM_Land` 是 additive 动画，单节点动画模式（AnimSingleNode）播不了。**
   引擎每次都会警告 `Setting an additive animation (MM_Land) on an AnimSingleNodeInstance
   is not allowed. This will not function correctly in cooked builds!`，
   而且这条警告被引擎钉在屏幕上。本工程没有别的落地动画，所以落地先不做
   （`LandAnim` 留空，落地就是 Fall 直接接回走路/跑步）；换到有非 additive 落地动画的资源时，
   把路径填回去，`LandHoldSeconds` 那段代码还在。

4. **速度阈值必须加回差，否则动画会高频抖动。**
   初版只用单阈值判断 idle/walk/run，实测出现 **180 ms 内切 6 次**
   （Idle→Walk→Idle→Walk…），屏幕上就是角色在两个姿势之间抽搐。
   根源是速度停在阈值上——推着墙走时碰撞响应把速度压在 0 附近就一直来回跨线。
   现在 `SpeedHysteresis = 60 cm/s`（高于阈值进、回到阈值出），
   腾空时上冲/下落也用双阈值 `RisingSpeed=30 / FallingSpeed=-60` 锁住，避免在跳跃顶点闪。

5. **「玩家不动」和「推着墙走」的截图长得一样。**
   所以游戏模式里加了每秒一行诊断日志（相机位置/朝向、角色 Z、是否着地、水平速度、
   自动演示当前给出的输入方向）。第一版自动演示因为把 `SizeSquared()`（4e-6）
   拿去和 `KINDA_SMALL_NUMBER`（1e-4）比较而**每帧提前 return**，
   玩家原地不动被抓 56 次 —— 就是这行日志一眼看出来的。

6. **bat 里 `%*` 不支持「取子串替换」。**
   `set "EXTRA=%*:-regen=%"` 看着像标准写法，实际**完全不替换**，而是把 `:-regen=`
   当字面量接到参数末尾（实测 `EXTRA=[-PursuitAutoPlay:-regen=]`）。
   `%VAR:old=new%` 只对真实变量有效，`%*` 是伪变量，必须先 `set "ORIGINAL=%*"` 再基于变量替换。
   这个错误很隐蔽：传给游戏的参数只多一个无意义 token，但同一行的「是否含 -regen」判断会
   **恒为真**，于是每次启动都重新生成地图（还会因此撞上编辑器的文件锁）。
   现在有测试 `tools\_test_bat_args.py`（5 项 PASS，且校验测试夹具与 `play.bat` 那几行
   逐字节一致，防止夹具漂移成"假绿灯"）。顺带实测出一条反直觉的：cmd 的变量替换
   **不区分大小写**，`-REGEN` 一样会被剥离。

7. **「AI 从来不跳」其实是三处高度错配，每一处单独看都自洽。**
   这是本项目最长的一条 bug 链，值得完整记下来，因为它演示了"日志说真话但结论全错"。

   **症状**：Cartoon City 上追逐者从不跳跃，绕过所有障碍。

   **几何背景**（一切错误的根源）：`ACharacter` 的 actor location 是**胶囊中心**，
   脚在 `Z − HalfHeight`（88）。所以 `AvoidTraceHeight = 70` 是**地面上方 158 cm**。

   | # | 缺陷 | 表现 |
   | --- | --- | --- |
   | 1 | 跳跃的「找正面」复用了胸口梁 | 梁从 90 cm 箱子上方飞过，打在后面的楼上，跳跃逻辑量的是**另一栋楼** |
   | 2 | 高度探针只有一个固定深度（25 cm） | 城里那道台阶唇只有二十几厘米厚，探针直接跨过远边落进空气 → `top none` |
   | 3 | 探针底停在脚上方 3 cm | 脚 −9 / 地 −10 / 探针底 −7 → 既打不到障碍也打不到地 → 空白 |

   三处合起来，`ProbeObstacleTop` 对城里真正的障碍（61 cm 台阶）返回「什么都没有」，
   `IsObstacleJumpable` 于是拒绝跳跃，`SteerAroundObstacles` 接手绕过去 —— 整条链
   **每一步都在诚实地执行**，只是输入是错的。

   **修法**：
   - 跳跃面改到 `JumpFaceProbeHeight = 90`（落在 45–120 跳跃带内，在胸口梁之下）；
   - 探针改成**多深度扫描**（`ProbeIntoFaceDepth=8` 起、`ProbeIntoFaceStepSize=12` 走 4 步，
     取第一个命中）—— 单深度是在赌"障碍比探针深"，而这个赌在真实地图上必输；
   - 探针底改到脚下方 `ProbeBelowFeetDepth = 60`；
   - 另加**台阶分支**（在 trigger 距离门**之前**）：从脚下和正前方 90/144 cm 处各向下打一次，
     前方地面比脚下高就是障碍。**升起的路面根本没有"正面"可言**，用找正面的方法对它无效。
     实测案例：脚 z=−9、脚下地 z=−10、前方 90 cm 处 z=+51 → **61 cm 台阶**。

   另外两个和高度无关但同样卡死追逐者的缺陷：

   - **`bJumpCommitted` 锁存**：胸口梁 300 cm 就看见障碍、跳跃只在 190 内起跳，
     中间那段绕障会把追逐者**掰离轴线**，障碍离开前方射线，于是它永远绕着它转。
     一旦判定可越，必须**禁止绕障**直到跳出去或彻底丢失目标。
   - **`SteerAroundObstacles` 的平局不能保留直线**：三根梁撞同一面**横向**墙时
     `Best` 保持直线 → 顶着墙 `vel=0`。现在直线也参与比较，平局取侧面，
     让追逐者斜着推上去、由移动组件解成**滑行**。另加：胶囊一旦接触障碍，
     转任何角度都会被拒（半径不够），所以 `StraightClear < CapsuleRadius` 时先
     减去 `AvoidBackoffWeight = 0.35` 的方向量，**先脱离接触再斜着靠近**。

   **实测结果**（Cartoon City，120 s `-PursuitAutoPlay`，障碍尚未修好锚点）：
   **71 次跳跃 / 20 次捕获 / 7 条真卡死告警**；跳跃的障碍高度分布 **47–120 cm（中位 94）**。
   注意这个上限 120：它**正好压在当时那道 150 的门之下**，说明物理弧顶早就把答案给了，
   只是门开得比弧高 —— 这是后来把 `MaxJumpHeight` 从 150 降到 120 的直接依据。
   受控 fixture（`-PursuitJumpTest`，45 s）：**5 次跳出 120 cm 箱子**、9 次捕获，可复现。

   **障碍锚点修好之后重跑**（150 s，`Saved/Logs/city_run2.log`）：

   | 指标 | 第一次（障碍悬空） | 第二次（障碍落地） |
   | --- | --- | --- |
   | 跳跃 | 71 | **213** |
   | 捕获 | 20 | **32** |
   | 真卡死告警 | 7 | **1** |
   | 跳跃高度范围 | 47–120（中位 94） | 45–120（中位 98） |
   | 四只追逐者跳跃数 | — | 86 / 49 / 36 / 42 |

   跳跃数涨 3 倍是**静态障碍真的进入了路径**的直接证据（68 次/分钟的跳跃里，
   绝大多数是反复压过那几只 `PX_` 箱）。四只都有跳跃，说明不是某一只的偶发行为。
   卡死告警从 7 条掉到 1 条，是因为原来的场地里追逐者被地形卡在狭角里、
   而现在那条通道被测试障碍改变了走法。

   **诊断日志**（`LogJumpProbe`）是这条链能查清的原因，值得保留：它把
   `face / at / trace / trigger`、`top`、`vel`、`jumpable 区间`、位置**并排**打印。
   其中 `top` 的哨兵值必须打印成 `none (downward probe found nothing)` 而不是 `-1`——
   `-1` 读起来像个**测量值**（"障碍顶在我脚下一厘米"），而它实际意思是"向下打没打到东西"。
   这个歧义让一轮排查走错了方向。

   卡死告警的门槛是**持续 3 秒**（`BlockedReportSeconds`）。逐帧版会在地图里刷 90 条噪音；
   1 秒版在 ±55° 绕行瞬间仍误报 48 条（转向时净速度本来就接近零）。3 秒才是真卡死。

8. **无头验证跑的进程必须能自己结束。**
   `UnrealEditor.exe` 留在内存里会让下一次 UBT 3 秒内以
   `Unable to build while Live Coding is active` 失败 —— 读起来像编译错误，其实不是。
   所以有两个自杀开关：`-PursuitRunSeconds=<n>`（普通脚本化跑）和
   `-PursuitJumpTestSeconds=<n>`（跳跃测试）。**依赖外部 kill 是会忘的**，忘一次查二十分钟。

   > 注意 `-PursuitRunSeconds` **不作用于 `APursuitAIEnv`** 的演示/训练路径 —— 那是另一个
   > actor 的 Tick。验证训练地图上的逃跑者时要靠外部计时。

9. **🔴 `-run=pythonscript` 这个 commandlet 里物理查询完全不可用。**
   UE 5.7.4 实测，Cartoon City 已加载、292 个 actor 在场，
   **每一个** line trace 变体都返回裸 `None`：

   ```
   unreal.SystemLibrary.line_trace_single            -> None
   unreal.SystemLibrary.line_trace_single_new         -> None
   unreal.SystemLibrary.line_trace_single_by_profile  -> None
   unreal.SystemLibrary.line_trace_single_by_channel  -> 该属性根本不存在
   world.line_trace_single / sweep_single             -> 该属性根本不存在
   ```

   测试对象是一个真实的 `StaticMeshActor`，从它自己包围盒正上方垂直穿下去 ——
   而它的组件诚实地报告 `collision_profile_name = BlockAll`。
   **裸 `None` 不是"没打到"**：没打到会返回 `(bool, HitResult)` 二元组。
   裸 `None` 意思是 wrapper 拒绝了这次调用。（这条区分方法是上一轮
   "`AStaticMeshActor` 运行时生成"那个坑留下来的，这次又用上了。）

   所以 `tools/gen_city_obstacles.py` **改用 `get_actor_bounds` 从关卡几何里量地面**，
   而不是 trace。包围盒对这座城市那些轴对齐的方块是精确的，
   `tools/inspect_demo_city.py` 已经验证过。

   顺带两条实测的坑：
   - `unreal.SystemLibrary` **没有** `line_trace_single_by_channel`——尽管某些文档这么写。
     取通道的那个调用就叫 **`line_trace_single`**，它的真实签名是从一个活的 `TypeError`
     拿到的，不是猜的。
   - **`Component.is_registered()` 会把脚本直接挂死**：没有异常、没有 traceback、
     没有任何后续输出。一个会静默停住的诊断比一个会抛异常的诊断更难查。

10. **"最高的东西"不等于"能站的东西"。**
    `gen_city_obstacles.py` 第一版用「脚下最高的静止网格」当地面，结果把障碍
    放到了**两辆车的车顶**上（`PX_Kerb_Low` on `SM_Car_06@z=163`、
    `PX_Gate_A` on `SM_Car_16@z=148`）—— 因为车的包围盒确实是那两个点上最高的东西。
    盒子放得很准，而且完全没用：**没有追逐者在车顶上走**。

    修法是两道独立的闸：一个"可站立高度带"（以全图几何中位数为基准 ±250/−150），
    **加上**按名字排除车辆（`Car`/`Van`/`Truck`/`Bus`/`Bike`/`Motor`）。
    为什么两道都要有：车顶只比中位数高 63 cm，**高度带根本拦不住它**。

    这条和 fixture 里那个跳箱的教训是同一个：**fixture 必须真的在它被测的位置上**。
    原来那版把障碍锚在 `PLAYER_START_Z = 162`（PlayerStart 的 z，也就是站那儿的胶囊中心），
    而广场地面在 **z = 10** —— 整个测试场地悬在半空中 172 cm，从任何人的头顶飘过去。
    生成脚本老老实实记录了每个 actor 的高度（"height=90" 对一只 90 cm 的箱子），
    地图也保存成功；唯一的症状出现在**另一次运行的另一个日志里**，
    而且读起来像是"AI 找不到障碍"而不是"障碍在天上"。

    所以脚本现在**每个障碍都自己量地面**，并且把量到的**地板名字**一起打进日志
    （`floor z=10 (on 'SM_road_001')`）。地面高度不再是一个假设。

    修好之后的生成日志，7/7 全部坐在实测地板上：

    ```
    scanned 285 city surface(s) for floor heights
    median top of all city geometry: z=100
      PX_Crate_Low_A   floor z=10 (on 'SM_road_001')       -> centre z=55  (height 90)
      PX_Crate_Low_B   floor z=10 (on 'SM_road_001')       -> centre z=65  (height 110)
      PX_Wall_High     floor z=10 (on 'SM_Set_B_Tiles_06') -> centre z=170 (height 320)
      PX_Wall_High2    floor z=10 (on 'SM_Set_B_Tiles_06') -> centre z=170 (height 320)
      PX_Kerb_Low      floor z=10 (on 'SM_Set_B_Tiles_06') -> centre z=25  (height 30)
      PX_Gate_A        floor z=10 (on 'SM_Set_B_Tiles_06') -> centre z=55  (height 90)
      PX_Gate_B        floor z=10 (on 'SM_Set_B_Tiles_06') -> centre z=55  (height 90)
    spawned 7 of 7 obstacle(s)
    ```

    另有两处障碍从原地**迁移**了：`PX_Kerb_Low` 从 `(-620, 420)` 挪到 `(620, 420)`，
    `PX_Gate_A` 从 `(-230, 1080)` 挪到 `(620, 1080)` —— 原位置上停着车，
    而"车顶"这个答案前面已经证明是错的。量不到地板时脚本会 `FAIL ... NOT spawning a
    floating box` 然后**不生成**，宁可少一个障碍也不做悬空件。

11. **`LowAvoidTraceHeight` 的盲区会制造一种「报告畅通但走不动」的卡死。**
    这是修好障碍锚点后、`city_run2.log` 里唯一剩下的那条真卡死（第 2249 行），
    值得单独记，因为它**和 `PX_` 测试障碍无关**，是城市自带地形：

    ```
    PursuitCharacter_1 jump probe at (786, 800, 90) feet z=2
      vel=2 want=575
      face 34 cm (at 90 cm, trace 300, trigger 190), top +38 cm above feet (jumpable 45 to 150)
    Warning: PursuitCharacter_1 has been stalled for 3.0 s with a clear feeler
      - face 34 cm reported clear, ground under feet z=-0 (step 90 cm ahead +40),
        forward face at 3/10/45/90 cm above feet = 34 / 53 / 53 / 53
    ```

    `vel=2`：玩家已经走到 **1143 cm** 之外，它一步没动过。

    **这是一条 150 cm 宽、38 cm 高的通条**，横切城市地面。坐标聚类把它圈得很清楚：
    `y ∈ [500, 620]` 上一个窄带，`x` 从 **669 一直到 1771** —— 不是某个角落，
    是横跨场景的一整道坎。27 次探针撞在它上面，其中 **6 次速度是 0**。

    三个判据**同时失效**，而且各自看都是合理的：

    | 判据 | 数值 | 为什么失效 |
    | --- | --- | --- |
    | 跳跃高度门 | 38 < 45 | 通条比 `MaxStepHeight` 还矮，本就不该跳 |
    | 台阶判据 | 只在 33 cm 内成立 | `LookAhead 90 × 0.37` 的窗口，车走到跟前时前方采样点**已经越过通条**落到另一侧地面 → 报 `step 90 cm ahead +40` 而不是"脚下抬升" |
    | 正面探针 | 4 点全高于通条 | `JumpFaceProbeHeight 90` + 降到脚下方，4 个采样点都落在 38 之下 → 报"无物体" |
    | 低位探针 | 6 cm 无正面 | 38 cm 的坎从 6 cm 高度看过去没有正面 → 报告**前方畅通** |

    于是：不转向（低位探针说畅通）、不后退（同上）、不跳（高度门正确地拒绝）。
    剩下唯一能救它的东西是胶囊自己的爬台阶 —— 但那要求**正面顶上去**，
    而它每次都被反射式转向带偏了。压过去的那 8 个样本（`vel=575`）就是概率上顶对了位置。

    **修复方向**（尚未实施）：低位探针不该只看"正面"，
    「从脚下方往上量、量到 0 到 `MaxStepHeight` 之间有任何实体」也应该算障碍，
    才能把它送进"可以顶过去或绕过去"的分支。这属于 `PursuitCharacter` 的改动，
    和本轮"把障碍放进真实位置"是两件独立的事，先记档。

    另注：`jump probe` 里 `top` 的哨兵 **`none (downward probe found nothing)` 出现了 386 次**，
    而 `top +8 cm` 只有 7 次 —— 说明绝大多数探针不是"量到低矮障碍"，而是**什么都没量到**。
    这两个数在旧日志里长得一样，只有加了哨兵文案之后才分得开。

12. **高度不是难度，是开关 —— 所以"高层平台"只能叠，不能堆。**

    **弧顶是算出来的，不是设的**：

    ```
    apex = JumpZVelocity² / (2 × DefaultGravityZ × GravityScale)
         = 640² / (2 × 980 × 1.6) = 131 cm（脚上方）
    ```

    `MaxJumpHeight` 必须**小于**它。原来设的 150 在弧顶之上，后果不是"跳得更高"，
    而是把 **121–150 这一段整体变成陷阱**：AI 会**决定跳**、**跳不过去**、
    然后**顶着障碍速度归零** —— 那是卡死，不是绕开。150 当初是从 125 提上去的，
    理由是"125 比城里常见的 150 墙还矮，应该肯跳"；前提没错，结论是反的：
    **这个旋钮只控制"肯不肯跳"，从不控制"跳不跳得过"**，所以提高它一面墙都没清掉，
    只是把正确的绕开改成了卡死。现在 = **120**（弧顶留 11 cm 落点余量，
    且两次跑批实测的高度分布上限都正好是 120）。

    → 真正有意思的区间只有 **75 cm 宽**（45–120）：44 cm 看不见、46 cm 要跳、121 cm 是墙。

    **推论**：想让平台"高"，**加高度是错的杠杆**。120 以上只是变成墙，只教会 AI 绕开。
    正确做法是**叠** —— `PX_Deck_*` / `PX_TowerW_*` / `PX_TowerS_*` 三座塔，
    每层 100 cm，**顶面 300 cm**，靠三跳 100 cm 爬上去。对照 `PX_Tower_Tall`（同样 300 cm、
    孤立不可达）就是那句话的全部证据：**同样的几何，只差一条上去的路**。

    **落点几何（决定了平台必须多深）**：起跳点在障碍面前 **190 cm**，之后一路直行，
    落到 100 cm 高度时已经在面后 **约 160 cm**。所以平台 **≥ 200 cm 深**才接得住 ——
    200 cm 以下的平台会被**飞过去**而不是踩上去。三座塔都用了 **300×300**。

    **三座塔共享一次地面实测**（`LAYOUT` 可选第 6 个字段 `floor_from`）。
    逐层各量各的，会让某个 15 cm 的凹陷把 100 cm 的一跳悄悄变成 115 cm —— 而且是
    **最坏的那种坏法**：塔照样站着、照样好看，只是**永远爬不上去**。
    第 6 字段让上面两层复用底层的读数，并且**仍然各自量一次自己的地面打进日志**
    （`shared with 'PX_Deck_1'; own XY agrees`）—— "故意共用"和"本来就一样"是两回事，
    两者之差就是一座**嵌在楼里**的塔：日志自洽、关卡里看不见、永远没人爬。

    **摆放本身就是 fixture 的一部分**：180 s 跑批实测追逐交通集中在 `y ∈ [-400, 600]`，
    而前两座塔在 `y = 1360–2120` —— 够得着，但只是"偶尔"（一次跑批西塔附近 11 跳、
    下一次只有 3 跳）。所以第三座 `PX_TowerS_*` 直接放进交通带（`x=1000, y=200..960`，
    紧邻那次跑批里吃掉 60 跳和 32 跳的两个障碍）。**塔摆在没人经过的地方，测的是风景。**

13. **绕障探针不知道障碍有多高 —— 低于 `MaxStepHeight` 的台阶会变成"报告畅通但走不动"。**

    现场（`city_run3.log`）：

    ```
    PursuitCharacter_4 jump probe at (815, 1066, 197) feet z=109 vel=0 want=575
      - face 37 cm (at 90 cm), top +42 cm above feet (jumpable 45 to 120)
    Warning: ... stalled for 3.0 s with a clear feeler
      - ground under feet z=107 (step 90 cm ahead +44)
      - forward face at 3/10/45/90 cm above feet = 42 / 42 / -1 / -1
    ```

    站在约 108 cm 的城市物件上，前面一个 **42 cm** 的台阶 —— **在 `MaxStepHeight`(45) 之下**，
    移动组件本来就该免费走上去。但它 `vel=0`，而且 `face 37~42` 这个数**贴着 `CapsuleRadius`(34)**。

    根因：`SteerAroundObstacles` 的两根梁只回答"前面有没有面"，**从不问"这个面有多高"**，
    所以 42 cm 的台阶和一面墙读法一模一样。于是它转向 → 侧移不够逃出梁 → 底部的
    `AvoidBackoffWeight` 又把它拉回来 → **在 34 cm 处形成极限环**，原地抖动、速度为 0。
    （上一次那条 38 cm 通条的卡死是同一回事，当时以为"四判据同时失效"，
    其实真正的最后一环在这里。）

    **第一次修法 —— 错了，而错法本身有教育意义。**
    调 `ProbeObstacleTop`，若 `top − feet ≤ MaxStepHeight` 就直接返回原方向。
    错在**量的地方**：`ProbeObstacleTop` 的台阶分支读的是**前方 90 cm 的地面**。
    当一道路缘沿着墙脚走时，它读到路缘的 40 cm 升高、判"可迈过"，
    而梁其实在 43 cm 处被一堵墙挡住 —— 同一行日志同时给出
    `forward face at 3/10/45/90 = 43 / 43 / 47 / 47`，**四个高度都有面 = 那是墙**。
    于是追逐者径直撞墙。同图 240 s：**卡死 3 → 15**，制造出一个新的卡死类。

    **教训（比修法本身重要）**：改完**必须用同一张图再跑一次对照**。
    否则"卡死换了个位置"会被读成"修好了"。

    **第二次修法**：改为**在报告的那个面之后 `ProbeIntoFaceDepth` 处向下打**
    （`Straight + ProbeIntoFaceDepth`，从 `feet + MaxStepHeight + 5` 打到 `feet − 60`）。
    低台阶**能被找到**（顶面在射线起点之下）；墙**找不到**（顶面在起点之上，
    射线起点落在墙体内，世界什么都不报）→ 退回旧绕障。
    **量不到 = 不改行为，是安全的失效方向。**

    **实测（同一张图、各 240 s `-PursuitAutoPlay`）—— 结果并不支持这个改动**：

    | 臂 | 跳跃 | 捕获 | **卡死告警** |
    | --- | --- | --- | --- |
    | v1 量 90 cm 前方地面 | 323 | 36 | **15** |
    | v2 在报告的面之后量 | 293 | 24 | **4** |
    | **关掉短路（原始行为）** | 315 | 34 | **2** |

    关掉反而最少，所以它**默认关闭**（`bStepOverLowFaces = false`，`-PursuitStepOver` 可开），
    代码与这段结论都保留了。

    ⚠️ **每臂只有 n=1，而这些跑批根本不可复现**：`APursuitCharacter` 走
    `CharacterMovementComponent`（dt 相关），而且**整个玩法模式没有任何种子**
    （`grep Seed/FMath::Rand` 零命中）—— 两次跑批的差异来自**帧时间抖动**，不是随机数。
    干净 A/B 的前提是**先让玩法模式可复现**（种子，或固定步长移动）。

    **这条缺陷仍然成立**：`vel=0 want=575` + `face 37~42`（正好贴着 `CapsuleRadius` 34）
    这个极限环是真实现象、成因清楚，但**"忽略可走的低面"不是它的解**。
    真要解决，应该从**后退权重与转向的相互作用**入手，而不是从高度判据入手。

    顺带：跳跃日志现在带**坐标**（`at (x, y, z)`）。总数说明不了任何事 ——
    "239 跳"只能说门在响，只有坐标能把一次跳跃**归属到某个 fixture**。
    而且注意**起跳点在障碍面前 190 cm**，所以 `grep "at (900, "` 会得到 0：
    塔被爬过，只是起跳点不在塔心坐标上。

14. **🔴 捕获判定完全不看高度 —— 站在平台上照样被下面的追逐者抓到。**

    现场的抱怨是"碰撞体积太大，它跳起来就碰到站在平台上的我"。真正的原因在代码里，
    比"体积大"更彻底：`TickChaser` **把 Z 丢掉之后才量距离**。

    ```cpp
    FVector ToPlayer = Player->GetActorLocation() - GetActorLocation();
    ToPlayer.Z = 0.0f;                        // ← 这一行
    const float Distance = ToPlayer.Size();
    ```

    所以捕获体是**一个无限高的圆柱**，和高度没有任何关系。两个可见后果：

    * 追逐者站在平台**下面**的地面上，只要水平距离进到 `CatchRadius` 里，就能捕获
      **站在平台顶上**的玩家 —— 而平台存在的意义正好相反（给追逐者一个必须爬的东西）；
    * 一次**只擦到平台边缘**、根本没落上去的跳跃，也算捕获。

    再叠一个容易误判的算术：两个胶囊半径都是 34、捕获半径是 85，而 **85 > 68**，
    所以"被捕"永远发生在"真正接触"之前约 17 cm。屏幕上看到的"它撞到我了"
    是**判定**，不是碰撞 —— 想让它保持这个手感，就不该去动碰撞体，该动判定。

    **修法**：捕获加**同层**条件，按**脚底到脚底**量。两个角色是同一个类、同一个胶囊，
    所以**原点差就是脚底差**，这里减半高反而会把门栏悄悄砍掉一半。

    | 参数 | 值 | 为什么是这个数 |
    | --- | --- | --- |
    | `CatchRadius`（水平） | 85 | 用户定的手感值，不动 |
    | `CatchHeightTolerance`（垂直） | **70** | 必须 **> 城市最高的地面杂物（~61 cm 的路缘）**，否则站上路缘就无敌；必须 **< 塔的第一层 110 cm**，否则平台一点不挡 |

    `-PursuitCatchHeight=<cm>` 可按次覆盖，**并且接受 0**（= 关掉这道门、恢复旧的
    无视高度行为），所以这是一条不用重编就能 A/B 的开关。已知代价：玩家跳跃弧顶
    131 cm，所以**地面上的追逐者抓不到跳到最高点的玩家** —— 跳一下能换约 1 秒，
    落地就被抓回来。这是闪避，不是 bug。

    **实测（同一坐标、各 45 s、只差一个开关）**，玩家用 `-PursuitPlayerAt=1035,2120,400`
    钉在 310 cm 塔顶靠边的位置（日志 `logs/verify_deck3_*.log`）：

    | 臂 | 玩家 pawnZ 中位 | 捕获 | 被门拦住 |
    | --- | --- | --- | --- |
    | 高度门开（默认 70） | **400**（= 站在塔顶） | **0** | **43** |
    | `-PursuitCatchHeight=0` | 400 | **21** | 0 |

    关掉那一臂的每一条都是同一个形状：

    ```
    caught by PursuitCharacter_1 (7 total) - 85 cm out, 310 cm off level (reach 85, gate 0)
    ```

    —— **水平 85 cm、垂直 310 cm**，也就是追逐者站在塔脚下，把塔顶的玩家抓了 21 次
    （45 s 内，平均 2 秒一次，回合一重置就再抓）。开门那一臂的 43 条都是
    `310 cm off the player's level - held by the height gate (70 cm)`，捕获 0 次。
    这就是"站在平台上还被打到"这个 bug 的完整复现与修好，一个开关之差。

    ⚠️ 注意 `-PursuitPlayerAt` 之外**没有别的办法**复现它：自演奏的玩家永远在地面。
    这个开关就是为了让"只在特定高度才发生的现象"可测而加的。

    **必须连带改的一处**：`TickChaserJump` 里"玩家已经够近，不必跳"的那句早退。
    它原来只看水平距离，而**站在平台下方、玩家在顶上的追逐者，正好是水平距离极小、
    垂直距离极大** —— 那句早退会取消掉"爬上去"唯一的那一跳，平台就从"挡你"
    变成"把追逐者困在下面"。现在它同样要求同层。

    **另外补了一行日志**，因为**沉默无法区分"门在起作用"和"门没跑"**：

    ```
    PursuitCharacter: PursuitCharacter_1 inside the reach (34 cm) but 110 cm off the
    player's level - held by the height gate (70 cm)
    ```

    捕获日志现在自报几何。理由是"我在平台上它还是抓到我"和"它爬上平台抓到我"
    **从玩家椅子上看是同一句话，却需要相反的修法**：

    ```
    PursuitPlay: caught by PursuitCharacter_2 (3 total) - 67 cm out, 4 cm off level
                 (reach 85, gate 70), resetting the round
    ```

    以后判断平台有没有生效，就看这条里的 `cm off level`：**很小 = 它真的爬上来了**（符合预期），
    很大 = 高度门没生效。

15. **🔴 录像"有没有拍到"这件事在本机只能量，不能看 —— 而且第一版度量方式是错的。**

    本机把 PNG 读回来得到 **`Content filtered`**，逐帧肉眼看 30 s 录像这条路是断的。
    于是写了 `tools/analyze_clip.py` 去量，第一版把「绿像素数」当成了"主体在不在画面里"，
    **校准之后发现这个指标是废的**：Cartoon City 满图绿树红车，实测 **绿最多 297000 px**
    （那一帧其实是一片公园）、**红从没低过 900**（那是车，不是玩家）。
    教训是通用的：**颜色计数在这张图上不是主体代理量**，只有"两种色同时为 0"才是无歧义的
    （说明画面里连植被带车带角色一个都没有）。

    真正成立的硬判据只有两条，都是**真发生过**的故障：
    `flat`（最大单色桶占比，**> 0.88 = 镜头在墙里**）与 `luma`（**< 12 = 关卡没加载的黑屏**）。
    合格片的参考值：**flat 峰值 0.32、luma 最低 104**。

16. **🔴 `FParse::Value` 的数值重载只有三个参数 —— 加第四个会得到一个像"没有这个重载"的 C2665。**

    ```cpp
    // 编译不过：error C2665，读起来像"根本没有 float 重载"
    FParse::Value(CmdLine, TEXT("PursuitGodPitch="), Value /*float*/, false);

    // 对：float/double/int32/uint32/int64/uint64 的重载都是 3 个参数
    FParse::Value(CmdLine, TEXT("PursuitGodPitch="), Value);
    ```

    `Engine/Source/Runtime/Core/Public/Misc/Parse.h`：**只有 `FString` 与 `TCHAR*` 两个重载**
    带尾参 `bShouldStopOnSeparator`（第 46、71 行），数值重载（第 56、58 行等）都没有。
    所以第 14 条里 `-PursuitPlayerAt=x,y,z` 需要 `false` 是因为**逗号是字符串问题**；
    读数字本身在数字结束处就停了，这里传 `false` 纯属多余，且是编译错误。

    ⚠️ 顺带一个容易误判的组合：这次 `Result: Failed (OtherCompilationError)` + `exit 6`
    正是第 5 节第 2 条"陈旧 `.rsp`"的**同一个签名**。**两者只能靠日志内容区分** ——
    真错误日志里有 `error Cxxxx: 文件(行,列)`，"陈旧 rsp"则是零编译器输出。
    这次是真错误，`grep error` 直接看到了 `PursuitCharacter.cpp(501,14): error C2665`。

---

## 6. 和训练环境的关系

两套东西**刻意分开**：

| | `L_PursuitAITrain`（训练） | `L_PursuitPlay`（本 Demo） |
| --- | --- | --- |
| 角色 | 不生成角色，红/绿球 | `ACharacter`，有物理、有胶囊 |
| 移动 | 纯位置算术 `AgentPos += Dir * Step`，**不读 DeltaSeconds** | 走 `UCharacterMovementComponent` |
| 随机数 | `-PursuitSeed=N` 可锁定 | 不涉及 |
| 相机 | 固定俯视 | 编辑器=第一人称｜`-game`=上帝视角（`play.bat` 覆盖成第三人称弹簧臂，见 1.1） |
| 游戏模式 | 引擎默认 | `APursuitPlayGameMode` |

游戏模式是**按关卡**设置的，所以训练地图完全看不到角色和追逐者，
`APursuitAIEnv` 也一行都没被改过。
「无渲染训练和有渲染演示动作一致」这件事的验证记录在 `TUTORIAL.md` 里。
