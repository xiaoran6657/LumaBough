"""LumaBough 新实验采集：独立预演、预注册、固定运行集合；不自动重试。"""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import random
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
# P 批次第三次完整尝试：新的实验 ID 与新的预注册，不复用 001/002 的任何运行或路径。
ATTEMPT = "004"
CELLS = [
    {"id": "packet-serial", "scene": "m7-cpu-scale", "mode": "serial", "workers": 1, "async": False},
    {"id": "packet-parallel", "scene": "m7-cpu-scale", "mode": "parallel", "workers": 8, "async": False},
    {"id": "stream-sync", "scene": "m7-streaming", "mode": "serial", "workers": 1, "async": False},
    {"id": "stream-async", "scene": "m7-streaming", "mode": "serial", "workers": 1, "async": True},
    {"id": "stream-parallel", "scene": "m7-streaming", "mode": "parallel", "workers": 8, "async": True},
]

def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))

def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

def identities(workspace):
    snapshot = read(workspace / "SOURCE-SNAPSHOT.json")
    files = {}
    for entry in snapshot["files"]:
        path = workspace / "source" / entry["path"]
        if sha(path) != entry["sha256"]:
            raise ValueError("frozen source drift: " + entry["path"])
        files[path.relative_to(workspace).as_posix()] = entry["sha256"]
    for folder in [workspace / "build/samples/rhi_sandbox/Release",
                   workspace / "source/out/m4-09/scene"]:
        for path in sorted(folder.rglob("*")):
            if path.is_file() and path.suffix not in {".pdb", ".lib", ".exp"}:
                files[path.relative_to(workspace).as_posix()] = sha(path)
    for name in ["environment.json", "SOURCE-SNAPSHOT.json", "BUILD-RECORD.json", "BUILD-INPUTS.json", "DEPENDENCY-DOWNLOAD-REPAIR.json", "PREREGISTRATION-DRAFT.md", "source.zip"]:
        files[name] = sha(workspace / name)
    return files

def command(workspace, cell, folder, run_index, sequence, validation=False, pilot=False):
    source = workspace / "source"
    exe = workspace / "build/samples/rhi_sandbox/Release/MiniEngineSandbox.exe"
    experiment = (f"E-LB-PACKET-{ATTEMPT}" if cell["scene"] == "m7-cpu-scale" else f"E-LB-STREAMING-{ATTEMPT}")
    if pilot:
        experiment += "-PILOT"
    args = [str(exe), "--rhi=d3d12", "--scene=" + cell["scene"], "--width=1920", "--height=1080",
            "--vsync=off", "--seed=6657", "--workers=" + str(cell["workers"]), "--chunk-size=256",
            "--packet-build=" + cell["mode"], "--layout=none", "--visible-ratio=0.5",
            "--entity-count=" + ("50000" if cell["scene"] == "m7-cpu-scale" else "500"),
            "--camera=" + str(source / "assets/tests/m7/fixed-camera.bin"),
            "--stream-script=" + str(source / "assets/tests/m7/streaming-burst.bin"),
            "--metrics=" + str(folder / "run.json"), "--machine-manifest=" + str(workspace / "environment.json"),
            "--experiment-id=" + experiment, "--variant=" + cell["id"], "--run-index=" + str(run_index),
            "--run-order-note=" + f"sequence={sequence};runId={folder.name}", "--foreground-gate=on"]
    if cell["mode"] == "parallel":
        args += ["--scheduler=per-worker", "--chunk-reserve=on"]
    if cell["scene"] == "m7-streaming":
        args += ["--async-assets=" + ("on" if cell["async"] else "off"), "--upload-mib-per-frame=4"]
        if cell["async"]:
            args += ["--upload-budget-cpu-ms=0.5", "--upload-budget-requests=16",
                     "--upload-budget-reload-percent=50", "--upload-aging-ms=50"]
    args += ["--frames=600", "--debug"] if validation else ["--benchmark", "--warmup-frames=120", "--measure-frames=600"]
    return args

def verify_run(workspace, folder, cell, pilot=False):
    readiness = read(folder / "foreground-readiness.json")
    if readiness["status"] != "READY" or readiness["requiredStableMs"] != 1000 or readiness["timeoutMs"] != 60000:
        raise ValueError("foreground readiness precondition failed")
    raw, notes = read(folder / "run.json"), read(folder / "run-notes.json")
    if raw["correctness"]["status"] != "PASS" or raw["correctness"]["validationMessages"] != 0:
        raise ValueError("correctness gate failed")
    if raw["sceneName"] != cell["scene"] or raw["variant"] != cell["id"]:
        raise ValueError("cell identity mismatch")
    expected = f"E-LB-PACKET-{ATTEMPT}" if cell["scene"] == "m7-cpu-scale" else f"E-LB-STREAMING-{ATTEMPT}"
    if pilot:
        expected += "-PILOT"
    if raw["experimentId"] != expected or len(raw["samples"]) != 600:
        raise ValueError("experiment/sample identity mismatch")
    if (raw["width"], raw["height"], raw["warmupFrames"], raw["measuredFrames"],
        raw["workers"], raw["chunkSize"], raw["seed"]) != (1920, 1080, 120, 600, cell["workers"], 256, 6657):
        raise ValueError("protocol controls mismatch")
    if raw["executableSha256"].lower() != sha(workspace / "build/samples/rhi_sandbox/Release/MiniEngineSandbox.exe"):
        raise ValueError("executable identity mismatch")
    if raw["machineManifestSha256"].lower() != sha(workspace / "environment.json"):
        raise ValueError("machine identity mismatch")
    env = raw["environment"]
    if env["buildType"] != "Release" or any(env[k] for k in
        ["debugLayer", "gpuValidation", "warp", "captureToolActive", "tracyConnected"]):
        raise ValueError("measurement build/environment mismatch")
    if not notes["foregroundOk"] or notes["foregroundRatio"] < .9:
        raise ValueError("foreground gate failed")
    source = workspace / "source"
    if notes["assetManifestSha256"] != sha(source / "out/m4-09/scene/manifest.json"):
        raise ValueError("cooked asset identity mismatch")
    if raw["cameraPathSha256"] != sha(source / "assets/tests/m7/fixed-camera.bin"):
        raise ValueError("camera identity mismatch")
    if raw["sceneManifestSha256"] != sha(source / "assets/recipes/m7-performance-scenes.json"):
        raise ValueError("scene recipe identity mismatch")
    if cell["scene"] == "m7-streaming" and raw["streamScriptSha256"] != sha(source / "assets/tests/m7/streaming-burst.bin"):
        raise ValueError("stream script identity mismatch")
    if raw["vsync"] or not raw["foregroundGate"] or raw["statistics"]["sampleCount"] != 600:
        raise ValueError("presentation/sample protocol mismatch")
    positive_gpu = sum(s["gpuFrameMs"] > 0 for s in raw["samples"])
    if positive_gpu != notes["gpuFrameSamples"] or positive_gpu < 594:
        raise ValueError("GPU valid sample coverage below 99 percent")
    if any(not math.isfinite(s["cpuFrameMs"]) or s["cpuFrameMs"] <= 0 for s in raw["samples"]):
        raise ValueError("invalid CPU sample")
    if cell["scene"] == "m7-streaming" and (len(raw["assetRequests"]) != 66 or
            any(r["result"] != "ready" for r in raw["assetRequests"])):
        raise ValueError("stream request completion failed")
    return {"correctness": raw["correctness"], "foregroundRatio": notes["foregroundRatio"],
            "gpuValidSamples": positive_gpu, "sourceCommitEmbedded": raw["sourceCommit"]}

def execute(workspace, folder, cell, index, sequence, validation=False, pilot=False):
    folder.mkdir(parents=True, exist_ok=False)
    args = command(workspace, cell, folder, index, sequence, validation, pilot)
    receipt = {"cell": cell["id"], "runId": folder.name, "sequence": sequence, "runIndex": index,
               "startedUtc": utc(), "command": args, "status": "RUNNING"}
    write(folder / "receipt.json", receipt)
    try:
        with (folder / "process.log").open("wb") as log:
            result = subprocess.run(args, cwd=workspace / "source", stdout=log, stderr=subprocess.STDOUT, timeout=300)
        receipt["exitCode"] = result.returncode
        if result.returncode:
            raise ValueError("process exit " + str(result.returncode))
        if validation:
            lines = (folder / "process.log").read_text(encoding="utf-8", errors="replace").splitlines()
            last = json.loads(next(line for line in reversed(lines) if line.startswith('{"status"')))
            if last["status"] != "PASS":
                raise ValueError("validation result failed")
            receipt["observed"] = last
        else:
            receipt["observed"] = verify_run(workspace, folder, cell, pilot)
        receipt["status"] = "PASS"
    except Exception as error:
        receipt["status"] = "FAIL"
        receipt["error"] = str(error)
        raise
    finally:
        receipt["endedUtc"] = utc()
        receipt["artifacts"] = {p.name: sha(p) for p in sorted(folder.iterdir()) if p.is_file() and p.name != "receipt.json"}
        write(folder / "receipt.json", receipt)
    print("PASS " + folder.name, flush=True)
    return receipt

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["pilot", "register", "run"])
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    if args.action == "pilot":
        root = workspace / "pilot"
        root.mkdir(exist_ok=False)
        for i, cell in enumerate(CELLS, 1):
            execute(workspace, root / ("validation-" + cell["id"]), cell, 1, i, validation=True, pilot=True)
            execute(workspace, root / ("timing-" + cell["id"]), cell, 1, i, pilot=True)
        write(root / "complete.json", {"status": "PASS", "completedUtc": utc()})
    elif args.action == "register":
        if (workspace / "PREREGISTRATION.json").exists():
            raise ValueError("registration already exists")
        if read(workspace / "pilot/complete.json")["status"] != "PASS":
            raise ValueError("pilot not complete")
        rng = random.Random(665720260920)
        order = []
        for repeat in range(1, 6):
            block = list(CELLS)
            rng.shuffle(block)
            for cell in block:
                order.append({"sequence": len(order) + 1, "repeat": repeat, "cell": cell["id"],
                              "runId": f"{len(order)+1:02d}-{cell['id']}-r{repeat}"})
        protocol = {
            "schemaVersion": 2, "registeredUtc": utc(),
            "supersedesInvalidAttempt": "lb-current-003",
            "foregroundReadiness": "Before warmup: continuous foreground 1000ms; timeout 60000ms; no focus forcing or pause during measurement",
            "experimentIds": [f"E-LB-PACKET-{ATTEMPT}", f"E-LB-STREAMING-{ATTEMPT}"], "sourceCommit": None,
            "driverSha256": sha(Path(__file__)), "analyzerSha256": sha(Path(__file__).with_name("summarize_portfolio_experiment.py")), "files": identities(workspace), "cells": CELLS, "order": order,
            "build": "Fresh VS2026 v145 Release /O2 /Ob2 /DNDEBUG with PDB, Tracy OFF",
            "primaryMetric": "cpuFrameMs", "unit": "ms", "aggregation": "median of five run medians",
            "warmupFrames": 120, "measuredFrames": 600, "width": 1920, "height": 1080, "seed": 6657,
            "noiseRule": "max(3 percent, 2 * baseline run-median MAD / baseline median * 100)",
            "protected": {"p95Cpu": "candidate <= baseline * 1.05",
                          "gpuFrameMs": "positive samples only; coverage >=99 percent; median of run medians; <=5 percent regression",
                          "residentBytes": "median of run medians <=5 percent regression; transient pool only",
                          "hitch": "common baseline pooled median + 5*MAD threshold; if MAD zero use 3*median; candidate rate <= baseline rate + max(0.01,0.05*baseline rate)",
                          "unavailable": "allocation counters excluded, never interpreted as zero allocations"},
            "drift": "each cell ordered by unique sequence; median last3 vs first2; abs change above noise threshold makes comparison INCONCLUSIVE",
            "decisionOrder": ["invalid input or missing runs => INVALID", "excess drift => INCONCLUSIVE",
                              "protected regression => REJECTED", "CPU decrease > noise => ACCEPTED",
                              "noise <=5 percent without sufficient decrease => REJECTED", "otherwise INCONCLUSIVE"],
            "retries": 0, "exclusion": "No silent exclusion or replacement; failed run aborts group, retain all attempts. New protocol/ID required for a restart.",
            "foregroundMinimumRatio": .9, "gpuMinimumValidSamples": 594,
            "execution": "5 randomized complete blocks; new process per run; no concurrent build/capture/recording; 3 seconds between runs",
            "comparisons": [["packet-serial", "packet-parallel"], ["stream-sync", "stream-async"],
                            ["stream-async", "stream-parallel"], ["stream-sync", "stream-parallel"]],
            "limitations": ["Single machine, run count five per cell", "No historical identity repair",
                            "No process-wide leak claim", "No temperature sensor configured; no thermal stability claim"]}
        write(workspace / "PREREGISTRATION.json", protocol)
        (workspace / "PREREGISTRATION.sha256").write_text(sha(workspace / "PREREGISTRATION.json") + "\n", encoding="ascii")
        print("registered " + sha(workspace / "PREREGISTRATION.json"), flush=True)
    else:
        registration = workspace / "PREREGISTRATION.json"
        assert sha(registration) == (workspace / "PREREGISTRATION.sha256").read_text().strip()
        protocol = read(registration)
        if protocol["driverSha256"] != sha(Path(__file__)):
            raise ValueError("driver drift after registration")
        if protocol["analyzerSha256"] != sha(Path(__file__).with_name("summarize_portfolio_experiment.py")):
            raise ValueError("analysis algorithm drift after registration")
        if identities(workspace) != protocol["files"]:
            raise ValueError("input drift after registration")
        root = workspace / "formal"
        root.mkdir(exist_ok=False)
        result = {"status": "RUNNING", "preregistrationSha256": sha(registration), "runs": []}
        write(root / "summary.json", result)
        try:
            for item in protocol["order"]:
                if identities(workspace) != protocol["files"]:
                    raise ValueError("frozen input drift before run")
                cell = next(c for c in CELLS if c["id"] == item["cell"])
                result["runs"].append(execute(workspace, root / item["runId"], cell, item["repeat"], item["sequence"]))
                if identities(workspace) != protocol["files"]:
                    raise ValueError("frozen input drift after run")
                write(root / "summary.json", result)
                time.sleep(3)
            result["status"] = "PASS"
        except Exception as error:
            result["status"] = "FAIL"
            result["error"] = str(error)
            raise
        finally:
            write(root / "summary.json", result)

if __name__ == "__main__":
    main()
