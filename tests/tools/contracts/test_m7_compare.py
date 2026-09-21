#!/usr/bin/env python3
"""M7-09 方法学契约测试：compare_m7_results.ps1 与 check_m7_drift.ps1 的行为。

09 文档「本步验证」要求可被自动检查：
  * A/B 控制变量逐字段匹配（sceneManifestSha256/rhi/workers/chunkSize/warmup/measured…）；
  * 样本数 = measuredFrames；correctness 非 PASS 即失败；NaN/非正样本拒绝；
  * INCONCLUSIVE 被诚实使用（噪声大时不冒充 ACCEPTED/REJECTED）；
  * 唯一变量：未声明的差异必须 INVALID；
  * protected 指标退化（>5%）必须判 REJECTED；
  * 漂移超限时不得给出 ACCEPTED。

实现方式：合成 raw run JSON 到临时目录，调用真实 PowerShell 脚本，断言退出码与
输出 JSON 的结果类别。raw JSON 的形状与 tools/benchmark 的写盘器一致（v4）。

退出码：0 = 全部通过；1 = 有失败。
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
COMPARE = os.path.join(REPO_ROOT, "tools", "performance", "compare_m7_results.ps1")
DRIFT = os.path.join(REPO_ROOT, "tools", "performance", "check_m7_drift.ps1")

FAILURES: list[str] = []
CHECKS = 0


def check(condition: bool, message: str) -> None:
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(message)
        print(f"  FAIL {message}")
    else:
        print(f"  ok   {message}")


def distribution(values: list[float]) -> dict:
    ordered = sorted(values)
    n = len(ordered)

    def quantile(q: float) -> float:
        index = q * (n - 1)
        lower = int(index)
        upper = min(lower + 1, n - 1)
        return ordered[lower] * (1.0 - (index - lower)) + ordered[upper] * (index - lower)

    median = quantile(0.5)
    mad = sorted(abs(v - median) for v in values)[n // 2]
    return {"median": median, "mad": mad, "p95": quantile(0.95), "p99": quantile(0.99),
            "maximum": ordered[-1]}


def make_run(frames: list[float], *, measured: int | None = None, correctness: str = "PASS",
             metric: str = "cpuFrameMs", gpu_frames: list[float] | None = None,
             resident_bytes: float = 1024.0, chunk_size: int = 256, seed: int = 6657,
             scene: str = "m7-cpu-scale", statistics_extra: dict | None = None,
             **overrides) -> dict:
    measured_frames = measured if measured is not None else len(frames)
    samples = []
    for index, frame_ms in enumerate(frames):
        sample = {"frameIndex": index, "cpuFrameMs": frame_ms}
        if metric != "cpuFrameMs":
            sample[metric] = frame_ms
        if gpu_frames is not None:
            sample["gpuFrameMs"] = gpu_frames[index % len(gpu_frames)]
        samples.append(sample)
    statistics = {
        "sampleCount": measured_frames,
        "cpuFrameMs": distribution(frames),
        "residentBytes": distribution([resident_bytes]),
        "allocationCount": distribution([1.0]),
    }
    if gpu_frames is not None:
        statistics["gpuFrameMs"] = distribution(gpu_frames)
    if metric != "cpuFrameMs":
        statistics[metric] = distribution(frames)
    if statistics_extra:
        statistics.update(statistics_extra)
    run = {
        "schemaVersion": 4,
        "experimentId": "E-M7-TEST",
        "variant": "baseline",
        "sceneName": scene,
        "rhi": "d3d12",
        "workers": 4,
        "chunkSize": chunk_size,
        "seed": seed,
        "warmupFrames": 10,
        "measuredFrames": measured_frames,
        "runIndex": 1,
        "packetBuildMode": "serial",
        "schedulerMode": "none",
        "chunkReserve": True,
        "foregroundGate": False,
        "vsync": False,
        "uploadBudgetCpuMs": 0.0,
        "uploadBudgetRequests": 0,
        "uploadBudgetReloadPercent": 0,
        "uploadAgingThresholdMs": 0.0,
        "loaderMode": "serial-sync-read-validate",
        "layoutVariant": "aos",
        "sceneManifestSha256": "A" * 64,
        "renderDrawLimit": 4096,
        "shadowDrawLimit": 512,
        "cameraPathSha256": "",
        "streamScriptSha256": "",
        "executableSha256": "E" * 64,
        "sourceCommit": "c" * 40,
        "runOrderNote": "",
        "samples": samples,
        "statistics": statistics,
        "correctness": {"status": correctness},
        "unavailableMetrics": [],
    }
    run.update(overrides)
    return run


def write_runs(root: str, name_to_run: dict[str, dict]) -> None:
    os.makedirs(root, exist_ok=True)
    for name, run in name_to_run.items():
        cell_dir = os.path.join(root, name)
        os.makedirs(cell_dir, exist_ok=True)
        with open(os.path.join(cell_dir, "run.json"), "w", encoding="utf-8") as handle:
            json.dump(run, handle)


def frame_series(median_ms: float, count: int = 200, spread: float = 0.02) -> list[float]:
    """确定性帧序列：中位数 = median_ms，抖动 ±spread。"""
    values = []
    for index in range(count):
        offset = ((index * 37) % 100) / 100.0 - 0.5  # -0.5..0.5
        values.append(median_ms * (1.0 + spread * offset * 2.0))
    return values


def run_compare(baseline: str, candidate: str, *extra: str, expect_exit: int | None = None,
                output_json: str | None = None) -> tuple[int, str, dict | None]:
    command = ["powershell", "-NoProfile", "-File", COMPARE, "-Baseline", baseline,
               "-Candidate", candidate, *extra]
    if output_json:
        command += ["-OutputJson", output_json]
    result = subprocess.run(command, capture_output=True, text=True)
    payload = None
    if output_json and os.path.exists(output_json):
        with open(output_json, encoding="utf-8") as handle:
            payload = json.load(handle)
    if expect_exit is not None:
        detail = (result.stdout + result.stderr).strip().replace("\n", " | ")[-300:]
        check(result.returncode == expect_exit,
              f"exit={result.returncode} expected={expect_exit} :: {detail}")
    return result.returncode, (result.stdout + result.stderr), payload


def case(name: str) -> None:
    print(f"[{name}]")


def test_self_check(work: str) -> None:
    case("self-check：同一批 run 自比较必须 0% 且通过")
    runs = {f"cell-r{i}": make_run(frame_series(10.0 + i * 0.01)) for i in range(1, 3)}
    root = os.path.join(work, "self")
    write_runs(root, runs)
    _, _, payload = run_compare(root, root, "-SelfCheck", expect_exit=0,
                                output_json=os.path.join(work, "self.json"))
    check(payload is not None and payload["overallResult"] == "ACCEPTED",
          "self-check overall = ACCEPTED")
    check(payload is not None and all(row["changePercent"] == 0.0 for row in payload["rows"]),
          "self-check 每 cell changePercent == 0")


def test_accepted(work: str) -> None:
    case("ACCEPTED：10% 改善 + protected 全绿 + 声明治疗字段")
    baseline = {f"cell-r{i}": make_run(frame_series(100.0)) for i in range(1, 5)}
    candidate = {f"cell-r{i}": make_run(frame_series(90.0), packet_build_mode="parallel",
                                        **{"packetBuildMode": "parallel"}) for i in range(1, 5)}
    root_a = os.path.join(work, "accept-baseline")
    root_b = os.path.join(work, "accept-candidate")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode", expect_exit=0,
                                output_json=os.path.join(work, "accept.json"))
    check(payload is not None and payload["overallResult"] == "ACCEPTED", "overall = ACCEPTED")
    row = payload["rows"][0] if payload else {}
    check(row.get("result") == "ACCEPTED" and row.get("verdict") == "IMPROVED", "verdict = IMPROVED")


def gpu_mixture(high_fraction: float, scale: float = 1.0) -> list[float]:
    """两态 GPU 序列（0.13 / 0.15 ms × scale）：按给定占比混合，逐 run 占比不同 → 逐 run 中位数不同。"""
    count = 200
    high_count = int(round(count * high_fraction))
    values = [scale * 0.15] * high_count + [scale * 0.13] * (count - high_count)
    return values


def test_precision_limited_protected(work: str) -> None:
    case("ACCEPTED（带精度受限标注）：受保护指标自身极差 ≥ 阈值且候选落在基线区间内")
    # gpuFrameMs 合成两态（0.13 / 0.15 ms，≈15% 间距）：基线 5 run 的逐 run 中位数覆盖两态
    # → 极差 ≈ 15% ≥ 5%，候选侧中位数落在基线区间内 → 仅报不判（M7-GPUTIME-RESOLUTION）。
    fractions = [0.95, 0.05, 0.9, 0.1, 0.95]
    baseline = {}
    for index, fraction in enumerate(fractions, start=1):
        baseline[f"cell-r{index}"] = make_run(frame_series(100.0), gpu_frames=gpu_mixture(fraction))
    candidate = {}
    for index in range(1, 6):
        candidate[f"cell-r{index}"] = make_run(frame_series(90.0), gpu_frames=gpu_mixture(0.5),
                                               **{"packetBuildMode": "parallel"})
    root_a = os.path.join(work, "precision-a")
    root_b = os.path.join(work, "precision-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode",
                                expect_exit=0, output_json=os.path.join(work, "precision.json"))
    check(payload is not None and payload["overallResult"] == "ACCEPTED", "overall = ACCEPTED")
    row = payload["rows"][0] if payload else {}
    check("gpuFrameMs" in (row.get("precisionLimited") or []), "gpuFrameMs 标记为精度受限")
    check(bool(row.get("protectedOk")), "精度受限不再否决判定")
    entry = (row.get("protected") or {}).get("gpuFrameMs") or {}
    check(bool(entry.get("ok")), "精度受限的 ok 被置为不否决")


def test_precision_limited_does_not_mask_regression(work: str) -> None:
    case("REJECTED：同样双峰的受保护指标，但候选跑出基线观测区间（真退化仍拦住）")
    fractions = [0.95, 0.05, 0.9, 0.1, 0.95]
    baseline = {}
    for index, fraction in enumerate(fractions, start=1):
        baseline[f"cell-r{index}"] = make_run(frame_series(100.0), gpu_frames=gpu_mixture(fraction))
    candidate = {}
    for index in range(1, 6):
        # ×2：0.26 / 0.30 超出基线侧 [0.13, 0.15] 的观测范围 → 不得以"精度受限"豁免。
        candidate[f"cell-r{index}"] = make_run(frame_series(90.0), gpu_frames=gpu_mixture(0.5, 2.0),
                                               **{"packetBuildMode": "parallel"})
    root_a = os.path.join(work, "precision-regression-a")
    root_b = os.path.join(work, "precision-regression-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode",
                                expect_exit=1, output_json=os.path.join(work, "precision-regression.json"))
    row = payload["rows"][0] if payload else {}
    check(row.get("result") == "REJECTED", "真退化仍然 REJECTED")
    check("gpuFrameMs" not in (row.get("precisionLimited") or []), "超出基线区间不标精度受限")


def saturated_hitch_series(median_ms: float, hitch_count: int, count: int = 200) -> list[float]:
    """中位 median_ms（带小抖动，保证 MAD>0）的帧序列 + hitch_count 个 5×MAD 以上的尖峰。

    抖动幅 0.2 ms ⇒ MAD ≈ 0.6 ms ⇒ 尖峰 (median + 40 ms) 远超 5×MAD 阈值，只有尖峰计入相对 hitch。
    """
    values = [median_ms + (index % 7) * 0.2 for index in range(count)]
    for index in range(hitch_count):
        values[index] = median_ms + 40.0
    return values


def test_hitch_metric_switches_when_absolute_saturated(work: str) -> None:
    case("REJECTED：绝对 hitch 饱和时切换到中位+k×MAD，仍能判出尖峰增加")
    # m7-cpu-scale 形态：帧中位 65 ms ⇒ 每一帧都 >16.67 ms（绝对计数饱和）；候选尖峰 40 → 80。
    baseline = {f"cell-r{i}": make_run(saturated_hitch_series(65.0, 40)) for i in range(1, 4)}
    candidate = {f"cell-r{i}": make_run(saturated_hitch_series(65.0, 80), **{"packetBuildMode": "parallel"})
                 for i in range(1, 4)}
    root_a = os.path.join(work, "hitch-sat-a")
    root_b = os.path.join(work, "hitch-sat-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode", expect_exit=1,
                                output_json=os.path.join(work, "hitch-sat.json"))
    row = payload["rows"][0] if payload else {}
    entry = (row.get("protected") or {}).get("hitch") or {}
    check(entry.get("metric") == "overMad5", f"应切换到中位+5×MAD，实际 {entry.get('metric')}")
    check(entry.get("absoluteSaturated") is True, "饱和标记缺失")
    # hitch 计数是**跨 run 求和**（每侧 3 run）：40×3 / 80×3。
    check(entry.get("baseline") == 120 and entry.get("candidate") == 240,
          f"相对 hitch 计数不符：{entry.get('baseline')}/{entry.get('candidate')}")
    check(row.get("result") == "REJECTED", "尖峰翻倍应判 REJECTED")


def test_hitch_metric_stays_absolute_when_not_saturated(work: str) -> None:
    case("未饱和时仍用绝对阈值（行为不变）")
    # 10 ms 中位 + 5 个 20 ms 尖峰（>16.67 但远非每帧）⇒ 绝对 hitch 未饱和 → 沿用 hitch16_67。
    def series(hitches: int) -> list[float]:
        values = [10.0] * 200
        for index in range(hitches):
            values[index] = 20.0
        return values

    baseline = {f"cell-r{i}": make_run(series(5)) for i in range(1, 4)}
    candidate = {f"cell-r{i}": make_run(series(5), **{"packetBuildMode": "parallel"}) for i in range(1, 4)}
    root_a = os.path.join(work, "hitch-abs-a")
    root_b = os.path.join(work, "hitch-abs-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode", expect_exit=1,
                                output_json=os.path.join(work, "hitch-abs.json"))
    row = payload["rows"][0] if payload else {}
    entry = (row.get("protected") or {}).get("hitch") or {}
    check(entry.get("metric") == "hitch16_67", f"未饱和应沿用绝对阈值，实际 {entry.get('metric')}")
    check(entry.get("absoluteSaturated") is False, "未饱和却被标记饱和")


def write_driver_summary(root: str, cells: list[tuple[str, int, str]]) -> None:
    """写驱动摘要（cells + executionOrder）：比较器的执行顺序来源（M7-09 起的记录形态）。"""
    payload = {
        "schemaVersion": 1,
        "cells": [{"cell": cell, "runIndex": index, "startedUtc": started, "attempts": 1}
                  for cell, index, started in cells],
        "executionOrder": [f"{cell}#{index}" for cell, index, _ in cells],
    }
    with open(os.path.join(root, "sweep-driver-summary.json"), "w", encoding="utf-8") as handle:
        json.dump(payload, handle)


def test_order_source_visible_when_missing(work: str) -> None:
    case("顺序来源可见：批次无驱动摘要时标注 runIndex 回退并告警（不静默使用）")
    # 每侧 4 run（≥2 才能做漂移分半），但**不写** sweep-driver-summary.json —— 这正是 M7-01…08 的形态。
    baseline = {f"cell-r{i}": make_run(frame_series(100.0 + i)) for i in range(1, 5)}
    candidate = {f"cell-r{i}": make_run(frame_series(90.0 + i), **{"packetBuildMode": "parallel"})
                 for i in range(1, 5)}
    root_a = os.path.join(work, "order-missing-a")
    root_b = os.path.join(work, "order-missing-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, output, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode",
                                     output_json=os.path.join(work, "order-missing.json"))
    row = payload["rows"][0] if payload else {}
    sources = (row.get("drift") or {}).get("orderSources") or {}
    check(sources.get("baseline") == "runIndex-fallback",
          f"应标注 runIndex 回退，实际 {sources.get('baseline')}")
    check(bool((row.get("drift") or {}).get("orderWarning")), "缺失顺序时应给出告警文本")
    check("drift order source" in output, "控制台应打印顺序来源")


def test_order_source_recorded(work: str) -> None:
    case("顺序来源可见：有驱动摘要时标注 driver-execution-order 且无告警")
    baseline = {f"cell-r{i}": make_run(frame_series(100.0 + i)) for i in range(1, 5)}
    candidate = {f"cell-r{i}": make_run(frame_series(90.0 + i), **{"packetBuildMode": "parallel"})
                 for i in range(1, 5)}
    root_a = os.path.join(work, "order-recorded-a")
    root_b = os.path.join(work, "order-recorded-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    write_driver_summary(root_a, [("cell", i, f"2026-09-19T00:0{i}:00Z") for i in range(1, 5)])
    write_driver_summary(root_b, [("cell", i, f"2026-09-19T00:0{i}:00Z") for i in range(1, 5)])
    _, output, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode",
                                     output_json=os.path.join(work, "order-recorded.json"))
    row = payload["rows"][0] if payload else {}
    drift = row.get("drift") or {}
    sources = drift.get("orderSources") or {}
    check(sources.get("baseline") == "driver-execution-order",
          f"应标注驱动记录顺序，实际 {sources.get('baseline')}")
    check(not drift.get("orderWarning"), "有记录顺序时不应告警")


def test_undeclared_variable(work: str) -> None:
    case("INVALID：治疗字段差异未声明（唯一变量强制）")
    baseline = {"cell-r1": make_run(frame_series(100.0))}
    candidate = {"cell-r1": make_run(frame_series(100.0), packet_build_mode="parallel",
                                     **{"packetBuildMode": "parallel"})}
    root_a = os.path.join(work, "undeclared-a")
    root_b = os.path.join(work, "undeclared-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, output, _ = run_compare(root_a, root_b, expect_exit=2)
    check("single-variable violation" in output, "未声明的差异被拒绝（single-variable violation）")


def test_control_mismatch(work: str) -> None:
    case("INVALID：控制变量不一致（seed）")
    baseline = {"cell-r1": make_run(frame_series(100.0))}
    candidate = {"cell-r1": make_run(frame_series(100.0), seed=1234)}
    root_a = os.path.join(work, "control-a")
    root_b = os.path.join(work, "control-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, output, _ = run_compare(root_a, root_b, expect_exit=2)
    check("control mismatch" in output and "seed" in output, "seed 差异被拒绝（control mismatch）")


def test_sample_count(work: str) -> None:
    case("INVALID：样本数 != measuredFrames")
    run = make_run(frame_series(100.0), measured=99)
    root_a = os.path.join(work, "samples-a")
    root_b = os.path.join(work, "samples-b")
    write_runs(root_a, {"cell-r1": run})
    write_runs(root_b, {"cell-r1": make_run(frame_series(100.0))})
    _, output, _ = run_compare(root_a, root_b, expect_exit=2)
    check("sample count" in output, "样本数不符被拒绝")


def test_correctness(work: str) -> None:
    case("INVALID：correctness 非 PASS")
    root_a = os.path.join(work, "correct-a")
    root_b = os.path.join(work, "correct-b")
    write_runs(root_a, {"cell-r1": make_run(frame_series(100.0), correctness="FAIL")})
    write_runs(root_b, {"cell-r1": make_run(frame_series(100.0))})
    _, output, _ = run_compare(root_a, root_b, expect_exit=2)
    check("correctness is not PASS" in output, "FAIL 的 run 被拒绝")


def test_non_positive_sample(work: str) -> None:
    case("INVALID：NaN / 非正样本")
    frames = frame_series(100.0)
    frames[7] = 0.0
    root_a = os.path.join(work, "nan-a")
    root_b = os.path.join(work, "nan-b")
    write_runs(root_a, {"cell-r1": make_run(frames)})
    write_runs(root_b, {"cell-r1": make_run(frame_series(100.0))})
    _, output, _ = run_compare(root_a, root_b, expect_exit=2)
    check("finite and positive" in output, "非正样本被拒绝")


def test_protected_regression(work: str) -> None:
    case("REJECTED：目标改善但 GPU/内存 protected 退化 > 5%")
    baseline = {f"cell-r{i}": make_run(frame_series(100.0), gpu_frames=[20.0], resident_bytes=1000.0)
                for i in range(1, 4)}
    candidate = {f"cell-r{i}": make_run(frame_series(90.0), gpu_frames=[25.0], resident_bytes=1000.0,
                                        **{"packetBuildMode": "parallel"}) for i in range(1, 4)}
    root_a = os.path.join(work, "protected-a")
    root_b = os.path.join(work, "protected-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode", expect_exit=1,
                                output_json=os.path.join(work, "protected.json"))
    row = payload["rows"][0] if payload else {}
    check(row.get("result") == "REJECTED" and row.get("verdict") == "PROTECTED-REGRESSION",
          "protected 退化 → PROTECTED-REGRESSION / REJECTED")
    check(row.get("protected", {}).get("gpuFrameMs", {}).get("ok") is False, "gpuFrameMs 保护项被判定失败")


def test_within_noise_tight_is_rejected(work: str) -> None:
    case("REJECTED：噪声足够紧（<=5%）但改善不足")
    baseline = {f"cell-r{i}": make_run(frame_series(100.0)) for i in range(1, 4)}
    candidate = {f"cell-r{i}": make_run(frame_series(99.5), **{"packetBuildMode": "parallel"})
                 for i in range(1, 4)}
    root_a = os.path.join(work, "tight-a")
    root_b = os.path.join(work, "tight-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode", expect_exit=1,
                                output_json=os.path.join(work, "tight.json"))
    row = payload["rows"][0] if payload else {}
    check(row.get("result") == "REJECTED" and row.get("verdict") == "WITHIN-NOISE-TIGHT",
          "紧测量下无改善 → REJECTED（无足够改善）")


def test_within_noise_loose_is_inconclusive(work: str) -> None:
    case("INCONCLUSIVE：run 间噪声过大（噪声下限 > 5%）")
    # 每侧 run 的中位数互相差 12% → run 间 MAD 大 → 噪声下限 > 5%。
    spreads = [88.0, 100.0, 112.0, 96.0, 104.0]
    baseline = {f"cell-r{i}": make_run(frame_series(value)) for i, value in enumerate(spreads, start=1)}
    candidate = {f"cell-r{i}": make_run(frame_series(value * 1.02), **{"packetBuildMode": "parallel"})
                 for i, value in enumerate(spreads, start=1)}
    root_a = os.path.join(work, "loose-a")
    root_b = os.path.join(work, "loose-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode", expect_exit=0,
                                output_json=os.path.join(work, "loose.json"))
    row = payload["rows"][0] if payload else {}
    check(row.get("result") == "INCONCLUSIVE" and row.get("verdict") == "WITHIN-NOISE-LOOSE",
          "大噪声下未分辨 → INCONCLUSIVE")
    check(row.get("requiredPercent", 0.0) > 5.0, "噪声下限 > 5%")


def test_noise_floor_override(work: str) -> None:
    case("噪声下限覆写：独立噪声探针给出的下限必须能显式生效")
    spreads = [88.0, 100.0, 112.0, 96.0, 104.0]
    baseline = {f"cell-r{i}": make_run(frame_series(value)) for i, value in enumerate(spreads, start=1)}
    candidate = {f"cell-r{i}": make_run(frame_series(value * 1.02), **{"packetBuildMode": "parallel"})
                 for i, value in enumerate(spreads, start=1)}
    root_a = os.path.join(work, "override-a")
    root_b = os.path.join(work, "override-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode",
                                "-NoiseFloorPercentOverride", "3", expect_exit=1,
                                output_json=os.path.join(work, "override.json"))
    row = payload["rows"][0] if payload else {}
    check(row.get("requiredPercent") == 3.0, "覆写后的噪声下限进入报告")
    check(row.get("result") == "REJECTED", "覆写下 2% 的退化被如实判为 REJECTED")


def test_drift_downgrade(work: str) -> None:
    case("INCONCLUSIVE：改善但存在时间漂移（后半明显变慢）")
    # baseline 稳（±0.25%），candidate 中位数改善 ~5% 但随时间单调变慢（后半 +5%）。
    baseline = {f"cell-r{i}": make_run(frame_series(100.0 + (i % 2) * 0.5), runIndex=i) for i in range(1, 5)}
    candidate = {f"cell-r{i}": make_run(frame_series(90.0 + i * 2.5), runIndex=i,
                                        **{"packetBuildMode": "parallel"}) for i in range(1, 5)}
    root_a = os.path.join(work, "drift-a")
    root_b = os.path.join(work, "drift-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TreatmentField", "packetBuildMode", expect_exit=0,
                                output_json=os.path.join(work, "drift.json"))
    row = payload["rows"][0] if payload else {}
    check(row.get("drift", {}).get("flagged") is True, "漂移被标记")
    check(row.get("result") == "INCONCLUSIVE" and row.get("verdict") == "IMPROVED-BUT-DRIFT",
          "改善 + 漂移 → INCONCLUSIVE")


def test_drift_tool(work: str) -> None:
    case("check_m7_drift.ps1：按执行顺序给出趋势与判定")
    root = os.path.join(work, "drift-tool")
    runs = {}
    summary_cells = []
    for i in range(1, 7):
        name = f"cell-r{i}"
        # 后 3 个 run 明显变慢：drift 应被标记。
        value = 100.0 if i <= 3 else 130.0
        runs[name] = make_run(frame_series(value), runIndex=i)
        summary_cells.append({"cell": "cell", "runIndex": i, "variant": "candidate",
                              "metricsPath": os.path.join(root, name, "run.json"),
                              "startedUtc": f"2026-09-18T00:0{i}:00Z", "sequence": i})
    write_runs(root, runs)
    summary = {"schemaVersion": 1, "interleave": True, "shuffleSeed": 1,
               "executableSha256": "E" * 64, "cells": summary_cells}
    with open(os.path.join(root, "sweep-driver-summary.json"), "w", encoding="utf-8") as handle:
        json.dump(summary, handle)
    report_path = os.path.join(work, "drift-tool.json")
    result = subprocess.run(["powershell", "-NoProfile", "-File", DRIFT, "-InputDirectory", root,
                             "-OutputJson", report_path], capture_output=True, text=True)
    payload = None
    if os.path.exists(report_path):
        with open(report_path, encoding="utf-8") as handle:
            payload = json.load(handle)
    check(payload is not None and payload["driftFlagged"] is True, "漂移路径被标记")
    check(result.returncode == 1, f"漂移时退出码 1（实际 {result.returncode}）")
    if payload:
        series = payload["series"].get("cell, candidate") or payload["series"].get("cell")
        check(series is not None and series["driftPercent"] > 0, "driftPercent 记录了变慢方向")


def test_drift_tool_clean(work: str) -> None:
    case("check_m7_drift.ps1：无漂移时退出码 0")
    root = os.path.join(work, "drift-clean")
    runs = {}
    summary_cells = []
    for i in range(1, 7):
        name = f"cell-r{i}"
        runs[name] = make_run(frame_series(100.0 + (i % 2) * 0.2), runIndex=i)
        summary_cells.append({"cell": "cell", "runIndex": i, "variant": "baseline",
                              "metricsPath": os.path.join(root, name, "run.json"),
                              "startedUtc": f"2026-09-18T01:0{i}:00Z", "sequence": i})
    write_runs(root, runs)
    with open(os.path.join(root, "sweep-driver-summary.json"), "w", encoding="utf-8") as handle:
        json.dump({"schemaVersion": 1, "interleave": True, "cells": summary_cells}, handle)
    result = subprocess.run(["powershell", "-NoProfile", "-File", DRIFT, "-InputDirectory", root],
                            capture_output=True, text=True)
    check(result.returncode == 0, f"无漂移退出码 0（实际 {result.returncode}）")


def test_metric_selection(work: str) -> None:
    case("目标指标可切换（layoutCullMs）且缺失时 INVALID")
    baseline = {f"cell-r{i}": make_run(frame_series(100.0), statistics_extra={"layoutCullMs": distribution([4.0])})
                for i in range(1, 3)}
    candidate = {f"cell-r{i}": make_run(frame_series(100.0), statistics_extra={"layoutCullMs": distribution([3.0])})
                 for i in range(1, 3)}
    root_a = os.path.join(work, "metric-a")
    root_b = os.path.join(work, "metric-b")
    write_runs(root_a, baseline)
    write_runs(root_b, candidate)
    _, _, payload = run_compare(root_a, root_b, "-TargetMetric", "layoutCullMs", expect_exit=0,
                                output_json=os.path.join(work, "metric.json"))
    row = payload["rows"][0] if payload else {}
    check(row.get("targetMetric") == "layoutCullMs" and row.get("changePercent") == -25.0,
          "按 layoutCullMs 计算 -25%")

    root_c = os.path.join(work, "metric-c")
    write_runs(root_c, {f"cell-r{i}": make_run(frame_series(100.0)) for i in range(1, 3)})
    _, output, _ = run_compare(root_a, root_c, "-TargetMetric", "layoutCullMs", expect_exit=2)
    check("missing from statistics" in output, "指标缺失 → INVALID")


def test_v1_compatibility(work: str) -> None:
    case("v1 兼容：缺 v2/v3/v4 字段按更早版本语义回填")
    legacy = make_run(frame_series(100.0))
    for field in ("packetBuildMode", "schedulerMode", "chunkReserve", "foregroundGate", "vsync",
                  "loaderMode", "uploadBudgetCpuMs", "uploadBudgetRequests", "uploadBudgetReloadPercent",
                  "uploadAgingThresholdMs", "layoutVariant", "executableSha256", "sourceCommit",
                  "cameraPathSha256", "streamScriptSha256"):
        legacy.pop(field, None)
    legacy["schemaVersion"] = 1
    root_a = os.path.join(work, "v1-a")
    root_b = os.path.join(work, "v1-b")
    write_runs(root_a, {"cell-r1": legacy})
    write_runs(root_b, {"cell-r1": make_run(frame_series(100.0))})
    code, output, _ = run_compare(root_a, root_b, "-SelfCheck")
    check(code in (0, 1), f"v1 与 v4 同数据可比（exit={code}）而不因缺字段失败")


def main() -> int:
    if shutil.which("powershell") is None:
        print("skip: powershell not available")
        return 0
    work = tempfile.mkdtemp(prefix="m7-compare-test-")
    print(f"work dir: {work}")
    try:
        test_self_check(work)
        test_accepted(work)
        test_precision_limited_protected(work)
        test_precision_limited_does_not_mask_regression(work)
        test_hitch_metric_switches_when_absolute_saturated(work)
        test_order_source_visible_when_missing(work)
        test_order_source_recorded(work)
        test_hitch_metric_stays_absolute_when_not_saturated(work)
        test_undeclared_variable(work)
        test_control_mismatch(work)
        test_sample_count(work)
        test_correctness(work)
        test_non_positive_sample(work)
        test_protected_regression(work)
        test_within_noise_tight_is_rejected(work)
        test_within_noise_loose_is_inconclusive(work)
        test_noise_floor_override(work)
        test_drift_downgrade(work)
        test_drift_tool(work)
        test_drift_tool_clean(work)
        test_metric_selection(work)
        test_v1_compatibility(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f"\n{CHECKS - len(FAILURES)}/{CHECKS} checks passed")
    if FAILURES:
        print("failures:")
        for failure in FAILURES:
            print(f"  - {failure}")
        return 1
    print("M7 comparison methodology tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
