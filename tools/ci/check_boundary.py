#!/usr/bin/env python3
"""Fail when engine C++ contains JRPG game vocabulary.

The engine is a set of generic services. Game rules (stats, battles, parties,
items, shops, encounters, quests...) belong in Lua: either the shipped lualib
kits or a game's own scripts. Run with --report to list hits without failing.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORDS = ["hp", "mp", "party", "battle", "battler", "battlers", "skill", "skills", "item", "items",
         "inventory", "shop", "shops", "encounter", "encounters", "quest", "quests", "troop", "troops",
         "enemy", "enemies", "gold", "exp", "experience", "equipment", "spell", "spells"]
# Identifier fragments are split on camelCase and snake_case boundaries.
PATTERN = re.compile(r"\b(" + "|".join(WORDS) + r")\b", re.IGNORECASE)
# Lines that legitimately mention these words carry this marker.
ALLOW_MARKER = "boundary-ok"
# Third-party APIs whose names contain the words (JSON iteration, ImGui,
# the exponential function) are not game vocabulary.
THIRD_PARTY = re.compile(r"\.items\(\)|ImGui::\w+|std::exp\b")
STRING = re.compile(r'"(?:[^"\\]|\\.)*"')


def split_identifiers(line):
    line = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", line)
    return line.replace("_", " ")


def scan(paths):
    hits = []
    for path in paths:
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if ALLOW_MARKER in line:
                continue
            code = THIRD_PARTY.sub("", STRING.sub('""', line.split("//", 1)[0]))
            match = PATTERN.search(split_identifiers(code))
            if match:
                hits.append((path.relative_to(ROOT), number, match.group(1), line.strip()))
    return hits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", action="store_true", help="print hits but exit successfully")
    args = parser.parse_args()
    files = sorted(p for p in (ROOT / "engine").rglob("*") if p.suffix in (".cpp", ".h", ".hpp"))
    hits = scan(files)
    for path, number, word, text in hits:
        print(f"{path}:{number}: '{word}': {text}")
    print(f"{len(hits)} boundary violation(s) in {len(files)} engine files")
    return 0 if args.report or not hits else 1


if __name__ == "__main__":
    sys.exit(main())
