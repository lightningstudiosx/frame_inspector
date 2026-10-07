"""Writes the same macro in every supported format (encoders follow each bot's published layout)."""
import json, struct, sys, os, msgpack
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
# frame, button, p2, down
INPUTS = [(12, 1, False, True), (40, 1, False, False), (41, 1, True, True), (77, 1, False, True),
          (90, 1, True, False), (91, 1, False, False), (300, 1, False, True), (1302, 1, False, False)]
TPS = 240.0
json.dump(INPUTS, open(f"{out}/expected.json", "w"))

def gdr1():
    return {"author": "kai", "description": "", "duration": 5.4, "gameVersion": 2.2081, "version": 1.0,
            "framerate": TPS, "seed": 0, "coins": 0, "ldm": False, "bot": {"name": "xdBot", "version": "v2.4.1"},
            "level": {"id": 128, "name": "test"},
            "inputs": [{"frame": f, "btn": b, "2p": p, "down": d} for f, b, p, d in INPUTS]}
json.dump(gdr1(), open(f"{out}/a.gdr.json", "w"))
open(f"{out}/b.gdr", "wb").write(msgpack.packb(gdr1()))

# GDR2 (maxnut/GDReplayFormat): magic, varints, big-endian float/double, delta-packed p1 then p2
def varint(v):
    v &= (1 << 64) - 1
    if v == 0: return b"\0"
    o = b""
    while v:
        byte = v & 0x7F; v >>= 7
        o += bytes([byte | (0x80 if v else 0)])
    return o
def vstr(s): s = s.encode(); return varint(len(s)) + s
def gdr2(platformer=False):
    b = b"GDR" + varint(2) + vstr("") + vstr("kai") + vstr("") + struct.pack(">f", 5.4) + varint(22081)
    b += struct.pack(">d", TPS) + varint(0) + varint(0) + varint(0) + varint(1 if platformer else 0)
    b += vstr("Eclipse") + varint(1) + varint(128) + vstr("test") + varint(0)  # no extension
    b += varint(0)  # deaths
    p1 = [i for i in INPUTS if not i[2]]; p2 = [i for i in INPUTS if i[2]]
    b += varint(len(INPUTS)) + varint(len(p1))
    for group in (p1, p2):
        p = 0
        for f, btn, _, d in group:
            delta = f - p
            packed = (delta << 3) | (btn << 1) | int(d) if platformer else (delta << 1) | int(d)
            b += varint(packed); p = f
    return b
open(f"{out}/c.gdr2", "wb").write(gdr2())
open(f"{out}/c2.gdr2", "wb").write(gdr2(True))

json.dump({"meta": {"fps": TPS}, "events": [{"frame": f, "down": d, **({"p2": True} if p else {})} for f, b, p, d in INPUTS]},
          open(f"{out}/d.mhr.json", "w"))

# zBot: f32 delta, f32 speedhack, then (i32 frame, '1'/'0' down, '1'/'0' player1)
z = struct.pack("<ff", 1 / TPS, 1.0)
for f, b, p, d in INPUTS: z += struct.pack("<iBB", f, 0x31 if d else 0x30, 0x30 if p else 0x31)
open(f"{out}/e.zbf", "wb").write(z)

# ReplayEngine 3
p1 = [i for i in INPUTS if not i[2]]; p2 = [i for i in INPUTS if i[2]]
r = struct.pack("<fIIII", TPS, 1, 0, len(p1), len(p2))
r += struct.pack("<IfffdB7x", 5, 1.0, 2.0, 0.0, 0.5, 0)  # one physics frame (32 bytes)
for group, isp1 in ((p1, 1), (p2, 0)):
    for f, b, p, d in group: r += struct.pack("<IB3xiB3x", f, int(d), b, isp1)
open(f"{out}/f.re3", "wb").write(r)

# Silicate v1
s = struct.pack("<dI", TPS, len(INPUTS))
for f, b, p, d in INPUTS: s += struct.pack("<I", (f << 4) | (8 if p else 0) | (b << 1) | int(d))
open(f"{out}/g.slc", "wb").write(s)

open(f"{out}/h.txt", "w").write(f"{TPS}\n" + "".join(f"{f} {int(d)} {b} {0 if p else 1}\n" for f, b, p, d in INPUTS))
open(f"{out}/i.xd", "w").write(f"{TPS}\n" + "".join(f"{f}|{int(d)}|{b}|{0 if p else 1}|0|1.0|2.0\n" for f, b, p, d in INPUTS))

# TASBot: one entry per frame with click codes 0 none / 1 press / 2 release
frames = {}
for f, b, p, d in INPUTS: frames.setdefault(f, [0, 0])[1 if p else 0] = 1 if d else 2
json.dump({"fps": TPS, "macro": [{"frame": f, "player_1": {"click": c[0], "x_position": 0}, "player_2": {"click": c[1]}}
                                 for f, c in sorted(frames.items())]}, open(f"{out}/j.json", "w"))
print("wrote", len(os.listdir(out)), "files")
