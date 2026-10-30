#include "engine/core/Log.h"
#include "engine/editor/Editor.h"
#include "engine/editor/EditorNative.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <cstdlib>
#include <imgui.h>
#include <map>

namespace fe {
using namespace editorui;

// A scripted walkthrough of the editor, for screenshots and checks that
// replays cannot make (replays feed the game's input, not ImGui's). The
// file FENGINE_EDITOR_SCRIPT names is a JSON list of steps, each with a
// `frame` (editor frames since it opened) and one of:
//   {"mouse": [x, y]}        move the mouse (window points); the real cursor is warped there
//   {"down": "left"} / {"up": "left"} / {"click": "left"}  (also "right", "middle"; "ctrl": true holds Ctrl)
//   {"wheel": 3}             mouse wheel notches (negative scrolls down)
//   {"expect_selection_count": 2}  how many things are selected (Ctrl+click adds)
//   {"import": ["/abs/file.png"], "purpose": "assets|geometry|new_map", "folder": "sprites"}
//                            what the OS file dialog would return (the dialog itself is SDL's)
//   {"drop": "/abs/file.png", "at": [x, y]}  an OS drag-and-drop onto the window
//   {"open_dialog": "assets|geometry"}  open the real OS file dialog (SDL's, a sheet on the window)
//   {"cancel_dialog": true}  dismiss it as Cancel would (macOS only; fails elsewhere)
//   {"expect_dialog_returned": 1}  how many times the dialog's callback has fired
//   {"expect_missing_count": 0}  references in maps and tables that no file supplies
//   {"key": "F5"} with optional "ctrl", "shift", "alt", "super": press and release a key
//                            (on macOS ImGui maps Ctrl chords to Cmd: send "super" for Ctrl+Z)
//   {"text": "abc"}          type characters into the focused field
//   {"capture": "file.png"}  screenshot after this frame
//   {"expect": "lua"}        a Lua expression that must be true (fe.dev console)
//   {"expect_selection": "elder"}  the selection's label must contain the text
//   {"expect_status": "Saved"}     the status line must contain the text
//   {"expect_mode": "play"}        "play", "edit" or "closed" (the editor is not open)
//   {"toggle_editor": true}  open or close the editor, as F1 does
//   {"quit": true}           end the run (after logging the tally)
// Results go to the log as "editor script: ...".
namespace {
std::map<std::string, ImGuiKey> keyNames() {
    std::map<std::string, ImGuiKey> keys = {
        {"F1", ImGuiKey_F1},         {"F2", ImGuiKey_F2},     {"F3", ImGuiKey_F3},       {"F4", ImGuiKey_F4},
        {"F5", ImGuiKey_F5},         {"F6", ImGuiKey_F6},     {"Delete", ImGuiKey_Delete}, {"Backspace", ImGuiKey_Backspace},
        {"Escape", ImGuiKey_Escape}, {"Enter", ImGuiKey_Enter}, {"Home", ImGuiKey_Home}, {"Tab", ImGuiKey_Tab},
        {"Space", ImGuiKey_Space},   {"Up", ImGuiKey_UpArrow}, {"Down", ImGuiKey_DownArrow},
        {"Left", ImGuiKey_LeftArrow}, {"Right", ImGuiKey_RightArrow}};
    for (char c = 'A'; c <= 'Z'; ++c) {
        keys[std::string(1, c)] = static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
    }
    for (char c = '0'; c <= '9'; ++c) {
        keys[std::string(1, c)] = static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
    }
    return keys;
}
int buttonOf(const std::string& name) {
    return name == "right" ? 1 : name == "middle" ? 2 : 0;
}
} // namespace

void Editor::loadScript() {
    const char* file = std::getenv("FENGINE_EDITOR_SCRIPT");
    if (!file || !*file) {
        return;
    }
    try {
        script_ = readJson(file);
        if (!script_.is_array()) {
            throw std::runtime_error("the script must be a JSON list of steps");
        }
        log(LogLevel::Info, "editor script: " + std::to_string(script_.size()) + " step(s) from " + file);
    } catch (const std::exception& error) {
        log(LogLevel::Error, std::string("editor script: ") + error.what());
        script_ = Json::array();
    }
}

void Editor::runScript() {
    if (script_.empty()) {
        return;
    }
    auto& io = ImGui::GetIO();
    for (const auto& [key, down] : scriptKeyReleases_) {
        io.AddKeyEvent(static_cast<ImGuiKey>(key), down);
    }
    scriptKeyReleases_.clear();
    for (const int key : scriptLateReleases_) {
        scriptKeyReleases_.push_back({key, false}); // next frame
    }
    scriptLateReleases_.clear();
    // Keep the scripted mouse where it was put: the backend warps the real
    // cursor to io.MousePos when WantSetMousePos is on, and follows it.
    if (scriptMouse_) {
        io.WantSetMousePos = true;
        io.MousePos = ImVec2(scriptMouse_->x, scriptMouse_->y);
    }
    const auto frame = scriptFrame_++;
    const auto check = [&](bool ok, const std::string& what) {
        (ok ? scriptPassed_ : scriptFailed_)++;
        log(ok ? LogLevel::Info : LogLevel::Error,
            std::string("editor script: ") + (ok ? "PASS " : "FAIL ") + what + " (frame " + std::to_string(frame) + ")");
    };
    static const auto keys = keyNames();
    while (scriptStep_ < script_.size()) {
        const auto& step = script_[scriptStep_];
        if (!step.is_object() || jsonNumber(step, "frame", 0) > static_cast<double>(frame)) {
            break;
        }
        ++scriptStep_;
        if (const auto mouse = step.find("mouse"); mouse != step.end() && mouse->is_array() && mouse->size() == 2) {
            scriptMouse_ = glm::vec2((*mouse)[0].get<float>(), (*mouse)[1].get<float>());
            io.AddMousePosEvent(scriptMouse_->x, scriptMouse_->y);
            io.WantSetMousePos = true;
            io.MousePos = ImVec2(scriptMouse_->x, scriptMouse_->y);
        }
        // Modifiers held around a mouse action ("ctrl": true on a click).
        const bool holdCtrl = (step.contains("click") || step.contains("down")) && jsonBool(step, "ctrl", false);
        const bool holdSuper = (step.contains("click") || step.contains("down")) && jsonBool(step, "super", false);
        if (holdCtrl) {
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
        }
        if (holdSuper) {
            io.AddKeyEvent(ImGuiMod_Super, true);
        }
        if (step.contains("down")) {
            io.AddMouseButtonEvent(buttonOf(jsonString(step, "down", "left")), true);
        }
        if (step.contains("up")) {
            io.AddMouseButtonEvent(buttonOf(jsonString(step, "up", "left")), false);
            if (jsonBool(step, "ctrl", false)) {
                scriptKeyReleases_.push_back({static_cast<int>(ImGuiMod_Ctrl), false});
            }
        }
        if (step.contains("click")) {
            const int button = buttonOf(jsonString(step, "click", "left"));
            io.AddMouseButtonEvent(button, true);
            // Released on the next frame, so ImGui sees a press and a release.
            scriptKeyReleases_.push_back({-1 - button, false});
            // Modifiers go up a frame after the button, so the release that
            // completes the click still sees them held.
            if (holdCtrl) {
                scriptLateReleases_.push_back(static_cast<int>(ImGuiMod_Ctrl));
            }
            if (holdSuper) {
                scriptLateReleases_.push_back(static_cast<int>(ImGuiMod_Super));
            }
        }
        if (const auto wheel = step.find("wheel"); wheel != step.end() && wheel->is_number()) {
            io.AddMouseWheelEvent(0, wheel->get<float>());
        }
        // What the OS file dialog would have returned: files for the import
        // queue ("purpose": "assets" (default), "geometry" or "new_map").
        if (const auto files = step.find("import"); files != step.end() && files->is_array()) {
            std::lock_guard lock(importQueue_->mutex);
            for (const auto& file : *files) {
                if (file.is_string()) {
                    importQueue_->files.emplace_back(file.get<std::string>());
                }
            }
            const auto purpose = jsonString(step, "purpose", "assets");
            importQueue_->purpose = purpose == "geometry"  ? ImportQueue::Purpose::Geometry
                                    : purpose == "new_map" ? ImportQueue::Purpose::NewMapGeometry
                                                           : ImportQueue::Purpose::Assets;
            importQueue_->folder = jsonString(step, "folder", assetFolder_);
        }
        // A file dropped from the OS at a window point.
        if (const auto drop = step.find("drop"); drop != step.end() && drop->is_string()) {
            const auto at = step.find("at");
            const glm::vec2 point = at != step.end() && at->is_array() && at->size() == 2
                                        ? glm::vec2((*at)[0].get<float>(), (*at)[1].get<float>())
                                        : (scriptMouse_ ? *scriptMouse_ : glm::vec2(0));
            dropFile(drop->get<std::string>(), point);
        }
        // The real OS file dialog: opened here, dismissed from outside the
        // process (a keystroke), and its callback's outcome checked.
        if (const auto purpose = jsonString(step, "open_dialog", {}); !purpose.empty()) {
            openImportDialog(purpose == "geometry" ? ImportQueue::Purpose::Geometry : ImportQueue::Purpose::Assets);
        }
        if (jsonBool(step, "cancel_dialog", false)) {
            check(cancelNativeFileDialog(window_), "cancelled the OS file dialog");
        }
        if (const auto count = step.find("expect_dialog_returned"); count != step.end() && count->is_number()) {
            std::lock_guard lock(importQueue_->mutex);
            check(importQueue_->returned == count->get<int>(),
                  "file dialog returned " + std::to_string(importQueue_->returned) + " time(s) (" +
                      std::to_string(importQueue_->cancelled) + " cancelled, " + std::to_string(importQueue_->failed) +
                      " failed), expected " + std::to_string(count->get<int>()));
        }
        if (const auto count = step.find("expect_missing_count"); count != step.end() && count->is_number()) {
            catalog_.scan();
            const auto have = static_cast<int>(catalog_.missing().size());
            check(have == count->get<int>(), std::to_string(have) + " missing reference(s), expected " + std::to_string(count->get<int>()));
        }
        if (const auto count = step.find("expect_selection_count"); count != step.end() && count->is_number()) {
            const auto have = static_cast<int>(extra_.size()) + (selection_.empty() ? 0 : 1);
            check(have == count->get<int>(), std::to_string(have) + " selected, expected " + std::to_string(count->get<int>()));
        }
        if (const auto name = jsonString(step, "key", {}); !name.empty()) {
            const auto found = keys.find(name);
            if (found == keys.end()) {
                check(false, "unknown key " + name);
            } else {
                const bool ctrl = jsonBool(step, "ctrl", false), shift = jsonBool(step, "shift", false),
                           alt = jsonBool(step, "alt", false), super = jsonBool(step, "super", false);
                if (super) {
                    io.AddKeyEvent(ImGuiMod_Super, true);
                    scriptKeyReleases_.push_back({static_cast<int>(ImGuiMod_Super), false});
                }
                if (ctrl) {
                    io.AddKeyEvent(ImGuiMod_Ctrl, true);
                }
                if (shift) {
                    io.AddKeyEvent(ImGuiMod_Shift, true);
                }
                if (alt) {
                    io.AddKeyEvent(ImGuiMod_Alt, true);
                }
                io.AddKeyEvent(found->second, true);
                scriptKeyReleases_.push_back({static_cast<int>(found->second), false});
                if (ctrl) {
                    scriptKeyReleases_.push_back({static_cast<int>(ImGuiMod_Ctrl), false});
                }
                if (shift) {
                    scriptKeyReleases_.push_back({static_cast<int>(ImGuiMod_Shift), false});
                }
                if (alt) {
                    scriptKeyReleases_.push_back({static_cast<int>(ImGuiMod_Alt), false});
                }
            }
        }
        if (const auto text = jsonString(step, "text", {}); !text.empty()) {
            io.AddInputCharactersUTF8(text.c_str());
        }
        if (const auto file = jsonString(step, "capture", {}); !file.empty()) {
            engine_.requests().captures.emplace_back(file);
        }
        if (const auto code = jsonString(step, "expect", {}); !code.empty()) {
            const auto result = engine_.lua().evaluate("return " + code);
            const bool ok = result.ok && (result.output == "true");
            check(ok, "expect " + code + (ok ? "" : " -> " + result.output));
        }
        if (const auto text = jsonString(step, "expect_selection", {}); !text.empty()) {
            const auto w = world();
            const auto label = w ? selectionLabel(*w, selection_) : std::string("(no world)");
            check(label.find(text) != std::string::npos, "selection '" + label + "' contains '" + text + "'");
        }
        if (const auto text = jsonString(step, "expect_status", {}); !text.empty()) {
            check(status_.find(text) != std::string::npos, "status '" + status_ + "' contains '" + text + "'");
        }
        if (const auto text = jsonString(step, "expect_mode", {}); !text.empty()) {
            const auto mode = !open_ ? "closed" : mode_ == Mode::Play ? "play" : "edit";
            check(mode == text, std::string("mode is ") + mode + ", expected " + text);
        }
        if (jsonBool(step, "toggle_editor", false)) {
            toggle();
        }
        if (jsonBool(step, "quit", false) || scriptStep_ == script_.size()) {
            log(scriptFailed_ ? LogLevel::Error : LogLevel::Info,
                "editor script: " + std::to_string(scriptPassed_) + " passed, " + std::to_string(scriptFailed_) +
                    " failed");
            if (jsonBool(step, "quit", false)) {
                engine_.requests().quit = true;
            }
        }
    }
    // Mouse button releases queued by "click" ride the key release list with negative ids.
    for (auto& [key, down] : scriptKeyReleases_) {
        if (key < 0) {
            io.AddMouseButtonEvent(-1 - key, false);
            key = static_cast<int>(ImGuiKey_None);
        }
    }
    std::erase_if(scriptKeyReleases_, [](const auto& entry) { return entry.first == static_cast<int>(ImGuiKey_None); });
}
} // namespace fe
