> 这是 A 批次的历史记录；当前状态和命令见 [批次 B](BATCH-B.md)。

# 导入审查：批次 A

更新：2026-09-20。首批接手和缺口登记完成，公开发布仍为 **BLOCKED**。

| 检查 | 实际结果 |
|---|---|
| 独立性 M9-P01 | 新建独立 Git main，初始无提交、无 remote；未复制旧历史 |
| 初始复制 | 669 文件与源工作区逐字节相同；其中 663 tracked，6 个缓存；0 漏项、0 内容变化 |
| 文件边界 M9-P02 | 当前清单允许候选 369，待审 308，排除 6；清单自身另行排除 hash |
| 许可/声明 M9-P03 | 自有项目 MIT 已选；第三方/资产待办已列明；C-M9-005 已收窄；V2 Ledger 6 BLOCKED / 1 N/A |
| 实现验证 | 仅静态依赖和新增 import 工具测试；未运行 C++ configure/build/CTest/GPU |
| 发布 | 未提交、未推送、未创建远端、未上传；CLI 登录只读核对，不代表发布完成 |

清单 files 不是完整可构建的分发包：pending 中仍有工程文件和必要测试输入。
不能将 files 单独打包后宣称可复现，也不能把 pending 一并提交绕过门禁。
发布前还需完成许可、隐私、依赖闭包和实际构建/运行验收。

初始模式扫描得到 334 条命中（267 私有文档引用，67 绝对路径模式），
不是 334 个秘密。另人工发现基线 hostName、旧发布器私人默认路径、
缺失资产许可细节及 V1 校验器字段差异。自动模式不覆盖图片、二进制和所有凭据格式，
无命中不代表最终隐私通过。六个缓存保留在本机，未删除用户文件。

## 下一批 B：目录、依赖与入口

1. 按 [依赖缺口](DEPENDENCY-GAPS.md) 处置 M7ExperimentViews 与旧工具默认路径；
   记录测试迁移依据，保留真实运行契约。
2. tools/tests 先列路径映射，更新 CMake/导入/互调；筛选公开 docs，修正过时路径。
3. 处理 pending 中的私人路径、机器标识、注释与素材；脱敏后记新 hash。
4. 修正资产登记和第三方 notice；项目 MIT 不覆盖第三方许可。
5. 写根 README、DEVELOPMENT 与静态架构；按新依赖闭包构建并运行相关测试。
6. 更新逐文件清单；每项 pending 以明确证据关闭，不自动批量放行。

后续 C/D 才实施最小 Demo、公开性能重算、最终 anchor Capture 和动态视频。
视频聚焦时间过程；定位、架构、范围与结论留静态材料。
本次文档只作交接，不自动授权下一批或外部发布。

## 实际检查入口

~~~powershell
python tools/portfolio/validate_import.py --stage import
python tools/portfolio/test_import.py
~~~

该 validator 只实现 import；不支持 entry/candidate/published，后续不得用 import PASS 替代。
详细初始清单、模式命中、源 dirty 状态、测试日志与清单 hash 回执留本地 out，
公共文档不携带私人绝对定位信息。
