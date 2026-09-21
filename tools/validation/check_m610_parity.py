#!/usr/bin/env python3
"""M6-10 RHI and legacy parity evidence checker.

The checker is dependency free. Missing or incomplete evidence is BLOCKED;
logical structure, semantic trace, or image threshold mismatches are FAIL.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import struct
import sys
import zlib
from pathlib import Path
from typing import Any

RHI_REQUIRED_FILES = (
    "color.ppm", "metadata.json", "m6-framegraph.json", "m6-access-plan.json",
    "m6-transient-plan.json", "m6-framegraph.dot", "semantic-trace.txt",
    "native-trace.txt", "frames.csv",
)
REQUIRED_FILES = RHI_REQUIRED_FILES
IDENTITY_KEYS = (
    "scene", "width", "height", "fixedTick", "assetManifestSha256",
    "environmentArtifactSha256", "shaderSemanticSha256", "visibleSequenceHash",
    "cameraValues", "lightValues", "debugView", "exposureEv", "iblProfile",
    "iblEnabled", "toneMapper", "culling", "shadow", "skyboxEnabled",
)
REQUIRED_METADATA_KEYS = IDENTITY_KEYS + (
    "renderer", "backend", "migrationLevel", "graphHash", "commandHash",
    "warningErrors", "fallbackUsed",
)
LEGACY_FILES = ("color.png", "color.png.json")
LEGACY_IDENTITY_KEYS = (
    "scene", "fixedTick", "assetManifestSha256", "environmentArtifactSha256",
    "shaderSemanticSha256",
    "cameraValues", "lightValues", "exposureEv", "iblProfile", "shadow",
    "visibleSequenceHash", "resolution", "debugView", "toneMapper", "culling",
    "skyboxEnabled", "pngSha256",
)
LEGACY_OPTIONAL_KEYS = ("iblEnabled",)
HASH_KEYS = {"graphHash", "commandHash", "planHash", "hash", "sha256"}
VOLATILE_KEYS = {"frame", "serial", "timestamp", "build", "commit", "backendSource"}
TRUNCATION_RE = re.compile(r"(?:trace[-_ ]?truncated|truncated|capture[-_ ]?cut|output[-_ ]?cut)", re.I)
HEX64_RE = re.compile(r"^[0-9a-fA-F]{64}$")
GRAPH_HASH_RE = re.compile(r"^(?:0x)?[0-9a-fA-F]{16}$")


class EvidenceError(ValueError):
    """Malformed or missing evidence."""


def _pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in items:
        if key in result:
            raise EvidenceError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_pairs)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"invalid JSON: {path}: {error}") from error


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _read_token(data: bytes, offset: int) -> tuple[bytes, int]:
    while offset < len(data):
        if data[offset] in b" \t\r\n":
            offset += 1
            continue
        if data[offset] == ord("#"):
            newline = data.find(b"\n", offset)
            if newline < 0:
                raise EvidenceError("PPM comment is not terminated")
            offset = newline + 1
            continue
        break
    start = offset
    while offset < len(data) and data[offset] not in b" \t\r\n#":
        offset += 1
    if start == offset:
        raise EvidenceError("PPM header is incomplete")
    return data[start:offset], offset


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise EvidenceError(f"cannot read {path}: {error}") from error
    magic, offset = _read_token(data, 0)
    if magic != b"P6":
        raise EvidenceError(f"{path} is not a P6 PPM")
    width_token, offset = _read_token(data, offset)
    height_token, offset = _read_token(data, offset)
    max_token, offset = _read_token(data, offset)
    try:
        width, height, maximum = int(width_token), int(height_token), int(max_token)
    except ValueError as error:
        raise EvidenceError(f"invalid PPM dimensions in {path}") from error
    if width <= 0 or height <= 0 or maximum != 255:
        raise EvidenceError(f"PPM requires positive dimensions and maxval 255: {path}")
    if offset >= len(data) or data[offset] not in b" \t\r\n":
        raise EvidenceError(f"PPM pixel separator is missing: {path}")
    offset += 2 if data[offset:offset + 2] == b"\r\n" else 1
    pixels = data[offset:]
    expected = width * height * 3
    if len(pixels) != expected:
        raise EvidenceError(f"PPM pixel length {len(pixels)} != {expected}: {path}")
    return width, height, pixels


def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)


def write_png(path: Path, width: int, height: int, pixels: bytes) -> None:
    if len(pixels) != width * height * 3:
        raise ValueError("RGB pixel length does not match dimensions")
    rows = b"".join(b"\x00" + pixels[row * width * 3:(row + 1) * width * 3] for row in range(height))
    content = b"\x89PNG\r\n\x1a\n"
    content += _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    content += _png_chunk(b"IDAT", zlib.compress(rows, 9))
    content += _png_chunk(b"IEND", b"")
    path.write_bytes(content)


def read_png(path: Path) -> tuple[int, int, bytes]:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise EvidenceError(f"cannot read {path}: {error}") from error
    signature = b"\x89PNG\r\n\x1a\n"
    if not data.startswith(signature):
        raise EvidenceError(f"{path} is not a PNG")
    offset = len(signature)
    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    saw_iend = False
    while offset + 12 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        end = offset + length + 12
        if end > len(data):
            raise EvidenceError(f"truncated PNG chunk in {path}")
        payload = data[offset + 8:offset + 8 + length]
        crc = struct.unpack(">I", data[offset + 8 + length:end])[0]
        if zlib.crc32(kind + payload) & 0xFFFFFFFF != crc:
            raise EvidenceError(f"invalid PNG chunk CRC in {path}")
        if kind == b"IHDR":
            if length != 13:
                raise EvidenceError(f"invalid PNG IHDR in {path}")
            width, height, bit_depth, color_type, compression, filter_method, interlace = struct.unpack(">IIBBBBB", payload)
            if not width or not height or bit_depth != 8 or color_type not in (2, 6) or compression or filter_method or interlace:
                raise EvidenceError(f"unsupported PNG format in {path}")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            saw_iend = True
            break
        offset = end
    if width is None or height is None or not compressed or not saw_iend:
        raise EvidenceError(f"PNG has no complete image payload: {path}")
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    try:
        raw = zlib.decompress(bytes(compressed))
    except zlib.error as error:
        raise EvidenceError(f"invalid PNG image data: {path}") from error
    expected = height * (stride + 1)
    if len(raw) != expected:
        raise EvidenceError(f"PNG scanline length {len(raw)} != {expected}: {path}")
    rows: list[bytes] = []
    offset = 0
    for _ in range(height):
        filter_type = raw[offset]
        encoded = raw[offset + 1:offset + 1 + stride]
        offset += stride + 1
        previous = rows[-1] if rows else bytes(stride)
        row = bytearray(stride)
        for index, value in enumerate(encoded):
            left = row[index - channels] if index >= channels else 0
            up = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = up
            elif filter_type == 3:
                predictor = (left + up) // 2
            elif filter_type == 4:
                estimate = left + up - upper_left
                pa, pb, pc = abs(estimate - left), abs(estimate - up), abs(estimate - upper_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else upper_left
            else:
                raise EvidenceError(f"unsupported PNG filter {filter_type}: {path}")
            row[index] = (value + predictor) & 0xFF
        rows.append(bytes(row))
    pixels = b"".join(row[index:index + 3] for row in rows for index in range(0, stride, channels))
    return int(width), int(height), pixels


def percentile(values: list[int], fraction: float) -> float:
    if not values:
        return 0.0
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lower, upper = math.floor(position), math.ceil(position)
    if lower == upper:
        return float(values[lower])
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def image_metrics(left: tuple[int, int, bytes], right: tuple[int, int, bytes], threshold8: float) -> dict[str, Any]:
    width, height, left_pixels = left
    right_width, right_height, right_pixels = right
    if (width, height) != (right_width, right_height):
        return {"status": "FAIL", "reason": "resolution mismatch", "left": [width, height], "right": [right_width, right_height]}
    if len(left_pixels) != width * height * 3 or len(right_pixels) != width * height * 3:
        raise EvidenceError("RGB byte count does not match resolution")
    if left_pixels == right_pixels:
        return {"status": "PASS", "resolution": [width, height], "mae8": 0.0, "mae": 0.0,
                "p99Error8": 0.0, "p99Error": 0.0, "changedRateAboveP99": 0.0,
                "changedPixels": 0, "pixelCount": width * height, "firstDifference": None}
    channel_error = 0
    pixel_errors: list[int] = []
    first: dict[str, Any] | None = None
    for index in range(0, len(left_pixels), 3):
        channels = [abs(left_pixels[index + channel] - right_pixels[index + channel]) for channel in range(3)]
        maximum = max(channels)
        channel_error += sum(channels)
        pixel_errors.append(maximum)
        if first is None and maximum > 0:
            pixel = index // 3
            first = {"x": pixel % width, "y": pixel // width, "left": list(left_pixels[index:index + 3]), "right": list(right_pixels[index:index + 3]), "maxError8": maximum}
    mae8 = channel_error / len(left_pixels) if left_pixels else 0.0
    p99_8 = percentile(pixel_errors, 0.99)
    changed = sum(error > threshold8 for error in pixel_errors)
    changed_rate = changed / len(pixel_errors) if pixel_errors else 0.0
    return {"status": "PASS", "resolution": [width, height], "mae8": mae8, "mae": mae8 / 255.0, "p99Error8": p99_8, "p99Error": p99_8 / 255.0, "changedRateAboveP99": changed_rate, "changedPixels": changed, "pixelCount": len(pixel_errors), "firstDifference": first}


def _normalise(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: _normalise(item) for key, item in sorted(value.items()) if key not in HASH_KEYS and key not in VOLATILE_KEYS}
    if isinstance(value, list):
        return [_normalise(item) for item in value]
    return value


def first_difference(left: Any, right: Any, path: str = "$") -> dict[str, Any] | None:
    left, right = _normalise(left), _normalise(right)
    if type(left) is not type(right):
        return {"path": path, "left": left, "right": right}
    if isinstance(left, dict):
        for key in sorted(set(left) | set(right)):
            if key not in left or key not in right:
                return {"path": f"{path}.{key}", "left": left.get(key), "right": right.get(key)}
            difference = first_difference(left[key], right[key], f"{path}.{key}")
            if difference:
                return difference
        return None
    if isinstance(left, list):
        if len(left) != len(right):
            return {"path": f"{path}.length", "left": len(left), "right": len(right)}
        for index, (left_item, right_item) in enumerate(zip(left, right)):
            difference = first_difference(left_item, right_item, f"{path}[{index}]")
            if difference:
                return difference
        return None
    return None if left == right else {"path": path, "left": left, "right": right}


def _nonempty(value: Any) -> bool:
    return value is not None and value != "" and value != [] and value != {}


def _valid_sha(value: Any) -> bool:
    return isinstance(value, str) and bool(HEX64_RE.fullmatch(value))


def _trace_bytes(value: str | bytes) -> bytes:
    if isinstance(value, bytes):
        return value
    try:
        return value.encode("latin-1")
    except UnicodeEncodeError as error:
        raise EvidenceError("semantic trace carrier is not lossless latin-1") from error


def _check_trace(path: Path, label: str) -> str:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise EvidenceError(f"cannot read {label}: {error}") from error
    # PipelineKey is a length-prefixed binary field in the native trace. Keep a
    # one-to-one latin-1 carrier; NUL and non-UTF-8 bytes are valid payload.
    text = data.decode("latin-1")
    if not data or not data.strip(b" \t\r\n"):
        raise EvidenceError(f"empty {label}")
    if TRUNCATION_RE.search(text):
        raise EvidenceError(f"truncated {label}")
    if data.count(b"\n") < 1:
        raise EvidenceError(f"{label} has no complete event sequence")
    return text


def _check_frames(path: Path) -> None:
    try:
        raw = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise EvidenceError(f"cannot read frames.csv: {error}") from error
    if not raw.strip() or not raw.endswith(("\n", "\r")):
        raise EvidenceError("frames.csv is empty or truncated")
    rows = list(csv.DictReader(raw.splitlines()))
    if not rows or not rows[0] or "frame" not in rows[0]:
        raise EvidenceError("frames.csv has no frame column/data")
    if any(any(TRUNCATION_RE.search(str(value or "")) for value in row.values()) for row in rows):
        raise EvidenceError("frames.csv contains a truncation marker")


def _has_graph_requirement(graph: dict[str, Any], name: str) -> bool:
    if name in _pass_names(graph):
        return True
    # Present is the graph epilogue contract in the current runner. It is
    # represented by BackBuffer.finalAccess=Present rather than a pass declaration.
    if name == "Present":
        return any(
            isinstance(item, dict)
            and item.get("name") == "BackBuffer"
            and item.get("finalAccess") == "Present"
            for item in graph.get("resources", [])
        )
    return False


def _pass_names(graph: Any) -> list[str]:
    passes = graph.get("passes", graph.get("nodes", [])) if isinstance(graph, dict) else []
    if not isinstance(passes, list):
        return []
    names: list[str] = []
    for item in passes:
        if isinstance(item, str):
            names.append(item)
        elif isinstance(item, dict):
            for key in ("name", "pass", "label", "id"):
                if isinstance(item.get(key), str):
                    names.append(item[key])
                    break
    return names


def _normalise_graph_hash(value: Any) -> str | None:
    if not isinstance(value, str):
        return None
    value = value.strip().lower()
    if value.startswith("0x"):
        value = value[2:]
    return value if re.fullmatch(r"[0-9a-f]{16}", value) else None


def _graph_hash_basis(metadata: dict[str, Any], graph: Any, access: Any, transient: Any) -> str:
    metadata_hash = _normalise_graph_hash(metadata.get("graphHash"))
    if metadata_hash is None:
        raise EvidenceError("metadata.graphHash is not a 64-bit GraphDump planHash")
    for name, dump, kind in (("m6-framegraph.json", graph, "framegraph"), ("m6-access-plan.json", access, "access-plan"), ("m6-transient-plan.json", transient, "transient-plan")):
        if not isinstance(dump, dict):
            raise EvidenceError(f"{name} must be a JSON object")
        if dump.get("schemaVersion") != 1 or dump.get("kind") != kind:
            raise EvidenceError(f"{name} has an invalid GraphDump header")
        plan_hash = _normalise_graph_hash(dump.get("planHash"))
        if plan_hash is None:
            raise EvidenceError(f"{name}.planHash is not a 64-bit hex value")
        if plan_hash != metadata_hash:
            raise EvidenceError(f"{name}.planHash does not match metadata.graphHash")
    return "graph-dump-planHash"


def _validate_graph_schema(graph: dict[str, Any], access: dict[str, Any], transient: dict[str, Any]) -> None:
    required_graph = {"statistics", "stages", "executionOrder", "passes", "resources", "versions", "edges", "roots", "lifetimes", "physicalAllocations", "transitions", "importOwnership"}
    absent = sorted(required_graph - set(graph))
    if absent:
        raise EvidenceError("m6-framegraph.json missing fields: " + ", ".join(absent))
    if not all(isinstance(graph.get(name), (dict, list)) for name in required_graph):
        raise EvidenceError("m6-framegraph.json has invalid field types")
    if not isinstance(access.get("transitions"), list):
        raise EvidenceError("m6-access-plan.json.transitions must be an array")
    for name in ("lifetimes", "physicalAllocations"):
        if not isinstance(transient.get(name), list):
            raise EvidenceError(f"m6-transient-plan.json.{name} must be an array")
    if not all(isinstance(item, dict) and isinstance(item.get("name"), str) for item in graph["resources"]):
        raise EvidenceError("m6-framegraph.json.resources has an invalid entry")
    if not all(isinstance(item, dict) and isinstance(item.get("name"), str) for item in graph["passes"]):
        raise EvidenceError("m6-framegraph.json.passes has an invalid entry")


def inspect_artifact(directory: Path, recipe: dict[str, Any] | None = None, manifest_sha256: str | None = None) -> dict[str, Any]:
    directory = directory.resolve()
    try:
        missing = [name for name in RHI_REQUIRED_FILES if not (directory / name).is_file()]
        if missing:
            raise EvidenceError("missing evidence: " + ", ".join(missing))
        metadata = load_json(directory / "metadata.json")
        graph = load_json(directory / "m6-framegraph.json")
        access = load_json(directory / "m6-access-plan.json")
        transient = load_json(directory / "m6-transient-plan.json")
        if not isinstance(metadata, dict):
            raise EvidenceError("metadata.json must be an object")
        absent = [key for key in REQUIRED_METADATA_KEYS if key not in metadata or not _nonempty(metadata[key])]
        if absent:
            raise EvidenceError("metadata identity is incomplete: " + ", ".join(absent))
        problems: list[str] = []
        if metadata["renderer"] != "rhi" or metadata["fallbackUsed"] is not False:
            problems.append("renderer/fallbackUsed contract failed")
        if type(metadata["warningErrors"]) is not int or metadata["warningErrors"] != 0:
            problems.append(f"warningErrors={metadata['warningErrors']}")
        for key in ("assetManifestSha256", "environmentArtifactSha256", "shaderSemanticSha256", "commandHash"):
            if not _valid_sha(metadata[key]):
                raise EvidenceError(f"metadata.{key} is not a SHA-256 hex string")
        if _normalise_graph_hash(metadata["graphHash"]) is None:
            raise EvidenceError("metadata.graphHash is not a 64-bit GraphDump planHash")
        if manifest_sha256 and metadata["assetManifestSha256"].lower() != manifest_sha256.lower():
            raise EvidenceError("assetManifestSha256 does not match supplied manifest")
        if not isinstance(graph, dict) or not isinstance(access, dict) or not isinstance(transient, dict):
            raise EvidenceError("M6 dump roots must be JSON objects")
        _validate_graph_schema(graph, access, transient)
        graph_hash_basis = _graph_hash_basis(metadata, graph, access, transient)
        semantic_trace = _check_trace(directory / "semantic-trace.txt", "semantic-trace.txt")
        _check_trace(directory / "native-trace.txt", "native-trace.txt")
        semantic_trace_bytes = _trace_bytes(semantic_trace)
        command_hash = sha256_bytes(semantic_trace_bytes)
        if metadata["commandHash"].lower() != command_hash.lower():
            raise EvidenceError("commandHash does not match semantic-trace.txt SHA-256")
        _check_frames(directory / "frames.csv")
        image = read_ppm(directory / "color.ppm")
        if int(metadata["width"]) != image[0] or int(metadata["height"]) != image[1]:
            raise EvidenceError("metadata resolution does not match color.ppm")
        resolution = metadata.get("resolution")
        if resolution not in ({"width": image[0], "height": image[1]}, [image[0], image[1]]):
            raise EvidenceError("metadata resolution object does not match color.ppm")
        try:
            dot = (directory / "m6-framegraph.dot").read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            raise EvidenceError(f"cannot read m6-framegraph.dot: {error}") from error
        if "digraph FrameGraph" not in dot or len(dot.strip().splitlines()) < 2:
            raise EvidenceError("m6-framegraph.dot is empty or incomplete")
        if recipe:
            expected_scene = recipe.get("scene")
            if expected_scene and metadata.get("scene") != expected_scene:
                problems.append("metadata scene does not match the parity recipe")
            absent_passes = [name for name in recipe.get("requiredPasses", []) if not _has_graph_requirement(graph, name)]
            if absent_passes:
                problems.append("required graph passes missing: " + ", ".join(absent_passes))
            names = {item.get("name") for item in graph.get("resources", []) if isinstance(item, dict)}
            absent_resources = [name for name in recipe.get("transientResources", []) if name not in names]
            if absent_resources:
                problems.append("required graph resources missing: " + ", ".join(absent_resources))
        png_path = directory / "color.png"
        if not png_path.exists():
            write_png(png_path, image[0], image[1], image[2])
        return {"status": "FAIL" if problems else "PASS", "directory": str(directory), "metadata": metadata, "graph": graph, "access": access, "transient": transient, "semanticTrace": semantic_trace, "semanticTraceSha256": command_hash, "semanticTraceBytes": len(semantic_trace_bytes), "image": image, "graphHashBasis": graph_hash_basis, "problems": problems}
    except EvidenceError as error:
        return {"status": "BLOCKED", "directory": str(directory), "problems": [str(error)]}
    except (OSError, TypeError, ValueError, OverflowError) as error:
        return {"status": "BLOCKED", "directory": str(directory), "problems": [str(error)]}


def _legacy_scene_matches_recipe(value: Any, recipe_scene: Any) -> bool:
    if value == recipe_scene:
        return True
    if not isinstance(value, str) or not isinstance(recipe_scene, str):
        return False
    path = value.replace("\\", "/").lower().rstrip("/")
    return path.endswith("/scene/manifest.json") and recipe_scene == "m4-visual-baseline"


def inspect_legacy_artifact(directory: Path, recipe: dict[str, Any] | None = None, manifest_sha256: str | None = None) -> dict[str, Any]:
    directory = directory.resolve()
    try:
        missing = [name for name in LEGACY_FILES if not (directory / name).is_file()]
        if missing:
            raise EvidenceError("missing legacy evidence: " + ", ".join(missing))
        metadata = load_json(directory / "color.png.json")
        if not isinstance(metadata, dict):
            raise EvidenceError("color.png.json must be an object")
        absent = [key for key in LEGACY_IDENTITY_KEYS if key not in metadata or not _nonempty(metadata[key])]
        if absent:
            raise EvidenceError("legacy identity is incomplete: " + ", ".join(absent))
        image = read_png(directory / "color.png")
        resolution = metadata["resolution"]
        if isinstance(resolution, dict):
            expected_resolution = [resolution.get("width"), resolution.get("height")]
        else:
            expected_resolution = resolution
        if expected_resolution != [image[0], image[1]]:
            raise EvidenceError("legacy metadata resolution does not match color.png")
        png_hash = metadata["pngSha256"]
        if not _valid_sha(png_hash):
            raise EvidenceError("legacy metadata.pngSha256 is not a SHA-256 hex string")
        actual_hash = sha256_bytes((directory / "color.png").read_bytes())
        if png_hash.lower() != actual_hash.lower():
            raise EvidenceError("pngSha256 does not match color.png")
        if manifest_sha256 and metadata["assetManifestSha256"].lower() != manifest_sha256.lower():
            raise EvidenceError("legacy assetManifestSha256 does not match supplied manifest")
        problems: list[str] = []
        if recipe:
            if not _legacy_scene_matches_recipe(metadata.get("scene"), recipe.get("scene")):
                problems.append("legacy scene does not match the parity recipe")
        return {"status": "FAIL" if problems else "PASS", "directory": str(directory), "metadata": metadata, "image": image, "optionalFieldsMissing": [key for key in LEGACY_OPTIONAL_KEYS if key not in metadata], "problems": problems}
    except EvidenceError as error:
        return {"status": "BLOCKED", "directory": str(directory), "problems": [str(error)]}
    except (OSError, TypeError, ValueError, OverflowError) as error:
        return {"status": "BLOCKED", "directory": str(directory), "problems": [str(error)]}


def _line_difference(left: str | bytes, right: str | bytes) -> dict[str, Any] | None:
    left_bytes, right_bytes = _trace_bytes(left), _trace_bytes(right)
    if left_bytes == right_bytes:
        return None
    limit = min(len(left_bytes), len(right_bytes))
    offset = next((i for i in range(limit) if left_bytes[i] != right_bytes[i]), limit)
    left_line_start = left_bytes.rfind(b"\n", 0, offset) + 1
    right_line_start = right_bytes.rfind(b"\n", 0, offset) + 1
    left_line_end = left_bytes.find(b"\n", offset)
    right_line_end = right_bytes.find(b"\n", offset)
    if left_line_end < 0:
        left_line_end = len(left_bytes)
    if right_line_end < 0:
        right_line_end = len(right_bytes)
    return {"line": left_bytes.count(b"\n", 0, offset) + 1, "byteOffset": offset, "leftByte": left_bytes[offset] if offset < len(left_bytes) else None, "rightByte": right_bytes[offset] if offset < len(right_bytes) else None, "left": left_bytes[left_line_start:left_line_end].decode("latin-1"), "right": right_bytes[right_line_start:right_line_end].decode("latin-1")}


def _thresholds(recipe: dict[str, Any] | None) -> tuple[float, float, float, int]:
    values = (recipe or {}).get("imageThresholds", {})
    mae = float(values.get("mae", 2.0 / 255.0))
    p99 = float(values.get("p99", 5.0 / 255.0))
    changed = float(values.get("changedRateAboveP99", 0.005))
    return mae, p99, changed, round(p99 * 255.0)


def _image_failure(image: dict[str, Any], recipe: dict[str, Any] | None) -> bool:
    mae, p99, changed, _ = _thresholds(recipe)
    return image.get("status") != "PASS" or image.get("mae", 0.0) > mae or image.get("p99Error", 0.0) > p99 or image.get("changedRateAboveP99", 0.0) > changed


def compare_artifacts(left: dict[str, Any], right: dict[str, Any], recipe: dict[str, Any] | None = None, *, require_distinct_backend: bool = True) -> dict[str, Any]:
    if left.get("status") == "BLOCKED" or right.get("status") == "BLOCKED":
        return {"status": "BLOCKED", "reason": "artifact evidence is incomplete", "left": left, "right": right}
    if left.get("status") != "PASS" or right.get("status") != "PASS":
        return {"status": "FAIL", "reason": "artifact contract failed", "leftProblems": left.get("problems", []), "rightProblems": right.get("problems", [])}
    failures: list[str] = []
    identity_differences: list[dict[str, Any]] = []
    left_meta, right_meta = left["metadata"], right["metadata"]
    if require_distinct_backend and left_meta.get("backend") == right_meta.get("backend"):
        identity_differences.append({"key": "backend", "left": left_meta.get("backend"), "right": right_meta.get("backend")})
    for key in IDENTITY_KEYS + ("migrationLevel", "scene", "renderer", "graphHash", "commandHash"):
        if left_meta.get(key) != right_meta.get(key):
            identity_differences.append({"key": key, "left": left_meta.get(key), "right": right_meta.get(key)})
    structure_differences: list[dict[str, Any]] = []
    for name in ("graph", "access", "transient"):
        difference = first_difference(left[name], right[name])
        if difference:
            structure_differences.append({"dump": name, **difference})
    trace_difference = _line_difference(left["semanticTrace"], right["semanticTrace"])
    if structure_differences:
        failures.append("logical graph/access/transient structure differs")
    if trace_difference:
        failures.append("semantic command trace differs")
    _, _, _, threshold8 = _thresholds(recipe)
    image = image_metrics(left["image"], right["image"], threshold8)
    if _image_failure(image, recipe):
        failures.append("image thresholds exceeded")
    if structure_differences or trace_difference:
        status = "FAIL"
    elif identity_differences:
        status = "BLOCKED"
    elif "image thresholds exceeded" in failures:
        status = "FAIL"
    else:
        status = "PASS"
    first = structure_differences[0] if structure_differences else trace_difference if trace_difference else identity_differences[0] if identity_differences else image.get("firstDifference")
    return {"status": status, "failures": failures, "identityDifferences": identity_differences, "structureDifferences": structure_differences, "traceDifference": trace_difference, "image": image, "firstDifference": first}


def compare_legacy_to_rhi(rhi: dict[str, Any], legacy: dict[str, Any], recipe: dict[str, Any] | None = None) -> dict[str, Any]:
    if rhi.get("status") == "BLOCKED" or legacy.get("status") == "BLOCKED":
        return {"status": "BLOCKED", "reason": "legacy or RHI evidence is incomplete", "rhi": rhi, "legacy": legacy}
    if rhi.get("status") != "PASS" or legacy.get("status") != "PASS":
        return {"status": "FAIL", "reason": "legacy or RHI artifact contract failed", "rhiProblems": rhi.get("problems", []), "legacyProblems": legacy.get("problems", [])}
    rhi_meta, legacy_meta = rhi["metadata"], legacy["metadata"]
    identity_differences: list[dict[str, Any]] = []
    expected_backend = str(rhi_meta.get("backend", "")).lower()
    actual_backend = str(legacy_meta.get("backend", "")).lower().replace("direct3d", "d3d")
    if actual_backend != expected_backend:
        identity_differences.append({"key": "backend", "rhi": rhi_meta.get("backend"), "legacy": legacy_meta.get("backend")})
    for key in LEGACY_IDENTITY_KEYS:
        if key in {"pngSha256", "scene", "resolution"}:
            continue
        if rhi_meta.get(key) != legacy_meta.get(key):
            identity_differences.append({"key": key, "rhi": rhi_meta.get(key), "legacy": legacy_meta.get(key)})
    expected_scene = (recipe or {}).get("scene", rhi_meta.get("scene"))
    if not _legacy_scene_matches_recipe(legacy_meta.get("scene"), expected_scene):
        identity_differences.append({"key": "scene", "rhi": rhi_meta.get("scene"), "legacy": legacy_meta.get("scene")})
    elif recipe and rhi_meta.get("scene") != recipe.get("scene"):
        identity_differences.append({"key": "scene", "rhi": rhi_meta.get("scene"), "legacy": legacy_meta.get("scene")})
    rhi_resolution = [rhi_meta.get("width"), rhi_meta.get("height")]
    legacy_resolution = legacy_meta.get("resolution")
    if isinstance(legacy_resolution, dict):
        legacy_resolution = [legacy_resolution.get("width"), legacy_resolution.get("height")]
    if legacy_resolution != rhi_resolution:
        identity_differences.append({"key": "resolution", "rhi": rhi_resolution, "legacy": legacy_resolution})
    optional_missing: list[str] = []
    if "iblEnabled" not in legacy_meta:
        optional_missing.append("iblEnabled")
    elif legacy_meta["iblEnabled"] != rhi_meta.get("iblEnabled"):
        identity_differences.append({"key": "iblEnabled", "rhi": rhi_meta.get("iblEnabled"), "legacy": legacy_meta.get("iblEnabled")})
    _, _, _, threshold8 = _thresholds(recipe)
    image = image_metrics(rhi["image"], legacy["image"], threshold8)
    failures = ["image thresholds exceeded"] if _image_failure(image, recipe) else []
    if identity_differences:
        status = "BLOCKED"
    elif failures:
        status = "FAIL"
    else:
        status = "PASS"
    return {"status": status, "failures": failures, "identityDifferences": identity_differences, "optionalLegacyFieldsMissing": optional_missing, "image": image, "firstDifference": identity_differences[0] if identity_differences else image.get("firstDifference")}


def compare_legacy_artifact(rhi: dict[str, Any], legacy: dict[str, Any], recipe: dict[str, Any] | None = None) -> dict[str, Any]:
    return compare_legacy_to_rhi(rhi, legacy, recipe)


def load_recipe(path: Path) -> dict[str, Any]:
    recipe = load_json(path)
    if not isinstance(recipe, dict):
        raise EvidenceError("recipe must be an object")
    return recipe


def _summary(value: Any) -> Any:
    """Keep JSON evidence compact while preserving nested failure details."""
    if isinstance(value, dict):
        return {key: _summary(item) for key, item in value.items()
                if key not in {"graph", "access", "transient", "semanticTrace", "semanticTraceBytes"}
                and not (key == "image" and isinstance(item, tuple))}
    if isinstance(value, (list, tuple)):
        return [_summary(item) for item in value]
    if isinstance(value, bytes):
        return {"sha256": sha256_bytes(value), "bytes": len(value)}
    return value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path, help="two RHI directories, or one root containing m610-run.json")
    parser.add_argument("--recipe", type=Path, default=Path("assets/recipes/m6-graph-baseline.json"))
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        recipe_path = args.recipe if args.recipe.is_absolute() else Path.cwd() / args.recipe
        recipe = load_recipe(recipe_path.resolve())
        manifest_hash = None
        if args.manifest:
            manifest_path = args.manifest if args.manifest.is_absolute() else Path.cwd() / args.manifest
            manifest_hash = sha256_bytes(manifest_path.read_bytes())
        directories = [path if path.is_absolute() else Path.cwd() / path for path in args.directories]
        if len(directories) == 1 and (directories[0] / "m610-run.json").is_file():
            run = load_json(directories[0] / "m610-run.json")
            result = {"status": run.get("status", "BLOCKED"), "run": run}
        elif len(directories) == 2:
            left, right = (inspect_artifact(path.resolve(), recipe, manifest_hash) for path in directories)
            result = compare_artifacts(left, right, recipe)
        else:
            raise ValueError("provide two RHI directories or one parity-run root")
        output = args.output
        if output:
            output_path = output if output.is_absolute() else Path.cwd() / output
            output_path.write_text(json.dumps(_summary(result), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(_summary(result), ensure_ascii=False, indent=2))
        return 0 if result.get("status") == "PASS" else 1 if result.get("status") == "FAIL" else 2
    except (OSError, ValueError, KeyError, TypeError, EvidenceError) as error:
        print(json.dumps({"status": "BLOCKED", "problems": [str(error)]}, ensure_ascii=False, indent=2))
        return 2


if __name__ == "__main__":
    sys.exit(main())
