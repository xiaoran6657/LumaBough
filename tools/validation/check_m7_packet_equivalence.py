"""M7-05 A15 独立检查：跨 worker/后端比较 packet/graph/command/截图证据。

输入是 run_m7_packet_sweep.ps1 产出的两个短协议批次（每个 run 一个目录，内含
run.json 与 screenshot.rgba）。本脚本不做统计，只做等价性判定：

  1. 批次内（同 RHI、同协议）：所有 run 的
     packetSequenceHash / graphHash / commandHash / screenshotHash 必须逐字节一致；
  2. 跨 RHI：packetSequenceHash 与 graphHash（CPU 侧语义）必须一致；commandHash
     按实测记录（backend 语义 trace 相同则为一致）；截图给出像素差统计而不是位相等
     （M6-12 已确立：跨后端截图允许 ≤1/255 级别的光栅差异）。

输出 JSON 是 A15 的机读摘要；任一硬约束失败即退出码 1。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


def load_runs(directory: Path) -> list[dict]:
    runs = []
    for run_file in sorted(directory.rglob("run.json")):
        raw = json.loads(run_file.read_text(encoding="utf-8"))
        screenshot = run_file.parent / "screenshot.rgba"
        runs.append(
            {
                "path": str(run_file),
                "runDir": str(run_file.parent),
                "rhi": raw["rhi"],
                "packetBuildMode": raw["packetBuildMode"],
                "schedulerMode": raw["schedulerMode"],
                "workers": raw["workers"],
                "chunkSize": raw["chunkSize"],
                "chunkReserve": raw["chunkReserve"],
                "measuredFrames": raw["measuredFrames"],
                "status": raw["correctness"]["status"],
                "packetSequenceHash": raw["correctness"]["packetSequenceHash"],
                "graphHash": raw["correctness"]["graphHash"],
                "commandHash": raw["correctness"]["commandHash"],
                "screenshotHash": raw["correctness"]["screenshotHash"],
                "screenshotFile": str(screenshot) if screenshot.is_file() else "",
            }
        )
    if not runs:
        raise SystemExit(f"no run.json under {directory}")
    return runs


def unique(values: list[str]) -> list[str]:
    return sorted(set(values))


def compare_screenshots(path_a: Path, path_b: Path) -> dict:
    try:
        import numpy as np
    except ImportError:  # pragma: no cover - 环境缺 numpy 时明确失败，不静默跳过
        raise SystemExit("numpy is required for the screenshot comparison")

    a = np.fromfile(path_a, dtype=np.uint8)
    b = np.fromfile(path_b, dtype=np.uint8)
    if a.size != b.size or a.size % 4 != 0:
        raise SystemExit(f"screenshot sizes differ or are not RGBA: {a.size} vs {b.size}")
    a = a.reshape(-1, 4).astype(np.int16)
    b = b.reshape(-1, 4).astype(np.int16)
    diff = np.abs(a - b)
    rgb = diff[:, :3]
    worst = rgb.max(axis=1)
    total = worst.size
    return {
        "pixelCount": int(total),
        "meanAbsoluteError": round(float(rgb.mean()), 6),
        "p99": int(np.percentile(rgb, 99)),
        "maximum": int(rgb.max()),
        "alphaMaximum": int(diff[:, 3].max()),
        "fractionAbove2": round(float((worst > 2).sum()) / total, 6),
        "fractionAbove16": round(float((worst > 16).sum()) / total, 6),
        "expected": "bit-identical only guarantees within one RHI; cross-RHI tolerates raster level differences (M6-12 parity)",
    }


def batch_report(runs: list[dict], label: str) -> dict:
    fields = ["packetSequenceHash", "graphHash", "commandHash", "screenshotHash"]
    identities = {field: unique([run[field] for run in runs]) for field in fields}
    problems = []
    for field, values in identities.items():
        if len(values) != 1 or not values[0]:
            problems.append(f"[{label}] {field} diverges or is empty: {values}")
    for run in runs:
        if run["status"] != "PASS":
            problems.append(f"[{label}] {run['path']} correctness status is not PASS")
        if not run["screenshotFile"]:
            problems.append(f"[{label}] {run['runDir']} has no screenshot.rgba")
    return {
        "label": label,
        "runs": len(runs),
        "rhis": unique([run["rhi"] for run in runs]),
        "workers": sorted({run["workers"] for run in runs}),
        "modes": unique([f"{run['packetBuildMode']}/{run['schedulerMode']}" for run in runs]),
        "measuredFrames": unique([str(run["measuredFrames"]) for run in runs]),
        "identities": {field: values[0] if len(values) == 1 else values for field, values in identities.items()},
        "problems": problems,
    }


def reference_run(runs: list[dict], workers: int, mode: str) -> dict:
    for run in runs:
        if run["workers"] == workers and run["packetBuildMode"] == mode:
            return run
    raise SystemExit(f"missing reference run: workers={workers} mode={mode}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--d3d11", type=Path, required=True)
    parser.add_argument("--d3d12", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    runs_11 = load_runs(args.d3d11)
    runs_12 = load_runs(args.d3d12)
    report = {
        "schema": "miniengine.packet-equivalence.v1",
        "d3d11": batch_report(runs_11, "d3d11"),
        "d3d12": batch_report(runs_12, "d3d12"),
    }
    problems = list(report["d3d11"]["problems"]) + list(report["d3d12"]["problems"])

    # 跨 RHI：CPU 侧语义（packet/graph）必须一致；commandHash 与截图按实测记录。
    a = report["d3d11"]["identities"]
    b = report["d3d12"]["identities"]
    cross = {
        "packetSequenceHashEqual": a["packetSequenceHash"] == b["packetSequenceHash"],
        "graphHashEqual": a["graphHash"] == b["graphHash"],
        "commandHashEqual": a["commandHash"] == b["commandHash"],
        "screenshotHashEqual": a["screenshotHash"] == b["screenshotHash"],
    }
    if not cross["packetSequenceHashEqual"] or not cross["graphHashEqual"]:
        problems.append("cross-RHI packet/graph identity diverged")

    serial_11 = reference_run(runs_11, 1, "serial")
    serial_12 = reference_run(runs_12, 1, "serial")
    if serial_11["screenshotFile"] and serial_12["screenshotFile"]:
        cross["screenshotPixels"] = compare_screenshots(
            Path(serial_11["screenshotFile"]), Path(serial_12["screenshotFile"])
        )
        cross["screenshotPixelSource"] = {
            "d3d11": serial_11["screenshotFile"],
            "d3d12": serial_12["screenshotFile"],
        }
    report["crossRhi"] = cross
    report["status"] = "FAIL" if problems else "PASS"
    report["problems"] = problems

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "crossRhi": cross, "problems": problems}, ensure_ascii=False, indent=2))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
