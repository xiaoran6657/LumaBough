# `assets/source/tests/` — M4 固定视觉基线场景源资产

本目录保存 M4 最终验收（09 篇）使用的固定测试场景 `m4-visual-baseline` 的源资产。
它是 M4-02/06/07/08 各篇反复 deferred 的"5×5 材质因子球阵列 + 固定场景"内容的落地，
随 M4-09 一起进入仓库。

## 文件清单

| 文件 | 说明 |
|---|---|
| `m4-visual-baseline.gltf` | 场景描述（glTF 2.0 core，29 个材质、31 个节点、30 个网格） |
| `m4-visual-baseline.bin` | 外部二进制 buffer（31152 B，POSITION/NORMAL/TEXCOORD_0/索引） |
| `m4-pbr-basecolor.png` | 贴图方块 base color（sRGB，8×8 棋盘 + 对角亮带） |
| `m4-pbr-normal.png` | 贴图方块切线空间法线贴图（线性，平坦 +Z + 4×4 凸起） |
| `m4-pbr-metallic-roughness.png` | 贴图方块 metallic-roughness（线性，G=roughness 斜坡、B=metallic 斜坡） |
| `m4-pbr-occlusion.png` | 贴图方块 occlusion（线性，R 通道，边缘压暗） |
| `m4-pbr-emissive.png` | 贴图方块 emissive（sRGB，中央发光矩形） |

## 来源与许可证

**全部为自有程序化生成内容，无第三方素材**，按 MIT 列入 assets/LICENSES.md 的自有内容登记；生成脚本 `tools/assets/gen-m4-visual-baseline.py` 入库，可确定性重建（不含随机数、
时间戳或字典序依赖）。重跑后请用 `git diff` 人工核对差异，不要把它接进自动构建——
源资产应是"改一次、审一次"的内容。

## 场景内容（与 `docs/architecture/README.md` 固定测试套件对应）

* 5×5 材质因子球阵列：列 = metallic 0→1，行 = roughness 0.1→0.9（自下而上）。
  布局是**正对相机的墙面**而非铺地的列阵：固定相机在引擎坐标 (0,3,-10) 且俯仰为 0，
  地面多行球会因视线接近平行而在屏幕上叠成一条带，无法逐球判读梯度。
* 大平面（地面，24×20，阴影接收体）。
* 薄柱（0.3×3×0.3，shadow bias / peter-panning 观察体）。
* 斜面（4×0.3×3，绕 Z 旋转 18°，PCF 软边界观察体）。
* 镜像实例（同斜面网格、X 取负缩放 → 负 determinant，mirrored winding 锚点）。
* 带 base/normal/metallic-roughness/occlusion/emissive 五贴图的方块。

## 修改与复验

修改生成器后重新生成，并核对 glTF、buffer 与纹理的一致性。场景变化使既有截图与性能身份失效。
当前可执行的烘焙及双后端运行命令见 [开发指南](../../../docs/DEVELOPMENT.md)。
本场景 recipe 为 assets/recipes/m4-visual-baseline.asset.json；环境引用 Qwantani，许可见 [资产登记](../../LICENSES.md)。
