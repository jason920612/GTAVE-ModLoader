"""Launch the game repeatedly and classify each run as ok / crash.
usage: stress.py <runs> <label>   (each run's ModLoader\loader.log is kept in stress_logs\<label>-<n>.log)
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
    # Start every run from a clean launcher state; a lingering launcher often never starts the game.
    for p in procs("PlayGTAV.exe", "Launcher.exe", "RockstarErrorHandler.exe"): p.kill()
    wait_gone(("PlayGTAV.exe", "Launcher.exe"), 30)
    time.sleep(5)
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
    if outcome != "ok":
        # Let the game finish writing its crash dump and exit on its own.
        end = time.time() + 90
        while time.time() < end and game.is_running(): time.sleep(1)
    src = r"D:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced\ModLoader\loader.log"
    os.makedirs("stress_logs", exist_ok=True)
    notes = ""
    try:
        text = open(src, encoding="utf-8", errors="replace").read()
        open(os.path.join("stress_logs", f"{label}-{i+1}.log"), "w", encoding="utf-8").write(text)
        notes = " ".join(sorted({w for w in ("fix:", "hang:") if any(l.split("] ", 2)[-1].startswith(w) for l in text.splitlines())}))
    except OSError:
        pass
    results.append(outcome); print(f"[{label}] run {i+1}: {outcome} {notes}", flush=True)
for p in procs(GAME): p.terminate()
print(f"[{label}] SUMMARY: {sum(r=='ok' for r in results)}/{len(results)} ok -> {results}", flush=True)
