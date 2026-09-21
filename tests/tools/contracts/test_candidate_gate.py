"""E5 候选门的字节核对：manifest/SHA256SUMS/EXE 不一致必须报错，不得静默通过。"""
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools/portfolio"))
from validate_publication import (E4_REQUIRED_RUNS, build_sensitive, e4_reader_errors,
                                  e4_run_errors, validate_package_files)

EXE = "e" * 64


def e4_record(**overrides):
    runs = [{"id": run_id, "exitCode": code, "packageExeSha256": EXE,
             "status": "expected-failure" if code else "PASS"} for run_id, code in E4_REQUIRED_RUNS]
    record = {"secondMachine": {"complete": True, "runs": runs}}
    record["secondMachine"].update(overrides.pop("secondMachine", {}))
    record.update(overrides)
    return record


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


    def test_package_cannot_widen_the_self_excluded_list(self):
        exe = b"exe-bytes"
        files = dict([blob("runtime/MiniEngineSandbox.exe", exe), blob("extra.bin", b"no hash for me")])
        rows, sums, exe_sha = self.manifest_for(files, hashlib.sha256(exe).hexdigest())
        rows.pop("extra.bin")
        files["SHA256SUMS.txt"] = sums
        files["PACKAGE-MANIFEST.json"] = json.dumps({
            "files": rows, "exeSha256": exe_sha,
            "selfExcluded": ["PACKAGE-MANIFEST.json", "SHA256SUMS.txt", "extra.bin"]}).encode()
        errors, _ = validate_package_files(files)
        self.assertTrue(any("selfExcluded must be exactly" in e for e in errors))

    def test_build_sensitive_list_covers_linked_inputs(self):
        for path in ("tools/benchmark/foo.cpp", "tools/CMakeLists.txt", "tools/assets/gen.py",
                     "tools/shader_compiler/generate_rhi_package.py", "tools/asset_cooker/src/SourceUri.cpp",
                     "engine/rhi/d3d12/src/D3D12Device.cpp", "samples/rhi_sandbox/M6SceneRunner.cpp",
                     "shaders/d3d12/PbrForward.hlsl", "assets/recipes/m9-portfolio-video.json",
                     "tests/rhi/CMakeLists.txt", "cmake/MiniEngineWarnings.cmake",
                     "CMakeLists.txt", "CMakePresets.json"):
            self.assertTrue(build_sensitive(path), path)
        for path in ("docs/evidence/BATCH-E.md", "tools/portfolio/scan_privacy.py", "AGENTS.md", "README.md",
                     "tests/tools/contracts/test_candidate_gate.py",
                     "tests/tools/contracts/test_public_performance_evidence.py",
                     "tests/tools/test_compare_m5_parity.py"):
            self.assertFalse(build_sensitive(path), path)

    def test_cpp_tests_under_tests_are_still_build_inputs(self):
        for path in ("tests/rhi/CMakeLists.txt", "tests/rhi/NativeDeviceTests.cpp",
                     "tests/render_graph/GraphDiagnosticsTests.cpp"):
            self.assertTrue(build_sensitive(path), path)

    def test_only_python_tests_are_exempt_inside_tests_tools(self):
        # 例外必须精确到 Python 测试文件；同目录的 C++ 与 CMake 仍是构建输入。
        for path in ("tests/tools/test_compare_m5_parity.py", "tests/tools/test_m610_parity.py",
                     "tests/tools/contracts/test_public_performance_evidence.py"):
            self.assertFalse(build_sensitive(path), path)
        for path in ("tests/tools/CMakeLists.txt", "tests/tools/AssetCacheTests.cpp",
                     "tests/tools/GltfImportAdapterTests.cpp", "tests/tools/RecipeJsonTests.cpp",
                     "tests/tools/SourceUriTests.cpp", "tests/tools/BuildKeyTests.cpp"):
            self.assertTrue(build_sensitive(path), path)

    def test_complete_e4_record_passes(self):
        self.assertEqual([], e4_run_errors(e4_record(), EXE))

    def test_incomplete_or_inconsistent_e4_records_fail(self):
        # 清空全部运行并声明未完成
        self.assertTrue(e4_run_errors({"secondMachine": {"complete": False, "runs": []}}, EXE))
        # 缺一个运行
        record = e4_record()
        record["secondMachine"]["runs"] = [r for r in record["secondMachine"]["runs"]
                                           if r["id"] != "d3d11-events-1202"]
        self.assertTrue(any("incomplete" in e for e in e4_run_errors(record, EXE)))
        # 重复记录同一个运行
        record = e4_record()
        record["secondMachine"]["runs"].append(dict(record["secondMachine"]["runs"][0]))
        self.assertTrue(any("more than once" in e for e in e4_run_errors(record, EXE)))
        # 正例退出码被改成 2
        record = e4_record()
        record["secondMachine"]["runs"][0]["exitCode"] = 2
        self.assertTrue(any("exit code" in e for e in e4_run_errors(record, EXE)))
        # 负例退出码被改成 0
        record = e4_record()
        record["secondMachine"]["runs"][-1]["exitCode"] = 0
        self.assertTrue(any("exit code" in e for e in e4_run_errors(record, EXE)))
        # complete=false 但运行齐全
        self.assertTrue(any("not marked complete" in e
                            for e in e4_run_errors(e4_record(secondMachine={"complete": False}), EXE)))
        # 绑定到别的包
        record = e4_record()
        record["secondMachine"]["runs"][0]["packageExeSha256"] = "f" * 64
        self.assertTrue(any("different package" in e for e in e4_run_errors(record, EXE)))

    def test_reader_status_semantics(self):
        self.assertEqual([], e4_reader_errors({"reader": {"status": "passed"}}))
        self.assertEqual([], e4_reader_errors({"reader": {"status": "deferred-by-owner"}}))
        self.assertTrue(e4_reader_errors({"reader": {"status": "pending"}}))
        self.assertTrue(e4_reader_errors({"reader": {"status": "something-else"}}))
        self.assertTrue(e4_reader_errors({}))


if __name__ == "__main__":
    unittest.main()
