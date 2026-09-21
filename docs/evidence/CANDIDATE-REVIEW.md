# E5 候选裁定

状态：**候选通过，范围已缩减**；`publication=BLOCKED`（无 remote、未推送、未公开）。

## 候选绑定

| 项 | 值 |
| --- | --- |
| 源码集合 | `PUBLICATION-FILES.json`（766 文件，entry 校验通过） |
| 证据/交付提交 | 本文件所在提交（见 `git log`） |
| 运行提交 | `0403d203419694cd185d3223ba77c75175456bcf`（EXE 内嵌） |
| 包 | `out/e-batch/package/lumabough-0403d20-v7`（157 文件 / 10,092,318 B） |
| 交付 ZIP | `lumabough-0403d20-v7.zip`（3,464,797 B / SHA-256 `071fbc41c6fa7fed772d9607776e6bbd901a14146d48c2efd7e1223fc413f77f`） |
| 包内清单 | `PACKAGE-MANIFEST.json`（含 `selfExcluded` 标注）与 `SHA256SUMS.txt` |
| EXE | SHA-256 `7e75b7767d0b4273b4b77c8d14e0e5ded682b9538581265f16951e17cffc6f28` |
| 场景 manifest | SHA-256 `4ae6eda980221395a80e3bb03374d051a07b8cd197f8a5c0b55097f427ba6260` |

## 门禁输出（可复跑）

~~~powershell
python -B tools/portfolio/validate_publication.py --stage entry
python -B tools/portfolio/validate_publication.py --stage candidate --package out/e-batch/package/lumabough-0403d20-v7.zip
~~~

第二条输出 `PASS candidate: 766 reviewed files, package bytes, privacy gate and E4 record verified`，
并打印读者延后提示。逐项含义：

1. **清单与链接**：哈希、大小、相对链接与锚点、第三方身份表、架构导出新鲜度。
2. **包字节**：包内每个文件与 `PACKAGE-MANIFEST.json`、`SHA256SUMS.txt` 逐项一致；没有未列出的文件；
   `selfExcluded` 的两个清单文件存在且明确标注。
3. **EXE 与构建绑定**：包内 EXE 与 `exeSha256` 一致；`builtAtCommit` 是 HEAD 的祖先；
   `engine/`、`samples/`、`shaders/`、`assets/` 在该提交之后没有改动（改动必须重建）。
4. **E3 隐私门**：`source-set` + `git` + 包 ZIP 三目标重扫，49 个唯一键全部 `approved`，扫描器退出 0。
5. **E4 记录绑定**：`E4-RESULTS.json` 的 EXE/zip 哈希与本次包一致；第二台机器逐项运行均为 PASS 或预期失败。
6. **依赖与许可**：[依赖缺口](DEPENDENCY-GAPS.md)、[许可预审](LICENSING-REVIEW.md) 与包内 `licenses/`
   （自有 LICENSE、THIRD-PARTY-NOTICES、ASSET-LICENSES + 随包 VC 运行时 4 个 DLL 与 WinPixEventRuntime 许可）。
7. **独立运行**：[E4 记录](E4-RESULTS.json)：第二台机器（Windows 11 / RTX 3090 Ti）双后端 1202 帧、
   Demo 事件路径、换目录启动、负例，以及跨机哈希一致。
8. **测试**：工具测试 52 项、契约测试 51 项均 OK（含候选门字节核对与读者状态语义）。

## 缩减的承诺（所有者决定，必须随包公开）

- **真实读者走查延后**：`E4-RESULTS.json.reader.status=deferred-by-owner`。不得声称经过真实读者验证，
  也不得把"示例能跑通"当作可用性证据。
- **不宣称当前加速**：P 的有效组结论为 1 INCONCLUSIVE + 3 REJECTED、无 ACCEPTED；历史 C-M9-002 保持 BLOCKED。
- **支持范围**：仅 Windows 10/11 x64 与 `m4-visual-baseline`；`m7-*` 性能场景、Tracy 与 Capture 工具链不在包内。
- **跨机结论边界**：第二台机器只证明该机行为；未做全新系统或更多 GPU 的通用承诺。

## 未完成 / 未授权

- 未创建 remote、未推送、未公开；F 需要所有者对目标仓库、ref、可见性与文案的逐项授权。
- 原始 Capture、视频、PDB、烘焙报告只留本地 `out`，不进候选集合。
