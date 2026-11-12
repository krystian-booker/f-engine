#!/usr/bin/env python3
"""Write samples/aetherbound/replays/*.json.

The replays do not record a person's key presses: they start
`dev/autoplay.lua`, a player made of code that presses the buttons through
the game's own input, and then wait for moments of the story (`wait_for`) to
take screenshots and check state. That keeps them valid when a line of
dialogue or a number in the battle tables changes.

    python3 tools/aetherbound/make_replays.py
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "samples", "aetherbound", "replays")
BATTLE = "require('game.battle.scene')"
FIELD = "require('game.field')"
STORY = "require('story.prologue')"
DIALOGUE = "require('game.dialogue')"
AUTO = "require('dev.autoplay')"


class Replay:
    def __init__(self, name):
        self.name, self.commands, self.tick = name, [], 4

    def add(self, after=2, **command):
        self.tick += after
        command["tick"] = self.tick
        self.commands.append(command)

    def lua(self, code, after=2):
        self.add(after, lua=code)

    def wait(self, expression, timeout=6000, after=2):
        self.add(after, wait_for=expression, timeout=timeout)

    def check(self, expression, message, after=2):
        self.add(after, **{"assert": expression, "message": message})

    def capture(self, shot, after=2, tolerance=0.035):
        self.add(after, capture=f"replay-out/aetherbound-{shot}.png",
                 compare=f"reference/{{backend}}/{shot}.png", tolerance=tolerance)

    def write(self):
        self.check("#fe.dev.errors() == 0", "script errors", after=10)
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, self.name + ".json"), "w", encoding="utf-8") as out:
            json.dump(self.commands, out, indent=1)
            out.write("\n")


def opening():
    r = Replay("opening")
    # The title, then the bridge: the opening is watched at its own pace.
    r.capture("title", after=150)
    r.add(input={"key:Return": 1})
    r.add(input={"key:Return": 0})
    r.wait(f"{STORY}.scene == 'fortress' and {FIELD}.world ~= nil", 900)
    r.lua(f"{AUTO}.start(false)")
    r.capture("bridge-wide", after=520)
    r.wait(f"{DIALOGUE}.line ~= nil and {DIALOGUE}.line.speaker == 'Lyra' and {DIALOGUE}.line.shown >= {DIALOGUE}.line.total", 3000)
    r.capture("bridge-dialogue", after=4)
    r.lua(f"{AUTO}.start(true)")
    r.wait(f"{FIELD}.locked == 0 and not {DIALOGUE}.active()", 9000)
    r.check(f"{FIELD}.actors.cael ~= nil and {FIELD}.world.map_id == 'fortress'", "the player has Cael on the fortress map")
    r.capture("field", after=40)
    # The player walks: through the gate and into the first fight.
    r.lua(f"{AUTO}.walk('enc1')")
    r.wait(f"{BATTLE}.active ~= nil and {BATTLE}.active.hint ~= nil", 6000)
    r.capture("battle-tutorial", after=30)
    r.wait(f"{BATTLE}.active ~= nil and {BATTLE}.active.menu ~= nil and {BATTLE}.active.hint == nil", 3000)
    r.capture("battle-commands", after=20)
    r.wait(f"{BATTLE}.active == nil and {FIELD}.locked == 0", 20000)
    r.check("require('game.party').state.cael.hp > 0", "the party won")
    r.write()


def colossus():
    r = Replay("colossus")
    r.lua(f"{STORY}.start('chamber')")
    r.lua(f"{AUTO}.start(true)")
    r.wait(f"{BATTLE}.active ~= nil and {BATTLE}.active.menu ~= nil", 9000)
    r.check(f"{BATTLE}.active.boss.hp == 8000", "the Colossus starts with 8,000 HP")
    r.capture("colossus-battle", after=30)
    r.wait(f"{BATTLE}.active == nil", 40000)
    r.wait("require('story.chamber').card ~= nil", 9000)
    r.capture("overload", after=60)
    r.wait(f"{BATTLE}.active ~= nil and {BATTLE}.active.menu ~= nil and {BATTLE}.active.menu.unit.id == 'rowan'", 40000)
    r.check(f"{BATTLE}.active.boss.invulnerable and {BATTLE}.active.boss.hp == 4000", "the Colossus stopped at half and cannot be hurt")
    r.capture("unbound", after=20)
    r.wait(f"{BATTLE}.active.menu ~= nil and #{BATTLE}.active.menu.commands == 1", 40000)
    r.check(f"{BATTLE}.active.menu.commands[1].label == 'LAST LIGHT'", "one command is left")
    r.capture("last-light", after=20)
    r.wait(f"{STORY}.scene == 'last_light'", 3000)
    r.write()


def ending():
    r = Replay("ending")
    r.lua(f"{STORY}.start('remains')")
    r.lua(f"{AUTO}.start(false)")
    r.wait(f"{FIELD}.world ~= nil and {FIELD}.world.map_id == 'ruins'", 900)
    r.check(f"{FIELD}.actors.rowan == nil", "Rowan is not there")
    r.wait(f"{DIALOGUE}.line ~= nil and {DIALOGUE}.line.speaker == 'Darius'", 6000)
    r.capture("ruins", after=30)
    r.wait(f"{DIALOGUE}.line ~= nil and {DIALOGUE}.line.markup:find('RIGHT HERE') ~= nil", 20000)
    r.capture("crater", after=20)
    r.wait(f"{STORY}.scene == 'end'", 30000)
    r.wait(f"{STORY}.ended", 3000)
    r.write()


def playthrough():
    """The whole prologue from the title, start to finish, with nothing skipped but the reading."""
    r = Replay("playthrough")
    r.add(after=120, input={"key:Return": 1})
    r.add(input={"key:Return": 0})
    r.wait(f"{STORY}.scene == 'fortress' and {FIELD}.world ~= nil", 900)
    r.lua(f"{AUTO}.start(true)")
    r.wait(f"{FIELD}.locked == 0 and not {DIALOGUE}.active()", 9000)
    for zone in ("enc1", "enc2"):
        r.lua(f"{AUTO}.walk('{zone}')")
        r.wait(f"{BATTLE}.active ~= nil", 9000)
        r.wait(f"{BATTLE}.active == nil and {FIELD}.locked == 0", 30000)
    r.lua(f"{AUTO}.walk('to_chamber')")
    r.wait(f"{STORY}.scene == 'chamber'", 9000)
    r.wait(f"{STORY}.scene == 'last_light'", 90000)
    r.wait(f"{STORY}.scene == 'remains'", 12000)
    r.wait(f"{STORY}.scene == 'end'", 30000)
    r.wait(f"{STORY}.ended", 3000)
    r.write()


if __name__ == "__main__":
    opening()
    colossus()
    ending()
    playthrough()
    print("wrote", ", ".join(sorted(os.listdir(OUT))))
