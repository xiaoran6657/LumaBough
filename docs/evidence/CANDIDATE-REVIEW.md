# E5 候选裁定

状态：**候选通过，范围已缩减**；`publication=BLOCKED`（无 remote、未推送、未公开）。

## 候选绑定

| 项 | 值 |
| --- | --- |
| 源码集合 | `PUBLICATION-FILES.json`：768 个受审文件；含清单自身共 769 个 Git 文件 |
| 运行提交 | `5d320c773c7c9943e28fde2185204d05ca9ca19c`（EXE 内嵌；六条 E4 记录均绑定此提交的 EXE） |
| 证据/打包提交 | `packagingCommit` = 见包内 `PACKAGE-MANIFEST.json`（本文件所在提交另行标注） |
| 包 | `out/e-batch/package/lumabough-5d320c7-v8`：158 个受覆盖文件 / 10,093,885 B |
| 交付 ZIP | `lumabough-5d320c7-v8.zip`：160 条目（158 受覆盖 + 2 自排除），3,466,423 B，SHA-256 `a94e41dbbb4438d82c1752734f8fd60cc224e7b95a63b6fa39c2e44fe96a5132` |
| 外部校验 | `out/e-batch/package/lumabough-5d320c7-v8-external.txt`（含 ZIP 自身哈希） |
| EXE | SHA-256 `f673d0baf9168574a59a75cc9bb68c8c48d23a816ff4c30496a5d195aa6d5b18` |
| 场景 manifest | SHA-256 `4ae6eda980221395a80e3bb03374d051a07b8cd197f8a5c0b55097f427ba6260` |
| 成片 | D3D12 `542e3b3e829afdf81b706000b9c913a22212eda10b6ad790002f3cc1601517af`（1,431,663 B）；D3D11 `1602ca64ef79af1f71ae10958c50826b3e61a217ade04ef5560b40d71ced95c2`（1,205,881 B） |

## 门禁输出（可复跑）

~~~powershell
python -B tools/portfolio/validate_publication.py --stage entry
python -B tools/portfolio/validate_publication.py --stage candidate --package out/e-batch/package/lumabough-5d320c7-v8.zip
~~~

第二条输出 `PASS candidate: 768 reviewed files, package bytes, privacy gate and E4 record verified`，
并打印读者延后提示。逐项含义：

1. **清单与链接**：哈希、大小、相对链接与锚点、第三方身份表、架构导出新鲜度。
2. **包字节**：包内每个文件与 `PACKAGE-MANIFEST.json`、`SHA256SUMS.txt` 逐项一致；没有未列出的文件；
   免校验名单由门禁固定为那两个清单文件，包不能自行扩大。
3. **EXE 与构建绑定**：包内 EXE 与 `exeSha256` 一致；`builtAtCommit` 是 HEAD 的祖先；
   `engine/ samples/ shaders/ assets/ tests/ cmake/ tools-benchmark tools-assets tools-shader_compiler
   tools-asset_cooker` 与根 `CMakeLists.txt`、`CMakePresets.json`、`tools/CMakeLists.txt` 在该提交之后没有改动。
4. **E3 隐私门**：`source-set` + `git` + 包 ZIP 三目标重扫，49 个唯一键全部 `approved`，扫描器退出 0；
   二进制字符串另行核查：EXE 内**不含**工程绝对路径（`__FILE__` 前缀已裁剪，`M610_PROJECT_ROOT` 宏已移除）。
5. **E4 记录**：六条必需运行 ID 齐全、不重复、`complete=true`、正例退出码 0、负例退出码 2，
   且每条都绑定本包 EXE；读者走查按所有者决定延后（门禁打印提示，不静默）。
6. **依赖与许可**：[依赖缺口](DEPENDENCY-GAPS.md)、[许可预审](LICENSING-REVIEW.md) 与包内 `licenses/`
   （自有 MIT、运行包副本的第三方/资产说明、`REDISTRIBUTABLES.md` 记录 VC 运行时 14.51.36247.0 与 WinPixEventRuntime 的哈希）。
7. **独立运行**：[E4 记录](E4-RESULTS.json)：第二台机器（Windows 11 / RTX 3090 Ti）双后端 1202 帧、
   双后端 Demo 事件路径、换目录启动、负例；跨机 `graphHash` / `commandHash` / `visibleSequenceHash` 一致。
8. **测试**：工具测试 52 项、契约测试 55 项均 OK（含候选门字节核对、构建敏感清单、E4 完整性负例、读者状态语义）。

## 缩减的承诺（所有者决定，必须随包公开）

- **真实读者走查延后**：`E4-RESULTS.json.reader.status=deferred-by-owner`。不得声称经过真实读者验证，
  也不得把"示例能跑通"当作可用性证据。
- **不宣称当前加速**：P 的有效组结论为 1 INCONCLUSIVE + 3 REJECTED、无 ACCEPTED；历史 C-M9-002 保持 BLOCKED。
- **支持范围**：仅 Windows 10/11 x64 与 `m4-visual-baseline`；`m7-*` 性能场景、Tracy 与 Capture 工具链不在包内。
- **跨机结论边界**：第二台机器只证明该机行为；未做全新系统或更多 GPU 的通用承诺。

## 未完成 / 未授权

- 未创建 remote、未推送、未公开；F 需要所有者对目标仓库、ref、可见性、附件与文案的逐项授权
  （见 [F 提案](F-PROPOSAL.md) 的授权清单）。
- 原始 Capture、视频原片、PDB、烘焙报告只留本地 `out`，不进候选集合。
