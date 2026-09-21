"""M6-10 性能证据解析的纯 CPU 正负向测试。"""
import csv
import json
import tempfile
import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'tools/legacy'))
import analyze_m610_performance as performance


class PerformanceEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.runs = [Path(self.temp.name) / str(index) for index in range(3)]
        metadata = {key: "fixture" for key in performance.IDENTITY}
        for key in ("assetManifestSha256", "environmentArtifactSha256", "shaderSemanticSha256"):
            metadata[key] = "a" * 64
        metadata.update({
            "backend": "d3d12", "buildType": "Release", "renderer": "rhi", "benchmark": True,
            "debugLayer": False, "gpuValidation": False, "vsync": False, "warp": False,
            "fallbackUsed": False, "captureToolActive": False, "warningErrors": 0, "warmupFrames": 120, "measuredFrames": 600,
            "windowForeground": True, "dredRequested": False,
            "fixedTick": 720, "migrationLevel": 9, "resolution": {"width": 1280, "height": 720},
        })
        for directory in self.runs:
            directory.mkdir()
            (directory / "metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
            with (directory / "frames.csv").open("w", newline="", encoding="utf-8") as stream:
                fields = ["frame", "measured", *performance.TIMINGS, *performance.COUNTS]
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                for frame in range(121, 721):
                    row = {key: 2 for key in performance.TIMINGS + performance.COUNTS}
                    row.update(frame=frame, measured=1, declared=8, live=8, culled=0)
                    writer.writerow(row)

    def edit_metadata(self, **changes):
        path = self.runs[1] / "metadata.json"
        value = json.loads(path.read_text(encoding="utf-8"))
        value.update(changes)
        path.write_text(json.dumps(value), encoding="utf-8")

    def test_complete_data_reports_statistics_without_claiming_acceptance(self):
        result = performance.analyze(self.runs)
        self.assertEqual(result["status"], "PASS_DATA")
        self.assertTrue(result["interpretationRequired"])
        self.assertEqual(result["aggregate"]["compileMs"]["median"], 2)
        self.assertEqual(result["aggregate"]["compileMs"]["p95"], 2)
        self.assertEqual(len(result["runs"][0]["artifacts"]["frames.csv"]), 64)

    def test_debug_or_capture_conditions_are_not_release_evidence(self):
        self.edit_metadata(debugLayer=True)
        with self.assertRaisesRegex(performance.EvidenceError, "debugLayer"):
            performance.analyze(self.runs)

    def test_background_or_dred_runs_are_rejected(self):
        self.edit_metadata(windowForeground=False)
        with self.assertRaisesRegex(performance.EvidenceError, "foreground"):
            performance.analyze(self.runs)
        self.edit_metadata(windowForeground=True, dredRequested=True)
        with self.assertRaisesRegex(performance.EvidenceError, "dredRequested"):
            performance.analyze(self.runs)

    def test_d3d11_lowering_count_is_not_a_native_barrier_count(self):
        self.edit_metadata(backend="d3d11")
        with self.assertRaisesRegex(performance.EvidenceError, "no native ResourceBarrier"):
            performance.analyze(self.runs)

    def test_partial_samples_are_rejected(self):
        path = self.runs[1] / "frames.csv"
        lines = path.read_text(encoding="utf-8").splitlines()
        path.write_text("\n".join(lines[:-1]) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(performance.EvidenceError, "600"):
            performance.analyze(self.runs)

    def test_different_input_hash_is_not_averaged(self):
        self.edit_metadata(environmentArtifactSha256="b" * 64)
        with self.assertRaisesRegex(performance.EvidenceError, "identities differ"):
            performance.analyze(self.runs)

    def test_duplicate_run_and_empty_identity_are_rejected(self):
        with self.assertRaisesRegex(performance.EvidenceError, "distinct"):
            performance.analyze([self.runs[0]] * 3)
        self.edit_metadata(shaderSemanticSha256="")
        with self.assertRaisesRegex(performance.EvidenceError, "shaderSemanticSha256"):
            performance.analyze(self.runs)


if __name__ == "__main__":
    unittest.main()
