#!/usr/bin/env python3
"""Regenerate the sandbox's original placeholder art and glTF dioramas.

Uses only Python's standard library. Geometry is in metres/tiles, Y up;
characters use 32 pixels per tile. No copyrighted game assets are included.
"""
import json
import math
from pathlib import Path
import random
import struct
import wave
import zlib

ROOT = Path(__file__).resolve().parents[1] / "samples" / "sandbox"
RNG = random.Random(6)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n")


def png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    data = b"".join(b"\0" + bytes(pixels[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(data, 9)) + chunk(b"IEND", b""))


def sprite(name, coat, hair, kind="person"):
    width, height = 128, 192
    pixels = bytearray(width * height * 4)
    def rect(x, y, w, h, color):
        for yy in range(max(0, y), min(height, y + h)):
            for xx in range(max(0, x), min(width, x + w)):
                i = (yy * width + xx) * 4
                pixels[i:i + 4] = bytes((*color[:3], color[3] if len(color) > 3 else 255))
    outline = (31, 29, 44)
    shade = tuple(int(v * 0.65) for v in coat)
    for direction in range(4):
        for frame in range(4):
            ox, oy = frame * 32, direction * 48
            bob = 1 if frame in (1, 3) else 0
            if kind == "slime":
                for y in range(18, 44):
                    for x in range(3, 29):
                        if ((x - 16) / (13 + bob)) ** 2 + ((y - 32) / (12 - bob)) ** 2 <= 1:
                            rect(ox + x, oy + y, 1, 1, coat if y < 35 else shade)
                rect(ox + 10, oy + 27, 3, 4, outline); rect(ox + 20, oy + 27, 3, 4, outline)
                rect(ox + 9, oy + 23, 4, 2, (200, 245, 221))
                continue
            if kind == "chest":
                rect(ox + 3, oy + 23, 26, 20, outline)
                rect(ox + 5, oy + 25, 22, 16, (137, 71, 40))
                rect(ox + 4, oy + 27, 24, 3, (238, 189, 94))
                rect(ox + 5, oy + 39, 22, 2, (238, 189, 94))
                rect(ox + 14, oy + 28, 4, 7, (255, 220, 122))
                continue
            if kind == "golem":
                rect(ox + 5, oy + 9, 22, 32, outline)
                rect(ox + 8, oy + 11 + bob, 16, 27, coat)
                rect(ox + 1, oy + 22 + bob, 7, 17, shade); rect(ox + 24, oy + 22 + bob, 7, 17, shade)
                rect(ox + 7, oy + 37, 8, 7, shade); rect(ox + 18, oy + 37, 8, 7, shade)
                rect(ox + 11, oy + 17 + bob, 3, 3, (97, 245, 255)); rect(ox + 20, oy + 17 + bob, 3, 3, (97, 245, 255))
                rect(ox + 15, oy + 27 + bob, 5, 6, (140, 255, 231))
                continue
            # Hand-placed clusters preserve a crisp silhouette at 32 PPU.
            rect(ox + 8, oy + 19 + bob, 16, 23, outline)
            rect(ox + 10, oy + 21 + bob, 12, 18, coat)
            rect(ox + 9, oy + 29 + bob, 14, 3, (235, 190, 105))
            rect(ox + 6, oy + 22 + bob, 5, 14, shade); rect(ox + 22, oy + 22 + bob, 5, 14, shade)
            rect(ox + 7, oy + 34 + bob, 4, 4, (240, 188, 144)); rect(ox + 22, oy + 34 + bob, 4, 4, (240, 188, 144))
            rect(ox + 8, oy + 38, 7, 7 - (frame % 2), outline); rect(ox + 18, oy + 38, 7, 6 + (frame % 2), outline)
            rect(ox + 8, oy + 4 + bob, 16, 17, outline)
            rect(ox + 10, oy + 7 + bob, 12, 12, (247, 201, 155))
            rect(ox + 9, oy + 3 + bob, 14, 6, hair); rect(ox + 7, oy + 7 + bob, 4, 8, hair)
            if direction == 2:
                rect(ox + 9, oy + 7 + bob, 14, 12, hair)
            elif direction == 1:
                rect(ox + 10, oy + 12 + bob, 2, 3, outline); rect(ox + 19, oy + 7 + bob, 5, 10, hair)
            elif direction == 3:
                rect(ox + 21, oy + 12 + bob, 2, 3, outline); rect(ox + 8, oy + 7 + bob, 5, 10, hair)
            else:
                rect(ox + 11, oy + 12 + bob, 2, 3, outline); rect(ox + 19, oy + 12 + bob, 2, 3, outline)
                rect(ox + 14, oy + 17 + bob, 4, 1, (160, 88, 76))
    png(ROOT / "sprites" / f"{name}.png", width, height, pixels)
    def frames(walk):
        return [[{"rect": [frame * 32, direction * 48, 32, 48], "duration": 0.16,
                  **({"event": "footstep"} if walk and frame in (1, 3) else {})}
                 for frame in (range(4) if walk else [0])]
                for direction in range(4)]
    write_json(ROOT / "sprites" / f"{name}.anim.json", {"atlas": f"{name}.png", "pixels_per_unit": 32,
        "pivot": [0.5, 1], "clips": {"idle": {"directions": frames(False)}, "walk": {"directions": frames(True)},
                                     "attack": {"loop": False, "directions": frames(True)}}})


class Diorama:
    def __init__(self):
        self.vertices = [[], []]
        self.indices = [[], []]

    def triangle(self, points, color, transparent=False):
        channel = int(transparent)
        a, b, c = points
        u, v = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        length = math.sqrt(sum(x * x for x in n)) or 1
        n = [x / length for x in n]
        for point, uv in zip(points, [(0, 0), (1, 0), (1, 1)]):
            self.indices[channel].append(len(self.vertices[channel]))
            # glTF vertex colors are linear; the palette above is authored sRGB.
            linear = tuple(c ** 2.2 for c in color[:3]) + (color[3],)
            self.vertices[channel].append((*point, *n, *uv, *linear))

    def quad(self, points, color, transparent=False):
        self.triangle([points[0], points[1], points[2]], color, transparent)
        self.triangle([points[0], points[2], points[3]], color, transparent)

    def box(self, x, y, z, w, h, d, color):
        x0, x1, z0, z1 = x - w / 2, x + w / 2, z - d / 2, z + d / 2
        self.quad([(x0,y+h,z0),(x0,y+h,z1),(x1,y+h,z1),(x1,y+h,z0)], color)
        for points, shade in [([(x0,y,z1),(x1,y,z1),(x1,y+h,z1),(x0,y+h,z1)],.88),
                              ([(x1,y,z0),(x0,y,z0),(x0,y+h,z0),(x1,y+h,z0)],.68),
                              ([(x0,y,z0),(x0,y,z1),(x0,y+h,z1),(x0,y+h,z0)],.78),
                              ([(x1,y,z1),(x1,y,z0),(x1,y+h,z0),(x1,y+h,z1)],.94)]:
            self.quad(points, tuple(c * shade for c in color[:3]) + (color[3],))

    def roof(self, x, y, z, w, h, d):
        c = (.37,.18,.18,1)
        self.quad([(x-w/2,y,z-d/2),(x,y+h,z-d/2),(x,y+h,z+d/2),(x-w/2,y,z+d/2)], c)
        self.quad([(x,y+h,z-d/2),(x+w/2,y,z-d/2),(x+w/2,y,z+d/2),(x,y+h,z+d/2)], (.53,.25,.21,1))
        for zz in (z-d/2,z+d/2): self.triangle([(x-w/2,y,zz),(x+w/2,y,zz),(x,y+h,zz)],(.62,.44,.31,1))

    def write(self, path):
        binary = bytearray()
        views, accessors, primitives = [], [], []
        for channel in range(2):
            if not self.vertices[channel]: continue
            verts, indices = self.vertices[channel], self.indices[channel]
            offset = len(binary)
            for vertex in verts: binary.extend(struct.pack("<12f", *vertex))
            view_index = len(views)
            views.append({"buffer":0,"byteOffset":offset,"byteLength":len(verts)*48,"byteStride":48,"target":34962})
            attrs = {}
            for name, byte_offset, kind, count in [("POSITION",0,"VEC3",3),("NORMAL",12,"VEC3",3),("TEXCOORD_0",24,"VEC2",2),("COLOR_0",32,"VEC4",4)]:
                accessor = {"bufferView":view_index,"byteOffset":byte_offset,"componentType":5126,"count":len(verts),"type":kind}
                if name == "POSITION":
                    accessor["min"] = [min(v[i] for v in verts) for i in range(count)]
                    accessor["max"] = [max(v[i] for v in verts) for i in range(count)]
                attrs[name] = len(accessors); accessors.append(accessor)
            offset = len(binary)
            binary.extend(struct.pack(f"<{len(indices)}I", *indices))
            views.append({"buffer":0,"byteOffset":offset,"byteLength":len(indices)*4,"target":34963})
            index = len(accessors); accessors.append({"bufferView":len(views)-1,"componentType":5125,"count":len(indices),"type":"SCALAR"})
            primitives.append({"attributes":attrs,"indices":index,"material":channel})
        doc = {"asset":{"version":"2.0","generator":"f-engine sandbox generator"}, "scene":0,"scenes":[{"nodes":[0,1]}],
               "nodes":[{"mesh":0},{"name":"Blender entry","translation":[8.5,0,11.5],"extras":{"fengine":{"spawn":"blender_entry"}}}],"meshes":[{"primitives":primitives}],"buffers":[{"byteLength":len(binary)}],
               "bufferViews":views,"accessors":accessors,"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1]}},
               {"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1]},"alphaMode":"BLEND","doubleSided":True}]}
        metadata = json.dumps(doc, separators=(",", ":")).encode()
        metadata += b" " * (-len(metadata) % 4)
        binary += b"\0" * (-len(binary) % 4)
        total = 12 + 8 + len(metadata) + 8 + len(binary)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(metadata), 0x4E4F534A) +
                         metadata + struct.pack("<II", len(binary), 0x004E4942) + binary)


def make_map(name, cave=False, battle=False):
    scene = Diorama()
    cells = []
    for z in range(14):
        for x in range(16):
            border = x in (0,15) or z in (0,13)
            path = 6 <= x <= 9 or 6 <= z <= 8
            tint = RNG.uniform(.92,1.06)
            color = (.22,.26,.32,1) if cave else (.52,.43,.30,1) if path else (.22,.34,.24,1)
            scene.quad([(x,0,z),(x,0,z+1),(x+1,0,z+1),(x+1,0,z)], tuple(v*tint for v in color[:3])+(1,))
            cells.append({"walkable":not border,"region":1 if cave and not battle else 0})
            if border: scene.box(x+.5,0,z+.5,1,RNG.uniform(.8,2.4) if cave else .4,1,(.24,.27,.31,1) if cave else (.38,.38,.32,1))
    def block(x0,z0,x1,z1):
        for z in range(z0,z1):
            for x in range(x0,x1): cells[z*16+x]["walkable"] = False
    if cave:
        for x,z in ([(2,2),(13,2)] if battle else [(3,3),(12,3),(3,9),(12,9)]):
            scene.box(x+.5,0,z+.5,1.6,2.3,1.6,(.24,.28,.35,1)); block(x,z,x+1,z+1)
            scene.box(x+.5,2.3,z+.5,.35,.7,.35,(.2,.8,1.2,1))
        lights = [{"position":[3.5,2.7,3.5],"color":[.25,.8,1],"radius":8,"intensity":3},
                  {"position":[12.5,2.7,9.5],"color":[.6,.35,1],"radius":8,"intensity":3}]
    elif battle:
        for x,z in [(2,3),(14,3)]:
            scene.box(x,0,z,.3,2,.3,(.32,.22,.15,1))
            scene.box(x,1.6,z,2.1,1.8,1.9,(.18,.32,.22,1))
        lights = [{"position":[4,2,5],"color":[1,.7,.35],"radius":8,"intensity":2}]
    else:
        for x,z,w,d in [(3.5,3.5,4,3),(12.5,3.5,4,3)]:
            scene.box(x,0,z,w,2.1,d,(.72,.61,.43,1)); scene.roof(x,2.1,z,w+.4,1.4,d+.4)
            scene.box(x,.1,z+d/2+.015,.85,1.4,.04,(.21,.16,.14,1))
            for xx in (x-1.2,x+1.2): scene.box(xx,1,z+d/2+.04,.65,.65,.05,(1.2,.77,.25,1))
            block(int(x-w/2),int(z-d/2),int(x+w/2)+1,int(z+d/2)+1)
        for x,z in [(2,7),(14,6),(2,11),(13,11)]:
            if z == 11 and x == 13: continue
            scene.box(x,.0,z,.25,1.6,.25,(.32,.22,.15,1))
            scene.box(x,1.2,z,1.8,1.5,1.6,(.18,.32,.22,1)); block(x-1,z-1,x+1,z+1)
        for x,z in [(6,8),(10,6)]:
            scene.box(x,0,z,.18,1.8,.18,(.22,.20,.20,1)); scene.box(x,1.8,z,.35,.45,.35,(2,1.4,.5,1))
        lights = [{"position":[6,2,8],"color":[1,.6,.24],"radius":5,"intensity":3},
                  {"position":[10,2,6],"color":[1,.6,.24],"radius":5,"intensity":3}]
        for z in range(10,13):
            for x in range(11,15):
                cells[z*16+x]["walkable"] = False
                cells[z*16+x]["flags"] = 1
        scene.quad([(11,.04,10),(11,.04,13),(15,.04,13),(15,.04,10)],(.15,.46,.61,.72),True)
    folder = ROOT / "maps" / name
    scene.write(folder / "geometry.glb")
    result = {"id":name,"name":"Crystal Grotto" if cave else "Amber Vale","geometry":"geometry.glb", "nav":{"width":16,"depth":14,"cells":cells},
              "spawns":{"entry":[8.5,0,11.5],"return":[8.5,0,2.5]},"camera":{"pitch":42,"distance":19,"fov":30,"min":[5,4],"max":[11,10]},
              "lights":lights,"ambient":[.18,.23,.35] if cave else [.4,.4,.38],"sun_color":[.35,.48,.7] if cave else [1,.83,.61],
              "sun_direction":[-.6,-1,-.4],"fog_density":.015,"fog_color":[.10,.14,.22],
              "bgm":"audio/grotto.wav" if cave else "audio/vale.wav", "post":{"bloom_strength":.4,"focus":.53,"focus_range":.23}}
    if not battle:
        result["encounters"] = {"1":"grotto"} if cave else {}
    return result


def audio():
    rate = 22050
    for name, notes in [("vale",[261.63,329.63,392,329.63,293.66,349.23,440,349.23]),
                        ("grotto",[220,261.63,329.63,261.63,196,246.94,293.66,246.94]),
                        ("battle",[220,220,329.63,293.66,261.63,293.66,329.63,440])]:
        samples = []
        for note in notes:
            for i in range(rate // 2):
                t = i / rate; envelope = min(1,t*25)*math.exp(-t*3)
                value = math.sin(t*note*math.tau)*.16 + math.sin(t*note/2*math.tau)*.1 + math.sin(t*note*2*math.tau)*.03
                samples.append(int(value*envelope*16000))
        folder = ROOT / "audio"; folder.mkdir(exist_ok=True)
        with wave.open(str(folder/f"{name}.wav"),"wb") as output:
            output.setnchannels(1); output.setsampwidth(2); output.setframerate(rate)
            output.writeframes(struct.pack(f"<{len(samples)}h",*samples))
    noise = random.Random(6)
    for name,duration,frequency in [("step",.055,110),("hit",.12,180),("chime",.25,880)]:
        samples=[]
        for i in range(int(rate*duration)):
            t=i/rate;envelope=(1-t/duration)**2
            value=(noise.uniform(-1,1)*.25+math.sin(t*frequency*math.tau)*.35)*envelope
            samples.append(int(value*16000))
        with wave.open(str(ROOT/"audio"/f"{name}.wav"),"wb") as output:
            output.setnchannels(1);output.setsampwidth(2);output.setframerate(rate)
            output.writeframes(struct.pack(f"<{len(samples)}h",*samples))
    write_json(ROOT/"audio/events.json",{"footstep":"audio/step.wav"})


def ui_and_fx():
    pixels=bytearray(24*24*4)
    for y in range(24):
        for x in range(24):
            distance=min(x,y,23-x,23-y)
            color=(177,193,225,255) if distance==0 else (50,72,112,255) if distance in (1,2) else (18,30,56,247)
            pixels[(y*24+x)*4:(y*24+x)*4+4]=bytes(color)
    png(ROOT/"ui/skin.png",24,24,pixels)
    write_json(ROOT/"ui/layout.json",{"skin":"ui/skin.png","skin_border":4})
    pixels=bytearray(16*16*4)
    for y in range(16):
        for x in range(16):
            radius=math.sqrt((x-7.5)**2+(y-7.5)**2)/7.5
            alpha=int(max(0,1-radius)**1.5*255)
            pixels[(y*16+x)*4:(y*16+x)*4+4]=bytes((255,255,255,alpha))
    png(ROOT/"sprites/spark.png",16,16,pixels)
    write_json(ROOT/"sprites/spark.anim.json",{"atlas":"spark.png","pivot":[.5,.5],"pixels_per_unit":32,
        "clips":{"idle":{"directions":[[{"rect":[0,0,16,16],"duration":1}]]}}})
    for name,tint in [("amber",(1.035,1,.96)),("grotto",(.98,1,1.025))]:
        lines=['TITLE "f-engine '+name+'"','LUT_3D_SIZE 16','DOMAIN_MIN 0 0 0','DOMAIN_MAX 1 1 1']
        for b in range(16):
            for g in range(16):
                for r in range(16):
                    lines.append(' '.join(f'{min(1,value/15*tint[i]):.6f}' for i,value in enumerate((r,g,b))))
        (ROOT/"ui"/f"{name}.cube").write_text('\n'.join(lines)+'\n')


def main():
    for name,coat,hair,kind in [
        ("aster",(178,61,66),(83,53,40),"person"),("mira",(49,126,142),(235,197,118),"person"),
        ("elder",(118,93,157),(204,203,197),"person"),("merchant",(89,142,86),(104,64,40),"person"),
        ("innkeeper",(74,109,163),(124,76,44),"person"),("slime",(86,177,132),(0,0,0),"slime"),
        ("golem",(106,112,138),(0,0,0),"golem"),("chest",(0,0,0),(0,0,0),"chest")]:
        sprite(name,coat,hair,kind)
    for name,cave,battle in [("town",False,False),("grotto",True,False),("battle_grass",False,True),("battle_cave",True,True)]:
        result = make_map(name,cave,battle)
        result["post"]["lut"]="ui/grotto.cube" if cave else "ui/amber.cube"
        # Event definitions are hand-authored; regeneration preserves them.
        target = ROOT/"maps"/name/"map.json"
        if target.exists(): result["entities"] = json.loads(target.read_text()).get("entities",[])
        write_json(target,result)
    audio()
    ui_and_fx()


if __name__ == "__main__":
    main()
