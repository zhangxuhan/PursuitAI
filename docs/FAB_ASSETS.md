# Fab / 商城资源导入本工程

Launcher「库 → 资源 → 添加到工程」的下拉框找不到本项目，不是本项目的配置错了，
是启动器**读不到**它。下面把机制、修法、以及两条更省事的替代路都写清楚。

---

## 1. 启动器的工程列表从哪来

只有一个来源，引擎源码
`Engine/Source/Developer/DesktopPlatform/Private/DesktopPlatformBase.cpp:1801-1877`
（函数 `EnumerateProjectsFromEngine`）：

```
<引擎目录>/Saved/Config/WindowsEditor/EditorSettings.ini
  [/Script/UnrealEd.EditorSettings]
    CreatedProjectPaths=<目录>        # 扫描该目录「下一级子目录」里的 *.uproject
    RecentlyOpenedProjectFiles=...    # 当作「字面路径」直接收进列表
```

本项目两处都不满足：

**1）5.7 那份 ini 里根本没有 `CreatedProjectPaths`。**
该键只在用编辑器「新建工程」对话框建工程时才写。本项目是手工建立的，所以按键不存在。
实测 `grep -c CreatedProjectPaths` = **0**，`--check` 会打印
`CreatedProjectPaths: NONE - nothing is scanned for this engine`。

**2）唯一的 recents 条目是结构体字面量，不是路径。**
UE 5.7 把该设置存成 `TArray<FRecentProjectFile>`，写成
`(ProjectName="...",LastOpenTime=...)`；但 `DesktopPlatform` 仍按 `TArray<FString>` 读
（源码 1817-1826 行：`MultiFind` 进 `TArray<FString>`，然后原样塞进工程列表）。
于是整串被当成一个文件路径，永远不会匹配到真实工程。启动器日志原话：

```
Found project "(ProjectName="<project>/PursuitAI.uproject",LastOpenTime=2026.09.19-13.48.23)" in recently opened files
```

整个列表里只有这一条是这种形状，其余（Test / ProjectT / ChatDemo / …）都是裸路径。
**这是编辑器（结构体）与启动器（字符串）的版本不一致**，不是本项目做了什么。

两者叠加 → 启动器眼里没有任何有效工程：

```
$ python tools\fix_fab_project_list.py --check
  CreatedProjectPaths: NONE - nothing is scanned for this engine
  RecentlyOpenedProjectFiles (1, 1 unusable):
      line 13: (ProjectName="<project>/PursuitAI.uproject",LastOpenTime=...)  <-- not a path
  launcher will therefore see:
      (nothing)
```

---

## 2. 修它：`tools\fix_fab_project_list.py`

```
python tools\fix_fab_project_list.py --check     # 只看现状，不改任何东西
python tools\fix_fab_project_list.py --dry-run   # 看会插入哪一行
python tools\fix_fab_project_list.py             # 真写，自动备份
```

约束与行为：

- **写之前必须关掉编辑器。** `UnrealEditor` 启动时读一次 `EditorSettings.ini`、退出时回写，
  边跑边改可能被覆盖掉。脚本会枚举进程并**拒绝写入**（提示是哪个 PID），`--force` 可越过。
  启动器不需要关，它只读不写这个文件 —— 但**改完必须重启启动器**，它在启动时缓存列表。
- 改的是
  `%LOCALAPPDATA%\UnrealEngine\5.7\Saved\Config\WindowsEditor\EditorSettings.ini`。
- 自动备份到同目录 `EditorSettings.ini.bak-<时间戳>`。**回滚 = 把备份覆盖回去**，没有别的状态。
- 幂等：已经有这条就打印 `already present - nothing to do`，不重复插入。
- 写完它会把启动器的扫描算法**重放一遍**（照引擎源码实现，同样只扫一级），
  打印启动器将会看到的工程列表，并给出 PASS/FAIL —— 判的是
  「这个 `.uproject` 有没有被扫出来」，不是「那一行有没有写进去」。

### 关键陷阱：路径必须填**父目录**

扫描是

```
for 每个 <CreatedProjectPaths>:
    for 每个 <它的一级子目录>:
        收集 <子目录>/*.uproject
```

**只下一级。** 所以要填 `E:\Project`，**不能**填 `E:\Project\UE5+ai`
—— 后者会去找 `<project>\\<子目录>\*.uproject`，什么都没扫到，而且**不报错**。

证据（UE 4.27 自己的配置，启动器确实消费了它）：

```
CreatedProjectPaths=E:\Project\Test        ← 配置里写的
Found project "<unrelated project>.uproject" in previously created project directory
                                            ↑ 日志里扫出来的
```

另外注意：那份配置里 `CreatedProjectPaths` 用**反斜杠**、`RecentlyOpenedProjectFiles` 用正斜杠，
两种都能读（启动器日志里显示的是它规范化后的正斜杠）。脚本按前者写，与已验证可用的格式一致。

副作用：`E:\Project` 下一级的其它工程也会一起出现在下拉框里（本机是 `E:\Project\LetsGo`），
无害，且这正是「扫描父目录」这个机制的本意。

---

## 3. 另外两条路（常常更省事）

### A. 用编辑器里的 Fab 面板 —— 推荐

`Engine/Plugins/Fab/Fab.uplugin` 的 `EnabledByDefault=true`，所以打开本工程就有 Fab 面板
（Window → Fab）。它把资源装进**当前打开的工程**，完全绕开上面那个下拉框，
不存在「找不到工程」的问题。

### B. 手动拷 VaultCache

下载后的资源落在 VaultCache，老式条目是解包好的 UE 内容，直接拷进 `Content\` 即可：

```
<vault>\<包名>\data\Content\...   →   <project>\\Content\...
```

两个 VaultCache 位置都要看：

- 默认：`C:\ProgramData\Epic\EpicGamesLauncher\VaultCache\`
  （本机的 `AnimeCha…`、`GreenwoodVillage…`、`HackAndS…` 等旧包都在这儿）
- **本机被重定向过**：启动器配置里有
  `VaultCacheDirectories=<VaultCache>/`，
  所以**新下载的包会落到 E 盘**。该目录目前只有 `FabLibrary/listings_v1.db`。

---

## 4. RPG Hero Squad 的状态

**尚未下载。** 本机 VaultCache 里没有它（`C:\ProgramData\Epic` 与 E 盘两处都查过）。
Fab 是按账号分发的，下载/导入必须用你自己的 Epic 账号操作，脚本代替不了。

按第 3 节 A 或第 2 节任一方式导入后，如果需要把角色换成这个包里的骨骼网格，
改 `Source/PursuitAI/PursuitCharacter.cpp` 里那 6 个 `TSoftObjectPtr` 资源路径即可
（见 `docs/PLAY_MAP.md` 第 3 节），换完看日志是不是 `6/6 clips`。
