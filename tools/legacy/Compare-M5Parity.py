#!/usr/bin/env python3
"""Compare two M5 parity PNGs and their sidecar metadata."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image


REQUIRED_FIELDS = (
    "assetManifestSha256",
    "shaderSemanticSha256",
    "environmentArtifactSha256",
    "cameraValues",
    "lightValues",
    "exposureEv",
    "iblProfile",
    "shadow",
    "fixedTick",
    "visibleSequenceHash",
    "resolution",
    "debugView",
    "toneMapper",
    "culling",
    "skyboxEnabled",
    "pngSha256",
)
REQUIRED_HASHES = {"assetManifestSha256", "shaderSemanticSha256", "environmentArtifactSha256", "pngSha256"}
MAE_LIMIT = 2.0
P99_LIMIT = 5.0
CHANGED_RATE_LIMIT = 0.005


def metadata_path(image_path: Path) -> Path:
    return Path(str(image_path) + ".json")


def is_missing(value: object) -> bool:
    return value is None or value == "" or value == [] or value == {}


def read_metadata(path: Path) -> tuple[dict | None, list[str]]:
    if not path.is_file():
        return None, ["metadata"]
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None, ["metadata"]
    if not isinstance(value, dict):
        return None, ["metadata"]
    return value, [name for name in REQUIRED_FIELDS if name not in value or is_missing(value[name])]


def compare_metadata(left: dict | None, right: dict | None, missing_a: list[str], missing_b: list[str]) -> list[str]:
    missing = [f"a.{name}" for name in missing_a] + [f"b.{name}" for name in missing_b]
    if left is None or right is None:
        return sorted(set(missing))
    mismatches = list(missing)
    for name in REQUIRED_FIELDS:
        if name in missing_a or name in missing_b:
            continue
        if name != "pngSha256" and left[name] != right[name]:
            mismatches.append(name)
    return sorted(set(mismatches))


def load_rgb(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.uint8)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def resolution_matches(value: object, image: np.ndarray) -> bool:
    height, width = image.shape[:2]
    if isinstance(value, dict):
        return value.get("width") == width and value.get("height") == height
    return isinstance(value, (list, tuple)) and len(value) == 2 and value == [width, height]


def diff_image(left: np.ndarray, right: np.ndarray | None) -> tuple[np.ndarray, dict]:
    if right is None or left.shape != right.shape:
        return np.zeros_like(left), {"mae": None, "p99Error": None, "changedRate": None}
    error = np.abs(left.astype(np.int16) - right.astype(np.int16))
    per_pixel = error.max(axis=2)
    diff = np.minimum(error * 8, 255).astype(np.uint8)
    return diff, {
        "mae": float(error.mean()),
        "p99Error": float(np.percentile(per_pixel, 99)),
        "changedRate": float(np.mean(per_pixel > 5)),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", required=True, type=Path)
    parser.add_argument("--b", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output
    diff_path = output.with_name(output.stem + ".diff.png")
    output.parent.mkdir(parents=True, exist_ok=True)

    left_meta, missing_a = read_metadata(metadata_path(args.a))
    right_meta, missing_b = read_metadata(metadata_path(args.b))
    mismatches = compare_metadata(left_meta, right_meta, missing_a, missing_b)
    diagnostics = {
        name: {"a": left_meta.get(name) if left_meta else None, "b": right_meta.get(name) if right_meta else None}
        for name in ("backend", "driver", "compiler")
    }

    image_error = None
    try:
        left_image = load_rgb(args.a)
        right_image = load_rgb(args.b)
        if left_meta and not resolution_matches(left_meta.get("resolution"), left_image):
            mismatches.append("a.resolution")
        if right_meta and not resolution_matches(right_meta.get("resolution"), right_image):
            mismatches.append("b.resolution")
        if left_image.shape != right_image.shape:
            image_error = "imageShape"
            diff, metrics = diff_image(left_image, None)
        else:
            diff, metrics = diff_image(left_image, right_image)
        if left_meta and left_meta.get("pngSha256") != sha256_file(args.a):
            mismatches.append("a.pngSha256")
        if right_meta and right_meta.get("pngSha256") != sha256_file(args.b):
            mismatches.append("b.pngSha256")
    except (OSError, ValueError):
        image_error = "image"
        left_image = np.zeros((1, 1, 3), dtype=np.uint8)
        diff, metrics = diff_image(left_image, None)

    if image_error:
        mismatches.append(image_error)
    Image.fromarray(diff, mode="RGB").save(diff_path)
    mismatches = sorted(set(mismatches))
    blocked = bool(mismatches)
    status = "BLOCKED" if blocked else (
        "FAIL" if metrics["mae"] > MAE_LIMIT or metrics["p99Error"] > P99_LIMIT or metrics["changedRate"] > CHANGED_RATE_LIMIT else "PASS"
    )
    result = {
        "status": status,
        "metadataComparable": not blocked,
        "mismatchedFields": mismatches,
        "diagnostics": diagnostics,
        "structuralReview": "required",
        "metrics": metrics,
        "thresholds": {"mae": MAE_LIMIT, "p99Error": P99_LIMIT, "changedRate": CHANGED_RATE_LIMIT},
        "diffPng": str(diff_path),
    }
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))
    return 0 if status == "PASS" else 1 if status == "FAIL" else 2


if __name__ == "__main__":
    sys.exit(main())