"""Motion strip: takes screenshots at fixed intervals and lays them out in one image (to judge motion from a still).
usage: strip.py <out.png> <count> <interval seconds> [crop: left,top,right,bottom as fractions] [columns]
Each frame is labelled with its time offset."""
import sys, time
import mss
from PIL import Image, ImageDraw

out, count, interval = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
crop = [float(v) for v in sys.argv[4].split(',')] if len(sys.argv) > 4 else [0, 0, 1, 1]
cols = int(sys.argv[5]) if len(sys.argv) > 5 else count
frames = []
with mss.MSS() as s:
    mon = s.monitors[1]
    t0 = time.time()
    for i in range(count):
        target = t0 + i * interval
        while time.time() < target:
            time.sleep(0.002)
        shot = s.grab(mon)
        img = Image.frombytes('RGB', shot.size, shot.rgb)
        w, h = img.size
        img = img.crop((int(crop[0] * w), int(crop[1] * h), int(crop[2] * w), int(crop[3] * h)))
        ImageDraw.Draw(img).text((8, 8), f'{time.time() - t0:.2f}s', fill=(255, 255, 0))
        frames.append(img)
fw, fh = frames[0].size
scale = min(1.0, 2400 / (fw * cols))
fw2, fh2 = int(fw * scale), int(fh * scale)
rows = (count + cols - 1) // cols
sheet = Image.new('RGB', (fw2 * cols, fh2 * rows))
for i, f in enumerate(frames):
    sheet.paste(f.resize((fw2, fh2)), ((i % cols) * fw2, (i // cols) * fh2))
sheet.save(out)
print('wrote', out, sheet.size)
