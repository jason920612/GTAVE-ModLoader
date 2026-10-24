"""A long run of the graphics mod played as a person plays it (research/phase0.md §59.12), for what only shows over time
and in motion: the player drives about the map on their own (the game's AI at the wheel, wandering), gets out now and
then and walks, looks about (the camera turned this way and that, the view modes changed), goes into buildings, now
and then is somewhere else on the map (a long drive in a moment), now and then shoots and blows things up (fire,
smoke, explosions about them: the game's particles); the clock runs and the weather changes. Every few
minutes some seconds are recorded as video; all along, the frame rate and the slowest frames, GPU memory and the
renderer's own timings are logged.

usage: ue5soak.py <out folder> [minutes] [--video-every <minutes>] [--seed <n>] [--hour <h>] [--weather <first weather>]
                  [--profile-over <GPU ms>]
       (the game running with test control: gtactl.py launch)
Writes <out>/soak.csv (every 15 s), <out>/events.txt, <out>/v###_<what>.mp4, and prints a summary. With --profile-over,
where the renderer's GPU time (its 10 s average) comes to that many ms, a GPU profile of a frame there (at most one in
two minutes): <out>/profile_<t>.txt, its passes of 0.3 ms or more.
"""
import csv, math, os, random, re, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gtactl  # noqa: E402

LOADER_LOG = os.path.join(gtactl.GAME, "ModLoader", "loader.log")
RENDERER_LOG = os.path.join(gtactl.GAME, "ModLoader", "ue5renderer.log")
RENDERER_CONSOLE = os.path.join(gtactl.GAME, "ModLoader", "ue5console.txt")  # (console commands for the renderer)

PLAYER_PED_ID, PLAYER_ID = 0xD80958FC74E988A6, 0x4F8644AF03D0E0D6
REQUEST_MODEL, HAS_MODEL_LOADED, CREATE_VEHICLE = 0x963D27A58DF860AC, 0x98A4EB5D89A0C952, 0xAF35D0D2583051B0
SET_PED_INTO_VEHICLE, GET_VEHICLE_PED_IS_IN, IS_PED_IN_ANY_VEHICLE = 0xF75B0D629E1C063D, 0x9A9112A0FE9A4713, 0x997ABD671D25CA0B
TASK_VEHICLE_DRIVE_WANDER, TASK_WANDER_STANDARD, TASK_LEAVE_VEHICLE = 0x480142959D337D00, 0xBB9CE077274F6A1B, 0xD3DBCE61A490BE02
CLEAR_PED_TASKS_IMMEDIATELY, SET_PED_KEEP_TASK = 0xAAA34F8A7CB32098, 0x971D38760FBC02EF
SET_ENTITY_INVINCIBLE, SET_ENTITY_COORDS, GET_ENTITY_COORDS = 0x3882114BDE571AD4, 0x06843DA7060A026B, 0x3FEF770D40960D5A
SET_ENTITY_HEADING, GET_ENTITY_SPEED, DOES_ENTITY_EXIST = 0x8E2530AA8ADA980E, 0xD5037BA82E12416F, 0x7239B21A38F536BA
SET_MAX_WANTED_LEVEL, SET_PLAYER_WANTED_LEVEL, SET_PLAYER_WANTED_LEVEL_NOW = 0xAA5F02DB48D704B9, 0x39FF19C64EF7DA5B, 0xE0A7D1E497FFCD6F
SET_EVERYONE_IGNORE_PLAYER, SET_VEHICLE_ON_GROUND_PROPERLY = 0x8EEDA153AD141BA4, 0x49733E92263139D1
SET_GAMEPLAY_CAM_RELATIVE_PITCH, SET_FOLLOW_PED_CAM_VIEW_MODE, SET_FOLLOW_VEHICLE_CAM_VIEW_MODE = 0x6D0858B8EDFD2B7D, 0x5A4F9EDF1673F704, 0xAC253D7842768F48
SET_VEHICLE_ENGINE_ON = 0x2497C4717C8B881E
ADD_EXPLOSION, START_SCRIPT_FIRE, REMOVE_SCRIPT_FIRE = 0xE3AD2BDBAEE269AC, 0x6B83617E04503888, 0x7FF548385680673F
GIVE_WEAPON_TO_PED, TASK_SHOOT_AT_COORD = 0xBF0FD6E56C964FCB, 0x46A6CC01E0826106

# Roads all over the map (x, y, z, heading): where a long drive would have taken the player.
ROADS = [
    (150, -1000, 29.4, 70), (300, 180, 103.5, 70), (-700, -200, 37.5, 120), (-1200, -1100, 7.6, 30), (-900, -2300, 13.6, 150),
    (900, -2900, 5.9, 90), (1100, -500, 62.0, 0), (-2200, -350, 13.2, 60), (1694, 3296, 41.1, 30), (1700, 4800, 42.0, 0),
    (-200, 6300, 31.5, 45), (-3100, 1100, 20.4, 0), (580, 2700, 42.0, 90), (2580, 361, 108.5, 0), (-1592, 5190, 4.0, 200),
    (2390, 4980, 45.0, 45), (-470, 1100, 323.0, 0), (1209, -3133, 5.9, 0), (-1037, -2738, 14.0, 150), (1395, 1118, 114.3, 90),
]
# Inside buildings the game has interiors of (on foot).
ROOMS = [
    (-11.0, -1436.0, 31.1), (-802.3, 175.0, 72.8), (1973.0, 3816.0, 33.4), (1274.0, -1714.0, 54.8), (7.0, 528.0, 170.6),
    (116.0, -1290.0, 28.3), (19.6, -1110.0, 29.8), (25.7, -1347.3, 29.5), (-163.0, -303.0, 39.7), (-560.0, 286.0, 82.2),
    (-1047.9, -233.0, 39.0),
]
WEATHERS = ["EXTRASUNNY", "CLEAR", "CLOUDS", "OVERCAST", "RAIN", "FOGGY", "THUNDER", "CLEARING", "SMOG"]
EXPLOSIONS = [3, 3, 7, 9, 20, 22]  # molotov, a car's, a petrol pump's, a smoke grenade, a flare
CARS = ["sultan", "buffalo", "tailgater", "baller", "felon", "oracle"]


OUT = object()  # (a native's output argument: Game.native returns what it wrote)


def joaat(text):
    h = 0
    for c in text.lower().encode():
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


class Game:
    def __init__(self, events):
        self.pipe = gtactl.Pipe(timeout=5)
        self.lock = threading.Lock()
        self.events = events

    def send(self, line):
        with self.lock:
            return self.pipe.send(line)

    def native(self, hash_, *args):
        """(int, float, (x, y, z)) of the result; with OUT arguments a fourth item: the same of each, as the native wrote it."""
        words = []
        for a in args:
            if a is OUT:
                words.append("out")
            elif isinstance(a, float):
                words.append(f"{a:.4f}")
            elif isinstance(a, str):
                words.append("s:" + a)
            else:
                words.append(str(int(a)))
        reply = self.send(f"native {hash_:#x} {' '.join(words)}".strip())
        if not reply.startswith("ok"):
            return None
        segments = reply.split(" | out ")
        parts = segments[0].split()
        result = int(parts[1]), float(parts[2]), (float(parts[3]), float(parts[4]), float(parts[5]))
        outs = []
        for seg in segments[1:]:
            o = seg.split()
            outs.append((int(o[0]), float(o[1]), (float(o[2]), float(o[3]), float(o[4]))))
        return result + (outs,) if outs else result

    def ped(self):
        r = self.native(PLAYER_PED_ID)
        return r[0] if r else 0

    def log(self, text):
        line = f"{time.strftime('%H:%M:%S')} {text}"
        print(line, flush=True)
        self.events.write(line + "\n")
        self.events.flush()


def ensure_car(g, state):
    """The player at the wheel of a car (a new one when theirs is gone)."""
    ped = g.ped()
    car = g.native(GET_VEHICLE_PED_IS_IN, ped, 1)
    car = car[0] if car else 0
    if car and g.native(DOES_ENTITY_EXIST, car)[0]:
        state["car"] = car
    if not state.get("car") or not g.native(DOES_ENTITY_EXIST, state["car"])[0]:
        model = joaat(random.choice(CARS))
        g.native(REQUEST_MODEL, model)
        for _ in range(50):
            if g.native(HAS_MODEL_LOADED, model)[0]:
                break
            time.sleep(0.1)
        x, y, z = g.native(GET_ENTITY_COORDS, ped, 1)[2]
        state["car"] = g.native(CREATE_VEHICLE, model, x + 3.0, y, z + 0.5, 0.0, 0, 0, 0)[0]
        g.log(f"new car {state['car']}")
    g.native(SET_ENTITY_INVINCIBLE, state["car"], 1, 0)
    if not g.native(IS_PED_IN_ANY_VEHICLE, ped, 0)[0]:
        g.native(CLEAR_PED_TASKS_IMMEDIATELY, ped)
        g.native(SET_PED_INTO_VEHICLE, ped, state["car"], -1)
    g.native(SET_VEHICLE_ENGINE_ON, state["car"], 1, 1, 0)
    return state["car"]


def calm(g):
    """No police, nobody minds the player, the player cannot die."""
    ped, player = g.ped(), g.native(PLAYER_ID)[0]
    g.native(SET_MAX_WANTED_LEVEL, 0)
    g.native(SET_PLAYER_WANTED_LEVEL, player, 0, 0)
    g.native(SET_PLAYER_WANTED_LEVEL_NOW, player, 0)
    g.native(SET_EVERYONE_IGNORE_PLAYER, player, 1)
    g.native(SET_ENTITY_INVINCIBLE, ped, 1, 0)


def look_about(g, seconds, stop):
    """The camera turned as a person turns it: now still, now a quick look, now a slow pan, up and down a little."""
    end = time.time() + seconds
    while time.time() < end and not stop.is_set():
        kind = random.random()
        if kind < 0.35:
            time.sleep(random.uniform(1.0, 4.0))
            continue
        rate = random.choice([-1, 1]) * (random.uniform(90, 200) if kind < 0.6 else random.uniform(15, 60))
        dur = random.uniform(0.4, 1.2) if abs(rate) > 80 else random.uniform(2.0, 5.0)
        g.send(f"spin {rate:.0f} {dur:.2f}")
        if random.random() < 0.3:
            g.native(SET_GAMEPLAY_CAM_RELATIVE_PITCH, random.uniform(-25.0, 15.0), 1.0)
        time.sleep(dur + random.uniform(0.2, 1.5))


def gpu_memory():
    """GPU memory by process and the adapter's (MB); the 3D engine's busy share by process (%, over 2 s)."""
    ps = ("Get-Counter -Counter '\\GPU Process Memory(*)\\Dedicated Usage','\\GPU Adapter Memory(*)\\Dedicated Usage' "
          "-ErrorAction SilentlyContinue | % { $_.CounterSamples } | % { $_.Path + '=' + $_.CookedValue }; "
          "Get-Counter -Counter '\\GPU Engine(*engtype_3D)\\Utilization Percentage' -SampleInterval 1 -MaxSamples 2 "
          "-ErrorAction SilentlyContinue | % { $_.CounterSamples } | % { 'busy ' + $_.InstanceName + '=' + $_.CookedValue }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    by_pid, adapter, busy = {}, 0.0, {}
    for line in out.splitlines():
        m = re.search(r"pid_(\d+)_.*=(\d+(\.\d+)?)", line)
        if line.startswith("busy "):
            if m:
                busy[int(m.group(1))] = busy.get(int(m.group(1)), 0) + float(m.group(2)) / 2
        elif m:
            by_pid[int(m.group(1))] = by_pid.get(int(m.group(1)), 0) + float(m.group(2)) / 2**20
        elif "gpu adapter memory" in line.lower():
            adapter = max(adapter, float(line.rsplit("=", 1)[1]) / 2**20)
    return by_pid, adapter, busy


def pids():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    found = {}
    for row in csv.reader(out.splitlines()):
        if row and row[0] in ("GTA5_Enhanced.exe", "UnrealEditor.exe"):
            found[row[0]] = int(row[1])
    return found.get("GTA5_Enhanced.exe"), found.get("UnrealEditor.exe")


def tail(path, since):
    try:
        with open(path, "rb") as f:
            f.seek(since)
            data = f.read()
        return data.decode("utf-8", "replace"), since + len(data)
    except OSError:
        return "", since


def profile_tree(text, least=0.3, deepest=8):
    """The passes of the last GPU profile (ProfileGPU) in some of the renderer's log, of at least `least` ms, to a depth."""
    if "GPU Profile for Frame" not in text:
        return []
    row = re.compile(r"┃(.*?)┃(.*?)┃\s{4}(\s*)(\S.*?)\s*┃")
    out = []
    for line in text.split("GPU Profile for Frame")[-1].splitlines():
        m = row.search(line)
        if not m:
            continue
        try:
            ms = float(m.group(2).split("│")[-1].replace("ms", "").strip())
        except ValueError:
            continue
        depth = len(m.group(3)) // 2
        if ms >= least and depth <= deepest:
            out.append(f"{ms:7.2f}  {'  ' * depth}{m.group(4)[:100]}")
    return out


def monitor(out_dir, stop, state, profile_over=None):
    """Every 15 s: frame rate (the loader's 5 s windows), the renderer's timings, GPU memory; a GPU profile where the
    renderer's GPU time comes to `profile_over` ms."""
    loader_at = os.path.getsize(LOADER_LOG) if os.path.exists(LOADER_LOG) else 0
    renderer_at = os.path.getsize(RENDERER_LOG) if os.path.exists(RENDERER_LOG) else 0
    profiled, profiling = 0.0, ""  # (when the last was asked for; what it is of, until it came)
    with open(os.path.join(out_dir, "soak.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t", "doing", "fps", "slowest_ms", "renderer_fps", "game_mb", "renderer_mb", "adapter_mb", "world_ms", "render_ms", "gpu_ms",
                    "objects", "drawn", "textures_mb", "meshes_mb", "evicted", "game_busy", "renderer_busy", "x", "y", "z", "hours", "weather",
                    "interior"])
        start = time.time()
        while not stop.wait(15):
            text, loader_at = tail(LOADER_LOG, loader_at)
            fps = [(float(a), float(b)) for a, b in re.findall(r"fps: ([\d.]+), slowest frame ([\d.]+) ms", text)]
            pace = [float(a) for a in re.findall(r"renderer ([\d.]+) frames/s", text)]
            rtext, renderer_at = tail(RENDERER_LOG, renderer_at)
            times = re.findall(r"time: world ([\d.]+) ms a frame \(slowest [\d.]+\), render thread ([\d.]+) ms, GPU ([\d.]+) ms; \d+ components pooled, (\d+) objects", rtext)
            drawn = re.findall(r"drawn (\d+) .*?textures \d+ \(([\d.]+) MB, \d+ failed\), meshes ([\d.]+) MB, (\d+) evicted", rtext)
            game, renderer = pids()
            by_pid, adapter, busy = gpu_memory()
            camera = re.findall(r"game camera ([-\d.]+) ([-\d.]+) ([-\d.]+) .*?hours ([\d.]+); weather \w+ -> (\w+) [\d.]+; interior (\d+)", rtext)
            row = [round(time.time() - start), state.get("doing", ""),
                   round(sum(a for a, _ in fps) / len(fps), 1) if fps else "", max((b for _, b in fps), default=""),
                   pace[-1] if pace else "", round(by_pid.get(game, 0)), round(by_pid.get(renderer, 0)), round(adapter)]
            row += list(times[-1]) if times else ["", "", "", ""]
            row += list(drawn[-1]) if drawn else ["", "", "", ""]
            row += [round(busy.get(game, 0)), round(busy.get(renderer, 0))]
            row += list(camera[-1]) if camera else ["", "", "", "", "", ""]
            w.writerow(row)
            f.flush()
            if profile_over and profiling and "GPU Profile for Frame" in rtext:
                with open(os.path.join(out_dir, f"profile_{row[0]}.txt"), "w", encoding="utf-8") as p:
                    p.write(profiling + "\n" + "\n".join(profile_tree(rtext)) + "\n")
                profiling = ""
            elif profile_over and times and float(times[-1][2]) >= profile_over and time.time() - profiled > 120:
                with open(RENDERER_CONSOLE, "w") as c:
                    c.write("r.ProfileGPU.ShowUI 0\nProfileGPU\n")
                profiled = time.time()
                profiling = f"GPU {times[-1][2]} ms, {state.get('doing', '')}" + (
                    f" at {' '.join(camera[-1][:3])}, {camera[-1][3]} h, weather {camera[-1][4]}, interior {camera[-1][5]}" if camera else "")


def main(args):
    out_dir = os.path.abspath(args[0])
    minutes = float(args[1]) if len(args) > 1 and not args[1].startswith("--") else 30
    video_every = float(args[args.index("--video-every") + 1]) if "--video-every" in args else 3
    random.seed(int(args[args.index("--seed") + 1]) if "--seed" in args else 1)
    hour = int(args[args.index("--hour") + 1]) if "--hour" in args else 7
    first_weather = args[args.index("--weather") + 1] if "--weather" in args else None
    profile_over = float(args[args.index("--profile-over") + 1]) if "--profile-over" in args else None
    os.makedirs(out_dir, exist_ok=True)
    events = open(os.path.join(out_dir, "events.txt"), "a", encoding="utf-8")
    g = Game(events)
    stop = threading.Event()
    state = {}
    threading.Thread(target=monitor, args=(out_dir, stop, state, profile_over), daemon=True).start()
    calm(g)
    g.send(f"time {hour} 0 run")
    g.send(f"weather {first_weather or random.choice(WEATHERS[:3])}")
    start = time.time()
    next_video = start + 60
    next_weather = start + 8 * 60
    videos = 0
    try:
        while time.time() - start < minutes * 60:
            calm(g)
            if time.time() >= next_weather:
                weather = random.choice(WEATHERS)
                g.send(f"weather {weather}")
                g.log(f"weather {weather}")
                next_weather = time.time() + random.uniform(6, 10) * 60
            roll = random.random()
            if roll < 0.55:
                what, seconds = "drive", random.uniform(60, 150)
                car = ensure_car(g, state)
                if random.random() < 0.35:
                    x, y, z, h = random.choice(ROADS)
                    g.native(SET_ENTITY_COORDS, car, float(x), float(y), float(z), 0, 0, 0, 0)
                    g.native(SET_ENTITY_HEADING, car, float(h))
                    g.native(SET_VEHICLE_ON_GROUND_PROPERLY, car, 5.0)
                    g.log(f"far away: {x} {y}")
                ped = g.ped()
                g.native(TASK_VEHICLE_DRIVE_WANDER, ped, car, random.uniform(18.0, 35.0), 786603 if random.random() < 0.5 else 1074528293)
                g.native(SET_PED_KEEP_TASK, ped, 1)
                g.native(SET_FOLLOW_VEHICLE_CAM_VIEW_MODE, random.choice([0, 1, 2, 1, 4]))
            elif roll < 0.8:
                what, seconds = "walk", random.uniform(30, 70)
                ped = g.ped()
                if g.native(IS_PED_IN_ANY_VEHICLE, ped, 0)[0]:
                    g.native(TASK_LEAVE_VEHICLE, ped, state.get("car", 0), 0)
                    time.sleep(3)
                g.native(TASK_WANDER_STANDARD, ped, 10.0, 10)
                g.native(SET_FOLLOW_PED_CAM_VIEW_MODE, random.choice([1, 1, 2, 4]))
            elif roll < 0.9:
                what, seconds = "mayhem", random.uniform(25, 45)
                # (on foot: shooting about, fires and explosions some metres off; nobody minds, nothing hurts the player)
                ped = g.ped()
                if g.native(IS_PED_IN_ANY_VEHICLE, ped, 0)[0]:
                    g.native(TASK_LEAVE_VEHICLE, ped, state.get("car", 0), 0)
                    time.sleep(3)
                x, y, z = g.native(GET_ENTITY_COORDS, ped, 1)[2]
                for _ in range(random.randint(2, 4)):
                    a = random.uniform(0, 6.283)
                    d = random.uniform(10, 25)
                    g.native(ADD_EXPLOSION, x + d * math.cos(a), y + d * math.sin(a), z, random.choice(EXPLOSIONS), 1.0, 1, 0, 0.2, 1)
                    time.sleep(random.uniform(0.5, 2))
                fire = g.native(START_SCRIPT_FIRE, x + 8.0, y + 6.0, z - 0.8, 10, 0)
                state.setdefault("fires", []).append(fire[0] if fire else 0)
                for old in state["fires"][:-3]:
                    g.native(REMOVE_SCRIPT_FIRE, old)
                state["fires"] = state["fires"][-3:]
                g.native(GIVE_WEAPON_TO_PED, ped, joaat("WEAPON_ASSAULTRIFLE"), 500, 0, 1)
                a = random.uniform(0, 6.283)
                g.native(TASK_SHOOT_AT_COORD, ped, x + 20 * math.cos(a), y + 20 * math.sin(a), z, int(seconds * 500), joaat("FIRING_PATTERN_FULL_AUTO"))
                g.native(SET_FOLLOW_PED_CAM_VIEW_MODE, random.choice([1, 1, 2, 4]))
            else:
                what, seconds = "inside", random.uniform(30, 60)
                ped = g.ped()
                x, y, z = random.choice(ROOMS)
                g.native(CLEAR_PED_TASKS_IMMEDIATELY, ped)
                g.send(f"tp {x} {y} {z}")
                time.sleep(2)
                g.native(TASK_WANDER_STANDARD, ped, 10.0, 10)
                g.native(SET_FOLLOW_PED_CAM_VIEW_MODE, random.choice([1, 4]))
            state["doing"] = what
            g.log(f"{what} for {seconds:.0f} s")
            end = time.time() + seconds
            while time.time() < end:
                if time.time() >= next_video:
                    videos += 1
                    name = os.path.join(out_dir, f"v{videos:03}_{what}.mp4")
                    g.send(f"record 12 30 2 {os.path.splitext(name)[0]}.grec")
                    g.log(f"video {name}")
                    next_video = time.time() + video_every * 60

                    def convert(name=name):
                        time.sleep(13)
                        while g.send("record status") == "ok recording":
                            time.sleep(0.5)
                        g.log(f"{os.path.basename(name)}: {gtactl.convert(os.path.splitext(name)[0] + '.grec', name, 30)}")
                        os.remove(os.path.splitext(name)[0] + ".grec")
                    threading.Thread(target=convert, daemon=True).start()
                look_about(g, min(20.0, end - time.time()), stop)
    finally:
        stop.set()
        time.sleep(20)
        g.native(SET_MAX_WANTED_LEVEL, 5)
        g.log("done")
    print(summary(os.path.join(out_dir, "soak.csv")))


def summary(path):
    rows = list(csv.DictReader(open(path)))
    if not rows:
        return "no samples"
    out = []
    for i in range(0, len(rows), 8):
        part = [r for r in rows[i:i + 8] if r["fps"]]
        if not part:
            continue
        fps = sum(float(r["fps"]) for r in part) / len(part)
        slow = max(float(r["slowest_ms"]) for r in part)
        out.append(f"{int(part[0]['t']) // 60:>3} min: {fps:5.1f} fps, slowest {slow:5.1f} ms, GPU memory {part[-1]['adapter_mb']} MB "
                   f"(game {part[-1]['game_mb']}, renderer {part[-1]['renderer_mb']}), render thread {part[-1]['render_ms']} ms, GPU {part[-1]['gpu_ms']} ms"
                   + (f", busy {part[-1]['game_busy']}+{part[-1]['renderer_busy']}%, {part[-1]['weather']} {part[-1]['hours']} h" if part[-1].get("weather") else ""))
    # (the slowest places: where to go back to)
    slow = sorted((r for r in rows if r["fps"] and r.get("x")), key=lambda r: float(r["fps"]))[:5]
    for r in slow:
        out.append(f"  slow: {r['fps']} fps {r['doing']} at {r['x']} {r['y']} {r['z']}, {r['weather']} {r['hours']} h, interior {r['interior']}, "
                   f"GPU {r['gpu_ms']} ms, render thread {r['render_ms']} ms, busy {r['game_busy']}+{r['renderer_busy']}%")
    return "\n".join(out)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "summary":
        print(summary(sys.argv[2]))
    else:
        main(sys.argv[1:])
