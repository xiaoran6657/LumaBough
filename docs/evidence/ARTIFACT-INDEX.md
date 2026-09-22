# 产物索引

## 当前公开交付

[v0.1.0-preview Release 与全部附件](https://github.com/xiaoran6657/LumaBough/releases/tag/v0.1.0-preview)已公开。
下载后先用 SHA256SUMS-external.txt 校验文件；运行包解压后还可按包内 SHA256SUMS.txt 校验。
附件的完整 SHA-256、字节数和匿名运行结果见[发布记录](PUBLICATION-RECORD.md)，本页不复制第二份哈希表。

| 文件 / 材料 | 用途与范围 |
|---|---|
| lumabough-5d320c7-v8.zip | Windows x64 双后端运行包；按包内 RUN.md 启动，不含性能场景或 Capture 工具链 |
| lumabough-demo-d3d12.mp4 / lumabough-demo-d3d11.mp4 | 双后端连续动态展示；[Demo 边界](../portfolio/DEMO.md) |
| lumabough-performance-evidence-v1.zip | 004 的冻结协议、25 × 600 逐帧样本和分析器；[复算命令与限制](PERFORMANCE-EVIDENCE-PACK.md) |
| SHA256SUMS-external.txt | 覆盖上述四个附件自身 SHA-256 |

发布 tag 指向 f26b8b98a727532081d0793bf34c54b230cd7173，运行构建为
5d320c773c7c9943e28fde2185204d05ca9ca19c；性能实验另有 EXE 与源码快照，
不能用运行包身份代替性能实验身份。[新性能记录](NEW-PERFORMANCE.md)给出
1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED。

## 当前记录与本地产物

[候选包裁定](CANDIDATE-REVIEW.md) · [E4 记录](E4-RESULTS.json) · [D Capture 与成片记录](BATCH-D.md)。

PIX/RenderDoc anchor 已完成本地结构和 GUI 核对，但原始 Capture、PDB、原片不属于公开附件。
本地核对不等于公众可下载回放；七项 [Claim Ledger](CLAIM-LEDGER.csv) 中 C-M9-007 保留这一公开交付限制。
真实读者走查仍按所有者决定延后。

## 历史记录与保留限制

| 入口 | 含义 |
|---|---|
| [历史勘正](HISTORICAL-CLAIMS.md) | 不继承原仓 PASS；池内存稳定不代表进程零泄漏 |
| [B 记录](BATCH-B.md)、[预览身份](RENDER-PREVIEW.json) | 当时本地构建与静态预览，身份不替换为最终发布构建 |
| [C 记录](BATCH-C.md)、[连续彩排](DEMO-REHEARSAL.json) | 当时六次双后端 Demo |
| [历史统计](PERFORMANCE-SUMMARY.json)、[输入变换清单](performance/TRANSFORM-MANIFEST.json) | 历史数值可复算，但 C-M9-002 实际源码身份仍 BLOCKED |
| [早期证据清单](EVIDENCE-MANIFEST.json) | B/C 时点的 artifact 哈希；LOCAL/BLOCKED 不代表当前远端可见性 |
| [逐文件公开集合](PUBLICATION-FILES.json) | 当前受审文件及哈希；entry schema 的 publicationStatus 不裁定远端发布 |

未公开的历史原始 parity/validation/soak 与故障契约材料不能由新 Demo、004 性能实验或摘要代替。
原历史身份/访问限制继续按各条记录解释，不因已有 Release 一律变 PASS。
