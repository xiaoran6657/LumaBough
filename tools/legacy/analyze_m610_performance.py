"""校验 M6-10 三轮 Release 数据；输出统计，性能接受结论由验收解释给出。"""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import math
import re
import statistics
from pathlib import Path

IDENTITY = (
    "backend", "assetManifestSha256", "environmentArtifactSha256", "shaderSemanticSha256",
    "cameraValues", "lightValues", "exposureEv", "iblProfile", "shadow", "fixedTick",
    "visibleSequenceHash", "resolution", "debugView", "toneMapper", "culling", "skyboxEnabled",
    "gpu", "driver", "migrationLevel", "sourceCommit",
)
TIMINGS = ("prepareMs", "buildMs", "compileMs", "executeMs", "presentMs", "cpuFrameMs")
COUNTS = (
    "declared", "live", "culled", "virtual", "physical", "physicalTransients", "poolBytes",
    "poolHighWaterBytes", "logicalTransitions", "nativeBarriers", "nativeUnbinds", "rhiCommands",
)


class EvidenceError(ValueError):
    pass


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    index = (len(ordered) - 1) * probability
    lower = math.floor(index)
    upper = math.ceil(index)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def load_run(directory: Path) -> dict:
    metadata_path = directory / "metadata.json"
    frames_path = directory / "frames.csv"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8-sig"))
    required = set(IDENTITY) | {
        "buildType", "benchmark", "debugLayer", "gpuValidation", "vsync", "warp",
        "warmupFrames", "measuredFrames", "fallbackUsed", "warningErrors", "renderer", "captureToolActive", "windowForeground", "dredRequested",
    }
    missing = sorted(required - metadata.keys())
    if missing:
        raise EvidenceError(f"{directory}: missing identity {missing}")
    for key in ("assetManifestSha256", "environmentArtifactSha256", "shaderSemanticSha256"):
        if not isinstance(metadata[key], str) or not re.fullmatch(r"[0-9a-fA-F]{64}", metadata[key]):
            raise EvidenceError(f"{directory}: invalid {key}")
    for key in ("cameraValues", "lightValues", "gpu", "driver", "sourceCommit"):
        if not isinstance(metadata[key], str) or not metadata[key].strip():
            raise EvidenceError(f"{directory}: empty {key}")
    if metadata["buildType"] != "Release" or metadata["renderer"] != "rhi":
        raise EvidenceError(f"{directory}: requires the Release RHI path")
    for key in ("debugLayer", "gpuValidation", "vsync", "warp", "fallbackUsed", "captureToolActive", "dredRequested"):
        if metadata[key] is not False:
            raise EvidenceError(f"{directory}: {key} must be false")
    if metadata["windowForeground"] is not True:
        raise EvidenceError(f"{directory}: measured window must remain foreground")
    if metadata["benchmark"] is not True or metadata["warningErrors"] != 0:
        raise EvidenceError(f"{directory}: benchmark flag or native diagnostics are invalid")
    if (metadata["warmupFrames"], metadata["measuredFrames"], metadata["fixedTick"]) != (120, 600, 720):
        raise EvidenceError(f"{directory}: requires 120 warm-up plus 600 measured frames")
    if metadata["migrationLevel"] != 9:
        raise EvidenceError(f"{directory}: benchmark must use the final migration level")
    with frames_path.open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 600:
        raise EvidenceError(f"{directory}: expected 600 measured rows, got {len(rows)}")
    values: dict[str, list[float]] = {key: [] for key in TIMINGS + COUNTS}
    for expected, row in enumerate(rows, 121):
        if int(row["frame"]) != expected or row["measured"] != "1":
            raise EvidenceError(f"{directory}: missing/duplicate/non-measured frame {expected}")
        for key in values:
            value = float(row[key])
            if not math.isfinite(value) or value < 0:
                raise EvidenceError(f"{directory}: invalid {key} at frame {expected}")
            if key in COUNTS and not value.is_integer():
                raise EvidenceError(f"{directory}: non-integral count {key} at frame {expected}")
            values[key].append(value)
        if metadata["backend"] == "d3d11" and int(row["nativeBarriers"]) != 0:
            raise EvidenceError(f"{directory}: D3D11 has no native ResourceBarrier API; use logicalTransitions")
        if int(row["declared"]) != int(row["live"]) + int(row["culled"]):
            raise EvidenceError(f"{directory}: pass-count conservation failed at frame {expected}")
    summary = {
        key: {
            "median": statistics.median(samples), "p95": percentile(samples, 0.95),
            "min": min(samples), "max": max(samples),
        } for key, samples in values.items()
    }
    return {
        "directory": str(directory.resolve()), "metadata": metadata, "statistics": summary,
        "artifacts": {
            "metadata.json": digest(metadata_path), "frames.csv": digest(frames_path),
        },
        "_samples": values,
    }


def analyze(directories: list[Path]) -> dict:
    if len(directories) != 3 or len({p.resolve() for p in directories}) != 3:
        raise EvidenceError("three distinct run directories are required")
    runs = [load_run(path) for path in directories]
    for run in runs[1:]:
        differences = [key for key in IDENTITY if run["metadata"][key] != runs[0]["metadata"][key]]
        if differences:
            raise EvidenceError(f"run identities differ: {differences}")
    combined = {
        key: [value for run in runs for value in run["_samples"][key]]
        for key in TIMINGS + COUNTS
    }
    for run in runs:
        del run["_samples"]
    return {
        "schemaVersion": 1, "status": "PASS_DATA",
        "interpretationRequired": True,
        "note": "三轮数据满足采样条件；此状态不代表已接受相对 M5 的性能变化。",
        "backend": runs[0]["metadata"]["backend"],
        "runs": runs,
        "aggregate": {
            key: {"median": statistics.median(values), "p95": percentile(values, 0.95),
                  "min": min(values), "max": max(values)}
            for key, values in combined.items()
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", type=Path, nargs=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = analyze(args.runs)
    except (EvidenceError, OSError, KeyError, ValueError, TypeError) as error:
        result = {"schemaVersion": 1, "status": "BLOCKED", "reason": str(error)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": result["status"], "output": str(args.output)}, ensure_ascii=False))
    return 0 if result["status"] == "PASS_DATA" else 2


if __name__ == "__main__":
    raise SystemExit(main())
