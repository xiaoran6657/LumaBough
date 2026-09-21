"""检查生产 sandbox 的进程级 CLI 行为，包括默认来源和禁止 fallback。"""
import argparse
import json
from pathlib import Path
import subprocess

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    failures = []
    cases = [
        ("invalid", ["--rhi=invalid"]),
        ("uppercase", ["--rhi=D3D12"]),
        ("missing", ["--rhi"]),
        ("empty", ["--rhi="]),
        ("duplicate", ["--rhi=d3d11", "--rhi=d3d12"]),
        ("unsupported", ["--rhi=vulkan"]),
        ("unsupported-gbv", ["--rhi=d3d11", "--gbv", "--headless"]),
        ("unknown", ["--unknown"]),
        ("negative-frames", ["--frames=-1"]),
        ("zero-frames", ["--frames=0"]),
        ("zero-width", ["--width=0"]),
        ("trailing-number", ["--height=20junk"]),
        ("flag-value", ["--headless=true"]),
        ("unknown-scene", ["--scene=unknown-scene"]),
    ]
    for name, arguments in cases:
        result = subprocess.run([str(args.exe.resolve()), *arguments], capture_output=True, timeout=30)
        stdout = result.stdout.decode("utf-8", errors="replace")
        stderr = result.stderr.decode("utf-8", errors="replace")
        passed = result.returncode != 0 and "d3d11" in stderr and "d3d12" in stderr and '"status":"PASS"' not in stdout
        if name == "unsupported-gbv":
            passed = passed and "no backend fallback" in stderr
        results.append({"case": name, "arguments": arguments, "exitCode": result.returncode,
                        "stdout": stdout, "stderr": stderr, "status": "PASS" if passed else "FAIL"})
        if not passed:
            failures.append(name)
    for name, arguments, backend, source in [
        ("default", [], "d3d12", "default"),
        ("explicit-d3d11", ["--rhi=d3d11"], "d3d11", "explicit"),
        ("explicit-d3d12", ["--rhi=d3d12"], "d3d12", "explicit")]:
        directory = args.output / name
        result = subprocess.run([str(args.exe.resolve()), *arguments, "--debug", "--headless",
                                 "--smoke-level=1", "--frames=3", "--width=32", "--height=32",
                                 f"--output={directory.resolve()}"], capture_output=True, timeout=60)
        try:
            metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
            passed = result.returncode == 0 and metadata["backend"] == backend and metadata["backendSource"] == source and not metadata["fallback"]
        except Exception:
            metadata = None
            passed = False
        results.append({"case": name, "exitCode": result.returncode, "metadata": metadata,
                        "stderr": result.stderr.decode("utf-8", errors="replace"), "status": "PASS" if passed else "FAIL"})
        if not passed:
            failures.append(name)
    report = {"schema": "miniengine.m6-06.cli.v1", "status": "FAIL" if failures else "PASS",
              "cases": results, "failures": failures}
    (args.output / "summary.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(report["status"], len(results), "cases", failures)
    return 1 if failures else 0

if __name__ == "__main__":
    raise SystemExit(main())
