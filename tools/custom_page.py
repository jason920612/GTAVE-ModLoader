"""Research: build a test 'Mods' settings page in the live pause menu and print addresses to watch."""
import struct, sys, json
sys.path.insert(0, "tools"); import memfind
pm = memfind.pm
import pymem
base = pymem.process.module_from_name(pm.process_handle, "GTA5_Enhanced.exe").lpBaseOfDll
tt = base + 0x3DD92B8
def joaat(s):
    h = 0
    for c in s.lower().encode():
        h = (h + c) & 0xffffffff; h = (h + (h << 10)) & 0xffffffff; h ^= h >> 6
    h = (h + (h << 3)) & 0xffffffff; h ^= h >> 11; return (h + (h << 15)) & 0xffffffff
def fmix(x):
    x ^= x >> 16; x = (x * 0x85ebca6b) & 0xffffffff; x ^= x >> 13; x = (x * 0xc2b2ae35) & 0xffffffff; x ^= x >> 16; return x
def add_text(label, text):
    mp = struct.unpack("<Q", pm.read_bytes(tt + 0x270, 8))[0]; cap = struct.unpack("<I", pm.read_bytes(tt + 0x278, 4))[0]
    h = joaat(label); data = text.encode() + b"\0"; s = pm.allocate(len(data)); pm.write_bytes(s, data, len(data))
    j = fmix(h) & (cap - 1)
    while True:
        eh = struct.unpack("<I", pm.read_bytes(mp + j * 16, 4))[0]
        if eh in (0, h): break
        j = (j + 1) & (cap - 1)
    pm.write_bytes(mp + j * 16 + 8, struct.pack("<Q", s), 8); pm.write_bytes(mp + j * 16, struct.pack("<I", h), 4)
    if eh == 0:
        c = struct.unpack("<I", pm.read_bytes(tt + 0x27c, 4))[0]; pm.write_bytes(tt + 0x27c, struct.pack("<I", c + 1), 4)
    return h
# locate the header tab item (target 42) and the screen array
tab = [h for h in memfind.find(struct.pack("<II", 0x2a, 0x8d0a157e), 10)
       if struct.unpack("<I", pm.read_bytes(h - 0x28, 4))[0] == 5][0]
hdr = memfind.find(struct.pack("<Q", tab - 5 * 0x28), 5)[0]
a = hdr
while 0 <= struct.unpack("<i", pm.read_bytes(a - 0x50 + 0x38, 4))[0] <= 200: a -= 0x50
screens = {}
while 0 <= (sid := struct.unpack("<i", pm.read_bytes(a + 0x38, 4))[0]) <= 200: screens[sid] = a; a += 0x50
def items_of(sid): return struct.unpack("<QH", pm.read_bytes(screens[sid], 10))
pm.write_int(tab + 4, add_text("ML_TAB_MODS", "模組"))
ctl, _ = items_of(24); vib = bytearray(pm.read_bytes(ctl + 3 * 0x28, 0x28))
aud, _ = items_of(22); sld = bytearray(pm.read_bytes(aud, 0x28))
st, _ = items_of(6);   cat = bytearray(pm.read_bytes(st + 1 * 0x28, 0x28))
SUB = int(sys.argv[1]) if len(sys.argv) > 1 else 112
struct.pack_into("<iI", cat, 0, SUB, add_text("ML_CAT_TEST", "測試模組"))
struct.pack_into("<I", vib, 4, add_text("ML_OPT_A", "測試開關")); vib[0x20] = 217
struct.pack_into("<I", sld, 4, add_text("ML_OPT_B", "測試數值")); sld[0x20] = 218
cat_arr = pm.allocate(0x28); pm.write_bytes(cat_arr, bytes(cat), 0x28)
it_arr = pm.allocate(0x50); pm.write_bytes(it_arr, bytes(vib) + bytes(sld), 0x50)
s6 = pm.read_bytes(screens[6], 0x50); s42 = pm.read_bytes(screens[42], 0x50)
pm.write_bytes(screens[42], struct.pack("<QHH", cat_arr, 1, 1) + s6[12:0x38] + s42[0x38:0x40] + s6[0x40:], 0x50)
audS = bytearray(pm.read_bytes(screens[22], 0x50)); old = pm.read_bytes(screens[SUB], 0x50)
audS[0x38:0x3c] = old[0x38:0x3c]; struct.pack_into("<QHH", audS, 0, it_arr, 2, 2)
pm.write_bytes(screens[SUB], bytes(audS), 0x50)
json.dump({"tab": tab, "screens": screens, "cat_arr": cat_arr, "it_arr": it_arr}, open("research/custom_page.json", "w"))
print(f"tab={tab:#x} s42={screens[42]:#x} sub{SUB}={screens[SUB]:#x} cat_arr={cat_arr:#x} it_arr={it_arr:#x}")
