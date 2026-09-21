# F 发布提案（草案，尚未执行）

**本文件不是发布授权**：不创建 remote、不推送、不公开。执行前需要所有者按第 8 节逐项授权。
本节所有哈希与数量对应候选包 v8（运行提交 `5d320c7`），发布时以最终干净提交重新核对。

## 1. 目标与可见性

| 项 | 提案 | 状态 |
| --- | --- | --- |
| 目标仓库 | `xiaoran6657/LumaBough`（名称可用性在执行时确认；不导入任何旧历史） | 待授权 |
| 可见性 | **先 private** 完成远端检查，再按明确授权转 public | 待授权 |
| 默认分支 | `main` | 待授权 |
| 发布标记 | annotated tag `v0.1.0-preview`，Release 标为 **预发布** | 待授权 |
| 其它仓库 | MiniEngine 保持私有，不做镜像、不导入历史 | 已明确 |

## 2. ref 与提交计划

发布提交 = 本提案获批后的一次干净提交（含裁定记录与本文档）；tag 指向该提交。
**运行包构建于 `5d320c7`**，与发布提交的关系在 Release 文案中写明（源码提交 ≠ 运行提交 ≠ 打包提交，逐一标注）。
只推送指定的 `main` 提交与 tag，不使用全引用推送。

## 3. 三个公开范围（分别授权）

| 范围 | 内容 | 现状 |
| --- | --- | --- |
| 源码仓库 | `PUBLICATION-FILES.json` 的 771 个受审文件 + 清单自身 = 772 个 Git 文件；**包含** `assets/source/` 的 11 个源资产（glTF/bin/HDR，许可见 `assets/LICENSES.md`） | 由 entry/candidate 门强制 |
| 运行 ZIP | 160 条目 = 158 个受覆盖文件 + 2 个自排除清单文件；不含资产源树（只带烘焙后的 `scene/`） | 附件，见第 4 节 |
| 成片 | D 批次双后端 mp4（1.21 MB / 1.43 MB） | 提案：**随 Release 附件公开**，README 给预览图与明确入口 |

源码仓库不含 `out/`、PDB、Capture、视频原片（`localOnlyRoots` 明确列出）；不改动、不覆盖历史失败记录。

## 4. 附件（Release assets）

| 附件 | 大小 | SHA-256 |
| --- | --- | --- |
| `lumabough-5d320c7-v8.zip` | 3,466,423 B | `a94e41dbbb4438d82c1752734f8fd60cc224e7b95a63b6fa39c2e44fe96a5132` |
| `lumabough-demo-d3d12.mp4` | 1,431,663 B | `542e3b3e829afdf81b706000b9c913a22212eda10b6ad790002f3cc1601517af` |
| `lumabough-demo-d3d11.mp4` | 1,205,881 B | `1602ca64ef79af1f71ae10958c50826b3e61a217ade04ef5560b40d71ced95c2` |
| `lumabough-performance-evidence-v1.zip` | 1,082,886 B | `c33c5b39d05b77b5587acdfe85b9131e377aeb48828c61ebe21acc0c10489ff5` |
| `SHA256SUMS-external.txt` | 小 | **包含以上每个附件自身的哈希**，供下载后先校验再解压；已按最终附件名生成于 `out/e5/attachments/SHA256SUMS-external.txt`（附件副本同目录） |

### 4b 性能证据包（去标识、可复算）

内容：冻结协议（`PREREGISTRATION.json`）、组记录（`formal/summary.json`）、25 次运行的**逐帧样本**
（`formal/<runId>/run.json`，含 600 帧/次）、运行备注、公开分析器副本，以及
`SANITIZATION.json`（每个被改写文件的 `originalSha256` → `publicSha256`）。
脱敏内容：机器清单路径、EXE 绝对路径、主机名与适配器 LUID；保留实验 ID、运行顺序、有效性字段、统计与逐帧数组。
结论不变：**1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED**；不把该实验当作 v8 的新性能测量。
复算方式：`python analyzer/summarize_portfolio_experiment.py --input . --output recompute.json`
（本机已实测：`validation=PASS_LOCAL`，四个比较与冻结结论逐项一致）。
细节与限制见 [公开性能证据包](PERFORMANCE-EVIDENCE-PACK.md)。

## 5. README 草稿（要点）

1. **定位**：Windows 上的小型 RHI/渲染图实验工程，带可复跑的演示、Capture 记录与性能方法学材料。
2. **是什么 / 不是什么**：双后端（D3D11/D3D12）程序化渲染演示与实验工具；**不是**通用引擎、
   不是性能承诺、不含编辑器或资产流水线产品化内容。
3. **支持矩阵**：Windows 10/11 x64；D3D11 与 D3D12；场景 `m4-visual-baseline`；
   app-local VC 运行时 + WinPixEventRuntime；`m7-*` 性能场景与 Capture 工具链不在包内。
4. **快速开始**：按 `RUN.md` 两条命令，期望 `status PASS` 与 `graphHash`；成片见 Release 附件。
5. **验证程度**：候选门（可复跑）+ 第二台机器独立运行（含跨机哈希一致）+ 隐私逐项批准基线；
   **明确声明：未做真实读者走查**。
6. **声明边界**：不宣称当前加速；历史 C-M9-002 数值是历史结论；性能证据包只支持复算方法学，不支持加速声明。

## 6. Release notes 草稿

- 交付：候选运行包（160 条目）+ 逐文件 SHA-256 + 许可证与第三方 notices + 运行说明与支持矩阵。
- 演示：双后端成片（含字幕说明暂停/重建/拒绝非法候选/恢复四个事件）。
- 性能：去标识证据包（逐帧输入 + 协议 + 分析器），结论 1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED。
- 验证：候选门通过；第二台机器双后端 1202 帧与 Demo 事件路径通过；跨机哈希一致。
- 已知限制：无真实读者验证；不支持 `m7-*` 性能场景与 Capture 工具链；只承诺实测范围内的行为。
- 不含：PDB、原始 Capture、视频原片、DXC/SDK/VS、资产源树、完整本机日志。

## 7. 风险与回退

| 风险 | 缓解 | 回退 |
| --- | --- | --- |
| 许可或署名遗漏 | 许可预审 + 包内 notices + 逐文件清单门 | 补文件后重跑候选门，重新打包并**发布新修订版本** |
| 隐私遗漏 | E3 三目标扫描 + 逐项批准基线 + 二进制字符串核查 | 脱敏或排除后重扫、重新批准受影响项，再发新修订 |
| 跨机失败 | 候选门 + 第二台机器记录 + 负例（响亮失败） | 修复后重建、重打包、重跑 E4 受影响项，再发新修订 |
| 被误读为性能证明 | README/Release 明确"不宣称当前加速" | 收紧文案或撤下 Release 附件 |

**回退约定**：公开后**不重发同名 tag**，一律发布新修订版本（如 `v0.1.0-preview.2`）；
把仓库转回 private 不能收回已被下载或已缓存的内容，因此回退以"新修订 + 明确的撤回说明"为准。

## 8. 授权清单（需要所有者逐项确认）

- [ ] 目标仓库与可见性（先 private，再单独授权转 public）
- [ ] 发布提交范围与 tag 名（`v0.1.0-preview`，预发布）
- [ ] 附件集合：v8 ZIP、两份 MP4、性能证据包、`SHA256SUMS-external.txt`
- [ ] 成片公开范围（提案：随 Release 附件）
- [ ] 性能材料范围（提案：附去标识逐帧证据包；原始冻结数据保持本地）
- [ ] README 与 Release 最终文案
- [ ] 明确允许 push 的具体动作与范围（仅指定 `main` 提交与该 tag，含凭据方式）
- [ ] 真实读者：维持已批准的延后决定，公开文案如实说明
