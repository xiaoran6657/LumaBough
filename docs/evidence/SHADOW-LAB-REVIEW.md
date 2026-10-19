# 软阴影展示验收

2026-10-19，本批原生 D3D12 离屏展示、公开仓构建和新解压包复验 **PASS**。采用独立 `LightingShowcase` 合同；旧 Sandbox 候选、历史双后端与性能结果仍保留各自身份。

## 支持范围

Windows x64 / D3D12 硬件 / 固定程序生成 AABB 几何；hard、固定核 PCF、PCSS，以及 Cornell 风格一次 Lambert 反弹。无窗口与常驻交互，未验收通用 mesh 光栅、运动稳定性、完整 GI 或实时帧率。

直接辐照度为中心点代理，光源边长只影响 visibility；反弹为 256 个固定余弦方向及第二段解析硬可见性。场景和图片均为自有程序生成，没有课程作业代码、Cornell 标定模型或实测纹理的再分发。

## 独立审查

实现与审查分开进行。审查重新构建，并独立复算硬阴影 506 点（零错误）、7×80 个 PCF/PCSS 点（可见性差为 0，平均 blocker 深度最大差约 3.83e-6）、36 个一次反弹点（最大误差约 2.18e-7）。没有遗留 P1/P2 阻断。

审查发现工具 CPU 相机的左右轴镜像错误，已修复叉积方向并补非对称锚点测试；没有放宽容差。原失败批次保留。另以隔离构建证实 shader 单独变化会同步更新运行目录 DXIL，未把疑点当成已确认缺陷。

公开迁移仅将 SDK 发现改为 CMake 自带注册表 API，另经审阅及无旧缓存的全新配置/构建；C++、HLSL 与核验工具字节不变。

## 本公开仓实际复验

受测源码提交：`de449ce2b3b7b9edbb836c905ad8a773d71b038b`。最终发布提交只补充说明和清单，不改动受测源码。Visual Studio 2026 / MSVC v145 / CMake 4.2+ / Windows SDK；全新 standalone 树分别构建 Debug、Release。实测适配器：AMD Radeon RX 9070。

| 验证 | 实际结果 |
|---|---|
| 工具契约测试 | 29/29 通过 |
| RHI 边界与逐文件公开集合 | 通过；不继承旧候选包门 |
| Release 精选组 | 10 个 960×600 GPU 预设，Debug Layer 零 warning/error |
| Debug 程序 | 320×200 独立运行，Debug Layer 干净 |
| CPU 硬阴影参考 | 稳定点 4763 个，材质/可见性错误均为 0 |
| 显示变换 | HDR 重建 PNG，各预设误差不超过 1 个 8bit 灰阶 |
| 受控差异 | 方法、光源尺寸、bias、采样预算的可见性均有可测变化 |
| 重复预设 | 不同进程相同参数，HDR/diagnostics 原始 float 完全一致；不代表常驻状态恢复 |
| 五个采集负例 | 非法选项/光源尺寸/采样数/非有限 bias 明确失败；已有输出目录字节未变 |
| 新解压包 | 中文、空格路径及不同工作目录，普通运行与 debug 一次反弹通过 |
| 包失败路径 | 缺 DXIL、缺 HLSL 明确失败且无 complete 回执；已有文件拒绝覆盖 |
| 隐私 | source-set + Git、实际解压目录、ZIP 分别扫描，均无未裁定/待批准项 |
| 许可 | 自有 MIT、程序生成几何；包保留 LICENSE，无第三方运行库或机器产物 |

首轮扫描把公开作者 URL 和标准 Windows 注册表键识别为机器名/UNC。下载链接集中到已有产物索引，SDK 查询使用 CMake 原生 API 后复扫通过；扫描器和处置批准基线均未修改。

## 运行包身份

下载入口见[产物索引](ARTIFACT-INDEX.md#软阴影展示)。`lumabough-shadow-lab-windows-x64.zip`：209276 字节，SHA-256：

`d9ae930d879770449a32a67758a4d0f753476bef7dee9f09843a13768180c6f3`

包含 8 个文件：EXE、DXIL、两份自有 shader 源、LICENSE、RUN.md、PACKAGE-MANIFEST.json、SHA256SUMS.txt。附件另附 `SHA256SUMS-external.txt`。正常运行不依赖 Python、DXC、SDK、测试框架或 Graphics Tools；`--debug` 才要求调试层。MSVC CRT 静态链接，EXE 导入仅 d3d12 / dxgi / ole32 / KERNEL32 系统 DLL。

| 运行输入 | SHA-256 |
|---|---|
| executable | `9e474dcef188fd0a9bff9aca0c9322a02c525972afb2f14f97aa693a798be98d` |
| shaderSource | `509d3d3fca424bab321a088d3a466e16fc5138ca12cd7232a41aef1ff78c0c51` |
| shaderBinary | `09851de22a3549cd5721808ff530d42f8b6bb01dc8128e22295afaa66f235c58` |
| shaderInclude | `7cc838b8d5783ba774fef7eed2375808d0681d22b03e1c3e8221daab2127033d` |

包内 manifest 绑定构建源码提交和附件 SHA，ZIP 与解压文件逐项相符。运行结果含本机身份路径，留用户本地；未把原始机器日志混入公开图和源码。

## 图像与结论边界

[实验页](../showcase/shadow-lab/README.md)和[媒体清单](../showcase/shadow-lab/manifest.json)提供 10 张未经修饰的原始 GPU PNG。公开仓重跑相同预设后核对这些图的字节；网站再从固定公开 tag 导入，不另生成“效果图”。

GPU 时间仅为一次冷 dispatch 的阴影图生成与着色，不含初始化、读回和编码。本批不新增通用性能加速或实时帧率声明；旧 1 INCONCLUSIVE + 3 REJECTED 保持原结论。Agent 辅助实现、验证与文档；本验收不表示所有者已完成个人课程学习。
