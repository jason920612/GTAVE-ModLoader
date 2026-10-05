"""Capture OutputDebugString output of every process (the classic DBWIN protocol) into a file.
usage: dbwin.py <out.txt> [substring filter]
Used with ModLoader\\debug_dred.txt = "layer": the D3D12 debug layer reports through OutputDebugString."""
import ctypes, ctypes.wintypes as w, struct, sys

k32 = ctypes.windll.kernel32
k32.CreateEventW.restype = w.HANDLE
k32.CreateFileMappingW.restype = w.HANDLE
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, ctypes.c_size_t]

buffer_ready = k32.CreateEventW(None, False, False, "DBWIN_BUFFER_READY")
data_ready = k32.CreateEventW(None, False, False, "DBWIN_DATA_READY")
mapping = k32.CreateFileMappingW(w.HANDLE(-1), None, 0x04, 0, 4096, "DBWIN_BUFFER")
view = k32.MapViewOfFile(mapping, 0x0004, 0, 0, 4096)
if not (buffer_ready and data_ready and view):
    sys.exit("DBWIN objects unavailable (another listener running?)")
needle = sys.argv[2] if len(sys.argv) > 2 else None
with open(sys.argv[1], "a", encoding="utf-8", errors="replace") as out:
    while True:
        k32.SetEvent(buffer_ready)
        if k32.WaitForSingleObject(data_ready, 0xFFFFFFFF) != 0:
            continue
        raw = ctypes.string_at(view, 4096)
        pid = struct.unpack_from("<I", raw)[0]
        text = raw[4:].split(b"\0")[0].decode("utf-8", "replace").rstrip()
        if needle is None or needle in text:
            out.write(f"[{pid}] {text}\n")
            out.flush()
