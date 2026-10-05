"""Launch the game repeatedly and classify each run as ok / crash.
usage: stress.py <runs> <label>
ok    = GTA5_Enhanced.exe still alive 130s after it started
crash = process vanished early or a new crash dump appeared"""
import glob, os, subprocess, sys, time, psutil
GAME = "GTA5_Enhanced.exe"
DUMPS = os.path.expandvars(r"%LOCALAPPDATA%\Rockstar Games\GTAV Enhanced\CrashLogs\*.dmp")
STEAM = r"C:\Program Files (x86)\Steam\steam.exe"
def procs(*names):
    return [p for p in psutil.process_iter(["name"]) if p.info["name"] in names]
def wait_gone(names, timeout):
    end = time.time() + timeout
    while time.time() < end and procs(*names): time.sleep(1)
    return not procs(*names)
runs, label = int(sys.argv[1]), sys.argv[2]
results = []
for i in range(runs):
    for p in procs(GAME): p.terminate()
    wait_gone((GAME,), 30) or [p.kill() for p in procs(GAME)]
    wait_gone(("PlayGTAV.exe", "Launcher.exe"), 60)
    before = set(glob.glob(DUMPS))
    subprocess.Popen([STEAM, "-applaunch", "3240220", "-nobattleye"])
    start = time.time(); game = None
    while time.time() - start < 180 and not game:
        g = procs(GAME); game = g[0] if g else None; time.sleep(1)
    if not game:
        results.append("nostart"); print(f"[{label}] run {i+1}: nostart", flush=True); continue
    t0 = time.time(); outcome = "ok"
    while time.time() - t0 < 130:
        if not game.is_running() or set(glob.glob(DUMPS)) - before:
            outcome = f"crash@{int(time.time()-t0)}s"; break
        time.sleep(1)
    results.append(outcome); print(f"[{label}] run {i+1}: {outcome}", flush=True)
for p in procs(GAME): p.terminate()
print(f"[{label}] SUMMARY: {sum(r=='ok' for r in results)}/{len(results)} ok -> {results}", flush=True)
