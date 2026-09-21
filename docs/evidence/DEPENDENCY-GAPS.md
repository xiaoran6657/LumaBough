# 依赖处理（批次 B）

| 编号 | 处置 |
|---|---|
| DEP-01 | 私有 M7ExperimentViews 归档一致性检查留源库；公共 CTest 新增 PortfolioEntry 与 PortfolioImportContracts。运行契约测试保留 |
| DEP-02 | 旧 V1 portfolio 脚本移至本地 out；V2 import 和 entry 校验职责独立，后续阶段未实现 |
| DEP-03 | 重写公共导航与相关源码注释；历史原件 ID 仅作来源标识，不是失效下载链接 |
| DEP-04 | 目录映射、Python import、CTest 和脚本互调同步；Python 42 项工具测试通过 |
| DEP-05 | 原始基线留 out；测试改用脱敏 schema fixture，不宣称为实测机器数据 |
| DEP-06 | 两种发布脚本必须显式 destination-root；保留预注册门契约测试 |
| DEP-07 | DEV shell 自动发现 VS；DXC 按所选 SDK 注册表查找；双配置共用构建树；固定 Pillow/NumPy 到隔离环境 |
| DEP-08 | 源码与静态预览来源/许可记录完成；二进制包的实际 DLL 再分发清单留打包批次 |

退休样例驱动器不适用于独立仓库，移到本地 out/m9/batch-b/private-only；需要原始输入的纯历史分析器保留 tools/legacy，并明确其适用范围。
