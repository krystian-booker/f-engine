#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/editor/PropLinks.h"
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
            ImGui::TextDisabled("What the placements and zones of this map are wired to: every field the "
                                "game's schemas type as a script or a map. Click a row to select it.");
            if (ImGui::BeginTable("wiring", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Object");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Field");
                ImGui::TableSetupColumn("Links to");
                ImGui::TableSetupColumn("State");
                ImGui::TableHeadersRow();
                int rowId = 0;
                const auto row = [&](const Selection& self, const std::string& name, const std::string& type,
                                     const PropLink& link, const std::string& state) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(rowId++);
                    if (ImGui::Selectable(name.c_str(), isSelected(self),
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        select(self);
                        frameSelection(*w, alpha_);
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(type.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(link.field.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%s %s",
                                link.kind == PropLink::Kind::Script ? ICON_FA_SCROLL : ICON_FA_DOOR_OPEN,
                                link.value.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(state.c_str());
                };
                // The game's word on each object, when its inspect_entities
                // hook gives one: {id: {state = "..."}}.
                const auto stateOf = [&](const std::string& id) {
                    const auto info = c.entities.find(id);
                    return info != c.entities.end() && info->is_object() ? jsonString(*info, "state", "")
                                                                         : std::string();
                };
                for (const auto& p : map.placements) {
                    for (const auto& link : propLinks(typeFields(schemas_, "entities", p.type), p.props)) {
                        row(Selection::placement(p.id), p.id, p.type, link, stateOf(p.id));
                    }
                }
                for (const auto& z : map.zones) {
                    for (const auto& link : propLinks(typeFields(schemas_, "zones", z.type), z.props)) {
                        row(Selection::zone(z.id), z.id, z.type + " zone", link, stateOf(z.id));
                    }
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
