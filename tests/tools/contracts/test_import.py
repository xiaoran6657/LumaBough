"""导入清单门的负例；不把运行证据缺口变成 PASS。"""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools/portfolio"))
from audit_import import linked, sha, walk
from validate_import import MANIFEST, relative, validate_tree

class ImportGateTests(unittest.TestCase):
    def setUp(self):
        out = Path(__file__).resolve().parents[3] / "out"
        out.mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=out)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.p = self.root / "sample.cpp"
        self.p.write_text("original\n", encoding="utf-8")
        self.manifest = {"schemaVersion":2,"stage":"import","selfExcludedPath":MANIFEST,
                         "localOnlyRoots":[".git","out"],"publicationStatus":"BLOCKED",
                         "files":[{"path":"sample.cpp","size":self.p.stat().st_size,"sha256":sha(self.p),
                                   "reason":"reviewed","category":"source"}],"pending":[],"excluded":[]}

    def test_valid(self):
        self.assertEqual([], validate_tree(self.root, self.manifest))

    def test_tamper(self):
        self.p.write_text("tampered\n", encoding="utf-8")
        self.assertTrue(any("mismatch" in s for s in validate_tree(self.root,self.manifest)))

    def test_extra_file(self):
        (self.root/"private.txt").write_text("private",encoding="utf-8")
        self.assertTrue(any("unclassified file" in s for s in validate_tree(self.root,self.manifest)))

    def test_missing(self):
        self.p.rename(self.root/"renamed.cpp")
        self.assertTrue(any("missing classified" in s for s in validate_tree(self.root,self.manifest)))

    def test_escape(self):
        for value in ("../private","C:/secret","/secret","a/../b","a\\b"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                relative(value)

    def test_source_path_escape(self):
        self.manifest["files"][0]["sourcePath"] = "../private"
        with self.assertRaises(ValueError):
            validate_tree(self.root, self.manifest)

    def test_duplicate(self):
        self.manifest["pending"] = copy.deepcopy(self.manifest["files"])
        self.assertTrue(any("duplicate" in s for s in validate_tree(self.root,self.manifest)))

    def test_reparse_rejected_without_traversal(self):
        with patch("validate_import.walk", return_value=iter([(self.root/"junction","reparse")])):
            self.assertTrue(any("unsafe filesystem" in s for s in validate_tree(self.root,self.manifest)))

    def test_reparse_attribute(self):
        class Info:
            st_mode = 0
            st_file_attributes = 0x400
        with patch.object(Path, "lstat", return_value=Info()):
            self.assertTrue(linked(self.root/"junction"))

    def test_pending_does_not_make_release_ready(self):
        self.manifest["publicationStatus"] = "PASS"
        self.assertTrue(any("readiness" in s for s in validate_tree(self.root,self.manifest)))


class EntryLinkTests(unittest.TestCase):
    def test_existing_private_file_not_allowed(self):
        from validate_publication import links
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "local.txt").write_text("local only", encoding="utf-8")
            (root / "README.md").write_text("[private](local.txt)", encoding="utf-8")
            self.assertTrue(any("outside reviewed" in e for e in links(root, {"README.md"})))


    def test_missing_file(self):
        from validate_publication import links
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("[missing](absent.md)", encoding="utf-8")
            self.assertTrue(links(root, {"README.md"}))

    def test_escape(self):
        from validate_publication import links
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("[escape](../outside.md)", encoding="utf-8")
            self.assertTrue(any("escaping" in e for e in links(root, {"README.md"})))

    def test_anchor_and_external(self):
        from validate_publication import links
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("# Hello\n[x](#hello) [site](https://example.com)\n", encoding="utf-8")
            self.assertEqual([], links(root, {"README.md"}))
            (root / "README.md").write_text("[x](#missing)", encoding="utf-8")
            self.assertTrue(links(root, {"README.md"}))

if __name__ == "__main__":
    unittest.main(verbosity=2)
