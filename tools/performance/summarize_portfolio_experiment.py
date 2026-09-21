"""从新实验逐帧 raw 重算；不使用历史实验裁定或合并帧池代替独立运行。"""
import argparse
import hashlib
import json
import math
from pathlib import Path
from statistics import median

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))

def quantile(values, q):
    values = sorted(values)
    if not values or not all(math.isfinite(v) for v in values):
        raise ValueError("empty/nonfinite samples")
    index = (len(values) - 1) * q
    low, high = math.floor(index), math.ceil(index)
    fraction = index - low
    return values[low] * (1 - fraction) + values[high] * fraction

def distribution(values):
    m = quantile(values, .5)
    return {"median": m, "mad": quantile([abs(v-m) for v in values], .5),
            "p95": quantile(values, .95), "p99": quantile(values, .99), "maximum": max(values)}

def statistics_for(raw):
    samples = raw["samples"]
    if len(samples) != 600 or raw["correctness"]["status"] != "PASS" or raw["correctness"]["validationMessages"]:
        raise ValueError("sample/correctness gate")
    result = {}
    for key, stored in raw["statistics"].items():
        if isinstance(stored, dict) and "median" in stored:
            actual = distribution([s[key] for s in samples])
            if any(not math.isclose(actual[k], stored[k], rel_tol=1e-10, abs_tol=1e-8) for k in actual):
                raise ValueError("stored statistics differ from samples: " + key)
            result[key] = actual
    cpu = [s["cpuFrameMs"] for s in samples]
    gpu = [s["gpuFrameMs"] for s in samples if s["gpuFrameMs"] > 0]
    if any(v <= 0 for v in cpu) or len(gpu) < 594:
        raise ValueError("CPU positivity/GPU coverage gate")
    result["gpuValid"] = distribution(gpu)
    result["gpuValidCount"] = len(gpu)
    result["absoluteHitches"] = {str(t): sum(v > t for v in cpu) for t in [16.67, 33.33, 50]}
    return result

def drift(runs):
    ordered = sorted(runs, key=lambda r: r["sequence"])
    medians = [r["statistics"]["cpuFrameMs"]["median"] for r in ordered]
    return 100 * (median(medians[2:]) - median(medians[:2])) / median(medians[:2])

def compare(a, b):
    aggregate = lambda runs, metric, field="median": median(r["statistics"][metric][field] for r in runs)
    baseline, candidate = aggregate(a,"cpuFrameMs"), aggregate(b,"cpuFrameMs")
    run_mad = median(abs(r["statistics"]["cpuFrameMs"]["median"] - baseline) for r in a)
    noise = max(3, 200 * run_mad / baseline)
    effect = 100 * (candidate - baseline) / baseline
    protected = {}
    for key, metric, field in [("p95Cpu","cpuFrameMs","p95"),("gpuFrameMs","gpuValid","median"),
                               ("residentBytes","residentBytes","median")]:
        av, bv = aggregate(a,metric,field), aggregate(b,metric,field)
        protected[key] = {"baseline": av, "candidate": bv, "pass": bv <= av * 1.05,
                          "changePercent": 100*(bv-av)/av if av else None}
    acpu = [s["cpuFrameMs"] for r in a for s in r["raw"]["samples"]]
    bcpu = [s["cpuFrameMs"] for r in b for s in r["raw"]["samples"]]
    am = median(acpu); mad = median(abs(v-am) for v in acpu)
    threshold = am + 5*mad if mad > 0 else 3*am
    ar, br = sum(v > threshold for v in acpu)/len(acpu), sum(v > threshold for v in bcpu)/len(bcpu)
    allowed = ar + max(.01, .05*ar)
    protected["hitch"] = {"thresholdMs": threshold, "baselineRate": ar, "candidateRate": br,
                          "maximumCandidateRate": allowed, "pass": br <= allowed}
    adrift, bdrift = drift(a), drift(b)
    if max(abs(adrift),abs(bdrift)) > noise:
        verdict, reason = "INCONCLUSIVE", "chronological drift exceeds noise threshold"
    elif not all(p["pass"] for p in protected.values()):
        verdict, reason = "REJECTED", "protected metric regression"
    elif effect < -noise:
        verdict, reason = "ACCEPTED", "CPU improvement exceeds noise threshold"
    elif noise <= 5:
        verdict, reason = "REJECTED", "insufficient CPU improvement"
    else:
        verdict, reason = "INCONCLUSIVE", "noise too wide"
    return {"baselineMedianMs":baseline,"candidateMedianMs":candidate,"changePercent":effect,
            "baselineP95Ms":aggregate(a,"cpuFrameMs","p95"),"candidateP95Ms":aggregate(b,"cpuFrameMs","p95"),
            "baselineP99Ms":aggregate(a,"cpuFrameMs","p99"),"candidateP99Ms":aggregate(b,"cpuFrameMs","p99"),
            "noisePercent":noise,"baselineRunMedianMadMs":run_mad,
            "drift":{"baselinePercent":adrift,"candidatePercent":bdrift},
            "protected":protected,"result":verdict,"reason":reason}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    root = args.input.resolve()
    protocol = read(root / "PREREGISTRATION.json")
    formal = read(root / "formal/summary.json")
    if formal["status"] != "PASS" or formal["preregistrationSha256"] != sha(root/"PREREGISTRATION.json"):
        raise ValueError("formal group or preregistration failed")
    if len(formal["runs"]) != 25 or len(protocol["order"]) != 25:
        raise ValueError("incomplete run set")
    if protocol["analyzerSha256"] != sha(Path(__file__)):
        raise ValueError("analysis algorithm changed after registration")
    transforms = read(root/"SANITIZATION.json")["files"] if (root/"SANITIZATION.json").exists() else {}
    def bound(path, original):
        current = sha(root/path)
        if current == original:
            return
        entry = transforms.get(path)
        if not entry or entry["originalSha256"] != original or entry["publicSha256"] != current:
            raise ValueError("artifact hash mismatch: " + path)
    runs = []
    hashes = {}
    invariants = ["rhi","sceneManifestSha256","cameraPathSha256","executableSha256","sourceCommit",
                  "machineManifestSha256","width","height","seed","warmupFrames","measuredFrames",
                  "chunkSize","chunkReserve","foregroundGate","vsync","layoutVariant"]
    controls = None
    for plan, receipt in zip(protocol["order"], formal["runs"]):
        if any(plan[k] != receipt[k] for k in ["runId","sequence","cell"]) or receipt["status"] != "PASS":
            raise ValueError("run order/identity mismatch")
        runpath = "formal/"+plan["runId"]+"/run.json"
        notespath = "formal/"+plan["runId"]+"/run-notes.json"
        bound(runpath,receipt["artifacts"]["run.json"]);bound(notespath,receipt["artifacts"]["run-notes.json"])
        raw, notes = read(root/runpath),read(root/notespath)
        now = {k:raw[k] for k in invariants}
        if controls is not None and now != controls:
            raise ValueError("undeclared control change")
        controls = now
        cell = next(c for c in protocol["cells"] if c["id"] == plan["cell"])
        if raw["sceneName"] != cell["scene"] or raw["packetBuildMode"] != cell["mode"] or raw["workers"] != cell["workers"]:
            raise ValueError("treatment mismatch")
        if not notes["foregroundOk"] or notes["foregroundRatio"] < .9:
            raise ValueError("foreground gate failed")
        stats = statistics_for(raw)
        if stats["gpuValidCount"] != notes["gpuFrameSamples"]:
            raise ValueError("GPU query count mismatch")
        runs.append({"runId":plan["runId"],"cell":plan["cell"],"sequence":plan["sequence"],
                     "raw":raw,"statistics":stats})
        hashes[runpath] = sha(root/runpath)
    comparisons = []
    for a,b in protocol["comparisons"]:
        aa,bb = [r for r in runs if r["cell"]==a],[r for r in runs if r["cell"]==b]
        if len(aa)!=5 or len(bb)!=5:
            raise ValueError("cell run count mismatch")
        comparisons.append({"baseline":a,"candidate":b,**compare(aa,bb)})
    summary = {"schemaVersion":1,"experimentIds":protocol["experimentIds"],"sourceCommit":None,
        "runtimeCommit":None,"publicationCommit":None,"validation":"PASS_LOCAL",
        "preregistrationSha256":sha(root/"PREREGISTRATION.json"),"analyzerSha256":sha(Path(__file__)),
        "rawHashes":hashes,"runCount":25,"measuredSamples":15000,"comparisons":comparisons,
        "runs":[{k:v for k,v in r.items() if k!="raw"} for r in runs],
        "limitations":protocol["limitations"],
        "historicalIdentityStatus":"unchanged; new experiments do not repair historical provenance"}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(summary,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"validation":"PASS_LOCAL","comparisons":comparisons},ensure_ascii=False))

if __name__ == "__main__":
    main()
