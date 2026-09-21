"""Regression contracts for public performance recomputation."""
import copy
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("portfolio_recompute", ROOT / "tools/portfolio/recompute_performance.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def fixture():
    samples = [{"cpuFrameMs": 10.0 + i % 20} for i in range(600)]
    return {"samples": samples, "measuredFrames": 600, "warmupFrames": 120,
            "correctness": {"status": "PASS"},
            "statistics": {"sampleCount": 600,
                           "cpuFrameMs": module.describe([s["cpuFrameMs"] for s in samples]),
                           "hitches": {"hitch16_67": 390, "hitch33_33": 0, "hitch50": 0}}}

class PerformanceRecomputeTests(unittest.TestCase):
    def test_quantile_interpolates(self):
        self.assertAlmostEqual(module.quantile([0, 10], .95), 9.5)

    def test_valid_samples_are_not_mutated(self):
        raw = fixture()
        before = copy.deepcopy(raw)
        self.assertEqual(module.recompute(raw), before)
        self.assertEqual(raw, before)

    def test_timing_tamper_rejected(self):
        raw = fixture()
        raw["samples"][0]["cpuFrameMs"] = 999
        with self.assertRaisesRegex(ValueError, "differs from samples"):
            module.recompute(raw)

    def test_hitch_tamper_rejected(self):
        raw = fixture()
        raw["statistics"]["hitches"]["hitch50"] = 1
        with self.assertRaisesRegex(ValueError, "hitch counts"):
            module.recompute(raw)

    def test_missing_sample_rejected(self):
        raw = fixture()
        raw["samples"].pop()
        with self.assertRaisesRegex(ValueError, "sample count"):
            module.recompute(raw)

    def test_failed_correctness_rejected(self):
        raw = fixture()
        raw["correctness"]["status"] = "FAIL"
        with self.assertRaisesRegex(ValueError, "correctness"):
            module.recompute(raw)

    def test_nonfinite_rejected(self):
        with self.assertRaises(ValueError):
            module.describe([float("nan")])

    def test_protocol_warmup_rejected(self):
        raw = fixture()
        raw["warmupFrames"] = 1
        with self.assertRaisesRegex(ValueError, "protocol"):
            module.recompute(raw)

    def test_chronology_keeps_batch_identity(self):
        paths = [Path("serial-r1/run.json"), Path("batch2/serial-r1/run.json"),
                 Path("serial-r2/run.json"), Path("batch2/serial-r2/run.json")]
        values = [10, 30, 20, 40]
        runs = {p: {"statistics": {"cpuFrameMs": {"median": value}}} for p, value in zip(paths, values)}
        times = {paths[0]: "01", paths[1]: "03", paths[2]: "02", paths[3]: "04"}
        result = module.chronological_drift(runs, times, "serial-*")
        self.assertEqual(result["runOrder"], [paths[i].as_posix() for i in [0, 2, 1, 3]])
        self.assertAlmostEqual(result["changePercent"], 100 * (35 - 15) / 15)

if __name__ == "__main__":
    unittest.main()

