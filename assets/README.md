# 资产目录

- source：离线原始 glTF、buffer、纹理与 HDRI。runtime 不直接读取。
- recipes：资产烘焙 recipe 与运行/性能配置，不能把全部 JSON 当 cooker 输入。
- tests/m7：可复现的合成性能场景输入。
- parity：历史双后端比较的参数契约，需配合相应分析器解释。

固定 PBR 场景的实际烘焙和运行命令见 [开发指南](../docs/DEVELOPMENT.md)。
来源、作者和逐项许可见 [LICENSES](LICENSES.md)。生成器位于 tools/assets。
生成内容发生变化后必须重做相应运行与身份绑定，不能沿用旧 screenshot/metadata。
构建产物与运行输出放 out；不把缓存当源资产提交。
