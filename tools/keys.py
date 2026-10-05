"""Send keyboard/mouse input to the foreground game window via scancodes.
usage: keys.py focus | key <name> [hold_s] | click <x> <y> | seq k1,k2,...[:delay]"""
import sys, time, ctypes, pydirectinput as d, psutil
d.PAUSE = 0.05
u32 = ctypes.windll.user32

def focus():
    import ctypes.wintypes as w
    pids = {p.pid for p in psutil.process_iter(['name']) if p.info['name'] == 'GTA5_Enhanced.exe'}
    found = []
    @ctypes.WINFUNCTYPE(ctypes.c_bool, w.HWND, w.LPARAM)
    def cb(h, _):
        pid = w.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value in pids and u32.IsWindowVisible(h): found.append(h)
        return True
    u32.EnumWindows(cb, 0)
    if found:
        u32.ShowWindow(found[0], 9); u32.SetForegroundWindow(found[0])
    return bool(found)

cmd = sys.argv[1]
if cmd == "focus": print(focus())
elif cmd == "key":
    focus(); time.sleep(0.2)
    hold = float(sys.argv[3]) if len(sys.argv) > 3 else 0.08
    d.keyDown(sys.argv[2]); time.sleep(hold); d.keyUp(sys.argv[2])
elif cmd == "click":
    focus(); time.sleep(0.2)
    d.moveTo(int(sys.argv[2]), int(sys.argv[3])); time.sleep(0.15)
    d.mouseDown(); time.sleep(0.12); d.mouseUp()
elif cmd == "seq":
    focus(); time.sleep(0.2)
    spec, _, delay = sys.argv[2].partition(":")
    for k in spec.split(","):
        d.keyDown(k); time.sleep(0.1); d.keyUp(k); time.sleep(float(delay or 0.4))
