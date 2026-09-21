# 连续 Demo 与静态锚点

本地验证：三轮 × D3D11 / D3D12，共六个连续进程通过。每进程 1202 帧，可见窗口、1920×1080、Release、debug layer 开启。每个后端分别启动一个进程，不把后端切换伪装成同一进程内切换。

先按[开发指南](../DEVELOPMENT.md)烘焙场景，然后从仓库根运行：

~~~powershell
python tools/portfolio/run_demo.py --exe out/build/windows-msvc-debug/samples/rhi_sandbox/Release/MiniEngineSandbox.exe --manifest out/demo/scene/manifest.json --output out/demo-rehearsal
~~~

输出目录必须不存在。脚本逐次启动、等待退出，任何失败立即停止，不自动重试。自动化无窗口运行可加 --headless，结果会明确标记模式；本次正式证据为 visible。不要在彩排期间手动拖拽窗口尺寸或关闭窗口。单进程超时为 300 秒。

| 顺序 | 动作前状态、动作 | 实际检查与失败条件 |
|---|---|---|
| 1 | 冷启动，固定相机渲染 600 帧 | 场景/资产加载或图执行失败则退出 |
| 2 | 第 601 帧、动作之前，逻辑标记 portfolio-capture | anchor.json + anchor.ppm；校验资产、shader、可见序列、图、尺寸、零 validation warningErrors |
| 3 | 已呈现静态场景，RHI swapchain 调为 0×0 | BeginFrame.serial 必须为 0 |
| 4 | 恢复为 1936×1088，使用相同 bytecode 重建 shader pipelines | 重载成功，pipeline 数量保持 7 |
| 5 | 提交空 bytecode 候选 | 必须拒绝，pipeline 数量保持；随后正常执行并呈现两帧 |
| 6 | 恢复 1920×1080，继续到末帧 | 最终尺寸、resizeCount=3、reloadSuccess=1、reloadRejected=1 |
| 7 | 末帧读回并退出 | 末帧与动作前像素逐字节一致；诊断和资源退休 gate 通过，完整事件序列与退出码为零 |

输出中的 frame 只是诊断定位；跨脚本与后续 Capture 使用逻辑标记 portfolio-capture。它是 CPU 侧证据标记；GPU 侧没有同名 marker，anchor 帧由「应用在该帧程序化触发单帧捕获 + 帧内只出现在截屏帧的 Screenshot 事件 + 该帧 anchor.json/anchor.ppm」三者绑定，见 [D 记录](../evidence/BATCH-D.md)。静态 anchor 的 commandHash 只在同协议同采样相位比较，不与旧 fixed-frame=300 或动态末帧强行等同。

这段 Demo 展示程序化的 RHI 尺寸/暂停恢复和 shader 候选处理契约。它不展示 OS 最小化、文件监听热更新、新 shader 效果、自由相机或动画。状态变化很短，视频可用真实事件字幕/慢放解释，不伪造交互。架构、范围、收益和入口仍用静态文档。

## 视频 / Capture 协议（D 批次）

C 批次的节奏（挂起后立刻重建、临时尺寸只比原尺寸大 16 像素）在画面上不可辨认：
场景是固定相机的静止画面，挂起状态不呈现任何新帧，0.8% 的尺寸差看不出来。
D 批次因此只增加两项**可读性控制**，事件与判定条件不变：

- `--tour-hold-frames=N`：每个事件之后保持该状态 N 帧（0 = C 批次的历史节奏）。
- `--tour-temporary-width/height=N`：临时 extent（缺省 = 历史 width+16 / height+8）。
- `--vsync=on`：按显示器刷新率呈现，使停留时长可预期（缺省 off，与 C 批次一致）。

视频/Capture 使用独立 recipe `assets/recipes/m9-portfolio-video.json`
（1562 帧 / warmup 780 / 停留 90 / 临时 960×540 / VSync on）：

~~~powershell
python tools/portfolio/run_demo.py --exe out/build/windows-msvc-debug/samples/rhi_sandbox/Release/MiniEngineSandbox.exe --manifest out/demo/scene/manifest.json --recipe assets/recipes/m9-portfolio-video.json --output out/demo-video
~~~

anchor 帧是该协议下的第 781 帧。双后端 anchor Capture（D3D11 → RenderDoc、
D3D12 → PIX）由应用在该帧程序化触发，记录与哈希见 [D 记录](../evidence/BATCH-D.md)。
**视频尚未录制**：本机没有可用的录屏工具，录制方式待所有者决定。

[彩排证据](../evidence/DEMO-REHEARSAL.json)记录六次运行的实际事件、静态身份、二进制/配方/驱动哈希、产物哈希及源码文件快照。大日志和 PPM 保留在本地 out；D 批次另行采集正式画面和 Capture。

## 视频（D 批次，已完成）
成片：out/d-batch/video/d3d12-v2/demo.mp4（1920x1080/60fps/29.8s）
- SHA-256 df7405e024d67594eb68c44db68dd6537fa0af551783dead6049709915549537
- 真实运行、无 Capture 注入；字幕由工具按事件锚定烧入（见 tools/portfolio/record_demo_video.py）
- 目前只有 D3D12 成片；原片与 PDB 等大文件留在本地，不进公开集合
- D3D11: demo.mp4 1920x1080/60fps/26.7s
- D3D11 sha256 d9e4a0023398e6b4e9b955a91d3ba6c0cb4ad3fd6544d7f4fbf734be15a27ae7
- 提交后重录（新 EXE）：D3D12 133e92d0… / D3D11 f5ef8a16…；旧成片作废
