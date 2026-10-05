"""Bring the first visible window of a process to the foreground. usage: focus.py <exe name>"""
import ctypes, ctypes.wintypes as w, sys, psutil
u32 = ctypes.windll.user32
pids = {p.pid for p in psutil.process_iter(['name']) if (p.info['name'] or '').lower() == sys.argv[1].lower()}
found = []
@ctypes.WINFUNCTYPE(ctypes.c_bool, w.HWND, w.LPARAM)
def cb(h, _):
    pid = w.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    if pid.value in pids and u32.IsWindowVisible(h) and u32.GetWindowTextLengthW(h) >= 0: found.append(h)
    return True
u32.EnumWindows(cb, 0)
for h in found[:1]:
    u32.ShowWindow(h, 9)
    # Alt tap lets SetForegroundWindow succeed from a background process.
    u32.keybd_event(0x12, 0, 0, 0); u32.SetForegroundWindow(h); u32.keybd_event(0x12, 0, 2, 0)
print(len(found), "window(s)")
