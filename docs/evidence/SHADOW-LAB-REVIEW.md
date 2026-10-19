# 软阴影展示验收

本文件在独立实现审查和公开仓复验完成后填写真实结果。它采用独立 `LightingShowcase` 离屏合同，不沿用旧 Sandbox 候选、双后端六运行或历史性能 PASS。

支持域：Windows x64 / D3D12 硬件 / 固定程序生成 AABB 几何；hard、PCF、PCSS；Cornell 风格一次 Lambert 反弹。正常运行不依赖 Python、DXC、SDK 或测试框架；调试层验收与普通运行分别执行。

精选图及参数见[软阴影实验](../showcase/shadow-lab/README.md)，字节身份见[媒体清单](../showcase/shadow-lab/manifest.json)。
