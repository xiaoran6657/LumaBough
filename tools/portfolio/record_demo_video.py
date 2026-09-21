"""录制 M6 连续 Demo 并烧入字幕（D 批次：聚焦动态内容的视频）。

只做"真实运行的画面"：脚本启动被测 EXE（不注入 Capture、不注入 overlay），
用 ffmpeg gdigrab 抓取该窗口客户端区域，运行结束后再烧字幕与转码。

时间线不靠猜：临时 extent 那段画面（960×540 被 DXGI 拉伸）由 ffmpeg 的 blurdetect
客观定位，其余事件（挂起、拒绝非法候选、恢复）按 recipe 的停留帧数与帧率换算到它两侧。
字幕文本只解释真实事件，不做能力扩张。

用法：
  python tools/portfolio/record_demo_video.py \
      --exe out/build/windows-msvc-debug/samples/rhi_sandbox/Release/MiniEngineSandbox.exe \
      --manifest out/demo/scene/manifest.json --recipe assets/recipes/m9-portfolio-video.json \
      --work out/d-batch/video/d3d12 --backend d3d12
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import time
from ctypes import wintypes as w
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_demo import collapse, read as read_json  # noqa: E402  （复用同一套 recipe 读取与事件折叠口径）

u = ctypes.WinDLL("user32", use_last_error=True)
u.GetForegroundWindow.restype = w.HWND
u.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM), w.LPARAM]
u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
u.GetWindowTextLengthW.argtypes = [w.HWND]
u.IsWindowVisible.argtypes = [w.HWND]
CREATE_NO_WINDOW = 0x08000000
WINDOW_TITLE = "MiniEngine M6 Pass Migration"


def enable_dpi_awareness() -> str:
    """必须在任何窗口/坐标 API 之前调用，否则 GetClientRect 返回缩放后的逻辑坐标，
    ddagrab 会按物理像素去截错区域（实测截到桌面壁纸）。"""
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # per-monitor v2
        return "per-monitor-v2"
    except (AttributeError, OSError):
        pass
    try:
        if ctypes.windll.user32.SetProcessDPIAware():
            return "system"
    except (AttributeError, OSError):
        pass
    raise RuntimeError("cannot enable DPI awareness; refusing to guess the capture rectangle")


DPI_AWARENESS = enable_dpi_awareness()

CUES = [
    ("intro", "LumaBough · M6 连续 Demo：固定相机与光照，持续渲染（{backend} / Release / VSync）"),
    ("suspend", "1）程序化 RHI 暂停：交换链切到 0×0，引擎停止提交帧（不是操作系统最小化）"),
    ("temporary", "2）交换链重建为 960×540：同一份 bytecode 重建全部 pipeline，画面由 DXGI 拉伸呈现"),
    ("reject", "3）非法 shader 候选（空 bytecode）被拒绝：pipeline 数量保持，画面继续呈现"),
    ("restored", "4）恢复 1920×1080：末帧与动作前锚点帧像素逐字节一致"),
]


def find_window(title: str) -> int:
    found = []

    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def visit(hwnd, _param):
        if not u.IsWindowVisible(hwnd):
            return True
        length = u.GetWindowTextLengthW(hwnd)
        if not length:
            return True
        buffer = ctypes.create_unicode_buffer(length + 1)
        u.GetWindowTextW(hwnd, buffer, length + 1)
        if title in buffer.value:
            found.append(int(hwnd))
        return True

    u.EnumWindows(visit, 0)
    return found[0] if found else 0


def minimize_own_console() -> int:
    """把自己所在的控制台窗口最小化，避免它压在录制窗口上（不影响已取得的前台）。"""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.GetConsoleWindow.restype = w.HWND
    hwnd = k32.GetConsoleWindow()
    if hwnd:
        u.ShowWindow(hwnd, 6)  # SW_MINIMIZE
    return int(hwnd or 0)


def app_arguments(recipe: dict, options: argparse.Namespace, run_dir: Path) -> list[str]:
    args = [str(options.exe), f"--rhi={options.backend}", "--scene=" + recipe["scene"],
            f"--width={recipe['width']}", f"--height={recipe['height']}",
            f"--frames={recipe['frames']}", "--exercise-changes", f"--migration-level={recipe.get('migrationLevel', 9)}",
            f"--output={run_dir}"]
    if recipe.get("vsync"):
        args.append("--vsync=on")
    if recipe.get("tourHoldFrames"):
        args.append(f"--tour-hold-frames={recipe['tourHoldFrames']}")
    temporary = recipe.get("tourTemporaryExtent")
    if temporary:
        args += [f"--tour-temporary-width={temporary['width']}", f"--tour-temporary-height={temporary['height']}"]
    if options.manifest:
        args.append(f"--manifest={options.manifest}")
    if options.debug_layer:
        args.append("--debug")
    return args


def window_client_rect(hwnd: int) -> tuple[int, int, int, int]:
    """窗口客户区在**物理像素**下的位置与尺寸。

    D3D 窗口按 DPI 缩放呈现（本机 175%），而 gdigrab/GDI 抓不到 flip-model 内容
    （实测抓到纯白），所以改用 ddagrab（Desktop Duplication）并按客户区精确取景。
    """
    u.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    u.ClientToScreen.argtypes = [w.HWND, ctypes.POINTER(w.POINT)]
    rect = w.RECT()
    if not u.GetClientRect(hwnd, ctypes.byref(rect)):
        raise OSError("GetClientRect failed")
    origin = w.POINT(0, 0)
    if not u.ClientToScreen(hwnd, ctypes.byref(origin)):
        raise OSError("ClientToScreen failed")
    width, height = rect.right - rect.left, rect.bottom - rect.top
    return origin.x, origin.y, width - width % 2, height - height % 2


def start_capture(ffmpeg: Path, fps: int, output: Path, rect: tuple[int, int, int, int]) -> subprocess.Popen:
    left, top, width, height = rect
    # ddagrab 输出 d3d11 硬件帧，必须 hwdownload 后才能进软件编码链。
    graph = (f"ddagrab=output_idx=0:framerate={fps}:draw_mouse=0:"
             f"video_size={width}x{height}:offset_x={left}:offset_y={top},hwdownload,format=bgra")
    command = [str(ffmpeg), "-y", "-hide_banner", "-loglevel", "warning", "-filter_complex", graph,
               "-c:v", "libx264", "-preset", "ultrafast", "-crf", "18", "-pix_fmt", "yuv420p", str(output)]
    return subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, creationflags=CREATE_NO_WINDOW)


def stop_capture(process: subprocess.Popen) -> None:
    try:
        if process.stdin:
            process.stdin.write(b"q")
            process.stdin.flush()
            process.stdin.close()
    except OSError:
        pass
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=15)


def sharpness_profile(ffmpeg: Path, source: Path, scan_fps: int = 8) -> list[tuple[float, float]]:
    """按固定采样率量画面清晰度（Laplacian 方差）。

    ffmpeg 自带的 blurdetect 在本片（静态场景 + DXGI 拉伸）上分辨不出差异，因此改为在
    抽帧上用 Pillow/numpy 算高频能量：临时 extent 段被拉伸，清晰度会明显下降。
    """
    import tempfile
    import numpy as np
    from PIL import Image

    with tempfile.TemporaryDirectory() as folder:
        pattern = str(Path(folder) / "f%05d.png")
        command = [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-i", str(source),
                   "-vf", f"fps={scan_fps}", "-an", pattern]
        subprocess.run(command, check=True, creationflags=CREATE_NO_WINDOW)
        profile = []
        for index, path in enumerate(sorted(Path(folder).glob("f*.png"))):
            array = np.asarray(Image.open(path).convert("L").resize((480, 270)), dtype=np.float32)
            vertical = np.abs(np.diff(array, axis=0))[:, :-1]
            horizontal = np.abs(np.diff(array, axis=1))[:-1, :]
            gradient = vertical + horizontal
            profile.append(((index + 0.5) / scan_fps, float(gradient.var())))
        if not profile:
            raise ValueError("frame scan produced no images; cannot anchor the timeline")
        return profile


def temporary_segment(profile: list[tuple[float, float]], minimum_seconds: float = 0.5) -> tuple[float, float]:
    """找最长的"清晰度下降"连续段：临时 extent 被 DXGI 拉伸的那几秒。

    下降幅度实测约 6%（1920×1080 的交换链本来就被 DWM 放大到物理像素，再降到 960×540 只是多一层），
    所以判据取"低于中位数 5%"而不是固定的大幅阈值。
    """
    from statistics import median

    baseline = median(value for _, value in profile)
    threshold = baseline * 0.95
    spacing = (profile[1][0] - profile[0][0]) if len(profile) > 1 else 1.0
    minimum_samples = max(2, int(round(minimum_seconds / spacing)))
    runs: list[list[float]] = []
    current: list[float] = []
    for time_s, value in profile:
        if value < threshold:
            current.append(time_s)
        elif current:
            runs.append(current)
            current = []
    if current:
        runs.append(current)
    runs = [run for run in runs if len(run) >= minimum_samples]
    if not runs:
        raise ValueError("no low-sharpness segment found: temporary extent may not be visible")
    longest = max(runs, key=len)
    return longest[0], longest[-1]


def subtitle_filter(times: dict, font: Path, backend: str) -> str:
    font_path = str(font).replace("\\", "/").replace(":", "\\:")
    parts = []
    for key, text in CUES:
        window = times[key]
        body = text.format(backend=backend)
        escaped = body.replace("\\", "").replace("'", "").replace(":", "：").replace(",", "，").replace("%", "％")
        parts.append(
            f"drawtext=fontfile='{font_path}':text='{escaped}':fontcolor=white:fontsize=34:"
            f"box=1:boxcolor=black@0.55:boxborderw=16:x=(w-text_w)/2:y=h-text_h-64:"
            f"enable='between(t,{window[0]:.3f},{window[1]:.3f})'")
    return ",".join(parts)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--recipe", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--backend", default="d3d12")
    parser.add_argument("--ffmpeg", default=Path("C:/tools/ffmpeg/bin/ffmpeg.exe"))
    parser.add_argument("--font", default=Path("C:/Windows/Fonts/msyh.ttc"))
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--title", default=WINDOW_TITLE)
    parser.add_argument("--debug-layer", action="store_true")
    parser.add_argument("--reuse-raw", type=Path,
                        help="复用已录制的 raw 视频：跳过采集，只做时间线锚定、烧字幕与转码")
    options = parser.parse_args(argv)

    for path in (options.exe, options.recipe, options.ffmpeg, options.font):
        if not Path(path).is_file():
            raise SystemExit(f"missing required file: {path}")
    if options.reuse_raw:
        if not options.reuse_raw.is_file():
            raise SystemExit(f"raw video missing: {options.reuse_raw}")
    elif options.work.exists():
        raise SystemExit(f"work directory must be fresh: {options.work}")
    run_dir = options.work / "run"
    run_dir.mkdir(parents=True, exist_ok=True)
    recipe = read_json(options.recipe)
    raw = options.work / "raw.mkv"
    final = options.work / "demo.mp4"

    application = app_arguments(recipe, options, run_dir)
    if options.reuse_raw:
        raw = options.reuse_raw
        return finish(options, recipe, run_dir, raw, final)
    demo_log = (options.work / "demo.log").open("wb")
    process = subprocess.Popen(application, stdin=subprocess.DEVNULL, stdout=demo_log,
                               stderr=subprocess.STDOUT, creationflags=CREATE_NO_WINDOW)
    capture = None
    try:
        deadline = time.monotonic() + 60
        while True:
            hwnd = find_window(options.title)
            if hwnd and u.GetForegroundWindow() == hwnd and not u.IsIconic(hwnd):
                break
            if process.poll() is not None:
                raise SystemExit("demo exited before its window became foreground")
            if time.monotonic() >= deadline:
                raise SystemExit("demo window did not reach foreground; refusing to record a covered window")
            time.sleep(0.5)
        capture_rect = window_client_rect(hwnd)
        minimize_own_console()
        capture = start_capture(options.ffmpeg, options.fps, raw, capture_rect)
        process.wait()
        stop_capture(capture)
        capture = None
        if process.returncode:
            raise SystemExit(f"demo exited with {process.returncode}")
    finally:
        if capture is not None:
            stop_capture(capture)
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=15)
        demo_log.close()

    return finish(options, recipe, run_dir, raw, final, capture_rect=capture_rect)


def finish(options: argparse.Namespace, recipe: dict, run_dir: Path, raw: Path, final: Path,
           times: dict | None = None, temp_start: float | None = None,
           temp_end: float | None = None, capture_rect: tuple[int, int, int, int] | None = None) -> int:
    """时间线锚定（若未给出则用 blurdetect 找临时 extent 段）+ 烧字幕 + 转码 + 落结果。"""
    if times is None:
        profile = sharpness_profile(options.ffmpeg, raw)
        temp_start, temp_end = temporary_segment(profile)
        hold_seconds = (recipe.get("tourHoldFrames", 0) or 0) / options.fps
        times = {
            "intro": (0.0, max(0.0, temp_start - hold_seconds - 0.2)),
            "suspend": (max(0.0, temp_start - hold_seconds - 0.2), temp_start),
            "temporary": (temp_start, temp_start + 1.2),
            "reject": (temp_start + 1.2, temp_end),
            "restored": (temp_end, 10_000.0),
        }
    # 目标尺寸固定为 1920×1080：gdigrab 在 DPI 缩放下抓的是物理像素（本机 175% → 3360×1890）。
    filters = f"scale=1920:1080:flags=lanczos,setsar=1,{subtitle_filter(times, options.font, options.backend.upper())}"
    encode = [str(options.ffmpeg), "-y", "-hide_banner", "-loglevel", "warning", "-i", str(raw),
              "-vf", filters, "-c:v", "libx264", "-preset", "medium", "-crf", "20", "-pix_fmt", "yuv420p",
              "-movflags", "+faststart", "-an", str(final)]
    subprocess.run(encode, check=True, creationflags=CREATE_NO_WINDOW)

    result = {
        "schemaVersion": 1,
        "kind": "demo recording (real run, no capture injection, subtitles burned)",
        "backend": options.backend,
        "executableSha256": sha256(options.exe), "recipeSha256": sha256(options.recipe),
        "rawSha256": sha256(raw), "videoSha256": sha256(final),
        "rawBytes": raw.stat().st_size, "videoBytes": final.stat().st_size,
        "fps": options.fps, "dpiAwareness": DPI_AWARENESS,
        "cueTimesSeconds": {key: list(value) for key, value in times.items()},
        "captureRectPhysicalPixels": ({"left": capture_rect[0], "top": capture_rect[1],
                                       "width": capture_rect[2], "height": capture_rect[3]}
                                      if capture_rect else None),
        "temporarySegmentSeconds": [temp_start, temp_end],
        "recipe": {key: recipe[key] for key in ("frames", "warmupFrames", "width", "height", "vsync",
                                                "tourHoldFrames", "tourTemporaryExtent") if key in recipe},
        "tourEvents": [json.loads(line) for line in (run_dir / "tour-events.jsonl").read_text(
            encoding="utf-8").splitlines() if line.strip()],
        "anchors": {"anchorJsonSha256": sha256(run_dir / "anchor.json"),
                    "anchorPpmSha256": sha256(run_dir / "anchor.ppm"),
                    "colorPpmSha256": sha256(run_dir / "color.ppm")},
    }
    result["eventSequence"] = collapse([event["marker"] for event in result["tourEvents"]])
    (options.work / "RECORD-RESULT.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                                                     encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("backend", "videoSha256", "videoBytes", "fps",
                                                   "temporarySegmentSeconds", "eventSequence")},
                     ensure_ascii=False, indent=2))
    return 0


def sha256(path: Path) -> str:
    import hashlib
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


if __name__ == "__main__":
    sys.exit(main())
