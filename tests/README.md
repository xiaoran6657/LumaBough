# 测试组织

模块 C++ 测试保持在 core、assets、world、tasks、render_graph、rhi 等目录。
fixtures 是测试输入；原 tests/baselines 中的历史结果已归档到本地，
不是可直接继承的当前验证。performance/environment.schema-example.json
保留在 fixtures 下，只检查字段契约，不作为新运行机器身份。

| 分类 | 入口与说明 |
|---|---|
| CPU | math、handle、schema、graph、assets、tasks；由各模块 CMake 与 GoogleTest 注册 |
| GPU | rhi 与渲染设备测试，依赖可用硬件或用例明确选择的 WARP/debug layer |
| 工具 | tools 下 Python unittest；tools/contracts 集中原工具自测，通过原 CTest 接线运行 |
| 文档 | PortfolioEntry 检查当前公共入口/链接与声明；不要求未来视频、包或远端 URL |
| 显式硬件破坏性场景 | DRED 真实 TDR 不是默认回归已执行项；不因默认 skip 宣称通过 |

本批唯一移出公开默认套件的旧检查是 M7ExperimentViews：
它验证私有历史账本的生成视图，留在原私有库；不属于运行时回归。
M6 全量私有编排器不公开，两个 parity 纯汇总函数留
[tools/legacy/m6_summary.py](../tools/legacy/m6_summary.py)，对应回归完整保留。

运行命令和当前真实数量见 [开发指南](../docs/DEVELOPMENT.md)和
[批次 B](../docs/evidence/BATCH-B.md)。不能把历史测试数量或发现列表当作实际运行通过数。
