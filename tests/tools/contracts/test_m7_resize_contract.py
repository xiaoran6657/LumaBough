"""M7 resize 路径的 CLI 契约测试（M7-RESIZE-PATH / CTest `M7ResizeCliContract`）。

只做**参数层**断言：`--exercise-resize` 的三条约束必须在创建任何 GPU 对象之前就拒绝，
因此本测试不需要真实设备（但需要 MiniEngineSandbox.exe 能加载——本仓库的 M7 测试一贯是
本地可复现优先，CI 若无 DirectX 运行时会在这里响亮失败而不是静默跳过）。

真正的端到端 resize 覆盖（真实 WM_SIZE → 交换链跟随 → viewport/回读重派生 → 遥测计数）
是驱动 + 发布证据，见 docs/architecture/DECISIONS.md 的 `E-M7-RESIZE-001`。
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

    case("缺 --stability-stress-every 时拒绝")
    code, output = run(args.exe, common + ["--exercise-resize"])
    check(code == 2, f"exit must be 2 (usage), got {code}")
    check("--exercise-resize requires --stability-stress-every" in output, "缺少依赖的报错缺失")
    print("  ok 缺依赖被拒绝")

    case("benchmark 下拒绝（固定 extent 测量契约）")
    benchmark = [
        "--rhi=d3d12", "--scene=m7-cpu-scale", "--benchmark", "--warmup-frames=10", "--measure-frames=20",
        "--vsync=off", "--seed=6657", "--workers=1", "--chunk-size=256", "--packet-build=serial",
        "--layout=none", "--upload-mib-per-frame=4", "--stability-stress-every=10", "--exercise-resize",
    ]
    code, output = run(args.exe, benchmark)
    check(code == 2, f"exit must be 2 (usage), got {code}")
    check("--exercise-resize is not allowed in benchmark mode" in output, "benchmark 禁止的报错缺失")
    print("  ok benchmark 下被拒绝")

    case("headless 下拒绝（需要真实窗口）")
    code, output = run(args.exe, common + ["--headless", "--stability-stress-every=40", "--exercise-resize"])
    check(code == 2, f"exit must be 2 (usage), got {code}")
    check("--exercise-resize requires a real window" in output, "headless 禁止的报错缺失")
    print("  ok headless 下被拒绝")

    case("--help 声明该选项与其约束")
    code, output = run(args.exe, ["--help"])
    check(code == 0, f"--help must exit 0, got {code}")
    check("--exercise-resize" in output, "--help 未声明 --exercise-resize")
    check("non-benchmark only" in output, "--help 未声明 non-benchmark only 约束")
    print("  ok help 文本一致")

    print("M7 resize CLI contract passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
