#!/usr/bin/env python3
"""生成 M7 三个固定场景共用的二进制输入。

产物：
  assets/tests/m7/fixed-camera.bin     固定相机路径（v1）
  assets/tests/m7/streaming-script.bin 固定流式请求脚本（v1）

为什么是二进制而不是 JSON：运行时不引入第二套 JSON 解析器；格式由
assets/tests/m7/README.md 冻结，长度与 magic 严格校验。生成脚本本身可复算：
同一条命令在任何机器上产生逐字节相同的文件（小端、float32）。

用法：
  python tools/assets/gen_m7_scene_inputs.py
"""

from __future__ import annotations

import hashlib
import math
import struct
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OUTPUT_DIR = REPO_ROOT / "assets" / "tests" / "m7"

CAMERA_MAGIC = b"MECAM7\x00\x00"
SCRIPT_MAGIC = b"MESTR7\x00\x00"
SCHEMA_VERSION = 1

# 相机与投影：左手系、row-vector、D3D 深度 0..1；与 M6 基线同一约定。
# 代理分布（M7SceneRunner 的圆锥体）以该视锥为参照，使可见率与深度近似无关。
CAMERA_EYE = (0.0, 3.0, -10.0)
CAMERA_FOV_Y = math.radians(60.0)
CAMERA_ASPECT = 1920.0 / 1080.0
CAMERA_NEAR = 0.1
CAMERA_FAR = 120.0

# 流式请求：帧号相对测量窗口（1 基），步长 24 帧；66 个请求覆盖 8..1568，
# 落在 1800 帧测量窗口内。体积分布：4.19 MB 环境 HDR → 5×350 KB 贴图 →
# 30×39 KB / 5×1.4 KB 网格 → 29×276 B 材质。
REQUEST_STRIDE = 24
FIRST_REQUEST_FRAME = 8

# M7-07 上传预算场景：同一批 66 个资产改按"突发"提交（每帧 16 个、间隔 2 帧），
# 使单帧上传需求达到 1.7—4.5 MiB：1/2/4 MiB 预算会真实成为约束，8 MiB 不会。
# 这是 E-M7-LOAD-002 的输入脚本；原始稀疏脚本仍是 m7-streaming 的串行基线输入。
BURST_REQUESTS_PER_FRAME = 16
BURST_FRAME_STRIDE = 2


def look_to_lh(eye, direction, up):
    length = math.sqrt(sum(component * component for component in direction))
    zaxis = tuple(component / length for component in direction)

    xaxis = (
        up[1] * zaxis[2] - up[2] * zaxis[1],
        up[2] * zaxis[0] - up[0] * zaxis[2],
        up[0] * zaxis[1] - up[1] * zaxis[0],
    )
    length = math.sqrt(sum(component * component for component in xaxis))
    xaxis = tuple(component / length for component in xaxis)

    yaxis = (
        zaxis[1] * xaxis[2] - zaxis[2] * xaxis[1],
        zaxis[2] * xaxis[0] - zaxis[0] * xaxis[2],
        zaxis[0] * xaxis[1] - zaxis[1] * xaxis[0],
    )
    return (
        xaxis[0], yaxis[0], zaxis[0], 0.0,
        xaxis[1], yaxis[1], zaxis[1], 0.0,
        xaxis[2], yaxis[2], zaxis[2], 0.0,
        -(xaxis[0] * eye[0] + xaxis[1] * eye[1] + xaxis[2] * eye[2]),
        -(yaxis[0] * eye[0] + yaxis[1] * eye[1] + yaxis[2] * eye[2]),
        -(zaxis[0] * eye[0] + zaxis[1] * eye[1] + zaxis[2] * eye[2]),
        1.0,
    )


def perspective_fov_lh(fov_y, aspect, near, far):
    height = 1.0 / math.tan(fov_y / 2.0)
    width = height / aspect
    return (
        width, 0.0, 0.0, 0.0,
        0.0, height, 0.0, 0.0,
        0.0, 0.0, far / (far - near), 1.0,
        0.0, 0.0, -(near * far) / (far - near), 0.0,
    )


def build_camera_path() -> bytes:
    view = look_to_lh(CAMERA_EYE, (0.0, 0.0, 1.0), (0.0, 1.0, 0.0))
    projection = perspective_fov_lh(CAMERA_FOV_Y, CAMERA_ASPECT, CAMERA_NEAR, CAMERA_FAR)
    payload = bytearray()
    payload += CAMERA_MAGIC
    payload += struct.pack("<II", SCHEMA_VERSION, 1)
    payload += struct.pack("<16f", *view)
    payload += struct.pack("<16f", *projection)
    payload += struct.pack("<4f", *CAMERA_EYE, 0.0)
    return bytes(payload)


# 资产 URI 与字节数来自 M4 冻结 manifest（out/m4-09/scene/manifest.json）。
# 它们是 cooker 对 assets/source/tests/m4-visual-baseline.gltf 的产物身份；
# 运行时会重新在 registry 中按 URI 解析并校验内容哈希，URI 漂移即显式失败。
PRIORITY_CRITICAL = 0
PRIORITY_VISIBLE = 1
PRIORITY_PREFETCH = 2

ASSETS = [
    (PRIORITY_CRITICAL, 4436, "asset://tests/m4-visual-baseline#world/default"),
    (PRIORITY_VISIBLE, 4194512, "asset://environments/m4-baseline"),
    (PRIORITY_VISIBLE, 349828, "asset://tests/m4-visual-baseline#image/0/baseColor"),
    (PRIORITY_VISIBLE, 349828, "asset://tests/m4-visual-baseline#image/1/normal"),
    (PRIORITY_VISIBLE, 349828, "asset://tests/m4-visual-baseline#image/2/metallicRoughness"),
    (PRIORITY_VISIBLE, 349828, "asset://tests/m4-visual-baseline#image/3/occlusion"),
    (PRIORITY_VISIBLE, 349828, "asset://tests/m4-visual-baseline#image/4/emissive"),
]
ASSETS += [
    (PRIORITY_PREFETCH, 1424 if index in (25, 26, 27, 28, 29) else 39344,
     f"asset://tests/m4-visual-baseline#mesh/{index}/primitive/0")
    for index in range(30)
]
ASSETS += [
    (PRIORITY_PREFETCH, 276, f"asset://tests/m4-visual-baseline#material/{index}")
    for index in range(29)
]


def build_stream_script() -> bytes:
    requests = []
    for index, (priority, size, uri) in enumerate(ASSETS):
        frame = FIRST_REQUEST_FRAME + index * REQUEST_STRIDE
        requests.append((frame, priority, size, uri))
    return encode_stream_script(requests)


def build_burst_stream_script() -> bytes:
    requests = []
    for index, (priority, size, uri) in enumerate(ASSETS):
        frame = FIRST_REQUEST_FRAME + (index // BURST_REQUESTS_PER_FRAME) * BURST_FRAME_STRIDE
        requests.append((frame, priority, size, uri))
    return encode_stream_script(requests)


def encode_stream_script(requests) -> bytes:
    payload = bytearray()
    payload += SCRIPT_MAGIC
    payload += struct.pack("<II", SCHEMA_VERSION, len(requests))
    payload += struct.pack("<Q", sum(request[2] for request in requests))
    for frame, priority, size, uri in requests:
        encoded = uri.encode("utf-8")
        payload += struct.pack("<IIQI", frame, priority, size, len(encoded))
        payload += encoded
    return bytes(payload)


def write(path: Path, payload: bytes) -> None:
    path.write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest().upper()
    print(f"{path.relative_to(REPO_ROOT)}  {len(payload)} bytes  sha256={digest}")


def main() -> int:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    write(OUTPUT_DIR / "fixed-camera.bin", build_camera_path())
    write(OUTPUT_DIR / "streaming-script.bin", build_stream_script())
    # M7-07：突发变体（同一批资产，帧号聚簇），供上传预算实验 E-M7-LOAD-002 使用。
    write(OUTPUT_DIR / "streaming-burst.bin", build_burst_stream_script())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
