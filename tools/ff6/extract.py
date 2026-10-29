#!/usr/bin/env python3
"""Export a verified FF3 US 1.1 ROM using a pinned external FF6 disassembly.

The reference supplies format definitions and codecs; no ROM is downloaded.
Only the Python standard library and the external romtools module are needed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

REFERENCE_COMMIT = "813013276c952fdd27edcf7b8b86f17291542cbf"
ROMTOOLS_COMMIT = "f023d6b2ebdafd5a23a79264a9a3775ff5ea537e"
ROM_SHA256 = "10eccc5d2fab81346dd759f6be478dcb682eef981e8d3d662da176e1f9a996bc"
MAPS = {i: f"location_{i:03d}" for i in range(18, 52)}
MAPS.update({5: "flashback_stage", 18: "overlook", 19: "narshe_assault",
        20: "narshe_exploration", 21: "narshe_north_variant", 22: "narshe_hills_maze",
        23: "narshe_cliffs_prologue", 30: "narshe_interiors_arvis", 39: "north_mine_entrance",
        40: "north_mines_variant", 41: "north_mines_and_secret_passage",
        50: "south_mines_escape", 51: "south_mines_defense",
        104: "beginner_school", 105: "beginner_school_room_1",
        106: "beginner_school_room_2", 107: "beginner_school_room_3",
        244: "imperial_exterior", 250: "imperial_flashback"}
)


def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def png(path, width, height, rgba):
    assert len(rgba) == width * height * 4
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def palette(data):
    colors = []
    for (word,) in struct.iter_unpack("<H", data):
        colors.append(bytes([((word >> shift) & 31) * 255 // 31 for shift in (0, 5, 10)] + [255]))
    return colors


def tile(data, index, bpp=4):
    start = index * bpp * 8
    if start + bpp * 8 > len(data):
        return [0] * 64
    return [sum(((data[start + (p // 2) * 16 + y * (1 if p == 2 and bpp == 3 else 2)
                            + (p % 2)] >> (7 - x)) & 1) << p for p in range(bpp))
            for y in range(8) for x in range(8)]


def blit_tile(dest, width, x, y, pixels, colors, color_base=0, hflip=False, vflip=False, transparent=True):
    for dy in range(8):
        for dx in range(8):
            value = pixels[(7 - dy if vflip else dy) * 8 + (7 - dx if hflip else dx)]
            if transparent and value == 0:
                continue
            offset = ((y + dy) * width + x + dx) * 4
            dest[offset:offset + 4] = colors[(color_base + value) % len(colors)]


def sheet(data, colors, bpp=4, columns=16):
    count = len(data) // (bpp * 8)
    width, height = columns * 8, max(1, math.ceil(count / columns)) * 8
    result = bytearray(width * height * 4)
    for i in range(count):
        blit_tile(result, width, i % columns * 8, i // columns * 8, tile(data, i, bpp), colors)
    return width, height, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=Path("rom-dump/ff6-narshe"))
    args = parser.parse_args()
    ref, out = args.reference.resolve(), args.output.resolve()
    rom = args.rom.read_bytes()
    if digest(rom) != ROM_SHA256:
        parser.error("Expected the exact unheadered US Rev 1 ROM identified in the spec; input was not modified.")
    for folder, expected in [(ref, REFERENCE_COMMIT), (ref / "tools/romtools", ROMTOOLS_COMMIT)]:
        actual = subprocess.check_output(["git", "-C", str(folder), "rev-parse", "HEAD"], text=True).strip()
        if actual != expected:
            parser.error(f"Reference revision mismatch at {folder}: expected {expected}, got {actual}")
    sys.path.insert(0, str(ref / "tools/romtools"))
    sys.path.insert(0, str(ref / "tools"))
    import romtools as rt
    from ff6_lzss import decode_lzss

    extractor = rt.AssetExtractor(bytearray(rom), "hirom")
    rip = json.loads((ref / "tools/rip_list_en.json").read_text())
    records, definitions, text_tables = [], {}, {}
    native = out / "native"

    def write(relative, data, **metadata):
        target = out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        records.append({"path": str(relative), "bytes": len(data), "sha256": digest(data), **metadata})

    def normalized(path):
        return Path(path.removeprefix("build/en/").removeprefix("assets/"))

    for category in ("text", "data", "array"):
        for definition in rip[category]:
            blob, spans = extractor.extract_asset(**definition)
            source_start = int(definition["asset_range"].split("-")[0], 16) - 0xC00000
            path = definition["asset_path"]
            names = definition.get("file_list", [f"{i:04x}" for i in range(len(spans))])
            definitions[path] = {"definition": definition, "names": names}
            if category == "text":
                template = (ref / path).read_text()
                codec = rt.TextCodec()
                for name in re.findall(r"^#char_tbl:(.+)$", template, re.M):
                    codec.load_char_table(str(ref / f"tools/char_table/{name}.json"))
                ids = re.findall(r"^#text:(.*)$", template, re.M)
                decoded = [{"id": ids[i] if i < len(ids) else str(i), "index": i,
                            "rom_offset": source_start + s.begin,
                            "text": codec.decode(blob[s.begin:s.end + 1])} for i, s in enumerate(spans)]
                rel = normalized(path)
                write(Path("native") / rel.with_suffix(".bin"), blob, source_offset=source_start,
                      source_bytes=len(blob), encoding="original text table")
                dump(out / "text" / rel.with_suffix(".json").name, decoded)
                text_tables[rel.stem] = decoded
                continue
            if category == "array":
                write(Path("native") / normalized(path), blob, source_offset=source_start,
                      source_bytes=len(blob), encoding="original array")
                dump(out / "indices" / (Path(path).stem + ".json"),
                     [{"index": i, "offset": s.begin, "length": s.end - s.begin + 1} for i, s in enumerate(spans)])
                continue
            for i, span in enumerate(spans):
                if span.is_empty() or span.begin < 0:
                    continue
                rel = normalized(path.replace("%s", names[i]))
                encoded = blob[span.begin:span.end + 1]
                decoded = encoded
                form = definition.get("format", "raw")
                if form == "lz":
                    decoded = decode_lzss(encoded)
                elif form == "stc":
                    stencil_rel = Path(str(rel).replace(".en.", ".")).with_suffix(rel.suffix + ".stn")
                    stencil = (native / stencil_rel).read_bytes()
                    side = 8 if len(stencil) == 8 else 16
                    assert len(stencil) in (8, 32)
                    tile_bytes = 24 if rel.suffix == ".3bpp" else 32
                    rows = [int.from_bytes(stencil[y * (side // 8):(y + 1) * (side // 8)], "big") for y in range(side)]
                    decoded, cursor = bytearray(side * side * tile_bytes), 0
                    for y in range(side):
                        for x in range(side):
                            if rows[y] & (1 << (side - 1 - x)):
                                offset = (y * side + x) * tile_bytes
                                decoded[offset:offset + tile_bytes] = encoded[cursor:cursor + tile_bytes]
                                cursor += tile_bytes
                    assert cursor <= len(encoded)
                write(Path("native") / rel, decoded, source_offset=source_start + span.begin,
                      source_bytes=len(encoded), source_sha256=digest(encoded), encoding=form, source_index=i)

    # Raw tables absent from the upstream rip list. Addresses are file offsets.
    extra = {"events/all_field_events.bin": (0xA0000, 0x2E600),
             "events/narshe_primary.bin": (0xC985B, 0x18D8),
             "events/map_startup.bin": (0x11FA00, 416 * 3),
             "events/trigger_pointers.bin": (0x40000, 0x342),
             "events/triggers.bin": (0x40342, 0x16CE),
             "events/npc_pointers.bin": (0x41A10, 0x342),
             "events/npcs.bin": (0x41D52, 0x4D6E),
             "battle/monster_stats.bin": (0xF0000, 384 * 32),
             "battle/ai_pointers.bin": (0xF8400, 384 * 2),
             "battle/ai_scripts.bin": (0xF8700, 0x3950),
             "battle/event_animation_region.bin": (0x109842, 0x39BE),
             "events/short_exit_pointers.bin": (0x1FBB00, 0x402),
             "events/long_exit_pointers.bin": (0x2DF480, 0x402),
             "sound/song_pointers.bin": (0x53E96, 85 * 3)}
    for name, (offset, size) in extra.items():
        write(Path("native") / name, rom[offset:offset + size], source_offset=offset, source_bytes=size, encoding="raw")

    # Song data are sequences for the SPC driver, not rendered recordings.
    songs = []
    for name, hex_id in re.findall(r"^\s+(\w+)\s+;= \$(\w+)\s*;", (ref / "src/sound/song_script.inc").read_text(), re.M):
        song_id = int(hex_id, 16)
        if song_id >= 85:
            continue
        p = int.from_bytes(rom[0x53E96 + song_id * 3:0x53E99 + song_id * 3], "little")
        offset = p - 0xC00000
        if not 0 <= offset < len(rom) - 2:
            continue
        size = int.from_bytes(rom[offset:offset + 2], "little")
        assert 0 < size < 0x10000 and offset + size + 2 <= len(rom)
        rel = Path("native/sound/sequences") / f"{song_id:02x}_{name.lower()}.bin"
        write(rel, rom[offset:offset + size + 2], source_offset=offset, source_bytes=size + 2, encoding="SPC sequence with length header")
        songs.append({"id": song_id, "name": name, "path": str(rel), "status": "sequence only; PCM render required"})
    dump(out / "indices/songs.json", songs)

    monster_names = text_tables["monster_name.en"]
    monsters = []
    for i in range(384):
        raw = rom[0xF0000 + i * 32:0xF0020 + i * 32]
        monsters.append({"id": i, "name": monster_names[i]["text"], "rom_offset": 0xF0000 + i * 32,
                         "speed": raw[0], "attack": raw[1], "defense": raw[5], "magic_defense": raw[6],
                         "magic_power": raw[7], "hp": int.from_bytes(raw[8:10], "little"),
                         "mp": int.from_bytes(raw[10:12], "little"), "exp": int.from_bytes(raw[12:14], "little"),
                         "gil": int.from_bytes(raw[14:16], "little"), "level": raw[16], "raw": raw.hex()})
    dump(out / "indices/monsters.json", monsters)
    groups = []
    for group_id in list(range(7)) + [64, 77, 115]:
        formation_ids = struct.unpack_from("<HH", rom, 0xF5000 + group_id * 4)
        formations = []
        for fid in formation_ids:
            raw = rom[0xF6200 + fid * 15:0xF6200 + fid * 15 + 15]
            members = []
            for slot in range(6):
                if raw[1] & (1 << slot):
                    mid = raw[2 + slot] | (((raw[14] >> slot) & 1) << 8)
                    members.append({"slot": slot, "monster_id": mid, "name": monsters[mid]["name"], "packed_position": raw[8 + slot]})
            formations.append({"id": fid, "members": members, "raw": raw.hex()})
        groups.append({"event_group_id": group_id, "formations": formations})
    dump(out / "indices/opening_encounters.json", groups)
    random_groups = []
    for map_id in MAPS:
        props = rom[0x2D8F00 + map_id * 33:0x2D8F21 + map_id * 33]
        if not props[5] & 128:
            continue
        group_id = rom[0xF5600 + map_id]
        formations = []
        for weight, fid in zip((5, 5, 5, 1), struct.unpack_from("<4H", rom, 0xF4800 + group_id * 8)):
            raw = rom[0xF6200 + fid * 15:0xF620F + fid * 15]
            members = []
            for slot in range(6):
                if raw[1] & (1 << slot):
                    mid = raw[2 + slot] | (((raw[14] >> slot) & 1) << 8)
                    members.append({"monster_id": mid, "name": monsters[mid]["name"]})
            formations.append({"id": fid, "weight_out_of_16": weight, "members": members})
        random_groups.append({"map_id": map_id, "group_id": group_id, "formations": formations})
    dump(out / "indices/opening_random_encounters.json", random_groups)

    # Sprite atlases from the actual per-pose tile offset table in the ROM.
    sprite_names = definitions["assets/gfx/map_sprite_gfx/%s.4bpp"]["names"]
    pal_names = definitions["assets/gfx/map_sprite_pal/%s.pal"]["names"]
    selected = {"terra": 2, "locke": 1, "mog": 5, "soldier": 1, "narshe_guard": 0,
                "arvis": 0, "old_man": 0, "dog": 0, "kefka": 3, "gestahl": 3, "banon": 3}
    sprite_meta = []
    for name, pal_id in selected.items():
        if name not in sprite_names:
            continue
        gfx = (native / f"gfx/map_sprite_gfx/{name}.4bpp").read_bytes()
        colors = palette((native / f"gfx/map_sprite_pal/{pal_names[pal_id]}.pal").read_bytes())
        width, height = 128, 8 * 24
        pixels = bytearray(width * height * 4)
        poses = []
        for pose in range(58):
            offsets = struct.unpack_from("<6H", rom, 0xCE3A + pose * 12)
            if max(offsets) + 32 > len(gfx):
                continue
            x0, y0 = pose % 8 * 16, pose // 8 * 24
            for t, offset in enumerate(offsets):
                blit_tile(pixels, width, x0 + t % 2 * 8, y0 + t // 2 * 8, tile(gfx, offset // 32), colors)
            if rom[0xCDBA + pose] & 0x40:
                for y in range(y0 + 8, y0 + 24):
                    row = [bytes(pixels[(y * width + x0 + x) * 4:(y * width + x0 + x + 1) * 4]) for x in range(16)]
                    for x, value in enumerate(reversed(row)):
                        p = (y * width + x0 + x) * 4
                        pixels[p:p + 4] = value
            poses.append({"pose": pose, "rect": [x0, y0, 16, 24]})
        png(out / f"previews/sprites/{name}.png", width, height, pixels)
        sprite_meta.append({"name": name, "palette": pal_id, "poses": poses,
                            "status": "pose atlas; palette assignments except Terra Locke Mog soldier require scene review"})
    dump(out / "indices/sprite_poses.json", sprite_meta)

    for rel, pal_rel, bpp in [("gfx/vehicle.4bpp", "gfx/map_sprite_pal/vehicle.pal", 4),
                              ("gfx/window/window_0000.4bpp", "gfx/window/window_0000.pal", 4)]:
        if (native / rel).exists() and (native / pal_rel).exists():
            w, h, pixels = sheet((native / rel).read_bytes(), palette((native / pal_rel).read_bytes()), bpp)
            png(out / ("previews/tiles/" + Path(rel).stem + ".png"), w, h, pixels)
    for portrait_dir in (native / "gfx/portrait").iterdir():
        if portrait_dir.is_dir() and (portrait_dir / (portrait_dir.name + ".pal")).exists():
            gfx = (portrait_dir / (portrait_dir.name + ".4bpp")).read_bytes()
            colors = palette((portrait_dir / (portrait_dir.name + ".pal")).read_bytes())
            w, h, pixels = sheet(gfx, colors, 4, 5)
            png(out / f"previews/portraits/{portrait_dir.name}.png", w, h, pixels)

    # Relevant enemy art, retaining palette variants and planar tile order.
    for gfx_name, pal_name in [("guard", "guard"), ("guard", "marshal"), ("lobo", "lobo"),
                               ("whelk_shell", "whelk"), ("whelk_head_1", "whelk_head_1"),
                               ("whelk_head_2", "whelk_head_2"), ("vaporite", "vaporite"),
                               ("vomammoth", "vomammoth"), ("were_rat", "were_rat"),
                               ("repo_man", "repo_man")]:
        choices = list((native / "gfx/monster_gfx").glob(gfx_name + ".[34]bpp"))
        pal_path = native / f"gfx/monster_gfx/{pal_name}.pal"
        if not choices or not pal_path.exists():
            continue
        gfx = choices[0].read_bytes()
        bpp = int(choices[0].suffix[1])
        columns = math.isqrt(len(gfx) // (bpp * 8))
        w, h, pixels = sheet(gfx, palette(pal_path.read_bytes()), bpp, columns)
        png(out / f"previews/enemies/{pal_name}.png", w, h, pixels)

    # Static map layers. Priority, palette animation, BG3 math, event edits and
    # parallax are preserved as metadata and must be applied by the importer.
    def named(group, index):
        d = definitions[group]
        return native / normalized(d["definition"]["asset_path"].replace("%s", d["names"][index]))

    map_records = []
    prop_blob = (native / "data/field/map_prop.bin").read_bytes()
    for map_id, name in MAPS.items():
        prop = prop_blob[map_id * 33:(map_id + 1) * 33]
        gfx_bits = int.from_bytes(prop[7:12], "little")
        gfx_ids = [(gfx_bits >> (i * 7)) & 127 for i in range(4)]
        tilesets = [(int.from_bytes(prop[11:13], "little") >> 2) & 127, prop[12] >> 1]
        layout_bits = int.from_bytes(prop[13:17], "little")
        layouts = [(layout_bits >> (i * 10)) & 1023 for i in range(3)]
        dimensions = [[16 << ((prop[23] >> 6) & 3), 16 << ((prop[23] >> 4) & 3)],
                      [16 << ((prop[23] >> 2) & 3), 16 << (prop[23] & 3)],
                      [16 << ((prop[24] >> 6) & 3), 16 << ((prop[24] >> 4) & 3)]]
        colors = palette(named("assets/gfx/map_pal/%s.pal", prop[25]).read_bytes())
        vram = bytearray(0x6000)
        # SNES VRAM addresses in the loader are words, hence byte destinations.
        for i, dest in enumerate((0, 0x2000, 0x3000, 0x4000)):
            if i == 3 and gfx_ids[2] == gfx_ids[3]:
                continue
            source_path = str(named("assets/gfx/map_gfx/%s.4bpp", gfx_ids[i]).relative_to(out))
            source_offset = next(r["source_offset"] for r in records if r["path"] == source_path)
            vram[dest:dest + 0x2000] = rom[source_offset:source_offset + 0x2000]
        layer_records = []
        tile_cache = {}
        for layer in range(3):
            lid = layouts[layer]
            if lid == 0:
                continue
            layout_path = named("assets/data/field/sub_tilemap/%s.bin", lid)
            cells = layout_path.read_bytes()
            w, h = dimensions[layer]
            layer_record = {"layer": layer + 1, "layout_id": lid, "layout_path": str(layout_path.relative_to(out)),
                            "width_cells": w, "height_cells": h, "cells": list(cells)}
            if layer < 2:
                form = named("assets/data/field/map_tileset/%s.bin", tilesets[layer]).read_bytes()
                pixels = bytearray(w * 16 * h * 16 * 4)
                priority = []
                for y in range(h):
                    for x in range(w):
                        cell = cells[(y * w + x) % len(cells)]
                        for quadrant in range(4):
                            off = quadrant * 256 + cell
                            word = form[off] | form[off + 1024] << 8
                            if word & 0x2000:
                                priority.append([x * 2 + quadrant % 2, y * 2 + quadrant // 2])
                            tile_id = word & 1023
                            if tile_id not in tile_cache:
                                tile_cache[tile_id] = tile(vram, tile_id)
                            blit_tile(pixels, w * 16, x * 16 + quadrant % 2 * 8, y * 16 + quadrant // 2 * 8,
                                      tile_cache[tile_id], colors, ((word >> 10) & 7) * 16,
                                      bool(word & 0x4000), bool(word & 0x8000))
                image_path = f"previews/maps/{map_id:03d}_{name}_bg{layer + 1}.png"
                png(out / image_path, w * 16, h * 16, pixels)
                layer_record.update(preview=image_path, priority_subtiles=priority, tileset_id=tilesets[layer])
            layer_records.append(layer_record)
        # Map-owned event and NPC records use a bank-relative pointer table.
        triggers, npcs = [], []
        for table, base, stride, target in [(0x40000, 0x40000, 5, triggers), (0x41A10, 0x41A10, 9, npcs)]:
            begin = int.from_bytes(rom[table + map_id * 2:table + map_id * 2 + 2], "little") + base
            end = int.from_bytes(rom[table + map_id * 2 + 2:table + map_id * 2 + 4], "little") + base
            assert (end - begin) % stride == 0
            for offset in range(begin, end, stride):
                raw = rom[offset:offset + stride]
                if stride == 5:
                    target.append({"x": raw[0], "y": raw[1], "event_snes": f"{0xCA0000 + int.from_bytes(raw[2:5], 'little'):06X}", "rom_offset": offset})
                else:
                    target.append({"x": raw[4] & 127, "y": raw[5] & 63, "gfx_id": raw[6], "palette": (raw[2] >> 2) & 7,
                                   "event_snes": f"{0xCA0000 + (int.from_bytes(raw[:3], 'little') & 0x3FFFF):06X}",
                                   "raw": raw.hex(), "rom_offset": offset})
        exits = []
        for kind, table, stride, map_offset in [("short", 0x1FBB00, 6, 2), ("long", 0x2DF480, 7, 3)]:
            begin, end = struct.unpack_from("<HH", rom, table + map_id * 2)
            assert (end - begin) % stride == 0
            for offset in range(table + begin, table + end, stride):
                raw = rom[offset:offset + stride]
                exits.append({"kind": kind, "x": raw[0], "y": raw[1],
                              "destination_map": int.from_bytes(raw[map_offset:map_offset + 2], "little") & 511,
                              "destination_x": raw[map_offset + 2], "destination_y": raw[map_offset + 3],
                              "raw": raw.hex(), "rom_offset": offset})
        record = {"id": map_id, "name": name, "rom_offset": 0x2D8F00 + map_id * 33,
                  "properties_hex": prop.hex(), "gfx_ids": gfx_ids, "bg3_gfx_id": (gfx_bits >> 28) & 63,
                  "palette_id": prop[25], "tile_properties_id": prop[4], "song_id": prop[28],
                  "layers": layer_records, "triggers": triggers, "npcs": npcs, "exits": exits,
                  "startup_event_snes": f"{0xCA0000 + int.from_bytes(rom[0x11FA00 + map_id * 3:0x11FA03 + map_id * 3], 'little'):06X}",
                  "status": "static BG1 BG2 decoded; runtime animation BG3 color math collision and event mutations require conversion"}
        dump(out / f"maps/{map_id:03d}_{name}.json", record)
        map_records.append({k: v for k, v in record.items() if k != "layers"})
    dump(out / "indices/maps.json", map_records)
    dialogue = text_tables["dlg.en"]
    dump(out / "text/narshe_dialogue.json", [d for d in dialogue if 1 <= d["index"] <= 63 or 599 <= d["index"] <= 637
                                            or d["index"] in (1744, 1746, 1748, 1756, 2923, 2924, 2925, 2926, 2927)])
    dump(out / "definitions.json", definitions)
    dump(out / "manifest.json", {"rom": {"filename": args.rom.name, "bytes": len(rom), "sha256": digest(rom),
                                         "crc32": f"{zlib.crc32(rom):08X}", "copier_header_bytes": 0},
                                 "reference": {"url": "https://github.com/everything8215/ff6", "commit": REFERENCE_COMMIT,
                                               "romtools_commit": ROMTOOLS_COMMIT},
                                 "native_file_count": len(records), "native_bytes": sum(r["bytes"] for r in records),
                                 "files": records})
    for record in records:
        actual = (out / record["path"]).read_bytes()
        assert len(actual) == record["bytes"] and digest(actual) == record["sha256"]
    print(f"Verified {len(records)} native exports, {len(map_records)} map records, {len(songs)} song sequences at {out}")


if __name__ == "__main__":
    main()
