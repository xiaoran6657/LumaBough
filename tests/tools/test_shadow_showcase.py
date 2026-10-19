#!/usr/bin/env python3
"""精选展示证据的反例契约；不需要 GPU，不接受自报 PASS 代替原始数据。"""
from __future__ import annotations

import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import time
import unittest


MODULE_PATH = Path(__file__).resolve().parents[2] / "tools" / "acceptance" / "shadow_showcase.py"
SPEC = importlib.util.spec_from_file_location("shadow_showcase", MODULE_PATH)
tool = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = tool
assert SPEC.loader is not None
SPEC.loader.exec_module(tool)
np, Image, _ = tool.dependencies()


class ShadowShowcaseEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "run"
        self.root.mkdir()
        self.params = tool.presets(64, 64)["hard"]
        self.linear = np.ones((64, 64, 4), dtype="<f4") * .2
        self.linear[..., 3] = 1.
        self.diagnostics = np.zeros((64, 64, 4), dtype="<f4")
        self.diagnostics[..., 0] = 1.
        self.metadata = {
            "camera": {"position": [0., 4., -6.], "target": [0., 0., 1.], "up": [0., 1., 0.], "verticalFovDegrees": 55.},
            "light": {"position": [-2., 5., -2.]},
            "geometry": {"boxes": [
                {"min": [-4., -.2, -3.], "max": [4., 0., 6.], "materialId": 0},
                {"min": [-.6, 0., -.2], "max": [.6, 1.5, 1.], "materialId": 1}]}}
        self.result = {
            "schemaVersion": 1, "status": "complete", "runId": "fixture-only-1", "backend": "D3D12-native",
            "params": self.params, "adapter": {"description": "fixture, not a measured GPU", "software": False},
            "debugLayer": {"enabled": True, "errorCount": 0, "warningCount": 0},
            "timing": {"shadowMapGpuMs": .1, "shadeGpuMs": .2, "totalGpuMs": .3,
                       "timestampFrequency": 1000000000, "gpuTicks": [100, 100100, 300100],
                       "scope": "fixture, not measured"},
            "identity": {role: {"path": role, "sha256": "a" * 64} for role in tool.IDENTITY_ROLES},
            "shadowMap": {"size": 8}, **self.metadata,
            "outputs": {}}
        self.write_artifacts()

    def tearDown(self):
        self.temp.cleanup()

    def write_artifacts(self):
        self.linear.tofile(self.root / "linear-hdr.rgba32f")
        self.diagnostics.tofile(self.root / "diagnostics.rgba32f")
        self.diagnostics[..., 0].tofile(self.root / "visibility.r32f")
        np.ones((8, 8), dtype="<f4").tofile(self.root / "shadow-depth.r32f")
        Image.fromarray(tool.output_transform(self.linear)).save(self.root / "color.png")
        visible = np.rint(self.diagnostics[..., 0] * 255).astype(np.uint8)
        Image.fromarray(visible).convert("RGB").save(self.root / "visibility.png")
        Image.new("RGB", (64, 64), (0, 0, 0)).save(self.root / "diagnostic.png")
        names = {"color": "color.png", "visibility": "visibility.png", "diagnostic": "diagnostic.png",
                 "linearHdr": "linear-hdr.rgba32f", "diagnostics": "diagnostics.rgba32f",
                 "visibilityRaw": "visibility.r32f", "shadowDepth": "shadow-depth.r32f"}
        self.result["outputs"] = {role: {"path": name, "bytes": (self.root / name).stat().st_size,
                                             "sha256": tool.sha256(self.root / name)} for role, name in names.items()}
        self.write_result()

    def write_result(self):
        (self.root / "result.json").write_text(json.dumps(self.result), encoding="utf-8")

    def refresh_output_hash(self, role):
        record = self.result["outputs"][role]
        path = self.root / record["path"]
        record.update(bytes=path.stat().st_size, sha256=tool.sha256(path))
        self.write_result()

    def test_valid_raw_and_png_contract(self):
        run = tool.validate_run(self.root, self.params)
        self.assertEqual(run["pngMaxError"], 0)

    def test_missing_raw_is_rejected_even_with_complete_status(self):
        (self.root / "diagnostics.rgba32f").unlink()
        with self.assertRaisesRegex(tool.EvidenceError, "missing"):
            tool.validate_run(self.root)

    def test_raw_mutation_is_rejected_by_sha(self):
        (self.root / "linear-hdr.rgba32f").write_bytes(b"changed")
        with self.assertRaisesRegex(tool.EvidenceError, "size mismatch"):
            tool.validate_run(self.root)

    def test_same_size_raw_mutation_is_rejected_by_sha(self):
        data = bytearray((self.root / "linear-hdr.rgba32f").read_bytes())
        data[0] ^= 1
        (self.root / "linear-hdr.rgba32f").write_bytes(data)
        with self.assertRaisesRegex(tool.EvidenceError, "SHA mismatch"):
            tool.validate_run(self.root)

    def test_rehashed_nan_is_still_rejected(self):
        self.linear[3, 4, 1] = float("nan")
        self.linear.tofile(self.root / "linear-hdr.rgba32f")
        self.refresh_output_hash("linearHdr")
        with self.assertRaisesRegex(tool.EvidenceError, "non-finite raw"):
            tool.validate_run(self.root)

    def test_rehashed_png_from_wrong_frame_is_rejected(self):
        Image.new("RGB", (64, 64), (200, 100, 0)).save(self.root / "color.png")
        self.refresh_output_hash("color")
        with self.assertRaisesRegex(tool.EvidenceError, "does not match linear HDR"):
            tool.validate_run(self.root)

    def test_visibility_file_cannot_disagree_with_diagnostics(self):
        np.zeros((64, 64), dtype="<f4").tofile(self.root / "visibility.r32f")
        self.refresh_output_hash("visibilityRaw")
        with self.assertRaisesRegex(tool.EvidenceError, "channels do not agree"):
            tool.validate_run(self.root)

    def test_old_output_is_rejected_even_when_hash_matches(self):
        old = time.time() - 3600.
        os.utime(self.root / "color.png", (old, old))
        with self.assertRaisesRegex(tool.EvidenceError, "stale output"):
            tool.validate_run(self.root, started_at=time.time())

    def test_requested_params_are_bound(self):
        requested = {**self.params, "lightSize": 1.2}
        with self.assertRaisesRegex(tool.EvidenceError, "do not match"):
            tool.validate_run(self.root, requested)

    def test_requested_float_params_use_explicit_binary32_identity(self):
        quantized = {**self.params, "lightSize": struct.unpack("<f", struct.pack("<f", .65))[0],
                     "bias": struct.unpack("<f", struct.pack("<f", .004))[0]}
        self.assertTrue(tool.same_params(quantized, self.params))
        wrong = {**quantized, "lightSize": .650001}
        self.assertFalse(tool.same_params(wrong, self.params))

    def test_executable_and_shader_identities_are_bound(self):
        for role in tool.IDENTITY_ROLES:
            with self.subTest(role=role):
                identity = {name: "a" * 64 for name in tool.IDENTITY_ROLES}
                identity[role] = "b" * 64
                with self.assertRaisesRegex(tool.EvidenceError, "identity mismatch"):
                    tool.validate_run(self.root, expected_identity=identity)

    def test_runtime_status_is_not_sufficient(self):
        self.result["debugLayer"]["warningCount"] = 1
        self.write_result()
        with self.assertRaisesRegex(tool.EvidenceError, "clean D3D12 debug"):
            tool.validate_run(self.root)

    def test_disabled_debug_layer_is_rejected_for_acceptance(self):
        self.result["debugLayer"]["enabled"] = False
        self.write_result()
        with self.assertRaisesRegex(tool.EvidenceError, "debug layer"):
            tool.validate_run(self.root)

    def test_missing_gpu_timing_cannot_be_reported_as_measured(self):
        self.result["timing"]["totalGpuMs"] = 0.
        self.write_result()
        with self.assertRaisesRegex(tool.EvidenceError, "positive GPU timing"):
            tool.validate_run(self.root)

    def test_reported_gpu_timing_must_match_raw_ticks(self):
        self.result["timing"]["shadeGpuMs"] = .21
        self.write_result()
        with self.assertRaisesRegex(tool.EvidenceError, "does not match raw ticks"):
            tool.validate_run(self.root)

    def test_unknown_parameter_and_nonfinite_values_are_rejected(self):
        invalid = ({**self.params, "unknown": 1}, {**self.params, "samples": 0},
                   {**self.params, "bias": float("nan")}, {**self.params, "width": True})
        for params in invalid:
            with self.subTest(params=params), self.assertRaises(tool.EvidenceError):
                tool.check_params(params)

    def test_artifact_traversal_and_output_alias_are_rejected(self):
        self.result["outputs"]["color"]["path"] = "../outside.png"
        self.write_result()
        with self.assertRaisesRegex(tool.EvidenceError, "escapes run"):
            tool.validate_run(self.root)

    def test_existing_output_is_never_overwritten(self):
        marker = self.root / "owner-file"
        marker.write_bytes(b"original")
        with self.assertRaisesRegex(tool.EvidenceError, "already exists"):
            tool.new_directory(self.root)
        self.assertEqual(marker.read_bytes(), b"original")

    def test_tool_unknown_option_fails_before_creating_output(self):
        output = Path(self.temp.name) / "must-not-exist"
        process = subprocess.run([sys.executable, str(MODULE_PATH), "analyze", "--input", str(self.root),
                                  "--output", str(output), "--unknown", "1"], capture_output=True, check=False)
        self.assertNotEqual(process.returncode, 0)
        self.assertFalse(output.exists())
        self.assertIn(b"unrecognized arguments", process.stderr)

    def test_incomplete_suite_does_not_emit_analysis_pass(self):
        (self.root / "suite.json").write_text(json.dumps({"schemaVersion": 1, "collectionComplete": False}), encoding="utf-8")
        output = Path(self.temp.name) / "analysis"
        with self.assertRaisesRegex(tool.EvidenceError, "incomplete"):
            tool.analyze(self.root, output)
        self.assertFalse((output / "analysis.json").exists())
        self.assertEqual(json.loads((output / "failure.json").read_text())["status"], "FAIL")

    def test_seal_covers_nested_manifests_and_rejects_added_files(self):
        nested = self.root / "nested"
        nested.mkdir()
        (nested / "attachment.txt").write_bytes(b"frozen")
        tool.seal(nested)
        tool.seal(self.root)
        tool.verify_seal(self.root)
        manifest = json.loads((self.root / "artifact-manifest.json").read_text())
        self.assertIn("nested/artifact-manifest.json", manifest["files"])
        (nested / "unexpected.txt").write_bytes(b"new")
        with self.assertRaisesRegex(tool.EvidenceError, "file set mismatch"):
            tool.verify_seal(self.root)

    def test_seal_rejects_changed_data(self):
        tool.seal(self.root)
        (self.root / "visibility.r32f").write_bytes(b"tampered")
        with self.assertRaisesRegex(tool.EvidenceError, "manifest mismatch"):
            tool.verify_seal(self.root)

    def test_sealed_input_cannot_be_modified_by_analyze_output(self):
        tool.seal(self.root)
        with self.assertRaisesRegex(tool.EvidenceError, "sealed source"):
            tool.analyze(self.root, self.root / "analysis")
        self.assertFalse((self.root / "analysis").exists())


class ShadowShowcaseGeometryTests(unittest.TestCase):
    def test_camera_non_symmetric_left_right_anchors_from_known_coordinates(self):
        # 朝 -Z、Y 向上的相机，右侧屏幕射线须指向 +X。预期数值直接由90° FOV推导，
        # 不调用 camera_ray 来生成 fixture，从而能识别相机求基函数自身的镜像错误。
        camera = {"position": [0., 0., 3.], "target": [0., 0., 0.], "up": [0., 1., 0.],
                  "verticalFovDegrees": 90.}
        boxes = [{"min": [-3.3, -.3, -.3], "max": [-2.7, .3, .3], "materialId": 11},
                 {"min": [2.7, -.3, -.3], "max": [3.3, .3, .3], "materialId": 22}]
        for screen_x, expected_x, expected_material in ((50., -1., 11), (150., 1., 22)):
            with self.subTest(screen_x=screen_x):
                origin, direction = tool.camera_ray(camera, screen_x, 50., 200, 100)
                self.assertEqual(origin, (0., 0., 3.))
                self.assertAlmostEqual(direction[0], expected_x / math.sqrt(2.), places=12)
                self.assertAlmostEqual(direction[1], 0., places=12)
                self.assertAlmostEqual(direction[2], -1. / math.sqrt(2.), places=12)
                hit = tool.first_hit(boxes, origin, direction)
                self.assertIsNotNone(hit)
                self.assertEqual(boxes[hit[2]]["materialId"], expected_material)
        _, above = tool.camera_ray(camera, 100., 25., 200, 100)
        self.assertAlmostEqual(above[1], .5 / math.sqrt(1.25), places=12)
        self.assertLess(above[2], 0.)

    def test_slab_known_hit_and_miss(self):
        hit = tool.ray_box((0., 2., -2.), (0., -1., 1.), (-1., 0., -1.), (1., 1., 1.))
        self.assertEqual(hit, (1., (0., 1., 0.)))
        self.assertIsNone(tool.ray_box((2., 2., -2.), (0., -1., 1.), (-1., 0., -1.), (1., 1., 1.)))

    def test_visibility_uses_occluder_between_surface_and_light(self):
        boxes = [{"min": [-2., -.1, -2.], "max": [2., 0., 2.]},
                 {"min": [-.5, .5, -.5], "max": [.5, 1.5, .5]}]
        self.assertTrue(tool.segment_occluded((0., .001, 0.), (0., 3., 0.), boxes, 0))
        self.assertFalse(tool.segment_occluded((1.5, .001, 0.), (1.5, 3., 0.), boxes, 0))
        # 光源以前的遮挡才有效，光源后方物体不能错误遮挡。
        self.assertFalse(tool.segment_occluded((0., .001, 0.), (0., .2, 0.), boxes, 0))

    def test_no_signal_cannot_pass_parameter_checks(self):
        arrays = {"linearHdr": np.ones((64, 64, 4), dtype=np.float32),
                  "diagnostics": np.ones((64, 64, 4), dtype=np.float32)}
        runs = {name: {"arrays": arrays, "params": params,
                       "result": {"sceneMetadata": {
                           "camera": {"position": [0., 3., -4.], "target": [0., 0., 0.], "up": [0., 1., 0.], "verticalFovDegrees": 45.},
                           "light": {"position": [0., 5., 0.]},
                           "geometry": {"boxes": [{"min": [-3., -.1, -3.], "max": [3., 0., 3.], "materialId": 1}]}}}}
                for name, params in tool.presets(64, 64).items()}
        report = tool.analyze_checks(runs)
        self.assertEqual(report["status"], "FAIL")
        for name in ("hardToPcf", "pcfToPcss", "lightSize", "bias", "sampling", "softness", "bounce", "hardReference"):
            self.assertFalse(report["checks"][name]["pass"], name)
        self.assertTrue(report["checks"]["restore"]["pass"])

    def test_restore_detects_small_float_change_below_display_precision(self):
        baseline = {"arrays": {"linearHdr": np.ones((2, 2, 4), dtype=np.float32)}}
        changed = {"arrays": {"linearHdr": baseline["arrays"]["linearHdr"].copy()}}
        changed["arrays"]["linearHdr"][0, 0, 0] += np.float32(1e-7)
        metric = tool.difference(baseline, changed, "linearHdr", (0, 1, 2, 3))
        self.assertGreater(metric["maxAbs"], 0.)
        self.assertEqual(metric["changedElements"], 0)  # 显示阈值内变化仍影响恢复的严格相等判据。

    def test_spatially_wrong_mask_is_detected_even_with_equal_mean(self):
        camera = {"position": [0., 4., -6.], "target": [0., 0., 1.], "up": [0., 1., 0.], "verticalFovDegrees": 55.}
        light = [-2., 5., -2.]
        boxes = [{"min": [-4., -.2, -3.], "max": [4., 0., 6.], "materialId": 0},
                 {"min": [-.6, 0., -.2], "max": [.6, 1.5, 1.], "materialId": 1}]
        diagnostics = np.zeros((128, 128, 4), dtype=np.float32)
        diagnostics[..., 3] = -1.
        # 由几何生成fixture：camera求交负责定位，遮挡用密集线段点采样作不同形式的参考。
        for row in range(128):
            for col in range(128):
                origin, direction = tool.camera_ray(camera, col + .5, row + .5, 128, 128)
                hit = tool.first_hit(boxes, origin, direction)
                if hit is None:
                    continue
                distance, normal, receiver = hit
                point = tuple(origin[i] + direction[i] * distance + normal[i] * .001 for i in range(3))
                blocked = False
                for fraction in np.linspace(.001, .999, 120):
                    sample = tuple(point[i] + (light[i] - point[i]) * float(fraction) for i in range(3))
                    if any(all(box["min"][i] < sample[i] < box["max"][i] for i in range(3))
                           for index, box in enumerate(boxes)):
                        blocked = True
                        break
                diagnostics[row, col, 0] = 0. if blocked else 1.
                diagnostics[row, col, 3] = boxes[receiver]["materialId"]
        run = {"params": {**tool.presets(128, 128)["hard"]}, "arrays": {"diagnostics": diagnostics},
               "result": {"sceneMetadata": {"camera": camera, "light": {"position": light}, "geometry": {"boxes": boxes}}}}
        correct = tool.hard_reference(run, stride=3)
        self.assertTrue(correct["pass"], correct)
        visibility = diagnostics[..., 0].copy()
        diagnostics[..., 0] = np.flip(visibility, axis=1)
        self.assertEqual(float(diagnostics[..., 0].mean()), float(visibility.mean()))
        wrong = tool.hard_reference(run, stride=3)
        self.assertFalse(wrong["pass"])
        self.assertGreater(wrong["visibilityErrors"], correct["visibilityErrors"])


if __name__ == "__main__":
    unittest.main()
