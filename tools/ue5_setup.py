"""Set up the UE5 project of the graphics mod outside the repository (research/phase0.md §58), and build it.

The project lives next to the export (its Content will hold the player's converted game assets, which never go into
the repository); Source, Config and Plugins are copied from this repository's ue5 folder on every run (the export drive
may be exFAT: no junctions there).
usage: ue5_setup.py <export folder> [--build] [--engine <UE folder>]
"""
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = sys.argv[1:]
out = os.path.abspath(args[0])
engine = args[args.index("--engine") + 1] if "--engine" in args else r"D:\Program Files\Epic Games\UE_5.8"
project = os.path.join(out, "Project")
os.makedirs(project, exist_ok=True)
shutil.copyfile(os.path.join(REPO, "ue5", "GTAVRenderer.uproject"), os.path.join(project, "GTAVRenderer.uproject"))
copied = 0
for name in ("Source", "Config", "Plugins"):
    src_root = os.path.join(REPO, "ue5", name)
    for folder, dirs, files in os.walk(src_root):
        dirs[:] = [d for d in dirs if d not in ("Binaries", "Intermediate")]
        for f in files:
            src = os.path.join(folder, f)
            dst = os.path.join(project, name, os.path.relpath(src, src_root))
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src) or os.path.getsize(dst) != os.path.getsize(src):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
                copied += 1
print(f"{copied} file(s) copied")
print("project:", os.path.join(project, "GTAVRenderer.uproject"))
if "--build" in args:
    build = os.path.join(engine, "Engine", "Build", "BatchFiles", "Build.bat")
    r = subprocess.run([build, "GTAVRendererEditor", "Win64", "Development", f"-Project={os.path.join(project, 'GTAVRenderer.uproject')}",
                        "-WaitMutex"], cwd=project)
    sys.exit(r.returncode)
