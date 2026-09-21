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

`git ls-tree -r --name-only origin/main` 与本地 `git ls-files`、以及公开清单逐项比对：

- 远端文件 **772** 个，本地跟踪 **772** 个，公开清单 **771** 个 + 清单自身
- `remote == local`：**True**
- `remote == manifest ∪ {清单自身}`：**True**
- 缺失 / 多余：**0 / 0**

## 附件（GitHub 侧实算哈希）

| 附件 | 大小 | 状态 | GitHub `digest` | 与本地一致 |
| --- | --- | --- | --- | --- |
| `lumabough-5d320c7-v8.zip` | 3,466,423 | uploaded | `sha256:a94e41dbbb4438d82c1752734f8fd60cc224e7b95a63b6fa39c2e44fe96a5132` | ✓ |
| `lumabough-demo-d3d12.mp4` | 1,431,663 | uploaded | `sha256:542e3b3e829afdf81b706000b9c913a22212eda10b6ad790002f3cc1601517af` | ✓ |
| `lumabough-demo-d3d11.mp4` | 1,205,881 | uploaded | `sha256:1602ca64ef79af1f71ae10958c50826b3e61a217ade04ef5560b40d71ced95c2` | ✓ |
| `lumabough-performance-evidence-v1.zip` | 1,082,886 | uploaded | `sha256:c33c5b39d05b77b5587acdfe85b9131e377aeb48828c61ebe21acc0c10489ff5` | ✓ |
| `SHA256SUMS-external.txt` | 569 | uploaded | `sha256:1424122bb500139b424e9be01f45db41b43ae2021469c50e4038adca5c63aaf9` | ✓ |

Release 文案见 `out/f5/release-notes.md`（随本提交一并归档），内容包含附件哈希、已验证范围、
未验证项（无真实读者走查）、不含内容与复跑命令。

## 匿名核验（本轮）

方法：独立 HTTP 客户端，**无登录、无认证头、无 Cookie，不使用 gh**；只取状态码与内容类型，不只看 200。

| 目标 | 结果 | 说明 |
| --- | --- | --- |
| 仓库主页 | **404** | 与 private 一致，未发生意外公开 |
| Release 页 | **404** | 同上 |
| 附件下载 URL | **404** | 同上 |

即：**已核实"当前未被匿名访问"**，但 F3 要求的"匿名可浏览 + 实际下载 + 哈希与 smoke 核验"
在 private 下无法完成——它不是失败，而是等待转 public 的授权。

## 待授权 / 未完成

- [ ] 转 public（唯一阻断项；授权后我做匿名下载、哈希比对与新解压目录 smoke）
- [ ] 匿名下载 ZIP 并与 `a94e41db…` 比对、按 `SHA256SUMS.txt` 校验逐文件
- [ ] 匿名核对 README 图片与链接、Release 页、每个附件
- [ ] 真实读者走查仍按所有者决定延后（不因发布而改变声明）
- [ ] 路线图 F 行在以上完成后才可标"完成"

## 事故处理约定

发现错包、泄漏或损坏立即停止后续发布并通知所有者；不覆盖同名 tag、不强推，
按 [F 提案](F-PROPOSAL.md) 的回退方案"发布新修订版本"（如 `v0.1.0-preview.2`）处理。

## 环境备注

本机 git 配置了本地 HTTP 代理（地址不随本文件公开），该代理下 TLS 握手失败；
本次推送/取回统一使用 `git -c http.proxy=` 直连成功，未修改所有者的代理配置。
