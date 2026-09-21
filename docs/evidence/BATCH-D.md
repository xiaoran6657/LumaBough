# D 批次：最终 anchor Capture 与动态视频

日期：2026-09-20。范围：本地 LumaBough；未提交、未推送、未公开。
阶段进度：**D1 完成、D2 完成（本地原件）、D3 BLOCKED（缺录屏工具，需所有者决定）、D4 部分完成（本轮文档）。**

> 状态更新（2026-09-21，E 批次）：以上是 D 当天快照。**D 现已收口**：录屏工具由所有者授权使用本机 ffmpeg，
> 双后端成片已产出并按最终 EXE 重录；Capture 亦按最终 EXE 重采。现行产物与哈希以
> [E 记录](BATCH-E.md) 与 [候选裁定](CANDIDATE-REVIEW.md) 为准；本节以下内容保留为当时记录，不倒改。

## 0. 前提与授权状态

- P 阶段仍是 BLOCKED：`lb-current-001/002` 两组均 INVALID，003 未启动（见 [新性能记录](NEW-PERFORMANCE.md)）。
  本轮按所有者指派进入 D，**不宣称任何当前加速结论**，视频与 Capture 只表达能力与契约。
- 本轮只做 D 范围内的改动：连续 Demo 的可读性控制、anchor 帧 GPU 捕获、证据与文档。
  未启动 E/F，未提交，未发布。
- 本轮没有改动 M7 性能路径（`M7SceneRunner`），因此不推翻也不修改 P 的任何失败记录；
  但 Release EXE 已重新构建，若将来启动 003，必须使用本轮之后的构建并重做冻结。
- Capture 运行为了工具链兼容关闭了 debug layer（原因见 §5），零诊断证据由 §4 的六次彩排
  （带 debug layer）承担；这一点不是放宽门槛，而是两条路径分开记录。

## 1. 实际能力：画面上能看到什么（D1 分析）

连续 Demo 是固定相机、固定光照、无动画的 PBR 场景，因此：

| 事件 | 画面表现 | 说明 |
|---|---|---|
| 冷启动后固定渲染 | 静止画面 | 场景本身没有动画 |
| RHI 挂起（交换链 0×0） | **无可见变化**：不再提交帧，窗口保留最后一帧 | 不是 OS 最小化；静止场景下无法用画面区分 |
| 改为临时尺寸 + 同 bytecode 重建 | **可见**（尺寸变化后由 DXGI 缩放呈现） | C 批次的 width+16 只差 0.8%，肉眼不可辨；本轮改用 960×540 |
| 非法 shader 候选被拒绝 | 无可见变化 | 拒绝是正确行为，画面继续呈现 |
| 恢复 1920×1080 | **可见**（恢复原始清晰度） | 末帧与锚点帧像素逐字节一致 |

结论：按 C 批次的历史节奏（挂起后立刻重建、临时尺寸只差 16 像素），录制出来是
"20 秒静止画面 + 一次不可见的闪烁"，不具备可读性。因此按 D1"最小可见事件状态或合理停留"
增加了**停留帧数**与**可辨认的临时尺寸**两项控制，事件本身与判定条件不变。

## 2. 本轮实现改动（D1）

| 文件 | 改动 | 影响面 |
|---|---|---|
| `samples/rhi_sandbox/RhiSelection.h/.cpp` | 新增 `--tour-hold-frames=N`、`--tour-temporary-width/height=N`；`--vsync` 对 M6 场景生效；用法文本同步 | 仅 `--exercise-changes` 路径；缺省值与 C 批次一致 |
| `samples/rhi_sandbox/M6SceneRunner.cpp` | tour 事件改写为停留状态机（顺序不变）；临时尺寸可配置；`swap.vsync` 跟随 `--vsync`；tour 运行的 GPU capture 绑在 anchor 帧（原为末帧）；metadata 的 `vsync` 字段改为实际值 | 只改 Demo 运行器，不改 engine/RHI 算法 |
| `assets/recipes/m9-portfolio-video.json`（新增） | 视频/Capture 协议：`frames=1562`、`warmupFrames=780`、`vsync=true`、`tourHoldFrames=90`、`tourTemporaryExtent=960×540` | 与历史 1202/600 协议并存 |
| `tools/portfolio/run_demo.py` | 按 recipe 传递上述选项；`warmupFrames` 校验由"必须等于 600"改为"≥600"；事件序列比较折叠连续重复的 `temporary-frame-presented` | 历史 recipe 仍合法 |

不改的部分：没有新增文件监听热更新、自由相机、动画、编辑器或新 shader 效果；
没有改动渲染算法；没有放宽任何判定门槛（事件序列、resizeCount、末帧像素一致、
资源退休 gate 全部保留）。

**一处与 C 批次的历史时序差异（已确认，不回改）**：`tourHoldFrames=0` 时，临时尺寸
在旧代码里被呈现 2 帧（`total/2+1`、`total/2+2`，随后恢复），改为状态机后呈现 1 帧
（恢复提前一帧）。事件集合、`resizeCount=3`、末帧像素一致与退出 gate 均未变；
`run_demo.py` 的事件比较改为折叠连续重复项，两种时序都能通过。C 批次的
[彩排证据](DEMO-REHEARSAL.json)保留原来的两帧记录，不倒改。
回归验证：不带新选项直接跑 1202 帧历史参数，两后端均 PASS，
`temporary-frame-presented` observed=1936（历史临时尺寸）。

## 3. 环境与工具身份

| 项 | 实际值 |
|---|---|
| GPU / 驱动 | AMD Radeon RX 9070 / 32.0.31035.1003（非 WARP） |
| 编译器 / 配置 | MSVC v145（VS 2026）、Release、Tracy OFF |
| RenderDoc | v1.45（`renderdoccmd.exe` SHA-256 `273352017e23e890fe9134de0157d1fe556676a4c6004bfe3265db1a4648ed07`） |
| PIX | 1.0.2603.25001-release（`pixtool.exe` SHA-256 `d34c906f33997f389f3af9b3ae92ed9c1eeaf302bf8c79fca61c86a1b8e37525`） |
| 运行 EXE | `out/build/windows-msvc-debug/samples/rhi_sandbox/Release/MiniEngineSandbox.exe`，1,872,384 B，SHA-256 `f0a9894fe6760735774204a2053d735facb37ec1880bbad8a62e5da0b8b0f4a8`（2026-09-20 23:51 构建） |
| 录屏工具 | **无**：ffmpeg / OBS / Game Bar 均未在本机可用（已实测 `ffmpeg`、`obs` 均不存在，winget 不可用） |
| 源码提交身份 | 仓库仍无 HEAD，`sourceCommit/runtimeCommit` 均为 null；绑定方式为逐文件哈希 + 本轮构建产物哈希 |

## 4. 双后端连续彩排（更新后协议）

命令（从仓库根执行）：

~~~powershell
python -B tools/portfolio/run_demo.py --exe out/build/windows-msvc-debug/samples/rhi_sandbox/Release/MiniEngineSandbox.exe --manifest out/demo/scene/manifest.json --recipe assets/recipes/m9-portfolio-video.json --output out/d-batch/rehearsal-video
~~~

结果：`PASS rehearsal-1/2/3 × d3d11/d3d12`，共六次可见窗口连续进程，每次 1562 帧、
1920×1080、Release、debug layer 开启、VSync on、停留 90 帧/状态、临时尺寸 960×540。
每次均通过：事件序列与逐项 observed/expected、`anchor.json` 静态身份、末帧与锚点帧像素一致、
`resizeCount=3 / reloadSuccess=1 / reloadRejected=1 / warningErrors=0`、资源退休 gate、退出码 0。

## 5. 双后端 anchor Capture（D2）

anchor 帧 = 第 781 帧（动作之前），逻辑标记 `portfolio-capture`；两个后端各一份可重新打开的单帧捕获。

| 后端 | 工具 | 触发方式 | 捕获文件 | 大小 | SHA-256 |
|---|---|---|---|---|---|
| D3D11 | RenderDoc v1.45 | `renderdoccmd capture` 注入 + 应用 `RenderDocScope` 在 anchor 帧程序化捕获 | `out/d-batch/anchor-capture/d3d11/d3d11-anchor.rdc` | 33,446,987 | `75644555a4b296c5eb256cb4e73dc62049f5bcabdf5516f7d1ccb0692f100053` |
| D3D12 | PIX 2603.25 | `pixtool launch … programmatic-capture --until-exit` + 应用 `PixScope` | `out/d-batch/anchor-capture/d3d12-run3/d3d12-anchor.wpix` | 37,506,950 | `2120fbf2220f6ec81a98321e2e618f9caec3497420a2539756d8f700a644d3a9` |

运行状态：两次捕获运行均完整跑完 1562 帧，`tour-events.jsonl` 以 `complete-clean-exit` 结束，
`anchor.ppm` 与 `color.ppm` 逐字节一致（两后端各自的哈希相同），`warningErrors=0`。

### 无 GUI 的结构核对（"重新打开"的可复核部分）

- D3D11：`renderdoccmd convert -i rdc -c xml` 导出 `d3d11-anchor.xml`
  （1,119,918 B，SHA-256 `89986becad0c368e44ad49a35429eaf27c8a35e6d8f61ca2f70b4ebee5b95b12`）。
  其中可见 `ID3DUserDefinedAnnotation::BeginEvent` 事件：`Timestamp.Begin` → `Shadow` →
  `M6.Shadow.<序>.<稳定 ID>` → `Skybox` / `M6.Skybox` → `ToneMap` / `M6.ToneMap` →
  `Screenshot` → `Timestamp.End`；thumbnail 为 1920×1080。
- D3D12：`pixtool open-capture … save-event-list` 导出 `d3d12-events.csv`
  （26,876 B，SHA-256 `d3d34b10a8818bed61e2200898f9887919c8cbb6a0d57b0f806dfb0ef037e2bd`）。
  事件树为 `M5.Frame N` → `Timestamp.Begin` → `Shadow`（30 次 draw）→ `Skybox` →
  `ToneMap` → `Screenshot` → `Timestamp.End`。

结论：**现有 pass/draw GPU marker 在两个后端都可见**，因此没有为了定位而新增 GPU marker；
anchor 帧的身份由三重事实绑定：① 捕获由应用在 anchor 帧程序化触发（tour 模式下只有该帧触发）；
② 帧内存在只在截屏帧出现的 `Screenshot` 事件；③ 该帧的 `anchor.json` 与 `anchor.ppm`
（1920×1080、`commandHash` 两后端一致 `274cad42fe457a84aa1da01789b6e2a789301bf55f04119f326b99fae2a8a8a0`）。

### 工具链坑（本轮实测，已记录以免重踩）

1. RenderDoc + `--debug` 冲突：D3D11 设备在 RenderDoc 注入下不带 debug layer，
   与应用的 `enableDebugLayer` 校验冲突，报
   `reported D3D11 capabilities do not match the selected device`。捕获运行因此不加 `--debug`。
2. `renderdoccmd capture` 默认把子进程工作目录设为 exe 所在目录，必须用 `-d <仓库根>`
   并给绝对路径，否则相对路径的 manifest/output 解析失败。
3. PIX `programmatic-capture` 不加 `--until-exit` 时，保存捕获后目标进程被终止
   （实测 anchor.ppm 写到一半、事件文件为空）。加 `--until-exit` 后应用完整退出。
4. `pixtool launch --command-line` 不接受裸空格，参数用应用自带的 `@响应文件` 传递。

### 尚未完成（D2 遗留）

- 在 RenderDoc / PIX **GUI** 中人工打开上述文件、定位 `Screenshot` 事件并查看资源/
  绑定/渲染通道：本轮无 GUI 自动化能力，未能执行；已给出可复核的命令行替代，但按 D2 规则
  不能标记为 PASS。需要所有者按 §7 的手工步骤确认。

## 6. 视频（D3）：BLOCKED

未录制任何视频。阻塞项与可选方案：

- 缺录屏工具（ffmpeg / OBS 均不存在，winget 不可用，Game Bar 需人工操作）。
  可选：A) 所有者用系统 Game Bar 人工录制；B) 所有者授权下载 ffmpeg 静态构建后由脚本录制
  （`ffmpeg -f gdigrab` 抓窗口）并做字幕烧录/转码；C) 授权安装 OBS。
- 未确定剪辑方案：原始素材约 26–35 秒/后端（静止开头 13 秒需剪切并明确标注）。
- 字幕/字体需可再分发资源；本轮尚未生成字幕稿（将在录制方案确定后按事件时间线给稿）。

## 7. 需要所有者决定的事项（按优先级）

1. **确认以"不宣称当前加速、保留 P 阻断"的缩减范围进入 D**（P 仍 BLOCKED，见 §0）。
2. **录屏方式**：A/B/C 选一（见 §6）。
3. **安排一次前台时段**：录制时需窗口可见且不被其他窗口遮挡，需与所有者确认时间。
4. **GUI 复核**：在 RenderDoc / PIX 中打开 §5 的两个文件，确认可定位 anchor 事件。
5. 若希望视频里临时尺寸更明显，可改 `m9-portfolio-video.json` 的 `tourTemporaryExtent`（当前 960×540）。

## 8. 本轮产物位置（均在 out，不进公开集合）

- `out/d-batch/rehearsal-video`：六次彩排（含 `summary.json`、逐次 `anchor.json/ppm`、`metadata.json`）。
- `out/d-batch/anchor-capture/d3d11`：D3D11 捕获 + 运行产物 + XML 导出。
- `out/d-batch/anchor-capture/d3d12-run3`：D3D12 捕获 + 运行产物 + 事件列表 CSV。
- `out/d-batch/anchor-capture/d3d12`、`d3d12-run2`、d3d12 早期尝试：PIX 终止进程的失败证据，保留不覆盖。
- `out/d-batch/capture-probe`、`out/d-batch/logic-check`：参数与工具链的短跑验证。

## 9. 视频（已完成：方案 B）
成片：out/d-batch/video/d3d12-v2/demo.mp4
- 1920x1080 / 60fps / 29.8s / H.264 / 1,448,362 B
- SHA-256 df7405e024d67594eb68c44db68dd6537fa0af551783dead6049709915549537
- 原片 raw.mkv SHA-256 098334d6db90c14d583e67bbca85dc67f20cd6023ee004276c75e603e5e147c3（9,314,550 B，仅本地）
- 被测 EXE SHA-256 f0a9894fe6760735774204a2053d735facb37ec1880bbad8a62e5da0b8b0f4a8
- 事件时刻：intro 0-14.49 / 挂起 14.49-16.19 / 临时 16.19-17.39 / 拒绝 17.39-17.69 / 恢复 17.69-29.8
- 锚定：清晰度扫描，实测下降 6%
- gdigrab 抓不到 flip-model（纯白），改 ddagrab+hwdownload
- DPI 感知必须在窗口坐标 API 之前（否则截到壁纸区域）
- 限制：目前只有 d3d12 成片，原始大文件留在本地
- D3D11 成片：1920x1080/60fps/26.7s
- D3D11 成片 SHA-256 d9e4a0023398e6b4e9b955a91d3ba6c0cb4ad3fd6544d7f4fbf734be15a27ae7
- D3D11 raw sha256 736d4e8f3b0ee1ed96f885608e28fe6ac1b7706071a68444d4ff6a0438e4edbb

## 10. Capture 复核（CLI 部分完成）
- D3D11：renderdoc-mcp 0.3.1 + RenderDoc 1.45 CLI 打开成功（206 事件 / 62 draws / 未降级）
- D3D11：事件 798 = Screenshot，夹在 ToneMap(779/784) 与 Timestamp.End(801) 之间；assert-clean 高等级零消息
- D3D11：在事件 794（最后一个 draw）导出后备缓冲 1920x1080，与本次运行 anchor.ppm 平均逐像素差 0.0846（最大 150）
- D3D12：pixtool 打开成功并导出事件列表（Screenshot 事件在帧内，见 d3d12/events.csv）
- D3D12：save-screenshot 在 PIX 2603.25 上稳定失败（0x80004003），属工具限制；已用 anchor.ppm + 事件列表替代
- 证据与结论：out/d-batch/anchor-review/（REVIEW.json、d3d11/rt_794_0.png、d3d12/events.csv），仅本地
- 仍需人工 GUI：在 RenderDoc/PIX 界面里目视定位 Screenshot 与资源历史（本机无 GUI 自动化）

### 10.1 人工 GUI 核对清单（所有者执行，约 2 分钟）
D3D11 / RenderDoc v1.45：
1. 打开 C:/Program Files/RenderDoc/qrenderdoc.exe
2. File → Open Capture → out/d-batch/anchor-capture/d3d11/d3d11-anchor.rdc
3. Event Browser 里找名称 Screenshot 的事件（ID 798，位于 ToneMap 与 Timestamp.End 之间）
4. 选中事件 794（全屏三角形 draw）→ Texture Viewer 应显示 1920x1080 的 PBR 场景
5. 判据：事件名可见 + 该帧是动作前的静态画面（清晰、1920x1080）
D3D12 / PIX 2603.25：
1. 打开 C:/Program Files/Microsoft PIX/2603.25/WinPix.exe
2. File → Open → out/d-batch/anchor-capture/d3d12-run3/d3d12-anchor.wpix（等 Start Analysis 结束）
3. Event List 展开 M5.Frame 根事件，应能看到嵌套的 Shadow / Skybox / ToneMap / Screenshot / Timestamp.End
4. 选中 ToneMap 段里的全屏 draw，看 Pipeline/OM 的 RTV 为 1920x1080 且预览是 PBR 场景
5. 注意：pixtool save-screenshot 在该版本报 0x80004003，属工具限制，不影响 GUI 查看
回填：把结果写进 out/d-batch/anchor-review/gui-notes.md（事件名/ID、RT 尺寸、可选截图）

### 10.2 人工 GUI 复核结果（2026-09-21）
- D3D11 通过：Event Browser 可见 ToneMap/M6.ToneMap → Screenshot(798) → Timestamp.End(801) → Present(808)
- D3D11 通过：Texture Viewer 的 RT0 = Swapchain Image 47、1920x1080 PBR 场景；窗口底部 No problems detected
- D3D12 部分确认：Event List 可见 Shadow(67) / Skybox(68) / ToneMap(70,71) / Screenshot(73→CopySubresourceRegion) / Present(79)
- D3D12 待确认：RTV M6.ScreenBackbuffer 的 Width/Height 是否为 1920x1080（截图列被截断显示 1020）
- 截图证据：out/d-batch/anchor-review/gui/（d3d11-renderdoc.png、d3d12-pix-*.png），仅本地
- D3D12 通过：Draw 71（M6.ToneMap）的 RTV0 M6.BackBuffer.Buffer[0] 为 1920x1080 / B8G8R8A8_UNORM，预览为同一 PBR 画面
- 结论：双后端 anchor Capture 的 CLI 与 GUI 复核均通过，D 阶段收口
- 提交后验证性重跑（2026-09-21，新 EXE b92cda44…）：彩排 6/6 通过、双后端 Capture 重采并复核通过；细节见 [E 记录](BATCH-E.md)
- 提交后重录（新 EXE 16ebb979…，2026-09-21）：D3D12 demo.mp4 SHA-256 133e92d0a9fef1b771c52fa459b79668d4165eb93943a2c09fac9e4f254c83fa，1,399,205 B
- 提交后重录 D3D11 demo.mp4 SHA-256 f5ef8a16a7ce40465f5ff9ef5783fc9f07872866f33a64de40bd93e19983044b，1,495,826 B
- 此前两份绑定旧 EXE f0a9894f… 的成片（df7405e0… / d9e4a002…）由此作废，仅留在本地
- 现行产物以 E 记录为准：最终 EXE b7012b7 / 9dd31e66…、双后端 Capture 与成片见 [E 记录](BATCH-E.md)（本节上面的哈希是提交前版本，保留作历史）
