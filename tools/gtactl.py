"""Test control for a running game (loader.json "debugRemote": true; src/loader/debug/remote.hpp), without the game window
in front: the person at the computer keeps using it.

usage:
  gtactl.py launch                 test settings on (windowed, no pause on focus loss, debugRemote), start the game,
                                   continue story mode; the window stays out of view and silent. The Rockstar launcher
                                   runs in offline mode (-scOfflineOnly, passed on by Steam): story mode needs no
                                   sign-in, and Rockstar's sign-in has been failing
  gtactl.py restore                the person's settings back (after the game closed)
  gtactl.py quit                   closes the game, then restore
  gtactl.py <step> [<step> ...]    steps run in order:
      key:<name>                   a mod hotkey (F9, F10, Q, ...)
      gamekey:<name>[,<ms>]        a key for the game itself (raw input; works in frontend menus such as the save screen)
      ctl:<id>[,<ms>[,<value>]]    a game control held (51 = context/E, 172-177 = phone/menu up down left right select back)
      hold:<id>,<ms>               the same, then waits until it was let go
      shot:<file.png>              the next frame
      wait:<seconds>
      log:<regex>[,<timeout s>]    waits for a line in the police mod's log (newer than this run's start)
      raw:<command line>           any pipe command (ping, story, window offscreen|restore, mute on|off, release,
                                   fakefocus <ms>: GetForegroundWindow reports the game for that long)
Exits 1 on the first step that fails.
"""
import json, os, re, shutil, subprocess, sys, time

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


def main(args):
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
        elif kind == "raw":
            reply = p.send(arg)
        else:
            sys.exit(f"unknown step {step}")
        if not reply.startswith("ok"):
            print(f"{step}: {reply}")
            sys.exit(1)
        if reply != "ok":
            print(reply)


if __name__ == "__main__":
    main(sys.argv[1:])
