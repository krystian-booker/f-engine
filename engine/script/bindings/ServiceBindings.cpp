#include "engine/core/Paths.h"
#include "engine/runtime/Engine.h"
#include "engine/script/bindings/Convert.h"
#include "engine/script/bindings/WorldTypes.h"

#include <algorithm>
#include <iterator>
#include <string_view>

namespace fe {
void bindSave(Engine& engine) {
    auto& host = engine.lua();
    auto& saves = engine.saves();
    ModuleBinder(
        host, engine.docs(), "fe.save",
        "Per-user persistence. Saves are JSON documents the game defines; the engine adds no schema.")
        .fn(
            "write",
            [&saves](const std::string& slot, sol::object data, sol::object meta) {
                saves.write(slot, luaToJson(data), lua::absent(meta) ? Json::object() : luaToJson(meta));
            },
            "fun(slot: string, data: table, meta?: table)",
            "Atomically write a save slot, keeping the previous version as its .bak. Raises (writing "
            "nothing) for data that could not be read back: tables nested deeper than 127 levels (the save "
            "file wraps them in one more) and the values fe.json.encode rejects.")
        .fn(
            "read",
            [&saves, &host](const std::string& slot) -> std::tuple<sol::object, sol::object, sol::object> {
                const auto loaded = saves.read(slot);
                if (!loaded) {
                    return {sol::lua_nil, sol::lua_nil, sol::lua_nil};
                }
                return {jsonToLua(host.lua(), loaded->data), jsonToLua(host.lua(), loaded->meta),
                        loaded->fromBackup ? sol::make_object(host.lua(), true) : sol::lua_nil};
            },
            "fun(slot: string): table?, table?, true?",
            "Read a slot's data and metadata (nil when empty). When the slot's file is missing or cut short "
            "(an interrupted write) the previous save kept as its .bak is read instead, a warning is logged "
            "and the third result is true. A slot file that is complete but unreadable (invalid JSON, "
            "nested too deep, not a save) raises an error instead of loading the previous save.")
        .fn(
            "list",
            [&saves, &host](sol::this_state state) {
                sol::state_view lua(state);
                auto out = lua.create_table();
                for (const auto& info : saves.list()) {
                    auto entry =
                        lua.create_table_with("slot", info.slot, "meta", jsonToLua(host.lua(), info.meta),
                                              "modified", info.modified, "thumbnail", info.thumbnail);
                    if (info.fromBackup) {
                        entry["backup"] = true;
                    }
                    out.add(entry);
                }
                return out;
            },
            "fun(): {slot: string, meta: table, modified: number, thumbnail: boolean, backup?: true}[]",
            "All saved slots. `backup` marks a slot whose file is missing or cut short, listed (and read) "
            "from its previous version; an unreadable slot has meta = {corrupt = true}.")
        .fn(
            "delete", [&saves](const std::string& slot) { saves.remove(slot); }, "fun(slot: string)",
            "Delete a slot, with its thumbnail and blobs.")
        .fn(
            "write_blob",
            [&saves](const std::string& slot, const std::string& name, const std::string& bytes) {
                saves.writeBlob(slot, name, bytes);
            },
            "fun(slot: string, name: string, bytes: string)",
            "Store bytes (any string, binary included) next to a slot under `name` (slot naming rules), "
            "through the save backend: world snapshots for a new game plus, replay inputs, anything that "
            "is not JSON. At most 16 MiB; the slot need not exist yet. Deleting the slot deletes its blobs.")
        .fn(
            "read_blob",
            [&saves](const std::string& slot, const std::string& name) -> sol::optional<std::string> {
                if (auto bytes = saves.readBlob(slot, name)) {
                    return *bytes;
                }
                return sol::nullopt;
            },
            "fun(slot: string, name: string): string?", "A slot's blob, or nil when there is none.")
        .fn(
            "remove_blob",
            [&saves](const std::string& slot, const std::string& name) { saves.removeBlob(slot, name); },
            "fun(slot: string, name: string)", "Delete one of a slot's blobs.")
        .fn(
            "blobs",
            [&saves](sol::this_state state, const std::string& slot) {
                sol::state_view lua(state);
                auto out = lua.create_table();
                for (const auto& name : saves.listBlobs(slot)) {
                    out.add(name);
                }
                return out;
            },
            "fun(slot: string): string[]", "Names of a slot's blobs, sorted.")
        .fn(
            "thumbnail",
            [&engine, &saves](const std::string& slot) {
                engine.requests().sceneCaptures.push_back(saves.thumbnailPath(slot));
            },
            "fun(slot: string)",
            "Capture the next frame's 3D scenes (no canvas UI, screen effects or editor) as the slot's "
            "thumbnail.")
        .fn(
            "thumbnail_path",
            [&saves](const std::string& slot) -> sol::optional<std::string> {
                if (auto uri = saves.thumbnailUri(slot)) {
                    return *uri;
                }
                return sol::nullopt;
            },
            "fun(slot: string): string?",
            "The slot's thumbnail as a 'user://thumbnails/<slot>.png' path for fe.canvas.image (nil when "
            "there is none).")
        .fn(
            "read_config", [&saves, &host] { return jsonToLua(host.lua(), saves.readConfig()); },
            "fun(): table", "Read the player's settings file.")
        .fn(
            "write_config", [&saves](sol::object data) { saves.writeConfig(luaToJson(data)); },
            "fun(settings: table)", "Write the player's settings file.");
}

namespace {
int padArgument(const sol::object& pad) {
    if (lua::absent(pad)) {
        return 0;
    }
    const auto number = lua::integer(pad, "A gamepad number");
    if (number < 1 || number > 64) {
        throw std::runtime_error("Gamepad numbers start at 1");
    }
    return static_cast<int>(number);
}

void rumble(InputState& input, float low, float high, double seconds, const sol::object& pad, bool triggers) {
    InputState::Rumble request;
    request.low = low;
    request.high = high;
    request.seconds = seconds;
    request.pad = padArgument(pad);
    request.triggers = triggers;
    input.rumble(request);
}

const InputState::Pad* findPad(const InputState& input, const sol::object& pad) {
    const auto found = input.pads().find(padArgument(pad));
    return found == input.pads().end() ? nullptr : &found->second;
}

const std::pair<const char*, const char*> TextKinds[] = {
    {"text", "text"}, {"name", "name"}, {"number", "number"}};
const std::pair<const char*, const char*> TransitionKinds[] = {
    {"none", "none"}, {"fade", "fade"}, {"swirl", "swirl"},    {"mosaic", "mosaic"},
    {"wipe", "wipe"}, {"iris", "iris"}, {"shatter", "shatter"}};

// Actions and axes are names the game defines; a misspelt one is an error,
// not an input that is never pressed.
const std::string& action(const InputState& input, const std::string& name) {
    if (!input.actions().contains(name)) {
        throw std::runtime_error("Unknown input action '" + name + "' (define it with fe.input.define)");
    }
    return name;
}
} // namespace

void bindInput(Engine& engine) {
    auto& host = engine.lua();
    auto& input = engine.input();
    ModuleBinder(
        host, engine.docs(), "fe.input",
        "Action-mapped input. Raw inputs are named 'key:<scancode name>' ('key:Z', 'key:Return'), "
        "'mouse:left', 'mouse:right', 'mouse:middle', 'mouse:x1', 'mouse:x2', 'pad:<control>' for any "
        "gamepad ('pad:a', 'pad:leftx') and 'pad1:<control>', 'pad2:<control>'... for one gamepad, "
        "numbered from 1 in connection order. Analog inputs take '+' or '-' for one direction "
        "('pad:leftx+'). The engine has no default bindings; scripts define every action. Text entry, "
        "gamepad descriptions and rumble are here too.")
        .fn(
            "define",
            [&input](const std::string& name, sol::object inputs, sol::object threshold) {
                input.define(name, lua::strings(lua::tableArg(inputs, "inputs")),
                             lua::within(lua::opt<float>(threshold, "threshold").value_or(0.5f), 0.0f, 1.0f,
                                         "threshold"));
            },
            "fun(action: string, inputs: string[], threshold?: number)", "Define or replace an action.")
        .fn(
            "define_axis",
            [&input](const std::string& name, sol::table spec) {
                InputState::Axis axis;
                axis.negative = lua::strings(lua::field(spec, "negative"));
                axis.positive = lua::strings(lua::field(spec, "positive"));
                axis.analog = lua::strings(lua::field(spec, "analog"));
                axis.deadzone = lua::within(lua::get(spec, "deadzone", 0.2f), 0.0f, 1.0f, "deadzone");
                input.defineAxis(name, std::move(axis));
            },
            "fun(name: string, spec: {negative?: string[], positive?: string[], analog?: string[], "
            "deadzone?: number})",
            "Define an axis from digital and analog raw inputs.")
        .fn(
            "clear", [&input] { input.clearDefinitions(); }, "fun()", "Remove all actions and axes.")
        .fn(
            "bind",
            [&input](const std::string& name, sol::object inputs) {
                input.bind(name, lua::strings(lua::tableArg(inputs, "inputs")));
            },
            "fun(action: string, inputs: string[])", "Rebind an action.")
        .fn(
            "bindings",
            [&input](const std::string& name) { return sol::as_table(input.bindings(action(input, name))); },
            "fun(action: string): string[]", "Current inputs of an action.")
        .fn(
            "pressed", [&input](const std::string& a) { return input.pressed(action(input, a)); },
            "fun(action: string): boolean", "Went down this tick.")
        .fn(
            "held", [&input](const std::string& a) { return input.held(action(input, a)); },
            "fun(action: string): boolean", "Currently down.")
        .fn(
            "released", [&input](const std::string& a) { return input.released(action(input, a)); },
            "fun(action: string): boolean", "Went up this tick.")
        .fn(
            "repeating",
            [&input](const std::string& a, sol::object delay, sol::object interval) {
                return input.repeat(
                    action(input, a),
                    lua::atLeast(lua::opt<double>(delay, "delay").value_or(0.35), 0.0, "delay"),
                    lua::atLeast(lua::opt<double>(interval, "interval").value_or(0.1), 0.001, "interval"));
            },
            "fun(action: string, delay?: number, interval?: number): boolean",
            "Pressed, then repeats while held (menu navigation).")
        .fn(
            "held_time", [&input](const std::string& a) { return input.heldTime(action(input, a)); },
            "fun(action: string): number", "Seconds the action has been held.")
        .fn(
            "axis",
            [&input](const std::string& name) {
                if (!input.hasAxis(name)) {
                    throw std::runtime_error("Unknown input axis '" + name +
                                             "' (define it with fe.input.define_axis)");
                }
                return input.axis(name);
            },
            "fun(axis: string): number", "Axis value -1..1.")
        .fn(
            "raw", [&input](const std::string& name) { return input.raw(name); },
            "fun(input: string): number", "Raw input value.")
        .fn(
            "pressed_inputs", [&input] { return sol::as_table(input.pressedInputs()); }, "fun(): string[]",
            "Raw inputs that went down this tick (for rebinding screens). Stick axes are left out.")
        .fn(
            "last_device", [&input] { return input.lastDevice(); }, "fun(): string",
            "'keyboard', 'gamepad' or 'mouse': the kind of device used last. A key or mouse button going "
            "down, "
            "the mouse travelling a little way, a gamepad button or a stick pushed past half way, and a "
            "gamepad "
            "being plugged in all count; releases, stick drift and a nudged mouse do not.")
        .fn(
            "device_changed", [&input] { return input.deviceChanged(); }, "fun(): boolean",
            "True on the tick fe.input.device_type() changed: the player touched the keyboard or mouse after "
            "a gamepad, pressed a button or moved a stick on a gamepad (or plugged one in) after the "
            "keyboard, "
            "picked up a gamepad of another layout, or unplugged the one in use. Redraw button prompts then.")
        .fn(
            "label", [&input](const std::string& name) { return input.label(name); },
            "fun(input: string): string",
            "What is printed on a raw input, for prompts and rebinding screens: a key's label under the "
            "player's keyboard layout ('key:Z' reads 'Y' on a German keyboard; the scancode name in headless "
            "runs and replays), otherwise the part after the colon ('pad:a' reads 'a').")
        .fn(
            "device_type",
            [&input](sol::object pad) -> sol::object {
                sol::state_view lua(pad.lua_state());
                if (lua::absent(pad)) {
                    return sol::make_object(lua, input.deviceType());
                }
                const auto* found = findPad(input, pad);
                return found ? sol::make_object(lua, found->type) : sol::make_object(lua, sol::lua_nil);
            },
            "fun(pad?: integer): 'keyboard'|'xbox'|'playstation'|'switch'|'steamdeck'|'generic'|nil",
            "Button layout for prompts: 'keyboard' while the keyboard or mouse was used last, otherwise the "
            "layout of the gamepad used last. With a gamepad number, that gamepad's layout (nil when it is "
            "not connected).")
        .fn(
            "device_name",
            [&input](sol::object pad) -> sol::object {
                sol::state_view lua(pad.lua_state());
                if (lua::absent(pad)) {
                    return sol::make_object(lua, input.deviceName());
                }
                const auto* found = findPad(input, pad);
                return found ? sol::make_object(lua, found->name) : sol::make_object(lua, sol::lua_nil);
            },
            "fun(pad?: integer): string?",
            "Model name of the gamepad used last, or of gamepad `pad` (nil when it is not connected).")
        .fn(
            "pads",
            [&input](sol::this_state state) {
                sol::state_view lua(state);
                auto out = lua.create_table();
                for (const auto& [number, pad] : input.pads()) {
                    out.add(lua.create_table_with("number", number, "name", pad.name, "type", pad.type,
                                                  "rumble", pad.rumble, "trigger_rumble", pad.triggerRumble));
                }
                return out;
            },
            "fun(): {number: integer, name: string, type: string, rumble: boolean, trigger_rumble: "
            "boolean}[]",
            "Connected gamepads by number, with their layout and whether they can rumble.")
        .fn(
            "pad_events",
            [&input](sol::this_state state) {
                sol::state_view lua(state);
                auto out = lua.create_table();
                for (const auto& event : input.padEvents()) {
                    out.add(lua.create_table_with("type", event.connected ? "connected" : "disconnected",
                                                  "pad", event.pad.number, "name", event.pad.name));
                }
                return out;
            },
            "fun(): {type: 'connected'|'disconnected', pad: integer, name: string}[]",
            "Gamepads connected or disconnected this tick, in order (fe.input.pads() already reflects "
            "them). A game that pauses when the player's pad is unplugged reads them every tick.")
        .fn(
            "rumble",
            [&input](float low, float high, double seconds, sol::object pad) {
                rumble(input, lua::within(low, 0.0f, 1.0f, "low"), lua::within(high, 0.0f, 1.0f, "high"),
                       lua::within(seconds, 0.0, 60.0, "seconds"), pad, false);
            },
            "fun(low: number, high: number, seconds: number, pad?: integer)",
            "Vibrate the low- and high-frequency motors (0..1) for up to 60 seconds; a new call replaces "
            "the previous one. Without a gamepad number, every gamepad. 0, 0 stops.")
        .fn(
            "rumble_triggers",
            [&input](float left, float right, double seconds, sol::object pad) {
                rumble(input, lua::within(left, 0.0f, 1.0f, "left"), lua::within(right, 0.0f, 1.0f, "right"),
                       lua::within(seconds, 0.0, 60.0, "seconds"), pad, true);
            },
            "fun(left: number, right: number, seconds: number, pad?: integer)",
            "Vibrate the trigger motors where a gamepad has them (see fe.input.pads).")
        .fn(
            "text_begin",
            [&input](sol::object rect, sol::object kind) {
                std::optional<glm::vec4> area;
                if (!lua::absent(rect)) {
                    area = lua::rect(rect);
                }
                const auto name = lua::opt<std::string>(kind, "Text kind").value_or("text");
                input.beginText(area, lua::choice(name, TextKinds, "text kind"));
            },
            "fun(rect?: Rect, kind?: 'text'|'name'|'number')",
            "Start text entry: typed text arrives through fe.input.text, the IME places its candidate window "
            "near `rect` (canvas pixels, the field being edited) and an on-screen keyboard opens where the "
            "system has one (Steam Deck, touch screens). Keys still report as raw inputs. Calling it again "
            "moves the area.")
        .fn(
            "text_end", [&input] { input.endText(); }, "fun()",
            "Stop text entry and close the IME or on-screen keyboard.")
        .fn(
            "text_active", [&input] { return input.textEntry().active; }, "fun(): boolean",
            "Text entry is on.")
        .fn(
            "text", [&input] { return input.text(); }, "fun(): string",
            "UTF-8 text committed this tick (typed characters, or an IME's finished composition).")
        .fn(
            "composition",
            [&input] { return std::make_tuple(input.composition(), input.compositionCursor()); },
            "fun(): string, integer",
            "The IME's unfinished text and the cursor position in it (in characters); '' when none. Draw it "
            "after the committed text.")
        .fn(
            "mouse", [&input] { return std::make_tuple(input.mouse().x, input.mouse().y); },
            "fun(): number, number", "Mouse position in canvas pixels.")
        .fn(
            "mouse_moved", [&input] { return input.mouseMoved(); }, "fun(): boolean",
            "Mouse moved this tick.")
        .fn("wheel", [&input] { return input.wheel(); }, "fun(): number", "Wheel movement this tick.");
    if (engine.devBindings()) {
        ModuleBinder(host, engine.docs(), "fe.input", "")
            .fn(
                "feed", [&input](const std::string& name, float value) { input.feed(name, value); },
                "fun(input: string, value: number)",
                "Inject a raw input; it applies on the next tick. Only in development builds, --test/--check "
                "runs and --replay/--record sessions.")
            .fn(
                "feed_text",
                [&input](const std::string& text, sol::object position) {
                    if (const auto cursor = lua::opt<int>(position, "cursor")) {
                        input.feedComposition(text, *cursor);
                    } else {
                        input.feedText(text);
                    }
                },
                "fun(text: string, cursor?: integer)",
                "Inject committed text, or with a cursor an IME composition; it applies on the next tick. "
                "Only in development builds, --test/--check runs and --replay/--record sessions.")
            .fn(
                "feed_pad",
                [&input](sol::object pad, sol::object spec) {
                    const int number = padArgument(pad);
                    if (number == 0) {
                        throw std::runtime_error("feed_pad needs a gamepad number");
                    }
                    if (lua::absent(spec)) {
                        input.disconnectPad(number);
                        return;
                    }
                    const auto options = lua::options(spec, "feed_pad spec");
                    InputState::Pad description;
                    description.number = number;
                    description.name = lua::get(*options, "name", std::string("Virtual"));
                    if (description.name.size() > 64) {
                        throw std::runtime_error("feed_pad name is at most 64 bytes");
                    }
                    description.type = lua::get(*options, "type", std::string("generic"));
                    static constexpr std::string_view Types[] = {"xbox", "playstation", "switch", "steamdeck",
                                                                 "generic"};
                    if (std::find(std::begin(Types), std::end(Types), description.type) == std::end(Types)) {
                        throw std::runtime_error(
                            "feed_pad type must be xbox, playstation, switch, steamdeck or "
                            "generic, got '" +
                            description.type + "'");
                    }
                    description.rumble = lua::get(*options, "rumble", false);
                    description.triggerRumble = lua::get(*options, "trigger_rumble", false);
                    input.connectPad(std::move(description));
                },
                "fun(pad: integer, spec?: {name?: string, type?: 'xbox'|'playstation'|'switch'|'steamdeck'|"
                "'generic', rumble?: boolean, trigger_rumble?: boolean})",
                "Connect a virtual gamepad numbered `pad` described by `spec` (name 'Virtual' of at most 64 "
                "bytes, type 'generic' and no rumble by default), or without a spec disconnect it; "
                "fe.input.pads() "
                "changes at once and the event arrives on the next tick. Only in development builds, "
                "--test/--check runs and --replay/--record sessions.");
    }
}

void bindAudio(Engine& engine) {
    auto& host = engine.lua();
    auto& audio = engine.audio();
    const auto root = host.gameRoot();
    ModuleBinder(host, engine.docs(), "fe.audio",
                 "Music layers with loop points, crossfades, seeking and pitch; sounds and voices; buses "
                 "with volume, low-pass, reverb and ducking; positional sounds heard from a world's camera.")
        .fn(
            "music",
            [&audio, root](const std::string& path, sol::object settings) {
                MusicOptions o;
                if (const auto options = lua::options(settings, "music options")) {
                    const auto& t = *options;
                    o.layer = lua::get(t, "layer", o.layer);
                    o.fade = lua::atLeast(lua::get(t, "fade", o.fade), 0.0, "fade");
                    o.loopStart = lua::get(t, "loop_start", o.loopStart);
                    o.loopEnd = lua::get(t, "loop_end", o.loopEnd);
                    o.volume = lua::within(lua::get(t, "volume", o.volume), 0.0f, 4.0f, "volume");
                    o.pitch = lua::within(lua::get(t, "pitch", o.pitch), 0.25f, 4.0f, "pitch");
                    o.preserveSpeed = lua::get(t, "preserve_speed", o.preserveSpeed);
                    o.loop = lua::get(t, "loop", o.loop);
                    o.sync = lua::get(t, "sync", o.sync);
                }
                audio.playMusic(resolveInside(root, path), o);
            },
            "fun(path: string, options?: {layer?: string, fade?: number, loop_start?: number, loop_end?: "
            "number, volume?: number, pitch?: number, preserve_speed?: boolean, loop?: boolean, sync?: "
            "boolean})",
            "Play music on a layer, crossfading from what was there. `sync` starts a stem at the main "
            "layer's position, sample-aligned. `pitch` (0.25-4) changes speed and pitch together, or with "
            "`preserve_speed` the pitch alone (the track keeps its duration; outside 0.5-2 it falls back to "
            "changing both); `volume` is 0 to 4.")
        .fn(
            "stop_music",
            [&audio](sol::object layer, sol::object fade) {
                audio.stopMusic(lua::opt<std::string>(layer, "layer").value_or(""),
                                lua::atLeast(lua::opt<double>(fade, "fade").value_or(0.5), 0.0, "fade"));
            },
            "fun(layer?: string, fade?: number)", "Fade out one layer, or all music.")
        .fn(
            "music_volume",
            [&audio](sol::this_state state, sol::object layer) {
                lua::atMostArguments(
                    state, 1, "fe.audio.music_volume(layer) only reads; use fe.audio.set_music_volume");
                return audio.musicVolume(lua::opt<std::string>(layer, "layer").value_or("main"));
            },
            "fun(layer?: string): number", "Volume of a music layer (default 'main').")
        .fn(
            "set_music_volume",
            [&audio](const std::string& layer, float volume, sol::object fade) {
                audio.setMusicVolume(layer, lua::within(volume, 0.0f, 4.0f, "volume"),
                                     lua::atLeast(lua::opt<double>(fade, "fade").value_or(0.5), 0.0, "fade"));
            },
            "fun(layer: string, volume: number, fade?: number)", "Mix a music layer (stems), 0 to 4.")
        .fn(
            "music_pitch",
            [&audio](sol::this_state state, sol::object layer) {
                lua::atMostArguments(state, 1,
                                     "fe.audio.music_pitch(layer) only reads; use fe.audio.set_music_pitch");
                return audio.musicPitch(lua::opt<std::string>(layer, "layer").value_or("main"));
            },
            "fun(layer?: string): number", "Speed and pitch of a music layer (default 'main').")
        .fn(
            "set_music_pitch",
            [&audio](const std::string& layer, float pitch, sol::object settings) {
                bool preserveSpeed = false;
                if (const auto options = lua::options(settings, "pitch options")) {
                    preserveSpeed = lua::get(*options, "preserve_speed", false);
                }
                audio.setMusicPitch(layer, lua::within(pitch, 0.25f, 4.0f, "pitch"), preserveSpeed);
            },
            "fun(layer: string, pitch: number, options?: {preserve_speed?: boolean})",
            "Speed and pitch of a layer, 0.25 to 4 (1 = normal); setting 'main' also sets its synced stems. "
            "With `preserve_speed` only the pitch changes and the track keeps its duration (0.5 to 2; "
            "outside that it changes both, logged once).")
        .fn(
            "current_music",
            [&audio](sol::object layer) {
                return audio.currentMusic(lua::opt<std::string>(layer, "layer").value_or("main"));
            },
            "fun(layer?: string): string", "Path playing on a layer.")
        .fn(
            "music_position",
            [&audio](sol::object layer) {
                return audio.musicPosition(lua::opt<std::string>(layer, "layer").value_or("main"));
            },
            "fun(layer?: string): number", "Playback position of a layer's track in seconds.")
        .fn(
            "seek_music",
            [&audio](const std::string& layer, double seconds) { audio.seekMusic(layer, seconds); },
            "fun(layer: string, seconds: number)",
            "Jump a layer's track to a position; seeking 'main' moves its synced stems with it.")
        .fn(
            "sound",
            [&audio, &engine, root](const std::string& path, sol::object settings) {
                SoundOptions o;
                if (const auto options = lua::options(settings, "sound options")) {
                    const auto& t = *options;
                    o.volume = lua::within(lua::get(t, "volume", o.volume), 0.0f, 4.0f, "volume");
                    o.pitch = lua::within(lua::get(t, "pitch", o.pitch), 0.1f, 4.0f, "pitch");
                    o.preserveSpeed = lua::get(t, "preserve_speed", o.preserveSpeed);
                    o.pan = lua::within(lua::get(t, "pan", o.pan), -1.0f, 1.0f, "pan");
                    o.loop = lua::get(t, "loop", o.loop);
                    o.bus = lua::get(t, "bus", o.bus);
                    o.radius = lua::get(t, "radius", o.radius);
                    if (o.radius <= 0) {
                        throw std::runtime_error("A sound's radius must be positive");
                    }
                    const sol::object position = lua::field(t, "position");
                    if (!lua::absent(position)) {
                        o.position = lua::position(position);
                        const sol::object world = lua::field(t, "world");
                        if (!lua::absent(world)) {
                            if (!world.is<WorldRef>()) {
                                throw std::runtime_error("Sound option 'world' must be a World");
                            }
                            o.listener = world.as<WorldRef&>().get().name();
                        } else if (const auto focus = engine.focusWorld()) {
                            o.listener = focus->name();
                        }
                    }
                }
                return audio.playSound(resolveInside(root, path), o);
            },
            "fun(path: string, options?: {volume?: number, pitch?: number, preserve_speed?: boolean, pan?: "
            "number, loop?: boolean, bus?: string, position?: Vec3, world?: World, radius?: number}): "
            "integer",
            "Play a sound; returns a voice id. With `position` it is heard from `world`'s camera (default "
            "the focus world) each tick: silent beyond `radius` (8) and panned by the camera's right vector. "
            "`volume` 0 to 4 (like music and buses), `pitch` 0.1 to 4 (speed and pitch together, or the "
            "pitch alone with `preserve_speed`, within 0.5 to 2), `pan` -1 to 1.")
        .fn(
            "stop",
            [&audio](int voice, sol::object fade) {
                audio.stopSound(voice, lua::atLeast(lua::opt<double>(fade, "fade").value_or(0), 0.0, "fade"));
            },
            "fun(voice: integer, fade?: number)", "Stop a voice.")
        .fn(
            "set_voice",
            [&audio](int voice, sol::table t) {
                const auto number = [&t](const char* key) {
                    return lua::opt<float>(lua::field(t, key), (std::string("Field '") + key + "'").c_str());
                };
                VoiceChange change{number("volume"), number("pan"), number("pitch"), std::nullopt,
                                   lua::opt<bool>(lua::field(t, "preserve_speed"), "Field 'preserve_speed'")};
                if (change.volume) {
                    change.volume = lua::within(*change.volume, 0.0f, 4.0f, "volume");
                }
                if (change.pan) {
                    change.pan = lua::within(*change.pan, -1.0f, 1.0f, "pan");
                }
                if (change.pitch) {
                    change.pitch = lua::within(*change.pitch, 0.1f, 4.0f, "pitch");
                }
                if (const sol::object position = lua::field(t, "position"); !lua::absent(position)) {
                    change.position = lua::position(position);
                }
                audio.setSound(voice, change);
            },
            "fun(voice: integer, change: {volume?: number, pan?: number, pitch?: number, preserve_speed?: "
            "boolean, position?: Vec3})",
            "Change a playing voice; omitted fields keep their value. Ranges as for fe.audio.sound: `volume` "
            "0 to 4, `pan` -1 to 1, `pitch` 0.1 to 4 (`preserve_speed` keeps the voice's duration under the "
            "pitch change, within 0.5 to 2).")
        .fn(
            "playing", [&audio](int voice) { return audio.playing(voice); }, "fun(voice: integer): boolean",
            "The voice has not finished or been stopped (paused voices still play). False without audio.")
        .fn(
            "position", [&audio](int voice) { return audio.position(voice); }, "fun(voice: integer): number",
            "Playback position of a voice in seconds.")
        .fn(
            "length",
            [&audio, root](const std::string& path) { return audio.length(resolveInside(root, path)); },
            "fun(path: string): number", "Duration of an audio file in seconds (0 without audio).")
        .fn(
            "bus_volume",
            [&audio](sol::this_state state, const std::string& bus) {
                lua::atMostArguments(state, 1,
                                     "fe.audio.bus_volume(bus) only reads; use fe.audio.set_bus_volume");
                return audio.busVolume(bus);
            },
            "fun(bus: string): number", "A bus volume ('master', 'music', 'sfx', 'voice', ...).")
        .fn(
            "set_bus_volume",
            [&audio](const std::string& bus, float volume) {
                audio.setBusVolume(bus, lua::within(volume, 0.0f, 4.0f, "volume"));
            },
            "fun(bus: string, volume: number)", "Set a bus volume, 0 to 4 (1 = unchanged).")
        .fn(
            "bus_effect",
            [&audio](sol::this_state state, const std::string& bus) {
                lua::atMostArguments(state, 1,
                                     "fe.audio.bus_effect(bus) only reads; use fe.audio.set_bus_effect");
                const auto effect = audio.busEffect(bus);
                return sol::state_view(state).create_table_with("lowpass", effect.lowpass, "reverb",
                                                                effect.reverb, "room", effect.room, "damping",
                                                                effect.damping);
            },
            "fun(bus: string): {lowpass: number, reverb: number, room: number, damping: number}",
            "A bus's effects (see fe.audio.set_bus_effect).")
        .fn(
            "set_bus_effect",
            [&audio](const std::string& bus, sol::table t) {
                BusEffect effect = audio.busEffect(bus);
                effect.lowpass = lua::atLeast(lua::get(t, "lowpass", effect.lowpass), 0.0f, "lowpass");
                effect.reverb = lua::within(lua::get(t, "reverb", effect.reverb), 0.0f, 1.0f, "reverb");
                effect.room = lua::within(lua::get(t, "room", effect.room), 0.0f, 1.0f, "room");
                effect.damping = lua::within(lua::get(t, "damping", effect.damping), 0.0f, 1.0f, "damping");
                effect.fade = lua::atLeast(lua::get(t, "fade", 0.0), 0.0, "fade");
                audio.setBusEffect(bus, effect);
            },
            "fun(bus: string, settings: {lowpass?: number, reverb?: number, room?: number, damping?: number, "
            "fade?: number})",
            "Change a bus's effects; omitted fields keep their value. `lowpass` is a cutoff in Hz (0 = off), "
            "`reverb` the wet mix 0-1, `room` and `damping` 0-1. Changes glide over `fade` seconds.")
        .fn(
            "dynamics",
            [&audio](sol::this_state state) {
                lua::atMostArguments(state, 0, "fe.audio.dynamics() only reads; use fe.audio.set_dynamics");
                const auto d = audio.dynamics();
                return sol::state_view(state).create_table_with("night", d.compress, "threshold", d.threshold,
                                                                "ratio", d.ratio, "makeup", d.makeup);
            },
            "fun(): {night: boolean, threshold: number, ratio: number, makeup: number}",
            "The master bus compression (see fe.audio.set_dynamics).")
        .fn(
            "set_dynamics",
            [&audio](sol::table t) {
                Dynamics d = audio.dynamics();
                d.compress = lua::get(t, "night", d.compress);
                d.threshold = lua::within(lua::get(t, "threshold", d.threshold), -60.0f, 0.0f, "threshold");
                d.ratio = lua::within(lua::get(t, "ratio", d.ratio), 1.0f, 20.0f, "ratio");
                d.makeup = lua::within(lua::get(t, "makeup", d.makeup), 0.0f, 24.0f, "makeup");
                audio.setDynamics(d);
            },
            "fun(settings: {night?: boolean, threshold?: number, ratio?: number, makeup?: number})",
            "Night listening: `night = true` compresses the master bus (above `threshold` dBFS, -60..0, the "
            "level rises 1/`ratio` as fast, 1..20; `makeup` dB, 0..24, is added back), so speech stays clear "
            "while peaks come down; the limiter still follows. Omitted fields keep their value.")
        .fn(
            "duck",
            [&audio](const std::string& bus, float amount, sol::object settings) {
                Ducking ducking;
                ducking.amount = lua::within(amount, 0.0f, 1.0f, "amount");
                if (const auto options = lua::options(settings, "duck options")) {
                    const auto& t = *options;
                    ducking.key = lua::get(t, "key", ducking.key);
                    ducking.attack = lua::atLeast(lua::get(t, "attack", ducking.attack), 0.0, "attack");
                    ducking.release = lua::atLeast(lua::get(t, "release", ducking.release), 0.0, "release");
                }
                audio.setDucking(bus, ducking);
            },
            "fun(bus: string, amount: number, options?: {key?: string, attack?: number, release?: number})",
            "Lower a bus by `amount` (0-1) while the `key` bus ('voice') is audible, gliding down over "
            "`attack` (0.08) and back over `release` (0.5) seconds. Amount 0 turns ducking off.")
        .fn(
            "paused", [&audio] { return audio.soundsPaused(); }, "fun(): boolean",
            "Whether the global pause is on (fe.audio.set_paused).")
        .fn(
            "set_paused", [&audio](bool paused) { audio.pauseSounds(paused); }, "fun(paused: boolean)",
            "The global pause: pausing holds everything playing now, sounds started while paused play "
            "(menu sounds), and resuming continues the held ones where they were.")
        .fn(
            "available", [&audio] { return audio.available(); }, "fun(): boolean",
            "An audio device is open.");
}

void bindScreen(Engine& engine) {
    auto& host = engine.lua();
    auto& screen = engine.screen();
    ModuleBinder(host, engine.docs(), "fe.screen",
                 "Full-screen effects. Scripts animate these values; renderers apply them.")
        .fn(
            "set",
            [&screen](sol::table t) {
                screen.fade = lua::color(lua::field(t, "fade"), screen.fade);
                screen.flash = lua::color(lua::field(t, "flash"), screen.flash);
                screen.tint = lua::color(lua::field(t, "tint"), screen.tint);
                screen.letterbox = lua::get(t, "letterbox", screen.letterbox);
                screen.letterboxHeight = lua::get(t, "letterbox_height", screen.letterboxHeight);
                screen.radialBlur = std::clamp(lua::get(t, "radial_blur", screen.radialBlur), 0.0f, 1.0f);
                screen.radialCentre = lua::vec2(lua::field(t, "radial_centre"), screen.radialCentre);
                screen.chromatic = std::clamp(lua::get(t, "chromatic", screen.chromatic), 0.0f, 1.0f);
                if (const auto transition = lua::field(t, "transition"); !lua::absent(transition)) {
                    const auto tr = lua::vectorTable(transition, "a transition");
                    screen.transition.kind = lua::choice(lua::get(tr, "kind", screen.transition.kind),
                                                         TransitionKinds, "transition kind");
                    screen.transition.progress = lua::get(tr, "progress", screen.transition.progress);
                    screen.transition.centre = lua::vec2(lua::field(tr, "centre"), screen.transition.centre);
                    screen.transition.color = lua::color(lua::field(tr, "color"), screen.transition.color);
                }
            },
            "fun(settings: {fade?: Color, flash?: Color, tint?: Color, letterbox?: number, "
            "letterbox_height?: number, radial_blur?: number, radial_centre?: {x: number, y: number}, "
            "chromatic?: "
            "number, transition?: {kind?: 'none'|'fade'|'swirl'|'mosaic'|'wipe'|'iris'|'shatter', progress?: "
            "number, centre?: {x: number, y: number}, color?: Color}})",
            "Change screen effects. Colors take any color form; the alpha channel is the effect's amount, "
            "and the {r, g, b, a} tables fe.screen.get returns are accepted as they are. `radial_blur` (held "
            "within 0..1) "
            "streaks the screen away from `radial_centre` ({x, y}, 0..1 with y down); `chromatic` "
            "(held within 0..1) splits colour towards the edges. Transition kinds: none, fade, swirl, "
            "mosaic, wipe, iris, "
            "shatter. While frozen (fe.screen.freeze) a transition reveals the live frame instead of "
            "`color`.")
        .fn(
            "freeze",
            [&screen] {
                screen.frozen = true;
                ++screen.freezeSerial;
            },
            "fun()",
            "Hold the frame now on screen (worlds and canvas, before screen effects) and show it instead of "
            "the "
            "live frame, which keeps rendering underneath. Transitions then go from the held frame to the "
            "live "
            "one; radial blur, chromatic aberration, fade and flash still apply. Calling it again "
            "recaptures.")
        .fn(
            "unfreeze", [&screen] { screen.frozen = false; }, "fun()",
            "Release the held frame and show the live one again.")
        .fn(
            "frozen", [&screen] { return screen.frozen; }, "fun(): boolean",
            "A frame is held (fe.screen.freeze).")
        .fn(
            "get",
            [&screen](sol::this_state state) {
                sol::state_view lua(state);
                return lua.create_table_with(
                    "fade", lua::colorTable(lua, screen.fade), "flash", lua::colorTable(lua, screen.flash),
                    "tint", lua::colorTable(lua, screen.tint), "letterbox", screen.letterbox,
                    "letterbox_height", screen.letterboxHeight, "radial_blur", screen.radialBlur,
                    "radial_centre", lua::table(lua, screen.radialCentre), "chromatic", screen.chromatic,
                    "transition",
                    lua.create_table_with("kind", screen.transition.kind, "progress",
                                          screen.transition.progress, "centre",
                                          lua::table(lua, screen.transition.centre), "color",
                                          lua::colorTable(lua, screen.transition.color)));
            },
            "fun(): table", "Current screen effect values.");
}
} // namespace fe
