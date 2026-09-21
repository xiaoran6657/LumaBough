# M7 固定场景输入

这里保存 M7 三个性能场景共用的**冻结二进制输入**。运行时不解析 JSON：格式由本文件
冻结，读取端（`samples/rhi_sandbox/M7SceneRunner.cpp`）做 magic、版本与**精确长度**校验，
任何漂移都显式失败，不会静默退化。生成器：`tools/assets/gen_m7_scene_inputs.py`（小端、float32，
逐字节可复算）。

| 文件 | 字节数 | SHA-256 |
|---|---:|---|
| `fixed-camera.bin` | 160 | `160466991F23B7021343824AC3DE781781FA65E1DE92D9C9F1D49F151337D43C` |
| `streaming-script.bin` | 4492 | `C60412A996DF43E66D4B1EA21CAB6235D0AB7B93A374C8003E37D6F8AD73A488` |

修改任一文件都必须重新生成 `assets/recipes/m7-performance-scenes.json` 中的哈希，
并说明原因（相机、分布或请求序列的变化都会让旧的性能证据不可比）。

## `fixed-camera.bin`（v1）

```text
offset  size  field
0       8     magic "MECAM7\0\0"
8       4     uint32 schemaVersion = 1
12      4     uint32 keyCount
16      144×N 每帧一个 key：
              float32 view[16]        row-major、row-vector、左手系
              float32 projection[16]  同上；D3D 深度 0..1
              float32 worldPosition[3]
              float32 reserved (0)
```

当前只有一个 key（固定相机：eye `(0, 3, -10)` 朝 `+Z`，fovY 60°，1920×1080，
near 0.1 / far 120）。`keyCount > 1` 时按测量帧号循环播放，`keyCount == 1` 为常量相机。

## `streaming-script.bin`（v1）

```text
offset  size  field
0       8     magic "MESTR7\0\0"
8       4     uint32 schemaVersion = 1
12      4     uint32 requestCount
16      8     uint64 totalExpectedBytes（信息字段，运行时逐请求核对）
24      变长  每条请求：
              uint32 frame         相对测量窗口的 1 基帧号（1 = 第一个测量帧）
              uint32 priority      0=Critical 1=Visible 2=Prefetch
              uint64 expectedBytes 期望产物字节数
              uint32 uriLength
              char   uri[uriLength] UTF-8 canonical URI
```

当前 66 条请求，步长 24 帧（帧 8..1568，落在 1800 帧测量窗口内）：

| 优先级 | 内容 | 体积分布 |
|---|---|---|
| Critical | world 产物 ×1 | 4.4 KB |
| Visible | 环境 HDR + 5 张 PBR 贴图 | 4.19 MB / 5×350 KB |
| Prefetch | 30 个网格 + 29 个材质 | 25×39 KB、5×1.4 KB、29×276 B |

URI 必须能在 M4 冻结 manifest（`out/m4-09/scene/manifest.json`）的 registry 中解析；
运行时读取对应产物字节并**重算 SHA-256 与 manifest 记录比对**，同时核对 `expectedBytes`，
不一致即记为 `failed:*` 并让整次 run 的 `correctness.status = FAIL`。
