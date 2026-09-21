"""M7 设备丢失注入的 CLI 契约测试（M7-DEVICE-LOST-INJECT / CTest `M7DeviceLostCliContract`）。

只做**参数层**断言：`--inject-device-lost-after` 的两条约束（benchmark 禁止、需要真实窗口）
必须在创建任何 GPU 对象之前就拒绝。真正的端到端注入（设备移除 → 下一帧响亮失败 → 非零退出）
是发布证据，见 docs/architecture/DECISIONS.md 的 `E-M7-DEVICE-LOST-001`。
"""

import argparse
import subprocess
import sys


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(exe: str, args: list[str]) -> tuple[int, str]:
    completed = subprocess.run([exe] + args, capture_output=True, text=True)
    return completed.returncode, completed.stdout + completed.stderr


def case(name: str) -> None:
    print(f"[{name}]")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    args = parser.parse_args()

    common = ["--rhi=d3d12", "--scene=m7-streaming", "--frames=120", "--vsync=off", "--seed=6657"]

    case("benchmark 下拒绝（负向注入不进性能测量）")
    benchmark = [
        "--rhi=d3d12", "--scene=m7-cpu-scale", "--benchmark", "--warmup-frames=10", "--measure-frames=20",
        "--vsync=off", "--seed=6657", "--workers=1", "--chunk-size=256", "--packet-build=serial",
        "--layout=none", "--upload-mib-per-frame=4", "--inject-device-lost-after=30",
    ]
    code, output = run(args.exe, benchmark)
    check(code == 2, f"exit must be 2 (usage), got {code}")
    check("--inject-device-lost-after is not allowed in benchmark mode" in output, "benchmark 禁止的报错缺失")
    print("  ok benchmark 下被拒绝")

    case("headless 下拒绝（需要真实窗口）")
    code, output = run(args.exe, common + ["--headless", "--inject-device-lost-after=30"])
    check(code == 2, f"exit must be 2 (usage), got {code}")
    check("--inject-device-lost-after requires a real window" in output, "headless 禁止的报错缺失")
    print("  ok headless 下被拒绝")

    case("--help 声明该选项与其约束")
    code, output = run(args.exe, ["--help"])
    check(code == 0, f"--help must exit 0, got {code}")
    check("--inject-device-lost-after" in output, "--help 未声明 --inject-device-lost-after")
    check("the next RHI call must fail loudly" in output, "--help 未声明响亮失败语义")
    print("  ok help 文本一致")

    print("M7 device-lost CLI contract passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
