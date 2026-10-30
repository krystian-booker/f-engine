#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

// Run-time entity state: changes here last until the entity is respawned
// and are not part of the map.
// Every section pushes its own ImGui id, so "Radius" of a light and "Layer"
// of the transform never collide with the Map panel's widgets of those names.
void Editor::drawEntity(World& w, entt::entity entity) {
    auto& registry = w.registry();
    auto& t = registry.get<Transform>(entity);
    ImGui::TextDisabled("Live entity: edits last until it is respawned and are not saved");
    if (const auto* tags = registry.try_get<Tags>(entity); tags && !tags->values.empty()) {
        std::string list;
        for (const auto& tag : tags->values) {
            list += (list.empty() ? "" : ", ") + tag;
        }
        ImGui::Text("Tags: %s", list.c_str());
    }
    ImGui::PushID("transform");
    auto position = t.position;
    int layer = t.layer;
    bool moved = editVec3("Position", position);
    moved |= editLayer("Layer", layer, w.grid());
    if (moved) {
        editTransform(w, entity, position, layer); // through the collision hash
    }
    ImGui::DragFloat2("Facing", glm::value_ptr(t.facing), 0.05f);
    if (registry.all_of<MeshInstance>(entity)) {
        float yaw = glm::degrees(t.yaw);
        if (ImGui::DragFloat("Yaw", &yaw, 1, -360, 360)) {
            t.yaw = glm::radians(yaw);
            t.previousYaw = t.yaw;
        }
        ImGui::DragFloat3("Scale", glm::value_ptr(t.scale), 0.01f, 0.01f, 100);
    }
    ImGui::PopID();
    if (auto* sprite = registry.try_get<Sprite>(entity)) {
        ImGui::PushID("sprite");
        ImGui::SeparatorText("Sprite");
        assetPicker("Animation", sprite->animation, AssetKind::Sprite);
        std::string clip = sprite->clip;
        if (clipPicker("Clip", clip, sprite->animation)) {
            w.setClip(entity, clip);
        }
        ImGui::Checkbox("Visible", &sprite->visible);
        ImGui::DragFloat("Scale", &sprite->scale, 0.01f, 0.01f, 1000);
        ImGui::ColorEdit4("Tint", glm::value_ptr(sprite->tint));
        ImGui::ColorEdit4("Outline", glm::value_ptr(sprite->outline));
        ImGui::PopID();
    }
    if (auto* mesh = registry.try_get<MeshInstance>(entity)) {
        ImGui::PushID("mesh");
        ImGui::SeparatorText("Mesh");
        assetPicker("Model", mesh->model, AssetKind::Model);
        ImGui::InputText("Animation", &mesh->animation);
        ImGui::Checkbox("Visible", &mesh->visible);
        ImGui::SameLine();
        ImGui::Checkbox("Playing", &mesh->playing);
        ImGui::ColorEdit4("Tint", glm::value_ptr(mesh->tint));
        ImGui::PopID();
    }
    if (auto* mover = registry.try_get<Mover>(entity)) {
        ImGui::PushID("mover");
        ImGui::SeparatorText("Mover");
        ImGui::DragFloat("Speed", &mover->speed, 0.05f, 0, 1000);
        ImGui::Checkbox("Collide", &mover->collide);
        ImGui::PopID();
    }
    if (auto* source = registry.try_get<LightSource>(entity)) {
        ImGui::PushID("light");
        ImGui::SeparatorText("Light");
        auto& l = source->light;
        ImGui::Checkbox("Enabled", &source->enabled);
        editVec3("Offset", l.position);
        editColor("Colour", l.color);
        ImGui::DragFloat("Radius", &l.radius, 0.1f, 0.1f, 1000);
        ImGui::DragFloat("Intensity", &l.intensity, 0.05f, 0, 50);
        ImGui::DragFloat("Flicker", &l.flicker, 0.01f, 0, 1);
        ImGui::DragFloat("Flicker speed", &l.flickerSpeed, 0.1f, 0, 60);
        ImGui::PopID();
    }
    if (auto* emitter = registry.try_get<Emitter>(entity)) {
        ImGui::PushID("emitter");
        ImGui::SeparatorText("Emitter");
        ImGui::Checkbox("Active", &emitter->active);
        ImGui::DragFloat("Rate", &emitter->settings.rate, 0.5f, 0, 1000);
        ImGui::PopID();
    }
    if (const auto* current = registry.try_get<Collider>(entity)) {
        ImGui::PushID("collider");
        ImGui::SeparatorText(current->solid ? "Collider" : "Collider (trigger)");
        auto collider = *current;
        bool changed = ImGui::DragFloat("Radius", &collider.radius, 0.01f, 0, 64);
        changed |= ImGui::DragFloat("Height", &collider.height, 0.05f, 0, 1000);
        changed |= ImGui::Checkbox("Solid", &collider.solid);
        if (changed) {
            editCollider(w, entity, collider);
        }
        ImGui::PopID();
    }
    if (auto* audio = registry.try_get<AudioEmitter>(entity)) {
        ImGui::PushID("audio");
        ImGui::SeparatorText("Audio");
        assetPicker("Sound", audio->sound, AssetKind::Audio);
        ImGui::DragFloat("Radius", &audio->radius, 0.1f, 0, 1000);
        ImGui::DragFloat("Volume", &audio->volume, 0.01f, 0, 4);
        ImGui::PopID();
    }
    if (auto* props = registry.try_get<Props>(entity)) {
        ImGui::PushID("props");
        ImGui::SeparatorText("Props");
        editJson("##props", props->value);
        ImGui::PopID();
    }
}
} // namespace fe
