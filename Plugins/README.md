# Plugins

## Schola

| 项 | 值 |
|---|---|
| 来源 | `https://github.com/GPUOpen-LibrariesAndSDKs/Schola.git` |
| 固定 tag | **v2.1.1** |
| 固定 commit | `fa4743b6d4e16ad63600796a91f8f4a642ef9d5d` |
| 本地路径 | `Plugins/Schola/`（插件根 = 仓库根） |
| 克隆方式 | `git clone --branch v2.1.1 --depth 1`（浅克隆） |

`Plugins/Schola/` 已在根 `.gitignore` 中被排除，不进入本项目仓库。

### 升级到新 tag 的方法

```bash
git -C Plugins/Schola fetch --tags origin
git -C Plugins/Schola checkout v2.2.0        # 换成目标 tag
# 然后清掉编译缓存并重编
rm -rf Binaries Intermediate Saved
```

升级后请同步更新本文件里的 tag / commit，并重新跑一遍 `PursuitAI.ScholaStatus` 自检。

### 注意事项

- `Schola.uplugin` 里 **没有 `EngineVersion` 字段**，所以不会因引擎小版本号被拒；
  但插件是**源码分发**，换 tag 后必须重编项目。
- 官方矩阵：Schola 2.1.x ↔ UE 5.5–5.7 / Python 3.10–3.12。
- 已知小瑕疵：v2.1.1 tag 的 `Schola.uplugin` 里 `VersionName` 仍写 `"2.1.0"`（仓库没同步改），
  以 **tag / commit** 为准，不要以 `VersionName` 判断实际版本。
- C++ 依赖（gRPC / protobuf / absl）已内置在 `Source/ThirdParty`，**不需要**跑 `windows_dependencies.bat`。
- 插件依赖三个引擎插件：`EnhancedInput` / `StateTree` / `GameplayStateTree`（已在 `.uproject` 里启用）。
- Python 侧包路径：`Plugins/Schola/Resources/python`（含 `pyproject.toml`）。
