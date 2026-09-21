# E4 操作手册：第二台机器独立运行 + 真实读者走查

本手册是给**所有者**和**真实读者**的具体步骤，agent 不能替代这两件事。
执行完把记录回填到 `docs/evidence/BATCH-E.md`（或在 `out/e4/` 留记录后告诉我路径，我整理入档）。

## 0. 交付物身份（先记下来，后面每步都要对）

| 项 | 值 |
| --- | --- |
| ZIP | `out/e-batch/package/lumabough-0403d20-v7.zip`（v5 及更早的包**不要再用**：换机必失败） |
| EXE SHA-256 | `7e75b7767d0b4273b4b77c8d14e0e5ded682b9538581265f16951e17cffc6f28` |
| 二进制内嵌提交 | `0403d203419694cd185d3223ba77c75175456bcf` |
| 场景 manifest SHA-256 | `4ae6eda980221395a80e3bb03374d051a07b8cd197f8a5c0b55097f427ba6260` |

**第一次 E4 失败的原因（已修复，记录在此避免重踩）**：旧包把构建机的绝对 `.dxil/.dxbc` 路径写进了 EXE，
换机后 `shader artifact missing`。修复后 `runtime\shaders\d3d11` 里应有 `.dxbc`、`runtime\shaders\d3d12` 里应有 `.dxil`——
解压后请先确认这两点再跑。

## 1. 第二台机器的最低要求

- Windows 10/11 x64（UCRT 自带），支持 D3D12 的 GPU（较老机器退到 `--rhi=d3d11` 也可）
- 内存 8 GB 起，磁盘空闲 200 MB
- **不需要**：Visual Studio、Windows SDK、DXC、CMake、Python、任何开发 PATH
- 传输方式：U 盘 / 网盘 / 内网共享，任选；传输后必须核对上面的 ZIP SHA-256

## 2. 步骤（按顺序做，每步记录命令、退出码、输出）

### 2.1 校验与解压

```powershell
Get-FileHash .\lumabough-b7012b7-v5.zip -Algorithm SHA256   # 期望 668b88bf…（见上表）
Expand-Archive .\lumabough-b7012b7-v5.zip -DestinationPath .\lb-e4
cd .\lb-e4
```

解压目标必须是**全新目录**，不要放进任何源码树。解压后确认目录里有
`runtime\`、`scene\`、`licenses\`、`README.md`、`RUN.md`、`SUPPORT-MATRIX.md`、`PACKAGE-MANIFEST.json`、`SHA256SUMS.txt`。

### 2.2 证明不依赖开发环境

```powershell
where.exe python ; where.exe cmake ; where.exe dxc        # 期望：都找不到（或与本次运行无关）
$env:PATH -split ';' | Select-String -Pattern 'Visual Studio|CMake|Windows Kits'   # 期望：无匹配
```

记录实际输出。若机器上确实装了这些工具，记录它们的存在，并在下面注明"未使用"。

### 2.3 双后端启动（主验证）

```powershell
.\runtime\MiniEngineSandbox.exe --rhi=d3d12 --scene=m4-visual-baseline --manifest=scene\manifest.json --migration-level=9 --frames=1202 --width=1920 --height=1080 --output=out-run-d3d12
"exit=$LASTEXITCODE"
```

期望：stdout 出现一行 JSON，含 `"status":"PASS"` 与 `graphHash`；退出码 0。

```powershell
.\runtime\MiniEngineSandbox.exe --rhi=d3d11 --scene=m4-visual-baseline --manifest=scene\manifest.json --migration-level=9 --frames=1202 --width=1920 --height=1080 --output=out-run-d3d11
"exit=$LASTEXITCODE"
```

### 2.4 产物与身份核对

```powershell
Get-ChildItem out-run-d3d12 | Select-Object Name, Length
Get-Content out-run-d3d12\metadata.json -Raw | Select-String -Pattern 'sourceCommit|shaderSemanticSha256|warningErrors|resizeCount'
Get-Content out-run-d3d12\tour-events.jsonl | Select-Object -Last 3
```

要记进记录表的关键值：`sourceCommit` 应含 `b7012b7`；`warningErrors` 应为 0；
`tour-events.jsonl` 末行应是 `complete-clean-exit`。`out-run-d3d11` 同样记录一份。

### 2.5 换工作目录启动（确认不依赖 cwd）

```powershell
cd ..
& "$PWD\lb-e4\runtime\MiniEngineSandbox.exe" --rhi=d3d12 --scene=m4-visual-baseline --manifest="$PWD\lb-e4\scene\manifest.json" --migration-level=9 --frames=60 --width=1280 --height=720 --output="$PWD\lb-e4\out-run-cwd"
"exit=$LASTEXITCODE"
cd .\lb-e4
```

期望：仍然 `PASS`。（注意：本包不支持 `m7-*` 性能场景，见 `SUPPORT-MATRIX.md`。）

### 2.6 负例（可选，但很有价值）

```powershell
Rename-Item .\runtime\shaders\d3d11 d3d11_off
.\runtime\MiniEngineSandbox.exe --rhi=d3d11 --scene=m4-visual-baseline --manifest=scene\manifest.json --migration-level=9 --frames=3 --width=1280 --height=720 --output=out-neg
"exit=$LASTEXITCODE"
Rename-Item .\runtime\shaders\d3d11_off d3d11
```

记录：是否**明确失败**（有错误文本、非零退出码），而不是静默给出错误画面。

## 3. 记录表（复制填写，回传给我）

| 项 | 实际值 |
| --- | --- |
| 机器/系统版本（`winver`） | |
| GPU 与驱动版本 | |
| ZIP SHA-256 校验 | |
| 2.2 开发环境检查输出 | |
| 2.3 D3D12 退出码 / status / graphHash | |
| 2.3 D3D11 退出码 / status / graphHash | |
| 2.4 sourceCommit / warningErrors / 末行事件 | |
| 2.5 换目录退出码 | |
| 2.6 负例错误文本（可选） | |
| 异常与截图 | |

补充两条本轮踩到的坑：

- 解压后确认 `runtime\shaders\d3d11` 有 `.dxbc`、`runtime\shaders\d3d12` 有 `.dxil`；缺了就说明拿到的是旧包。
- 程序化捕获（`--pix-capture` / `--renderdoc-capture`）要求**目标目录先存在**，否则应用启动即退出且没有捕获文件。

失败时请一并提供：完整 stdout/stderr、`out-run-*\metadata.json`、出错命令原文。

## 4. 真实读者走查（另一件事，不要和第二台机器合并）

读者要求：**未参与实现**（同事/朋友即可）；只允许看包内文档；不得看本仓库、不得被提示。

给读者的任务书（原样转达）：

1. 从 ZIP 解压开始，只按包内 `README.md` / `RUN.md` / `SUPPORT-MATRIX.md` 操作；
2. 让示例场景跑起来，并**找到两样证据**：一次运行的 `graphHash`、一次事件记录（如 `complete-clean-exit`）；
3. 用一句话说出这个包**能**做什么、**不能**做什么（`SUPPORT-MATRIX.md` 里写了）；
4. 计时：从解压到跑出 `graphHash` 用了多久。

读者反馈表（请读者自己填，原文回传，不要修饰）：

| 项 | 读者原文 |
| --- | --- |
| 卡住/需要猜的地方（第几步、当时想什么） | |
| 文档里看不懂的词/句 | |
| 实际是否跑出 `graphHash`（是/否） | |
| 耗时 | |
| 读者认为的能力范围（一句话） | |
| 其他抱怨或建议 | |

**不要替读者解释或修正**。卡住本身就是最有价值的反馈。

## 5. 回传之后我会做什么

1. 把第二台机器记录与读者反馈整理进 `docs/evidence/BATCH-E.md`（失败项按失败记录，不写成通过）；
2. 按反馈修复；**包字节一旦变化，旧 ZIP SHA、旧独立运行结果与旧读者反馈都不能复用**，会重新打包并请你重跑受影响的部分；
3. 全部通过后进入 E5：候选裁定 + F 发布提案（仍不推送、不公开）。
