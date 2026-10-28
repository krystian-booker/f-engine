#include "engine/editor/Editor.h"
#include "engine/core/Paths.h"
#include "engine/nav/Query.h"
#include "engine/runtime/Engine.h"
#include <algorithm>
#include <cstdio>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <set>

namespace fe {
namespace {
bool inputText(const char* label, std::string& value, ImGuiInputTextFlags flags = 0) {
    char buffer[1024];
    std::snprintf(buffer, sizeof buffer, "%s", value.c_str());
    if (ImGui::InputText(label, buffer, sizeof buffer, flags)) {
        value = buffer;
        return true;
    }
    return false;
}

bool inputJson(const char* label, Json& value) {
    std::string text = value.dump(2);
    char buffer[8192];
    std::snprintf(buffer, sizeof buffer, "%s", text.c_str());
    if (ImGui::InputTextMultiline(label, buffer, sizeof buffer, ImVec2(-1, 120))) {
        const auto parsed = Json::parse(buffer, nullptr, false);
        if (!parsed.is_discarded()) {
            value = parsed;
            return true;
        }
    }
    return false;
}

bool editVec3(const char* label, glm::vec3& v, float speed = 0.05f) {
    return ImGui::DragFloat3(label, glm::value_ptr(v), speed);
}
bool editColor(const char* label, glm::vec3& v) {
    return ImGui::ColorEdit3(label, glm::value_ptr(v), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
}
} // namespace

Editor::Editor(Engine& engine) : engine_(engine) {
}

void Editor::toggle() {
    open_ = !open_;
    if (open_) {
        refreshSchemas();
    } else if (auto* w = world()) {
        for (auto it = w->overlays().begin(); it != w->overlays().end();) {
            it = it->first.starts_with("editor:") ? w->overlays().erase(it) : std::next(it);
        }
    }
}

bool Editor::capturesInput() const {
    if (!open_) {
        return false;
    }
    const auto& io = ImGui::GetIO();
    return io.WantCaptureKeyboard || io.WantCaptureMouse;
}

World* Editor::world() {
    if (const auto w = engine_.world(worldName_)) {
        return w.get();
    }
    const auto focus = engine_.focusWorld();
    if (focus) {
        worldName_ = focus->name();
    }
    return focus.get();
}

void Editor::refreshSchemas() {
    const auto described = engine_.lua().console("require('fe.devtools').describe()");
    const auto parsed = Json::parse(described, nullptr, false);
    schemas_ = parsed.is_object() ? parsed : Json::object();
}

void Editor::checkpoint() {
    if (auto* w = world()) {
        undo_.push_back(w->map().toJson());
        if (undo_.size() > 64) {
            undo_.pop_front();
        }
        redo_.clear();
    }
}

void Editor::restore(const Json& snapshot) {
    if (auto* w = world()) {
        try {
            w->map() = MapData::fromJson(snapshot);
        } catch (const std::exception& error) {
            status_ = error.what();
        }
    }
}

void Editor::saveMap() {
    auto* w = world();
    if (!w) {
        return;
    }
    try {
        const auto json = w->map().toJson();
        MapData::fromJson(json); // validate before writing
        writeJson(resolveInside(engine_.lua().gameRoot(), "maps/" + w->map().id + "/map.json"), json);
        status_ = "Saved maps/" + w->map().id + "/map.json";
    } catch (const std::exception& error) {
        status_ = error.what();
    }
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
}

void Editor::drawWorldTab() {
    if (ImGui::BeginCombo("World", worldName_.c_str())) {
        for (const auto& [name, w] : engine_.worlds()) {
            (void)w;
            if (ImGui::Selectable(name.c_str(), name == worldName_)) {
                worldName_ = name;
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
    std::vector<entt::entity> entities;
    for (const auto entity : registry.view<Transform>()) {
        entities.push_back(entity);
    }
    ImGui::Text("%zu entities, %zu particles, %zu lights", entities.size(), w->particles().size(),
                w->activeLights().size());
    ImGui::BeginChild("entities", ImVec2(0, 180), true);
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const auto* name = registry.try_get<Name>(entities[i]);
        const auto label = (name ? name->id : "#" + std::to_string(static_cast<std::uint32_t>(entities[i]))) +
                           "##" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), selectedEntity_ == static_cast<int>(i))) {
            selectedEntity_ = static_cast<int>(i);
        }
    }
    ImGui::EndChild();
    if (selectedEntity_ < 0 || selectedEntity_ >= static_cast<int>(entities.size())) {
        return;
    }
    const auto entity = entities[static_cast<std::size_t>(selectedEntity_)];
    auto& t = registry.get<Transform>(entity);
    editVec3("Position", t.position);
    ImGui::InputInt("Layer", &t.layer);
    ImGui::DragFloat2("Facing", glm::value_ptr(t.facing), 0.05f);
    if (auto* sprite = registry.try_get<Sprite>(entity)) {
        ImGui::SeparatorText("Sprite");
        inputText("Animation", sprite->animation);
        inputText("Clip", sprite->clip);
        ImGui::Checkbox("Visible", &sprite->visible);
        ImGui::ColorEdit4("Tint", glm::value_ptr(sprite->tint));
        ImGui::ColorEdit4("Outline", glm::value_ptr(sprite->outline));
    }
    if (auto* mover = registry.try_get<Mover>(entity)) {
        ImGui::SeparatorText("Mover");
        ImGui::DragFloat("Speed", &mover->speed, 0.05f, 0, 20);
        ImGui::Checkbox("Collide", &mover->collide);
    }
    if (auto* props = registry.try_get<Props>(entity)) {
        ImGui::SeparatorText("Props");
        inputJson("##props", props->value);
    }
}

std::optional<GridPos> Editor::hoveredCell(World& w) {
    const auto& io = ImGui::GetIO();
    if (io.WantCaptureMouse) {
        return std::nullopt;
    }
    const auto viewport = engine_.viewport();
    const glm::vec2 mouse(io.MousePos.x * io.DisplayFramebufferScale.x,
                          io.MousePos.y * io.DisplayFramebufferScale.y);
    const glm::vec2 ndc(mouse.x / static_cast<float>(viewport.x) * 2 - 1,
                        1 - mouse.y / static_cast<float>(viewport.y) * 2);
    glm::vec3 origin, direction;
    w.camera().ray(ndc, static_cast<float>(viewport.x) / static_cast<float>(viewport.y), origin, direction);
    const auto hit = raycastGrid(w.grid(), origin, direction, w.camera().farPlane);
    if (!hit) {
        return std::nullopt;
    }
    return hit->cell;
}

void Editor::updateOverlays(World& w) {
    auto& grid = w.grid();
    std::vector<GridPos> blocked, tagged;
    const auto tagMask = tag_.empty() ? 0u : grid.tagMask({tag_});
    for (int z = 0; z < grid.depth(); ++z) {
        for (int x = 0; x < grid.width(); ++x) {
            const auto& cell = grid.at(x, z, std::clamp(layer_, 0, grid.layerCount() - 1));
            if (!cell.walkable) {
                blocked.push_back({x, z, layer_});
            }
            if (tagMask && (cell.tags & tagMask)) {
                tagged.push_back({x, z, layer_});
            }
        }
    }
    auto& overlays = w.overlays();
    if (!showGrid_) {
        overlays.erase("editor:blocked");
        overlays.erase("editor:tag");
        return;
    }
    overlays["editor:blocked"] = OverlayLayer{blocked, {1, 0.25f, 0.3f, 0.35f}, 0, 90, true, true};
    overlays["editor:tag"] = OverlayLayer{tagged, {0.3f, 0.9f, 1, 0.35f}, 0, 91, true, true};
}

void Editor::drawPaint(World& w) {
    auto& grid = w.grid();
    ImGui::Checkbox("Show grid overlay", &showGrid_);
    ImGui::SliderInt("Layer", &layer_, 0, grid.layerCount() - 1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add layer")) {
        checkpoint();
        layer_ = grid.addLayer("layer" + std::to_string(grid.layerCount()));
    }
    const char* modes[] = {"Walkable", "Height", "Ramp", "Add tag", "Remove tag"};
    ImGui::Combo("Brush", &paintMode_, modes, 5);
    ImGui::SliderInt("Radius", &brushRadius_, 0, 4);
    if (paintMode_ == 0) {
        ImGui::Checkbox("Walkable", &walkable_);
    } else if (paintMode_ == 1) {
        ImGui::DragFloat("Height", &height_, 0.25f, -20, 40);
    } else if (paintMode_ == 2) {
        const char* ramps[] = {"none", "north", "east", "south", "west"};
        ImGui::Combo("Ramp", &ramp_, ramps, 5);
    }
    inputText("Tag", tag_);
    ImGui::Checkbox("Paint with left mouse", &painting_);
    updateOverlays(w);
    const auto cell = hoveredCell(w);
    if (cell) {
        const auto& c = grid.at(cell->x, cell->z, std::clamp(layer_, 0, grid.layerCount() - 1));
        ImGui::Text("Cell %d,%d  height %.2f  %s", cell->x, cell->z, c.height,
                    c.walkable ? "walkable" : "blocked");
        w.cursor() = OverlayCursor{true, {cell->x, cell->z, layer_}, {1, 1, 1, 0.8f}};
    }
    if (!painting_ || !cell || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        return;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        checkpoint();
    }
    for (int dz = -brushRadius_; dz <= brushRadius_; ++dz) {
        for (int dx = -brushRadius_; dx <= brushRadius_; ++dx) {
            const GridPos p{cell->x + dx, cell->z + dz, layer_};
            if (!grid.contains(p)) {
                continue;
            }
            auto& target = grid.at(p.x, p.z, p.layer);
            switch (paintMode_) {
            case 0:
                target.walkable = walkable_;
                break;
            case 1:
                target.height = height_;
                break;
            case 2:
                target.ramp = static_cast<Ramp>(ramp_);
                break;
            case 3:
                if (!tag_.empty()) {
                    target.tags |= grid.tagBit(tag_, true);
                }
                break;
            default:
                if (!tag_.empty()) {
                    target.tags &= ~grid.tagBit(tag_, false);
                }
                break;
            }
        }
    }
}

bool Editor::editProps(const std::string& schemaName, bool entity, Json& props) {
    const auto& group = schemas_.value(entity ? "entities" : "schemas", Json::object());
    if (!group.contains(schemaName)) {
        return inputJson("Props", props);
    }
    bool changed = false;
    const auto& fields = group.at(schemaName).value("fields", Json::object());
    for (const auto& [name, spec] : fields.items()) {
        const auto type = spec.value("type", std::string("any"));
        const auto label = name + (spec.value("optional", false) ? "" : " *");
        ImGui::PushID(name.c_str());
        if (type == "string" || type == "ref") {
            std::string value = props.value(name, std::string{});
            const auto hint =
                type == "ref" ? label + " (" + spec.value("table", std::string{}) + " id)" : label;
            if (inputText(hint.c_str(), value)) {
                props[name] = value;
                changed = true;
            }
        } else if (type == "number" || type == "integer") {
            double value = props.value(name, spec.value("default", 0.0));
            if (ImGui::InputDouble(label.c_str(), &value)) {
                props[name] = type == "integer" ? Json(static_cast<long long>(value)) : Json(value);
                changed = true;
            }
        } else if (type == "boolean") {
            bool value = props.value(name, spec.value("default", false));
            if (ImGui::Checkbox(label.c_str(), &value)) {
                props[name] = value;
                changed = true;
            }
        } else if (type == "enum") {
            const auto options = spec.value("options", std::vector<std::string>{});
            std::string value = props.value(name, spec.value("default", std::string{}));
            if (ImGui::BeginCombo(label.c_str(), value.c_str())) {
                for (const auto& option : options) {
                    if (ImGui::Selectable(option.c_str(), option == value)) {
                        props[name] = option;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            Json value = props.value(name, Json());
            ImGui::TextUnformatted(label.c_str());
            if (inputJson("##json", value)) {
                props[name] = value;
                changed = true;
            }
        }
        if (spec.contains("doc") && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", spec.at("doc").get<std::string>().c_str());
        }
        ImGui::PopID();
    }
    return changed;
}

void Editor::drawPlacements(World& w) {
    auto& placements = w.map().placements;
    if (ImGui::Button("Add placement")) {
        checkpoint();
        Placement p;
        p.id = "placement" + std::to_string(placements.size() + 1);
        p.type = "npc";
        p.position = w.camera().target;
        placements.push_back(p);
        selectedPlacement_ = static_cast<int>(placements.size()) - 1;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Click map to move", &placing_);
    ImGui::BeginChild("placements", ImVec2(0, 120), true);
    for (std::size_t i = 0; i < placements.size(); ++i) {
        const auto label = placements[i].id + " (" + placements[i].type + ")";
        if (ImGui::Selectable(label.c_str(), selectedPlacement_ == static_cast<int>(i))) {
            selectedPlacement_ = static_cast<int>(i);
        }
    }
    ImGui::EndChild();
    if (selectedPlacement_ < 0 || selectedPlacement_ >= static_cast<int>(placements.size())) {
        return;
    }
    auto& p = placements[static_cast<std::size_t>(selectedPlacement_)];
    if (placing_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (const auto cell = hoveredCell(w)) {
            checkpoint();
            p.position = {static_cast<float>(cell->x) + 0.5f,
                          w.grid().heightAt({cell->x + 0.5f, cell->z + 0.5f}, cell->layer),
                          static_cast<float>(cell->z) + 0.5f};
            p.layer = cell->layer;
        }
    }
    if (ImGui::IsItemActivated()) {
        checkpoint();
    }
    inputText("Id", p.id);
    if (ImGui::BeginCombo("Type", p.type.c_str())) {
        for (const auto& [type, spec] : schemas_.value("entities", Json::object()).items()) {
            (void)spec;
            if (ImGui::Selectable(type.c_str(), type == p.type)) {
                p.type = type;
            }
        }
        ImGui::EndCombo();
    }
    inputText("Custom type", p.type);
    editVec3("Position", p.position);
    ImGui::DragFloat2("Facing", glm::value_ptr(p.facing), 0.05f, -1, 1);
    ImGui::InputInt("Layer", &p.layer);
    ImGui::SeparatorText("Props");
    editProps(p.type, true, p.props);
    if (ImGui::Button("Delete placement")) {
        checkpoint();
        placements.erase(placements.begin() + selectedPlacement_);
        selectedPlacement_ = -1;
    }
}

void Editor::drawZones(World& w) {
    auto& zones = w.map().zones;
    if (ImGui::Button("Add zone")) {
        checkpoint();
        const auto t = w.camera().target;
        zones.push_back({"zone" + std::to_string(zones.size() + 1),
                         "camera",
                         {t.x - 2, t.z - 2},
                         {t.x + 2, t.z + 2},
                         -1,
                         Json::object()});
        selectedZone_ = static_cast<int>(zones.size()) - 1;
    }
    for (std::size_t i = 0; i < zones.size(); ++i) {
        if (ImGui::Selectable((zones[i].id + " (" + zones[i].type + ")").c_str(),
                              selectedZone_ == static_cast<int>(i))) {
            selectedZone_ = static_cast<int>(i);
        }
    }
    if (selectedZone_ < 0 || selectedZone_ >= static_cast<int>(zones.size())) {
        return;
    }
    auto& z = zones[static_cast<std::size_t>(selectedZone_)];
    inputText("Zone id", z.id);
    inputText("Zone type", z.type);
    ImGui::DragFloat2("Min", glm::value_ptr(z.min), 0.1f);
    ImGui::DragFloat2("Max", glm::value_ptr(z.max), 0.1f);
    ImGui::InputInt("Zone layer (-1 all)", &z.layer);
    editProps(z.type, true, z.props);
    std::vector<GridPos> cells;
    for (int cz = static_cast<int>(std::floor(z.min.y)); cz < static_cast<int>(std::ceil(z.max.y)); ++cz) {
        for (int cx = static_cast<int>(std::floor(z.min.x)); cx < static_cast<int>(std::ceil(z.max.x));
             ++cx) {
            if (w.grid().contains(cx, cz, 0)) {
                cells.push_back({cx, cz, 0});
            }
        }
    }
    w.overlays()["editor:zone"] = OverlayLayer{cells, {1, 0.85f, 0.2f, 0.3f}, 1, 92, true, true};
    if (ImGui::Button("Delete zone")) {
        checkpoint();
        zones.erase(zones.begin() + selectedZone_);
        selectedZone_ = -1;
        w.overlays().erase("editor:zone");
    }
}

void Editor::drawLights(World& w) {
    auto& lights = w.map().lights;
    if (ImGui::Button("Add light")) {
        checkpoint();
        LightSettings light;
        light.position = w.camera().target + glm::vec3(0, 2, 0);
        lights.push_back(light);
        selectedLight_ = static_cast<int>(lights.size()) - 1;
    }
    for (std::size_t i = 0; i < lights.size(); ++i) {
        if (ImGui::Selectable(("Light " + std::to_string(i)).c_str(),
                              selectedLight_ == static_cast<int>(i))) {
            selectedLight_ = static_cast<int>(i);
        }
    }
    if (selectedLight_ < 0 || selectedLight_ >= static_cast<int>(lights.size())) {
        return;
    }
    auto& l = lights[static_cast<std::size_t>(selectedLight_)];
    bool spot = l.type == LightType::Spot;
    if (ImGui::Checkbox("Spot", &spot)) {
        l.type = spot ? LightType::Spot : LightType::Point;
    }
    editVec3("Light position", l.position);
    editColor("Light colour", l.color);
    ImGui::DragFloat("Radius", &l.radius, 0.1f, 0.1f, 100);
    ImGui::DragFloat("Intensity", &l.intensity, 0.05f, 0, 50);
    ImGui::DragFloat("Flicker", &l.flicker, 0.01f, 0, 1);
    if (spot) {
        editVec3("Direction", l.direction);
        ImGui::DragFloat("Inner cone", &l.innerCone, 0.5f, 1, 89);
        ImGui::DragFloat("Outer cone", &l.outerCone, 0.5f, 1, 89);
    }
    if (ImGui::Button("Delete light")) {
        checkpoint();
        lights.erase(lights.begin() + selectedLight_);
        selectedLight_ = -1;
    }
}

void Editor::drawEnvironment(World& w) {
    auto& env = w.environment();
    editColor("Ambient", env.ambient);
    editColor("Sun colour", env.sunColor);
    editVec3("Sun direction", env.sunDirection, 0.01f);
    editColor("Fog colour", env.fogColor);
    ImGui::DragFloat("Fog density", &env.fogDensity, 0.001f, 0, 1);
    ImGui::DragFloat("Fog height", &env.fogHeight, 0.1f);
    ImGui::Checkbox("Shadows", &env.shadows);
    ImGui::DragFloat("Shadow distance", &env.shadowDistance, 0.5f, 4, 200);
    ImGui::SeparatorText("Sky");
    inputText("Sky mode", env.sky.mode);
    editColor("Sky top", env.sky.top);
    editColor("Sky horizon", env.sky.horizon);
    editColor("Sky bottom", env.sky.bottom);
    ImGui::SliderFloat("Clouds", &env.sky.cloudCover, 0, 1);
    ImGui::SeparatorText("Weather");
    inputText("Weather kind", env.weather.kind);
    ImGui::SliderFloat("Weather intensity", &env.weather.intensity, 0, 2);
    editVec3("Wind", env.weather.wind);
    ImGui::SeparatorText("Post");
    auto& p = env.post;
    ImGui::Checkbox("Bloom", &p.bloom);
    ImGui::SameLine();
    ImGui::Checkbox("Depth of field", &p.depthOfField);
    ImGui::SameLine();
    ImGui::Checkbox("Vignette", &p.vignette);
    ImGui::Checkbox("Colour grade", &p.colorGrade);
    ImGui::SliderFloat("Bloom strength", &p.bloomStrength, 0, 2);
    ImGui::SliderFloat("Focus line", &p.focus, 0, 1);
    ImGui::SliderFloat("Focus range", &p.focusRange, 0, 1);
    ImGui::SliderFloat("Aperture", &p.aperture, 0, 8);
    ImGui::SliderFloat("Exposure", &p.exposure, 0.1f, 4);
    ImGui::SliderInt("Pixel scale", &p.pixelScale, 1, 8);
    inputText("LUT", p.lut);
    inputText("LUT blend target", p.lutBlendTarget);
    ImGui::SliderFloat("LUT blend", &p.lutBlend, 0, 1);
}

void Editor::drawPoints(World& w) {
    auto& points = w.map().points;
    static std::string name = "new_point";
    inputText("Point name", name);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add at camera")) {
        checkpoint();
        points[name] = MapPoint{w.camera().target, {0, 1}, 0};
    }
    for (auto it = points.begin(); it != points.end();) {
        ImGui::PushID(it->first.c_str());
        ImGui::TextUnformatted(it->first.c_str());
        editVec3("##position", it->second.position);
        const bool erase = ImGui::SmallButton("Remove");
        ImGui::PopID();
        it = erase ? points.erase(it) : std::next(it);
    }
}

void Editor::importMarkers(World& w) {
    const auto model = engine_.assets().tryModel(w.geometryPath());
    if (!model) {
        status_ = "Geometry not found";
        return;
    }
    checkpoint();
    auto& map = w.map();
    int imported = 0;
    for (const auto& marker : model->markers) {
        const auto& props = marker.properties;
        if (props.contains("collision") && marker.hasBounds) {
            const auto config = props.at("collision").is_object() ? props.at("collision") : Json::object();
            for (int z = std::max(0, static_cast<int>(std::floor(marker.min.z)));
                 z < std::min(map.grid.depth(), static_cast<int>(std::ceil(marker.max.z))); ++z) {
                for (int x = std::max(0, static_cast<int>(std::floor(marker.min.x)));
                     x < std::min(map.grid.width(), static_cast<int>(std::ceil(marker.max.x))); ++x) {
                    auto& cell = map.grid.at(x, z, config.value("layer", 0));
                    cell.walkable = config.value("walkable", false);
                    if (cell.walkable) {
                        cell.height = config.value("height", marker.max.y);
                    }
                }
            }
            ++imported;
        }
        if (props.contains("spawn")) {
            const auto name =
                props.at("spawn").is_string() ? props.at("spawn").get<std::string>() : marker.name;
            map.points[name] =
                MapPoint{{marker.position.x, map.grid.heightAt({marker.position.x, marker.position.z}),
                          marker.position.z},
                         {0, 1},
                         0};
            ++imported;
        }
        if (props.contains("light") &&
            props.at("light").value("type", std::string("point")) != "directional") {
            auto light = LightSettings::fromJson(props.at("light"));
            light.position = marker.position;
            map.lights.push_back(light);
            ++imported;
        }
    }
    status_ = "Imported " + std::to_string(imported) + " markers";
}

void Editor::drawMapTab() {
    auto* w = world();
    if (!w) {
        ImGui::TextDisabled("No world loaded");
        return;
    }
    ImGui::Text("%s (%s)  %dx%d, %d layer(s)", w->map().name.c_str(), w->map().id.c_str(), w->grid().width(),
                w->grid().depth(), w->grid().layerCount());
    if (ImGui::Button("Save map")) {
        saveMap();
    }
    ImGui::SameLine();
    if (ImGui::Button("Undo") && !undo_.empty()) {
        redo_.push_back(w->map().toJson());
        restore(undo_.back());
        undo_.pop_back();
    }
    ImGui::SameLine();
    if (ImGui::Button("Redo") && !redo_.empty()) {
        undo_.push_back(w->map().toJson());
        restore(redo_.back());
        redo_.pop_back();
    }
    ImGui::SameLine();
    if (ImGui::Button("Import Blender markers")) {
        importMarkers(*w);
    }
    if (ImGui::CollapsingHeader("Grid painting", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawPaint(*w);
    }
    if (ImGui::CollapsingHeader("Placements")) {
        drawPlacements(*w);
    }
    if (ImGui::CollapsingHeader("Zones")) {
        drawZones(*w);
    }
    if (ImGui::CollapsingHeader("Lights")) {
        drawLights(*w);
    }
    if (ImGui::CollapsingHeader("Points")) {
        drawPoints(*w);
    }
    if (ImGui::CollapsingHeader("Environment")) {
        drawEnvironment(*w);
    }
    if (ImGui::CollapsingHeader("Map props")) {
        inputJson("##mapprops", w->map().props);
    }
}

void Editor::drawDataTab() {
    if (ImGui::Button("Refresh schemas")) {
        refreshSchemas();
    }
    const auto tables = schemas_.value("tables", Json::array());
    if (ImGui::BeginCombo("Table", dataTable_.c_str())) {
        for (const auto& t : tables) {
            const auto name = t.value("name", std::string{});
            if (ImGui::Selectable(name.c_str(), name == dataTable_)) {
                dataTable_ = name;
                dataRecord_ = -1;
                try {
                    dataDocument_ =
                        readJson(resolveInside(engine_.lua().gameRoot(), t.at("file").get<std::string>()));
                } catch (const std::exception& error) {
                    status_ = error.what();
                }
            }
        }
        ImGui::EndCombo();
    }
    const auto table = std::find_if(tables.begin(), tables.end(),
                                    [&](const Json& t) { return t.value("name", "") == dataTable_; });
    if (table == tables.end() || !dataDocument_.is_array()) {
        ImGui::TextDisabled("Pick a table (array files are editable here)");
        return;
    }
    ImGui::BeginChild("records", ImVec2(0, 160), true);
    for (std::size_t i = 0; i < dataDocument_.size(); ++i) {
        const auto id = dataDocument_[i].value("id", std::to_string(i));
        if (ImGui::Selectable(id.c_str(), dataRecord_ == static_cast<int>(i))) {
            dataRecord_ = static_cast<int>(i);
        }
    }
    ImGui::EndChild();
    if (ImGui::Button("Add record")) {
        dataDocument_.push_back({{"id", "new_" + std::to_string(dataDocument_.size())}});
        dataRecord_ = static_cast<int>(dataDocument_.size()) - 1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save table")) {
        try {
            writeJson(resolveInside(engine_.lua().gameRoot(), table->at("file").get<std::string>()),
                      dataDocument_);
            status_ = "Saved " + table->at("file").get<std::string>() + " (hot reloads)";
        } catch (const std::exception& error) {
            status_ = error.what();
        }
    }
    if (dataRecord_ >= 0 && dataRecord_ < static_cast<int>(dataDocument_.size())) {
        auto& record = dataDocument_[static_cast<std::size_t>(dataRecord_)];
        const auto schema = table->value("schema", std::string{});
        if (!schema.empty()) {
            editProps(schema, false, record);
        } else {
            inputJson("Record", record);
        }
    }
}

void Editor::drawConsoleTab() {
    ImGui::BeginChild("log", ImVec2(0, -30), true);
    for (const auto& line : consoleLog_) {
        ImGui::TextWrapped("%s", line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    if (inputText("Lua", consoleInput_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        consoleLog_.push_back("> " + consoleInput_);
        consoleLog_.push_back(engine_.lua().console(consoleInput_));
        consoleInput_.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
}

void Editor::drawProfilerTab(double frameMilliseconds, std::uint32_t drawCalls) {
    const auto& stats = engine_.stats();
    ImGui::Text("Frame %.2f ms (%.0f fps)", frameMilliseconds,
                frameMilliseconds > 0 ? 1000.0 / frameMilliseconds : 0.0);
    ImGui::Text("Lua update %.2f ms, draw %.2f ms", stats.luaUpdate, stats.luaDraw);
    ImGui::Text("World draw calls %u", drawCalls);
    ImGui::Text("Canvas quads %zu", engine_.canvas().quads().size());
    ImGui::Text("Tasks %s, Lua memory %s KB", engine_.lua().console("require('fe.task').count()").c_str(),
                engine_.lua().console("math.floor(collectgarbage('count'))").c_str());
    for (const auto& [name, w] : engine_.worlds()) {
        ImGui::Text("World %s: %zu entities, %zu particles, %zu lights", name.c_str(),
                    w->registry().view<Transform>().size(), w->particles().size(), w->activeLights().size());
    }
    if (!stats.zones.empty()) {
        ImGui::SeparatorText("Script zones");
        for (const auto& [name, ms] : stats.zones) {
            ImGui::Text("%s: %.3f ms", name.c_str(), ms);
        }
    }
}

void Editor::drawLaunchTab() {
    const auto list = engine_.lua().console("table.concat(require('fe.devtools').list(), '\\n')");
    std::size_t start = 0;
    bool any = false;
    while (start < list.size()) {
        const auto end = list.find('\n', start);
        const auto name = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!name.empty() && ImGui::Button(name.c_str(), ImVec2(-1, 0))) {
            engine_.lua().console("require('fe.devtools').launch(" + Json(name).dump() + ")");
            paused_ = false;
        }
        any = any || !name.empty();
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (!any) {
        ImGui::TextDisabled("Register launchers with require('fe.devtools').register(name, fn)");
    }
}
} // namespace fe
