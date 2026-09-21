"""P7 证据包脱敏契约：绝对路径必须被压成相对形式，且不能碰非路径文本。"""
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools/performance"))
from make_public_performance_evidence import strip_abs


class PublicPerformanceEvidenceTests(unittest.TestCase):
    def test_strips_bare_and_flag_prefixed_paths(self):
        self.assertEqual("out/performance/lb-current-004/build/x.exe",
                         strip_abs("G:\\Programming\\LumaBough\\out\\performance\\lb-current-004\\build\\x.exe"))
        self.assertEqual("--camera=out/performance/lb-current-004/source/assets/tests/m7/fixed-camera.bin",
                         strip_abs("--camera=G:\\Programming\\LumaBough\\out\\performance\\lb-current-004\\source"
                                   "\\assets\\tests\\m7\\fixed-camera.bin"))
        self.assertEqual("--scene=m7-streaming", strip_abs("--scene=m7-streaming"))
        self.assertEqual("--width=1920", strip_abs("--width=1920"))

    def test_paths_outside_the_workspace_keep_only_the_basename(self):
        self.assertEqual("<abs>/secret.bin", strip_abs("C:\\elsewhere\\private\\secret.bin"))

    def test_forward_slash_and_relative_inputs_are_left_alone(self):
        self.assertEqual("out/performance/lb-current-004", strip_abs("out/performance/lb-current-004"))
        self.assertEqual("assets/recipes/m7-performance-scenes.json",
                         strip_abs("assets/recipes/m7-performance-scenes.json"))


if __name__ == "__main__":
    unittest.main()
