# 工具目录

| 目录 | 职责 |
|---|---|
| asset_cooker / shader_compiler | C++ 离线资产与 shader 工具，CMake target 名称保留 |
| benchmark / tasks_bench | C++ 统计协议与可选微基准 |
| assets | 确定性测试输入生成；重生成前确认是否改变冻结输入字节 |
| performance | M7 采集、比较、汇总、机器环境与 Tracy sink |
| validation | RHI 边界、图/场景检查、smoke |
| capture | PIX/RenderDoc/诊断日志辅助；需要外部工具与单独产物审查 |
| evidence | 预注册与本地证据归档；目标路径必须显式指定，不是 GitHub 发布器 |
| portfolio | 文件清单与阶段检查；当前支持 import / entry |
| dev | 开发 shell、格式化与架构图导出 |
| legacy | 历史复算及被回归测试覆盖的兼容逻辑，需其协议输入；不是当前验收入口 |

工具契约测试集中在 [tests/tools/contracts](../tests/tools/contracts)；
已有 Python unittest 仍在 tests/tools。迁移映射见 [PATH-MAP](../docs/evidence/PATH-MAP.json)。

~~~powershell
python tools/portfolio/validate_publication.py --stage entry
python tests/tools/contracts/test_import.py
python -B -m unittest discover -s tests/tools -p "test_*.py"
python tools/validation/check_rhi_boundary.py
~~~

导入扫描器 audit_import.py 只在有私有来源访问权限时用于本地溯源；
日常公共检查不需要源库。所有输出写 out，不能把原始机器信息/录屏/调试符号直接进公开清单。
V1 portfolio 工具和仅服务私有旧目标的编排已退出公开源码。

## 作品集 C 工具

run_demo.py 和 recompute_performance.py 位于 portfolio；操作与限制见 [Demo](../docs/portfolio/DEMO.md) 和 [性能报告](../docs/portfolio/PERFORMANCE.md)。performance/history 的固定比较器仅用于历史规则复核。

## 当前源码新性能工具

performance/run_portfolio_experiment.py 负责 pilot/register/run，summarize_portfolio_experiment.py 校验完整25次集合后复算。需要预先冻结源码、构建和环境；当前两组均 INVALID，不能重用原目录补跑。方法、输入约束和接续边界见[新实验记录](../docs/evidence/NEW-PERFORMANCE.md)。
