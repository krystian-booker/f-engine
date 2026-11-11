#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/editor/SceneEdits.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <IconsFontAwesome6.h>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

namespace {
// Where a new object goes: the ground under the editor camera's focus.
glm::vec3 dropPoint(const World& w, const Camera& camera, int layer) {
    const auto& grid = w.authored().grid;
    const auto focus = camera.focus();
    const auto cell = snapToCell(grid, focus, layer);
    return cellCentre(grid, cell);
}
} // namespace

void Editor::addPlacement(World& w) {
    const auto& placements = w.authored().placements;
    Placement p;
    p.type = defaultType("entities");
    p.id = uniquePlacementId(p.type.empty() ? "placement" : p.type, placements);
    p.position = dropPoint(w, sceneCamera(w), layer_);
    p.layer = layer_;
    gesture_ = false;
    edit(w, [&](MapData& m) { m.placements.push_back(p); });
    gesture_ = false;
    select(Selection::placement(p.id));
    placementChanged(w, p.id);
}

void Editor::addZone(World& w) {
    const auto& zones = w.authored().zones;
    const auto t = dropPoint(w, sceneCamera(w), layer_);
    auto type = defaultType("zones");
    Zone zone{
        uniqueId("zone", zones), type.empty() ? "zone" : type, {t.x - 2, t.z - 2}, {t.x + 2, t.z + 2}, -1,
        Json::object()};
    gesture_ = false;
    edit(w, [&](MapData& m) { m.zones.push_back(zone); });
    gesture_ = false;
    select(Selection::zone(zone.id));
}

void Editor::addPoint(World& w, const std::string& name) {
    if (name.empty()) {
        status_ = "Give the point a name first";
        return;
    }
    auto unique = name;
    for (int n = 2; w.authored().points.contains(unique); ++n) {
        unique = name + std::to_string(n);
    }
    const MapPoint point{dropPoint(w, sceneCamera(w), layer_), {0, 1}, layer_};
    gesture_ = false;
    edit(w, [&](MapData& m) { m.points[unique] = point; });
    gesture_ = false;
    select(Selection::point(unique));
}

void Editor::addLight(World& w) {
    LightSettings light;
    light.position = dropPoint(w, sceneCamera(w), layer_) + glm::vec3(0, 2, 0);
    gesture_ = false;
    edit(w, [&](MapData& m) { m.lights.push_back(light); });
    gesture_ = false;
    select(Selection::light(static_cast<int>(w.authored().lights.size()) - 1));
}

void Editor::addEmitter(World& w) {
    const MapEmitter emitter{dropPoint(w, sceneCamera(w), layer_), EmitterSettings{}};
    gesture_ = false;
    edit(w, [&](MapData& m) { m.emitters.push_back(emitter); });
    gesture_ = false;
    select(Selection::emitter(static_cast<int>(w.authored().emitters.size()) - 1));
}

void Editor::addCollider(World& w) {
    const auto t = dropPoint(w, sceneCamera(w), layer_);
    const Box box{t - glm::vec3(0.5f, 0, 0.5f), t + glm::vec3(0.5f, 1, 0.5f)};
    gesture_ = false;
    edit(w, [&](MapData& m) { m.colliders.push_back(box); });
    gesture_ = false;
    select(Selection::collider(static_cast<int>(w.authored().colliders.size()) - 1));
}

void Editor::drawPlacementForm(World& w, std::size_t index) {
    const auto& placements = w.authored().placements;
    if (index >= placements.size()) {
        return;
    }
    auto p = placements[index];
    const auto previousId = p.id;
    bool changed = false, respawn = false;
    const auto entity = placementEntity(w, p.id);
    if (entity == entt::null) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1),
                           ICON_FA_TRIANGLE_EXCLAMATION " Not spawned in the running game");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("No entity has this id: the type has no spawner, it was hidden by a page, or\n"
                              "the game is not on this map. Save or Play from here to spawn it.");
        }
    }
    if (ImGui::InputText("Id", &p.id)) {
        changed = respawn = true;
    }
    if (ImGui::BeginCombo("Type", p.type.c_str())) {
        // Named: items() of a temporary would outlive what it iterates.
        const auto entityTypes = schemas_.value("entities", Json::object());
        for (const auto& type : entityTypes.items()) {
            if (ImGui::Selectable(type.key().c_str(), type.key() == p.type)) {
                p.type = type.key();
                changed = respawn = true;
            }
            if (const auto doc = type.value().find("doc");
                doc != type.value().end() && doc->is_string() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", doc->get<std::string>().c_str());
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::TreeNode("Advanced")) {
        if (ImGui::InputText("Custom type", &p.type)) {
            changed = respawn = true;
        }
        ImGui::TreePop();
    }
    ImGui::SeparatorText("Transform");
    changed |= editVec3("Position", p.position);
    changed |= ImGui::DragFloat2("Facing", glm::value_ptr(p.facing), 0.05f, -1, 1);
    changed |= editLayer("Layer", p.layer, w.authored().grid);
    changed |= ImGui::DragFloat("Yaw", &p.yaw, 1, -360, 360, "%.0f deg");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Turns mesh placements (props); sprites face the camera");
    }
    float uniform = p.scale.x;
    const bool wasUniform = p.scale.x == p.scale.y && p.scale.y == p.scale.z;
    if (wasUniform) {
        if (ImGui::DragFloat("Scale", &uniform, 0.01f, 0.01f, 100)) {
            p.scale = glm::vec3(uniform);
            changed = true;
            respawn = entity == entt::null || !w.registry().all_of<MeshInstance>(entity);
        }
    } else if (ImGui::DragFloat3("Scale", glm::value_ptr(p.scale), 0.01f, 0.01f, 100)) {
        changed = true;
    }
    ImGui::SeparatorText("Props");
    if (editProps(fieldsFor(p.type, {"entities"}), p.props)) {
        changed = respawn = true;
    }
    if (const auto modelPath = p.props.find("model"); modelPath != p.props.end() && modelPath->is_string()) {
        ImGui::SeparatorText("Collision");
        if (const auto model = engine_.assets().tryModel(modelPath->get<std::string>())) {
            const auto footprint = footprintCells(*model, p);
            if (p.props.contains("pages")) {
                ImGui::TextDisabled("Round collider while solid: a scripted prop shows and hides itself,\n"
                                    "so the walkability bake leaves it out.");
            } else if (const auto solid = p.props.find("solid");
                       solid != p.props.end() && solid->is_boolean() && !solid->get<bool>()) {
                ImGui::TextDisabled("None (solid = false): decoration bodies walk through.");
            } else if (!w.authored().bake.empty()) {
                ImGui::TextDisabled("Blocks through the walkability bake, from the model's %s.",
                                    hasCollisionNodes(*model) ? "collision node(s)" : "visible mesh");
            } else {
                ImGui::TextDisabled("Blocks once the map's walkability is baked (Map tab: Bake now).");
            }
            ImGui::TextDisabled("Footprint: %d x %d cell(s)", footprint.x, footprint.y);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Model props with their origin at the centre of the base and a footprint of whole\n"
                    "cells; a hidden node marked {\"collision\": true} stands in for the visible mesh.");
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Model not found: %s",
                               modelPath->get<std::string>().c_str());
        }
    }
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
        if (p.id != previousId) {
            select(Selection::placement(p.id));
            placementChanged(w, previousId); // the old id's entity goes
        }
        if (respawn) {
            placementChanged(w, p.id);
        } else {
            syncPlacementEntity(w, p);
        }
    }
    if (entity != entt::null) {
        if (const auto inspect = devtool("inspect_entity");
            inspect != sol::lua_nil && (p.props.contains("pages") || p.props.contains("script"))) {
            const auto result = engine_.lua().invoke(inspect, p.id, w.name());
            if (result.valid() && result.get_type() == sol::type::string) {
                const auto info = Json::parse(result.get<std::string>(), nullptr, false);
                if (info.is_object()) {
                    ImGui::SeparatorText("Event pages (live)");
                    for (const auto& page : jsonArray(info, "pages")) {
                        const int index = static_cast<int>(jsonNumber(page, "index", 0));
                        const bool active = jsonBool(page, "active", false);
                        ImGui::PushID(index);
                        if (active) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1, 0.6f, 1));
                        }
                        ImGui::Text("%s#%d %s: %s", active ? ICON_FA_CIRCLE_CHECK " " : "   ", index,
                                    jsonString(page, "trigger", "action").c_str(),
                                    jsonString(page, "script", "(none)").c_str());
                        if (active) {
                            ImGui::PopStyleColor();
                        }
                        if (const auto error = page.find("error");
                            error != page.end() && error->is_string()) {
                            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "   %s",
                                               error->get<std::string>().c_str());
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(ICON_FA_PLAY " Run")) {
                            if (const auto run = devtool("run_page"); run != sol::lua_nil) {
                                if (succeeded(engine_.lua().invoke(run, p.id, index))) {
                                    paused_ = false; // the page needs ticks to play out
                                    status_ = "Running page " + std::to_string(index) + " of " + p.id;
                                }
                            }
                        }
                        ImGui::PopID();
                    }
                    if (const auto running = info.find("running");
                        running != info.end() && running->is_object()) {
                        ImGui::Text(ICON_FA_GEARS " Running: %s", jsonString(*running, "name", "").c_str());
                    }
                    if (!jsonString(info, "routine", "").empty()) {
                        ImGui::TextDisabled("Routine: %s", jsonString(info, "routine", "").c_str());
                    }
                }
            }
        }
        if (ImGui::CollapsingHeader("Live entity")) {
            ImGui::PushID("entity");
            drawEntity(w, entity);
            ImGui::PopID();
        }
    }
}

void Editor::drawZoneForm(World& w, std::size_t index) {
    const auto& zones = w.authored().zones;
    if (index >= zones.size()) {
        return;
    }
    auto z = zones[index];
    const auto previousId = z.id;
    bool changed = ImGui::InputText("Zone id", &z.id);
    if (ImGui::BeginCombo("Zone type", z.type.c_str())) {
        const auto zoneTypes = schemas_.value("zones", Json::object());
        for (const auto& type : zoneTypes.items()) {
            if (ImGui::Selectable(type.key().c_str(), type.key() == z.type)) {
                z.type = type.key();
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::TreeNode("Advanced")) {
        changed |= ImGui::InputText("Custom zone type", &z.type);
        ImGui::TreePop();
    }
    changed |= ImGui::DragFloat2("Min", glm::value_ptr(z.min), 0.1f);
    changed |= ImGui::DragFloat2("Max", glm::value_ptr(z.max), 0.1f);
    changed |= editLayer("Zone layer (-1 all)", z.layer, w.authored().grid, -1);
    ImGui::SeparatorText("Zone props");
    changed |= editProps(fieldsFor(z.type, {"zones", "entities"}), z.props);
    if (const auto problem = changed ? editProblem(zones, z, index) : std::string{}; !problem.empty()) {
        status_ = problem;
    } else if (changed) {
        order(z.min, z.max);
        edit(w, [&](MapData& m) {
            if (index < m.zones.size()) {
                m.zones[index] = z;
            }
        });
        if (z.id != previousId) {
            select(Selection::zone(z.id));
        }
    }
}

void Editor::updateZoneOverlay(World& w) {
    if (selection_.kind != Selection::Kind::Zone) {
        if (w.overlays().erase("editor:zone") > 0) {
            zoneOverlayKey_.clear();
        }
        return;
    }
    const auto* z = findZone(w.authored(), selection_.id);
    if (!z) {
        w.overlays().erase("editor:zone");
        zoneOverlayKey_.clear();
        return;
    }
    const auto key =
        w.name() + "/" + z->id + "/" + Json{z->min.x, z->min.y, z->max.x, z->max.y, z->layer, layer_}.dump();
    if (key != zoneOverlayKey_ || !w.overlays().contains("editor:zone")) {
        zoneOverlayKey_ = key;
        w.overlays()["editor:zone"] =
            OverlayLayer{zoneCells(*z, w.authored().grid, layer_), {1, 0.85f, 0.2f, 0.3f}, 1, 92, true, true};
    }
}

void Editor::drawPointForm(World& w, const std::string& name) {
    const auto& points = w.authored().points;
    const auto found = points.find(name);
    if (found == points.end()) {
        return;
    }
    auto point = found->second;
    bool changed = editVec3("Point position", point.position);
    changed |= ImGui::DragFloat2("Point facing", glm::value_ptr(point.facing), 0.05f, -1, 1);
    changed |= editLayer("Point layer", point.layer, w.authored().grid);
    if (changed) {
        point.position = clampToGrid(w.authored().grid, point.position);
        edit(w, [&](MapData& m) { m.points[name] = point; });
    }
    static std::string rename;
    if (ImGui::InputText("Rename", &rename, ImGuiInputTextFlags_EnterReturnsTrue) && !rename.empty() &&
        !points.contains(rename)) {
        const auto moved = point;
        const auto newName = rename;
        gesture_ = false;
        edit(w, [&](MapData& m) {
            m.points.erase(name);
            m.points[newName] = moved;
        });
        gesture_ = false;
        select(Selection::point(newName));
        rename.clear();
        return;
    }
    const auto playFrom = devtool("play_from");
    ImGui::BeginDisabled(playFrom == sol::lua_nil);
    if (ImGui::Button(ICON_FA_PLAY " Play from here")) {
        // The call-out may unload or replace `w`; the caller holds it alive
        // for the rest of this frame and nothing below reads the map.
        const auto map = w.authored().id;
        if (succeeded(engine_.lua().invoke(playFrom, map, name))) {
            setMode(Mode::Play);
            status_ = "Playing from " + name;
        }
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();
    if (playFrom == sol::lua_nil && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Needs an editor.play_from(map, point) hook in the game's main.lua");
    }
}

void Editor::drawLightForm(World& w, std::size_t index) {
    const auto& lights = w.authored().lights;
    if (index >= lights.size()) {
        return;
    }
    auto l = lights[index];
    bool spot = l.type == LightType::Spot;
    bool changed = false;
    if (ImGui::Checkbox("Spot", &spot)) {
        l.type = spot ? LightType::Spot : LightType::Point;
        changed = true;
    }
    changed |= editVec3("Light position", l.position);
    changed |= editColor("Light colour", l.color);
    changed |= ImGui::DragFloat("Radius", &l.radius, 0.1f, 0.1f, 1000);
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
}

void Editor::drawEmitterForm(World& w, std::size_t index) {
    const auto& emitters = w.authored().emitters;
    if (index >= emitters.size()) {
        return;
    }
    auto e = emitters[index];
    auto& s = e.settings;
    bool changed = editVec3("Emitter position", e.position);
    changed |= assetPicker("Sprite", s.sprite, AssetKind::Sprite, true);
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
}

void Editor::drawColliderForm(World& w, std::size_t index) {
    const auto& colliders = w.authored().colliders;
    if (index >= colliders.size()) {
        return;
    }
    auto box = colliders[index];
    ImGui::TextDisabled("A solid box bodies cannot enter; routes go around the cells it covers");
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
}

void Editor::drawEnvironment(World& w) {
    ImGui::TextDisabled("Edits replace the live environment (scripted changes too)");
    auto env = w.authored().environment;
    // Lists of records are edited as forms from a spec and read back like the map file.
    static const Json backdropSpec = {
        {"type", "list"},
        {"of",
         {{"type", "record"},
          {"fields",
           {{"image", {{"type", "asset"}, {"asset", "image"}, {"doc", "A card behind the scene"}}},
            {"distance", {{"type", "number"}, {"optional", true}, {"default", 60}}},
            {"height", {{"type", "number"}, {"optional", true}, {"default", 20}}},
            {"width", {{"type", "number"}, {"optional", true}, {"default", 80}}},
            {"yaw",
             {{"type", "number"}, {"optional", true}, {"default", 0}, {"doc", "Degrees around the camera"}}},
            {"elevation", {{"type", "number"}, {"optional", true}, {"default", 0}}},
            {"parallax",
             {{"type", "number"},
              {"optional", true},
              {"default", 0.2},
              {"doc", "0 fixed, 1 moves with the world"}}},
            {"tint",
             {{"type", "list"},
              {"of", {{"type", "number"}}},
              {"optional", true},
              {"doc", "[r, g, b, a]"}}}}}}}};
    static const Json customSpec = {{"type", "list"},
                                    {"of",
                                     {{"type", "record"},
                                      {"fields",
                                       {{"shader",
                                         {{"type", "string"},
                                          {"default", "custom"},
                                          {"doc", "shaders/fs_<name>.sc (a-z, 0-9, _ and -)"}}},
                                        {"params",
                                         {{"type", "list"},
                                          {"of", {{"type", "number"}}},
                                          {"optional", true},
                                          {"doc", "Up to 16 numbers (u_params)"}}}}}}}};
    auto editSection = [&](const char* label, const char* pointer) {
        auto value = env.toJson().at(Json::json_pointer(pointer));
        const Json& spec = std::string_view(pointer) == "/sky/backdrops" ? backdropSpec : customSpec;
        if (!editValue(label, spec, value)) {
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
    changed |= assetPicker("Panorama", env.sky.panorama, AssetKind::Image, true);
    changed |= ImGui::SliderFloat("Clouds", &env.sky.cloudCover, 0, 1);
    changed |= ImGui::DragFloat("Cloud speed", &env.sky.cloudSpeed, 0.001f, 0, 1);
    changed |= editColor("Cloud colour", env.sky.cloudColor);
    changed |= editSection("Backdrops", "/sky/backdrops");
    ImGui::SeparatorText("Weather");
    changed |= ImGui::InputText("Weather kind", &env.weather.kind);
    changed |= ImGui::SliderFloat("Weather intensity", &env.weather.intensity, 0, 2);
    changed |= editVec3("Wind", env.weather.wind);
    changed |= editColor("Weather colour", env.weather.color);
    changed |= assetPicker("Weather sprite", env.weather.sprite, AssetKind::Sprite, true);
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
    changed |= ImGui::DragFloat("Pixel scale", &p.pixelScale, 0.05f, 1, 8, "%.2f");
    changed |= assetPicker("LUT", p.lut, AssetKind::Lut, true);
    changed |= assetPicker("LUT blend target", p.lutBlendTarget, AssetKind::Lut, true);
    changed |= ImGui::SliderFloat("LUT blend", &p.lutBlend, 0, 1);
    changed |= editSection("Custom passes", "/post/custom");
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
