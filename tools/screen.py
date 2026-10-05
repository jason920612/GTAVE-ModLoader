"""Capture the primary monitor (or the GTA window) to a PNG for inspection."""
import sys, mss, mss.tools
out = sys.argv[1] if len(sys.argv) > 1 else "shot.png"
with mss.mss() as s:
    img = s.grab(s.monitors[1])
    from PIL import Image
    im = Image.frombytes("RGB", img.size, img.rgb)
    im.thumbnail((1600, 900))
    im.save(out)
print(out)
