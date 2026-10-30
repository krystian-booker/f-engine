#include "engine/core/AssetCache.h"
#include "engine/input/SdlInput.h"
#include "engine/platform/Window.h"
#include "engine/runtime/ReplayHarness.h"
#include "engine/text/TextSystem.h"
#include <doctest/doctest.h>

using namespace fe;

namespace {
struct Fixture {
    AssetCache assets{FENGINE_TEST_GAME, FENGINE_LUALIB};
    TextSystem text{assets};
    Canvas canvas{text, 320, 180};
    InputState state;
    SdlInput input{state};

    void send(const SDL_Event& event, bool captured = false) {
        input.process(event, canvas, {1280, 720}, 1, captured);
    }
    void key(SDL_Scancode code, bool down, bool captured = false) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.scancode = code;
        event.key.down = down;
        send(event, captured);
    }
    void mouse(Uint8 button, bool down, bool captured = false) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.button = button;
        event.button.down = down;
        send(event, captured);
    }
    void pad(SDL_JoystickID id, SDL_GamepadButton button, bool down) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        event.gbutton.which = id;
        event.gbutton.button = static_cast<Uint8>(button);
        event.gbutton.down = down;
        send(event);
    }
    void device(Uint32 type, SDL_JoystickID id) {
        SDL_Event event{};
        event.type = type;
        event.gdevice.which = id;
        send(event);
    }
};
} // namespace

TEST_CASE("releases reach the game while a developer window has focus; presses don't") {
    Fixture f;
    f.state.define("confirm", {"key:Z"});
    f.key(SDL_SCANCODE_Z, true);
    f.state.tick(1.0 / 60);
    CHECK(f.state.held("confirm"));
    f.key(SDL_SCANCODE_Z, false, true);
    f.state.tick(1.0 / 60);
    CHECK_FALSE(f.state.held("confirm"));
    f.key(SDL_SCANCODE_Z, true, true);
    f.state.tick(1.0 / 60);
    CHECK_FALSE(f.state.held("confirm"));
    f.state.define("click", {"mouse:left"});
    f.mouse(SDL_BUTTON_LEFT, true);
    f.mouse(SDL_BUTTON_LEFT, false, true);
    f.state.tick(1.0 / 60); // a tap within one tick reads as pressed, then released
    CHECK(f.state.pressed("click"));
    f.state.tick(1.0 / 60);
    CHECK(f.state.released("click"));
    CHECK_FALSE(f.state.held("click"));
}

TEST_CASE("side mouse buttons have their own names") {
    Fixture f;
    f.mouse(SDL_BUTTON_X1, true);
    f.mouse(SDL_BUTTON_X2, true);
    f.state.tick(1.0 / 60);
    CHECK(f.state.raw("mouse:x1") == 1);
    CHECK(f.state.raw("mouse:x2") == 1);
    CHECK(f.state.raw("mouse:middle") == 0);
}

TEST_CASE("each gamepad has its own namespace and pad: combines them") {
    Fixture f;
    f.device(SDL_EVENT_GAMEPAD_ADDED, 11);
    f.device(SDL_EVENT_GAMEPAD_ADDED, 22);
    CHECK(f.state.lastDevice() == "keyboard"); // connecting a pad is not using it
    f.pad(11, SDL_GAMEPAD_BUTTON_SOUTH, true);
    f.pad(22, SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK(f.state.lastDevice() == "gamepad");
    f.state.tick(1.0 / 60);
    CHECK(f.state.raw("pad1:a") == 1);
    CHECK(f.state.raw("pad2:a") == 1);
    f.pad(11, SDL_GAMEPAD_BUTTON_SOUTH, false);
    f.state.tick(1.0 / 60);
    CHECK(f.state.raw("pad1:a") == 0);
    CHECK(f.state.raw("pad:a") == 1); // the second pad still holds it
    f.device(SDL_EVENT_GAMEPAD_REMOVED, 22);
    f.state.tick(1.0 / 60);
    CHECK(f.state.raw("pad:a") == 0);
    CHECK(f.state.raw("pad2:a") == 0);
    f.device(SDL_EVENT_GAMEPAD_ADDED, 33);
    f.pad(33, SDL_GAMEPAD_BUTTON_EAST, true);
    f.state.tick(1.0 / 60);
    CHECK(f.state.raw("pad2:b") == 1); // the free number is reused
}

TEST_CASE("focus loss releases every held input and the recorder sees it") {
    Fixture f;
    ReplayRecorder recorder(1);
    f.input.record(&recorder);
    f.key(SDL_SCANCODE_LEFT, true);
    f.pad(5, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    f.send(focus);
    CHECK(f.state.raw("key:Left") == 0);
    CHECK(f.state.raw("pad:a") == 0);
    recorder.stamp(1);
    const auto commands = recorder.toJson(1).at("commands");
    bool released = false;
    for (const auto& command : commands) {
        if (!command.contains("input")) {
            continue;
        }
        for (const auto& pair : command.at("input")) {
            released = released || (pair.at(0) == "key:Left" && pair.at(1) == 0);
        }
    }
    CHECK(released);
}

TEST_CASE("window activity follows focus and minimise events") {
    WindowActivity activity;
    CHECK(activity.foreground());
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    activity.handle(event);
    CHECK_FALSE(activity.foreground());
    event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    activity.handle(event);
    event.type = SDL_EVENT_WINDOW_MINIMIZED;
    activity.handle(event);
    CHECK_FALSE(activity.foreground());
    event.type = SDL_EVENT_WINDOW_RESTORED;
    activity.handle(event);
    CHECK(activity.foreground());
}

TEST_CASE("typed text and IME compositions reach InputState on the next tick") {
    Fixture f;
    f.state.beginText(std::nullopt, "text");
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = "é";
    f.send(event);
    f.send(event, true); // a developer window has focus
    event.type = SDL_EVENT_TEXT_EDITING;
    event.edit.text = "かな";
    event.edit.start = 2;
    f.send(event);
    CHECK(f.state.text().empty());
    f.state.tick(1.0 / 60);
    CHECK(f.state.text() == "é");
    CHECK(f.state.composition() == "かな");
    CHECK(f.state.compositionCursor() == 2);
}

TEST_CASE("a composition ends with focus loss and text entry, and the recorder sees text") {
    Fixture f;
    ReplayRecorder recorder(1);
    f.input.record(&recorder);
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_EDITING;
    event.edit.text = "ignored";
    f.send(event); // no text entry is active: the pre-edit belongs to a developer window
    f.state.tick(1.0 / 60);
    CHECK(f.state.composition().empty());

    f.state.beginText(std::nullopt, "name");
    event.edit.text = "かな";
    event.edit.start = 1;
    f.send(event);
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = "仮";
    f.send(event);
    recorder.stamp(1);
    f.state.tick(1.0 / 60);
    CHECK(f.state.text() == "仮");
    CHECK(f.state.composition() == "かな");

    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    f.send(focus);
    recorder.stamp(2);
    f.state.tick(1.0 / 60);
    CHECK(f.state.composition().empty());
    CHECK(f.state.compositionCursor() == 0);

    event.type = SDL_EVENT_TEXT_EDITING;
    event.edit.text = "new";
    f.send(event);
    f.state.endText();
    event.edit.text = "late"; // still queued when the game ended text entry
    f.send(event);
    f.state.tick(1.0 / 60);
    CHECK(f.state.composition().empty());

    const auto commands = recorder.toJson(2).at("commands");
    REQUIRE(commands.size() == 3);
    CHECK(commands[0] == Json{{"tick", 1}, {"text", "仮"}});
    CHECK(commands[1] == Json{{"tick", 1}, {"text", "かな"}, {"cursor", 1}});
    CHECK(commands[2] == Json{{"tick", 2}, {"text", ""}, {"cursor", 0}});
}

TEST_CASE("gamepad types map to prompt layouts") {
    CHECK(SdlInput::padType(SDL_GAMEPAD_TYPE_XBOXONE, 0x045e, 0x0b13, false) == "xbox");
    CHECK(SdlInput::padType(SDL_GAMEPAD_TYPE_XBOX360, 0x045e, 0x028e, false) == "xbox");
    CHECK(SdlInput::padType(SDL_GAMEPAD_TYPE_PS4, 0x054c, 0x09cc, false) == "playstation");
    CHECK(SdlInput::padType(SDL_GAMEPAD_TYPE_PS5, 0x054c, 0x0ce6, false) == "playstation");
    CHECK(SdlInput::padType(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, 0x057e, 0x2009, false) == "switch");
    CHECK(SdlInput::padType(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR, 0x057e, 0x2008, false) == "switch");
    CHECK(SdlInput::padType(SDL_GAMEPAD_TYPE_STANDARD, 0x28de, 0x1205, false) == "steamdeck");
    CHECK(SdlInput::padType(SDL_GAMEPAD_TYPE_XBOX360, 0x28de, 0x11ff, true) == "steamdeck");
    CHECK(SdlInput::padType(SDL_GAMEPAD_TYPE_XBOX360, 0x28de, 0x11ff, false) == "xbox");
    CHECK(SdlInput::padType(SDL_GAMEPAD_TYPE_UNKNOWN, 0x1234, 0x5678, false) == "generic");
}

namespace {
struct VirtualPad {
    SDL_JoystickID id = 0;
    Uint16 low = 0, high = 0, left = 0, right = 0;
    int rumbles = 0, triggerRumbles = 0;
    VirtualPad(Uint16 vendor, Uint16 product, const char* name) {
        REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = vendor;
        desc.product_id = product;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = name;
        desc.userdata = this;
        desc.Rumble = [](void* user, Uint16 l, Uint16 h) {
            auto* self = static_cast<VirtualPad*>(user);
            self->low = l;
            self->high = h;
            ++self->rumbles;
            return true;
        };
        desc.RumbleTriggers = [](void* user, Uint16 l, Uint16 r) {
            auto* self = static_cast<VirtualPad*>(user);
            self->left = l;
            self->right = r;
            ++self->triggerRumbles;
            return true;
        };
        id = SDL_AttachVirtualJoystick(&desc);
        REQUIRE(id != 0);
    }
    ~VirtualPad() {
        SDL_DetachVirtualJoystick(id);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
    VirtualPad(const VirtualPad&) = delete;
    VirtualPad& operator=(const VirtualPad&) = delete;
};
} // namespace

TEST_CASE("connected gamepads are described and rumble on request") {
    VirtualPad dualsense(0x054c, 0x0ce6, "Virtual DualSense");
    Fixture f;
    f.device(SDL_EVENT_GAMEPAD_ADDED, dualsense.id);
    REQUIRE(f.state.pads().contains(1));
    const auto& pad = f.state.pads().at(1);
    CHECK(pad.type == "playstation");
    CHECK(pad.rumble);
    CHECK(pad.triggerRumble);
    f.pad(dualsense.id, SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK(f.state.deviceType() == "playstation");

    f.state.rumble({0, 0.5f, 1.0f, 0.2, false});
    f.state.rumble({1, 1.0f, 0.0f, 0.2, true});
    f.state.rumble({2, 1.0f, 1.0f, 0.2, false}); // no second pad
    f.input.apply(nullptr, f.canvas, {1280, 720}, 1);
    CHECK(dualsense.rumbles == 1);
    CHECK(dualsense.low == 0x8000);
    CHECK(dualsense.high == 0xffff);
    CHECK(dualsense.triggerRumbles == 1);
    CHECK(dualsense.left == 0xffff);

    f.device(SDL_EVENT_GAMEPAD_REMOVED, dualsense.id);
    CHECK(f.state.pads().empty());
}

TEST_CASE("the Steam Deck's own controls use the steamdeck layout") {
    VirtualPad deck(0x28de, 0x1205, "Steam Deck");
    Fixture f;
    f.device(SDL_EVENT_GAMEPAD_ADDED, deck.id);
    REQUIRE(f.state.pads().contains(1));
    CHECK(f.state.pads().at(1).type == "steamdeck");
}
