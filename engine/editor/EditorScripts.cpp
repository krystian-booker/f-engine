#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <IconsFontAwesome6.h>
#include <imgui.h>

namespace fe {
using namespace editorui;

namespace {
// Results of the game's editor hooks, refreshed a few times a second.
struct ScriptsCache {
    std::chrono::steady_clock::time_point fetched{};
    Json tasks = Json::array(), entities = Json::object(), events = Json::array();
    bool showHelpers = false;
};
ScriptsCache& cache() {
    static ScriptsCache c;
    return c;
}
const char* triggerIcon(const std::string& trigger) {
    if (trigger == "touch") {
        return ICON_FA_SHOE_PRINTS;
    }
    if (trigger == "auto") {
        return ICON_FA_BOLT;
    }
    if (trigger == "parallel") {
        return ICON_FA_REPEAT;
    }
    return ICON_FA_COMMENT;
}
} // namespace

// What triggers what on the current map, what is running, and what the
// world reported lately.
void Editor::drawScriptsPanel() {
    const auto w = world();
    if (!w) {
        ImGui::TextDisabled("No world loaded");
        return;
    }
    auto& c = cache();
    if (Clock::now() - c.fetched > std::chrono::milliseconds(250)) {
        c.fetched = Clock::now();
        const auto fetch = [&](const char* name, Json& into, const Json& fallback) {
            into = fallback;
            if (const auto fn = devtool(name); fn != sol::lua_nil) {
                const auto result = engine_.lua().invoke(fn, w->name());
                if (result.valid() && result.get_type() == sol::type::string) {
                    const auto parsed = Json::parse(result.get<std::string>(), nullptr, false);
                    if (!parsed.is_discarded() && !parsed.is_null()) {
                        into = parsed;
                    }
                }
            }
        };
        fetch("tasks", c.tasks, Json::array());
        fetch("inspect_entities", c.entities, Json::object());
        fetch("recent_events", c.events, Json::array());
    }
    const auto& map = w->authored();
    if (ImGui::BeginTabBar("scripts")) {
        if (ImGui::BeginTabItem(ICON_FA_DIAGRAM_PROJECT " Scene wiring")) {
            ImGui::TextDisabled("Every placement with a script and how it fires. Click a row to select it; "
                                "the active page is the one its conditions pick right now.");
            if (ImGui::BeginTable("wiring", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Object");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Trigger");
                ImGui::TableSetupColumn("Script");
                ImGui::TableSetupColumn("State");
                ImGui::TableHeadersRow();
                int rowId = 0;
                const auto row = [&](const Selection& self, const std::string& name, const std::string& type,
                                     const std::string& trigger, const std::string& script,
                                     const std::string& state, bool active) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(rowId++);
                    if (active) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1, 0.6f, 1));
                    }
                    if (ImGui::Selectable(name.c_str(), isSelected(self),
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        select(self);
                        frameSelection(*w, alpha_);
                    }
                    if (active) {
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(type.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%s %s", triggerIcon(trigger), trigger.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(script.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(state.c_str());
                };
                for (const auto& p : map.placements) {
                    const auto self = Selection::placement(p.id);
                    const Json info = c.entities.contains(p.id) ? c.entities.at(p.id) : Json();
                    std::string state;
                    if (info.is_object()) {
                        if (const auto running = info.find("running");
                            running != info.end() && running->is_object()) {
                            state = "running " + jsonString(*running, "name", "");
                        } else if (const auto parallel = info.find("parallel");
                                   parallel != info.end() && parallel->is_object()) {
                            state = "parallel running";
                        } else if (jsonBool(info, "auto_done", false)) {
                            state = "auto done";
                        }
                    } else {
                        state = "not spawned";
                    }
                    if (const auto pages = p.props.find("pages");
                        pages != p.props.end() && pages->is_array()) {
                        std::size_t index = 0;
                        for (const auto& page : *pages) {
                            ++index;
                            if (!page.is_object()) {
                                continue;
                            }
                            const auto script = jsonString(page, "script", "(none)");
                            const auto trigger = jsonString(page, "trigger", "action");
                            std::string when;
                            if (const auto conditions = page.find("when");
                                conditions != page.end() && conditions->is_object()) {
                                when = " when " + conditions->dump();
                            }
                            bool active = false;
                            if (info.is_object()) {
                                for (const auto& known : jsonArray(info, "pages")) {
                                    if (jsonNumber(known, "index", 0) == static_cast<double>(index)) {
                                        active = jsonBool(known, "active", false);
                                        if (const auto error = known.find("error");
                                            error != known.end() && error->is_string()) {
                                            state = "page error: " + error->get<std::string>();
                                        }
                                    }
                                }
                            }
                            row(self, p.id + " #" + std::to_string(index), p.type, trigger + when, script,
                                active ? (state.empty() ? "active page" : state) : state, active);
                        }
                    } else if (const auto script = p.props.find("script");
                               script != p.props.end() && script->is_string()) {
                        row(self, p.id, p.type, jsonString(p.props, "trigger", "action"),
                            script->get<std::string>(), state, false);
                    } else if (p.props.contains("map") && p.props.at("map").is_string()) {
                        row(self, p.id, p.type, jsonString(p.props, "trigger", "touch"),
                            "warp to " + jsonString(p.props, "map", "?") + " / " +
                                jsonString(p.props, "point", "?"),
                            state, false);
                    } else if (info.is_object() &&
                               (jsonBool(info, "interact", false) || jsonBool(info, "on_touch", false))) {
                        row(self, p.id, p.type, jsonBool(info, "on_touch", false) ? "touch" : "action",
                            "built-in (" + p.type + ")", state, false);
                    }
                }
                for (const auto& z : map.zones) {
                    row(Selection::zone(z.id), z.id, "zone", "enter / exit", z.type + " handler", "", false);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_GEARS " Running tasks")) {
            ImGui::Checkbox("Show helper tasks", &c.showHelpers);
            ImGui::SameLine();
            ImGui::TextDisabled("%zu task(s); whatever the game's editor.tasks hook reports", c.tasks.size());
            if (ImGui::BeginTable("tasks", 6,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Task");
                ImGui::TableSetupColumn("Parent");
                ImGui::TableSetupColumn("World");
                ImGui::TableSetupColumn("Age");
                ImGui::TableSetupColumn("Children");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                for (const auto& t : c.tasks) {
                    if (!c.showHelpers && jsonBool(t, "helper", false)) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(jsonString(t, "name", "?").c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", jsonString(t, "parent", "").c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", jsonString(t, "world", "").c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1fs", jsonNumber(t, "age", 0));
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", static_cast<int>(jsonNumber(t, "children", 0)));
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(jsonNumber(t, "index", 0)));
                    if (ImGui::SmallButton(ICON_FA_XMARK)) {
                        if (const auto cancel = devtool("cancel_task"); cancel != sol::lua_nil) {
                            succeeded(
                                engine_.lua().invoke(cancel, static_cast<int>(jsonNumber(t, "index", 0))));
                            c.fetched = {};
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Cancel this task (and its children)");
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_WAVE_SQUARE " Recent events")) {
            ImGui::TextDisabled(
                "The last world events the field handled: touches, zone enters and exits, footsteps...");
            if (ImGui::BeginTable("events", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Time");
                ImGui::TableSetupColumn("Event");
                ImGui::TableSetupColumn("Who");
                ImGui::TableSetupColumn("What");
                ImGui::TableHeadersRow();
                for (auto it = c.events.rbegin(); it != c.events.rend(); ++it) {
                    const auto& e = *it;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", jsonNumber(e, "time", 0));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(jsonString(e, "type", "").c_str());
                    ImGui::TableNextColumn();
                    const auto a = jsonString(e, "a", ""), b = jsonString(e, "b", "");
                    ImGui::Text("%s%s%s", a.c_str(), b.empty() ? "" : " / ", b.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(jsonString(e, "name", "").c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
} // namespace fe
