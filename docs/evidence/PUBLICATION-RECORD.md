# F 发布记录（执行部分）

状态：**远端已创建并发布预发布版本（私有）**；匿名下载核验与"标记 F 完成"仍待转 public 的授权。
本文件只记录真实执行结果；未执行的部分明确标注为待授权，不写成已完成。

## 远端与提交

| 项 | 值 |
| --- | --- |
| 账号 / 仓库 | `xiaoran6657` / `LumaBough`（由本次执行创建；创建前只读查询确认不存在） |
| 远端 URL | https://github.com/xiaoran6657/LumaBough |
| 可见性 | **private**（按提案先私有；转 public 需单独授权） |
| 分支 | `main` |
| 发布提交（tag 指向） | `f26b8b98a727532081d0793bf34c54b230cd7173` |
| tag | `v0.1.0-preview`（annotated） |
| Release | https://github.com/xiaoran6657/LumaBough/releases/tag/v0.1.0-preview （标为预发布） |
| 运行提交 | `5d320c773c7c9943e28fde2185204d05ca9ca19c`（EXE 内嵌；与发布提交分开记录） |
| 打包提交 | 见包内 `PACKAGE-MANIFEST.json` 的 `packagingCommit` |

## 远端 tree 核验

`git ls-tree -r --name-only <ref>` 与本地 `git ls-files`、以及公开清单逐项比对。
**两个快照分别核验，不要把旧快照描述成当前 main**：

| 快照 | 提交 | 受审文件 | 加清单自身 | 比对结果 |
| --- | --- | --- | --- | --- |
| 发布 tag `v0.1.0-preview` | `f26b8b98a727532081d0793bf34c54b230cd7173` | 771 | **772** | 与本地 blob、清单完全一致；缺失/多余 0/0 |
| 当前 `main` | 本文件所在提交（发布后追加的记录提交） | 772 | **773** | 与本地 blob、清单完全一致；缺失/多余 0/0 |

## 附件（GitHub 侧实算哈希）

| 附件 | 大小 | 状态 | GitHub `digest` | 与本地一致 |
| --- | --- | --- | --- | --- |
| `lumabough-5d320c7-v8.zip` | 3,466,423 | uploaded | `sha256:a94e41dbbb4438d82c1752734f8fd60cc224e7b95a63b6fa39c2e44fe96a5132` | ✓ |
| `lumabough-demo-d3d12.mp4` | 1,431,663 | uploaded | `sha256:542e3b3e829afdf81b706000b9c913a22212eda10b6ad790002f3cc1601517af` | ✓ |
| `lumabough-demo-d3d11.mp4` | 1,205,881 | uploaded | `sha256:1602ca64ef79af1f71ae10958c50826b3e61a217ade04ef5560b40d71ced95c2` | ✓ |
| `lumabough-performance-evidence-v1.zip` | 1,082,886 | uploaded | `sha256:c33c5b39d05b77b5587acdfe85b9131e377aeb48828c61ebe21acc0c10489ff5` | ✓ |
| `SHA256SUMS-external.txt` | 569 | uploaded | `sha256:1424122bb500139b424e9be01f45db41b43ae2021469c50e4038adca5c63aaf9` | ✓ |

Release 文案的**仓库内永久快照**：[RELEASE-NOTES-v0.1.0-preview.md](RELEASE-NOTES-v0.1.0-preview.md)
（已纳入公开清单）；远端 Release 页面：
https://github.com/xiaoran6657/LumaBough/releases/tag/v0.1.0-preview 。
提交时的原始草稿只在本地 `out/f5/release-notes.md` 留存（未纳入 Git，不随仓库发布）。
文案内容包含附件哈希、已验证范围、未验证项（无真实读者走查）、不含内容与复跑命令。

## 匿名核验

方法：独立 HTTP 客户端，**无登录、无认证头、无 Cookie，不使用 gh**；核对状态码、内容类型与实际内容，
不只看 200。两轮分别对应可见性变更前与变更后。

### 变更前（private）

| 目标 | 结果 | 说明 |
| --- | --- | --- |
| 仓库主页 | **404** | 与 private 一致，未发生意外公开 |
| Release 页 | **404** | 同上 |
| 附件下载 URL | **404** | 同上 |

### 变更后（public，本轮完成）

| 目标 | 结果 |
| --- | --- |
| 仓库主页 | 200 `text/html` |
| README 预览图（raw） | 200 `image/png` 324,110 B |
| Release 页 | 200 `text/html` |

匿名**实际下载**并与本地候选逐一比对：

| 附件 | 下载后 SHA-256 | 与本地一致 |
| --- | --- | --- |
| `lumabough-5d320c7-v8.zip` | `a94e41dbbb4438d82c1752734f8fd60cc224e7b95a63b6fa39c2e44fe96a5132` | ✓ |
| `lumabough-demo-d3d12.mp4` | `542e3b3e829afdf81b706000b9c913a22212eda10b6ad790002f3cc1601517af` | ✓ |
| `lumabough-demo-d3d11.mp4` | `1602ca64ef79af1f71ae10958c50826b3e61a217ade04ef5560b40d71ced95c2` | ✓ |
| `lumabough-performance-evidence-v1.zip` | `c33c5b39d05b77b5587acdfe85b9131e377aeb48828c61ebe21acc0c10489ff5` | ✓ |

下载包解压到**新目录**后的核验：

- 按包内 `SHA256SUMS.txt` 校验：158 个受覆盖文件**全部匹配**，多出的 2 个是包内标注的自排除清单文件
- 在新解压目录运行：D3D12 1202 帧 PASS、D3D11 1202 帧 PASS、D3D12 `--exercise-changes` 1202 帧 PASS
  （末行 `complete-clean-exit`）
- 关键身份：`graphHash 0xBC2FC5CF0A38B0FD`、`commandHash d87bf3a6…`、
  `visibleSequenceHash 0x61E54BF2EB73DB0E`、`warningErrors 0`、EXE 内嵌 `sourceCommit=5d320c7`
  ——与第一台、第二台机器的记录一致

公开时间与方法：2026-09-22，PowerShell `Invoke-WebRequest`（无凭据、无 Cookie、不使用 gh），
产物保留在 `out/f5/anon/`（下载包、附件与新解压目录的运行输出）。

## 待授权 / 未完成

- [x] 转 public（已按授权执行；`gh repo edit --visibility public --accept-visibility-change-consequences`）
- [x] 匿名下载 ZIP 并与 `a94e41db…` 比对、按 `SHA256SUMS.txt` 校验逐文件
- [x] 匿名核对 README 图片与链接、Release 页、每个附件
- [ ] **2 个处置键待批准**（`PUBLICATION-RECORD.md` 与 `RELEASE-NOTES-v0.1.0-preview.md` 里的账号/仓库地址，
  均为 `proposed`；批准前候选门仍会报待批准项）
- [ ] 真实读者走查仍按所有者决定延后（不因发布而改变声明）
- [ ] 路线图 F 行在上述处置键批准后标"完成"

## 事故处理约定

发现错包、泄漏或损坏立即停止后续发布并通知所有者；不覆盖同名 tag、不强推，
按 [F 提案](F-PROPOSAL.md) 的回退方案"发布新修订版本"（如 `v0.1.0-preview.2`）处理。

## 环境备注

本机 git 配置了本地 HTTP 代理（地址不随本文件公开）。实测三种组合：
- 代理 + schannel（默认）：TLS 握手失败；
- 直连（`git -c http.proxy=`）：前两次成功，随后出现空回复/连接超时；
- **代理 + `git -c http.sslBackend=openssl`：稳定成功**（本次最终采用）。

均为一次性 `-c` 覆盖，未修改所有者的代理与 SSL 配置。
