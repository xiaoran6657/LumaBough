#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen-m4-visual-baseline.py — 生成 M4 固定视觉基线场景的 glTF 源资产。

用途
----
一次性生成 ``assets/source/tests/m4-visual-baseline.gltf`` 及其外部 ``.bin`` 与
五张 PBR 贴图（base color / normal / metallic-roughness / occlusion / emissive）。
场景内容对应 ``docs/architecture/README.md`` 的 ``m4-visual-baseline`` 固定测试套件：

* 5×5 材质因子球阵列（列 = metallic 0→1，行 = roughness 0.1→0.9）；
* 大平面（地面，阴影接收体）；
* 薄柱（bias / peter-panning 观察体）；
* 斜面（PCF 边界与梯度观察体）；
* 镜像实例（负 determinant 节点，验证 mirrored winding）；
* 一个带 base/normal/metallic-roughness/occlusion/emissive 五贴图的方块。

设计约束（改动前必读）
----------------------
1. **纯 Python 标准库**：不引入 numpy/PIL，PNG 用 ``zlib`` + ``struct`` 直接写，
   保证任何装有 Python 3.8+ 的 Windows 都能重跑（本工具不进 CMake 构建）。
2. **确定性**：不使用随机数、时间戳或字典的隐式遍历顺序；相同输入必产生逐字节
   相同的输出，否则 Cooker 的 BuildKey 每次都失效、截图基线无法复现。
3. **坐标与绕序**：glTF 为右手系 Y-up，Cooker 以 ``C = diag(-1,1,1,1)`` 转换到引擎
   左手系，故 glTF 的 +X 在引擎里是 -X，Y/Z 不变。引擎按 CCW 为正面
   （``FrontCounterClockwise=TRUE``），所以本文件生成的所有三角形必须是"从外侧看
   逆时针"。
4. **不写 KHR 扩展**：Cooker 对 ``emissiveStrength`` / ``alphaMode`` / ``doubleSided``
   是 hard fail，本场景刻意全部使用 glTF 2.0 core metallic-roughness。
5. **不使用顶点颜色 / morph / skin**，Cooker 目前不支持。

用法
----
在仓库根目录执行（路径全部相对仓库根，便于直接复制命令）：

    python tools\\assets\\gen-m4-visual-baseline.py

输出文件已入库，属于人工维护的源资产；重跑后请用 ``git diff`` 人工核对差异，
不要把它接进自动构建——源资产应该是"改一次、审一次"的内容，不是构建产物。
"""

from __future__ import annotations

import json
import math
import os
import struct
import zlib
from typing import Any, Dict, List, Sequence, Tuple

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT_DIR = os.path.join(REPO_ROOT, "assets", "source", "tests")

GLTF_NAME = "m4-visual-baseline.gltf"
BIN_NAME = "m4-visual-baseline.bin"

# glTF 常量（写出字面量而不是引入依赖，便于阅读与离线核对）。
COMPONENT_TYPE_UNSIGNED_INT = 5125
TARGET_ARRAY_BUFFER = 34962
TARGET_ELEMENT_ARRAY_BUFFER = 34963

# 采样器：LINEAR / LINEAR_MIPMAP_LINEAR / REPEAT——与 M4 角色化 mip 生成配套。
SAMPLER_LINEAR_MIPMAP_LINEAR_REPEAT = {
    "magFilter": 9729,
    "minFilter": 9987,
    "wrapS": 10497,
    "wrapT": 10497,
}

IMAGE_SIZE = 256


# ============================================================================
# PNG 写出（RGBA8，color type 6）
# ============================================================================
def write_png_rgba(path: str, size: int, pixels: bytearray) -> None:
    """写出一张 size×size 的 RGBA8 PNG。

    只依赖标准库：IHDR / IDAT / IEND 三个块，IDAT 用 zlib 压缩的逐行
    filter=0（None）扫描线。不用 filter=1/2/3/4 是因为编码结果是字节确定的
    且解码端一定支持；PNG 规范只要求解码器支持全部 filter 类型，编码端可任选。
    """
    stride = size * 4
    if len(pixels) != stride * size:
        raise ValueError("pixel buffer size mismatch")

    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type 0 = None
        raw += pixels[y * stride : (y + 1) * stride]

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", header)
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as f:
        f.write(png)


def build_textures(out_dir: str) -> Dict[str, str]:
    """生成五张 PBR 贴图，返回 glTF slot 名 → 文件名的映射。

    五张图都是程序化的、可解释的：验收时只要看一眼就能判断"图有没有生效"，
    比噪声图更容易定位采样错误（尤其是 normal map 的 G 通道朝向）。
    """
    size = IMAGE_SIZE
    stride = size * 4

    def blank() -> bytearray:
        return bytearray(stride * size)

    def put(buf: bytearray, x: int, y: int, r: int, g: int, b: int, a: int = 255) -> None:
        o = (y * size + x) * 4
        buf[o + 0] = r
        buf[o + 1] = g
        buf[o + 2] = b
        buf[o + 3] = a

    # ---- base color（sRGB）：8×8 深蓝/浅黄棋盘 + 一条对角亮带 ----
    base = blank()
    cell = size // 8
    for y in range(size):
        for x in range(size):
            checker = ((x // cell) + (y // cell)) % 2 == 0
            r, g, b = (232, 196, 96) if checker else (36, 52, 112)
            # 对角亮带：提供一条可辨方向的高频特征，便于判断 UV 朝向。
            if abs((x + y) - size) < 8:
                r, g, b = 246, 246, 236
            put(base, x, y, r, g, b)

    # ---- normal map（线性）：整体平坦 +Z，叠加 4×4 个球面凸起 ----
    normal = blank()
    bumps = 4
    radius = size / (bumps * 2.0)
    for y in range(size):
        for x in range(size):
            cx = (math.floor(x / (size / bumps)) + 0.5) * (size / bumps)
            cy = (math.floor(y / (size / bumps)) + 0.5) * (size / bumps)
            dx = (x - cx) / radius
            dy = (y - cy) / radius
            d2 = dx * dx + dy * dy
            if d2 >= 1.0:
                nx = ny = 0.0
                nz = 1.0
            else:
                nz = math.sqrt(1.0 - d2)
                nx, ny = dx, -dy  # glTF 法线贴图约定：+Y 指向"上"（OpenGL 风格）
            # 切线空间 [-1,1] → 纹理 [0,1]，8-bit 量化。
            put(normal, x, y, int(round((nx * 0.5 + 0.5) * 255)), int(round((ny * 0.5 + 0.5) * 255)), int(round((nz * 0.5 + 0.5) * 255)))

    # ---- metallic-roughness（线性）：G = 横向 roughness 斜坡，B = 纵向 metallic 斜坡 ----
    mr = blank()
    for y in range(size):
        for x in range(size):
            roughness = 0.10 + 0.80 * (x / (size - 1.0))  # 左粗糙低 → 右高
            metallic = y / (size - 1.0)  # 上金属低 → 下高
            put(mr, x, y, 255, int(round(roughness * 255)), int(round(metallic * 255)))

    # ---- occlusion（线性）：R 为 AO，中心全亮、四边压暗 ----
    ao = blank()
    for y in range(size):
        for x in range(size):
            fx = min(x, size - 1 - x) / (size * 0.5)
            fy = min(y, size - 1 - y) / (size * 0.5)
            edge = min(1.0, min(fx, fy) * 2.0)
            value = int(round(255.0 * (0.35 + 0.65 * edge)))
            put(ao, x, y, value, value, value)

    # ---- emissive（sRGB）：全黑 + 中央一块发光矩形块 ----
    emissive = blank()
    for y in range(size):
        for x in range(size):
            inside = (size * 0.35) < x < (size * 0.65) and (size * 0.35) < y < (size * 0.65)
            if inside:
                put(emissive, x, y, 255, 138, 32)
            else:
                put(emissive, x, y, 0, 0, 0)

    files = {
        "baseColorTexture": "m4-pbr-basecolor.png",
        "normalTexture": "m4-pbr-normal.png",
        "metallicRoughnessTexture": "m4-pbr-metallic-roughness.png",
        "occlusionTexture": "m4-pbr-occlusion.png",
        "emissiveTexture": "m4-pbr-emissive.png",
    }
    buffers = {
        "baseColorTexture": base,
        "normalTexture": normal,
        "metallicRoughnessTexture": mr,
        "occlusionTexture": ao,
        "emissiveTexture": emissive,
    }
    for slot, name in files.items():
        write_png_rgba(os.path.join(out_dir, name), size, buffers[slot])
    return files


# ============================================================================
# 几何生成
# ============================================================================
Vec3 = Tuple[float, float, float]


def cross(a: Vec3, b: Vec3) -> Vec3:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(a: Vec3) -> Vec3:
    length = math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
    if length == 0.0:
        return (0.0, 0.0, 0.0)
    return (a[0] / length, a[1] / length, a[2] / length)


def build_box() -> Tuple[List[Vec3], List[Vec3], List[Tuple[float, float]], List[int]]:
    """生成 1×1×1 的单位立方体（中心在原点）。

    每个面按 (u, w) 基底展开，w = cross(n, u)：这样四个角在"从外侧看"的视角下
    就是逆时针的，与引擎 ``FrontCounterClockwise=TRUE`` 的约定一致。
    """
    positions: List[Vec3] = []
    normals: List[Vec3] = []
    uvs: List[Tuple[float, float]] = []
    indices: List[int] = []

    faces: List[Tuple[Vec3, Vec3]] = [
        ((1.0, 0.0, 0.0), (0.0, 0.0, -1.0)),   # +X
        ((-1.0, 0.0, 0.0), (0.0, 0.0, 1.0)),   # -X
        ((0.0, 1.0, 0.0), (1.0, 0.0, 0.0)),    # +Y
        ((0.0, -1.0, 0.0), (1.0, 0.0, 0.0)),   # -Y
        ((0.0, 0.0, 1.0), (1.0, 0.0, 0.0)),    # +Z
        ((0.0, 0.0, -1.0), (-1.0, 0.0, 0.0)),  # -Z
    ]

    for n, u in faces:
        nn = normalize(n)
        uu = normalize(u)
        ww = normalize(cross(nn, uu))
        center = (nn[0] * 0.5, nn[1] * 0.5, nn[2] * 0.5)
        corners = [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)]
        start = len(positions)
        for su, sw in corners:
            positions.append(
                (
                    center[0] + (uu[0] * su + ww[0] * sw) * 0.5,
                    center[1] + (uu[1] * su + ww[1] * sw) * 0.5,
                    center[2] + (uu[2] * su + ww[2] * sw) * 0.5,
                )
            )
            normals.append(nn)
            # glTF 的 UV 原点在左上角，故 v 取反，使贴图不上下颠倒。
            uvs.append(((su + 1.0) * 0.5, 1.0 - (sw + 1.0) * 0.5))
        indices += [start, start + 1, start + 2, start, start + 2, start + 3]

    return positions, normals, uvs, indices


def build_sphere(radius: float, segments: int, rings: int) -> Tuple[List[Vec3], List[Vec3], List[Tuple[float, float]], List[int]]:
    """生成 UV 球。

    不使用极点共享：极点处的 UV 是退化的，会让 MikkTSpace 在同一顶点上求出互相
    矛盾的切线。这里按 ring 复制极点顶点，代价是少量重复顶点，换来确定性的切线。
    """
    positions: List[Vec3] = []
    normals: List[Vec3] = []
    uvs: List[Tuple[float, float]] = []
    indices: List[int] = []

    for j in range(rings + 1):
        theta = math.pi * j / rings
        for i in range(segments + 1):
            phi = 2.0 * math.pi * i / segments
            nx = math.sin(theta) * math.cos(phi)
            ny = math.cos(theta)
            nz = math.sin(theta) * math.sin(phi)
            positions.append((nx * radius, ny * radius, nz * radius))
            normals.append((nx, ny, nz))
            uvs.append((i / segments, 1.0 - j / rings))

    for j in range(rings):
        for i in range(segments):
            a = j * (segments + 1) + i
            b = a + segments + 1
            quad = (a, b, b + 1, a + 1)
            # 每个四边形切成两个三角形；绕序按面法线与球面法线同向来纠正，
            # 避免"手推一次绕序、换个参数就反了"的隐性错误。
            p0, p1, p2 = positions[quad[0]], positions[quad[1]], positions[quad[2]]
            face = normalize(cross((p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]), (p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2])))
            reference = normals[quad[0]]
            if face[0] * reference[0] + face[1] * reference[1] + face[2] * reference[2] < 0.0:
                indices += [quad[0], quad[2], quad[1], quad[0], quad[3], quad[2]]
            else:
                indices += [quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]]

    return positions, normals, uvs, indices


# ============================================================================
# glTF 组装
# ============================================================================
class GltfBuilder:
    """极简 glTF 2.0 组装器：只支持本场景用到的子集，不支持就抛异常。"""

    def __init__(self) -> None:
        self.buffer = bytearray()
        self.buffer_views: List[Dict[str, Any]] = []
        self.accessors: List[Dict[str, Any]] = []

    def _append_aligned(self, data: bytes, alignment: int, target: int) -> int:
        while len(self.buffer) % alignment != 0:
            self.buffer.append(0)
        offset = len(self.buffer)
        self.buffer += data
        self.buffer_views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data), "target": target})
        return len(self.buffer_views) - 1

    def add_vec3(self, values: Sequence[Vec3]) -> int:
        payload = bytearray()
        min_v = [float("inf")] * 3
        max_v = [float("-inf")] * 3
        for v in values:
            payload += struct.pack("<fff", float(v[0]), float(v[1]), float(v[2]))
            for axis in range(3):
                min_v[axis] = min(min_v[axis], v[axis])
                max_v[axis] = max(max_v[axis], v[axis])
        view = self._append_aligned(bytes(payload), 4, TARGET_ARRAY_BUFFER)
        self.accessors.append(
            {
                "bufferView": view,
                "componentType": 5126,  # FLOAT
                "count": len(values),
                "type": "VEC3",
                "min": [float(x) for x in min_v],
                "max": [float(x) for x in max_v],
            }
        )
        return len(self.accessors) - 1

    def add_vec2(self, values: Sequence[Tuple[float, float]]) -> int:
        payload = bytearray()
        for v in values:
            payload += struct.pack("<ff", float(v[0]), float(v[1]))
        view = self._append_aligned(bytes(payload), 4, TARGET_ARRAY_BUFFER)
        self.accessors.append(
            {"bufferView": view, "componentType": 5126, "count": len(values), "type": "VEC2"}
        )
        return len(self.accessors) - 1

    def add_indices(self, values: Sequence[int]) -> int:
        payload = bytearray()
        for v in values:
            payload += struct.pack("<I", int(v))
        view = self._append_aligned(bytes(payload), 4, TARGET_ELEMENT_ARRAY_BUFFER)
        self.accessors.append(
            {"bufferView": view, "componentType": COMPONENT_TYPE_UNSIGNED_INT, "count": len(values), "type": "SCALAR"}
        )
        return len(self.accessors) - 1


def quaternion_from_axis_angle(axis: Vec3, angle_radians: float) -> List[float]:
    half = angle_radians * 0.5
    s = math.sin(half)
    n = normalize(axis)
    return [n[0] * s, n[1] * s, n[2] * s, math.cos(half)]


def main() -> None:
    os.makedirs(OUT_DIR, exist_ok=True)
    textures = build_textures(OUT_DIR)

    builder = GltfBuilder()

    sphere_geo = build_sphere(0.5, 32, 16)
    box_geo = build_box()

    sphere_attrs = (
        builder.add_vec3(sphere_geo[0]),
        builder.add_vec3(sphere_geo[1]),
        builder.add_vec2(sphere_geo[2]),
        builder.add_indices(sphere_geo[3]),
    )
    box_attrs = (
        builder.add_vec3(box_geo[0]),
        builder.add_vec3(box_geo[1]),
        builder.add_vec2(box_geo[2]),
        builder.add_indices(box_geo[3]),
    )

    def primitive(attrs, material: int) -> Dict[str, Any]:
        return {
            "attributes": {"POSITION": attrs[0], "NORMAL": attrs[1], "TEXCOORD_0": attrs[2]},
            "indices": attrs[3],
            "material": material,
        }

    # ---- 材质 ----
    # 25 个球材质 + 地面 + 薄柱 + 斜面 + 贴图方块 = 29 个。
    # 顺序固定（先列 metallic 再行 roughness），保证重复生成时索引一致。
    materials: List[Dict[str, Any]] = []
    grid_materials: List[int] = []
    for row in range(5):
        for column in range(5):
            metallic = column / 4.0
            roughness = 0.1 + row * 0.2
            materials.append(
                {
                    "name": "SphereGrid_m%02d_r%02d" % (column, row),
                    "pbrMetallicRoughness": {
                        "baseColorFactor": [0.92, 0.92, 0.94, 1.0],
                        "metallicFactor": metallic,
                        "roughnessFactor": roughness,
                    },
                }
            )
            grid_materials.append(len(materials) - 1)

    ground_material = len(materials)
    materials.append(
        {
            "name": "GroundPlane",
            "pbrMetallicRoughness": {"baseColorFactor": [0.42, 0.44, 0.47, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.85},
        }
    )
    column_material = len(materials)
    materials.append(
        {
            "name": "ThinColumn",
            "pbrMetallicRoughness": {"baseColorFactor": [0.78, 0.72, 0.62, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.6},
        }
    )
    slope_material = len(materials)
    materials.append(
        {
            "name": "Slope",
            "pbrMetallicRoughness": {"baseColorFactor": [0.30, 0.38, 0.52, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.5},
        }
    )
    textured_material = len(materials)
    materials.append(
        {
            "name": "TexturedSlab",
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "metallicFactor": 1.0,
                "roughnessFactor": 1.0,
                "baseColorTexture": {"index": 0},
                "metallicRoughnessTexture": {"index": 2},
            },
            "normalTexture": {"index": 1},
            "occlusionTexture": {"index": 3},
            "emissiveTexture": {"index": 4},
            "emissiveFactor": [1.0, 1.0, 1.0],
        }
    )

    # ---- 网格：25 个球网格共享同一份 accessor，只是材质不同 ----
    meshes: List[Dict[str, Any]] = []
    sphere_mesh_start = len(meshes)
    for index, material in enumerate(grid_materials):
        meshes.append({"name": "Sphere%02d" % index, "primitives": [primitive(sphere_attrs, material)]})

    ground_mesh = len(meshes)
    meshes.append({"name": "GroundSlab", "primitives": [primitive(box_attrs, ground_material)]})
    column_mesh = len(meshes)
    meshes.append({"name": "ColumnSlab", "primitives": [primitive(box_attrs, column_material)]})
    slope_mesh = len(meshes)
    meshes.append({"name": "SlopeSlab", "primitives": [primitive(box_attrs, slope_material)]})
    textured_mesh = len(meshes)
    meshes.append({"name": "TexturedSlab", "primitives": [primitive(box_attrs, textured_material)]})
    # 镜像实例必须独占一个 mesh 索引：Cooker 的 mesh URI 由网格索引派生，两个节点共用
    # 同一 mesh 会让 manifest 出现两条相同 assetUri 的条目，而 AssetRegistry 对重复
    # AssetId 是 fail-closed（"duplicate asset identity" → 整份 manifest 被拒）。
    # 这是 Cooker 侧的既有行为，此处按"场景避让"处理，不在本工具里规避。
    mirror_mesh = len(meshes)
    meshes.append({"name": "MirrorSlopeSlab", "primitives": [primitive(box_attrs, slope_material)]})

    # ---- 节点 ----
    # 相机固定在引擎坐标 (0, 3, -10) 朝 +Z，故场景主体放在 z ∈ [0, 10]。
    # glTF +X 在引擎里是 -X，球阵列关于 X 对称，镜像不会影响可读性。
    nodes: List[Dict[str, Any]] = [{"name": "M4VisualBaseline", "children": []}]
    root_children: List[int] = []

    def add_node(node: Dict[str, Any]) -> int:
        nodes.append(node)
        index = len(nodes) - 1
        root_children.append(index)
        return index

    add_node({"name": "GroundPlane", "mesh": ground_mesh, "translation": [0.0, -0.25, 5.0], "scale": [24.0, 0.5, 20.0]})

    # 球阵列是"正对相机的墙面"而不是"铺在地面的列阵"：固定相机在 (0,3,-10) 且
    # 俯仰角为 0，地面上的多行球会因视线几乎平行于地面而在屏幕上叠成一条带，
    # 无法逐球判读 metallic/roughness 梯度。列 = metallic 0→1，行 = roughness
    # 0.1→0.9（自下而上），与 02 篇的材质因子约定一致。
    column_spacing = 2.0
    row_spacing = 1.6
    for row in range(5):
        for column in range(5):
            x = (column - 2) * column_spacing
            y = 1.4 + row * row_spacing
            add_node(
                {
                    "name": "Sphere_c%d_r%d" % (column, row),
                    "mesh": sphere_mesh_start + row * 5 + column,
                    "translation": [x, y, 0.5],
                }
            )

    add_node({"name": "ThinColumn", "mesh": column_mesh, "translation": [7.0, 1.5, 2.0], "scale": [0.30, 3.0, 0.30]})
    add_node({"name": "Slope", "mesh": slope_mesh, "translation": [-6.0, 0.45, 5.0], "rotation": quaternion_from_axis_angle((0.0, 0.0, 1.0), math.radians(18.0)), "scale": [4.0, 0.3, 3.0]})
    # 镜像实例：X 取负行列式为 -1，Cooker/引擎必须把它标成 mirrored 并翻转绕序，
    # 否则这一块的正面会被背面剔除掉（这是 06 篇"mirrored winding"的可视化锚点）。
    add_node(
        {
            "name": "MirroredSlope",
            "mesh": mirror_mesh,
            "translation": [-13.0, 0.45, 12.0],
            "rotation": quaternion_from_axis_angle((0.0, 0.0, 1.0), math.radians(18.0)),
            "scale": [-4.0, 0.3, 3.0],
        }
    )
    add_node({"name": "TexturedSlab", "mesh": textured_mesh, "translation": [9.0, 1.6, 12.0], "scale": [3.0, 3.0, 3.0]})

    nodes[0]["children"] = root_children

    gltf: Dict[str, Any] = {
        "asset": {"version": "2.0", "generator": "MiniEngine tools/gen-m4-visual-baseline.py"},
        "scene": 0,
        "scenes": [{"name": "m4-visual-baseline", "nodes": [0]}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "textures": [{"sampler": 0, "source": i} for i in range(5)],
        "samplers": [SAMPLER_LINEAR_MIPMAP_LINEAR_REPEAT],
        "images": [{"uri": textures[key]} for key in ("baseColorTexture", "normalTexture", "metallicRoughnessTexture", "occlusionTexture", "emissiveTexture")],
        "accessors": builder.accessors,
        "bufferViews": builder.buffer_views,
        "buffers": [{"uri": BIN_NAME, "byteLength": len(builder.buffer)}],
    }

    with open(os.path.join(OUT_DIR, BIN_NAME), "wb") as f:
        f.write(bytes(builder.buffer))
    with open(os.path.join(OUT_DIR, GLTF_NAME), "w", encoding="utf-8", newline="\n") as f:
        json.dump(gltf, f, indent=2, ensure_ascii=False, sort_keys=False)
        f.write("\n")

    print("wrote %s (%d bytes) + %s (%d bytes)" % (GLTF_NAME, os.path.getsize(os.path.join(OUT_DIR, GLTF_NAME)), BIN_NAME, len(builder.buffer)))


if __name__ == "__main__":
    main()
