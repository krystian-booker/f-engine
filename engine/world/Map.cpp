#include "engine/world/Map.h"
#include "engine/assets/Mesh.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace fe {
namespace {
glm::vec2 vec2(const Json& j) {
    if (!j.is_array() || j.size() != 2) throw std::runtime_error("Expected two coordinates");
    const glm::vec2 v(j[0].get<float>(),j[1].get<float>());
    if (!std::isfinite(v.x) || !std::isfinite(v.y)) throw std::runtime_error("Non-finite coordinates");
    return v;
}
glm::vec3 vec3(const Json& j) {
    if (!j.is_array() || j.size() != 3) throw std::runtime_error("Expected three coordinates");
    const glm::vec3 v(j[0].get<float>(),j[1].get<float>(),j[2].get<float>());
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) throw std::runtime_error("Non-finite coordinates");
    return v;
}
Json json(glm::vec2 v) { return {v.x,v.y}; }
Json json(glm::vec3 v) { return {v.x,v.y,v.z}; }
CameraSettings readCamera(const Json& j, CameraSettings c = {}) {
    c.pitch = j.value("pitch",c.pitch); c.yaw = j.value("yaw",c.yaw); c.distance = j.value("distance",c.distance); c.fov = j.value("fov",c.fov);
    if (j.contains("min")) c.min = vec2(j.at("min"));
    if (j.contains("max")) c.max = vec2(j.at("max"));
    if (!std::isfinite(c.pitch) || !std::isfinite(c.yaw) || !std::isfinite(c.distance) || !std::isfinite(c.fov) || c.pitch < 5 || c.pitch > 85 || c.distance <= 0 || c.distance > 1000 ||
        c.fov < 5 || c.fov > 100 || c.min.x > c.max.x || c.min.y > c.max.y) throw std::runtime_error("Invalid camera settings");
    return c;
}
Json cameraJson(const CameraSettings& c) { return {{"pitch",c.pitch},{"yaw",c.yaw},{"distance",c.distance},
    {"fov",c.fov},{"min",json(c.min)},{"max",json(c.max)}}; }
}
bool EventPage::matches(const GameState& state) const {
    if (conditions.contains("flags")) for (const auto& [flag,value] : conditions.at("flags").items())
        if (state.flag(flag) != value.get<bool>()) return false;
    if (conditions.contains("variables")) for (const auto& c : conditions.at("variables")) {
        const double value = state.variable(c.at("id"));
        const double expected = c.at("value"); const auto op = c.value("op", "==");
        if (op == "==" ? value != expected : op == "!=" ? value == expected : op == ">=" ? value < expected :
            op == "<=" ? value > expected : op == ">" ? value <= expected : op == "<" ? value >= expected : true) return false;
    }
    if (conditions.contains("item") && state.itemCount(conditions.at("item")) < 1) return false;
    if (conditions.contains("actor")) {
        const auto id = conditions.at("actor").get<std::string>();
        if (std::find(state.party.begin(),state.party.end(),id) == state.party.end()) return false;
    }
    return true;
}
MapData MapData::load(const std::filesystem::path& path) {
    try { return fromJson(readJson(path)); }
    catch (const std::exception& e) { throw std::runtime_error(path.string() + ": " + e.what()); }
}
MapData MapData::fromJson(const Json& j) {
    MapData m; m.id = j.at("id"); m.name = j.value("name",m.id); m.nav = NavGrid::fromJson(j.at("nav"));
    m.geometry = j.value("geometry","geometry.glb"); m.bgm = j.value("bgm","");
    for (const auto& [name,p] : j.at("spawns").items()) {
        auto position = vec3(p);
        if (!m.nav.canStand({position.x,position.z})) throw std::runtime_error("Spawn '" + name + "' is outside walkable cells");
        position.y = m.nav.heightAt({position.x,position.z}); m.spawns[name] = position;
    }
    if (m.id.empty() || m.spawns.empty()) throw std::runtime_error("Map needs an id and at least one spawn");
    std::set<std::string> ids;
    for (const auto& e : j.value("entities",Json::array())) {
        MapEntity out; out.id = e.at("id"); out.name = e.value("name",out.id);
        if (out.id.empty() || out.id == "player" || !ids.insert(out.id).second) throw std::runtime_error("Duplicate/reserved entity id: " + out.id);
        out.position = vec3(e.at("position")); out.sprite = e.value("sprite",""); out.clip = e.value("clip","idle");
        out.facing = vec2(e.value("facing",Json::array({0,1}))); out.solid = e.value("solid",true);
        out.speed = e.value("speed",1.5f); out.radius = e.value("radius",0.25f); out.routine = e.value("routine","stationary");
        if (!std::isfinite(out.speed) || !std::isfinite(out.radius) || out.speed < 0 || out.speed > 20 || out.radius < 0 || out.radius > 0.49f) throw std::runtime_error("Invalid entity movement settings");
        if (out.routine!="stationary" && out.routine!="patrol" && out.routine!="wander") throw std::runtime_error("Unknown NPC routine: "+out.routine);
        for (const auto& point : e.value("route",Json::array())) out.route.push_back(vec2(point));
        for (const auto& p : e.value("pages",Json::array())) {
            EventPage page; page.conditions = p.value("conditions",Json::object()); page.script = p.value("script","");
            page.trigger = p.value("trigger","action"); page.sprite = p.value("sprite",out.sprite); page.solid = p.value("solid",out.solid);
            if (page.trigger != "action" && page.trigger != "touch" && page.trigger != "autorun" && page.trigger != "parallel")
                throw std::runtime_error("Unknown event trigger: " + page.trigger);
            out.pages.push_back(std::move(page));
        }
        m.entities.push_back(std::move(out));
    }
    m.camera = readCamera(j.value("camera",Json::object()));
    for (const auto& z : j.value("camera_zones",Json::array())) {
        CameraZone zone; zone.min = vec2(z.at("min")); zone.max = vec2(z.at("max")); zone.camera = readCamera(z.at("camera"),m.camera);
        if (zone.min.x>zone.max.x || zone.min.y>zone.max.y) throw std::runtime_error("Invalid camera zone bounds");
        m.cameraZones.push_back(zone);
    }
    for (const auto& l : j.value("lights",Json::array())) {
        MapLight light; light.position = vec3(l.at("position")); light.color = vec3(l.value("color",json(light.color)));
        light.radius = l.value("radius",5.0f); light.intensity = l.value("intensity",2.0f);
        if (!std::isfinite(light.radius) || !std::isfinite(light.intensity) || light.radius <= 0 || light.intensity < 0) throw std::runtime_error("Invalid point light");
        m.lights.push_back(light);
    }
    if (j.contains("ambient")) m.ambient = vec3(j.at("ambient"));
    if (j.contains("sun_color")) m.sunColor = vec3(j.at("sun_color"));
    if (j.contains("sun_direction")) m.sunDirection = vec3(j.at("sun_direction"));
    if (glm::length(m.sunDirection) < 0.001f) throw std::runtime_error("Sun direction cannot be zero");
    if (j.contains("fog_color")) m.fogColor = vec3(j.at("fog_color"));
    m.fogDensity = j.value("fog_density",m.fogDensity);
    const auto post = j.value("post",Json::object());
    m.post.bloom = post.value("bloom",true); m.post.depthOfField = post.value("depth_of_field",true);
    m.post.vignette = post.value("vignette",true); m.post.colorGrade = post.value("color_grade",true);
    m.post.bloomStrength = post.value("bloom_strength",0.3f); m.post.focus = post.value("focus",0.55f);
    m.post.focusRange = post.value("focus_range",0.25f); m.post.exposure = post.value("exposure",1.0f); m.post.lut = post.value("lut","");
    m.post.focusDistance=post.value("focus_distance",0.0f);m.post.aperture=post.value("aperture",1.5f);
    for (const float value:{m.fogDensity,m.post.bloomStrength,m.post.focusRange,m.post.focusDistance,m.post.aperture})
        if (!std::isfinite(value) || value<0) throw std::runtime_error("Fog and post effect values must be finite and nonnegative");
    if (!std::isfinite(m.post.exposure) || m.post.exposure<=0 || !std::isfinite(m.post.focus) || m.post.focus<0 || m.post.focus>1) throw std::runtime_error("Invalid exposure or focus");
    const auto encounterTables = j.value("encounters",Json::object());
    for (const auto& [region,id] : encounterTables.items()) m.encounters[std::stoi(region)] = id;
    return m;
}
Json MapData::toJson() const {
    Json spawnsJson = Json::object(), entitiesJson = Json::array(), lightsJson = Json::array(), zonesJson = Json::array(), encounterJson = Json::object();
    for (const auto& [name,p] : spawns) spawnsJson[name] = json(p);
    for (const auto& e : entities) {
        Json pages = Json::array(), route = Json::array();
        for (const auto& p : e.pages) pages.push_back({{"conditions",p.conditions},{"script",p.script},{"trigger",p.trigger},{"sprite",p.sprite},{"solid",p.solid}});
        for (const auto& p : e.route) route.push_back(json(p));
        entitiesJson.push_back({{"id",e.id},{"name",e.name},{"position",json(e.position)},{"facing",json(e.facing)},
            {"sprite",e.sprite},{"clip",e.clip},{"speed",e.speed},{"radius",e.radius},{"solid",e.solid},
            {"routine",e.routine},{"route",route},{"pages",pages}});
    }
    for (const auto& l : lights) lightsJson.push_back({{"position",json(l.position)},{"color",json(l.color)},{"radius",l.radius},{"intensity",l.intensity}});
    for (const auto& z : cameraZones) zonesJson.push_back({{"min",json(z.min)},{"max",json(z.max)},{"camera",cameraJson(z.camera)}});
    for (const auto& [region,id] : encounters) encounterJson[std::to_string(region)] = id;
    return {{"id",id},{"name",name},{"geometry",geometry},{"bgm",bgm},{"nav",nav.toJson()},{"spawns",spawnsJson},
        {"entities",entitiesJson},{"lights",lightsJson},{"camera",cameraJson(camera)},{"camera_zones",zonesJson},
        {"ambient",json(ambient)},{"sun_color",json(sunColor)},{"sun_direction",json(sunDirection)},
        {"fog_color",json(fogColor)},{"fog_density",fogDensity},{"encounters",encounterJson},
        {"post",{{"bloom",post.bloom},{"depth_of_field",post.depthOfField},{"vignette",post.vignette},{"color_grade",post.colorGrade},
            {"bloom_strength",post.bloomStrength},{"focus",post.focus},{"focus_range",post.focusRange},{"exposure",post.exposure},{"lut",post.lut},
            {"focus_distance",post.focusDistance},{"aperture",post.aperture}}}};
}
const CameraSettings& MapData::cameraAt(glm::vec3 p) const {
    for (auto it = cameraZones.rbegin(); it != cameraZones.rend(); ++it)
        if (p.x >= it->min.x && p.x <= it->max.x && p.z >= it->min.y && p.z <= it->max.y) return it->camera;
    return camera;
}
void MapData::importMarkers(const Mesh& geometry) {
    auto imported=*this;
    for (const auto& marker:geometry.markers) {
        const auto& properties=marker.properties;
        if (properties.contains("collision") && properties.at("collision")!=false && marker.hasBounds) {
            const auto config=properties.at("collision").is_object()?properties.at("collision"):Json::object();
            const bool walkable=config.value("walkable",false);
            const int x0=std::max(0,int(std::floor(marker.min.x))),x1=std::min(nav.width(),int(std::ceil(marker.max.x)));
            const int z0=std::max(0,int(std::floor(marker.min.z))),z1=std::min(nav.depth(),int(std::ceil(marker.max.z)));
            for (int z=z0;z<z1;++z) for (int x=x0;x<x1;++x) {
                auto& cell=imported.nav.at(x,z);cell.walkable=walkable;
                if (walkable) cell.height=config.value("height",marker.max.y);
            }
        }
        if (properties.contains("spawn") && properties.at("spawn")!=false) {
            const auto name=properties.at("spawn").is_string()?properties.at("spawn").get<std::string>():marker.name;
            imported.spawns[name]={marker.position.x,imported.nav.heightAt({marker.position.x,marker.position.z}),marker.position.z};
        }
        if (properties.contains("light") && properties.at("light")!=false) {
            const auto data=properties.at("light").is_object()?properties.at("light"):Json::object();
            const auto color=vec3(data.value("color",Json::array({1,.75,.45})));
            if (data.value("type",std::string("point"))=="directional") {
                imported.sunColor=color*data.value("intensity",1.0f);imported.sunDirection=vec3(data.at("direction"));
            } else {
                MapLight light;light.position=marker.position;light.color=color;light.intensity=data.value("intensity",2.0f);light.radius=data.value("radius",5.0f);
                const auto found=std::find_if(imported.lights.begin(),imported.lights.end(),[&](const auto& existing){return glm::length(existing.position-light.position)<.001f;});
                if (found==imported.lights.end()) imported.lights.push_back(light);else *found=light;
            }
        }
    }
    // Reuse the authored-map validator and commit the import as one operation.
    *this=MapData::fromJson(imported.toJson());
}
}
