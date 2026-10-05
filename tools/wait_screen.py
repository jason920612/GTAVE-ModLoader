"""Poll screenshots until the screen matches a condition. usage: wait_screen.py autosave|world <timeout_s>
autosave: the yellow-title dialog (warning screen) is visible; world: HUD minimap is visible (not a dialog)."""
import sys, time, mss
from PIL import Image
mode, timeout = sys.argv[1], float(sys.argv[2])
end = time.time() + timeout
def grab():
    with mss.MSS() as s:
        img = s.grab(s.monitors[1]); return Image.frombytes("RGB", img.size, img.rgb)
while time.time() < end:
    im = grab()
    # warning dialogs have a yellow title around the centre and black background elsewhere
    title = im.crop((800, 430, 1120, 520)).convert("RGB")
    yellow = sum(1 for r, g, b in title.getdata() if r > 200 and 150 < g < 220 and b < 90)
    corner = im.crop((0, 600, 300, 1000)).convert("L")
    dark = sum(corner.getdata()) / (300 * 400)
    if mode == "autosave" and yellow > 300 and dark < 10: print("autosave"); sys.exit(0)
    if mode == "world" and dark > 25 and yellow < 50: print("world"); sys.exit(0)
    time.sleep(1)
print("timeout"); sys.exit(1)
