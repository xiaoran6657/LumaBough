"""M6-11 稳定性诊断的纯 CPU 正负向测试；不访问 GPU，不依赖真实运行。"""
import csv
import importlib.util
import io
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "m611_stability", Path(__file__).resolve().parents[2] / "tools/legacy/analyze_m611_stability.py")
stability = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stability)


def make_csv(samples: int, *, alive=lambda index: 8, created=lambda index: index,
             registry=lambda index: 408) -> str:
    stream = io.StringIO()
    writer = csv.DictWriter(stream, fieldnames=stability.COLUMNS)
    writer.writeheader()
    for index in range(samples):
        row = {name: 0 for name in stability.COLUMNS}
        row["frame"] = (index + 1) * 300
        row["alive"] = alive(index)
        row["retiring"] = 1
        row["registrySlots"] = registry(index)
        row["maxLiveHandles"] = 408
        row["declared"] = 8
        row["live"] = 7
        row["culled"] = 1
        row["virtualResources"] = 12
        row["physicalResources"] = 9
        row["physicalTransients"] = 3
        row["transientCreated"] = created(index)
        row["transientReused"] = 0
        row["transientRetired"] = 0
        row["poolBytes"] = 4096
        row["poolHighWaterBytes"] = 4096
        row["poolResources"] = 3
        row["poolHighWaterResources"] = 3
        row["resourceSets"] = 182
        row["pipelines"] = 7
        row["descriptorRanges"] = 12
        row["uploadBytes"] = 65536
        row["barriers"] = 1478 * (index + 1)
        row["unbinds"] = 0
        row["rhiCommands"] = 870
        writer.writerow(row)
    return stream.getvalue()


class StabilityAnalysisTests(unittest.TestCase):
    def test_plateau_and_constant_rate_pass(self):
        result = stability.analyze_text(make_csv(33))
        self.assertEqual(result["status"], "PASS", result["failures"])
        self.assertEqual(result["windowSamples"], 25)

    def test_live_counter_growth_fails(self):
        result = stability.analyze_text(make_csv(33, alive=lambda index: 8 + index))
        self.assertEqual(result["status"], "FAIL")
        self.assertTrue(any("alive" in failure for failure in result["failures"]))

    def test_cumulative_acceleration_fails(self):
        result = stability.analyze_text(make_csv(33, created=lambda index: index * index))
        self.assertEqual(result["status"], "FAIL")
        self.assertTrue(any("transientCreated" in failure for failure in result["failures"]))

    def test_watermark_may_step_once_but_not_keep_growing(self):
        result = stability.analyze_text(make_csv(33, registry=lambda index: 408 if index < 12 else 684))
        self.assertEqual(result["status"], "PASS", result["failures"])
        self.assertEqual(result["counters"]["registrySlots"]["increaseCount"], 1)
        result = stability.analyze_text(make_csv(33, registry=lambda index: 408 + index * 8))
        self.assertEqual(result["status"], "FAIL")
        self.assertTrue(any("registrySlots" in failure for failure in result["failures"]))
        doubled = stability.analyze_text(make_csv(33, registry=lambda index: 408 + (276 if index >= 10 else 0)
                                                  + (276 if index >= 20 else 0)))
        self.assertEqual(doubled["status"], "FAIL")

    def test_small_fluctuation_around_plateau_is_not_growth(self):
        def noisy(index):
            return 8 + (1 if index % 5 == 0 else 0) - (1 if index % 7 == 0 else 0)

        result = stability.analyze_text(make_csv(33, alive=noisy))
        self.assertEqual(result["status"], "PASS", result["failures"])

    def test_missing_column_and_short_input_are_rejected(self):
        text = make_csv(33).replace("uploadBytes", "uploadedBytes")
        with self.assertRaisesRegex(stability.StabilityError, "missing columns"):
            stability.analyze_text(text)
        with self.assertRaisesRegex(stability.StabilityError, "insufficient samples"):
            stability.analyze_text(make_csv(3))
        with self.assertRaisesRegex(stability.StabilityError, "empty"):
            stability.analyze_text("")


if __name__ == "__main__":
    unittest.main()
