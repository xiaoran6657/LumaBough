import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image


SCRIPT = Path(__file__).parents[2] / "tools" / "legacy" / "Compare-M5Parity.py"


def write_case(directory: Path, name: str, pixels: np.ndarray, **metadata):
    image = directory / f"{name}.png"
    Image.fromarray(pixels, mode="RGB").save(image)
    values = {
        "assetManifestSha256": "asset",
        "shaderSemanticSha256": "shader",
        "environmentArtifactSha256": "environment",
        "cameraValues": [0, 0, -10],
        "lightValues": [0, -1, 0],
        "exposureEv": 0,
        "iblProfile": "ibl-v1",
        "shadow": "shadow-v1",
        "fixedTick": 120,
        "visibleSequenceHash": "visible",
        "resolution": [pixels.shape[1], pixels.shape[0]],
        "debugView": "Lit",
        "toneMapper": "aces",
        "culling": "back",
        "skyboxEnabled": True,
        "pngSha256": hashlib.sha256(image.read_bytes()).hexdigest(),
        "backend": "d3d11",
        "driver": "driver-a",
        "compiler": "fxc",
    }
    values.update(metadata)
    image.with_name(image.name + ".json").write_text(json.dumps(values), encoding="utf-8")
    return image


class CompareM5ParityTests(unittest.TestCase):
    def run_compare(self, image_a, image_b, directory):
        output = directory / "result.json"
        process = subprocess.run(
            [sys.executable, str(SCRIPT), "--a", str(image_a), "--b", str(image_b), "--output", str(output)],
            capture_output=True,
            text=True,
        )
        self.assertTrue(output.is_file(), process.stderr)
        return process.returncode, json.loads(output.read_text(encoding="utf-8"))

    def test_missing_required_fields_blocks_without_crash(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            pixels = np.zeros((2, 2, 3), dtype=np.uint8)
            left = write_case(directory, "a", pixels)
            right = write_case(directory, "b", pixels)
            for image in (left, right):
                metadata = json.loads(image.with_name(image.name + ".json").read_text())
                metadata.pop("environmentArtifactSha256")
                image.with_name(image.name + ".json").write_text(json.dumps(metadata))
            code, result = self.run_compare(left, right, directory)
            self.assertEqual(code, 2)
            self.assertEqual(result["status"], "BLOCKED")
            self.assertIn("a.environmentArtifactSha256", result["mismatchedFields"])

    def test_single_side_missing_field_blocks(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            pixels = np.zeros((2, 2, 3), dtype=np.uint8)
            left = write_case(directory, "a", pixels)
            right = write_case(directory, "b", pixels)
            metadata_path = left.with_name(left.name + ".json")
            metadata = json.loads(metadata_path.read_text())
            metadata.pop("cameraValues")
            metadata_path.write_text(json.dumps(metadata))
            code, result = self.run_compare(left, right, directory)
            self.assertEqual(code, 2)
            self.assertIn("a.cameraValues", result["mismatchedFields"])

    def test_backend_difference_passes_and_preserves_diagnostics(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            pixels = np.zeros((2, 2, 3), dtype=np.uint8)
            left = write_case(directory, "a", pixels, backend="d3d11", compiler="fxc")
            right = write_case(directory, "b", pixels, backend="d3d12", compiler="dxc", driver="driver-b")
            code, result = self.run_compare(left, right, directory)
            self.assertEqual(code, 0)
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["diagnostics"]["backend"], {"a": "d3d11", "b": "d3d12"})
            self.assertEqual(result["diagnostics"]["compiler"], {"a": "fxc", "b": "dxc"})

    def test_resolution_mismatch_blocks(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            left = write_case(directory, "a", np.zeros((2, 2, 3), dtype=np.uint8))
            right = write_case(directory, "b", np.zeros((3, 2, 3), dtype=np.uint8), resolution=[2, 3])
            code, result = self.run_compare(left, right, directory)
            self.assertEqual(code, 2)
            self.assertIn("imageShape", result["mismatchedFields"])

    def test_png_hash_mismatch_blocks(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            pixels = np.zeros((2, 2, 3), dtype=np.uint8)
            left = write_case(directory, "a", pixels, pngSha256="wrong")
            right = write_case(directory, "b", pixels)
            code, result = self.run_compare(left, right, directory)
            self.assertEqual(code, 2)
            self.assertIn("a.pngSha256", result["mismatchedFields"])

    def test_image_threshold_exceeded_fails(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            left_pixels = np.zeros((2, 2, 3), dtype=np.uint8)
            right_pixels = left_pixels.copy()
            right_pixels[0, 0] = [20, 20, 20]
            left = write_case(directory, "a", left_pixels)
            right = write_case(directory, "b", right_pixels)
            code, result = self.run_compare(left, right, directory)
            self.assertEqual(code, 1)
            self.assertEqual(result["status"], "FAIL")
            self.assertEqual(result["structuralReview"], "required")


if __name__ == "__main__":
    unittest.main()