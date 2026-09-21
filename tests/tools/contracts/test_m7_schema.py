#!/usr/bin/env python3
"""M7 机读契约的真实 JSON 解析校验（M7-A06 证据格式）。

C++ 单测只能做廉价的结构检查；这里用 Python 的 json 解析器验证：
  1. BenchmarkRun 序列化结果能被标准解析器接受，且控制变量/统计/样本字段齐全；
  2. 场景 recipe 与机器环境 manifest 的必需字段与哈希格式成立。

用法（由 CTest 调用，也可手工执行）：
  python tests/tools/contracts/test_m7_schema.py --exe <MiniEnginePerformanceContractTests.exe> \
      --recipe assets/recipes/m7-performance-scenes.json \
      --environment tests/baselines/m7/environment.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

HEX64 = re.compile(r"^[0-9a-f]{64}$")

RUN_FIELDS = [
    "schemaVersion", "experimentId", "variant", "sceneName", "sceneManifestPath", "sceneManifestSha256",
    "cameraPathSha256", "streamScriptSha256", "machineManifestPath", "machineManifestSha256", "sourceCommit",
    "executablePath", "executableSha256", "rhi", "workers", "chunkSize", "seed", "runIndex", "width", "height",
    "renderDrawLimit", "shadowDrawLimit", "vsync", "warmupFrames", "measuredFrames", "uploadMiBPerFrame",
    "loaderMode", "runOrderNote", "defaultsUsed", "unavailableMetrics", "environment", "correctness",
    "statistics", "samples", "assetRequests",
]

# v2（M7-05）新增：并行 RenderPacket 的控制变量与前台 gate 状态。v1 文件没有这些字段，
# 因此按 schemaVersion 条件校验，旧 raw 仍可读。
RUN_FIELDS_V2 = ["packetBuildMode", "schedulerMode", "chunkReserve", "foregroundGate"]
SAMPLE_FIELDS_V2 = ["packetWaitMs"]

# v3（M7-07）新增：上传预算控制变量与上传流水线证据。
RUN_FIELDS_V3 = ["uploadBudgetCpuMs", "uploadBudgetRequests", "uploadBudgetReloadPercent",
                 "uploadAgingThresholdMs"]
SAMPLE_FIELDS_V3 = ["uploadBytes", "uploadPendingCount", "uploadDeferredRetires"]
STATISTICS_FIELDS_V3 = ["totalUploadsStarted", "totalUploadsFailed", "totalReloadCommits",
                        "totalDeferredRetires", "totalFairnessHolds", "totalAgingPromotions",
                        "totalUploadEvents", "uploadBytesEstimated", "uploadBytesActual",
                        "uploadEstimatedErrorBytes", "residentUploadBytes", "uploadBytesReleased",
                        "cancelWasteBytes", "uploadPendingHighWater", "uploadInFlightHighWater"]

# v4（M7-08）新增：数据布局实验的控制变量与证据。
RUN_FIELDS_V4 = ["layoutVariant"]
SAMPLE_FIELDS_V4 = ["layoutCullMs"]
STATISTICS_FIELDS_V4 = ["layoutCullMs", "layoutCheckFrames", "layoutCheckMismatches"]

SAMPLE_FIELDS = [
    "frameSerial", "cpuFrameMs", "gpuFrameMs", "fixedUpdateMs", "worldExtractMs", "packetBuildMs", "cullMs",
    "packetMergeMs", "packetSortMs", "renderGraphBuildMs", "renderGraphCompileMs", "rhiSubmitMs", "presentMs",
    "ioReadMs", "decodeMs", "uploadMs", "taskQueueDepth", "activeWorkers", "stealAttempts", "stealSuccesses",
    "uploadQueueBytes", "uploadsCommitted", "allocationCount", "allocatedBytes", "residentBytes",
    "visibleCount", "drawCount",
]

DISTRIBUTION_FIELDS = ["median", "mad", "p95", "p99", "maximum"]


def fail(message: str) -> None:
    print(f"M7 schema validation failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def validate_run(path: str) -> None:
    with open(path, "r", encoding="utf-8") as stream:
        run = json.load(stream)

    require(run["schemaVersion"] in (1, 2, 3, 4),
            "schemaVersion must be 1 (M7-01), 2 (M7-05+), 3 (M7-07+) or 4 (M7-08+)")
    fields = RUN_FIELDS + (RUN_FIELDS_V2 if run["schemaVersion"] >= 2 else [])
    fields += RUN_FIELDS_V3 if run["schemaVersion"] >= 3 else []
    fields += RUN_FIELDS_V4 if run["schemaVersion"] >= 4 else []
    for field in fields:
        require(field in run, f"run JSON is missing field {field}")
    sample_fields = SAMPLE_FIELDS + (SAMPLE_FIELDS_V2 if run["schemaVersion"] >= 2 else [])
    sample_fields += SAMPLE_FIELDS_V3 if run["schemaVersion"] >= 3 else []
    sample_fields += SAMPLE_FIELDS_V4 if run["schemaVersion"] >= 4 else []
    if run["schemaVersion"] >= 3:
        for field in STATISTICS_FIELDS_V3:
            require(field in run["statistics"], f"statistics JSON is missing field {field}")
    if run["schemaVersion"] >= 4:
        for field in STATISTICS_FIELDS_V4:
            require(field in run["statistics"], f"statistics JSON is missing field {field}")
        require("layoutSemanticHash" in run["correctness"],
                "correctness JSON is missing field layoutSemanticHash")
    require(run["correctness"]["status"] == "PASS", "correctness.status must be PASS")
    require(len(run["samples"]) == run["measuredFrames"], "samples length must equal measuredFrames")
    require(run["statistics"]["sampleCount"] == run["measuredFrames"], "statistics.sampleCount mismatch")
    for name in ("cpuFrameMs", "cullMs", "packetSortMs", "packetBuildMs", "visibleCount"):
        distribution = run["statistics"][name]
        for key in DISTRIBUTION_FIELDS:
            require(key in distribution, f"statistics.{name} is missing {key}")
            require(isinstance(distribution[key], (int, float)), f"statistics.{name}.{key} must be numeric")
    for name in ("hitch16_67", "hitch33_33", "hitch50"):
        require(name in run["statistics"]["hitches"], f"statistics.hitches is missing {name}")
    require(isinstance(run["unavailableMetrics"], list), "unavailableMetrics must be a list")
    require(isinstance(run["renderDrawLimit"], int) and run["renderDrawLimit"] >= 0,
            "renderDrawLimit must be a non-negative integer")
    for sample in run["samples"]:
        missing = [field for field in sample_fields if field not in sample]
        require(not missing, f"sample is missing fields: {missing}")
        require(sample["cpuFrameMs"] > 0, "cpuFrameMs must be positive")


def validate_recipe(path: str) -> None:
    with open(path, "r", encoding="utf-8") as stream:
        recipe = json.load(stream)
    require(recipe["schemaVersion"] == 1, "recipe schemaVersion must be 1")
    require(HEX64.match(recipe["common"]["cameraPathSha256"].lower()) is not None,
            "recipe cameraPathSha256 must be 64 hex chars")
    names = {scene["name"] for scene in recipe["scenes"]}
    require(names == {"m7-cpu-scale", "m7-layout", "m7-streaming"}, f"unexpected scene set: {names}")
    streaming = next(scene for scene in recipe["scenes"] if scene["name"] == "m7-streaming")
    require(HEX64.match(streaming["requestScriptSha256"].lower()) is not None,
            "recipe requestScriptSha256 must be 64 hex chars")
    cpu_scale = next(scene for scene in recipe["scenes"] if scene["name"] == "m7-cpu-scale")
    require(cpu_scale["renderProxyCount"] == 50000, "m7-cpu-scale must fix 50000 proxies")


def validate_environment(path: str) -> None:
    with open(path, "r", encoding="utf-8") as stream:
        environment = json.load(stream)
    for field in ("schemaVersion", "capturedUtc", "hostName", "cpuModel", "logicalCores",
                  "physicalMemoryBytes", "powerPlan", "gpuAdapters"):
        require(field in environment, f"environment manifest is missing {field}")
    require(environment["schemaVersion"] == 1, "environment schemaVersion must be 1")
    require(environment["logicalCores"] > 0, "environment logicalCores must be positive")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--recipe", required=True)
    parser.add_argument("--environment", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as directory:
        out_path = os.path.join(directory, "benchmark-run.json")
        environment = dict(os.environ)
        environment["MINIENGINE_M7_SCHEMA_OUT"] = out_path
        completed = subprocess.run(
            [args.exe, "--gtest_filter=M7BenchmarkSchema.*"],
            cwd=directory,
            env=environment,
            capture_output=True,
            text=True,
        )
        if completed.returncode != 0:
            print(completed.stdout, file=sys.stderr)
            print(completed.stderr, file=sys.stderr)
            fail("gtest schema suite failed")
        require(os.path.exists(out_path), "MINIENGINE_M7_SCHEMA_OUT hook did not write a run JSON")
        validate_run(out_path)

    validate_recipe(args.recipe)
    validate_environment(args.environment)
    print("M7 schema validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
