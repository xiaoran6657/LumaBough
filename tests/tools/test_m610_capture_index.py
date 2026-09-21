"""捕获索引的二进制长度和截断负向检查，不访问 GPU。"""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("capture_index", Path(__file__).resolve().parents[2] / "tools/capture/index_m610_capture.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class CaptureIndexTests(unittest.TestCase):
    header = b"miniengine.native-rhi-semantic.v1\n"

    def test_binary_pipeline_payload_does_not_split_commands(self):
        payload = b"\x00\xff\n10:BeginLabel\n"
        data = (self.header + b"11:SetPipeline r1 3:8 i0 f0 t" + str(len(payload)).encode() + b":" +
                payload + b"\n10:BeginLabel r0 i0 f0 t6:Shadow\n")
        records = capture.parse_trace(data)
        self.assertEqual(len(records), 2)
        self.assertEqual(records[0]["pipelineKeySha256"], capture.digest(payload))
        self.assertEqual(records[1]["text"], "Shadow")
        self.assertEqual(records[1]["commandIndex"], 1)

    def test_truncated_payload_and_wrong_count_are_rejected(self):
        for record in (b"10:BeginLabel r0 i0 f0 t6:Shad\n", b"10:BeginLabel r2 0:1 i0 f0 t0:\n"):
            with self.subTest(record=record), self.assertRaises(ValueError):
                capture.parse_trace(self.header + record)

    def test_non_text_pipeline_is_the_only_binary_text_field(self):
        with self.assertRaises(UnicodeError):
            capture.parse_trace(self.header + b"10:BeginLabel r0 i0 f0 t1:\xff\n")


if __name__ == "__main__":
    unittest.main()
