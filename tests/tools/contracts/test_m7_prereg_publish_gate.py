"""发布器预注册门的契约测试（M7-PREREG-EVIDENCE / CTest `M7PreregPublishGate`）。

断言三件事（全部用临时目录 + 临时证据库，不碰真实库）：

1. 证据链完整的源目录：`-RequirePrereg` 通过，且发布（写临时库）产出 manifest；
2. 载荷被篡改（改完不重新盖章）：发布**在写任何目标文件之前**中止，临时库里没有该 topic；
3. 事后补写（重新盖章但载荷 mtime 晚于数据）：同样中止，且报出 ordering 原因。

复用 `check_m7_prereg.py` 的字段清单与哈希实现，避免两处口径漂移。
"""

import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../tools/evidence')))
import check_m7_prereg as prereg  # noqa: E402  同一目录的工具，复用必填字段与 sha256


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_json(path: str, payload: dict) -> None:
    with io.open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(json.dumps(payload, ensure_ascii=False, indent=1))


def build_source(root: str, *, tamper: bool, post_stamp: bool) -> str:
    """构造一个实验源目录：载荷 → 盖章 → 数据（顺序即纪律）。"""
    os.makedirs(root, exist_ok=True)
    payload = {field: "x" for field in prereg.REQUIRED_FIELDS}
    payload["experimentId"] = "E-M7-GATE-TEST"
    payload["status"] = "PRE-REGISTERED"
    payload_path = os.path.join(root, "prereg-payload.json")
    write_json(payload_path, payload)
    time.sleep(0.05)
    stamp = {
        "schema": 1,
        "experimentId": "E-M7-GATE-TEST",
        "status": "PRE-REGISTERED",
        "payloadFile": "prereg-payload.json",
        "payloadSha256": prereg.sha256_file(payload_path),
    }
    write_json(os.path.join(root, "prereg.json"), stamp)
    time.sleep(0.05)
    write_json(os.path.join(root, "run.json"), {"status": "PASS"})

    if tamper:  # 改载荷但不重新盖章
        payload["hypothesis"] = "事后改写的说法"
        write_json(payload_path, payload)
    if post_stamp:  # 重新盖章，但载荷 mtime 已经晚于数据
        stamp["payloadSha256"] = prereg.sha256_file(payload_path)
        write_json(os.path.join(root, "prereg.json"), stamp)
    return root


def run_publish(repo: str, source: str, store: str, topic: str, *extra: str) -> tuple[int, str]:
    command = [
        "powershell", "-NoProfile", "-File", os.path.join(repo, "tools", "evidence", "publish_evidence.ps1"),
        "-Source", source, "-Milestone", "M7", "-Topic", topic,
        "-DestinationRoot", store, "-Repository", repo, *extra,
    ]
    completed = subprocess.run(command, capture_output=True, text=True)
    return completed.returncode, completed.stdout + completed.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()
    repo = os.path.abspath(args.repo)

    work = tempfile.mkdtemp(prefix="m7-prereg-gate-")
    failures = 0
    try:
        print("[1/3] 证据链完整 → 门通过且发布产物落地")
        store = os.path.join(work, "store-ok")
        source = build_source(os.path.join(work, "src-ok"), tamper=False, post_stamp=False)
        code, output = run_publish(repo, source, store, "gate-ok", "-RequirePrereg")
        check(code == 0, f"publish must succeed, got {code}\n{output}")
        check("pre-registration gate OK" in output, "gate 未报告 OK")
        manifest = os.path.join(store, "M7", "gate-ok", "artifact-manifest.json")
        check(os.path.exists(manifest), "发布产物 manifest 缺失")
        print("  ok 门通过 + manifest 落地")

        print("[2/3] 载荷被篡改 → 写盘前中止")
        store = os.path.join(work, "store-tamper")
        source = build_source(os.path.join(work, "src-tamper"), tamper=True, post_stamp=False)
        code, output = run_publish(repo, source, store, "gate-tamper", "-RequirePrereg")
        check(code != 0, "tampered payload must abort the publish")
        check("SHA-256 mismatch" in output, f"缺少哈希失配原因\n{output}")
        check(not os.path.exists(os.path.join(store, "M7", "gate-tamper")), "中止后仍写了目标目录")
        check("artifact-manifest.json" not in output or "FAILED" in output, "中止信息不明确")
        print("  ok 篡改被挡在写盘之前")

        print("[3/3] 事后补写（重新盖章）→ 时序步中止")
        store = os.path.join(work, "store-post")
        source = build_source(os.path.join(work, "src-post"), tamper=True, post_stamp=True)
        code, output = run_publish(repo, source, store, "gate-post", "-RequirePrereg")
        check(code != 0, "post-stamped payload must abort the publish")
        check("ordering violated" in output, f"缺少时序原因\n{output}")
        check(not os.path.exists(os.path.join(store, "M7", "gate-post")), "中止后仍写了目标目录")
        print("  ok 事后补写被时序步挡住")

        print("[4/4] 不启用开关时行为不变（M6 之类无预注册的 topic 照常可发）")
        store = os.path.join(work, "store-legacy")
        source = os.path.join(work, "src-legacy")
        os.makedirs(source, exist_ok=True)
        write_json(os.path.join(source, "bundle.json"), {"kind": "no-prereg"})
        code, output = run_publish(repo, source, store, "legacy-ok")
        check(code == 0, f"publish without the switch must succeed, got {code}\n{output}")
        print("  ok 默认行为不变")
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        failures += 1
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print("M7 prereg publish gate: FAILED")
        return 1
    print("M7 prereg publish gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
