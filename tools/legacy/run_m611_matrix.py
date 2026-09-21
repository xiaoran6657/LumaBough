"""M6-11 真实后端矩阵：稳定性运行、窗口组合（resize/minimize/foreground + hot reload）。

模式：
- stability：隐藏窗口长跑，输出 diagnostics.csv，随后由 analyze_m611_stability.py 判定。
- combos：可见窗口 + --exercise-changes，脚本驱动 Win32 前台/最小化/resize，核对 reload/resize 计数。

本脚本只操作本次启动的 sample 与脚本自建焦点窗口；不修改生产配置。
"""
from __future__ import annotations

import argparse
import ctypes
import json
import subprocess
import time
from ctypes import wintypes as w
from pathlib import Path

u = ctypes.WinDLL("user32", use_last_error=True)
u.GetForegroundWindow.restype = w.HWND
u.SetForegroundWindow.argtypes = [w.HWND]
u.ShowWindow.argtypes = [w.HWND, ctypes.c_int]
u.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
u.IsWindowVisible.argtypes = [w.HWND]
u.IsIconic.argtypes = [w.HWND]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                              ctypes.c_int, w.HWND, w.HMENU, w.HINSTANCE, w.LPVOID]
u.CreateWindowExW.restype = w.HWND
u.DestroyWindow.argtypes = [w.HWND]
u.AttachThreadInput.argtypes = [w.DWORD, w.DWORD, w.BOOL]
u.PeekMessageW.argtypes = [ctypes.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT]
u.TranslateMessage.argtypes = [ctypes.POINTER(w.MSG)]
u.DispatchMessageW.argtypes = [ctypes.POINTER(w.MSG)]
u.BringWindowToTop.argtypes = [w.HWND]
k32 = ctypes.WinDLL("kernel32")
k32.GetCurrentThreadId.restype = w.DWORD


def pause(seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        message = w.MSG()
        while u.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
            u.TranslateMessage(ctypes.byref(message))
            u.DispatchMessageW(ctypes.byref(message))
        time.sleep(0.01)


def focus(hwnd) -> bool:
    foreground = u.GetForegroundWindow()
    other = u.GetWindowThreadProcessId(foreground, None) if foreground else 0
    current = k32.GetCurrentThreadId()
    attached = other and other != current and u.AttachThreadInput(current, other, True)
    try:
        u.BringWindowToTop(hwnd)
        return bool(u.SetForegroundWindow(hwnd))
    finally:
        if attached:
            u.AttachThreadInput(current, other, False)


Callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [Callback, w.LPARAM]


def window_for(pid: int):
    found = []

    @Callback
    def callback(hwnd, _):
        process = w.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(process))
        if process.value == pid and u.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    u.EnumWindows(callback, 0)
    return found[0] if found else None


def wait_for_window(process: subprocess.Popen, directory: Path, timeout: float = 30.0):
    limit = time.monotonic() + timeout
    while time.monotonic() < limit:
        hwnd = window_for(process.pid)
        if hwnd or process.poll() is not None:
            return hwnd
        pause(0.05)
    return None


def base_command(exe: Path, backend: str, frames: int, directory: Path, interval: int, debug: bool, gbv: bool,
                 headless: bool) -> list[str]:
    command = [str(exe), f"--rhi={backend}", f"--frames={frames}", f"--output={directory}",
               f"--diagnostics-interval={interval}"]
    if debug:
        command.append("--debug")
    if gbv:
        command.append("--gbv")
    if headless:
        command.append("--headless")
    return command


def read_metadata(directory: Path) -> dict:
    return json.loads((directory / "metadata.json").read_text(encoding="utf-8"))


def run_stability(exe: Path, output: Path, backend: str, frames: int, interval: int, debug: bool, gbv: bool) -> dict:
    directory = output / backend
    directory.mkdir(parents=True, exist_ok=True)
    command = base_command(exe, backend, frames, directory, interval, debug, gbv, headless=True)
    start = time.monotonic()
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        completed = subprocess.run(command, stdout=out, stderr=err, timeout=7200)
    seconds = time.monotonic() - start
    (directory / "invocation.json").write_text(json.dumps({"command": command, "exitCode": completed.returncode,
                                                           "seconds": seconds}, indent=2), encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(f"{backend} stability run failed: {(directory / 'stderr.log').read_text(errors='replace')[-2000:]}")
    metadata = read_metadata(directory)
    if metadata["warningErrors"] != 0:
        raise RuntimeError(f"{backend} stability run reported warning/errors")
    return {"backend": backend, "frames": frames, "seconds": round(seconds, 2), "debugLayer": metadata["debugLayer"],
            "gpuValidation": metadata["gpuValidation"], "warningErrors": metadata["warningErrors"],
            "directories": str(directory)}


def run_combos(exe: Path, output: Path, backend: str, frames: int, interval: int, gbv: bool, cycles: int) -> dict:
    directory = output / backend
    directory.mkdir(parents=True, exist_ok=True)
    command = base_command(exe, backend, frames, directory, interval, debug=True, gbv=gbv, headless=False)
    command.append("--exercise-changes")
    events = []
    original = u.GetForegroundWindow()
    helper = u.CreateWindowExW(0, "STATIC", "MiniEngine M6-11 focus probe", 0x10CF0000, 640, 80, 240, 100, None, None,
                               None, None)
    if not helper:
        raise RuntimeError("focus probe window creation failed")
    try:
        with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
            process = subprocess.Popen(command, stdout=out, stderr=err)
            try:
                hwnd = wait_for_window(process, directory)
                if not hwnd or process.poll() is not None:
                    raise RuntimeError("sample window did not appear")
                focus(hwnd)
                pause(0.7)
                for cycle in range(cycles):
                    if process.poll() is not None:
                        raise RuntimeError("sample exited before stress completed")
                    focus(helper)
                    pause(0.4)
                    events.append({"cycle": cycle, "event": "foreground-away",
                                   "nativeObserved": u.GetForegroundWindow() != hwnd})
                    if u.GetForegroundWindow() == hwnd:
                        raise RuntimeError("foreground-away request did not take effect")
                    u.ShowWindow(hwnd, 6)
                    pause(0.4)
                    if not u.IsIconic(hwnd):
                        raise RuntimeError("native minimize did not take effect")
                    events.append({"cycle": cycle, "event": "minimized", "nativeObserved": True})
                    u.ShowWindow(hwnd, 9)
                    if not u.SetWindowPos(hwnd, None, 80, 80, 420 + cycle * 31, 320 + cycle * 19, 0x0004):
                        raise RuntimeError("resize failed")
                    focus(hwnd)
                    pause(0.4)
                    events.append({"cycle": cycle, "event": "restored-resized",
                                   "foreground": u.GetForegroundWindow() == hwnd})
                # 等样本自行完成固定帧数并退出；不提前关窗，否则固定帧证据不完整。
                deadline = time.monotonic() + 1800
                while process.poll() is None and time.monotonic() < deadline:
                    pause(0.25)
                if process.poll() is None:
                    u.PostMessageW(hwnd, 0x0010, 0, 0)
                code = process.wait(timeout=180)
                if code != 0:
                    raise RuntimeError(f"{backend} combos run failed with exit code {code}: "
                                       f"{(directory / 'stderr.log').read_text(errors='replace')[-2000:]}")
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=15)
    finally:
        u.DestroyWindow(helper)
        if original:
            focus(original)
    metadata = read_metadata(directory)
    if metadata["warningErrors"] != 0 or metadata["reloadSuccess"] != 1 or metadata["reloadRejected"] != 1:
        raise RuntimeError(f"{backend} combos run diagnostics/reload gate failed: {metadata}")
    if metadata["resizeCount"] < 3:
        raise RuntimeError(f"{backend} combos run did not exercise resize: {metadata['resizeCount']}")
    (directory / "window-events.json").write_text(json.dumps(events, indent=2), encoding="utf-8")
    return {"backend": backend, "frames": frames, "resizeCount": metadata["resizeCount"],
            "reloadSuccess": metadata["reloadSuccess"], "reloadRejected": metadata["reloadRejected"],
            "warningErrors": metadata["warningErrors"], "events": len(events), "directories": str(directory)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--mode", choices=["stability", "combos"], required=True)
    parser.add_argument("--backend", choices=["d3d11", "d3d12"], required=True)
    parser.add_argument("--frames", type=int, required=True)
    parser.add_argument("--interval", type=int, default=300)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--gbv", action="store_true")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.mode == "stability":
        report = run_stability(args.exe, args.output, args.backend, args.frames, args.interval, args.debug, args.gbv)
    else:
        report = run_combos(args.exe, args.output, args.backend, args.frames, args.interval, args.gbv, args.cycles)
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
