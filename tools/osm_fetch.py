"""OpenStreetMap data for the world builder (tools/worldgen osm): roads, buildings, coastline, water, land use, trees and
the like in a latitude/longitude box, fetched from the Overpass API in tiles (one category at a time, waiting between
requests; a tile no server will answer for is split in four), saved as Overpass JSON. The data is OpenStreetMap's (ODbL:
(c) OpenStreetMap contributors); it stays on this computer.

usage: osm_fetch.py <out dir> <south> <west> <north> <east> [tiles per side (default 3)]
  e.g. osm_fetch.py D:\\GTAV-UE5\\osm\\miami 25.745 -80.232 25.866 -80.115
Files already there are not fetched again.
"""
import json, os, sys, time, urllib.parse, urllib.request

# The main instance and public mirrors: a busy one (502/504, rate limits) hands over to the next.
ENDPOINTS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
    "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
]
AGENT = "GTAVE-WorldBuilder/0.1 (personal map research; tools/osm_fetch.py)"
# Overpass filters per category: ways and relations with these tags, their nodes with them ("out body; >; out skel qt").
CATEGORIES = {  # (coast and land first: what the island's shape needs)
    "water": 'way["natural"="coastline"]; way["natural"="water"]; relation["natural"="water"]; way["waterway"]; relation["waterway"];'
             ' way["natural"="bay"]; relation["natural"="bay"];',
    "land": 'way["landuse"]; relation["landuse"]; way["leisure"]; relation["leisure"]; way["natural"~"beach|sand|wood|scrub|grassland|wetland"];'
            ' relation["natural"~"beach|sand|wood|scrub|grassland|wetland"]; way["amenity"="parking"]; way["aeroway"];',
    "roads": 'way["highway"];',
    "points": 'node["natural"="tree"]; node["highway"~"traffic_signals|stop|crossing"]; node["amenity"]; node["leisure"];',
    "buildings": 'way["building"]; relation["building"]; way["building:part"];',
    "rail": 'way["railway"];',
}


def fetch(query, rounds=2):
    """The answer, or None when no server gave one in `rounds` rounds."""
    data = urllib.parse.urlencode({"data": query}).encode()
    for attempt in range(rounds * len(ENDPOINTS)):
        endpoint = ENDPOINTS[attempt % len(ENDPOINTS)]
        try:
            req = urllib.request.Request(endpoint, data=data, headers={"User-Agent": AGENT})
            with urllib.request.urlopen(req, timeout=330) as r:
                body = r.read()
            json.loads(body)  # (a whole answer: valid JSON)
            return body
        except Exception as e:  # rate limited, busy or too big: the next one
            print(f"  {endpoint}: {str(e)[:120]}", flush=True)
            time.sleep(10 * (attempt // len(ENDPOINTS) + 1))
    return None


def tile(out, name, filt, s, w, n, e, label, depth=0):
    path = os.path.join(out, f"{name}_{label}.json")
    if os.path.exists(path):
        return
    query = f"[out:json][timeout:300][maxsize:536870912][bbox:{s:.6f},{w:.6f},{n:.6f},{e:.6f}];({filt});out body;>;out skel qt;"
    print(f"{name} {label} ...", flush=True)
    body = fetch(query)
    if body is None:
        if depth >= 3:
            raise RuntimeError(f"{name} {label}: no answer even for a tile this small")
        ms, mw = (s + n) / 2, (w + e) / 2
        for k, (a, b, c, d) in enumerate(((s, w, ms, mw), (s, mw, ms, e), (ms, w, n, mw), (ms, mw, n, e))):
            tile(out, name, filt, a, b, c, d, f"{label}{k}", depth + 1)
        return
    with open(path + ".part", "wb") as f:
        f.write(body)
    os.replace(path + ".part", path)
    print(f"  {len(body) // 1024} KB", flush=True)
    time.sleep(5)


def main():
    if len(sys.argv) < 6:
        print(__doc__)
        return 1
    out = sys.argv[1]
    south, west, north, east = (float(v) for v in sys.argv[2:6])
    n = int(sys.argv[6]) if len(sys.argv) > 6 else 3
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "box.json"), "w") as f:
        json.dump({"south": south, "west": west, "north": north, "east": east, "tiles": n,
                   "source": "OpenStreetMap (ODbL), (c) OpenStreetMap contributors"}, f)
    for name, filt in CATEGORIES.items():
        for j in range(n):
            for i in range(n):
                s = south + (north - south) * j / n
                nn = south + (north - south) * (j + 1) / n
                w = west + (east - west) * i / n
                e = west + (east - west) * (i + 1) / n
                tile(out, name, filt, s, w, nn, e, f"{j}_{i}")
    print("done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
