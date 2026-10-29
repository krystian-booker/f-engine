#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

void Editor::drawWorldTab() {
    if (ImGui::BeginCombo("World", worldName_.c_str())) {
        for (const auto& entry : engine_.worlds()) {
            if (ImGui::Selectable(entry.first.c_str(), entry.first == worldName_)) {
                worldName_ = entry.first;
                selectedEntity_ = entt::null;
            }
        }
        ImGui::EndCombo();
    }
    auto* w = world();
    if (!w) {
        ImGui::TextDisabled("No world loaded");
        return;
    }
    auto& registry = w->registry();
    const auto lights = w->activeLights();
    ImGui::Text("%zu entities, %zu particles, %zu lights", registry.view<Transform>().size(),
                w->particles().size(), lights.size());
    ImGui::BeginChild("entities", ImVec2(0, 180), ImGuiChildFlags_Borders);
    for (const auto entity : registry.view<Transform>()) {
        const auto* name = registry.try_get<Name>(entity);
        const auto key = static_cast<std::uint32_t>(entt::to_integral(entity));
        ImGui::PushID(static_cast<int>(key));
        if (ImGui::Selectable((name ? name->id : "#" + std::to_string(entt::to_entity(entity))).c_str(),
                              selectedEntity_ == entity)) {
            selectedEntity_ = entity;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (registry.valid(selectedEntity_) && registry.all_of<Transform>(selectedEntity_)) {
        drawEntity(*w, selectedEntity_);
    }
    if (ImGui::CollapsingHeader("Active lights")) {
        for (std::size_t i = 0; i < lights.size(); ++i) {
            const auto& l = lights[i];
            ImGui::Text("%s at (%.1f, %.1f, %.1f) radius %.1f intensity %.2f",
                        l.type == LightType::Spot ? "Spot" : "Point", l.position.x, l.position.y,
                        l.position.z, l.radius, l.intensity);
            ImGui::SameLine();
            ImGui::ColorButton(("##light" + std::to_string(i)).c_str(),
                               ImVec4(l.color.r, l.color.g, l.color.b, 1));
        }
    }
}

// Run-time entity state: changes here last until the entity is respawned
// and are not part of the map.
void Editor::drawEntity(World& w, entt::entity entity) {
    auto& registry = w.registry();
    auto& t = registry.get<Transform>(entity);
    editVec3("Position", t.position);
    editLayer("Layer", t.layer, w.grid());
    ImGui::DragFloat2("Facing", glm::value_ptr(t.facing), 0.05f);
    if (auto* sprite = registry.try_get<Sprite>(entity)) {
        ImGui::SeparatorText("Sprite");
        ImGui::InputText("Animation", &sprite->animation);
        ImGui::InputText("Clip", &sprite->clip);
        ImGui::Checkbox("Visible", &sprite->visible);
        ImGui::ColorEdit4("Tint", glm::value_ptr(sprite->tint));
        ImGui::ColorEdit4("Outline", glm::value_ptr(sprite->outline));
    }
    if (auto* mover = registry.try_get<Mover>(entity)) {
        ImGui::SeparatorText("Mover");
        ImGui::DragFloat("Speed", &mover->speed, 0.05f, 0, 20);
        ImGui::Checkbox("Collide", &mover->collide);
    }
    if (auto* source = registry.try_get<LightSource>(entity)) {
        ImGui::SeparatorText("Light");
        auto& l = source->light;
        ImGui::Checkbox("Enabled", &source->enabled);
        editVec3("Offset", l.position);
        editColor("Colour", l.color);
        ImGui::DragFloat("Radius", &l.radius, 0.1f, 0.1f, 100);
        ImGui::DragFloat("Intensity", &l.intensity, 0.05f, 0, 50);
        ImGui::DragFloat("Flicker", &l.flicker, 0.01f, 0, 1);
        ImGui::DragFloat("Flicker speed", &l.flickerSpeed, 0.1f, 0, 60);
    }
    if (auto* emitter = registry.try_get<Emitter>(entity)) {
        ImGui::SeparatorText("Emitter");
        ImGui::Checkbox("Active", &emitter->active);
        ImGui::DragFloat("Rate", &emitter->settings.rate, 0.5f, 0, 1000);
    }
    if (const auto* current = registry.try_get<Collider>(entity)) {
        ImGui::SeparatorText("Collider");
        auto collider = *current;
        bool changed = ImGui::DragFloat("Collider radius", &collider.radius, 0.01f, 0.01f, 10);
        changed |= ImGui::DragFloat("Collider height", &collider.height, 0.05f, 0.01f, 20);
        changed |= ImGui::Checkbox("Solid", &collider.solid);
        if (changed) {
            editCollider(w, entity, collider);
        }
    }
    if (auto* props = registry.try_get<Props>(entity)) {
        ImGui::SeparatorText("Props");
        editJson("##props", props->value);
    }
}
} // namespace fe
