#!/usr/bin/env python3
"""软阴影展示的精选采集、原始数据核对与拼图入口。

只依赖 Python、NumPy 和 Pillow；原生演示正常运行不依赖本工具。
collect 必须使用新目录，analyze 只读既有 run，另建分析目录。
这里的 PASS 只表示公开列出的机器检查通过，不代替独立实现审查。
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import stat
import struct
import subprocess
import sys
import time
from typing import Any


PARAM_KEYS = {"scene", "shadow", "lightSize", "bias", "samples", "indirect", "width", "height"}
OUTPUT_ROLES = {"color", "visibility", "diagnostic", "linearHdr", "diagnostics", "shadowDepth", "visibilityRaw"}
IDENTITY_ROLES = {"executable", "shaderSource", "shaderBinary", "shaderInclude"}
SHA_LENGTH = 64


class EvidenceError(ValueError):
    """输入或采集未满足已声明契约，调用方必须保留现场。"""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def write_json(path: Path, document: Any) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"),
                           parse_constant=lambda token: (_ for _ in ()).throw(EvidenceError(f"non-finite JSON: {token}")))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"cannot read JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise EvidenceError(f"expected JSON object: {path}")
    return value


def reject_reparse(path: Path) -> None:
    """拒绝任意现存父路径的 symlink/junction，不把输出重定向到其他任务。"""
    current = path.absolute()
    for candidate in (current, *current.parents):
        if candidate.exists() or candidate.is_symlink():
            info = candidate.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise EvidenceError(f"symlink/reparse path is not allowed: {candidate}")


def new_directory(path: Path) -> None:
    reject_reparse(path)
    if path.exists():
        raise EvidenceError(f"output already exists; choose a new run directory: {path}")
    path.mkdir(parents=True, exist_ok=False)


def artifact_path(root: Path, relative: Any) -> Path:
    if not isinstance(relative, str) or not relative or "\\" in relative:
        raise EvidenceError("artifact path must be a nonempty portable relative path")
    path = Path(relative)
    if path.is_absolute() or any(part in ("..", ".") for part in path.parts) or ":" in relative:
        raise EvidenceError(f"artifact path escapes run: {relative}")
    target = root / path
    reject_reparse(target)
    if not target.is_file() or root.resolve() not in target.resolve().parents:
        raise EvidenceError(f"missing/outside artifact: {relative}")
    return target


def finite_number(value: Any) -> bool:
    return isinstance(value, (float, int)) and not isinstance(value, bool) and math.isfinite(value)


def valid_hash(value: Any) -> bool:
    return isinstance(value, str) and len(value) == SHA_LENGTH and all(c in "0123456789abcdef" for c in value)


def check_params(params: Any) -> dict:
    if not isinstance(params, dict) or set(params) != PARAM_KEYS:
        raise EvidenceError(f"params keys must be exactly {sorted(PARAM_KEYS)}")
    for key, allowed in (("scene", ("shadow-bench", "cornell")), ("shadow", ("hard", "pcf", "pcss")),
                         ("indirect", ("direct", "bounce"))):
        if params[key] not in allowed:
            raise EvidenceError(f"unknown params.{key}: {params[key]}")
    for key, low, high in (("lightSize", .01, 2.5), ("bias", 0., .5)):
        if not finite_number(params[key]) or not low <= params[key] <= high:
            raise EvidenceError(f"out-of-range params.{key}")
    for key, low, high in (("samples", 4, 128), ("width", 64, 3840), ("height", 64, 2160)):
        if not isinstance(params[key], int) or isinstance(params[key], bool) or not low <= params[key] <= high:
            raise EvidenceError(f"out-of-range integer params.{key}")
    return params


def same_params(actual: dict, requested: dict) -> bool:
    """运行时 float 常量按 IEEE754 binary32 身份比对，其余参数严格相等。"""
    check_params(actual)
    check_params(requested)
    for key in PARAM_KEYS:
        if key in ("lightSize", "bias"):
            if struct.pack("<f", actual[key]) != struct.pack("<f", requested[key]):
                return False
        elif actual[key] != requested[key]:
            return False
    return True


def dependencies():
    try:
        import numpy as np
        from PIL import Image, ImageDraw
    except ImportError as error:
        raise EvidenceError("NumPy and Pillow are required; use an already configured Python runtime") from error
    return np, Image, ImageDraw


def output_transform(linear):
    np, _, _ = dependencies()
    nonnegative = np.maximum(linear[..., :3], 0.)
    mapped = nonnegative / (1. + nonnegative)
    srgb = np.where(mapped <= .0031308, 12.92 * mapped, 1.055 * np.power(mapped, 1. / 2.4) - .055)
    return np.rint(np.clip(srgb, 0., 1.) * 255).astype(np.uint8)


def validate_run(root: Path, expected_params: dict | None = None,
                 expected_identity: dict | None = None, started_at: float | None = None) -> dict:
    """核对字节、身份、数据和呈现；不由 runtime 自报 status 推导技术正确性。"""
    np, Image, _ = dependencies()
    reject_reparse(root)
    result_path = root / "result.json"
    result = read_json(result_path)
    if result.get("schemaVersion") != 1 or result.get("status") != "complete":
        raise EvidenceError("result must be schemaVersion=1 and status=complete")
    if result.get("backend") != "D3D12-native":
        raise EvidenceError("native D3D12 backend is required")
    params = check_params(result.get("params"))
    if expected_params is not None and not same_params(params, expected_params):
        raise EvidenceError("result params do not match the launched command")
    if not isinstance(result.get("runId"), str) or not result["runId"].strip():
        raise EvidenceError("missing unique runtime runId")
    adapter = result.get("adapter", {})
    if not isinstance(adapter, dict) or adapter.get("software") is not False or not adapter.get("description"):
        raise EvidenceError("hardware adapter metadata is required")
    debug = result.get("debugLayer", {})
    if (not isinstance(debug, dict) or debug.get("enabled") is not True
            or type(debug.get("errorCount")) is not int or type(debug.get("warningCount")) is not int
            or debug.get("errorCount") != 0 or debug.get("warningCount") != 0):
        raise EvidenceError("enabled, clean D3D12 debug layer is required")
    timing = result.get("timing", {})
    if (not isinstance(timing, dict) or not isinstance(timing.get("scope"), str) or not timing["scope"].strip()
            or not all(finite_number(timing.get(key)) and timing[key] > 0.
                       for key in ("shadowMapGpuMs", "shadeGpuMs", "totalGpuMs", "timestampFrequency"))
            or timing["totalGpuMs"] < max(timing["shadowMapGpuMs"], timing["shadeGpuMs"])):
        raise EvidenceError("positive GPU timing and explicit scope are required")
    ticks = timing.get("gpuTicks")
    if (not isinstance(ticks, list) or len(ticks) != 3 or any(type(tick) is not int or tick <= 0 for tick in ticks)
            or not ticks[0] < ticks[1] < ticks[2]):
        raise EvidenceError("nonzero, monotonic GPU timestamps are required")
    raw_ms = [(ticks[1] - ticks[0]) * 1000. / timing["timestampFrequency"],
              (ticks[2] - ticks[1]) * 1000. / timing["timestampFrequency"],
              (ticks[2] - ticks[0]) * 1000. / timing["timestampFrequency"]]
    for key, measured in zip(("shadowMapGpuMs", "shadeGpuMs", "totalGpuMs"), raw_ms):
        if not math.isclose(timing[key], measured, rel_tol=1e-7, abs_tol=1e-7):
            raise EvidenceError(f"GPU timing does not match raw ticks: {key}")
    identity = result.get("identity", {})
    if not isinstance(identity, dict) or not IDENTITY_ROLES.issubset(identity):
        raise EvidenceError("missing executable/shader identity")
    for role in IDENTITY_ROLES:
        record = identity[role]
        if not isinstance(record, dict) or not valid_hash(record.get("sha256")):
            raise EvidenceError(f"invalid identity hash: {role}")
        if expected_identity is not None and record["sha256"] != expected_identity[role]:
            raise EvidenceError(f"identity mismatch: {role}")
    outputs = result.get("outputs", {})
    if not isinstance(outputs, dict) or not OUTPUT_ROLES.issubset(outputs):
        raise EvidenceError(f"missing outputs; require {sorted(OUTPUT_ROLES)}")
    files = {}
    for role, record in outputs.items():
        if not isinstance(record, dict) or not valid_hash(record.get("sha256")):
            raise EvidenceError(f"invalid output hash: {role}")
        path = artifact_path(root, record.get("path"))
        if type(record.get("bytes")) is not int or record["bytes"] <= 0 or path.stat().st_size != record["bytes"]:
            raise EvidenceError(f"output size mismatch: {role}")
        if sha256(path) != record["sha256"]:
            raise EvidenceError(f"output SHA mismatch: {role}")
        if started_at is not None and path.stat().st_mtime < started_at - 2.:
            raise EvidenceError(f"stale output predates process: {role}")
        files[role] = path
    if len({record["path"] for record in outputs.values()}) != len(outputs):
        raise EvidenceError("different output roles alias the same file")
    if started_at is not None and result_path.stat().st_mtime < started_at - 2.:
        raise EvidenceError("stale result predates process")
    width, height = params["width"], params["height"]
    arrays = {}
    for role, channels in (("linearHdr", 4), ("diagnostics", 4), ("visibilityRaw", 1)):
        path = files[role]
        if path.stat().st_size != width * height * channels * 4:
            raise EvidenceError(f"raw float count mismatch: {role}")
        data = np.fromfile(path, dtype="<f4").reshape(height, width, channels)
        if not np.isfinite(data).all():
            raise EvidenceError(f"non-finite raw data: {role}")
        arrays[role] = data
    shadow = np.fromfile(files["shadowDepth"], dtype="<f4")
    shadow_size = result.get("shadowMap", {}).get("size")
    if type(shadow_size) is not int or shadow.size != shadow_size ** 2 or not np.isfinite(shadow).all():
        raise EvidenceError("invalid shadow depth count/data")
    if not (shadow >= 0.).all() or not (arrays["linearHdr"][..., :3] >= 0.).all():
        raise EvidenceError("negative radiance/depth")
    diagnostics = arrays["diagnostics"]
    visibility = diagnostics[..., 0]
    if not ((visibility >= 0.) & (visibility <= 1.)).all():
        raise EvidenceError("visibility outside [0,1]")
    if not np.array_equal(visibility, arrays["visibilityRaw"][..., 0]):
        raise EvidenceError("visibility channels do not agree")
    if not (diagnostics[..., 1:3] >= 0.).all():
        raise EvidenceError("negative blocker depth or penumbra")
    if not np.all(np.equal(diagnostics[..., 3], np.rint(diagnostics[..., 3]))):
        raise EvidenceError("material IDs must be integral")
    pixels = {}
    for role in ("color", "visibility", "diagnostic"):
        with Image.open(files[role]) as image:
            if image.format != "PNG" or image.size != (width, height):
                raise EvidenceError(f"invalid PNG format/dimensions: {role}")
            pixels[role] = np.array(image.convert("RGB"))
    difference = np.abs(pixels["color"].astype(np.int16) - output_transform(arrays["linearHdr"]).astype(np.int16))
    if int(difference.max()) > 1:
        raise EvidenceError("color PNG does not match linear HDR/output transform")
    expected_visibility = np.rint(visibility * 255).astype(np.uint8)
    if np.abs(pixels["visibility"].astype(np.int16) - expected_visibility[..., None].astype(np.int16)).max() > 1:
        raise EvidenceError("visibility PNG does not match float visibility")
    diagnostic_rgb = np.stack((diagnostics[..., 1] / 8., diagnostics[..., 2] * 50., 1. - diagnostics[..., 0]), axis=2)
    expected_diagnostic = np.rint(np.clip(diagnostic_rgb, 0., 1.) * 255).astype(np.uint8)
    if np.abs(pixels["diagnostic"].astype(np.int16) - expected_diagnostic.astype(np.int16)).max() > 1:
        raise EvidenceError("diagnostic PNG does not match raw channels")
    geometry = {key: result.get(key) for key in ("camera", "light", "geometry")}
    validate_geometry(geometry)
    # 内存规范化便于参考计算；不改写运行时原始 result.json。
    result["sceneMetadata"] = geometry
    material_ids = {-1, *(box["materialId"] for box in geometry["geometry"]["boxes"])}
    if not np.isin(diagnostics[..., 3], list(material_ids)).all():
        raise EvidenceError("diagnostics contain an unknown material ID")
    if not np.any(arrays["linearHdr"][..., :3] > 0.) or not np.any(diagnostics[..., 3] >= 0):
        raise EvidenceError("empty radiance/geometry signal")
    return {"root": root, "result": result, "params": params, "files": files, "arrays": arrays,
            "color": pixels["color"], "pngMaxError": int(difference.max())}


def vector(value: Any, label: str) -> tuple:
    if not isinstance(value, list) or len(value) != 3 or not all(finite_number(x) for x in value):
        raise EvidenceError(f"invalid 3-vector: {label}")
    return tuple(float(x) for x in value)


def validate_geometry(metadata: Any) -> None:
    if not isinstance(metadata, dict):
        raise EvidenceError("missing sceneMetadata")
    camera = metadata.get("camera", {})
    for key in ("position", "target", "up"):
        vector(camera.get(key), f"camera.{key}")
    fov = camera.get("verticalFovDegrees")
    if not finite_number(fov) or not 1. < fov < 175.:
        raise EvidenceError("invalid camera FOV")
    vector(metadata.get("light", {}).get("position"), "light.position")
    boxes = metadata.get("geometry", {}).get("boxes")
    if not isinstance(boxes, list) or not boxes:
        raise EvidenceError("missing scene boxes")
    for box in boxes:
        low, high = vector(box.get("min"), "box.min"), vector(box.get("max"), "box.max")
        if not all(a < b for a, b in zip(low, high)) or type(box.get("materialId")) is not int:
            raise EvidenceError("invalid box extent/material ID")


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def subtract(a, b):
    return tuple(x - y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(value):
    length = math.sqrt(dot(value, value))
    if length < 1e-10:
        raise EvidenceError("degenerate camera basis")
    return tuple(x / length for x in value)


def ray_box(origin, direction, low, high):
    """CPU 双精度 slab 求交；返回首交和面法线，不读取 GPU 深度或可见性。"""
    near, far, normal = -math.inf, math.inf, (0., 0., 0.)
    for axis in range(3):
        if abs(direction[axis]) < 1e-12:
            if not low[axis] <= origin[axis] <= high[axis]:
                return None
            continue
        first = (low[axis] - origin[axis]) / direction[axis]
        second = (high[axis] - origin[axis]) / direction[axis]
        sign = -1. if first < second else 1.
        first, second = min(first, second), max(first, second)
        if first > near:
            near = first
            normal = tuple(sign if index == axis else 0. for index in range(3))
        far = min(far, second)
        if far < near:
            return None
    if near <= 1e-7 or far < near:
        return None
    return near, normal


def camera_ray(camera: dict, x: float, y: float, width: int, height: int):
    position, target, up = (tuple(camera[key]) for key in ("position", "target", "up"))
    forward = normalize(subtract(target, position))
    # 原生入口采用 forward×up 的右手观察基；反序会把 CPU 参考水平镜像。
    right = normalize(cross(forward, up))
    vertical = cross(right, forward)
    tan_half = math.tan(math.radians(camera["verticalFovDegrees"]) / 2.)
    u, v = (2. * x / width - 1.) * width / height * tan_half, (1. - 2. * y / height) * tan_half
    return position, normalize(tuple(forward[i] + right[i] * u + vertical[i] * v for i in range(3)))


def first_hit(boxes: list, origin, direction):
    best = None
    for index, box in enumerate(boxes):
        intersection = ray_box(origin, direction, box["min"], box["max"])
        if intersection is not None and (best is None or intersection[0] < best[0]):
            best = (*intersection, index)
    return best


def segment_occluded(point, light, boxes: list, receiver: int, margin: float = 0.) -> bool:
    delta = subtract(light, point)
    for index, box in enumerate(boxes):
        if index == receiver:
            continue
        low = tuple(box["min"][i] - margin for i in range(3))
        high = tuple(box["max"][i] + margin for i in range(3))
        if any(a >= b for a, b in zip(low, high)):
            continue
        intersection = ray_box(point, delta, low, high)
        if intersection is not None and 1e-5 < intersection[0] < 1. - 1e-5:
            return True
    return False


def hard_reference(run: dict, stride: int = 6, margin: float = .035) -> dict:
    """固定采样域由 CPU 几何决定，边界排除不依赖 GPU 信号。"""
    params, metadata = run["params"], run["result"]["sceneMetadata"]
    camera, boxes, light = metadata["camera"], metadata["geometry"]["boxes"], metadata["light"]["position"]
    width, height = params["width"], params["height"]
    diagnostics = run["arrays"]["diagnostics"]
    tested = shadowed = lit = boundaries = material_errors = visibility_errors = 0
    worst = []
    for row in range(stride // 2, height, stride):
        for col in range(stride // 2, width, stride):
            origin, direction = camera_ray(camera, col + .5, row + .5, width, height)
            hit = first_hit(boxes, origin, direction)
            if hit is None:
                continue
            distance, normal, receiver = hit
            # 图像几何边界带：角点射线须仍属于同盒同面。
            stable = True
            for dx, dy in ((-.6, -.6), (.6, -.6), (-.6, .6), (.6, .6)):
                edge_origin, edge_direction = camera_ray(camera, col + .5 + dx, row + .5 + dy, width, height)
                edge_hit = first_hit(boxes, edge_origin, edge_direction)
                if edge_hit is None or edge_hit[1:] != hit[1:]:
                    stable = False
                    break
            if not stable:
                boundaries += 1
                continue
            point = tuple(origin[i] + direction[i] * distance + normal[i] * .001 for i in range(3))
            # 背光面朝光源的线段穿过自身盒体；外盒margin不得制造虚假的自交。
            self_blocked = dot(normal, subtract(light, point)) < -1e-6
            blocked = self_blocked or segment_occluded(point, light, boxes, receiver, -margin)
            possible = self_blocked or segment_occluded(point, light, boxes, receiver, margin)
            if blocked != possible:
                boundaries += 1
                continue
            expected = 0. if blocked else 1.
            actual = float(diagnostics[row, col, 0])
            material_wrong = float(diagnostics[row, col, 3]) != boxes[receiver]["materialId"]
            material_errors += material_wrong
            visibility_errors += abs(expected - actual) > .05
            if (abs(expected - actual) > .05 or material_wrong) and len(worst) < 12:
                worst.append({"x": col, "y": row, "reference": expected, "actual": actual,
                              "expectedMaterial": boxes[receiver]["materialId"],
                              "actualMaterial": float(diagnostics[row, col, 3])})
            tested += 1
            shadowed += blocked
            lit += not blocked
    minimum = max(20, width * height // (stride * stride * 25))
    # 少量剩余 raster shadow 边界误差单列，不允许丢弃整个失败区。
    ok = tested >= minimum and shadowed >= 5 and lit >= 5 and material_errors == 0 and visibility_errors <= tested * .015
    return {"pass": bool(ok), "tested": tested, "shadowed": shadowed, "lit": lit, "boundaryExcluded": boundaries,
            "materialErrors": material_errors, "visibilityErrors": visibility_errors,
            "visibilityErrorFraction": visibility_errors / tested if tested else None,
            "stride": stride, "worldBoundaryMargin": margin, "worst": worst}


def difference(left: dict, right: dict, role: str = "diagnostics", channels=(0,)) -> dict:
    np, _, _ = dependencies()
    a, b = left["arrays"][role][..., channels], right["arrays"][role][..., channels]
    if a.shape != b.shape:
        raise EvidenceError("comparison shape mismatch")
    delta = np.abs(a.astype(np.float64) - b.astype(np.float64))
    return {"maxAbs": float(delta.max()), "meanAbs": float(delta.mean()),
            "changedElements": int(np.count_nonzero(delta > 1e-6)), "elements": int(delta.size)}


def presets(width: int, height: int) -> dict:
    base = {"scene": "shadow-bench", "shadow": "pcss", "lightSize": .65, "bias": .004,
            "samples": 64, "indirect": "direct", "width": width, "height": height}
    variants = {"hard": {"shadow": "hard"}, "pcf": {"shadow": "pcf"}, "pcss": {},
                "light-small": {"lightSize": .15}, "light-large": {"lightSize": 1.2},
                "bias-large": {"bias": .3}, "samples-low": {"samples": 8},
                "cornell-direct": {"scene": "cornell"},
                "cornell-bounce": {"scene": "cornell", "indirect": "bounce"}, "restore": {}}
    return {name: {**base, **variant} for name, variant in variants.items()}


def command(executable: Path, params: dict, output: Path) -> list:
    names = {"lightSize": "light-size"}
    args = [str(executable), "--debug"]
    for key, value in params.items():
        args.extend(("--" + names.get(key, key), str(value)))
    return args + ["--output", str(output)]


def source_binding(root: Path) -> dict:
    """仅绑定可编译源码与构建/采集工具，不把正在编辑的说明页误当源码漂移。"""
    listing = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                             cwd=root, capture_output=True, check=False)
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, capture_output=True, text=True, check=False)
    if listing.returncode or revision.returncode:
        raise EvidenceError("source root must be a Git checkout")
    suffixes = {".cpp", ".h", ".hpp", ".hlsl", ".hlsli", ".cmake", ".py"}
    files = {}
    for encoded in listing.stdout.split(b"\0"):
        if not encoded:
            continue
        relative = encoded.decode("utf-8")
        path = root / relative
        if path.suffix.lower() in suffixes or path.name in ("CMakeLists.txt", "CMakePresets.json"):
            reject_reparse(path)
            if path.is_file():
                files[relative] = {"bytes": path.stat().st_size, "sha256": sha256(path)}
    if not files:
        raise EvidenceError("source binding is empty")
    return {"head": revision.stdout.strip(), "files": files}


def run_process(args: list, log: Path, timeout: float) -> dict:
    started = time.time()
    try:
        with log.open("xb") as stream:
            process = subprocess.run(args, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout, check=False)
        return {"command": args, "returncode": process.returncode, "startedEpoch": started,
                "elapsedSeconds": time.time() - started, "log": log.name}
    except subprocess.TimeoutExpired as error:
        raise EvidenceError(f"process timed out after {timeout}s; log retained at {log}") from error


def analyze_checks(runs: dict) -> dict:
    np, _, _ = dependencies()
    required = set(presets(64, 64))
    if set(runs) != required:
        raise EvidenceError(f"suite run set mismatch; require {sorted(required)}")
    size = runs["pcss"]["params"]
    expected_presets = presets(size["width"], size["height"])
    for name, run in runs.items():
        if not same_params(run["params"], expected_presets[name]):
            raise EvidenceError(f"selected preset mismatch: {name}")
    for names in (("hard", "pcf", "pcss", "light-small", "light-large", "bias-large", "samples-low", "restore"),
                  ("cornell-direct", "cornell-bounce")):
        first = runs[names[0]]["result"]["sceneMetadata"]
        for name in names[1:]:
            metadata = runs[name]["result"]["sceneMetadata"]
            if (metadata["camera"] != first["camera"] or metadata["geometry"] != first["geometry"]
                    or metadata["light"]["position"] != first["light"]["position"]):
                raise EvidenceError(f"camera/geometry/light changed inside comparison: {name}")
    checks = {"hardReference": hard_reference(runs["hard"])}
    pairs = (("hardToPcf", "hard", "pcf"), ("pcfToPcss", "pcf", "pcss"),
             ("lightSize", "light-small", "light-large"), ("bias", "pcss", "bias-large"),
             ("sampling", "pcss", "samples-low"))
    for label, left, right in pairs:
        metric = difference(runs[left], runs[right])
        metric["pass"] = metric["changedElements"] >= 10 and metric["maxAbs"] > .01
        checks[label] = metric
    restore = difference(runs["pcss"], runs["restore"], "linearHdr", (0, 1, 2, 3))
    restore_diagnostics = difference(runs["pcss"], runs["restore"], "diagnostics", (0, 1, 2, 3))
    checks["restore"] = {"pass": restore["maxAbs"] == 0. and restore_diagnostics["maxAbs"] == 0.,
                         "linearHdr": restore, "diagnostics": restore_diagnostics}
    fractions = {}
    for name in ("hard", "pcf", "pcss", "light-small", "light-large"):
        d = runs[name]["arrays"]["diagnostics"]
        valid = d[..., 3] >= 0
        fractions[name] = {"surfacePixels": int(valid.sum()),
                           "fractionalPixels": int(((d[..., 0] > .001) & (d[..., 0] < .999) & valid).sum()),
                           "maxPenumbraUv": float(d[..., 2].max())}
    checks["softness"] = {"pass": fractions["hard"]["fractionalPixels"] == 0
                          and fractions["pcf"]["fractionalPixels"] > 10
                          and fractions["light-large"]["fractionalPixels"] > fractions["light-small"]["fractionalPixels"]
                          and fractions["light-large"]["maxPenumbraUv"] > fractions["light-small"]["maxPenumbraUv"],
                          "runs": fractions}
    direct, bounce = runs["cornell-direct"], runs["cornell-bounce"]
    delta = bounce["arrays"]["linearHdr"][..., :3] - direct["arrays"]["linearHdr"][..., :3]
    surface = direct["arrays"]["diagnostics"][..., 3] >= 0
    checks["bounce"] = {"pass": bool(np.isfinite(delta).all() and float(delta.min()) >= -1e-5
                                      and int(((delta.max(axis=2) > .005) & surface).sum()) >= 20),
                         "minChannelDelta": float(delta.min()), "maxChannelDelta": float(delta.max()),
                         "changedSurfacePixels": int(((delta.max(axis=2) > .005) & surface).sum()),
                         "scope": "一次漫反射教学模型的非零信号；不构成多次反弹 GI 或物理精度验收"}
    return {"status": "PASS" if all(check["pass"] for check in checks.values()) else "FAIL", "checks": checks}


def collage(runs: dict, output: Path) -> list:
    _, Image, ImageDraw = dependencies()
    groups = {"algorithms": (("hard", "Hard shadow"), ("pcf", "PCF"), ("pcss", "PCSS")),
              "light-size": (("light-small", "PCSS / light size 0.15"), ("light-large", "PCSS / light size 1.20")),
              "bias": (("pcss", "PCSS / bias 0.004"), ("bias-large", "PCSS / bias 0.30 (artifact)")),
              "sampling": (("samples-low", "PCSS / 8 samples"), ("pcss", "PCSS / 64 samples")),
              "cornell": (("cornell-direct", "Cornell-style / direct"), ("cornell-bounce", "Cornell-style / one diffuse bounce"))}
    outputs = []
    for name, panels in groups.items():
        panel_width = min(runs[panels[0][0]]["params"]["width"], 720)
        original = runs[panels[0][0]]["params"]
        panel_height = round(original["height"] * panel_width / original["width"])
        canvas = Image.new("RGB", (panel_width * len(panels), panel_height + 56), (20, 24, 30))
        draw = ImageDraw.Draw(canvas)
        for index, (run_name, label) in enumerate(panels):
            image = Image.fromarray(runs[run_name]["color"], "RGB").resize((panel_width, panel_height), Image.Resampling.LANCZOS)
            canvas.paste(image, (index * panel_width, 56))
            draw.text((index * panel_width + 12, 10), label, fill=(240, 242, 245))
            params = runs[run_name]["params"]
            draw.text((index * panel_width + 12, 30),
                      f"light={params['lightSize']:g}  bias={params['bias']:g}  samples/phase={params['samples']}", fill=(172, 184, 198))
        path = output / (name + ".png")
        canvas.save(path)
        outputs.append(path.name)
    return outputs


def analyze(source: Path, output: Path) -> dict:
    source = source.resolve(strict=True)
    if (source / "artifact-manifest.json").is_file():
        verify_seal(source)
        if output.absolute() == source or source in output.absolute().parents:
            raise EvidenceError("analyze output cannot change a sealed source directory")
    new_directory(output)
    try:
        suite = read_json(source / "suite.json")
        if suite.get("schemaVersion") != 1 or suite.get("collectionComplete") is not True:
            raise EvidenceError("suite collection is incomplete")
        if not isinstance(suite.get("runs"), dict):
            raise EvidenceError("missing suite runs")
        binding = artifact_path(source, "source-binding.json")
        if not valid_hash(suite.get("sourceBindingSha256")) or sha256(binding) != suite["sourceBindingSha256"]:
            raise EvidenceError("source binding SHA mismatch")
        binding_data = read_json(binding)
        if not binding_data.get("files") or not isinstance(binding_data.get("head"), str):
            raise EvidenceError("source binding is empty/incomplete")
        runs, run_ids = {}, set()
        for name, launch in suite["runs"].items():
            if launch.get("returncode") != 0:
                raise EvidenceError(f"run process failed: {name}")
            run_dir = artifact_path(source, launch["result"]).parent
            run = validate_run(run_dir, launch["params"], suite["identity"], launch["startedEpoch"])
            if run["result"]["runId"] in run_ids:
                raise EvidenceError("duplicate runtime runId; possible stale result reuse")
            run_ids.add(run["result"]["runId"])
            runs[name] = run
        checks = analyze_checks(runs)
        negatives = suite.get("negativeTests", [])
        expected_negatives = {"unknown-option", "bad-light-size", "bad-samples", "non-finite-bias", "existing-output"}
        if (not isinstance(negatives, list) or {item.get("name") for item in negatives} != expected_negatives
                or len(negatives) != len(expected_negatives) or not all(item.get("pass") is True for item in negatives)):
            checks["status"] = "FAIL"
        report = {"schemaVersion": 1, "createdUtc": utc_now(), **checks,
                  "negativeTests": negatives, "collages": collage(runs, output),
                  "sourceSuiteSha256": sha256(source / "suite.json"),
                  "scope": "工具机器检查；独立实现审查另行签收", "pngMaxErrors": {name: run["pngMaxError"] for name, run in runs.items()}}
        write_json(output / "analysis.json", report)
        seal(output)
        return report
    except Exception as error:
        write_json(output / "failure.json", {"status": "FAIL", "error": str(error), "createdUtc": utc_now()})
        raise


def seal(root: Path) -> None:
    records = {}
    for path in sorted(root.rglob("*")):
        if path.is_file() and path != root / "artifact-manifest.json":
            reject_reparse(path)
            records[path.relative_to(root).as_posix()] = {"bytes": path.stat().st_size, "sha256": sha256(path)}
    write_json(root / "artifact-manifest.json", {"schemaVersion": 1, "files": records})


def verify_seal(root: Path) -> None:
    manifest = read_json(root / "artifact-manifest.json")
    records = manifest.get("files", {})
    if manifest.get("schemaVersion") != 1 or not isinstance(records, dict) or not records:
        raise EvidenceError("empty/invalid artifact manifest")
    actual = {path.relative_to(root).as_posix() for path in root.rglob("*")
              if path.is_file() and path != root / "artifact-manifest.json"}
    if actual != set(records):
        raise EvidenceError("artifact manifest file set mismatch")
    for relative, record in records.items():
        path = artifact_path(root, relative)
        if path.stat().st_size != record.get("bytes") or sha256(path) != record.get("sha256"):
            raise EvidenceError(f"artifact manifest mismatch: {relative}")


def collect(executable: Path, output: Path, source_root: Path, width: int, height: int, timeout: float) -> dict:
    executable = executable.resolve(strict=True)
    source_root = source_root.resolve(strict=True)
    expected_presets = presets(width, height)
    for params in expected_presets.values():
        check_params(params)
    binding = source_binding(source_root)
    new_directory(output)
    write_json(output / "source-binding.json", binding)
    suite = {"schemaVersion": 1, "createdUtc": utc_now(), "collectionComplete": False,
             "executable": str(executable), "sourceBindingSha256": sha256(output / "source-binding.json"),
             "runs": {}, "negativeTests": []}
    try:
        for name, params in expected_presets.items():
            run_dir = output / name
            launch = run_process(command(executable, params, run_dir), output / (name + ".log"), timeout)
            launch.update({"params": params, "result": name + "/result.json"})
            suite["runs"][name] = launch
            if launch["returncode"] != 0:
                raise EvidenceError(f"native run {name} failed with {launch['returncode']}")
            run = validate_run(run_dir, params, suite.get("identity"), launch["startedEpoch"])
            for role in IDENTITY_ROLES:
                record = run["result"]["identity"][role]
                actual_path = Path(record.get("path", ""))
                if not actual_path.is_absolute() or not actual_path.is_file() or sha256(actual_path) != record["sha256"]:
                    raise EvidenceError(f"runtime identity does not match actual file: {role}")
            if "identity" not in suite:
                suite["identity"] = {role: run["result"]["identity"][role]["sha256"] for role in IDENTITY_ROLES}
                if suite["identity"]["executable"] != sha256(executable):
                    raise EvidenceError("runtime executable hash does not match launched EXE")
        negative_cases = {"unknown-option": ["--unknown-showcase-option", "1"],
                          "bad-light-size": ["--light-size", "-1"], "bad-samples": ["--samples", "0"],
                          "non-finite-bias": ["--bias", "nan"]}
        for name, extra in negative_cases.items():
            target = output / ("negative-" + name)
            args = [str(executable), "--output", str(target), *extra]
            launch = run_process(args, output / ("negative-" + name + ".log"), timeout)
            complete = target.exists() and (target / "result.json").exists() and read_json(target / "result.json").get("status") == "complete"
            diagnostic = (output / launch["log"]).read_text(encoding="utf-8", errors="replace").strip()
            suite["negativeTests"].append({"name": name, **launch,
                                            "diagnostic": diagnostic[:2048],
                                            "pass": launch["returncode"] != 0 and not complete and bool(diagnostic)})
        # 已有目录负例必须证明原始附件零触碰。
        existing = output / "hard"
        before = {path.name: sha256(path) for path in existing.iterdir() if path.is_file()}
        launch = run_process(command(executable, expected_presets["hard"], existing), output / "negative-existing-output.log", timeout)
        after = {path.name: sha256(path) for path in existing.iterdir() if path.is_file()}
        suite["negativeTests"].append({"name": "existing-output", **launch,
                                        "pass": launch["returncode"] != 0 and before == after})
        if source_binding(source_root) != binding:
            raise EvidenceError("source changed during collection; evidence cannot bind one source identity")
        if sha256(executable) != suite["identity"]["executable"]:
            raise EvidenceError("executable changed during collection")
        suite["collectionComplete"] = True
        suite["finishedUtc"] = utc_now()
        write_json(output / "suite.json", suite)
        report = analyze(output, output / "analysis")
        seal(output)
        return report
    except Exception as error:
        if not (output / "suite.json").exists():
            suite["failure"] = str(error)
            write_json(output / "suite.json", suite)
        write_json(output / "failure.json", {"status": "FAIL", "error": str(error), "createdUtc": utc_now()})
        raise


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    gather = commands.add_parser("collect", help="串行运行精选预设和负例；必须先取得 GPU 使用权")
    gather.add_argument("--exe", required=True, type=Path)
    gather.add_argument("--output", required=True, type=Path)
    gather.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    gather.add_argument("--width", type=int, default=960)
    gather.add_argument("--height", type=int, default=600)
    gather.add_argument("--timeout", type=float, default=180.)
    inspect = commands.add_parser("analyze", help="只读既有采集并写入新分析目录")
    inspect.add_argument("--input", required=True, type=Path)
    inspect.add_argument("--output", required=True, type=Path)
    return result


def main(argv=None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "collect":
            report = collect(args.exe, args.output, args.source_root, args.width, args.height, args.timeout)
        else:
            report = analyze(args.input, args.output)
        print(json.dumps({"status": report["status"], "output": str(args.output)}, ensure_ascii=False))
        return 0 if report["status"] == "PASS" else 1
    except (EvidenceError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
