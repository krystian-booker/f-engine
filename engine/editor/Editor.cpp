#include "engine/editor/Editor.h"
#include "engine/core/Paths.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

Editor::Editor(Engine& engine) : engine_(engine) {
}

void Editor::toggle() {
    open_ = !open_;
    if (open_) {
        refreshSchemas();
    } else {
        close();
    }
}

void Editor::close() {
    for (const auto& entry : engine_.worlds()) {
        auto& w = *entry.second;
        std::erase_if(w.overlays(), [](const auto& overlay) { return overlay.first.starts_with("editor:"); });
        if (cursorShown_) {
            w.cursor().visible = false;
        }
    }
    cursorShown_ = false;
    gridOverlayKey_.clear();
    zoneOverlayKey_.clear();
}

bool Editor::capturesInput() const {
    if (!open_) {
        return false;
    }
    const auto& io = ImGui::GetIO();
    return io.WantCaptureKeyboard || io.WantCaptureMouse;
}

std::shared_ptr<World> Editor::world() {
    if (auto w = engine_.world(worldName_)) {
        return w;
    }
    auto focus = engine_.focusWorld();
    if (focus) {
        worldName_ = focus->name();
    }
    return focus;
}

sol::object Editor::devtool(const char* name) {
    try {
        const auto module = engine_.lua().require("fe.devtools");
        if (module.get_type() == sol::type::table) {
            sol::object fn = module.as<sol::table>()[name];
            if (fn.get_type() == sol::type::function) {
                return fn;
            }
        }
    } catch (const std::exception& error) {
        status_ = error.what();
    }
    return sol::lua_nil;
}

bool Editor::succeeded(const sol::protected_function_result& result) {
    if (result.valid()) {
        return true;
    }
    const sol::error error = result;
    status_ = error.what();
    return false;
}

void Editor::refreshSchemas() {
    schemasFetched_ = Clock::now();
    schemas_ = Json::object();
    const auto describe = devtool("describe");
    if (describe == sol::lua_nil) {
        return;
    }
    const auto result = engine_.lua().invoke(describe);
    if (succeeded(result) && result.get_type() == sol::type::string) {
        const auto parsed = Json::parse(result.get<std::string>(), nullptr, false);
        schemas_ = parsed.is_object() ? parsed : Json::object();
    }
}

std::string Editor::defaultType(const char* group) const {
    const auto types = jsonObject(schemas_, group);
    const auto preferred = jsonString(jsonObject(schemas_, "defaults"), group, {});
    if (!preferred.empty()) {
        return preferred;
    }
    return types.empty() ? std::string{} : types.begin().key();
}

const Json* Editor::fieldsFor(const std::string& type, std::initializer_list<const char*> groups) const {
    for (const auto* group : groups) {
        const auto found = schemas_.find(group);
        if (found != schemas_.end() && found->contains(type)) {
            const auto& spec = found->at(type);
            const auto fields = spec.find("fields");
            return fields == spec.end() ? nullptr : &*fields;
        }
    }
    return nullptr;
}

void Editor::edit(World& w, const std::function<void(MapData&)>& change) {
    history_.apply(w, change, !gesture_);
    gesture_ = true;
    ++revision_;
}

void Editor::undo(World& w, bool forward) {
    try {
        if (forward ? history_.redo(w) : history_.undo(w)) {
            ++revision_;
            status_ = forward ? "Redone" : "Undone";
        }
    } catch (const std::exception& error) {
        status_ = std::string("Cannot restore: ") + error.what();
    }
    gesture_ = false;
}

void Editor::saveMap(World& w) {
    try {
        const auto json = w.authored().toJson();
        MapData::fromJson(json); // validate before writing
        writeJson(resolveInside(engine_.lua().gameRoot(), "maps/" + w.authored().id + "/map.json"), json);
        status_ = "Saved maps/" + w.authored().id + "/map.json";
    } catch (const std::exception& error) {
        status_ = std::string("Not saved: ") + error.what();
    }
}

bool Editor::editJson(const char* label, Json& value, float height) {
    // While the field is active ImGui keeps the typed text; invalid JSON is
    // not applied, and leaving the field shows the last valid value again.
    const auto id = ImGui::GetID(label);
    auto* invalid = ImGui::GetStateStorage();
    std::string text = value.dump(2);
    bool changed = false;
    if (ImGui::InputTextMultiline(label, &text, ImVec2(-1, height))) {
        auto parsed = Json::parse(text, nullptr, false);
        invalid->SetBool(id, parsed.is_discarded());
        if (!parsed.is_discarded()) {
            value = std::move(parsed);
            changed = true;
        }
    }
    if (!ImGui::IsItemActive()) {
        invalid->SetBool(id, false);
    } else if (invalid->GetBool(id)) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Invalid JSON (not applied)");
    }
    return changed;
}

bool Editor::editProps(const Json* schema, Json& props) {
    if (!schema || !schema->is_object()) {
        return editJson("Props", props);
    }
    const auto& fields = *schema;
    if (!props.is_object()) {
        return editJson("Props", props);
    }
    bool changed = false;
    for (const auto& [name, spec] : fields.items()) {
        if (!spec.is_object()) {
            continue;
        }
        const auto type = jsonString(spec, "type", "any");
        const bool optional = jsonBool(spec, "optional", false);
        const auto label = name + (optional ? "" : " *");
        // A value whose type does not match its schema (a number where the
        // schema says string, an edited file) is shown in the JSON editor
        // instead of aborting the form: json.value() would throw on it.
        const Json* current = props.contains(name) ? &props.at(name) : nullptr;
        const auto typed = [&](bool (Json::*check)() const) { return !current || (*current.*check)(); };
        ImGui::PushID(name.c_str());
        if ((type == "string" || type == "ref") && typed(&Json::is_string)) {
            std::string value = current ? current->get<std::string>() : std::string{};
            const auto hint = type == "ref" ? label + " (" + jsonString(spec, "table", {}) + " id)" : label;
            if (ImGui::InputText(hint.c_str(), &value)) {
                props[name] = value;
                changed = true;
            }
        } else if ((type == "number" || type == "integer") && typed(&Json::is_number)) {
            double value = current ? current->get<double>() : jsonNumber(spec, "default", 0.0);
            if (ImGui::InputDouble(label.c_str(), &value)) {
                props[name] = type == "integer" ? Json(static_cast<long long>(value)) : Json(value);
                changed = true;
            }
        } else if (type == "boolean" && typed(&Json::is_boolean)) {
            bool value = current ? current->get<bool>() : jsonBool(spec, "default", false);
            if (ImGui::Checkbox(label.c_str(), &value)) {
                props[name] = value;
                changed = true;
            }
        } else if (type == "enum" && typed(&Json::is_string)) {
            const std::string value = current ? current->get<std::string>() : jsonString(spec, "default", {});
            if (ImGui::BeginCombo(label.c_str(), value.c_str())) {
                if (const auto options = spec.find("options"); options != spec.end() && options->is_array()) {
                    for (const auto& option : *options) {
                        if (!option.is_string()) {
                            continue;
                        }
                        const auto text = option.get<std::string>();
                        if (ImGui::Selectable(text.c_str(), text == value)) {
                            props[name] = text;
                            changed = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            Json value = current ? *current : Json();
            ImGui::TextUnformatted(label.c_str());
            if (editJson("##json", value, 80)) {
                props[name] = value;
                changed = true;
            }
        }
        if (const auto doc = spec.find("doc"); doc != spec.end() && doc->is_string() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", doc->get<std::string>().c_str());
        }
        if (optional && props.contains(name)) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                props.erase(name);
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove (use the default)");
            }
        }
        ImGui::PopID();
    }
    return changed;
}

void Editor::drawErrors() {
    const auto& errors = engine_.lua().errors();
    if (errors.empty() || errors.size() == errorsSeen_) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620, 220), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Script error")) {
        ImGui::TextWrapped("%s", errors.back().message.c_str());
        if (ImGui::Button("Dismiss")) {
            errorsSeen_ = errors.size();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) {
            engine_.lua().clearErrors();
            errorsSeen_ = 0;
        }
        ImGui::SameLine();
        ImGui::Text("%zu error(s)", errors.size());
    }
    ImGui::End();
}

void Editor::draw(double frameMilliseconds, std::uint32_t drawCalls) {
    if (!open_) {
        return;
    }
    if (gesture_ && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        gesture_ = false;
        // The stroke or drag is over: record the map it left, once.
        if (const auto w = world()) {
            history_.settle(*w);
        }
    }
    // Games register tables and types while starting; look again until they have.
    if (jsonArray(schemas_, "tables").empty() && Clock::now() - schemasFetched_ > std::chrono::seconds(2)) {
        refreshSchemas();
    }
    const bool cursorWas = cursorShown_;
    cursorShown_ = false;
    ImGuiErrorRecoveryState recovery;
    ImGui::ErrorRecoveryStoreState(&recovery);
    try {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 430, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420, std::min(760.0f, ImGui::GetIO().DisplaySize.y - 20)),
                                 ImGuiCond_FirstUseEver);
        if (ImGui::Begin("f-engine editor (F1)", &open_)) {
            ImGui::Checkbox("Pause game", &paused_);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status_.c_str());
            if (ImGui::BeginTabBar("tabs")) {
                if (ImGui::BeginTabItem("World")) {
                    drawWorldTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Map")) {
                    drawMapTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Data")) {
                    drawDataTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Console")) {
                    drawConsoleTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Profiler")) {
                    drawProfilerTab(frameMilliseconds, drawCalls);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Launch")) {
                    drawLaunchTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    } catch (const std::exception& error) {
        auto& io = ImGui::GetIO();
        const bool assertOnError = io.ConfigErrorRecoveryEnableAssert;
        io.ConfigErrorRecoveryEnableAssert = false;
        ImGui::ErrorRecoveryTryToRecoverState(&recovery);
        io.ConfigErrorRecoveryEnableAssert = assertOnError;
        status_ = std::string("Editor error: ") + error.what();
    }
    if (cursorWas && !cursorShown_) {
        if (const auto w = world()) {
            w->cursor().visible = false;
        }
    }
    if (!open_) {
        close(); // closed with the window's close button
    }
}
} // namespace fe
