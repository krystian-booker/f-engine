#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <IconsFontAwesome6.h>
#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <map>

namespace fe {
using namespace editorui;

namespace {
bool matches(const std::string& filter, const std::string& text) {
    if (filter.empty()) {
        return true;
    }
    const auto lower = [](std::string s) {
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    return lower(text).find(lower(filter)) != std::string::npos;
}
} // namespace

void Editor::drawHierarchy() {
    if (ImGui::BeginCombo("##world", worldName_.c_str())) {
        for (const auto& entry : engine_.worlds()) {
            if (ImGui::Selectable(entry.first.c_str(), entry.first == worldName_)) {
                worldName_ = entry.first;
                selection_ = {};
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The loaded world the editor shows");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS " Filter", &hierarchyFilter_);
    const auto w = world();
    if (!w) {
        ImGui::TextDisabled("No world loaded");
        return;
    }
    const auto& map = w->authored();
    const auto row = [&](const Selection& self, const std::string& text, glm::vec4 color) {
        if (!matches(hierarchyFilter_, text)) {
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.r, color.g, color.b, 1));
        const bool selected = isSelected(self);
        if (ImGui::Selectable(text.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::GetIO().KeyCtrl && !selection_.empty()) {
                toggleExtra(self);
            } else {
                select(self);
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                frameSelection(*w, alpha_);
            }
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            hovered_ = self;
        }
        if (ImGui::BeginPopupContextItem()) {
            select(self);
            if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Focus")) {
                frameSelection(*w, alpha_);
            }
            if (self.kind != Selection::Kind::Entity && ImGui::MenuItem(ICON_FA_CLONE " Duplicate")) {
                duplicateSelection(*w);
            }
            if (self.kind == Selection::Kind::Point) {
                const auto playFrom = devtool("play_from");
                if (ImGui::MenuItem(ICON_FA_PLAY " Play from here", nullptr, false, playFrom != sol::lua_nil)) {
                    const auto mapId = map.id;
                    if (succeeded(engine_.lua().invoke(playFrom, mapId, self.id))) {
                        setMode(Mode::Play);
                    }
                }
            }
            if (ImGui::MenuItem(ICON_FA_TRASH " Delete")) {
                deleteSelection(*w);
            }
            ImGui::EndPopup();
        }
    };
    const auto group = [&](const char* label, bool* visible, std::size_t count, const auto& body) {
        ImGui::PushID(label);
        if (visible) {
            ImGui::Checkbox("##show", visible);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show in the Scene");
            }
            ImGui::SameLine();
        }
        const auto title = std::string(label) + " (" + std::to_string(count) + ")";
        const bool openNode = ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        if (openNode) {
            body();
            ImGui::TreePop();
        }
        ImGui::PopID();
    };
    using namespace handleColors;
    group(ICON_FA_FLAG " Points", &handles_.points, map.points.size(), [&] {
        for (const auto& [name, point] : map.points) {
            row(Selection::point(name), name, PointColor);
        }
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("##newpoint", &newPoint_);
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_PLUS " Add point") && !newPoint_.empty()) {
            addPoint(*w, newPoint_);
        }
    });
    group(ICON_FA_PERSON " Placements", &handles_.placements, map.placements.size(), [&] {
        // Grouped by type so a town's NPCs, chests and doors read as lists.
        std::map<std::string, std::vector<const Placement*>> byType;
        for (const auto& p : map.placements) {
            byType[p.type].push_back(&p);
        }
        for (const auto& [type, list] : byType) {
            const auto title = type + " (" + std::to_string(list.size()) + ")";
            if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
                for (const auto* p : list) {
                    const bool spawned = placementEntity(*w, p->id) != entt::null;
                    row(Selection::placement(p->id), p->id + (spawned ? "" : "  (not spawned)"),
                        spawned ? glm::vec4(1) : PlacementColor);
                }
                ImGui::TreePop();
            }
        }
        if (ImGui::SmallButton(ICON_FA_PLUS " Add placement")) {
            addPlacement(*w);
        }
    });
    group(ICON_FA_VECTOR_SQUARE " Zones", &handles_.zones, map.zones.size(), [&] {
        for (const auto& z : map.zones) {
            row(Selection::zone(z.id), z.id + " (" + z.type + ")", ZoneColor);
        }
        if (ImGui::SmallButton(ICON_FA_PLUS " Add zone")) {
            addZone(*w);
        }
    });
    group(ICON_FA_LIGHTBULB " Lights", &handles_.lights, map.lights.size(), [&] {
        for (std::size_t i = 0; i < map.lights.size(); ++i) {
            const auto& l = map.lights[i];
            row(Selection::light(static_cast<int>(i)),
                std::string(l.type == LightType::Spot ? "Spot " : "Point ") + std::to_string(i), LightColor);
        }
        if (ImGui::SmallButton(ICON_FA_PLUS " Add light")) {
            addLight(*w);
        }
    });
    group(ICON_FA_FIRE " Emitters", &handles_.emitters, map.emitters.size(), [&] {
        for (std::size_t i = 0; i < map.emitters.size(); ++i) {
            row(Selection::emitter(static_cast<int>(i)),
                "Emitter " + std::to_string(i) + " " + map.emitters[i].settings.sprite, EmitterColor);
        }
        if (ImGui::SmallButton(ICON_FA_PLUS " Add emitter")) {
            addEmitter(*w);
        }
    });
    group(ICON_FA_CUBE " Colliders", &handles_.colliders, map.colliders.size(), [&] {
        for (std::size_t i = 0; i < map.colliders.size(); ++i) {
            row(Selection::collider(static_cast<int>(i)), "Collider " + std::to_string(i), ColliderColor);
        }
        if (ImGui::SmallButton(ICON_FA_PLUS " Add collider")) {
            addCollider(*w);
        }
        if (!map.bake.colliders.empty()) {
            ImGui::TextDisabled("%zu baked box(es) from the geometry and props (Map tab)", map.bake.colliders.size());
        }
    });
    // Live entities that no placement made: the player, followers, spawned things.
    auto& registry = w->registry();
    std::vector<entt::entity> others;
    for (const auto entity : registry.view<Transform>()) {
        if (entityPlacement(*w, entity).empty()) {
            others.push_back(entity);
        }
    }
    group(ICON_FA_GHOST " Live entities", &handles_.triggers, others.size(), [&] {
        ImGui::TextDisabled("Run-time state; edits here are not saved with the map");
        for (const auto entity : others) {
            const auto* name = registry.try_get<Name>(entity);
            const auto label = name ? name->id : "#" + std::to_string(entt::to_entity(entity));
            row(Selection::ofEntity(*w, entity), label, glm::vec4(0.8f, 0.85f, 1, 1));
        }
    });
}
} // namespace fe
