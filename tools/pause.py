"""Open the pause menu reliably (the first Esc after loading is often swallowed)."""
import time, subprocess, mss, pydirectinput as d
from PIL import Image
def tap(k, wait=0.9):
    d.keyDown(k); time.sleep(0.15); d.keyUp(k); time.sleep(wait)
def menu_open():
    with mss.MSS() as s:
        im = s.grab(s.monitors[1]); img = Image.frombytes("RGB", im.size, im.rgb)
    reg = img.crop((310, 130, 780, 180)).convert("L")
    return sum(1 for p in reg.get_flattened_data() if p > 230) > 800
def open_menu():
    subprocess.run(["python", "tools/focus.py", "GTA5_Enhanced.exe"], capture_output=True); time.sleep(0.4)
    for _ in range(3):
        if menu_open(): return True
        tap("esc", 3.0)
    return menu_open()
if __name__ == "__main__":
    print("menu open:", open_menu())
