"""M7 预注册证据校验器（M7-PREREG-EVIDENCE / CTest `M7PreregEvidence`）。

验证三件事，全部失败即退出非零：

1. **完整性**：目录里有 `prereg.json` + 载荷文件，载荷字段齐全（与记录器同一份必填清单）；
2. **未篡改**：载荷文件当前 SHA-256 == 盖章里记录的 `payloadSha256`；
3. **时序**（关键）：**载荷文件的 mtime 严格早于目录内全部其他文件**——即"预注册先于采集"。

时序这一步的形状值得说明：作者可能事后重写载荷再重新盖章，那时载荷 mtime 会晚于数据文件，
校验器照样报 FAIL（重新盖章救不了时序）。反过来，只改载荷不重新盖章会在第 2 步被抓到。
两条一起才构成"不可自证"的闭合。

用法：
  python tools/evidence/check_m7_prereg.py --dir out/stress-001 [--manifest <topic>/artifact-manifest.json]
  python tools/evidence/check_m7_prereg.py --self-test        # CTest 用：构造通过/篡改/事后盖章三种夹具
"""

import argparse
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import time

REQUIRED_FIELDS = ("experimentId", "hypothesis", "targetMetric", "expectedDirection", "threshold",
                   "protectedMetrics", "plan", "plannedRuns", "decisionRule")


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def load_json(path: str):
    with io.open(path, encoding="utf-8-sig") as handle:
        return json.load(handle)


def check_dir(root: str, manifest_path: str | None = None) -> list[str]:
    """返回问题列表（空列表 = 通过）。"""
    problems: list[str] = []

    prereg_path = os.path.join(root, "prereg.json")
    if not os.path.isfile(prereg_path):
        return [f"no prereg.json under {root}"]

    prereg = load_json(prereg_path)
    payload_name = prereg.get("payloadFile") or ""
    payload_path = os.path.join(root, payload_name)
    if not payload_name or not os.path.isfile(payload_path):
        return [f"payload file '{payload_name}' referenced by prereg.json is missing"]

    payload = load_json(payload_path)
    missing = [field for field in REQUIRED_FIELDS
               if payload.get(field) in (None, "", []) or (isinstance(payload.get(field), str) and not payload[field].strip())]
    if missing:
        problems.append(f"payload is missing required fields: {', '.join(missing)}")
    if payload.get("experimentId") != prereg.get("experimentId"):
        problems.append("payload experimentId does not match prereg.json")
    if prereg.get("status") not in ("PRE-REGISTERED", "post-hoc"):
        problems.append(f"prereg.status is {prereg.get('status')!r}, expected PRE-REGISTERED or post-hoc")

    actual_sha = sha256_file(payload_path)
    if actual_sha != (prereg.get("payloadSha256") or "").upper():
        problems.append(f"payload SHA-256 mismatch: file={actual_sha[:16]}… stamped={str(prereg.get('payloadSha256'))[:16]}… "
                        "(payload was edited after stamping)")

    # 时序：载荷 mtime 必须严格早于其他所有文件（含 prereg.json 自己与全部数据文件）。
    payload_mtime = os.path.getmtime(payload_path)
    others = []
    for base, _, files in os.walk(root):
        for name in files:
            path = os.path.join(base, name)
            if os.path.abspath(path) == os.path.abspath(payload_path):
                continue
            others.append((os.path.getmtime(path), os.path.relpath(path, root)))
    if not others:
        problems.append("no data files found next to the pre-registration (nothing to order against)")
    else:
        earliest = min(others)
        if payload_mtime >= earliest[0]:
            problems.append(
                f"ordering violated: payload mtime {time.strftime('%Y-%m-%dT%H:%M:%S', time.gmtime(payload_mtime))}Z "
                f"is not earlier than '{earliest[1]}' "
                f"({time.strftime('%Y-%m-%dT%H:%M:%S', time.gmtime(earliest[0]))}Z)")

    if manifest_path:
        manifest = load_json(manifest_path)
        entries = {entry["path"]: entry["sha256"].upper() for entry in manifest.get("files", [])}
        for relative in filter(None, (payload_name, "prereg.json")):
            key = relative.replace("\\", "/")
            if key in entries and entries[key] != sha256_file(os.path.join(root, relative)):
                problems.append(f"topic manifest hash mismatch for {key}")
            if key not in entries:
                problems.append(f"{key} is not listed in the topic manifest")

    return problems


def self_test() -> int:
    failures = 0
    root = tempfile.mkdtemp(prefix="m7-prereg-test-")
    try:
        def write(name: str, data: dict, directory: str = root) -> str:
            path = os.path.join(directory, name)
            with io.open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(json.dumps(data, ensure_ascii=False, indent=1))
            return path

        payload = {field: "x" for field in REQUIRED_FIELDS}
        payload["experimentId"] = "E-M7-TEST"
        payload["status"] = "PRE-REGISTERED"
        payload_path = write("prereg-payload.json", payload)
        time.sleep(0.05)
        prereg = {
            "schema": 1,
            "experimentId": "E-M7-TEST",
            "status": "PRE-REGISTERED",
            "payloadFile": "prereg-payload.json",
            "payloadSha256": sha256_file(payload_path),
        }
        write("prereg.json", prereg)
        time.sleep(0.05)
        write("run.json", {"status": "PASS"})  # 数据在预注册之后

        problems = check_dir(root)
        if problems:
            print(f"FAIL: 正确夹具应通过，却报 {problems}", file=sys.stderr)
            failures += 1
        else:
            print("  ok 预注册先于数据 → PASS")

        # 篡改：改载荷但不重新盖章
        payload["hypothesis"] = "已经被改成事后说法"
        write("prereg-payload.json", payload)
        problems = check_dir(root)
        if not any("SHA-256 mismatch" in problem for problem in problems):
            print(f"FAIL: 篡改载荷未被发现：{problems}", file=sys.stderr)
            failures += 1
        else:
            print("  ok 载荷被篡改 → 被哈希步抓到")

        # 事后盖章：重新盖章但载荷 mtime 已经晚于数据
        prereg["payloadSha256"] = sha256_file(payload_path)
        write("prereg.json", prereg)
        problems = check_dir(root)
        if not any("ordering violated" in problem for problem in problems):
            print(f"FAIL: 事后盖章未被时序抓到：{problems}", file=sys.stderr)
            failures += 1
        else:
            print("  ok 事后盖章 → 被时序步抓到")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", default="")
    parser.add_argument("--manifest", default="")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        failures = self_test()
        print("M7 prereg evidence self-test passed" if failures == 0 else f"M7 prereg self-test failed ({failures})")
        return 1 if failures else 0

    if not args.dir:
        parser.error("--dir or --self-test is required")
    problems = check_dir(args.dir, args.manifest or None)
    if problems:
        for problem in problems:
            print(f"FAIL: {problem}", file=sys.stderr)
        return 1
    print(f"pre-registration evidence OK: {args.dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
