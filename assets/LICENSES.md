# 资产来源与许可

2026-09-20 按本仓库实际文件复核。逐文件来源和原始 SHA-256 见 [第三方来源清单](../docs/evidence/THIRD-PARTY-SOURCES.json)。所有 15 个第三方载荷均与所列上游字节一致；重命名仅发生于 demo 的 Triangle.gltf → scene.gltf。

| 内容 | 作者 / 来源 | 许可 / 修改 |
|---|---|---|
| source/demo/scene.gltf、Triangle.bin；tests/fixtures/Triangle、TriangleWithoutIndices | Marco Hutter / Khronos glTF-Sample-Assets | [CC0-1.0](../docs/licenses/CC0-1.0.txt)；demo 文件改名，内容未改 |
| tests/fixtures/BoxTextured | Cesium / Khronos glTF-Sample-Assets | [CC-BY-4.0](../docs/licenses/CC-BY-4.0.txt)，保留 [Cesium 标识声明](../docs/licenses/Cesium-MARK-NOTICE.txt)；未修改，只作测试 |
| tests/fixtures/SimpleSparseAccessor | JCG / Khronos glTF-Sample-Assets | 上游正文称 CC0、Legal 段称 CC-BY-4.0；保守按 CC-BY-4.0 署名；未修改 |
| source/environments/qwantani_1k.hdr | Rob Tuytel / [Poly Haven Qwantani](https://polyhaven.com/a/qwantani) | CC0-1.0；1k 原始 HDR，未缩放 |
| source/tests 下 7 个场景/纹理文件 | xiaoran6657 / 本项目程序生成 | 项目 MIT；生成器 tools/assets/gen-m4-visual-baseline.py |
| tests/m7 下场景输入 | xiaoran6657 / 本项目程序生成 | 项目 MIT；生成器 tools/assets/gen_m7_scene_inputs.py |
| tests/fixtures 的自制正负例 | xiaoran6657 / 本项目 | 项目 MIT；说明见 fixtures README |

Khronos 文件复核固定在 c6a6bd13ab2b3c685c7903d03561b8a9392f38b8；每个模型的原始 URL 在来源清单中。BoxTextured 的 Cesium 图案不是本项目标识，不表示得到 Cesium 背书。

## Recipe

均为项目自有 MIT 文本。Recipe 的许可不覆盖其引用的第三方资产。

| 文件 | source / 用途 |
|---|---|
| demo.asset.json | demo/scene.gltf |
| m4-environment.asset.json | demo/scene.gltf |
| m4-visual-baseline.asset.json | tests/m4-visual-baseline.gltf |
| m6-graph-baseline.json | 运行/性能采集配置；不是 cooker 单资产 recipe |
| m7-performance-scenes.json | 运行/性能采集配置；不是 cooker 单资产 recipe |
| m9-portfolio-demo.json | 运行/性能采集配置；不是 cooker 单资产 recipe |

完整自有文件身份由 [公开文件清单](../docs/evidence/PUBLICATION-FILES.json) 绑定。新增资产须补来源、许可和变换记录；运行时读取烘焙产物，不直接读取这些源文件。
