# M3 glTF Fixtures

本目录为 M3 Cooker 导入测试提供 glTF 2.0 fixture。来源分两类：官方样例与项目自制。源项目记录称每个 fixture 均经过 Khronos `gltf_validator`（v2.0.0-dev.3.10，Windows x64）验证；合法 fixture 预期 `Errors=0, Warnings=0`，负向 fixture 预期至少 1 个 error。

## 目录

| Fixture | 来源 | 目的 | Validator 预期 |
|---|---|---|---|
| `Triangle/` | Khronos glTF-Sample-Assets | indexed POSITION、无 texture | 0 errors |
| `TriangleWithoutIndices/` | Khronos glTF-Sample-Assets | sequential index generation | 0 errors |
| `BoxTextured/` | Khronos glTF-Sample-Assets | mesh + UV + PNG | 0 errors |
| `SimpleSparseAccessor/` | Khronos glTF-Sample-Assets | sparse accessor 展开 | 0 errors |
| `SimpleHierarchy/` | 项目自制 | TRS 与 matrix 混合层级、多 root | 0 errors |
| `AxisBasis/` | 项目自制 | 坐标轴 golden：验证 glTF 手性转 MiniEngine 手性 | 0 errors |
| `MirroredNode/` | 项目自制 | negative determinant / front-face 翻转 | 0 errors |
| `InvalidTraversal/` | 项目自制（负向） | URI sandbox 逃逸拒绝 | ≥1 error |
| `InvalidIndex/` | 项目自制（负向） | index 越界拒绝 | ≥1 error |

## 官方样例许可

2026-09-20 对四组官方模型逐文件复核：Triangle 与 TriangleWithoutIndices 为 Marco Hutter 的 CC0；
BoxTextured 为 Cesium 的 CC-BY-4.0 并附标识声明；SimpleSparseAccessor 为 JCG，按较严格的 CC-BY-4.0 署名。
固定提交、原始 URL、字节比对与完整文本见 [资产许可](../../assets/LICENSES.md) 和 [来源清单](../../docs/evidence/THIRD-PARTY-SOURCES.json)。

## 自制 fixture 说明

自制 fixture 使用 `generator: "MiniEngine M3 fixture (manual)"` 标记，顶点数据见各 `.bin`。
布局统一为：indices（`uint16`，offset 0）→ padding 到 4 字节 → positions（`float32 xyz`）。
共享一个 `Triangle.bin` 布局：3 顶点三角形 `(0,0,0),(1,0,0),(0,1,0)`，44 字节。

### SimpleHierarchy.gltf

- 3 个 node：`RootTRS`（TRS，mesh 0）、`RootMatrix`（matrix 恒等，child 为 `ChildTRS`）、`ChildTRS`（TRS，mesh 0）。
- 同时覆盖 root TRS、root matrix、child hierarchy 三条路径。
- `SimpleHierarchy.bin`：44 字节（与 Triangle 相同布局）。

### AxisBasis.gltf

- 3 个独立 mesh（几何相同：单位三角形），node `AxisX`/`AxisY`/`AxisZ` 分别位于 glTF +X/+Y/+Z。
- golden 预期（转换到 MiniEngine 左手系后）：glTF `-X right` 对应 MiniEngine `+X right`，glTF `+Y up` 对应 MiniEngine `+Y up`，glTF `+Z forward` 对应 MiniEngine `+Z forward`。由 Cooker 的 golden test 断言顶点/节点位置，而非仅"看起来正常"。
- `AxisBasis.bin`：132 字节（3 段独立 `indices+pad+positions` 交错布局）。

### MirroredNode.gltf

- node `scale=[-1,1,1]`，行列式为负。
- 预期 Cooker 保留负行列式并在 RenderItem 标记 `mirrored`，D3D11 选择相反 front-face；禁止全局关闭 culling。
- `MirroredNode.bin`：44 字节。

### InvalidTraversal.gltf（负向）

- buffer URI 为 `../../../../secret.bin`，相对 source root 规范化后逃出（多层 `..`）。
- 预期 Cooker 在打开文件前按 URI sandbox 规则拒绝（先检查路径再打开）。
- validator 报 1 个 error（IO_ERROR）。**故意不提供该 .bin 文件。**

### InvalidIndex.gltf（负向）

- 3 个顶点，但 indices accessor 声明 `max=[5]`，索引 `5 >= vertexCount(3)`。
- 预期 Cooker 在 index `< vertexCount` 校验时失败。
- validator 报 1 个 error（ACCESSOR_INDEX_OOB）。
- `InvalidIndex.bin`：44 字节（真实二进制，仅索引值越界）。

## 复验命令

```powershell
gltf_validator.exe -o -r tests/fixtures/Triangle/Triangle.gltf
# 其余文件同理；两个负向 fixture 预期 Errors >= 1
```

## 注意

- `ABeautifulGame.glb`（Khronos 全特性压力样例，42 MB）曾用于人工查看；审计后已移除（无任何代码/测试引用，体积不适合入库），如确需可从 Khronos glTF-Sample-Assets 重新获取。
- 新增 fixture 时同步更新本 README。
