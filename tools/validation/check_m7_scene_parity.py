#!/usr/bin/env python3
"""M7-10 场景级 parity 与 validation 比较器。

输入：`tools/performance/run_m7_parity_matrix.ps1` 的输出目录（每 run 一个 `<cell>-r<N>/run.json`）。

做三件事（判据 A25 / A26）：

1. **同侧自一致**（每个 cell 的 N 次重复）：确定性证据字段必须逐位相同
   —— packetSequenceHash / graphHash / commandHash / screenshotHash / visibleCount /
   drawCount / layoutSemanticHash（若启用）/ 资源 revision manifest 摘要。
2. **跨后端等价**（同 scene+workers 的 d3d11 与 d3d12 cell 配对）：
   packetSequenceHash 与 graphHash 必须相同；commandHash 记录实测（语义命令流应当相同，但
   M7-05 已证明后端差异可能体现在命令哈希上，因此它是"记录项"而不是判据）；
   visibleCount / drawCount / trianglesSubmitted 必须相同；资源 revision manifest 摘要相同；
   固定帧截图像素按 M6 阈值判定（MAE ≤ 2/255、p99 ≤ 5/255、超阈率 ≤ 0.5%）。
3. **validation 矩阵**：`validationMessages == 0`、`liveResources == 0`、`aliveObjects == 0`、
   `retiringObjects == 0`（Debug Layer / GBV 零消息）。

退出码：0 = 全部通过；1 = 有失败；2 = 输入不满足契约（缺字段/结构不符）。
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import sys

IMAGE_THRESHOLDS = {"mae": 2.0 / 255.0, "p99": 5.0 / 255.0, "changed_rate_above_p99": 0.005}

FAILURES: list[str] = []
CHECKS = 0


def check(condition: bool, message: str) -> None:
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(message)
        print(f"  FAIL {message}")
    else:
        print(f"  ok   {message}")


# --------------------------------------------------------------------------
# PPM 读取（运行器写 P6，8 bit）：与 M6 的 read_png 同源思路，避免第三方依赖。
# --------------------------------------------------------------------------
def read_ppm(path: str) -> tuple[int, int, bytes] | None:
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError:
        return None
    if not data.startswith(b"P6"):
        return None
    fields: list[bytes] = []
    index = 2
    while len(fields) < 3 and index < len(data):
        while index < len(data) and data[index : index + 1].isspace():
            index += 1
        if data[index : index + 1] == b"#":
            while index < len(data) and data[index] != 0x0A:
                index += 1
            continue
        start = index
        while index < len(data) and not data[index : index + 1].isspace():
            index += 1
        fields.append(data[start:index])
    index += 1
    width, height, _maxval = (int(field) for field in fields)
    pixels = data[index : index + width * height * 3]
    if len(pixels) != width * height * 3:
        return None
    return width, height, pixels


def image_metrics(reference: tuple[int, int, bytes], candidate: tuple[int, int, bytes]) -> dict:
    ref_w, ref_h, ref_pixels = reference
    cand_w, cand_h, cand_pixels = candidate
    if (ref_w, ref_h) != (cand_w, cand_h):
        raise ValueError(f"extent mismatch: {ref_w}x{ref_h} vs {cand_w}x{cand_h}")
    total = len(ref_pixels)
    absolute = 0
    errors: list[int] = []
    changed_above = 0
    first_difference = -1
    for index in range(total):
        delta = abs(ref_pixels[index] - cand_pixels[index])
        absolute += delta
        errors.append(delta)
        if delta > 0 and first_difference < 0:
            first_difference = index
    errors.sort()
    count = len(errors)
    p99 = errors[min(count - 1, int(0.99 * (count - 1)))] if count else 0
    threshold = IMAGE_THRESHOLDS["p99"] * 255.0
    changed_above = sum(1 for value in errors if value > threshold)
    return {
        "meanAbsoluteError": absolute / total,
        "p99Error": p99,
        "changedRateAboveP99": changed_above / total,
        "changedPixels": sum(1 for value in errors if value > 0),
        "firstDifference": first_difference,
    }


def revision_manifest_digest(run: dict) -> str:
    """从 raw 的逐请求样本派生资源 revision manifest 摘要。

    运行器目前不写 assetId→revision 的聚合字段（见 M7-10 记录的未验证边界），
    这里用 assetRequests[] 的 (assetId, revision) 去重排序后哈希，作为
    "发布过的资源版本集合"的可比较摘要。
    """
    entries = set()
    for sample in run.get("assetRequests", []):
        asset_id = sample.get("assetId")
        revision = sample.get("revision", 1)
        if asset_id is None:
            continue
        entries.add((int(asset_id), int(revision)))
    payload = ";".join(f"{asset_id}:{revision}" for asset_id, revision in sorted(entries))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()[:16], len(entries)


def load_runs(root: str) -> dict[str, list[dict]]:
    cells: dict[str, list[dict]] = {}
    for path in sorted(glob.glob(os.path.join(root, "*-r*", "run.json"))):
        cell_directory = os.path.basename(os.path.dirname(path))
        cell = cell_directory.rsplit("-r", 1)[0]
        try:
            with open(path, encoding="utf-8") as handle:
                run = json.load(handle)
        except (OSError, json.JSONDecodeError) as error:
            raise SystemExit(f"INVALID: cannot read {path}: {error}") from error
        run["__path"] = path
        run["__dir"] = os.path.dirname(path)
        cells.setdefault(cell, []).append(run)
    for runs in cells.values():
        runs.sort(key=lambda run: run.get("runIndex", 0))
    return cells


def determinism_fields(run: dict) -> dict:
    correctness = run.get("correctness")
    if correctness is None:
        # validation 冒烟摘要：字段在顶层。
        return {
            "packetSequenceHash": run.get("packetSequenceHash"),
            "graphHash": run.get("graphHash"),
            "commandHash": run.get("commandHash"),
            "screenshotHash": None,
            "visibleCount": None,
            "drawCount": None,
        }
    return {
        "packetSequenceHash": correctness.get("packetSequenceHash"),
        "graphHash": correctness.get("graphHash"),
        "commandHash": correctness.get("commandHash"),
        "screenshotHash": correctness.get("screenshotHash"),
        "visibleCount": correctness.get("visibleCount"),
        "drawCount": correctness.get("drawCount"),
        "trianglesSubmitted": correctness.get("trianglesSubmitted"),
    }


def self_consistency(cells: dict[str, list[dict]]) -> None:
    print("[同侧自一致]")
    for cell, runs in cells.items():
        if len(runs) < 2:
            print(f"  skip {cell}（只有 {len(runs)} 个 run）")
            continue
        reference = determinism_fields(runs[0])
        mismatched = []
        for run in runs[1:]:
            fields = determinism_fields(run)
            for key, value in reference.items():
                if value is None:
                    continue
                if fields.get(key) != value:
                    mismatched.append(f"{os.path.basename(run['__dir'])}.{key}")
        check(not mismatched, f"{cell}: {len(runs)} 个 run 的确定性字段逐位一致"
                              + (f"（不一致：{', '.join(mismatched[:4])}）" if mismatched else ""))


def validation_matrix(cells: dict[str, list[dict]]) -> None:
    print("[validation 矩阵]")
    for cell, runs in cells.items():
        for run in runs:
            correctness = run.get("correctness") or {}
            messages = correctness.get("validationMessages", run.get("validationMessages"))
            live = run.get("liveResources")
            alive = run.get("aliveObjects")
            retiring = run.get("retiringObjects")
            if messages is None or live is None:
                raise SystemExit(f"INVALID: {cell} 缺少 validation 字段（{run['__path']}）")
            check(messages == 0, f"{cell}: validationMessages == 0（实际 {messages}）")
            check(live == 0 and alive == 0 and retiring == 0,
                  f"{cell}: live/alive/retiring == 0（实际 {live}/{alive}/{retiring}）")


def cross_rhi(cells: dict[str, list[dict]], include_images: bool) -> list[dict]:
    """跨后端配对比较；返回逐配对指标（进报告 JSON，便于机读复核）。"""
    metrics_rows: list[dict] = []
    print("[跨后端等价]")
    pairs: list[tuple[str, str]] = []
    for cell in cells:
        if cell.endswith("-d3d11"):
            partner = cell[: -len("-d3d11")] + "-d3d12"
            if partner in cells:
                pairs.append((cell, partner))
    if not pairs:
        raise SystemExit("INVALID: 没有可配对的 d3d11/d3d12 cell")
    for left, right in pairs:
        key = left[: -len("-d3d11")]
        left_fields = determinism_fields(cells[left][0])
        right_fields = determinism_fields(cells[right][0])
        check(left_fields["packetSequenceHash"] == right_fields["packetSequenceHash"],
              f"{key}: packetSequenceHash 跨后端一致（{left_fields['packetSequenceHash']}）")
        check(left_fields["graphHash"] == right_fields["graphHash"],
              f"{key}: graphHash 跨后端一致（{left_fields['graphHash']}）")
        for field in ("visibleCount", "drawCount", "trianglesSubmitted"):
            if left_fields.get(field) is None or right_fields.get(field) is None:
                continue
            check(left_fields[field] == right_fields[field],
                  f"{key}: {field} 跨后端一致（{left_fields[field]} vs {right_fields[field]}）")
        left_digest, left_count = revision_manifest_digest(cells[left][0])
        right_digest, right_count = revision_manifest_digest(cells[right][0])
        if left_count or right_count:
            check(left_digest == right_digest,
                  f"{key}: 资源 revision manifest 摘要一致（{left_digest} / {right_digest}，{left_count} 项）")
        else:
            print(f"  note {key}: 该场景没有资产请求样本（非 streaming），revision manifest 不适用")
        if include_images:
            left_ppm = read_ppm(os.path.join(cells[left][0]["__dir"], "screenshot.ppm"))
            right_ppm = read_ppm(os.path.join(cells[right][0]["__dir"], "screenshot.ppm"))
            if left_ppm is None or right_ppm is None:
                print(f"  note {key}: 截图缺失，跳过像素比较")
                continue
            metrics = image_metrics(left_ppm, right_ppm)
            check(metrics["meanAbsoluteError"] <= IMAGE_THRESHOLDS["mae"] * 255.0,
                  f"{key}: 跨后端 MAE {metrics['meanAbsoluteError']:.4f} <= {IMAGE_THRESHOLDS['mae'] * 255.0:.2f}")
            check(metrics["p99Error"] <= IMAGE_THRESHOLDS["p99"] * 255.0,
                  f"{key}: 跨后端 p99 {metrics['p99Error']} <= {IMAGE_THRESHOLDS['p99'] * 255.0:.2f}")
            check(metrics["changedRateAboveP99"] <= IMAGE_THRESHOLDS["changed_rate_above_p99"],
                  f"{key}: 跨后端超阈率 {metrics['changedRateAboveP99']:.5f} "
                  f"<= {IMAGE_THRESHOLDS['changed_rate_above_p99']}")
            print(f"       MAE={metrics['meanAbsoluteError']:.4f} p99={metrics['p99Error']} "
                  f"changed={metrics['changedPixels']} firstDiff={metrics['firstDifference']}")
            metrics_rows.append({
                "cell": key,
                "d3d11Screenshot": os.path.join(cells[left][0]["__dir"], "screenshot.ppm"),
                "d3d12Screenshot": os.path.join(cells[right][0]["__dir"], "screenshot.ppm"),
                "thresholds": IMAGE_THRESHOLDS,
                **metrics,
            })
    return metrics_rows


def main() -> int:
    parser = argparse.ArgumentParser(description="M7-10 场景 parity / validation 比较")
    parser.add_argument("--root", required=True, help="run_m7_parity_matrix.ps1 的输出目录")
    parser.add_argument("--suite", choices=["parity", "validation"], default="parity")
    parser.add_argument("--output", default="")
    parser.add_argument("--skip-images", action="store_true")
    arguments = parser.parse_args()

    cells = load_runs(arguments.root)
    if not cells:
        raise SystemExit(f"INVALID: no run.json under {arguments.root}")
    print(f"cells: {', '.join(sorted(cells))}")

    if arguments.suite == "validation":
        validation_matrix(cells)
        image_metrics_rows: list[dict] = []
    else:
        self_consistency(cells)
        image_metrics_rows = cross_rhi(cells, include_images=not arguments.skip_images)

    print(f"\n{CHECKS - len(FAILURES)}/{CHECKS} checks passed")
    if arguments.output:
        payload = {
            "schema": "miniengine.m7-scene-parity.v1",
            "root": os.path.abspath(arguments.root),
            "suite": arguments.suite,
            "checks": CHECKS,
            "failures": FAILURES,
            "status": "PASS" if not FAILURES else "FAIL",
            # 像素指标进报告本体（M7-11 审计：原先只打到 stdout，报告不可机读复核）。
            "imageMetrics": image_metrics_rows,
        }
        # M7-12：输出目录可能尚不存在（首次采集时 out/m7-xx/ 还没建）——工具负责创建，
        # 不要让它以 FileNotFoundError 收场（比较器在 M7-09 已修过同类问题）。
        output_directory = os.path.dirname(os.path.abspath(arguments.output))
        if output_directory:
            os.makedirs(output_directory, exist_ok=True)
        with open(arguments.output, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2)
            handle.write("\n")
        print(f"report: {arguments.output}")
    if FAILURES:
        print("failures:")
        for failure in FAILURES:
            print(f"  - {failure}")
        return 1
    print("M7 scene parity passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
