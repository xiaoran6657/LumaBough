"""E5 候选门的字节核对：manifest/SHA256SUMS/EXE 不一致必须报错，不得静默通过。"""
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools/portfolio"))
from validate_publication import e4_reader_errors, validate_package_files


def blob(name, payload):
    return name, payload


class CandidateGateTests(unittest.TestCase):
    def manifest_for(self, files, exe_sha):
        rows = {name: {"size": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
                for name, payload in files.items() if name not in ("PACKAGE-MANIFEST.json", "SHA256SUMS.txt")}
        sums = "\n".join(row["sha256"] + "  " + name for name, row in rows.items()) + "\n"
        return rows, sums.encode("utf-8"), exe_sha

    def test_consistent_package_passes(self):
        exe = b"exe-bytes"
        files = dict([blob("runtime/MiniEngineSandbox.exe", exe), blob("README.md", b"readme")])
        rows, sums, exe_sha = self.manifest_for(files, hashlib.sha256(exe).hexdigest())
        files["SHA256SUMS.txt"] = sums
        files["PACKAGE-MANIFEST.json"] = json.dumps({"files": rows, "exeSha256": exe_sha,
                                                     "selfExcluded": ["PACKAGE-MANIFEST.json", "SHA256SUMS.txt"]}).encode()
        errors, manifest = validate_package_files(files)
        self.assertEqual([], errors)
        self.assertIsNotNone(manifest)

    def test_tampered_payload_is_reported(self):
        exe = b"exe-bytes"
        files = dict([blob("runtime/MiniEngineSandbox.exe", exe)])
        rows, sums, exe_sha = self.manifest_for(files, hashlib.sha256(exe).hexdigest())
        files["SHA256SUMS.txt"] = sums
        files["PACKAGE-MANIFEST.json"] = json.dumps({"files": rows, "exeSha256": exe_sha,
                                                     "selfExcluded": ["PACKAGE-MANIFEST.json", "SHA256SUMS.txt"]}).encode()
        files["runtime/MiniEngineSandbox.exe"] = b"tampered!!"
        errors, _ = validate_package_files(files)
        self.assertTrue(any("does not match its manifest" in e for e in errors))

    def test_unlisted_and_missing_files_are_reported(self):
        exe = b"exe-bytes"
        files = dict([blob("runtime/MiniEngineSandbox.exe", exe)])
        rows, sums, exe_sha = self.manifest_for(files, hashlib.sha256(exe).hexdigest())
        files["SHA256SUMS.txt"] = sums
        files["PACKAGE-MANIFEST.json"] = json.dumps({"files": rows, "exeSha256": exe_sha,
                                                     "selfExcluded": ["PACKAGE-MANIFEST.json", "SHA256SUMS.txt"]}).encode()
        files["scene/extra.bin"] = b"surprise"
        errors, _ = validate_package_files(files)
        self.assertTrue(any("not covered by the manifest" in e for e in errors))
        del files["scene/extra.bin"]
        files.pop("SHA256SUMS.txt")
        errors, _ = validate_package_files(files)
        self.assertTrue(any("selfExcluded file is missing" in e for e in errors))

    def test_wrong_exe_hash_is_reported(self):
        files = dict([blob("runtime/MiniEngineSandbox.exe", b"exe-bytes")])
        rows, sums, _ = self.manifest_for(files, "0" * 64)
        files["SHA256SUMS.txt"] = sums
        files["PACKAGE-MANIFEST.json"] = json.dumps({"files": rows, "exeSha256": "0" * 64,
                                                     "selfExcluded": ["PACKAGE-MANIFEST.json", "SHA256SUMS.txt"]}).encode()
        errors, _ = validate_package_files(files)
        self.assertTrue(any("does not match exeSha256" in e for e in errors))


    def test_reader_status_semantics(self):
        self.assertEqual([], e4_reader_errors({"reader": {"status": "passed"}}))
        self.assertEqual([], e4_reader_errors({"reader": {"status": "deferred-by-owner"}}))
        self.assertTrue(e4_reader_errors({"reader": {"status": "pending"}}))
        self.assertTrue(e4_reader_errors({"reader": {"status": "something-else"}}))
        self.assertTrue(e4_reader_errors({}))


if __name__ == "__main__":
    unittest.main()
