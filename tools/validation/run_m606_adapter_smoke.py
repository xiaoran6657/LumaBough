"""逐级运行同一 sandbox 的双后端路径，并按冻结公式检查截图。"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time

def tone(value: float) -> int:
    x = value / (1.0 + value)
    return round(255 * (12.92 * x if x <= .0031308 else 1.055 * x ** (1 / 2.4) - .055))

def screenshot(directory: Path, level: int) -> tuple[dict, bytes]:
    info = json.loads((directory / "readback.json").read_text(encoding="utf-8"))
    raw = (directory / "screenshot.rgba").read_bytes()
    width, height, pitch = info["width"], info["height"], info["rowPitch"]
    assert info["ready"] and not info["unavailable"]
    assert len(raw) == info["byteCount"] and pitch >= width * 4
    assert len(raw) >= (height - 1) * pitch + width * 4
    packed = b"".join(raw[y * pitch:y * pitch + width * 4] for y in range(height))
    def pixel(x: int, y: int) -> list[int]:
        return list(packed[(y * width + x) * 4:(y * width + x) * 4 + 4])
    expected = [10, 20, 36, 255] if level == 1 else [tone(4), tone(2), tone(1), 255]
    samples = []
    if level < 3:
        deviations = [abs(value - expected[i % 4]) for i, value in enumerate(packed)]
        assert max(deviations) <= 1, (level, max(deviations), pixel(width // 2, height // 2), expected)
        samples.append({"location": "all", "expected": expected, "maxDeviation": max(deviations)})
    else:
        multiplier = 2 if level == 6 else 1
        for name, x, y, depth in (("center", width // 2, height // 2, .25),
                                  ("corner", 2, 2, 1.0)):
            expected = [tone(depth * multiplier), 0, 0, 255]
            actual = pixel(x, y)
            assert max(abs(a - b) for a, b in zip(actual, expected)) <= 1, (level, name, actual, expected)
            samples.append({"location": name, "actual": actual, "expected": expected})
        for i in range(0, len(packed), 4):
            assert packed[i + 1:i + 4] == bytes((0, 0, 255)), ("invalid depth display channels", i)
    ppm = f"P6\n{width} {height}\n255\n".encode() + b"".join(packed[i:i+3] for i in range(0, len(packed), 4))
    (directory / "screenshot.ppm").write_bytes(ppm)
    return {"width": width, "height": height, "sha256": hashlib.sha256(packed).hexdigest(),
            "samples": samples}, packed

def run(exe: Path, root: Path, backend: str, level: int, warp: bool) -> dict:
    mode = "warp" if warp else "hardware"
    directory = root / f"{backend}-{mode}-level{level}"
    directory.mkdir(parents=True, exist_ok=True)
    command = [str(exe), f"--rhi={backend}", "--headless", "--debug",
               f"--smoke-level={level}", "--frames=9", "--width=96", "--height=64",
               f"--output={directory}"]
    if backend == "d3d12":
        command.append("--gbv")
    if warp:
        command.append("--warp")
    start = time.monotonic()
    result = subprocess.run(command, capture_output=True, timeout=90)
    (directory / "stdout.log").write_bytes(result.stdout)
    (directory / "stderr.log").write_bytes(result.stderr)
    assert result.returncode == 0, f'{directory.name}: exit {result.returncode}: {result.stderr.decode(errors="replace")[-3000:]}'
    metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
    assert metadata["backend"] == backend and metadata["backendSource"] == "explicit"
    assert metadata["warpRequested"] == warp and not metadata["fallback"]
    assert metadata["frames"] == 9 and metadata["debugLayer"]
    assert not any(metadata[key] for key in ("nativeWarningErrors", "nativeLiveResources", "aliveObjects", "retiringObjects"))
    assert metadata["nativeSubmittedBatches"] >= 9 and metadata["nativeCompletedSerial"] >= 9
    assert backend != "d3d12" or metadata["gpuValidation"]
    assert level < 4 or metadata["resizeExercised"]
    assert level < 6 or metadata["reloadExercised"]
    assert level < 2 or metadata["explicitUnbinds" if backend == "d3d11" else "barriers"] > 0
    pixels, packed = screenshot(directory, level)
    return {"backend": backend, "mode": mode, "level": level, "status": "PASS",
            "durationSeconds": time.monotonic() - start, "command": command,
            "metadata": metadata, "screenshot": pixels, "_packed": packed}

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--warp", action="store_true")
    parser.add_argument("--level", type=int, choices=range(1, 7))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"schema": "miniengine.m6-06.adapter-smoke.v1", "status": "FAIL", "runs": [], "comparisons": []}
    try:
        for level in ([args.level] if args.level else range(1, 7)):
            pair = [run(args.exe.resolve(), args.output.resolve(), backend, level, args.warp) for backend in ("d3d11", "d3d12")]
            pixels = [item.pop("_packed") for item in pair]
            assert len(pixels[0]) == len(pixels[1])
            maximum = max(abs(a - b) for a, b in zip(*pixels))
            assert maximum <= 1, (level, "backend pixel disagreement", maximum)
            report["runs"].extend(pair)
            report["comparisons"].append({"level": level, "maxChannelDifference": maximum, "status": "PASS"})
            print(f"PASS level {level}: two native backends, max channel difference {maximum}", flush=True)
        report["status"] = "PASS"
    except Exception as error:
        report["failure"] = str(error)
        print(f"FAIL: {error}", flush=True)
    (args.output / "summary.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0 if report["status"] == "PASS" else 1

if __name__ == "__main__":
    raise SystemExit(main())
