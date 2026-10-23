"""Test control for a running game (loader.json "debugRemote": true; src/loader/debug/remote.hpp), without the game window
in front: the person at the computer keeps using it.

usage:
  gtactl.py launch                 test settings on (windowed, no pause on focus loss, debugRemote), start the game,
                                   continue story mode; the window stays out of view and silent. The Rockstar launcher
                                   runs in offline mode (-scOfflineOnly, passed on by Steam): story mode needs no
                                   sign-in, and Rockstar's sign-in has been failing
  gtactl.py restore                the person's settings back (after the game closed)
  gtactl.py quit                   closes the game, then restore
  gtactl.py convert <file.grec> <file.mp4> [fps]
                                   a recording (see video:) as an MP4
  gtactl.py <step> [<step> ...]    steps run in order:
      key:<name>                   a mod hotkey (F9, F10, Q, ...)
      gamekey:<name>[,<ms>]        a key for the game itself (raw input; works in frontend menus such as the save screen)
      ctl:<id>[,<ms>[,<value>]]    a game control held (51 = context/E, 172-177 = phone/menu up down left right select back)
      hold:<id>,<ms>               the same, then waits until it was let go
      shot:<file.png>              the next frame
      video:<file.mp4>,<seconds>[,<fps>[,<scale>]]
                                   that long of what the game presents (30 frames a second, half size: the defaults),
                                   shown at the pace it was presented (a hitch shows as one); prints the frame times.
                                   Runs while the steps after it wait, so: video:a.mp4,10 raw:spin 60 8 wait:11
      wait:<seconds>
      log:<regex>[,<timeout s>]    waits for a line in the police mod's log (newer than this run's start)
      raw:<command line>           any pipe command (ping, story, window offscreen|restore, mute on|off, release,
                                   fakefocus <ms>: GetForegroundWindow reports the game for that long,
                                   cam x y z heading pitch [fov [keep]] | cam off: a fixed scripted camera at noon,
                                   clear sky ("keep": the clock and weather left as they are; while a scripted camera
                                   is on, the game keeps its shadows' sun where it was when the camera came on),
                                   tp x y z: the player there, depthdump <file>: the next DLSS pass's depth and stencil,
                                   spin <degrees a second> <seconds>: the gameplay camera turned round the player,
                                   native <hash> [args]: a native called on the game thread (integers, floats with a
                                   '.', s:text) -> "ok <int> <float> <x> <y> <z>" (tools/ue5soak.py drives with it),
                                   record <seconds> <fps> <scale> <file> | record status: see video:,
                                   time h [m] [run]: the clock there, stopped (the game's sun then stays where it
                                   was: "run" lets it go on), date d m y: the calendar, weather <type>: kept)
Exits 1 on the first step that fails.
"""
import json, os, re, shutil, struct, subprocess, sys, threading, time

PIPE = r"\\.\pipe\gtave-modloader"
GAME = r"D:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced"
LOADER_JSON = os.path.join(GAME, "ModLoader", "loader.json")
SETTINGS = os.path.expanduser(r"~\Documents\Rockstar Games\GTAV Enhanced\settings.xml")
BACKUP = SETTINGS + ".gtactl-backup"
MOD_LOG = os.path.join(GAME, "ModLoader", "mods", "police", "log.txt")
LAUNCHER_LOG = os.path.expanduser(r"~\Documents\Rockstar Games\Launcher\launcher.log")


class Pipe:
    def __init__(self, timeout=0):
        end = time.time() + timeout
        while True:
            try:
                self.f = open(PIPE, "r+b", buffering=0)
                return
            except OSError:
                if time.time() >= end:
                    raise
                time.sleep(0.5)

    def send(self, line):
        self.f.write((line + "\n").encode("utf-8"))
        reply = b""
        while not reply.endswith(b"\n"):
            chunk = self.f.read(1)
            if not chunk:
                raise OSError("pipe closed")
            reply += chunk
        return reply.decode("utf-8").strip()


def set_setting(text, name, value):
    return re.sub(rf'<{name} value="[^"]*" />', f'<{name} value="{value}" />', text)


def test_settings(on):
    """The game's display settings and loader.json for a test run, or the person's back."""
    with open(LOADER_JSON, encoding="utf-8") as f:
        cfg = json.load(f)
    cfg["debugRemote"] = on
    with open(LOADER_JSON, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
    if on:
        if not os.path.exists(BACKUP):
            shutil.copy2(SETTINGS, BACKUP)
        with open(SETTINGS, encoding="utf-8") as f:
            text = f.read()
        text = set_setting(text, "Windowed", 1)
        text = set_setting(text, "PauseOnFocusLoss", 0)
        # (more for one run: GTACTL_SETTINGS="name=value,name=value")
        for item in filter(None, os.environ.get("GTACTL_SETTINGS", "").split(",")):
            name, value = item.split("=", 1)
            text = set_setting(text, name.strip(), value.strip())
        with open(SETTINGS, "w", encoding="utf-8") as f:
            f.write(text)
    elif os.path.exists(BACKUP):
        shutil.move(BACKUP, SETTINGS)


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq GTA5_Enhanced.exe"], capture_output=True, text=True).stdout
    return "GTA5_Enhanced.exe" in out


def sign_in_failed(since):
    """The Rockstar launcher started after `since` and could not sign in (its fatal "OnScAuthError")."""
    try:
        if os.path.getmtime(LAUNCHER_LOG) < since:
            return False
        with open(LAUNCHER_LOG, encoding="utf-8", errors="replace") as f:
            return "OnScAuthError with fatal error" in f.read()
    except OSError:
        return False


def launch():
    if game_running():
        sys.exit("the game is already running")
    test_settings(True)
    started = time.time()
    # Steam passes extra arguments on to the Rockstar launcher. (Killing a launcher stuck on its sign-in error and
    # starting another one makes PlayGTAV, Steam's process, show "unable to start the game" on the desktop.)
    subprocess.Popen([r"C:\Program Files (x86)\Steam\steam.exe", "-applaunch", "3240220", "-nobattleye", "-scOfflineOnly"])
    end = time.time() + 300
    while True:
        try:
            p = Pipe()
            break
        except OSError:
            if time.time() >= end:
                sys.exit("the game did not start" + (" (the launcher could not sign in)" if sign_in_failed(started) else ""))
        time.sleep(1)
    print("pipe up:", p.send("ping"))
    end = time.time() + 300
    while time.time() < end:
        state = p.send("ping")
        if state == "ok landing":
            time.sleep(3)
            print("story:", p.send("story"))
        elif state == "ok story":
            print("story mode is running")
            return p
        time.sleep(2)
    sys.exit("story mode did not start")


def quit_game():
    # Windows keeps the game's mute for its next start: undone first (else the loader undoes it at the next start).
    try:
        Pipe().send("mute off")
    except OSError:
        pass
    subprocess.run(["taskkill", "/F", "/IM", "GTA5_Enhanced.exe"], capture_output=True)
    time.sleep(3)
    test_settings(False)


def log_size():
    try:
        return os.path.getsize(MOD_LOG)
    except OSError:
        return 0


def wait_log(pattern, timeout, since):
    rx = re.compile(pattern)
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(MOD_LOG, "rb") as f:
                f.seek(since)
                for line in f.read().decode("utf-8", "replace").splitlines():
                    if rx.search(line):
                        return line
        except OSError:
            pass
        time.sleep(0.5)
    return None


def read_recording(path):
    """The frames (time, width, height, tags, BGR bytes) and every present's time of a recorder file."""
    frames, presents = [], []
    with open(path, "rb") as f:
        magic, version, scale = struct.unpack("<III", f.read(12))
        if magic != 0x43455247:
            raise ValueError("not a recording")
        while True:
            head = f.read(4)
            if len(head) < 4:
                break
            kind = struct.unpack("<I", head)[0]
            if kind == 1:
                t, w, h = struct.unpack("<dII", f.read(16))
                tags = struct.unpack("<IIII", f.read(16))
                frames.append((t, w, h, tags, f.read(w * h * 3)))
            elif kind == 2:
                n = struct.unpack("<I", f.read(4))[0]
                presents = list(struct.unpack(f"<{n}d", f.read(8 * n)))
            else:
                break
    return frames, presents


def frame_stats(presents):
    gaps = sorted((b - a) * 1000 for a, b in zip(presents, presents[1:]))
    if not gaps:
        return "no frames"
    mean = sum(gaps) / len(gaps)
    return (f"{len(presents)} presents, {1000 / mean:.1f} fps; frame time median {gaps[len(gaps) // 2]:.1f} ms, 99% {gaps[int(len(gaps) * 0.99)]:.1f} ms, "
            f"slowest {gaps[-1]:.1f} ms; over 33 ms: {sum(g > 33.4 for g in gaps)}, over 50 ms: {sum(g > 50 for g in gaps)}")


def convert(src, dst, fps=30):
    import cv2
    import numpy as np
    frames, presents = read_recording(src)
    if not frames:
        return "no frames"
    w, h = frames[0][1], frames[0][2]
    out = cv2.VideoWriter(dst, cv2.VideoWriter_fourcc(*"mp4v"), fps, (w, h))
    # (at the pace the frames came: each moment of the video shows the newest frame by then)
    end, i, k = frames[-1][0], 0, 0
    while k / fps <= end:
        while i + 1 < len(frames) and frames[i + 1][0] <= k / fps:
            i += 1
        t, fw, fh, tags, pixels = frames[i]
        image = np.frombuffer(pixels, np.uint8).reshape(fh, fw, 3)
        out.write(image if (fw, fh) == (w, h) else cv2.resize(image, (w, h)))
        k += 1
    out.release()
    with open(os.path.splitext(dst)[0] + ".csv", "w") as f:
        f.write("time,tag0,tag1,tag2,tag3\n")
        for t, fw, fh, tags, pixels in frames:
            f.write(f"{t:.4f},{tags[0]},{tags[1]},{tags[2]},{tags[3]}\n")
    return f"{len(frames)} frames, {w}x{h}; " + frame_stats(presents)


def main(args):
    if args[:1] == ["convert"] and len(args) >= 3:
        print(convert(args[1], args[2], float(args[3]) if len(args) > 3 else 30))
        return
    if args == ["launch"]:
        launch()
        return
    if args == ["restore"]:
        test_settings(False)
        return
    if args == ["quit"]:
        quit_game()
        return
    p = Pipe(timeout=5)
    since = log_size()
    background = []
    for step in args:
        kind, _, arg = step.partition(":")
        if kind == "wait":
            time.sleep(float(arg))
            continue
        if kind == "log":
            pattern, _, timeout = arg.rpartition(",") if re.search(r",\d+$", arg) else (arg, "", "30")
            line = wait_log(pattern, float(timeout), since)
            print(line if line else f"no log line /{pattern}/")
            if not line:
                sys.exit(1)
            continue
        if kind == "key":
            reply = p.send(f"key {arg}")
        elif kind == "gamekey":
            reply = p.send("gamekey " + " ".join(arg.split(",")))
        elif kind in ("ctl", "hold"):
            parts = arg.split(",")
            reply = p.send("control " + " ".join(parts))
            if kind == "hold":
                time.sleep(int(parts[1]) / 1000 + 0.1)
        elif kind == "shot":
            reply = p.send(f"shot {os.path.abspath(arg)}")
        elif kind == "video":
            parts = arg.split(",")
            dst, seconds = os.path.abspath(parts[0]), float(parts[1])
            fps = parts[2] if len(parts) > 2 else "30"
            scale = parts[3] if len(parts) > 3 else "2"
            raw = os.path.splitext(dst)[0] + ".grec"
            reply = p.send(f"record {seconds} {fps} {scale} {raw}")

            def finish(dst=dst, raw=raw, seconds=seconds, fps=float(fps)):
                time.sleep(seconds)
                q = Pipe(timeout=5)
                while q.send("record status") == "ok recording":
                    time.sleep(0.5)
                print(f"video {dst}: {convert(raw, dst, fps)}")
            background.append(threading.Thread(target=finish))
            background[-1].start()
        elif kind == "raw":
            reply = p.send(arg)
        else:
            sys.exit(f"unknown step {step}")
        if not reply.startswith("ok"):
            print(f"{step}: {reply}")
            sys.exit(1)
        if reply != "ok":
            print(reply)
    for t in background:
        t.join()


if __name__ == "__main__":
    main(sys.argv[1:])
