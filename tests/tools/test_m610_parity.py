''"""Persistent CPU contract tests for the M6-10 parity tools.

These tests only construct evidence files; they never launch a renderer or touch a GPU.
"""
from __future__ import annotations

import copy
import hashlib
import json
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/legacy"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/validation"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/capture"))

ROOT = Path(__file__).parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from check_m610_parity import (  # noqa: E402
    compare_legacy_to_rhi,
    compare_artifacts,
    inspect_artifact,
    inspect_legacy_artifact,
    sha256_bytes,
    write_png,
)
import run_m610_parity  # noqa: E402
from run_m610_parity import self_compare  # noqa: E402


RECIPE = {
    "scene": "m4-visual-baseline",
    "requiredPasses": ["Shadow", "ForwardHDR", "Skybox", "ToneMap", "Present"],
    "transientResources": ["SceneDepth", "HdrColor"],
    "imageThresholds": {"mae": 2 / 255, "p99": 5 / 255, "changedRateAboveP99": 0.005},
}
PLAN_HASH = "0123456789abcdef"
ASSET_SHA = "a" * 64
ENV_SHA = "b" * 64
SHADER_SHA = "c" * 64


def _graph(kind: str) -> dict:
    base = {
        "schemaVersion": 1,
        "kind": kind,
        "frame": 300,
        "build": "Debug",
        "commit": "test",
        "planHash": PLAN_HASH,
    }
    if kind == "framegraph":
        base.update({
            "importOwnership": [], "statistics": {}, "stages": [], "executionOrder": [0, 1, 2, 3, 4],
            "passes": [{"declaration": i, "name": name, "executionOrder": i, "live": True}
                       for i, name in enumerate(RECIPE["requiredPasses"])],
            "resources": [{"resource": 0, "name": "SceneDepth"}, {"resource": 1, "name": "HdrColor"}, {"resource": 2, "name": "BackBuffer", "finalAccess": "Present"}],
            "versions": [], "edges": [], "roots": [], "lifetimes": [], "physicalAllocations": [], "transitions": [],
        })
    elif kind == "access-plan":
        base["transitions"] = []
    else:
        base.update({"policy": "stable_first_fit_exact_descriptor_whole_resource_lane_pool", "lifetimes": [], "physicalAllocations": []})
    return base


def _write_ppm(path: Path, pixels: bytes = bytes((0, 0, 0, 10, 10, 10))) -> None:
    path.write_bytes(b"P6\n2 1\n255\n" + pixels)


def _write_rhi(directory: Path, backend: str = "d3d11", pixels: bytes | None = None) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    _write_ppm(directory / "color.ppm", pixels or bytes((0, 0, 0, 10, 10, 10)))
    trace = b"Frame 300\nPresent BackBuffer\n"
    (directory / "semantic-trace.txt").write_bytes(trace)
    (directory / "native-trace.txt").write_text("native begin\nnative end\n", encoding="utf-8")
    (directory / "frames.csv").write_text("frame,measured\n300,0\n", encoding="utf-8")
    graph = _graph("framegraph")
    access = _graph("access-plan")
    transient = _graph("transient-plan")
    for name, value in (("m6-framegraph.json", graph), ("m6-access-plan.json", access), ("m6-transient-plan.json", transient)):
        (directory / name).write_text(json.dumps(value), encoding="utf-8")
    (directory / "m6-framegraph.dot").write_text("digraph FrameGraph {\n  p0;\n}\n", encoding="utf-8")
    metadata = {
        "schemaVersion": 1, "renderer": "rhi", "backend": backend, "scene": "m4-visual-baseline",
        "width": 2, "height": 1, "resolution": {"width": 2, "height": 1}, "fixedTick": 300,
        "assetManifestSha256": ASSET_SHA, "environmentArtifactSha256": ENV_SHA,
        "shaderSemanticSha256": SHADER_SHA, "visibleSequenceHash": "0x1111111111111111",
        "cameraValues": "camera", "lightValues": "light", "debugView": "5", "exposureEv": 0,
        "iblProfile": "baseline", "iblEnabled": True, "toneMapper": "reinhard",
        "culling": {"main": True, "shadow": True}, "shadow": {"resolution": 2048},
        "skyboxEnabled": True, "migrationLevel": 9, "graphHash": "0x" + PLAN_HASH.upper(),
        "commandHash": hashlib.sha256(trace).hexdigest(), "warningErrors": 0, "fallbackUsed": False,
    }
    (directory / "metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
    return inspect_artifact(directory, RECIPE)


def _write_legacy(directory: Path, backend: str = "D3D11") -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    pixels = bytes((0, 0, 0, 10, 10, 10))
    write_png(directory / "color.png", 2, 1, pixels)
    metadata = {
        "schemaVersion": 1, "backend": backend,
        "scene": "fixture-root/out/scene/manifest.json",
        "resolution": [2, 1], "fixedTick": 300, "assetManifestSha256": ASSET_SHA,
        "environmentArtifactSha256": ENV_SHA, "iblProfile": "baseline", "exposureEv": 0,
        "toneMapper": "reinhard", "shaderSemanticSha256": SHADER_SHA,
        "visibleSequenceHash": "0x1111111111111111", "cameraValues": "camera", "lightValues": "light",
        "debugView": "5", "skyboxEnabled": True, "culling": {"main": True, "shadow": True},
        "shadow": {"resolution": 2048},
        "pngSha256": hashlib.sha256((directory / "color.png").read_bytes()).hexdigest(),
    }
    (directory / "color.png.json").write_text(json.dumps(metadata), encoding="utf-8")
    return inspect_legacy_artifact(directory, RECIPE)


class M610ParityToolTests(unittest.TestCase):
    def test_legacy_shader_identity_missing_or_different_blocks(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            rhi = _write_rhi(root / "rhi")
            directory = root / "legacy"
            _write_legacy(directory)
            metadata_path = directory / "color.png.json"
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            metadata["shaderSemanticSha256"] = "d" * 64
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            mismatch = compare_legacy_to_rhi(rhi, inspect_legacy_artifact(directory, RECIPE), RECIPE)
            self.assertEqual(mismatch["status"], "BLOCKED")
            self.assertIn("shaderSemanticSha256", [item["key"] for item in mismatch["identityDifferences"]])
            del metadata["shaderSemanticSha256"]
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            self.assertEqual(inspect_legacy_artifact(directory, RECIPE)["status"], "BLOCKED")

    def test_graphdump_plan_hash_and_identity_are_checked(self):
        with tempfile.TemporaryDirectory() as raw:
            report = _write_rhi(Path(raw))
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(report["graphHashBasis"], "graph-dump-planHash")

    def test_binary_pipeline_key_trace_is_lossless_and_byte_compared(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            left_dir = root / "left"
            right_dir = root / "right"
            _write_rhi(left_dir)
            _write_rhi(right_dir)
            payload = b"SetPipeline r1 7:178 i0 f0 t669:M6.GraphicsPipeline.v1@\x00\x00\xd0\x00\x01\n"
            for directory in (left_dir, right_dir):
                trace_path = directory / "semantic-trace.txt"
                trace = trace_path.read_bytes() + payload
                trace_path.write_bytes(trace)
                metadata_path = directory / "metadata.json"
                metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
                metadata["commandHash"] = hashlib.sha256(trace).hexdigest()
                metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            left = inspect_artifact(left_dir, RECIPE)
            right_trace_path = right_dir / "semantic-trace.txt"
            right_trace = right_trace_path.read_bytes().replace(b"\xd0", b"\xd1", 1)
            right_trace_path.write_bytes(right_trace)
            right_metadata_path = right_dir / "metadata.json"
            right_metadata = json.loads(right_metadata_path.read_text(encoding="utf-8"))
            right_metadata["commandHash"] = hashlib.sha256(right_trace).hexdigest()
            right_metadata_path.write_text(json.dumps(right_metadata), encoding="utf-8")
            right = inspect_artifact(right_dir, RECIPE)
            self.assertEqual(left["status"], "PASS")
            self.assertEqual(right["status"], "PASS")
            self.assertEqual(left["semanticTrace"].encode("latin-1"), (left_dir / "semantic-trace.txt").read_bytes())
            self.assertEqual(left["semanticTraceSha256"], hashlib.sha256((left_dir / "semantic-trace.txt").read_bytes()).hexdigest())
            result = compare_artifacts(left, right, RECIPE, require_distinct_backend=False)
            self.assertEqual(result["status"], "FAIL")
            self.assertEqual(result["traceDifference"]["leftByte"], 0xD0)
            self.assertEqual(result["traceDifference"]["rightByte"], 0xD1)

    def test_missing_identity_is_blocked(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            _write_rhi(directory)
            metadata_path = directory / "metadata.json"
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            del metadata["iblProfile"]
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            self.assertEqual(inspect_artifact(directory, RECIPE)["status"], "BLOCKED")

    def test_structural_difference_is_fail_even_with_matching_image(self):
        with tempfile.TemporaryDirectory() as raw:
            left = _write_rhi(Path(raw) / "left")
            right = _write_rhi(Path(raw) / "right")
            right = copy.deepcopy(right)
            right["graph"]["resources"][0]["name"] = "WrongResource"
            result = compare_artifacts(left, right, RECIPE)
            self.assertEqual(result["status"], "FAIL")
            self.assertEqual(result["structureDifferences"][0]["dump"], "graph")

    def test_image_threshold_is_fail_without_averaging_first_difference(self):
        with tempfile.TemporaryDirectory() as raw:
            left = _write_rhi(Path(raw) / "left")
            right = _write_rhi(Path(raw) / "right", backend="d3d12", pixels=bytes((40, 40, 40, 10, 10, 10)))
            result = compare_artifacts(left, right, RECIPE)
            self.assertEqual(result["status"], "FAIL")
            self.assertIn("image thresholds exceeded", result["failures"])
            self.assertEqual(result["firstDifference"]["x"], 0)

    def test_self_consistency_blocks_followups_when_evidence_is_blocked(self):
        result = self_compare({"status": "BLOCKED"}, {"status": "PASS"}, RECIPE)
        self.assertEqual(result["status"], "BLOCKED")

    def test_runner_does_not_start_legacy_after_self_consistency_failure(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            manifest = root / "manifest.json"
            manifest.write_text("{}", encoding="utf-8")
            recipe = root / "recipe.json"
            recipe.write_text(json.dumps(RECIPE), encoding="utf-8")
            output = root / "run"
            blocked = {"status": "BLOCKED", "directory": str(root / "evidence"), "problems": ["fixture"]}
            with mock.patch.object(run_m610_parity, "run_one", return_value=({}, blocked)) as run_one_mock, \
                 mock.patch.object(run_m610_parity, "run_legacy_one", side_effect=AssertionError("legacy must be gated")):
                code = run_m610_parity.main([
                    "--exe", sys.executable, "--manifest", str(manifest), "--recipe", str(recipe),
                    "--output-root", str(output), "--views", "0", "--self-runs", "2",
                    "--legacy", "--legacy-exe", sys.executable,
                ])
            self.assertEqual(code, 2)
            self.assertEqual(run_one_mock.call_count, 4)
            report = json.loads((output / "m610-run.json").read_text(encoding="utf-8"))
            self.assertFalse(report["crossBackendStartedAfterSelfPass"])
            self.assertEqual(report["legacy"][0]["status"], "BLOCKED")

    def test_legacy_run_failure_cannot_be_dropped_from_overall_status(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            manifest = root / "manifest.json"
            manifest.write_text("{}", encoding="utf-8")
            recipe = root / "recipe.json"
            recipe.write_text(json.dumps(RECIPE), encoding="utf-8")
            output = root / "run"
            rhi_pass = _write_rhi(root / "rhi")
            legacy_blocked = {"status": "BLOCKED", "backend": "d3d11", "debugView": 0, "problems": ["legacy fixture"]}
            cross_pass = {"status": "PASS"}
            with mock.patch.object(run_m610_parity, "run_one", return_value=({}, rhi_pass)), \
                 mock.patch.object(run_m610_parity, "run_legacy_one", return_value=legacy_blocked), \
                 mock.patch.object(run_m610_parity, "inspect_artifact", return_value=rhi_pass), \
                 mock.patch.object(run_m610_parity, "compare_artifacts", return_value=cross_pass):
                code = run_m610_parity.main([
                    "--exe", sys.executable, "--manifest", str(manifest), "--recipe", str(recipe),
                    "--output-root", str(output), "--views", "0", "--self-runs", "10",
                    "--legacy", "--legacy-exe", sys.executable,
                ])
            self.assertEqual(code, 2)
            report = json.loads((output / "m610-run.json").read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "BLOCKED")
            self.assertFalse(report["contractComplete"])
            self.assertEqual(report["legacyRunResults"][0]["status"], "BLOCKED")
            self.assertTrue(report["crossBackendStartedAfterSelfPass"])

    def test_legacy_comparisons_are_skipped_by_default_after_retirement(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            manifest = root / "manifest.json"
            manifest.write_text("{}", encoding="utf-8")
            recipe = root / "recipe.json"
            recipe.write_text(json.dumps(RECIPE), encoding="utf-8")
            output = root / "run"
            artifact = _write_rhi(root / "artifact")
            with mock.patch.object(run_m610_parity, "run_one", return_value=({}, artifact)), \
                 mock.patch.object(run_m610_parity, "run_legacy_one",
                                   side_effect=AssertionError("legacy must stay retired")), \
                 mock.patch.object(run_m610_parity, "inspect_artifact", return_value=artifact), \
                 mock.patch.object(run_m610_parity, "compare_artifacts", return_value={"status": "PASS"}):
                code = run_m610_parity.main([
                    "--exe", sys.executable, "--manifest", str(manifest), "--recipe", str(recipe),
                    "--output-root", str(output), "--self-runs", "10",
                ])
            result = json.loads((output / "m610-run.json").read_text(encoding="utf-8"))
            self.assertEqual(code, 0)
            self.assertEqual(result["status"], "PASS")
            self.assertTrue(result["legacySkipped"])
            self.assertEqual(result["legacyComparisons"], [])

    def test_partial_matrix_cannot_report_gate_pass(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            manifest = root / "manifest.json"
            manifest.write_text("{}", encoding="utf-8")
            recipe = root / "recipe.json"
            recipe.write_text(json.dumps(RECIPE), encoding="utf-8")
            artifact = _write_rhi(root / "artifact")

            def legacy(executable, backend, view, output, *args, **kwargs):
                return {"status": "PASS", "backend": backend, "debugView": view,
                        "output": str(output), "report": artifact}

            with mock.patch.object(run_m610_parity, "run_one", return_value=({}, artifact)), \
                 mock.patch.object(run_m610_parity, "run_legacy_one", side_effect=legacy), \
                 mock.patch.object(run_m610_parity, "inspect_artifact", return_value=artifact), \
                 mock.patch.object(run_m610_parity, "compare_legacy_to_rhi", return_value={"status": "PASS"}), \
                 mock.patch.object(run_m610_parity, "compare_artifacts", return_value={"status": "PASS"}):
                code = run_m610_parity.main([
                    "--exe", sys.executable, "--manifest", str(manifest), "--recipe", str(recipe),
                    "--output-root", str(root / "run"), "--views", "0", "--self-runs", "1",
                    "--legacy", "--legacy-exe", sys.executable,
                ])
            result = json.loads((root / "run/m610-run.json").read_text(encoding="utf-8"))
            self.assertEqual(code, 2)
            self.assertEqual(result["status"], "PARTIAL")
            self.assertFalse(result["contractComplete"])

    def test_self_consistency_rejects_one_channel_one_step_drift(self):
        with tempfile.TemporaryDirectory() as raw:
            left = _write_rhi(Path(raw) / "left")
            right = _write_rhi(Path(raw) / "right")
            right["image"] = (2, 1, bytes((11, 10, 10, 10, 10, 10)))
            result = self_compare(left, right, RECIPE)
            self.assertEqual(result["status"], "FAIL")
            self.assertFalse(result["exactRgbMatch"])

    def test_legacy_sidecar_needs_no_graph_and_missing_ibl_enabled_is_allowed(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            rhi = _write_rhi(root / "rhi")
            legacy = _write_legacy(root / "legacy")
            self.assertEqual(legacy["status"], "PASS")
            result = compare_legacy_to_rhi(rhi, legacy, RECIPE)
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["optionalLegacyFieldsMissing"], ["iblEnabled"])


if __name__ == "__main__":
    unittest.main()
