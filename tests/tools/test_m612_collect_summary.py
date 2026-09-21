"""M6-12 collect 阶段 parity 汇总的 CPU 回归测试（审计勘误 2026-09-16）。

背景一：phase_parity 产物把明细写进 `legacyComparisonResults`/`crossBackendResults`，
collect 的汇总曾读取 `legacyComparisons`/`crossBackend` 而得到 0/0（冻结的
out/m6-12/summary.json 即受影响，实际值 20/10）。本测试锁定两种字段形状。
背景二（审计二轮 P2）：`legacySkipped` 必须从 run manifest 经 parity_phase_payload
贯穿到 parity_summary——此前阶段产物丢失该字段。链路用例锁定此传递。
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/legacy"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/validation"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/capture"))

ROOT = Path(__file__).parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import m6_summary as run_m612_acceptance  # noqa: E402
from m6_summary import parity_summary  # noqa: E402


def phase_payload(*, results_names: bool, legacy: int = 20, cross: int = 10) -> dict:
    payload = {"status": "PASS", "contractComplete": True, "records": 200,
               "legacyComparisonCount" if results_names else "legacyComparisons": legacy,
               "crossBackendCount" if results_names else "crossBackend": cross}
    if results_names:
        payload["legacyComparisonResults"] = [{"backend": "d3d11"}] * legacy
        payload["crossBackendResults"] = [{"debugView": v} for v in range(cross)]
        payload["legacySkipped"] = None
    else:
        payload["legacyComparisons"] = [{"backend": "d3d11"}] * legacy
        payload["crossBackend"] = [{"debugView": v} for v in range(cross)]
    return payload


class ParitySummaryTests(unittest.TestCase):
    def test_current_phase_json_shape_counts_results_lists(self):
        summary = parity_summary(phase_payload(results_names=True))
        self.assertEqual(summary["legacyComparisons"], 20)
        self.assertEqual(summary["crossBackend"], 10)
        self.assertEqual(summary["records"], 200)
        self.assertEqual(summary["status"], "PASS")
        self.assertTrue(summary["contractComplete"])
        self.assertIsNone(summary["legacySkipped"])

    def test_legacy_run_manifest_shape_still_counts(self):
        summary = parity_summary(phase_payload(results_names=False))
        self.assertEqual(summary["legacyComparisons"], 20)
        self.assertEqual(summary["crossBackend"], 10)

    def test_retired_legacy_skip_is_reported(self):
        payload = phase_payload(results_names=True, legacy=0, cross=10)
        payload["legacySkipped"] = {"status": "SKIPPED", "reason": "legacy A/B renderer retired"}
        summary = parity_summary(payload)
        self.assertEqual(summary["legacyComparisons"], 0)
        self.assertEqual(summary["crossBackend"], 10)
        self.assertIsNotNone(summary["legacySkipped"])

    def test_empty_payload_degrades_to_zeroes(self):
        summary = parity_summary({})
        self.assertEqual(summary["legacyComparisons"], 0)
        self.assertEqual(summary["crossBackend"], 0)
        self.assertIsNone(summary["status"])
        self.assertIsNone(summary["records"])


def run_manifest(*, legacy: int, cross: int, skipped: bool) -> dict:
    """模拟 tools/legacy/run_m610_parity.py 写出的 m610-run.json（单数字段名）。"""
    return {"schema": "miniengine.m6-10.parity-run.v2", "status": "PASS",
            "contractComplete": True,
            "records": [{"backend": "d3d11"}] * 200,
            "legacy": [], "legacyRunResults": [],
            "legacyComparisons": [{"backend": "d3d11", "debugView": v} for v in range(legacy)],
            "legacySkipped": ({"status": "SKIPPED", "reason": "legacy A/B renderer retired"} if skipped else None),
            "crossBackend": [{"debugView": v} for v in range(cross)],
            "representatives": []}


class LegacySkipChainTests(unittest.TestCase):
    """run manifest → parity_phase_payload → parity_summary 的传递链。"""

    def test_legacy_skipped_survives_the_whole_chain(self):
        manifest = run_manifest(legacy=0, cross=10, skipped=True)
        payload = run_m612_acceptance.parity_phase_payload(manifest, {"code": 0, "seconds": 1.0}, "sha")
        summary = run_m612_acceptance.parity_summary(payload)
        self.assertEqual(summary["legacyComparisons"], 0)
        self.assertEqual(summary["crossBackend"], 10)
        self.assertIsNotNone(summary["legacySkipped"])
        self.assertEqual(summary["legacySkipped"]["status"], "SKIPPED")

    def test_legacy_counts_survive_the_whole_chain_when_not_skipped(self):
        manifest = run_manifest(legacy=20, cross=10, skipped=False)
        payload = run_m612_acceptance.parity_phase_payload(manifest, {"code": 0, "seconds": 1.0}, "sha")
        summary = run_m612_acceptance.parity_summary(payload)
        self.assertEqual(summary["legacyComparisons"], 20)
        self.assertEqual(summary["crossBackend"], 10)
        self.assertIsNone(summary["legacySkipped"])


if __name__ == "__main__":
    unittest.main()
