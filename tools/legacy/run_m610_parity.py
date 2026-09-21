#!/usr/bin/env python3
"""Run the M6-10 RHI parity matrix.

Each backend and debug view is sampled selfRuns times first. Cross-backend checks
are started only after every self run has complete evidence. The default command
is the new RHI CLI supplied by the M6-10 acceptance contract.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/legacy"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/validation"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/capture"))
from typing import Any

_THIS = Path(__file__).resolve()
_REPO = _THIS.parents[2]
sys.path.insert(0, str(_THIS.parent))
from check_m610_parity import (  # noqa: E402
    IDENTITY_KEYS,
    compare_artifacts,
    compare_legacy_to_rhi,
    inspect_legacy_artifact,
    first_difference,
    image_metrics,
    inspect_artifact,
    load_recipe,
    sha256_bytes,
)


def find_executable(explicit: Path | None) -> Path:
    if explicit:
        candidate = explicit if explicit.is_absolute() else (_REPO / explicit)
        if not candidate.is_file():
            raise FileNotFoundError(f"MiniEngineSandbox executable not found: {candidate}")
        return candidate.resolve()
    candidates = [
        _REPO / "out/build/windows-msvc-debug/samples/rhi_sandbox/Debug/MiniEngineSandbox.exe",
        _REPO / "out/build/windows-msvc-debug/samples/rhi_sandbox/Release/MiniEngineSandbox.exe",
        _REPO / "out/build/windows-msvc-debug/samples/rhi_sandbox/RelWithDebInfo/MiniEngineSandbox.exe",
        _REPO / "out/build/windows-msvc-debug/samples/rhi_sandbox/MiniEngineSandbox.exe",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError(
        "MiniEngineSandbox.exe was not found; pass --exe with the built RHI sandbox"
    )


DEFAULT_DEBUG_VIEWS = (1, 2, 3, 4, 5, 6, 8, 7, 10, 0)


def parse_views(value: str) -> list[int]:
    result = []
    for item in value.split(","):
        try:
            view = int(item.strip(), 10)
        except ValueError as error:
            raise ValueError(f"invalid debug view: {item}") from error
        if view < 0:
            raise ValueError("debug views must be non-negative")
        result.append(view)
    if not result:
        raise ValueError("at least one debug view is required")
    if len(set(result)) != len(result):
        raise ValueError("debug views must be unique")
    return result


def self_compare(left: dict[str, Any], right: dict[str, Any], recipe: dict[str, Any]) -> dict[str, Any]:
    """Compare one backend/view and require exact RGB bytes for determinism."""
    result = compare_artifacts(left, right, recipe, require_distinct_backend=False)
    if result.get("status") == "BLOCKED":
        return result
    left_image, right_image = left.get("image"), right.get("image")
    exact = (
        isinstance(left_image, tuple) and isinstance(right_image, tuple)
        and left_image[0:2] == right_image[0:2]
        and left_image[2] == right_image[2]
    )
    result["exactRgbMatch"] = exact
    if not exact:
        result.setdefault("failures", []).append("self-consistency RGB bytes differ")
        result["status"] = "FAIL"
    return result


def _manifest_value(value: Any, key: str | None = None) -> Any:
    if key in {"graph", "access", "transient", "semanticTrace"}:
        return None
    if key == "image" and isinstance(value, tuple) and len(value) == 3 and isinstance(value[2], bytes):
        return {
            "resolution": [value[0], value[1]],
            "rgbSha256": sha256_bytes(value[2]),
            "rgbBytes": len(value[2]),
        }
    if isinstance(value, dict):
        return {name: _manifest_value(item, name) for name, item in value.items()
                if name not in {"graph", "access", "transient", "semanticTrace"}}
    if isinstance(value, list):
        return [_manifest_value(item) for item in value]
    if isinstance(value, tuple):
        return [_manifest_value(item) for item in value]
    if isinstance(value, bytes):
        return {"sha256": sha256_bytes(value), "bytes": len(value)}
    return value


def _write_checkpoint(root: Path, phase: str, **values: Any) -> None:
    payload = {
        "schema": "miniengine.m6-10.parity-checkpoint.v1",
        "phase": phase,
        "updatedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        **values,
    }
    (root / "m610-checkpoint.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def _write_invocation(output: Path, invocation: dict[str, Any]) -> None:
    (output / "invocation.json").write_text(
        json.dumps(invocation, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def run_legacy_one(
    executable: Path,
    backend: str,
    view: int,
    output: Path,
    manifest: Path,
    width: int,
    height: int,
    fixed_frame: int,
    timeout: float,
    dry_run: bool,
    extra_args: list[str],
    wrapper: bool = True,
    recipe: dict[str, Any] | None = None,
    manifest_sha256: str | None = None,
) -> dict[str, Any]:
    output.mkdir(parents=True, exist_ok=False)
    if wrapper:
        # The current sandbox owns all four selectors. In particular, do not add
        # --rhi to a legacy invocation: ParseRhiOptions rejects that combination.
        args = [
            f"--renderer=legacy-{backend}",
            "--scene=m4-visual-baseline",
            f"--fixed-frame={fixed_frame}",
            f"--debug-view={view}",
            "--exposure=0",
            f"--width={width}",
            f"--height={height}",
            f"--manifest={manifest}",
            f"--output={output}",
        ]
    else:
        default_args = [
            "--fixed-camera",
            f"--fixed-frame={fixed_frame}",
            f"--capture={output / 'color.png'}",
            f"--capture-metadata={output / 'color.png.json'}",
            f"--width={width}",
            f"--height={height}",
            "--vsync=0",
            f"--manifest={manifest}",
        ]
        values = {
            "backend": backend, "debug_view": str(view), "debugView": str(view),
            "output": str(output), "manifest": str(manifest), "width": str(width),
            "height": str(height), "fixed_frame": str(fixed_frame), "fixedFrame": str(fixed_frame),
        }
        args = [item.format_map(values) for item in (extra_args if extra_args else default_args)]
    command = [str(executable), *args]
    report: dict[str, Any] = {
        "path": f"legacy-{backend}", "backend": backend, "debugView": view,
        "executable": str(executable), "command": command, "output": str(output),
        "renderer": f"legacy-{backend}", "wrapper": wrapper,
    }
    if dry_run:
        report["status"] = "DRY-RUN"
        _write_invocation(output, report)
        return report
    _write_invocation(output, report)
    try:
        environment = os.environ.copy()
        environment["MINIENGINE_DEBUG_MODE"] = str(view)
        completed = subprocess.run(
            command, cwd=str(_REPO), env=environment, capture_output=True, text=True,
            errors="replace", timeout=timeout, check=False,
        )
        report["returnCode"] = completed.returncode
        _write_invocation(output, report)
        (output / "process.stdout.txt").write_text(completed.stdout, encoding="utf-8")
        (output / "process.stderr.txt").write_text(completed.stderr, encoding="utf-8")
        if completed.returncode != 0:
            report["status"] = "BLOCKED"
            report["problems"] = ["legacy runtime returned non-zero; see process stderr"]
            return report
        if not (output / "color.png").is_file() or not (output / "color.png.json").is_file():
            report["status"] = "BLOCKED"
            report["problems"] = ["legacy runtime exited without color.png and color.png.json"]
            return report
        inspected = inspect_legacy_artifact(output, recipe, manifest_sha256)
        report["status"] = inspected["status"]
        report["report"] = inspected
        if inspected.get("problems"):
            report["problems"] = inspected["problems"]
    except subprocess.TimeoutExpired as error:
        report["status"] = "BLOCKED"
        report["problems"] = ["legacy runtime timeout"]
        (output / "process.stdout.txt").write_text(str(error.stdout or ""), encoding="utf-8")
        (output / "process.stderr.txt").write_text(str(error.stderr or ""), encoding="utf-8")
        _write_invocation(output, report)
    return report


def _status(records: list[dict[str, Any]]) -> str:
    statuses = [record.get("status") for record in records]
    if "FAIL" in statuses:
        return "FAIL"
    if "BLOCKED" in statuses:
        return "BLOCKED"
    if "PARTIAL" in statuses:
        return "PARTIAL"
    return "PASS"


def run_one(
    executable: Path,
    backend: str,
    view: int,
    output: Path,
    manifest: Path,
    width: int,
    height: int,
    fixed_frame: int,
    timeout: float,
    dry_run: bool,
) -> tuple[dict[str, Any], dict[str, Any] | None]:
    output.mkdir(parents=True, exist_ok=False)
    command = [
        str(executable),
        "--renderer=rhi",
        f"--rhi={backend}",
        "--scene=m4-visual-baseline",
        "--migration-level=9",
        f"--width={width}",
        f"--height={height}",
        f"--fixed-frame={fixed_frame}",
        f"--debug-view={view}",
        "--headless",
        "--debug",
        f"--manifest={manifest}",
        f"--output={output}",
    ]
    invocation: dict[str, Any] = {
        "executable": str(executable),
        "backend": backend,
        "debugView": view,
        "command": command,
        "commandLine": " ".join(shlex.quote(item) for item in command),
        "manifest": str(manifest),
        "output": str(output),
    }
    if dry_run:
        _write_invocation(output, invocation)
        return invocation, None
    _write_invocation(output, invocation)
    try:
        completed = subprocess.run(
            command, cwd=str(_REPO), capture_output=True, text=True, errors="replace",
            timeout=timeout, check=False,
        )
        invocation["returnCode"] = completed.returncode
        _write_invocation(output, invocation)
        (output / "process.stdout.txt").write_text(completed.stdout, encoding="utf-8")
        (output / "process.stderr.txt").write_text(completed.stderr, encoding="utf-8")
        invocation["stdoutFile"] = str(output / "process.stdout.txt")
        invocation["stderrFile"] = str(output / "process.stderr.txt")
    except subprocess.TimeoutExpired as error:
        invocation["returnCode"] = None
        invocation["timeout"] = timeout
        (output / "process.stdout.txt").write_text(str(error.stdout or ""), encoding="utf-8")
        (output / "process.stderr.txt").write_text(str(error.stderr or ""), encoding="utf-8")
        _write_invocation(output, invocation)
        return invocation, {"status": "BLOCKED", "directory": str(output), "problems": ["runtime timeout"]}
    if completed.returncode != 0:
        return invocation, {
            "status": "BLOCKED", "directory": str(output),
            "problems": [f"runtime returned {completed.returncode}; see process stderr"],
        }
    return invocation, None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--legacy", action="store_true",
                        help="run legacy A/B comparisons; requires explicit --legacy-exe because the retired "
                             "legacy samples only exist in git history (LEGACY-SAMPLES-RETIRE)")
    parser.add_argument("--legacy-exe", action="append", type=Path,
                        help="optional concrete legacy executable(s), d3d11 then d3d12")
    parser.add_argument("--legacy-arg", action="append", default=[],
                        help="legacy argument token for a concrete executable")
    parser.add_argument("--manifest", type=Path, default=Path("out/m4-09/scene/manifest.json"))
    parser.add_argument("--recipe", type=Path, default=Path("assets/recipes/m6-graph-baseline.json"))
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--backend", action="append", choices=("d3d11", "d3d12"))
    parser.add_argument("--views", default=','.join(str(view) for view in DEFAULT_DEBUG_VIEWS),
                        help="comma-separated debug-view values; default is the ten M5 views")
    parser.add_argument("--self-runs", type=int)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fixed-frame", type=int, default=300)
    parser.add_argument("--timeout-seconds", type=float, default=180.0)
    parser.add_argument("--dry-run", action="store_true",
                        help="write the command matrix without launching the runtime")
    args = parser.parse_args(argv)
    try:
        executable = find_executable(args.exe)
        legacy_executables: list[Path | None] = []
        for legacy in args.legacy_exe or []:
            candidate = legacy if legacy.is_absolute() else _REPO / legacy
            if not candidate.is_file():
                raise FileNotFoundError(f"legacy executable not found: {candidate}")
            legacy_executables.append(candidate.resolve())
        if len(legacy_executables) > 2:
            raise ValueError("--legacy-exe may be given at most twice (d3d11 then d3d12)")
        if args.legacy and not any(legacy_executables):
            raise ValueError("legacy A/B renderer retired (LEGACY-SAMPLES-RETIRE); the executables only exist in "
                             "git history, so --legacy requires explicit --legacy-exe pointing at restored copies")
        while len(legacy_executables) < 2:
            legacy_executables.append(None)
        manifest = args.manifest if args.manifest.is_absolute() else _REPO / args.manifest
        manifest = manifest.resolve()
        if not manifest.is_file():
            raise FileNotFoundError(f"manifest not found: {manifest}")
        recipe_path = args.recipe if args.recipe.is_absolute() else _REPO / args.recipe
        recipe = load_recipe(recipe_path.resolve())
        backends = args.backend or ["d3d11", "d3d12"]
        if len(backends) != 2 or set(backends) != {"d3d11", "d3d12"}:
            raise ValueError("parity requires exactly two distinct backends: d3d11 and d3d12")
        views = parse_views(args.views)
        self_runs = args.self_runs if args.self_runs is not None else int(recipe.get("rhiSelfConsistencyRuns", 10))
        if self_runs < 1:
            raise ValueError("--self-runs must be positive")
        contract_problems: list[str] = []
        required_self_runs = max(10, int(recipe.get("rhiSelfConsistencyRuns", 10)))
        if self_runs < required_self_runs:
            contract_problems.append(f"self consistency has {self_runs} runs; {required_self_runs} required")
        if set(views) != set(DEFAULT_DEBUG_VIEWS) or len(views) != len(DEFAULT_DEBUG_VIEWS):
            contract_problems.append("debug view matrix is incomplete; all ten M5 views are required")
        contract_complete = not contract_problems
        if args.width < 1 or args.height < 1 or args.fixed_frame < 1:
            raise ValueError("width, height and fixed-frame must be positive")
        output_root = args.output_root
        if output_root is None:
            stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
            output_root = _REPO / f"out/m6-10/rhi-parity-{stamp}"
        elif not output_root.is_absolute():
            output_root = _REPO / output_root
        output_root = output_root.resolve()
        if output_root.exists() and any(output_root.iterdir()):
            raise FileExistsError(f"output root is not empty: {output_root}")
        output_root.mkdir(parents=True, exist_ok=False)
        manifest_hash = sha256_bytes(manifest.read_bytes())
        records: list[dict[str, Any]] = []
        representatives: list[dict[str, Any]] = []
        self_outcomes: list[dict[str, Any]] = []
        self_ok = True
        # Complete the whole RHI matrix before any legacy or cross-backend work.
        for backend in backends:
            for view in views:
                baseline: dict[str, Any] | None = None
                first_output = output_root / "rhi" / backend / f"view-{view}" / "self-01"
                for run_index in range(self_runs):
                    output = output_root / "rhi" / backend / f"view-{view}" / f"self-{run_index + 1:02d}"
                    invocation, runtime_report = run_one(
                        executable, backend, view, output, manifest, args.width, args.height,
                        args.fixed_frame, args.timeout_seconds, args.dry_run,
                    )
                    if args.dry_run:
                        report: dict[str, Any] = {"status": "DRY-RUN", "directory": str(output)}
                    elif runtime_report:
                        report = runtime_report
                    else:
                        report = inspect_artifact(output, recipe, manifest_hash)
                    invocation["reportStatus"] = report["status"]
                    if run_index == 0:
                        baseline = report
                        if report.get("status") not in ("PASS", "DRY-RUN"):
                            self_ok = False
                    elif args.dry_run:
                        pass
                    elif baseline and baseline.get("status") == "PASS" and report.get("status") == "PASS":
                        consistency = self_compare(baseline, report, recipe)
                        report["selfConsistency"] = consistency
                        if consistency["status"] != "PASS":
                            self_ok = False
                    else:
                        self_ok = False
                    if report.get("status") not in ("PASS", "DRY-RUN"):
                        self_ok = False
                    self_outcomes.append({"backend": backend, "debugView": view, "run": run_index + 1, "report": report})
                    records.append({"backend": backend, "debugView": view, "run": run_index + 1,
                                    "invocation": invocation, "report": _manifest_value(report)})
                    _write_checkpoint(
                        output_root, "rhi-self", backend=backend, debugView=view,
                        run=run_index + 1, status=report.get("status"),
                        recordedRuns=len(records), selfRuns=self_runs,
                    )
                    print(
                        f"[M6-10] RHI self {backend} view={view} "
                        f"run={run_index + 1}/{self_runs} status={report.get('status')}",
                        flush=True,
                    )
                representatives.append({"backend": backend, "debugView": view,
                                        "representative": str(first_output.resolve())})
        legacy_records: list[dict[str, Any]] = []
        legacy_run_results: list[dict[str, Any]] = []
        legacy_comparisons: list[dict[str, Any]] = []
        legacy_skipped: dict[str, Any] | None = None
        if not args.legacy:
            # 2026-09-16：旧 concrete samples 退役（LEGACY-SAMPLES-RETIRE）。legacy 对照默认跳过，
            # 显式 --legacy 且提供 --legacy-exe 时才尝试（退役 exes 只存在于 git 历史）。
            legacy_skipped = {"status": "SKIPPED",
                              "reason": "legacy A/B renderer retired (LEGACY-SAMPLES-RETIRE); pass --legacy to opt in"}
        elif args.dry_run or self_ok:
            for index, backend in enumerate(backends):
                legacy_executable = legacy_executables[index] or executable
                wrapper = legacy_executables[index] is None
                for view in views:
                    output = output_root / "legacy" / backend / f"view-{view}"
                    legacy_record = run_legacy_one(
                        legacy_executable, backend, view, output, manifest, args.width,
                        args.height, args.fixed_frame, args.timeout_seconds, args.dry_run,
                        args.legacy_arg, wrapper=wrapper, recipe=recipe,
                        manifest_sha256=manifest_hash,
                    )
                    legacy_records.append(legacy_record)
                    _write_checkpoint(
                        output_root, "legacy", backend=backend, debugView=view,
                        status=legacy_record.get("status"),
                        completedRuns=len(legacy_records),
                    )
                    print(
                        f"[M6-10] legacy {backend} view={view} "
                        f"status={legacy_record.get('status')}", flush=True,
                    )
            if not args.dry_run:
                for record in legacy_records:
                    if record.get("status") not in ("PASS", "DRY-RUN"):
                        legacy_run_results.append({
                            "phase": "legacy", "backend": record.get("backend"),
                            "debugView": record.get("debugView"),
                            "status": record.get("status", "BLOCKED"),
                            "reason": record.get("problems", record.get("reason", "legacy run did not produce evidence")),
                        })
                        continue
                    representative = next(item for item in representatives
                                          if item["backend"] == record["backend"] and item["debugView"] == record["debugView"])
                    rhi_report = inspect_artifact(Path(representative["representative"]), recipe, manifest_hash)
                    legacy_report = record.get("report")
                    if legacy_report is None:
                        legacy_report = inspect_legacy_artifact(Path(record["output"]), recipe, manifest_hash)
                    legacy_comparisons.append({
                        "backend": record["backend"], "debugView": record["debugView"],
                        **compare_legacy_to_rhi(rhi_report, legacy_report, recipe),
                    })
            else:
                legacy_run_results = [{"phase": "legacy", "status": "DRY-RUN"}]
        else:
            legacy_records = [{"status": "BLOCKED", "reason": "RHI self-consistency did not pass; legacy comparison was not started"}]
            legacy_run_results = [{"phase": "legacy", "status": "BLOCKED", "reason": legacy_records[0]["reason"]}]
        cross: list[dict[str, Any]] = []
        if args.dry_run:
            overall = "DRY-RUN"
        elif not self_ok:
            cross = [{"status": "BLOCKED", "reason": "all RHI self-consistency runs must pass before cross-backend comparison"}]
            overall = _status([item["report"] for item in self_outcomes])
            if overall == "PASS":
                overall = "BLOCKED"
        else:
            for view in views:
                left = next(item for item in representatives if item["backend"] == backends[0] and item["debugView"] == view)
                right = next(item for item in representatives if item["backend"] == backends[1] and item["debugView"] == view)
                left_report = inspect_artifact(Path(left["representative"]), recipe, manifest_hash)
                right_report = inspect_artifact(Path(right["representative"]), recipe, manifest_hash)
                cross_result = {"debugView": view, **compare_artifacts(left_report, right_report, recipe)}
                cross.append(cross_result)
                _write_checkpoint(
                    output_root, "cross-backend", debugView=view,
                    status=cross_result.get("status"), completedViews=len(cross),
                )
                print(
                    f"[M6-10] cross d3d11/d3d12 view={view} "
                    f"status={cross_result.get('status')}", flush=True,
                )
            all_results = [item for item in legacy_run_results] + [item for item in legacy_comparisons] + cross
            overall = _status(all_results) if all_results else "BLOCKED"
            if overall == "PASS" and not contract_complete:
                overall = "PARTIAL"
        run_manifest = {
            "schema": "miniengine.m6-10.parity-run.v2",
            "status": overall,
            "executable": str(executable), "manifest": str(manifest), "manifestSha256": manifest_hash,
            "recipe": str(recipe_path.resolve()), "backends": backends, "debugViews": views,
            "rhiSelfConsistencyRuns": self_runs, "fixedFrame": args.fixed_frame,
            "resolution": [args.width, args.height], "selfRunsCompletedBeforeCross": True,
            "crossBackendStartedAfterSelfPass": self_ok,
            "contractComplete": contract_complete, "contractProblems": contract_problems,
            "representatives": representatives, "records": records,
            "legacy": [_manifest_value(item) for item in legacy_records],
            "legacyRunResults": legacy_run_results,
            "legacyComparisons": [_manifest_value(item) for item in legacy_comparisons],
            "legacySkipped": legacy_skipped,
            "crossBackend": cross,
        }
        (output_root / "m610-run.json").write_text(json.dumps(run_manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({
            "schema": run_manifest["schema"], "status": overall,
            "contractComplete": contract_complete, "rhiSelfConsistencyRuns": self_runs,
            "debugViews": views, "legacyComparisons": len(legacy_comparisons),
            "legacySkipped": legacy_skipped is not None,
            "crossBackend": len(cross), "output": str(output_root),
        }, ensure_ascii=False, indent=2), flush=True)
        return 0 if overall in ("PASS", "DRY-RUN") else (1 if overall == "FAIL" else 2)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps({"schema": "miniengine.m6-10.parity-run.v2", "status": "BLOCKED", "problems": [str(error)]}, ensure_ascii=False, indent=2))
        return 2


if __name__ == "__main__":
    sys.exit(main())
