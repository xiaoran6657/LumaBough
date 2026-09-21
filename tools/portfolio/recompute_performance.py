"""Recompute all per-frame distributions, then apply the pinned historical comparison rule."""
import argparse
import copy
import hashlib
import json
import math
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))

def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

def quantile(values, q):
    ordered = sorted(values)
    index = q * (len(ordered) - 1)
    lo, hi = math.floor(index), math.ceil(index)
    return ordered[lo] * (1 - (index - lo)) + ordered[hi] * (index - lo)

def describe(values):
    if not values or not all(math.isfinite(v) for v in values):
        raise ValueError("empty or non-finite distribution")
    median = quantile(values, .5)
    return {"median": median, "mad": quantile([abs(v - median) for v in values], .5),
            "p95": quantile(values, .95), "p99": quantile(values, .99), "maximum": max(values)}

def recompute(run):
    result = copy.deepcopy(run)
    samples = run["samples"]
    if len(samples) != run["measuredFrames"] or len(samples) != 600:
        raise ValueError("unexpected measured sample count")
    if run["warmupFrames"] != 120 or run["statistics"]["sampleCount"] != len(samples):
        raise ValueError("protocol/statistics sample count mismatch")
    if run["correctness"]["status"] != "PASS":
        raise ValueError("correctness failed")
    for metric, old in run["statistics"].items():
        if isinstance(old, dict) and "median" in old:
            new = describe([s[metric] for s in samples])
            for key, value in new.items():
                if not math.isclose(value, old[key], rel_tol=1e-10, abs_tol=1e-8):
                    raise ValueError(f"stored distribution differs from samples: {metric}.{key}")
            result["statistics"][metric] = new
    hitches = {key: sum(s["cpuFrameMs"] > limit for s in samples)
               for key, limit in [("hitch16_67", 16.67), ("hitch33_33", 33.33), ("hitch50", 50)]}
    if hitches != run["statistics"]["hitches"]:
        raise ValueError("stored hitch counts differ from samples")
    result["statistics"]["hitches"] = hitches
    return result

def chronological_drift(runs, timestamps, pattern):
    import fnmatch
    selected = sorted((path, run) for path, run in runs.items() if fnmatch.fnmatch(path.parent.name, pattern))
    selected.sort(key=lambda item: timestamps[item[0]])
    medians = [run["statistics"]["cpuFrameMs"]["median"] for _, run in selected]
    cut = len(medians) // 2
    first, last = statistics.median(medians[:cut]), statistics.median(medians[cut:])
    return {"runOrder": [p.as_posix() for p, _ in selected],
            "changePercent": 100 * (last - first) / first}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=ROOT / "docs/evidence/performance")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--powershell", default="powershell.exe")
    args = parser.parse_args()
    source, output = args.input.resolve(), args.output.resolve()
    if output.exists():
        parser.error("use a fresh output directory to preserve previous results")
    output.mkdir(parents=True)
    manifest = read(source / "TRANSFORM-MANIFEST.json")
    for entry in manifest["files"]:
        relative = Path(entry["path"])
        if relative.is_absolute() or ".." in relative.parts or not (source / relative).resolve().is_relative_to(source):
            raise ValueError("unsafe input path")
        if sha(source / entry["path"]) != entry["publicSha256"]:
            raise ValueError("input hash mismatch: " + entry["path"])
    comparator = ROOT / "tools/performance/history/compare_m7_20260919.ps1"
    summary = {"schemaVersion": 2, "kind": "historical-raw-recomputation",
               "evidenceStatus": "BLOCKED", "accessStatus": "LOCAL",
               "runtimeCommit": None, "publicationCommit": None,
               "sourceCommit": None,
               "embeddedSourceCommit": "0c7a1a3b9c6f442148ed703fb4fa9282537b6d67",
               "sourceIdentityStatus": "UNRESOLVED_CONFIGURE_TIME_COMMIT",
               "generator": {"version": 1, "sha256": sha(Path(__file__)),
                             "comparatorSha256": sha(comparator),
                             "command": "python tools/portfolio/recompute_performance.py --output out/performance-recomputed"},
               "inputManifestSha256": sha(source / "TRANSFORM-MANIFEST.json"),
               "metricsScope": "CPU frame time, ms; median of per-run medians; linear interpolated quantiles",
               "warmupFrames": 120, "measuredFrames": 600, "runsPerCell": 5,
               "experiments": [],
               "limitations": [
                   "Historical binary source cannot be proven from its configure-time embedded commit.",
                   "No current-machine performance remeasurement is represented.",
                   "Allocation counters are unavailable; raw zero values do not prove zero allocations.",
                   "GPU statistics retain historical zero placeholders for unread delayed samples.",
                   "Historical comparator order lookup can fall back to runIndex; corrected chronological drift is reported separately.",
                   "Numeric recomputation is reproducible; historical build-and-rerun provenance remains blocked."]}
    for experiment, expected_count in [("2026-09-19-m7-method-003", 10), ("2026-09-19-m7-combined-001", 15)]:
        root = source / experiment
        protocol = read(root / "protocol.json")
        runs = {}
        for path in sorted(root.rglob("run.json")):
            relative = path.relative_to(root)
            runs[relative] = recompute(read(path))
            write(output / experiment / relative, runs[relative])
        if len(runs) != expected_count:
            raise ValueError("protocol run set incomplete")
        timestamps = {}
        for driver in sorted(root.rglob("sweep-driver-summary.json")):
            d = read(driver)
            for cell in d["cells"]:
                path = Path(cell["metricsPath"]).relative_to(experiment)
                if path in timestamps:
                    raise ValueError("duplicate run timestamp identity")
                timestamps[path] = cell["startedUtc"]
            write(output / experiment / driver.relative_to(root), d)
        if set(timestamps) != set(runs):
            raise ValueError("driver/run membership mismatch")
        comparisons = []
        for rule in protocol["comparisons"]:
            out = output / experiment / (rule["name"] + ".json")
            command = [args.powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(comparator),
                       "-Baseline", str(output / experiment), "-Candidate", str(output / experiment),
                       "-BaselineFilter", rule["baselineFilter"], "-CandidateFilter", rule["candidateFilter"],
                       "-TreatmentField", ",".join(rule["treatmentFields"]),
                       "-MatchOn", "sceneName,rhi,chunkSize",
                       "-ProtectedRegressionPercent", str(rule["protectedRegressionPercent"]),
                       "-NoiseFloorPercentOverride", str(rule["noiseFloorOverridePercent"]),
                       "-OutputJson", str(out)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=120)
            out.with_suffix(".log").write_bytes(result.stdout + result.stderr)
            if result.returncode not in (0, 1) or not out.exists():
                raise ValueError("historical comparison failed: " + str(out))
            calculated = read(out)
            row = calculated["rows"][0]
            for field in ["baselineRuns", "candidateRuns", "baselineMedianMs", "candidateMedianMs",
                          "changePercent", "p95ChangePercent", "result"]:
                if row[field] != rule["historicalRow"][field]:
                    raise ValueError(f"historical result mismatch: {experiment}/{field}")
            drift = {side: chronological_drift(runs, timestamps, rule[side + "Filter"])
                     for side in ["baseline", "candidate"]}
            drift["flagged"] = any(abs(drift[s]["changePercent"]) > row["requiredPercent"]
                                   for s in ["baseline", "candidate"])
            comparisons.append({"name": rule["name"], "result": row, "chronologicalDrift": drift,
                                "historicalResultReproduced": True})
        summary["experiments"].append({"id": next(iter(runs.values()))["experimentId"],
            "preregistrationCommit": ("2429dc1639ed2c53b4c9b49a7ff7c5ea8dfbb911" if expected_count == 10 else "45b1c1a675479119e2fcb802fa82ef51c728ba41"),
            "runCount": len(runs), "sampleCount": sum(len(r["samples"]) for r in runs.values()),
            "inputHashes": {p.relative_to(source).as_posix(): sha(p) for p in sorted(root.rglob("*.json"))},
            "comparisons": comparisons})
    write(output / "PERFORMANCE-SUMMARY.json", summary)
    print(json.dumps({"numericRecomputation": "PASS", "evidenceStatus": summary["evidenceStatus"],
                      "comparisons": [(e["id"], c["name"], c["result"]["changePercent"],
                        c["chronologicalDrift"]) for e in summary["experiments"] for c in e["comparisons"]]}))

if __name__ == "__main__":
    main()
