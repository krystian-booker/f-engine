#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

void Editor::drawPlacements(World& w) {
    const auto& placements = w.authored().placements;
    if (ImGui::Button("Add placement")) {
        Placement p;
        p.id = uniqueId("placement", placements);
        p.type = defaultType("entities");
        p.position = w.camera().target;
        edit(w, [&](MapData& m) { m.placements.push_back(p); });
        selectedPlacement_ = static_cast<int>(placements.size()) - 1;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Click map to move", &placing_);
    ImGui::BeginChild("placements", ImVec2(0, 120), ImGuiChildFlags_Borders);
    for (std::size_t i = 0; i < placements.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable((placements[i].id + " (" + placements[i].type + ")").c_str(),
                              selectedPlacement_ == static_cast<int>(i))) {
            selectedPlacement_ = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!validIndex(selectedPlacement_, placements)) {
        return;
    }
    const auto index = static_cast<std::size_t>(selectedPlacement_);
    auto p = placements[index];
    bool changed = false;
    if (placing_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (const auto cell = hoveredCell(w)) {
            p.position = {static_cast<float>(cell->x) + 0.5f,
                          w.authored().grid.heightAt({cell->x + 0.5f, cell->z + 0.5f}, cell->layer),
                          static_cast<float>(cell->z) + 0.5f};
            p.layer = cell->layer;
            changed = true;
        }
    }
    changed |= ImGui::InputText("Id", &p.id);
    if (ImGui::BeginCombo("Type", p.type.c_str())) {
        for (const auto& type : schemas_.value("entities", Json::object()).items()) {
            if (ImGui::Selectable(type.key().c_str(), type.key() == p.type)) {
                p.type = type.key();
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    changed |= ImGui::InputText("Custom type", &p.type);
    changed |= editVec3("Position", p.position);
    changed |= ImGui::DragFloat2("Facing", glm::value_ptr(p.facing), 0.05f, -1, 1);
    changed |= editLayer("Layer", p.layer, w.authored().grid);
    ImGui::SeparatorText("Props");
    changed |= editProps(fieldsFor(p.type, {"entities"}), p.props);
    const bool duplicate = std::any_of(placements.begin(), placements.end(), [&](const Placement& other) {
        return &other != &placements[index] && other.id == p.id;
    });
    if (changed && (p.id.empty() || duplicate)) {
        status_ = "Placement ids must be unique and not empty";
    } else if (changed) {
        edit(w, [&](MapData& m) {
            if (index < m.placements.size()) {
                m.placements[index] = p;
            }
        });
    }
    if (ImGui::Button("Delete placement")) {
        edit(w, [&](MapData& m) {
            if (index < m.placements.size()) {
                m.placements.erase(m.placements.begin() + static_cast<std::ptrdiff_t>(index));
            }
        });
        selectedPlacement_ = -1;
    }
}

void Editor::drawZones(World& w) {
    const auto& zones = w.authored().zones;
    if (ImGui::Button("Add zone")) {
        const auto t = w.camera().target;
        auto type = defaultType("zones");
        Zone zone{"zone" + std::to_string(zones.size() + 1),
                  type.empty() ? "zone" : type,
                  {t.x - 2, t.z - 2},
                  {t.x + 2, t.z + 2},
                  -1,
                  Json::object()};
        edit(w, [&](MapData& m) { m.zones.push_back(zone); });
        selectedZone_ = static_cast<int>(zones.size()) - 1;
    }
    for (std::size_t i = 0; i < zones.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable((zones[i].id + " (" + zones[i].type + ")").c_str(),
                              selectedZone_ == static_cast<int>(i))) {
            selectedZone_ = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (!validIndex(selectedZone_, zones)) {
        w.overlays().erase("editor:zone");
        zoneOverlayKey_.clear();
        return;
    }
    const auto index = static_cast<std::size_t>(selectedZone_);
    auto z = zones[index];
    bool changed = ImGui::InputText("Zone id", &z.id);
    if (ImGui::BeginCombo("Zone type", z.type.c_str())) {
        for (const auto& type : schemas_.value("zones", Json::object()).items()) {
            if (ImGui::Selectable(type.key().c_str(), type.key() == z.type)) {
                z.type = type.key();
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    changed |= ImGui::InputText("Custom zone type", &z.type);
    changed |= ImGui::DragFloat2("Min", glm::value_ptr(z.min), 0.1f);
    changed |= ImGui::DragFloat2("Max", glm::value_ptr(z.max), 0.1f);
    changed |= editLayer("Zone layer (-1 all)", z.layer, w.authored().grid, -1);
    ImGui::SeparatorText("Zone props");
    changed |= editProps(fieldsFor(z.type, {"zones", "entities"}), z.props);
    if (changed) {
        order(z.min, z.max);
        edit(w, [&](MapData& m) {
            if (index < m.zones.size()) {
                m.zones[index] = z;
            }
        });
    }
    const auto key =
        w.name() + "/" + z.id + "/" + Json{z.min.x, z.min.y, z.max.x, z.max.y, z.layer, layer_}.dump();
    if (key != zoneOverlayKey_ || !w.overlays().contains("editor:zone")) {
        zoneOverlayKey_ = key;
        w.overlays()["editor:zone"] =
            OverlayLayer{zoneCells(z, w.authored().grid, layer_), {1, 0.85f, 0.2f, 0.3f}, 1, 92, true, true};
    }
    if (ImGui::Button("Delete zone")) {
        edit(w, [&](MapData& m) {
            if (index < m.zones.size()) {
                m.zones.erase(m.zones.begin() + static_cast<std::ptrdiff_t>(index));
            }
        });
        selectedZone_ = -1;
    }
}

void Editor::drawLights(World& w) {
    const auto& lights = w.authored().lights;
    if (ImGui::Button("Add light")) {
        LightSettings light;
        light.position = w.camera().target + glm::vec3(0, 2, 0);
        edit(w, [&](MapData& m) { m.lights.push_back(light); });
        selectedLight_ = static_cast<int>(lights.size()) - 1;
    }
    for (std::size_t i = 0; i < lights.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(("Light " + std::to_string(i)).c_str(),
                              selectedLight_ == static_cast<int>(i))) {
            selectedLight_ = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (!validIndex(selectedLight_, lights)) {
        return;
    }
    const auto index = static_cast<std::size_t>(selectedLight_);
    auto l = lights[index];
    bool spot = l.type == LightType::Spot;
    bool changed = false;
    if (ImGui::Checkbox("Spot", &spot)) {
        l.type = spot ? LightType::Spot : LightType::Point;
        changed = true;
    }
    changed |= editVec3("Light position", l.position);
    changed |= editColor("Light colour", l.color);
    changed |= ImGui::DragFloat("Radius", &l.radius, 0.1f, 0.1f, 100);
    changed |= ImGui::DragFloat("Intensity", &l.intensity, 0.05f, 0, 50);
    changed |= ImGui::DragFloat("Flicker", &l.flicker, 0.01f, 0, 1);
    changed |= ImGui::DragFloat("Flicker speed", &l.flickerSpeed, 0.1f, 0, 60);
    changed |= ImGui::Checkbox("Cast shadows", &l.castShadows);
    if (spot) {
        changed |= editVec3("Direction", l.direction, 0.01f);
        changed |= ImGui::DragFloat("Inner cone", &l.innerCone, 0.5f, 1, 89);
        changed |= ImGui::DragFloat("Outer cone", &l.outerCone, 0.5f, 1, 89);
    }
    ImGui::SeparatorText("Flicker curve");
    ImGui::TextDisabled("Keys [seconds, scale]; loops over the last key's time");
    auto& curve = l.flickerCurve;
    for (std::size_t k = 0; k < curve.size(); ++k) {
        ImGui::PushID(static_cast<int>(k));
        changed |= ImGui::DragFloat2("##key", glm::value_ptr(curve[k]), 0.01f, 0, 600);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            curve.erase(curve.begin() + static_cast<std::ptrdiff_t>(k));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add key")) {
        addFlickerKey(l);
        changed = true;
    }
    if (changed) {
        if (const auto problem = editProblem(l); !problem.empty()) {
            status_ = problem;
        } else {
            edit(w, [&](MapData& m) {
                if (index < m.lights.size()) {
                    m.lights[index] = l;
                }
            });
        }
    }
    if (ImGui::Button("Delete light")) {
        edit(w, [&](MapData& m) {
            if (index < m.lights.size()) {
                m.lights.erase(m.lights.begin() + static_cast<std::ptrdiff_t>(index));
            }
        });
        selectedLight_ = -1;
    }
}

void Editor::drawEmitters(World& w) {
    const auto& emitters = w.authored().emitters;
    if (ImGui::Button("Add emitter")) {
        const MapEmitter emitter{w.camera().target, EmitterSettings{}};
        edit(w, [&](MapData& m) { m.emitters.push_back(emitter); });
        selectedEmitter_ = static_cast<int>(emitters.size()) - 1;
    }
    for (std::size_t i = 0; i < emitters.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const auto label = "Emitter " + std::to_string(i) + " " + emitters[i].settings.sprite;
        if (ImGui::Selectable(label.c_str(), selectedEmitter_ == static_cast<int>(i))) {
            selectedEmitter_ = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (!validIndex(selectedEmitter_, emitters)) {
        return;
    }
    const auto index = static_cast<std::size_t>(selectedEmitter_);
    auto e = emitters[index];
    auto& s = e.settings;
    bool changed = editVec3("Emitter position", e.position);
    changed |= ImGui::InputText("Sprite", &s.sprite);
    changed |= ImGui::DragFloat("Rate", &s.rate, 0.5f, 0, 1000);
    changed |= ImGui::DragInt("Burst", &s.burst, 1, 0, 10000);
    changed |= ImGui::DragFloat("Lifetime", &s.lifetime, 0.05f, 0.01f, 60);
    changed |= ImGui::DragFloat("Lifetime jitter", &s.lifetimeJitter, 0.01f, 0, 1);
    changed |= editVec3("Velocity", s.velocity);
    changed |= editVec3("Velocity jitter", s.velocityJitter);
    changed |= editVec3("Gravity", s.gravity);
    changed |= editVec3("Area", s.area);
    changed |= ImGui::DragFloat("Size", &s.size, 0.01f, 0, 10);
    changed |= ImGui::DragFloat("Size at end", &s.sizeEnd, 0.01f, 0, 10);
    changed |= ImGui::DragFloat("Drag", &s.drag, 0.01f, 0, 10);
    changed |= ImGui::DragFloat("Stretch", &s.stretch, 0.01f, 0, 10);
    changed |= editColor("Colour", s.color);
    changed |= editColor("Colour at end", s.colorEnd);
    changed |= editBlend("Blend", s.blend);
    changed |= ImGui::Checkbox("Unlit", &s.unlit);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("World space", &s.worldSpace);
    changed |= ImGui::DragFloat("Distort", &s.distort, 0.01f, 0, 4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Above 0 the particles only bend the scene behind them (heat haze)");
    }
    changed |= ImGui::DragInt("Max particles", &s.maxParticles, 4, 1, 16384);
    if (changed) {
        if (const auto problem = editProblem(s); !problem.empty()) {
            status_ = problem;
        } else {
            edit(w, [&](MapData& m) {
                if (index < m.emitters.size()) {
                    m.emitters[index] = e;
                }
            });
        }
    }
    if (ImGui::Button("Delete emitter")) {
        edit(w, [&](MapData& m) {
            if (index < m.emitters.size()) {
                m.emitters.erase(m.emitters.begin() + static_cast<std::ptrdiff_t>(index));
            }
        });
        selectedEmitter_ = -1;
    }
}

void Editor::drawColliders(World& w) {
    const auto& colliders = w.authored().colliders;
    if (ImGui::Button("Add collider")) {
        const auto t = w.camera().target;
        const Box box{t - glm::vec3(0.5f, 0, 0.5f), t + glm::vec3(0.5f, 1, 0.5f)};
        edit(w, [&](MapData& m) { m.colliders.push_back(box); });
        selectedCollider_ = static_cast<int>(colliders.size()) - 1;
    }
    for (std::size_t i = 0; i < colliders.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(("Collider " + std::to_string(i)).c_str(),
                              selectedCollider_ == static_cast<int>(i))) {
            selectedCollider_ = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (!validIndex(selectedCollider_, colliders)) {
        return;
    }
    const auto index = static_cast<std::size_t>(selectedCollider_);
    auto box = colliders[index];
    bool changed = editVec3("Box min", box.min);
    changed |= editVec3("Box max", box.max);
    if (changed) {
        order(box.min, box.max);
        edit(w, [&](MapData& m) {
            if (index < m.colliders.size()) {
                m.colliders[index] = box;
            }
        });
    }
    if (ImGui::Button("Delete collider")) {
        edit(w, [&](MapData& m) {
            if (index < m.colliders.size()) {
                m.colliders.erase(m.colliders.begin() + static_cast<std::ptrdiff_t>(index));
            }
        });
        selectedCollider_ = -1;
    }
}

void Editor::drawEnvironment(World& w) {
    ImGui::TextDisabled("Edits replace the live environment (scripted changes too)");
    auto env = w.authored().environment;
    // Lists of records are edited as JSON and read back like the map file.
    auto editSection = [&](const char* label, const char* pointer) {
        auto value = env.toJson().at(Json::json_pointer(pointer));
        ImGui::TextUnformatted(label);
        if (!editJson((std::string("##") + pointer).c_str(), value, 90)) {
            return false;
        }
        try {
            env = withEnvironmentJson(env, pointer, value);
            return true;
        } catch (const std::exception& error) {
            status_ = error.what();
            return false;
        }
    };
    bool changed = editColor("Ambient", env.ambient);
    changed |= editColor("Sun colour", env.sunColor);
    changed |= editVec3("Sun direction", env.sunDirection, 0.01f);
    changed |= ImGui::Checkbox("Shadows", &env.shadows);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Anisotropic", &env.anisotropic);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Pixel snap", &env.pixelSnap);
    changed |= ImGui::DragFloat("Shadow distance", &env.shadowDistance, 0.5f, 4, 200);
    changed |= ImGui::SliderFloat("Shadow strength", &env.shadowStrength, 0, 1);
    changed |= ImGui::DragFloat("Shadow bias", &env.shadowBias, 0.001f, 0, 0.5f);
    changed |= ImGui::DragFloat("Sprite light height", &env.spriteLightHeight, 0.05f, 0, 4);
    ImGui::SeparatorText("Fog");
    changed |= editColor("Fog colour", env.fogColor);
    changed |= ImGui::DragFloat("Fog density", &env.fogDensity, 0.001f, 0, 1);
    changed |= ImGui::DragFloat("Fog height", &env.fogHeight, 0.1f);
    changed |= ImGui::DragFloat("Fog height falloff", &env.fogHeightFalloff, 0.005f, 0, 4);
    changed |= ImGui::SliderFloat("Fog max", &env.fogMax, 0, 1);
    ImGui::SeparatorText("Water");
    auto& water = env.water;
    changed |= ImGui::DragFloat("Wave scale", &water.waveScale, 0.01f, 0, 8);
    changed |= ImGui::DragFloat("Wave speed", &water.waveSpeed, 0.01f, 0, 8);
    changed |= ImGui::DragFloat("Fresnel", &water.fresnel, 0.01f, 0, 4);
    changed |= ImGui::DragFloat("Specular", &water.specular, 0.01f, 0, 4);
    changed |= editColor("Deep colour", water.deepColor);
    changed |= ImGui::DragFloat("Depth falloff", &water.depthFalloff, 0.01f, 0, 8);
    changed |= ImGui::SliderFloat("Reflection", &water.reflection, 0, 1);
    changed |= ImGui::DragFloat("Refraction", &water.refraction, 0.001f, 0, 0.5f);
    ImGui::SeparatorText("Sky");
    changed |= editChoice("Sky mode", env.sky.mode, {"none", "color", "gradient", "panorama"});
    changed |= editColor("Sky top", env.sky.top);
    changed |= editColor("Sky horizon", env.sky.horizon);
    changed |= editColor("Sky bottom", env.sky.bottom);
    changed |= ImGui::InputText("Panorama", &env.sky.panorama);
    changed |= ImGui::SliderFloat("Clouds", &env.sky.cloudCover, 0, 1);
    changed |= ImGui::DragFloat("Cloud speed", &env.sky.cloudSpeed, 0.001f, 0, 1);
    changed |= editColor("Cloud colour", env.sky.cloudColor);
    changed |= editSection("Backdrops", "/sky/backdrops");
    ImGui::SeparatorText("Weather");
    changed |= ImGui::InputText("Weather kind", &env.weather.kind);
    changed |= ImGui::SliderFloat("Weather intensity", &env.weather.intensity, 0, 2);
    changed |= editVec3("Wind", env.weather.wind);
    changed |= editColor("Weather colour", env.weather.color);
    changed |= ImGui::InputText("Weather sprite", &env.weather.sprite);
    ImGui::SeparatorText("Post");
    auto& p = env.post;
    changed |= ImGui::Checkbox("Bloom", &p.bloom);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Depth of field", &p.depthOfField);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Vignette", &p.vignette);
    changed |= ImGui::Checkbox("Colour grade", &p.colorGrade);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("SSAO", &p.ssao);
    changed |= ImGui::SliderFloat("Bloom strength", &p.bloomStrength, 0, 2);
    changed |= ImGui::SliderFloat("Bloom threshold", &p.bloomThreshold, 0, 4);
    changed |= ImGui::SliderFloat("Bloom knee", &p.bloomKnee, 0, 1);
    changed |= ImGui::SliderFloat("Bloom radius", &p.bloomRadius, 0, 1);
    changed |= ImGui::SliderInt("Bloom levels", &p.bloomLevels, 1, 8);
    changed |= editChoice("DOF mode", p.dofMode, {"depth", "tilt", "both"});
    changed |= ImGui::SliderFloat("Focus line", &p.focus, 0, 1);
    changed |= ImGui::SliderFloat("Focus range", &p.focusRange, 0, 1);
    changed |= ImGui::DragFloat("Focus distance", &p.focusDistance, 0.1f, 0, 200);
    changed |= ImGui::SliderFloat("Aperture", &p.aperture, 0, 8);
    changed |= ImGui::SliderFloat("DOF radius", &p.dofRadius, 0, 32);
    changed |= ImGui::SliderFloat("SSAO radius", &p.ssaoRadius, 0.05f, 4);
    changed |= ImGui::SliderFloat("SSAO strength", &p.ssaoStrength, 0, 4);
    changed |= ImGui::SliderFloat("SSAO bias", &p.ssaoBias, 0, 0.5f);
    changed |= editChoice("Antialias", p.antialias, {"auto", "fxaa", "none"});
    changed |= ImGui::SliderFloat("Chromatic aberration", &p.chromaticAberration, 0, 2);
    changed |= ImGui::SliderFloat("Distortion", &p.distortion, 0, 4);
    changed |= ImGui::SliderFloat("Exposure", &p.exposure, 0.1f, 4);
    changed |= editChoice("Tonemap", p.tonemap, {"aces", "none"});
    changed |= ImGui::SliderFloat("Vignette strength", &p.vignetteStrength, 0, 2);
    changed |= ImGui::SliderInt("Pixel scale", &p.pixelScale, 1, 8);
    changed |= ImGui::InputText("LUT", &p.lut);
    changed |= ImGui::InputText("LUT blend target", &p.lutBlendTarget);
    changed |= ImGui::SliderFloat("LUT blend", &p.lutBlend, 0, 1);
    changed |= editSection("Custom passes [{shader, params}]", "/post/custom");
    if (!changed) {
        return;
    }
    if (const auto problem = editProblem(env); !problem.empty()) {
        status_ = problem;
    } else {
        edit(w, [&](MapData& m) { m.environment = env; });
    }
}
} // namespace fe
