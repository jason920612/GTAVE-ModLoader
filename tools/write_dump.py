"""Write a minidump (thread stacks + module list) of the running game, e.g. while an error box is up.
usage: write_dump.py <out.dmp>"""
import ctypes, ctypes.wintypes as w, sys
import pymem

pm = pymem.Pymem("GTA5_Enhanced.exe")
dbghelp = ctypes.windll.dbghelp
k32 = ctypes.windll.kernel32
k32.CreateFileW.restype = w.HANDLE
h = k32.CreateFileW(sys.argv[1], 0x40000000, 0, None, 2, 0x80, None)  # GENERIC_WRITE, CREATE_ALWAYS
MINIDUMP_WITH_THREAD_INFO = 0x1000
ok = dbghelp.MiniDumpWriteDump(w.HANDLE(pm.process_handle), pm.process_id, w.HANDLE(h), MINIDUMP_WITH_THREAD_INFO, None, None, None)
k32.CloseHandle(w.HANDLE(h))
print("ok" if ok else f"failed {k32.GetLastError():#x}")
