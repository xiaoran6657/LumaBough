"""E3 扫描器契约：只产出待审项；approved 才算裁定，proposed 仍应让门禁失败。"""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools/portfolio"))
import scan_privacy


def load_module():
    return importlib.util.module_from_spec(importlib.util.spec_from_file_location("scan_privacy_mod", scan_privacy.__file__))


class ScanPrivacyTests(unittest.TestCase):
    def setUp(self):
        out = Path(__file__).resolve().parents[3] / "out"
        out.mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=out)
        self.addCleanup(self.tmp.cleanup)
        self.pkg = Path(self.tmp.name) / "pkg"
        self.pkg.mkdir()
        self.report = Path(self.tmp.name) / "report.json"

    def scan(self, *extra):
        argv = ["scan_privacy.py", "--package", str(self.pkg), "--output", str(self.report), *extra]
        with patch.object(sys, "argv", argv):
            return scan_privacy.main()

    def test_flags_absolute_path_and_user(self):
        (self.pkg / "note.md").write_text("build at C:/Program Files/tool and user alice here\n", encoding="utf-8")
        rules = {r[0] for r in scan_privacy.rules("alice", "machine")}
        findings = []
        scan_privacy.scan_file(self.pkg / "note.md", "note.md", findings, scan_privacy.rules("alice", "machine"))
        self.assertIn("absolute-path", {f["rule"] for f in findings})
        self.assertIn("username", {f["rule"] for f in findings})
        self.assertIn("machine-name", rules)

    def test_forbidden_artifact_is_block(self):
        (self.pkg / "crash.pdb").write_bytes(b"\x00binary")
        findings = []
        scan_privacy.scan_file(self.pkg / "crash.pdb", "crash.pdb", findings, scan_privacy.rules("", ""))
        self.assertEqual(["forbidden-artifact"], [f["rule"] for f in findings])
        self.assertEqual("block", findings[0]["severity"])

    def test_unadjudicated_fails_and_proposed_still_fails(self):
        (self.pkg / "note.md").write_text("build at C:/tools/tool.exe\n", encoding="utf-8")
        self.assertEqual(1, self.scan())
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(1, report["summary"]["unadjudicated"])
        row = report["findings"][0]
        baseline = Path(self.tmp.name) / "baseline.json"
        item = {"rule": row["rule"], "path": row["path"], "matchSha256": row["matchSha256"],
                "disposition": "keep", "reason": "tool default", "status": "proposed"}
        baseline.write_text(json.dumps({"items": [item]}), encoding="utf-8")
        self.assertEqual(1, self.scan("--baseline", str(baseline)))
        item["status"] = "approved"
        baseline.write_text(json.dumps({"items": [item]}), encoding="utf-8")
        self.assertEqual(0, self.scan("--baseline", str(baseline)))
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(0, report["summary"]["unadjudicated"])
        self.assertEqual(0, report["summary"]["pendingApproval"])

    def test_text_like_skips_binary(self):
        self.assertFalse(scan_privacy.text_like(b"\x00\x01\x02"))
        self.assertTrue(scan_privacy.text_like("plain text".encode("utf-8")))


if __name__ == "__main__":
    unittest.main()
