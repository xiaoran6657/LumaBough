# 第三方声明

项目自有代码采用 [MIT](LICENSE)。以下第三方内容保留各自许可，不被项目 MIT 覆盖。

| 内容 | 固定版本 / 来源 | 许可与随附文本 |
|---|---|---|
| GoogleTest | v1.17.0 | [BSD-3-Clause](docs/licenses/GoogleTest-LICENSE.txt) |
| fastgltf | v0.9.0 | [MIT](docs/licenses/fastgltf-LICENSE.txt) |
| simdjson | v3.12.3 | [Apache-2.0](docs/licenses/simdjson-LICENSE.txt) |
| Tracy（可选） | v0.13.1 | [BSD-3-Clause](docs/licenses/Tracy-LICENSE.txt) |
| Google Benchmark（可选） | v1.9.4 | [Apache-2.0](docs/licenses/GoogleBenchmark-LICENSE.txt) |
| WinPixEventRuntime | 1.0.240308001 | [MIT](docs/licenses/WinPixEventRuntime-LICENSE.txt) |

WinPIX 的 [第三方声明](docs/licenses/WinPixEventRuntime-ThirdPartyNotices.txt) 同时保留。

- mikktspace：Zlib；来源提交 3e895b49d05ea07e4c2133156cfa94369e19e409。许可文本内嵌于 [原文件](tools/asset_cooker/deps/mikktspace/mikktspace.c)，文件逐字节未改动。
- stb：MIT OR Unlicense；来源提交 2c980bb59875b0d32144a71867fbdebb2f77cd20。许可文本内嵌于 [原文件](tools/asset_cooker/deps/stb/stb_image.h)，文件逐字节未改动。

资产模型、HDRI 和署名见 [资产许可](assets/LICENSES.md)；精确来源 URL、获取复核日期及 SHA-256 见 [来源清单](docs/evidence/THIRD-PARTY-SOURCES.json)。

Python 开发依赖通过 requirements-dev.txt 安装，未作为源码或 wheel 随仓库再分发。
SDK、DXC、VC Runtime、glTF Validator 与 GUI 工具不包含在源码树；未来二进制包须另核对实际携带的文件与许可。
