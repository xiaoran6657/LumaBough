# 许可复核（批次 B）

自有部分按所有者决定采用 [MIT](../../LICENSE)，署名 xiaoran6657。
第三方内容逐项保留许可，汇总见 [完整声明](../../THIRD-PARTY-NOTICES.md)。

本次补齐 GoogleTest、fastgltf、simdjson、Tracy、Google Benchmark 和实际下载的 WinPixEventRuntime 许可原文；WinPIX 附包内第三方声明。配置确认 simdjson 3.12.3，修正旧 MIT 记录为 Apache-2.0；MikkTSpace 改记 Zlib，stb 固定上游提交。

15 个内嵌代码/资产文件均与固定上游或官方 HDRI 原文件逐字节一致，详见 [来源清单](THIRD-PARTY-SOURCES.json)。demo、所有 recipe、自有生成器场景、正负测试输入已列入 [资产登记](../../assets/LICENSES.md)。

SimpleSparseAccessor 上游正文 CC0 与 Legal 段 CC-BY-4.0 不一致，采用较严格的 CC-BY-4.0 署名；BoxTextured 保留 Cesium 标识声明，只用于测试。

本批闭合源码树与当前静态预览的许可记录。未来实际二进制包中的 DXC/VC Runtime/SDK DLL、Capture、字体、音乐和视频必须按携带文件复核；不能把当前源码许可检查当作未来包的分发许可。

## 运行包二进制再分发（E5 补充）

- **随包 VC 运行时**：`msvcp140.dll`、`msvcp140_atomic_wait.dll`、`vcruntime140.dll`、`vcruntime140_1.dll`，
  版本 **14.51.36247.0**，取自 Visual Studio Build Tools 的 VC Redist 目录（`Microsoft.VC145.CRT`），
  与 Redist 原件逐字节一致、未修改，按 app-local 方式随应用分发。
  适用条款：Microsoft 可再分发代码说明 https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution 。
- **WinPixEventRuntime.dll**：来自 PIX NuGet 包，许可与第三方声明随包提供
  （`licenses/WinPixEventRuntime-license.txt`、`licenses/WinPixEventRuntime-ThirdPartyNotices.txt`）。
- **包内许可说明按运行包重排**：源码树的相对链接在 `licenses/` 下会失效，打包时改写为纯文本路径；
  新增 `licenses/REDISTRIBUTABLES.md` 记录随包二进制的文件、版本与 SHA-256。
- **MIT 只覆盖自有部分**，不覆盖上述第三方二进制；包内 `licenses/LICENSE` 与第三方说明并列提供。
- **未随包**：DXC、VS/SDK、资产源树、PDB、Capture、视频、字体、音乐。
- 成片若将来公开，需按第 8 节单独授权；字体（`msyh.ttc`）只在本地录制时使用，不随包分发。
