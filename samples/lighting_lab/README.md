# 原生软阴影展示

`LightingShowcase` 是独立 Windows x64 / D3D12 compute 离屏程序。通过固定预设对照硬阴影、PCF、PCSS，并在 Cornell 风格开顶小室中比较直接光和一次 Lambert 漫反射反弹。

```powershell
cmake -S samples/lighting_lab -B out/build/shadow-showcase -G "Visual Studio 18 2026" -A x64
cmake --build out/build/shadow-showcase --config Release --target LightingShowcase
out/build/shadow-showcase/Release/LightingShowcase.exe --scene shadow-bench --shadow pcss --output out/shadow-new
```

需要 CMake 4.2+、Visual Studio 2026 C++ 和 Windows SDK/DXC。独立配置不下载第三方依赖。运行时只需 EXE 及同目录 `shaders/`，不依赖 GoogleTest、Python、SDK 或原开发目录。`--debug` 可启用系统已安装的 Graphics Tools。

参数用 `--help` 查看，`--scene` 为 `shadow-bench|cornell`、`--shadow` 为 `hard|pcf|pcss`、`--indirect` 为 `direct|bounce`。可调光源完整边长、比较 bias、采样预算、输出宽高。已有输出目录会被拒绝；错误参数或缺失 shader 返回非零。

实际输出包含 `color.png`、可见性/诊断图、原始 float32 数据、Debug Layer 日志和 `result.json`。身份记录中的本机路径只用于本地复现，不直接发布原始日志。

此路径使用程序生成 AABB 几何和 GPU 解析阴影图；直接光为中心点代理，面积只参与可见性。它不提供通用 mesh 光栅管线、常驻交互、多次反弹 GI 或跨后端等价。

对照图、解释和验收见 [软阴影实验](../../docs/showcase/shadow-lab/README.md)。
