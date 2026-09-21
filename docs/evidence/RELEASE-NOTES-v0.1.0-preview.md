# LumaBough v0.1.0-preview（预发布）· Release 文案存档

本文件是 Release `v0.1.0-preview` 文案的仓库内快照，便于不访问远端也能读到发布说明；
远端地址：https://github.com/xiaoran6657/LumaBough/releases/tag/v0.1.0-preview

本 Release 的三类产物都来自本仓库源码，并绑定具体提交：
**运行提交 `5d320c7`**（EXE 内嵌）、打包提交见包内 `PACKAGE-MANIFEST.json`、发布提交 `f26b8b9`（tag 指向）。
它们互不冒充：源码提交 ≠ 运行提交 ≠ 打包提交。

## 附件

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `lumabough-5d320c7-v8.zip` | 3,466,423 B | `a94e41dbbb4438d82c1752734f8fd60cc224e7b95a63b6fa39c2e44fe96a5132` |
| `lumabough-demo-d3d12.mp4` | 1,431,663 B | `542e3b3e829afdf81b706000b9c913a22212eda10b6ad790002f3cc1601517af` |
| `lumabough-demo-d3d11.mp4` | 1,205,881 B | `1602ca64ef79af1f71ae10958c50826b3e61a217ade04ef5560b40d71ced95c2` |
| `lumabough-performance-evidence-v1.zip` | 1,082,886 B | `c33c5b39d05b77b5587acdfe85b9131e377aeb48828c61ebe21acc0c10489ff5` |
| `SHA256SUMS-external.txt` | — | 覆盖以上四个附件自身的哈希 |

校验顺序：先用 `SHA256SUMS-external.txt` 校验附件自身，再按各包内 `SHA256SUMS.txt` 校验逐文件。

## 运行包

- 160 个条目（158 个受覆盖文件 + 2 个包内清单文件，后者在包内标注为自排除）。
- 解压到任意目录后按包内 `RUN.md` 两条命令即可双后端运行；**不需要** Visual Studio、SDK、DXC 或 Python。
- 已验证：Windows 10/11 x64、D3D11 与 D3D12、场景 `m4-visual-baseline`；第二台机器
  （Windows 11 / NVIDIA RTX 3090 Ti）独立运行双后端 1202 帧与 Demo 事件路径通过，
  跨机 `graphHash` / `commandHash` / `visibleSequenceHash` 一致；故意删掉着色器目录时会明确报错而不是静默出错。
- 未验证：无真实读者走查（按所有者决定延后）；`m7-*` 性能场景与 GPU Capture 工具链不在包内；
  不承诺全新系统或更多 GPU。

## 演示视频

双后端各一份，展示连续 Demo 的四个事件：程序化 RHI 暂停（交换链 0×0）→ 重建为 960×540
（同一份 bytecode 重建 pipeline）→ 非法 shader 候选（空 bytecode）被拒绝 → 恢复 1920×1080
（末帧与动作前锚点帧像素一致）。字幕说明每个阶段，关键字幕停留不少于 1.5 秒。

## 性能证据包

去标识的逐帧输入（25 次运行 × 600 帧）、冻结协议、组记录与公开分析器副本；
`SANITIZATION.json` 记录每个被处理文件的 `originalSha256` → `publicSha256`。
包内分析器复算结果与冻结结论**逐项一致**：**1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED**。
**不宣称当前加速**；历史 C-M9-002 与历史性能摘要保持 BLOCKED；该包不代表本运行包的性能。

## 已知限制与不含内容

- 无真实读者验证。
- 不含：PDB、原始 Capture、视频原片、资产源树、DXC/VS/SDK、完整本机日志。
- 许可：自有部分为 MIT；随包的 VC 运行时（14.51.36247.0）与 WinPixEventRuntime 按各自条款分发，
  见包内 `licenses/REDISTRIBUTABLES.md` 与仓库 `docs/evidence/LICENSING-REVIEW.md`。
- 能力结论以仓库内 [Claim Ledger](CLAIM-LEDGER.csv) 与 [候选裁定](CANDIDATE-REVIEW.md) 为准。

## 复跑核验

~~~powershell
# 源码仓库内；包目录或 ZIP 均可
python -B tools/portfolio/validate_publication.py --stage candidate --package <解压目录或 ZIP>
~~~

它会检查：逐文件清单与链接、包内字节与两份校验表、EXE 与构建提交绑定、隐私门（三目标扫描 + 逐项批准基线）、
E4 记录与包的一一绑定。
