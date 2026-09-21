"""M7-08 布局微基准汇总：把 Google Benchmark JSON 收敛成表与机读摘要。

用法：
    python tools/performance/summarize_m7_layout_micro.py \\
        --input out/m7-08/layout-micro.json \\
        --summary out/performance/layout-micro-summary.json

口径：
* 只读 run_type == "aggregate" 的 mean/median/cv 行（Google Benchmark 的重复统计）；
* 时间统一换算为微秒；字节账直接取 counters（bytes_per_entity / soa_scan_bytes 等）；
* 按 (布局, 对象数) 输出表；不在这里做 accept/reject 判定（判定在验收记录里，
  必须同时引用等价性测试与端到端 sweep）。
"""

import argparse
import json
from typing import Any


def load(input_path: str) -> dict[str, Any]:
    with open(input_path, "r", encoding="utf-8") as stream:
        return json.load(stream)


def collect(data: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """run_name -> {aggregate_name: row}。"""
    collected: dict[str, dict[str, Any]] = {}
    for row in data["benchmarks"]:
        if row.get("run_type") != "aggregate":
            continue
        collected.setdefault(row["run_name"], {})[row["aggregate_name"]] = row
    return collected


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--summary", required=True)
    args = parser.parse_args()

    data = load(args.input)
    collected = collect(data)

    def sort_key(name: str) -> tuple[str, int]:
        layout, count = name.split("/")
        return (layout, int(count))

    entries = []
    header = f"{'benchmark':<28}{'mean us':>10}{'median us':>11}{'cv %':>7}{'B/entity':>10}{'scan B':>9}{'cold B':>8}"
    print(header)
    print("-" * len(header))
    for name in sorted(collected, key=sort_key):
        row = collected[name]
        mean_us = row["mean"]["real_time"] / 1000.0
        median_us = row["median"]["real_time"] / 1000.0
        cv_percent = row["cv"]["real_time"] * 100.0
        counters = row["mean"]
        bytes_per_entity = counters.get("bytes_per_entity", 0.0)
        scan_bytes = counters.get("soa_scan_bytes", 0.0)
        cold_bytes = counters.get("hot_cold_cold_bytes", 0.0)
        print(
            f"{name:<28}{mean_us:>10.1f}{median_us:>11.1f}{cv_percent:>7.2f}"
            f"{bytes_per_entity:>10.0f}{scan_bytes:>9.0f}{cold_bytes:>8.0f}"
        )
        entries.append(
            {
                "benchmark": name,
                "meanUs": round(mean_us, 3),
                "medianUs": round(median_us, 3),
                "cvPercent": round(cv_percent, 3),
                "bytesPerEntity": bytes_per_entity,
                "scanBytesPerEntity": scan_bytes,
                "coldBytesPerEntity": cold_bytes,
            }
        )

    summary = {
        "schema": "m7-layout-micro/1",
        "source": args.input,
        "libraryVersion": data["context"].get("library_version"),
        "hostName": data["context"].get("host_name"),
        "numCpus": data["context"].get("num_cpus"),
        "caches": data["context"].get("caches"),
        "entries": entries,
    }
    with open(args.summary, "w", encoding="utf-8") as stream:
        json.dump(summary, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(f"\nsummary: {args.summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
