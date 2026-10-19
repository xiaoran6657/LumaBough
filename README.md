# LumaBough · 光枝

围绕软阴影与光照的 C++20 / D3D12 学习实验：在固定场景中比较硬阴影、PCF、PCSS，查看光源尺寸、bias 和采样预算如何改变结果，并用 Cornell 风格小室观察一次漫反射反弹。

![PCSS 阴影测试台的真实 GPU 输出](docs/showcase/shadow-lab/media/shadow-pcss.png)

## 从这里开始

- [对照图、参数和算法解释](docs/showcase/shadow-lab/README.md)
- [原生演示的构建与运行](samples/lighting_lab/README.md)
- [本批独立审查、复验与支持范围](docs/evidence/SHADOW-LAB-REVIEW.md)
- [v0.2.0-shadow-lab 运行包](https://github.com/xiaoran6657/LumaBough/releases/tag/v0.2.0-shadow-lab)

首版是原生 D3D12 compute 离屏程序，以预设和命令行参数生成实际 GPU 图片、线性数据及诊断。几何由程序生成；阴影图以 GPU 光线/AABB 相交产生。直接光采用中心点代理，光源面积用于可见性近似。Cornell 风格小室使用一次 Lambert 反弹；不声明通用 mesh 光栅管线、常驻交互或完整 GI。

## 独立构建

Windows x64、Visual Studio 2026 C++、Windows SDK/DXC、CMake 4.2+。此目标可单独配置，不下载整个工程的第三方依赖：

```powershell
cmake -S samples/lighting_lab -B out/build/shadow-showcase -G "Visual Studio 18 2026" -A x64
cmake --build out/build/shadow-showcase --config Release --target LightingShowcase
out/build/shadow-showcase/Release/LightingShowcase.exe --scene shadow-bench --shadow pcss --light-size 0.65 --bias 0.004 --samples 64 --width 960 --height 600 --output out/shadow-example
```

EXE 通过自身位置加载随附 shader。正常运行不需要 Python、DXC、SDK 或测试框架；每次使用新的输出目录。下载运行包后按其中 `RUN.md` 启动，并核对 `SHA256SUMS.txt`。`--debug` 仅用于已安装 Graphics Tools 的验证。

## 渲染工程背景

项目的基础工程保留 D3D11 / D3D12、RenderPacket、Render Graph、资源生命周期、确定性 packet 构建和 revision 资产加载。内部 namespace、target 和程序名保留 MiniEngine；本公开工程由开发工程选择性迁移，没有导入私人 Git 历史。

- [架构与三张图](docs/architecture/README.md)
- [完整工程开发指南](docs/DEVELOPMENT.md)
- [原双后端演示](docs/portfolio/DEMO.md)
- [历史性能结果与边界](docs/portfolio/PERFORMANCE.md)
- [旧预览运行包 v0.1.0-preview](https://github.com/xiaoran6657/LumaBough/releases/tag/v0.1.0-preview)

旧预览包的双后端场景、旧性能实验和本次软阴影展示分别保留身份与支持域。历史性能为 1 INCONCLUSIVE + 3 REJECTED，无当前加速声明；本次单次 GPU dispatch 时间不替代完整帧率或性能 benchmark。Vulkan 未进入此公开工程，设备丢失不承诺自动恢复，glTF 导入仍为受限子集。

## 来源、协作与许可

Agent 辅助完成实现、实验核验和文档整理。工程交付与所有者个人学习进度分别记录，不将协作产出表述为纯手工独立实现。新增场景及图像由本项目程序生成，没有导入 GAMES202 作业代码、课程模型或 Cornell 实测资产；论文与算法来源见[实验说明](docs/showcase/shadow-lab/README.md#参考)。

自有部分采用 [MIT](LICENSE)。既有第三方代码和资产保留各自[许可与署名](THIRD-PARTY-NOTICES.md)。[文档目录](docs/README.md) · [路线图](docs/ROADMAP.md) · [变更记录](docs/CHANGELOG.md)
