"""准备工具的安全闸门回归：路径逃逸、链接/联接点、哈希漂移、空依赖、fresh 工作区。

不调用 cmake：只测"准备"里的判定与文件操作，构建步骤由真实尝试覆盖。
"""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("prepare_experiment", ROOT / "tools/performance/prepare_portfolio_experiment.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def make_repo(base: Path, entries: dict[str, str]) -> tuple[Path, Path]:
    repo = base / "repo"
    repo.mkdir(parents=True)
    manifest_files = []
    for name, body in entries.items():
        path = write(repo / name, body)
        manifest_files.append({"path": name, "sha256": m.sha256_of(path), "category": "test"})
    manifest = write(repo / "docs/evidence/PUBLICATION-FILES.json",
                     json.dumps({"schemaVersion": 2, "files": manifest_files}))
    return repo, manifest


class PrepareExperimentTests(unittest.TestCase):
    def setUp(self):
        self.base = Path(tempfile.mkdtemp(prefix="prepare-"))
        self.addCleanup(shutil.rmtree, self.base, ignore_errors=True)

    def test_happy_path_freezes_and_archives(self):
        repo, manifest = make_repo(self.base, {"a/b.txt": "one", "c.txt": "two"})
        write(repo / "delta.txt", "delta")
        entries = m.load_entries(repo, manifest, ("delta.txt",))
        source = self.base / "ws/source"
        m.copy_sources(repo, source, entries)
        m.verify_frozen(source, entries)
        snapshot = m.write_snapshot(self.base / "ws", repo, manifest, entries)
        archive = m.write_archive(self.base / "ws", source, entries)
        self.assertEqual(len(entries), 4)
        self.assertTrue(snapshot.is_file())
        with zipfile_names(archive) as names:
            self.assertEqual(names, sorted(entry["path"] for entry in entries))

    def test_relative_escape_rejected(self):
        repo, manifest = make_repo(self.base, {"a.txt": "a"})
        write(self.base / "outside.txt", "outside")
        document = json.loads(manifest.read_text(encoding="utf-8"))
        document["files"].append({"path": "../outside.txt", "sha256": m.sha256_of(self.base / "outside.txt")})
        manifest.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "escapes"):
            m.load_entries(repo, manifest, ())

    def test_absolute_path_rejected(self):
        repo, manifest = make_repo(self.base, {"a.txt": "a"})
        with self.assertRaisesRegex(ValueError, "relative"):
            m.resolve_inside(repo, str(self.base / "a.txt"))

    def test_hash_drift_rejected(self):
        repo, manifest = make_repo(self.base, {"a.txt": "a"})
        write(repo / "a.txt", "changed")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            m.load_entries(repo, manifest, ())

    def test_missing_delta_rejected(self):
        repo, manifest = make_repo(self.base, {"a.txt": "a"})
        with self.assertRaisesRegex(ValueError, "delta file missing"):
            m.load_entries(repo, manifest, ("not-there.txt",))

    def test_junction_component_rejected(self):
        repo, manifest = make_repo(self.base, {"a.txt": "a"})
        outside = self.base / "outside"
        outside.mkdir()
        write(outside / "a.txt", "a")
        link = repo / "linked"
        created = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)],
                                 capture_output=True, text=True)
        if created.returncode != 0:
            self.skipTest("junction creation unavailable on this machine")
        document = json.loads(manifest.read_text(encoding="utf-8"))
        document["files"].append({"path": "linked/a.txt", "sha256": m.sha256_of(outside / "a.txt")})
        manifest.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "link or junction"):
            m.load_entries(repo, manifest, ())

    def test_dependency_reuse_verifies_hashes_and_rejects_empty(self):
        parent = self.base / "parent"
        write(parent / "DEPENDENCY-DOWNLOAD-REPAIR.json", "{}")
        source = write(parent / "build/_deps/lib-src/file.h", "content")
        inputs = {"dependencyFiles": {"_deps/lib-src/file.h": m.sha256_of(source)}}
        write(parent / "BUILD-INPUTS.json", json.dumps(inputs))
        copied = m.verify_dependencies(parent, self.base / "target/build")
        self.assertEqual(list(copied), ["_deps/lib-src/file.h"])
        self.assertTrue((self.base / "target/build/_deps/lib-src/file.h").is_file())

        empty = parent / "build/_deps/lib-src/empty.h"
        empty.write_bytes(b"")
        inputs["dependencyFiles"]["_deps/lib-src/empty.h"] = m.sha256_of(empty)
        write(parent / "BUILD-INPUTS.json", json.dumps(inputs))
        with self.assertRaisesRegex(ValueError, "allow-empty"):
            m.verify_dependencies(parent, self.base / "target2/build")
        with self.assertRaisesRegex(ValueError, "not dependency files"):
            m.verify_dependencies(parent, self.base / "target3/build", ("_deps/lib-src/other.h",))
        copied = m.verify_dependencies(parent, self.base / "target4/build", ("_deps/lib-src/empty.h",))
        self.assertEqual(copied["_deps/lib-src/empty.h"], m.sha256_of(empty))
        repair = json.loads((self.base / "target4/DEPENDENCY-DOWNLOAD-REPAIR.json").read_text(encoding="utf-8"))
        self.assertEqual(list(repair["emptyFiles"]), ["_deps/lib-src/empty.h"])

    def test_dependency_hash_mismatch_rejected(self):
        parent = self.base / "parent"
        write(parent / "DEPENDENCY-DOWNLOAD-REPAIR.json", "{}")
        source = write(parent / "build/_deps/lib-src/file.h", "content")
        inputs = {"dependencyFiles": {"_deps/lib-src/file.h": "0" * 64}}
        write(parent / "BUILD-INPUTS.json", json.dumps(inputs))
        self.assertTrue(source.is_file())
        with self.assertRaisesRegex(ValueError, "dependency hash mismatch"):
            m.verify_dependencies(parent, self.base / "target/build")

    def test_existing_workspace_refused(self):
        repo, _manifest = make_repo(self.base, {"a.txt": "a"})
        workspace = repo / "out/performance/lb-current-900"
        workspace.mkdir(parents=True)
        args = m.parse_args(["--workspace", str(workspace), "--parent-workspace", str(workspace),
                             "--attempt", "900", "--repo", str(repo)])
        with self.assertRaisesRegex(ValueError, "fresh"):
            m.prepare(args)

    def test_configure_command_pins_controls(self):
        command = m.configure_command(Path("cmake"), Path("src"), Path("build"))
        text = " ".join(str(item) for item in command)
        self.assertIn("-DME_ENABLE_TRACY=OFF", text)
        self.assertIn("Visual Studio 18 2026", text)
        self.assertIn("v145", text)


class zipfile_names:
    def __init__(self, path):
        self.path = path

    def __enter__(self):
        import zipfile
        with zipfile.ZipFile(self.path) as archive:
            return sorted(archive.namelist())

    def __exit__(self, *exc):
        return False


if __name__ == "__main__":
    unittest.main()
