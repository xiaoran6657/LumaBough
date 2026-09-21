"""对当前启动的 RHI sandbox 做真实 Win32 resize/minimize/foreground 验证。"""
from __future__ import annotations
import argparse, ctypes, json, subprocess, time
from ctypes import wintypes as w
from pathlib import Path
u = ctypes.WinDLL("user32", use_last_error=True)
u.GetForegroundWindow.restype = w.HWND
u.SetForegroundWindow.argtypes = [w.HWND]
u.ShowWindow.argtypes = [w.HWND, ctypes.c_int]
u.SetWindowPos.argtypes = [w.HWND,w.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,w.UINT]
u.GetWindowThreadProcessId.argtypes = [w.HWND,ctypes.POINTER(w.DWORD)]
u.IsWindowVisible.argtypes = [w.HWND]
u.IsIconic.argtypes = [w.HWND]
u.PostMessageW.argtypes = [w.HWND,w.UINT,w.WPARAM,w.LPARAM]
u.CreateWindowExW.argtypes = [w.DWORD,w.LPCWSTR,w.LPCWSTR,w.DWORD,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,w.HWND,w.HMENU,w.HINSTANCE,w.LPVOID]
u.CreateWindowExW.restype = w.HWND
u.DestroyWindow.argtypes = [w.HWND]
u.AttachThreadInput.argtypes = [w.DWORD,w.DWORD,w.BOOL]
k32 = ctypes.WinDLL("kernel32")
k32.GetCurrentThreadId.restype = w.DWORD
u.PeekMessageW.argtypes = [ctypes.POINTER(w.MSG),w.HWND,w.UINT,w.UINT,w.UINT]
u.TranslateMessage.argtypes = [ctypes.POINTER(w.MSG)]
u.DispatchMessageW.argtypes = [ctypes.POINTER(w.MSG)]
u.BringWindowToTop.argtypes = [w.HWND]
def pause(seconds):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        message=w.MSG()
        while u.PeekMessageW(ctypes.byref(message),None,0,0,1):
            u.TranslateMessage(ctypes.byref(message))
            u.DispatchMessageW(ctypes.byref(message))
        time.sleep(.01)
def focus(hwnd):
    foreground = u.GetForegroundWindow()
    other = u.GetWindowThreadProcessId(foreground,None) if foreground else 0
    current = k32.GetCurrentThreadId()
    attached = other and other != current and u.AttachThreadInput(current,other,True)
    try:
        u.BringWindowToTop(hwnd)
        return bool(u.SetForegroundWindow(hwnd))
    finally:
        if attached: u.AttachThreadInput(current,other,False)
Callback = ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
u.EnumWindows.argtypes = [Callback,w.LPARAM]
def window_for(pid):
    found=[]
    @Callback
    def callback(hwnd, _):
        process = w.DWORD()
        u.GetWindowThreadProcessId(hwnd,ctypes.byref(process))
        if process.value == pid and u.IsWindowVisible(hwnd): found.append(hwnd)
        return True
    u.EnumWindows(callback,0)
    return found[0] if found else None
def run(exe, output, backend):
    directory=output/backend
    directory.mkdir(parents=True,exist_ok=True)
    command=[str(exe),f"--rhi={backend}","--debug","--smoke-level=6","--width=320","--height=240","--frames=600",f"--output={directory}"]
    if backend=="d3d12":command.append("--gbv")
    events=[]
    original=u.GetForegroundWindow()
    # 本测试进程拥有的临时窗口，提供可验证的跨进程前台目标。
    helper=u.CreateWindowExW(0,"STATIC","MiniEngine focus probe",0x10CF0000,640,80,240,100,None,None,None,None)
    assert helper,"focus probe window creation failed"
    with (directory/"stdout.log").open("wb") as out, (directory/"stderr.log").open("wb") as err:
        process=subprocess.Popen(command,stdout=out,stderr=err)
        try:
            limit=time.monotonic()+20
            hwnd=None
            while time.monotonic()<limit:
                hwnd=window_for(process.pid)
                if hwnd or process.poll() is not None:break
                pause(.05)
            assert hwnd and process.poll() is None,"test window did not appear"
            focus(hwnd)
            pause(.7)
            for cycle in range(3):
                assert process.poll() is None,"sample exited before stress completed"
                # 明确切到此前的前台窗口，再最小化；只最小化不保证 WM_ACTIVATEAPP。
                focus(helper)
                pause(.4)
                events.append({"cycle":cycle,"event":"foreground-away","nativeObserved":u.GetForegroundWindow()!=hwnd})
                assert u.GetForegroundWindow()!=hwnd,"foreground-away request did not take effect"
                u.ShowWindow(hwnd,6)
                pause(.4)
                assert u.IsIconic(hwnd),"native minimize did not take effect"
                events.append({"cycle":cycle,"event":"minimized","nativeObserved":True})
                u.ShowWindow(hwnd,9)
                assert u.SetWindowPos(hwnd,None,80,80,420+cycle*31,320+cycle*19,0x0004),"resize failed"
                focus(hwnd)
                pause(.4)
                assert not u.IsIconic(hwnd)
                events.append({"cycle":cycle,"event":"restored-resized","foreground":u.GetForegroundWindow()==hwnd})
            u.PostMessageW(hwnd,0x0010,0,0)
            assert process.wait(timeout=30)==0,(directory/"stderr.log").read_text(errors="replace")[-4000:]
        finally:
            if process.poll() is None: process.terminate();process.wait(timeout=10)
            u.DestroyWindow(helper)
            if original:focus(original)
    metadata=json.loads((directory/"metadata.json").read_text())
    for key in ("resizeExercised","minimizeObserved","focusLost","focusRestored","reloadExercised"): assert metadata[key],(backend,key,metadata)
    for key in ("nativeWarningErrors","nativeLiveResources","aliveObjects","retiringObjects"): assert metadata[key]==0,(backend,key)
    assert metadata["frames"]>6
    return {"backend":backend,"status":"PASS","events":events,"metadata":metadata,"command":command}
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--exe",type=Path,required=True);p.add_argument("--output",type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    report={"schema":"miniengine.m6-06.window-stress.v1","status":"FAIL","runs":[]}
    try:
        for backend in ("d3d11","d3d12"):
            report["runs"].append(run(args.exe.resolve(),args.output.resolve(),backend));print("PASS",backend,flush=True)
        report["status"]="PASS"
    except Exception as error:report["failure"]=str(error);print("FAIL",error,flush=True)
    (args.output/"summary.json").write_text(json.dumps(report,indent=2)+"\n")
    return 0 if report["status"]=="PASS" else 1
if __name__=="__main__":raise SystemExit(main())
