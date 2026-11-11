#!/usr/bin/env python3
"""Fail when engine C++ contains JRPG game vocabulary.

The engine is a set of generic services. Game rules (stats, battles, parties,
items, shops, encounters, quests...) belong in a game's own Lua; the engine
ships none. Code and string literals are both checked (a
`type == "battle"` test or a doc string about parties is game knowledge too);
comments are not. Run with --report to list hits without failing.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORDS = ["hp", "mp", "party", "battle", "battler", "battlers", "skill", "skills", "item", "items",
         "inventory", "shop", "shops", "encounter", "encounters", "quest", "quests", "troop", "troops",
         "enemy", "enemies", "gold", "exp", "experience", "equipment", "spell", "spells", "npc", "npcs"]
# Identifier fragments are split on camelCase and snake_case boundaries.
PATTERN = re.compile(r"\b(" + "|".join(WORDS) + r")\b", re.IGNORECASE)
# Lines that legitimately mention these words carry this marker in a comment.
ALLOW_MARKER = "boundary-ok"
# String literals that use a listed word in a non-game sense: (file, exact
# literal) -> reason.
ALLOWED_LITERALS = {
    ("engine/text/Markup.cpp", "gold"): "a colour name in the markup palette",
}
# Third-party APIs whose names contain the words (JSON iteration, ImGui,
# the exponential function) are not game vocabulary.
THIRD_PARTY = re.compile(r"\.items\(\)|ImGui::\w+|std::exp\b")
RAW_STRING = re.compile(r'R"([^()\\ ]{0,16})\(')


def split_identifiers(text):
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", text)
    return text.replace("_", " ")


def separate(source):
    """Splits C++ source into code and literals. Returns (code, literals):
    `code` is `source` with comments and literal contents blanked (newlines
    kept, so line numbers still match) and `literals` lists the
    (line number, contents) of every string and character literal."""
    code, literals = [], []
    i, n, line = 0, len(source), 1

    def blank(text):
        code.append(re.sub(r"[^\n]", " ", text))
        return text.count("\n")

    def literal(start, end):
        literals.append((line, source[start:end]))
        return blank(source[start:end])

    while i < n:
        ch, two = source[i], source[i:i + 2]
        raw = RAW_STRING.match(source, i) if ch == "R" and (i == 0 or not source[i - 1].isalnum()) else None
        if two == "//":
            end = source.find("\n", i)
            end = n if end < 0 else end
            line += blank(source[i:end])
            i = end
        elif two == "/*":
            end = source.find("*/", i + 2)
            end = n if end < 0 else end + 2
            line += blank(source[i:end])
            i = end
        elif raw:
            closer = ")" + raw.group(1) + '"'
            end = source.find(closer, raw.end())
            end = n if end < 0 else end
            code.append(source[i:raw.end()])
            line += literal(raw.end(), end)
            code.append(source[end:end + len(closer)])
            i = end + len(closer)
        elif ch == '"' or (ch == "'" and not (i > 0 and source[i - 1].isalnum())):
            j = i + 1
            while j < n and source[j] != ch and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            code.append(ch)
            line += literal(i + 1, j)
            code.append(source[j:j + 1])
            i = j + 1
        else:
            code.append(ch)
            line += ch == "\n"
            i += 1
    return "".join(code), literals


def scan(paths):
    hits = []
    for path in paths:
        relative = path.relative_to(ROOT)
        source = path.read_text(errors="replace")
        lines = source.splitlines()
        allowed = {number for number, text in enumerate(lines, 1) if ALLOW_MARKER in text}
        code, literals = separate(source)
        found = []
        for number, text in enumerate(code.splitlines(), 1):
            match = PATTERN.search(split_identifiers(THIRD_PARTY.sub("", text)))
            if match:
                found.append((number, "code", match.group(1)))
        for number, text in literals:
            match = PATTERN.search(split_identifiers(text))
            if match and (relative.as_posix(), text) not in ALLOWED_LITERALS:
                found.append((number, "string", match.group(1)))
        for number, kind, word in sorted(found):
            if number not in allowed:
                hits.append((relative, number, kind, word, lines[number - 1].strip()))
    return hits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", action="store_true", help="print hits but exit successfully")
    args = parser.parse_args()
    files = sorted(p for p in (ROOT / "engine").rglob("*") if p.suffix in (".cpp", ".h", ".hpp"))
    hits = scan(files)
    for path, number, kind, word, text in hits:
        print(f"{path}:{number}: '{word}' in {kind}: {text}")
    print(f"{len(hits)} boundary violation(s) in {len(files)} engine files")
    return 0 if args.report or not hits else 1


if __name__ == "__main__":
    sys.exit(main())
