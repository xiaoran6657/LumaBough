"""M6-11 稳定性诊断分析：live 计数必须形成平台，cumulative 计数不得加速。

输入是 MiniEngineSandbox 的 diagnostics.csv（每 N 帧一行）。工具只做数学判定，
不代替工程接受；warm-up 之后的样本用于判定。
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

LIVE = ["alive", "retiring", "declared", "live", "culled", "virtualResources", "physicalResources",
        "physicalTransients", "poolBytes", "poolResources", "resourceSets", "pipelines", "descriptorRanges",
        "uploadBytes", "rhiCommands"]
# 水位计数只增不减（句柄注册表槽位与 pool 高水位）：允许判定窗口内一次性容量阶跃，
# 出现第二次上升即视为持续增长。
WATERMARK = ["registrySlots", "maxLiveHandles", "poolHighWaterBytes", "poolHighWaterResources"]
CUMULATIVE = ["transientCreated", "transientReused", "transientRetired", "barriers", "unbinds"]
COLUMNS = ["frame", *LIVE, *WATERMARK, *CUMULATIVE]


class StabilityError(ValueError):
    """输入不满足判定前提。"""


def _slope(series: list[float]) -> float:
    count = len(series)
    mean_x = (count - 1) / 2
    mean_y = sum(series) / count
    denominator = sum((index - mean_x) ** 2 for index in range(count))
    return sum((index - mean_x) * (value - mean_y) for index, value in enumerate(series)) / denominator


def _mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def analyze_text(text: str, warmup_fraction: float = 0.25, slope_tolerance: float = 0.05,
                 rate_tolerance: float = 0.25) -> dict:
    rows = list(csv.DictReader(text.splitlines()))
    if not rows:
        raise StabilityError("diagnostics csv is empty")
    missing = [name for name in COLUMNS if name not in rows[0]]
    if missing:
        raise StabilityError("missing columns: " + ",".join(missing))
    if warmup_fraction < 0 or warmup_fraction >= 0.9:
        raise StabilityError("warmup fraction must be in [0, 0.9)")
    window = rows[max(1, int(len(rows) * warmup_fraction)):]
    if len(window) < 4:
        raise StabilityError(f"insufficient samples after warm-up: {len(window)}")
    failures: list[str] = []
    counters: dict[str, dict] = {}
    for name in LIVE:
        series = [int(row[name]) for row in window]
        slope = _slope([float(value) for value in series])
        counters[name] = {"class": "live", "first": series[0], "last": series[-1], "min": min(series),
                          "max": max(series), "slope": round(slope, 6)}
        if slope > slope_tolerance:
            failures.append(f"{name}: live counter grows (slope={slope:.4f}, first={series[0]}, last={series[-1]})")
    for name in WATERMARK:
        series = [int(row[name]) for row in window]
        increases = [index + 1 for index, (earlier, later) in enumerate(zip(series, series[1:])) if later > earlier]
        counters[name] = {"class": "watermark", "first": series[0], "last": series[-1],
                          "increaseCount": len(increases), "increaseSamples": increases}
        if len(increases) >= 2:
            failures.append(f"{name}: watermark grows repeatedly (increases at samples {increases[:6]}, "
                            f"first={series[0]}, last={series[-1]})")
    for name in CUMULATIVE:
        series = [int(row[name]) for row in window]
        deltas = [current - previous for previous, current in zip(series, series[1:])]
        quarter = max(1, len(deltas) // 4)
        first_mean = _mean([float(value) for value in deltas[:quarter]])
        last_mean = _mean([float(value) for value in deltas[-quarter:]])
        counters[name] = {"class": "cumulative", "first": series[0], "last": series[-1],
                          "firstQuarterMeanDelta": round(first_mean, 6), "lastQuarterMeanDelta": round(last_mean, 6)}
        if any(value < 0 for value in deltas):
            failures.append(f"{name}: cumulative counter decreased")
        if last_mean > first_mean * (1.0 + rate_tolerance) + 1e-9:
            failures.append(f"{name}: cumulative rate accelerates "
                            f"(first quarter mean={first_mean:.3f}, last quarter mean={last_mean:.3f})")
    return {"schema": "miniengine.m6-11.stability.v1", "status": "FAIL" if failures else "PASS",
            "samples": len(rows), "windowSamples": len(window), "failures": failures, "counters": counters}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--diagnostics", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--warmup-fraction", type=float, default=0.25)
    parser.add_argument("--slope-tolerance", type=float, default=0.05)
    parser.add_argument("--rate-tolerance", type=float, default=0.25)
    args = parser.parse_args()
    try:
        result = analyze_text(args.diagnostics.read_text(encoding="utf-8"), args.warmup_fraction,
                              args.slope_tolerance, args.rate_tolerance)
    except StabilityError as error:
        result = {"schema": "miniengine.m6-11.stability.v1", "status": "FAIL", "failures": [str(error)],
                  "counters": {}}
    result["input"] = str(args.diagnostics)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f'{result["status"]}: {len(result.get("failures", []))} failures')
    for failure in result.get("failures", []):
        print(" ", failure)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
