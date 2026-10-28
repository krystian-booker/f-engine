#include "engine/core/Paths.h"
#include "engine/runtime/Engine.h"
#include "engine/script/bindings/Convert.h"

namespace fe {
void bindSave(Engine& engine) {
    auto& host = engine.lua();
    auto& saves = engine.saves();
    ModuleBinder(
        host, engine.docs(), "fe.save",
        "Per-user persistence. Saves are JSON documents the game defines; the engine adds no schema.")
        .fn(
            "write",
            [&saves](const std::string& slot, sol::object data, sol::optional<sol::object> meta) {
                saves.write(slot, luaToJson(data), meta ? luaToJson(*meta) : Json::object());
            },
            "fun(slot: string, data: table, meta?: table)", "Atomically write a save slot.")
        .fn(
            "read",
            [&saves, &host](const std::string& slot) -> std::tuple<sol::object, sol::object> {
                const auto loaded = saves.read(slot);
                if (!loaded) {
                    return {sol::lua_nil, sol::lua_nil};
                }
                return {jsonToLua(host.lua(), loaded->data), jsonToLua(host.lua(), loaded->meta)};
            },
            "fun(slot: string): table?, table?", "Read a slot's data and metadata (nil when empty).")
        .fn(
            "list",
            [&saves, &host](sol::this_state state) {
                sol::state_view lua(state);
                auto out = lua.create_table();
                for (const auto& info : saves.list()) {
                    out.add(lua.create_table_with("slot", info.slot, "meta", jsonToLua(host.lua(), info.meta),
                                                  "modified", info.modified, "thumbnail", info.thumbnail));
                }
                return out;
            },
            "fun(): {slot: string, meta: table, modified: number, thumbnail: boolean}[]", "All saved slots.")
        .fn(
            "delete", [&saves](const std::string& slot) { saves.remove(slot); }, "fun(slot: string)",
            "Delete a slot.")
        .fn(
            "thumbnail",
            [&engine, &saves](const std::string& slot) {
                engine.requests().captures.push_back(saves.thumbnailPath(slot));
            },
            "fun(slot: string)", "Capture the next frame as the slot's thumbnail.")
        .fn(
            "read_config", [&saves, &host] { return jsonToLua(host.lua(), saves.readConfig()); },
            "fun(): table", "Read the player's settings file.")
        .fn(
            "write_config", [&saves](sol::object data) { saves.writeConfig(luaToJson(data)); },
            "fun(settings: table)", "Write the player's settings file.");
}

void bindInput(Engine& engine) {
    auto& host = engine.lua();
    auto& input = engine.input();
    ModuleBinder(host, engine.docs(), "fe.input",
                 "Action-mapped input. Raw inputs are named 'key:Z', 'pad:a', 'pad:leftx+', 'mouse:left'. "
                 "The engine has no default bindings; scripts define every action.")
        .fn(
            "define",
            [&input](const std::string& action, sol::object inputs, sol::optional<float> threshold) {
                input.define(action, lua::strings(inputs), threshold.value_or(0.5f));
            },
            "fun(action: string, inputs: string[], threshold?: number)", "Define or replace an action.")
        .fn(
            "define_axis",
            [&input](const std::string& name, sol::table spec) {
                InputState::Axis axis;
                axis.negative = lua::strings(spec["negative"]);
                axis.positive = lua::strings(spec["positive"]);
                axis.analog = lua::strings(spec["analog"]);
                axis.deadzone = lua::get(spec, "deadzone", 0.2f);
                input.defineAxis(name, std::move(axis));
            },
            "fun(name: string, spec: {negative?: string[], positive?: string[], analog?: string[], "
            "deadzone?: number})",
            "Define an axis from digital and analog inputs.")
        .fn(
            "clear", [&input] { input.clearDefinitions(); }, "fun()", "Remove all actions and axes.")
        .fn(
            "bind",
            [&input](const std::string& action, sol::object inputs) {
                input.bind(action, lua::strings(inputs));
            },
            "fun(action: string, inputs: string[])", "Rebind an action.")
        .fn(
            "bindings", [&input](const std::string& action) { return sol::as_table(input.bindings(action)); },
            "fun(action: string): string[]", "Current inputs of an action.")
        .fn(
            "pressed", [&input](const std::string& a) { return input.pressed(a); },
            "fun(action: string): boolean", "Went down this tick.")
        .fn(
            "held", [&input](const std::string& a) { return input.held(a); }, "fun(action: string): boolean",
            "Currently down.")
        .fn(
            "released", [&input](const std::string& a) { return input.released(a); },
            "fun(action: string): boolean", "Went up this tick.")
        .fn(
            "repeating",
            [&input](const std::string& a, sol::optional<double> delay, sol::optional<double> interval) {
                return input.repeat(a, delay.value_or(0.35), interval.value_or(0.1));
            },
            "fun(action: string, delay?: number, interval?: number): boolean",
            "Pressed, then repeats while held (menu navigation).")
        .fn(
            "held_time", [&input](const std::string& a) { return input.heldTime(a); },
            "fun(action: string): number", "Seconds the action has been held.")
        .fn(
            "axis", [&input](const std::string& a) { return input.axis(a); }, "fun(axis: string): number",
            "Axis value -1..1.")
        .fn(
            "raw", [&input](const std::string& name) { return input.raw(name); },
            "fun(input: string): number", "Raw input value.")
        .fn(
            "pressed_inputs", [&input] { return sol::as_table(input.pressedInputs()); }, "fun(): string[]",
            "Raw inputs that went down this tick (for rebinding screens).")
        .fn(
            "last_device", [&input] { return input.lastDevice(); }, "fun(): string",
            "'keyboard', 'gamepad' or 'mouse' — pick button prompts with this.")
        .fn(
            "device_name", [&input] { return input.deviceName(); }, "fun(): string", "Gamepad model name.")
        .fn(
            "mouse", [&input] { return std::make_tuple(input.mouse().x, input.mouse().y); },
            "fun(): number, number", "Mouse position in canvas pixels.")
        .fn(
            "mouse_moved", [&input] { return input.mouseMoved(); }, "fun(): boolean",
            "Mouse moved this tick.")
        .fn(
            "wheel", [&input] { return input.wheel(); }, "fun(): number", "Wheel movement this tick.")
        .fn(
            "feed", [&input](const std::string& name, float value) { input.feed(name, value); },
            "fun(input: string, value: number)", "Inject a raw input (tests and replays).");
}

void bindAudio(Engine& engine) {
    auto& host = engine.lua();
    auto& audio = engine.audio();
    const auto root = host.gameRoot();
    ModuleBinder(host, engine.docs(), "fe.audio",
                 "Music layers with loop points and crossfades, sounds and buses.")
        .fn(
            "music",
            [&audio, root](const std::string& path, sol::optional<sol::table> options) {
                MusicOptions o;
                if (options) {
                    const auto& t = *options;
                    o.layer = lua::get(t, "layer", o.layer);
                    o.fade = lua::get(t, "fade", o.fade);
                    o.loopStart = lua::get(t, "loop_start", o.loopStart);
                    o.loopEnd = lua::get(t, "loop_end", o.loopEnd);
                    o.volume = lua::get(t, "volume", o.volume);
                    o.loop = lua::get(t, "loop", o.loop);
                    o.sync = lua::get(t, "sync", o.sync);
                }
                audio.playMusic(resolveInside(root, path), o);
            },
            "fun(path: string, options?: {layer?: string, fade?: number, loop_start?: number, loop_end?: "
            "number, "
            "volume?: number, loop?: boolean, sync?: boolean})",
            "Play music on a layer, crossfading from what was there.")
        .fn(
            "stop_music",
            [&audio](sol::optional<std::string> layer, sol::optional<double> fade) {
                audio.stopMusic(layer.value_or(""), fade.value_or(0.5));
            },
            "fun(layer?: string, fade?: number)", "Fade out one layer, or all music.")
        .fn(
            "music_volume",
            [&audio](const std::string& layer, float volume, sol::optional<double> fade) {
                audio.setMusicVolume(layer, volume, fade.value_or(0.5));
            },
            "fun(layer: string, volume: number, fade?: number)", "Mix a music layer (stems).")
        .fn(
            "current_music",
            [&audio](sol::optional<std::string> layer) { return audio.currentMusic(layer.value_or("main")); },
            "fun(layer?: string): string", "Path playing on a layer.")
        .fn(
            "sound",
            [&audio, root](const std::string& path, sol::optional<sol::table> options) {
                SoundOptions o;
                if (options) {
                    const auto& t = *options;
                    o.volume = lua::get(t, "volume", o.volume);
                    o.pitch = lua::get(t, "pitch", o.pitch);
                    o.pan = lua::get(t, "pan", o.pan);
                    o.loop = lua::get(t, "loop", o.loop);
                    o.bus = lua::get(t, "bus", o.bus);
                }
                return audio.playSound(resolveInside(root, path), o);
            },
            "fun(path: string, options?: {volume?: number, pitch?: number, pan?: number, loop?: boolean, "
            "bus?: string}): integer",
            "Play a sound; returns a voice id.")
        .fn(
            "stop",
            [&audio](int voice, sol::optional<double> fade) { audio.stopSound(voice, fade.value_or(0)); },
            "fun(voice: integer, fade?: number)", "Stop a voice.")
        .fn(
            "set_voice", [&audio](int voice, float volume, float pan) { audio.setSound(voice, volume, pan); },
            "fun(voice: integer, volume: number, pan: number)", "Change a playing voice.")
        .fn(
            "bus_volume",
            [&audio](const std::string& bus, sol::optional<float> volume) {
                if (volume) {
                    audio.setBusVolume(bus, *volume);
                }
                return audio.busVolume(bus);
            },
            "fun(bus: string, volume?: number): number",
            "Get or set a bus volume ('master', 'music', 'sfx', ...).")
        .fn(
            "available", [&audio] { return audio.available(); }, "fun(): boolean",
            "An audio device is open.");
}

void bindScreen(Engine& engine) {
    auto& host = engine.lua();
    auto& screen = engine.screen();
    ModuleBinder(host, engine.docs(), "fe.screen",
                 "Full-screen effects. Scripts animate these values (see fe.tween); renderers apply them.")
        .fn(
            "set",
            [&screen](sol::table t) {
                if (t["fade"].valid()) {
                    screen.fade = lua::color(t["fade"], screen.fade);
                }
                if (t["flash"].valid()) {
                    screen.flash = lua::color(t["flash"], screen.flash);
                }
                if (t["tint"].valid()) {
                    screen.tint = lua::color(t["tint"], screen.tint);
                }
                screen.letterbox = lua::get(t, "letterbox", screen.letterbox);
                if (t["transition"].valid()) {
                    sol::table tr = t["transition"];
                    screen.transition.kind = lua::get(tr, "kind", screen.transition.kind);
                    screen.transition.progress = lua::get(tr, "progress", screen.transition.progress);
                    screen.transition.centre = lua::vec2(tr["centre"], screen.transition.centre);
                    screen.transition.color = lua::color(tr["color"], screen.transition.color);
                }
            },
            "fun(settings: {fade?: number[], flash?: number[], tint?: number[], letterbox?: number, "
            "transition?: {kind?: string, progress?: number, centre?: table, color?: any}})",
            "Change screen effects. Colors are {r,g,b,amount}.")
        .fn(
            "get",
            [&screen](sol::this_state state) {
                sol::state_view lua(state);
                return lua.create_table_with(
                    "fade", lua::colorTable(lua, screen.fade), "flash", lua::colorTable(lua, screen.flash),
                    "tint", lua::colorTable(lua, screen.tint), "letterbox", screen.letterbox, "transition",
                    lua.create_table_with("kind", screen.transition.kind, "progress",
                                          screen.transition.progress, "centre",
                                          lua::table(lua, screen.transition.centre), "color",
                                          lua::colorTable(lua, screen.transition.color)));
            },
            "fun(): table", "Current screen effect values.");
}
} // namespace fe
