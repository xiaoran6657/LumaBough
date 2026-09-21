import argparse, hashlib, json, os, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
MARKERS = ["portfolio-capture", "rhi-suspended", "same-bytecode-reload",
           "invalid-shader-rejected", "pipeline-count-retained",
           "temporary-frame-presented", "temporary-frame-presented",
           "original-extent-restored", "complete-clean-exit"]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def collapse(markers):
    # D 批次：停留模式会在同一状态里重复上报 temporary-frame-presented；
    # 折叠连续重复项后仍与 C 批次的事件序列一致，逐条 observed/expected 检查不受影响。
    return [marker for index, marker in enumerate(markers) if index == 0 or markers[index - 1] != marker]

def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))

def verify(folder, recipe, backend):
    anchor = read(folder / "anchor.json")
    metadata = read(folder / "metadata.json")
    events = [json.loads(line) for line in (folder / "tour-events.jsonl").read_text().splitlines()]
    if collapse([e["marker"] for e in events]) != collapse(MARKERS):
        raise ValueError("checkpoint sequence mismatch")
    if any(e["observed"] != e["expected"] for e in events):
        raise ValueError("checkpoint observation failed")
    if anchor["backend"] != backend or anchor["frame"] != recipe["frames"] // 2:
        raise ValueError("anchor identity mismatch")
    if any(anchor[k] != v for k, v in recipe["expected"].items()):
        raise ValueError("static anchor mismatch")
    if (metadata["reloadSuccess"], metadata["reloadRejected"], metadata["resizeCount"],
        metadata["fixedTick"], metadata["warningErrors"]) != (1, 1, 3, recipe["frames"], 0):
        raise ValueError("final gates failed")
    if (metadata["width"], metadata["height"]) != (recipe["width"], recipe["height"]):
        raise ValueError("final resolution mismatch")
    if sha(folder / "anchor.ppm") != sha(folder / "color.ppm"):
        raise ValueError("final pixels differ from pre-action anchor")
    return {"backend": backend, "anchor": anchor, "events": events,
            "final": {k: metadata[k] for k in ["fixedTick", "reloadSuccess", "reloadRejected",
                       "resizeCount", "warningErrors", "gpu", "driver", "debugLayer"]},
            "artifacts": {p.name: sha(p) for p in sorted(folder.iterdir()) if p.is_file()}}

def main():
    parser = argparse.ArgumentParser(description="Run three continuous rehearsals on each Core backend.")
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--recipe", type=Path, default=ROOT / "assets/recipes/m9-portfolio-demo.json")
    parser.add_argument("--headless", action="store_true")
    args = parser.parse_args()
    recipe = read(args.recipe)
    # warmupFrames 只要求下限（>=600）：D 批次的视频/Capture 协议在动作之后加入了停留帧，
    # 因此总帧数与 warmup 同步变长；历史 1202/600 协议仍然合法。
    if (recipe["schemaVersion"] != 2 or recipe["frames"] // 2 - 1 != recipe["warmupFrames"]
            or recipe["warmupFrames"] < 600 or recipe["backends"] != ["d3d11", "d3d12"]):
        parser.error("unsupported rehearsal protocol")
    exe, manifest, output = args.exe.resolve(), args.manifest.resolve(), args.output.resolve()
    if output.exists():
        parser.error("output must be a new directory; preserve previous attempts")
    output.mkdir(parents=True)
    identity = {"executableSha256": sha(exe), "recipeSha256": sha(args.recipe),
                "manifestSha256": sha(manifest), "driverSha256": sha(Path(__file__))}
    summary = {"schemaVersion": 1, "status": "RUNNING", "identity": identity,
               "mode": "headless" if args.headless else "visible", "runs": []}
    def save():
        (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    save()
    try:
        for rehearsal in range(1, 4):
            for backend in recipe["backends"]:
                folder = output / f"rehearsal-{rehearsal}-{backend}"
                folder.mkdir()
                command = [str(exe), f"--rhi={backend}", "--scene=m4-visual-baseline",
                           f"--manifest={manifest}", "--migration-level=9",
                           f"--frames={recipe['frames']}", f"--width={recipe['width']}",
                           f"--height={recipe['height']}", "--exercise-changes", "--debug",
                           f"--output={folder}"]
                if args.headless:
                    command.append("--headless")
                # D 批次可读性控制：只影响状态停留时长与临时尺寸，不改变事件与检查项。
                if recipe.get("vsync"):
                    command.append("--vsync=on")
                if recipe.get("tourHoldFrames"):
                    command.append(f"--tour-hold-frames={recipe['tourHoldFrames']}")
                temporary = recipe.get("tourTemporaryExtent")
                if temporary:
                    command.append(f"--tour-temporary-width={temporary['width']}")
                    command.append(f"--tour-temporary-height={temporary['height']}")
                with (folder / "process.log").open("wb") as log:
                    result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=300)
                if result.returncode:
                    raise ValueError(f"{folder.name}: exit {result.returncode}")
                run = verify(folder, recipe, backend)
                run["rehearsal"] = rehearsal
                summary["runs"].append(run)
                save()
                print(f"PASS {folder.name}", flush=True)
        if len({r["anchor"]["commandHash"] for r in summary["runs"]}) != 1:
            raise ValueError("anchor command hash differs across runs/backends")
        if sha(exe) != identity["executableSha256"]:
            raise ValueError("executable changed during rehearsals")
        summary["status"] = "PASS"
    except Exception as error:
        summary["status"] = "FAIL"
        summary["error"] = str(error)
        raise
    finally:
        save()

if __name__ == "__main__":
    main()

