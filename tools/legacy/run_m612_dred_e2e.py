"""DRED-E2E-REMOVAL 取证执行器（用户 2026-09-16 批准的真实故障注入）。

运行 MiniEngineD3D12DredE2E（越界 UAV 写 → GPU 页错误 → 设备移除）并把证据
（reason、breadcrumbs、page-fault 链、格式化报告）打包到 out/m6-audit/dred-e2e/。
注意：执行会复位显示驱动数秒，桌面上可能出现闪烁；不要同时运行其他 3D 应用。
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        default=REPO / "out/build/windows-msvc-debug/tests/rhi/d3d12/Release/MiniEngineD3D12DredE2E.exe")
    parser.add_argument("--output", type=Path, default=REPO / "out/m6-audit/dred-e2e")
    args = parser.parse_args()
    exe = args.exe if args.exe.is_absolute() else REPO / args.exe
    if not exe.is_file():
        print(json.dumps({"status": "BLOCKED", "problems": [f"e2e executable missing: {exe}"]}, indent=1))
        return 2
    output = args.output if args.output.is_absolute() else REPO / args.output
    output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, MINIENGINE_DRED_E2E="1",
                       MINIENGINE_DRED_EVIDENCE_OUT=str(output / "dred-evidence.json"))
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    result = subprocess.run([str(exe), "--gtest_color=no"], capture_output=True, text=True, cwd=str(REPO),
                            env=environment, timeout=300)
    (output / "gtest-console.log").write_text((result.stdout or "") + (result.stderr or ""), encoding="utf-8")
    entries = {}
    for name in ("dred-evidence.json", "gtest-console.log"):
        candidate = output / name
        if candidate.is_file():
            entries[name] = {"bytes": candidate.stat().st_size, "sha256": sha256(candidate)}
    payload = {
        "schema": "miniengine.m6-audit.dred-e2e.v1",
        "generatedUtc": started,
        "finishedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "executable": str(exe), "executableSha256": sha256(exe),
        "exitCode": result.returncode,
        "gate": "MINIENGINE_DRED_E2E=1（用户 2026-09-16 批准的真实故障注入）",
        "entries": entries,
        "consoleTail": ((result.stdout or "") + (result.stderr or "")).splitlines()[-14:],
    }
    (output / "dred-e2e-run.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "PASS" if result.returncode == 0 else "FAIL",
                      "exit": result.returncode, "output": str(output)}, indent=1))
    return 0 if result.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
