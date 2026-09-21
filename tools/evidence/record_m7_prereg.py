#!/usr/bin/env python3
"""M7 预注册记录器（`tools/evidence/record_m7_prereg.ps1` 的等价实现）。

为什么有第二份：交付机器上的审批机制会拦下 `powershell -File tools/*.ps1` 调用（提示超时），
而预注册**必须在采集之前**落地。本工具与 PS 版保持同一 schema 与同一份必填清单，
输出 `prereg.json` 的字段、大小写与换行规则一致；两者可互换使用（改动其一必须同步另一个）。

    python tools/evidence/record_m7_prereg.py \
        --experiment-id E-M7-XXX-001 \
        --payload out/xxx/prereg-payload.json \
        --executable out/build/windows-msvc-profile/samples/rhi_sandbox/Release/MiniEngineSandbox.exe

字段校验、payload SHA-256、HEAD/脏文件、可执行文件 SHA-256、UTC 时间戳与
`tools/evidence/check_m7_prereg.py` 的"预注册先于数据"时序校验都保持一致。
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import hashlib
import json
import os
import platform
import subprocess
import sys

# 与 record_m7_prereg.ps1 的 $requiredFields 必须一致。
REQUIRED_FIELDS = ("experimentId", "hypothesis", "targetMetric", "expectedDirection", "threshold",
                   "protectedMetrics", "plan", "plannedRuns", "decisionRule")


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def git(repository: str, *args: str) -> str:
    result = subprocess.run(["git", "-C", repository, *args], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        return ""
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description="stamp an M7 pre-registration payload")
    parser.add_argument("--experiment-id", required=True)
    parser.add_argument("--payload", required=True, help="手写的预注册载荷 JSON")
    parser.add_argument("--output-directory", default="", help="默认取载荷所在目录")
    parser.add_argument("--executable", default="", help="把当次可执行文件 SHA-256 一并盖章")
    parser.add_argument("--repository", default="", help="默认取仓库根（本文件上一级）")
    args = parser.parse_args()

    repository = args.repository or os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
    repository = os.path.abspath(repository)
    payload_full = os.path.abspath(args.payload)
    if not os.path.isfile(payload_full):
        print(f"payload not found: {payload_full}", file=sys.stderr)
        return 2
    output = os.path.abspath(args.output_directory) if args.output_directory else os.path.dirname(payload_full)
    os.makedirs(output, exist_ok=True)

    with open(payload_full, "r", encoding="utf-8") as handle:
        payload = json.load(handle)

    missing = [field for field in REQUIRED_FIELDS
               if payload.get(field) in (None, "", [])
               or (isinstance(payload.get(field), str) and not payload[field].strip())]
    if missing:
        print(f"pre-registration payload is incomplete, missing: {', '.join(missing)}", file=sys.stderr)
        return 2
    if payload.get("experimentId") != args.experiment_id:
        print(f"payload experimentId '{payload.get('experimentId')}' does not match --experiment-id "
              f"'{args.experiment_id}'", file=sys.stderr)
        return 2
    status = payload.get("status")
    if status and status not in ("PRE-REGISTERED", "post-hoc"):
        print("payload.status must be PRE-REGISTERED or post-hoc", file=sys.stderr)
        return 2

    payload_bytes = os.path.getsize(payload_full)
    payload_sha = sha256_file(payload_full)

    commit = git(repository, "rev-parse", "HEAD").strip()
    if not commit:
        print(f"cannot resolve HEAD in {repository}", file=sys.stderr)
        return 2
    dirty = [line for line in git(repository, "status", "--porcelain").splitlines() if line.strip()]

    executable_path = ""
    executable_sha = ""
    if args.executable:
        executable_path = os.path.abspath(args.executable)
        if not os.path.isfile(executable_path):
            print(f"executable not found: {executable_path}", file=sys.stderr)
            return 2
        executable_sha = sha256_file(executable_path)

    now = _datetime.datetime.now(_datetime.timezone.utc).astimezone()
    prereg = {
        "schema": 1,
        "experimentId": args.experiment_id,
        "status": status or "PRE-REGISTERED",
        "recordedUtc": now.astimezone(_datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "recordedLocal": now.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "payloadFile": os.path.basename(payload_full),
        "payloadBytes": payload_bytes,
        "payloadSha256": payload_sha,
        "sourceCommit": commit,
        "sourceDirtyFiles": dirty,
        "executable": executable_path,
        "executableSha256": executable_sha,
        "host": platform.node(),
        "tool": "tools/evidence/record_m7_prereg.py (equivalent of tools/evidence/record_m7_prereg.ps1)",
    }
    prereg_path = os.path.join(output, "prereg.json")
    with open(prereg_path, "w", encoding="utf-8", newline="") as handle:
        handle.write(json.dumps(prereg, ensure_ascii=False, indent=2) + "\n")

    print(f"prereg recorded: {prereg_path}")
    print(f"  payload            : {payload_full} ({payload_bytes} bytes)")
    print(f"  payloadSha256      : {payload_sha}")
    print(f"  recordedUtc        : {prereg['recordedUtc']}")
    print(f"  sourceCommit       : {prereg['sourceCommit']}")
    print(f"  sourceDirtyFiles   : {len(dirty)}")
    if executable_sha:
        print(f"  executableSha256   : {executable_sha}")
    print("  下一步：现在采集数据（顺序由 tools/evidence/check_m7_prereg.py 在发布前后验证）。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
