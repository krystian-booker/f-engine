#include "engine/world/MapData.h"
#include <cmath>
#include <set>
#include <stdexcept>

namespace fe {
glm::vec2 jsonVec2(const Json& j) {
    if (!j.is_array() || j.size() != 2) {
        throw std::runtime_error("Expected [x, y]");
    }
    const glm::vec2 v(j[0].get<float>(), j[1].get<float>());
    if (!std::isfinite(v.x) || !std::isfinite(v.y)) {
        throw std::runtime_error("Non-finite coordinates");
    }
    return v;
}

glm::vec3 jsonVec3(const Json& j) {
    if (!j.is_array() || j.size() != 3) {
        throw std::runtime_error("Expected [x, y, z]");
    }
    const glm::vec3 v(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        throw std::runtime_error("Non-finite coordinates");
    }
    return v;
}

glm::vec4 jsonVec4(const Json& j) {
    if (!j.is_array() || (j.size() != 3 && j.size() != 4)) {
        throw std::runtime_error("Expected [r, g, b] or [r, g, b, a]");
    }
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(),
            j.size() == 4 ? j[3].get<float>() : 1.0f};
}

Json toJson(glm::vec2 v) {
    return {v.x, v.y};
}
Json toJson(glm::vec3 v) {
    return {v.x, v.y, v.z};
}
Json toJson(glm::vec4 v) {
    return {v.x, v.y, v.z, v.w};
}

namespace {
template<class T> void read(const Json& j, const char* key, T& value) {
    if (j.contains(key)) {
        value = j.at(key).get<T>();
    }
}
void readVec(const Json& j, const char* key, glm::vec3& value) {
    if (j.contains(key)) {
        value = jsonVec3(j.at(key));
    }
}
void readVec(const Json& j, const char* key, glm::vec4& value) {
    if (j.contains(key)) {
        value = jsonVec4(j.at(key));
    }
}
void requireFinite(std::initializer_list<float> values, const char* what) {
    for (const float v : values) {
        if (!std::isfinite(v)) {
            throw std::runtime_error(std::string("Non-finite ") + what);
        }
    }
}
} // namespace

LightSettings LightSettings::fromJson(const Json& j) {
    LightSettings light;
    light.type = j.value("type", std::string("point")) == "spot" ? LightType::Spot : LightType::Point;
    readVec(j, "position", light.position);
    readVec(j, "color", light.color);
    readVec(j, "direction", light.direction);
    read(j, "radius", light.radius);
    read(j, "intensity", light.intensity);
    read(j, "inner_cone", light.innerCone);
    read(j, "outer_cone", light.outerCone);
    read(j, "flicker", light.flicker);
    read(j, "flicker_speed", light.flickerSpeed);
    read(j, "shadows", light.castShadows);
    requireFinite({light.radius, light.intensity, light.innerCone, light.outerCone, light.flicker}, "light");
    if (light.radius <= 0 || light.intensity < 0 || glm::length(light.direction) < 1e-4f) {
        throw std::runtime_error("Invalid light radius, intensity or direction");
    }
    return light;
}

Json LightSettings::toJson() const {
    return {{"type", type == LightType::Spot ? "spot" : "point"},
            {"position", fe::toJson(position)},
            {"color", fe::toJson(color)},
            {"direction", fe::toJson(direction)},
            {"radius", radius},
            {"intensity", intensity},
            {"inner_cone", innerCone},
            {"outer_cone", outerCone},
            {"flicker", flicker},
            {"flicker_speed", flickerSpeed},
            {"shadows", castShadows}};
}

EmitterSettings EmitterSettings::fromJson(const Json& j) {
    EmitterSettings e;
    read(j, "sprite", e.sprite);
    read(j, "rate", e.rate);
    read(j, "burst", e.burst);
    read(j, "lifetime", e.lifetime);
    read(j, "lifetime_jitter", e.lifetimeJitter);
    readVec(j, "velocity", e.velocity);
    readVec(j, "velocity_jitter", e.velocityJitter);
    readVec(j, "gravity", e.gravity);
    readVec(j, "area", e.area);
    read(j, "size", e.size);
    e.sizeEnd = e.size;
    read(j, "size_end", e.sizeEnd);
    read(j, "drag", e.drag);
    read(j, "stretch", e.stretch);
    readVec(j, "color", e.color);
    readVec(j, "color_end", e.colorEnd);
    read(j, "unlit", e.unlit);
    read(j, "world_space", e.worldSpace);
    read(j, "max", e.maxParticles);
    requireFinite({e.rate, e.lifetime, e.size, e.sizeEnd, e.drag}, "emitter");
    if (e.rate < 0 || e.rate > 5000 || e.lifetime <= 0 || e.lifetime > 120 || e.size <= 0 || e.burst < 0 ||
        e.burst > 4096 || e.maxParticles < 1 || e.maxParticles > 16384) {
        throw std::runtime_error("Invalid emitter settings");
    }
    return e;
}

Json EmitterSettings::toJson() const {
    return {{"sprite", sprite},
            {"rate", rate},
            {"burst", burst},
            {"lifetime", lifetime},
            {"lifetime_jitter", lifetimeJitter},
            {"velocity", fe::toJson(velocity)},
            {"velocity_jitter", fe::toJson(velocityJitter)},
            {"gravity", fe::toJson(gravity)},
            {"area", fe::toJson(area)},
            {"size", size},
            {"size_end", sizeEnd},
            {"drag", drag},
            {"stretch", stretch},
            {"color", fe::toJson(color)},
            {"color_end", fe::toJson(colorEnd)},
            {"unlit", unlit},
            {"world_space", worldSpace},
            {"max", maxParticles}};
}

Environment Environment::fromJson(const Json& j) {
    Environment env;
    readVec(j, "ambient", env.ambient);
    readVec(j, "sun_color", env.sunColor);
    readVec(j, "sun_direction", env.sunDirection);
    readVec(j, "fog_color", env.fogColor);
    read(j, "fog_density", env.fogDensity);
    read(j, "fog_height", env.fogHeight);
    read(j, "fog_height_falloff", env.fogHeightFalloff);
    read(j, "shadows", env.shadows);
    read(j, "shadow_distance", env.shadowDistance);
    if (glm::length(env.sunDirection) < 1e-4f || env.fogDensity < 0 || env.shadowDistance <= 0) {
        throw std::runtime_error("Invalid sun direction, fog density or shadow distance");
    }
    const auto sky = j.value("sky", Json::object());
    read(sky, "mode", env.sky.mode);
    readVec(sky, "top", env.sky.top);
    readVec(sky, "horizon", env.sky.horizon);
    readVec(sky, "bottom", env.sky.bottom);
    read(sky, "panorama", env.sky.panorama);
    read(sky, "cloud_cover", env.sky.cloudCover);
    read(sky, "cloud_speed", env.sky.cloudSpeed);
    readVec(sky, "cloud_color", env.sky.cloudColor);
    for (const auto& b : sky.value("backdrops", Json::array())) {
        Backdrop backdrop;
        read(b, "image", backdrop.image);
        read(b, "distance", backdrop.distance);
        read(b, "height", backdrop.height);
        read(b, "width", backdrop.width);
        read(b, "yaw", backdrop.yaw);
        read(b, "elevation", backdrop.elevation);
        read(b, "parallax", backdrop.parallax);
        readVec(b, "tint", backdrop.tint);
        env.sky.backdrops.push_back(backdrop);
    }
    const auto weather = j.value("weather", Json::object());
    read(weather, "kind", env.weather.kind);
    read(weather, "intensity", env.weather.intensity);
    readVec(weather, "wind", env.weather.wind);
    readVec(weather, "color", env.weather.color);
    read(weather, "sprite", env.weather.sprite);
    const auto post = j.value("post", Json::object());
    auto& p = env.post;
    read(post, "bloom", p.bloom);
    read(post, "depth_of_field", p.depthOfField);
    read(post, "vignette", p.vignette);
    read(post, "color_grade", p.colorGrade);
    read(post, "bloom_strength", p.bloomStrength);
    read(post, "bloom_threshold", p.bloomThreshold);
    read(post, "focus", p.focus);
    read(post, "focus_range", p.focusRange);
    read(post, "focus_distance", p.focusDistance);
    read(post, "aperture", p.aperture);
    read(post, "exposure", p.exposure);
    read(post, "vignette_strength", p.vignetteStrength);
    read(post, "lut", p.lut);
    read(post, "lut_blend_target", p.lutBlendTarget);
    read(post, "lut_blend", p.lutBlend);
    read(post, "pixel_scale", p.pixelScale);
    requireFinite(
        {p.bloomStrength, p.focus, p.focusRange, p.focusDistance, p.aperture, p.exposure, p.lutBlend},
        "post settings");
    if (p.exposure <= 0 || p.pixelScale < 1 || p.pixelScale > 8) {
        throw std::runtime_error("Invalid exposure or pixel_scale");
    }
    return env;
}

Json Environment::toJson() const {
    Json backdrops = Json::array();
    for (const auto& b : sky.backdrops) {
        backdrops.push_back({{"image", b.image},
                             {"distance", b.distance},
                             {"height", b.height},
                             {"width", b.width},
                             {"yaw", b.yaw},
                             {"elevation", b.elevation},
                             {"parallax", b.parallax},
                             {"tint", fe::toJson(b.tint)}});
    }
    return {{"ambient", fe::toJson(ambient)},
            {"sun_color", fe::toJson(sunColor)},
            {"sun_direction", fe::toJson(sunDirection)},
            {"fog_color", fe::toJson(fogColor)},
            {"fog_density", fogDensity},
            {"fog_height", fogHeight},
            {"fog_height_falloff", fogHeightFalloff},
            {"shadows", shadows},
            {"shadow_distance", shadowDistance},
            {"sky",
             {{"mode", sky.mode},
              {"top", fe::toJson(sky.top)},
              {"horizon", fe::toJson(sky.horizon)},
              {"bottom", fe::toJson(sky.bottom)},
              {"panorama", sky.panorama},
              {"cloud_cover", sky.cloudCover},
              {"cloud_speed", sky.cloudSpeed},
              {"cloud_color", fe::toJson(sky.cloudColor)},
              {"backdrops", backdrops}}},
            {"weather",
             {{"kind", weather.kind},
              {"intensity", weather.intensity},
              {"wind", fe::toJson(weather.wind)},
              {"color", fe::toJson(weather.color)},
              {"sprite", weather.sprite}}},
            {"post",
             {{"bloom", post.bloom},
              {"depth_of_field", post.depthOfField},
              {"vignette", post.vignette},
              {"color_grade", post.colorGrade},
              {"bloom_strength", post.bloomStrength},
              {"bloom_threshold", post.bloomThreshold},
              {"focus", post.focus},
              {"focus_range", post.focusRange},
              {"focus_distance", post.focusDistance},
              {"aperture", post.aperture},
              {"exposure", post.exposure},
              {"vignette_strength", post.vignetteStrength},
              {"lut", post.lut},
              {"lut_blend_target", post.lutBlendTarget},
              {"lut_blend", post.lutBlend},
              {"pixel_scale", post.pixelScale}}}};
}

MapData MapData::load(const std::filesystem::path& path) {
    try {
        return fromJson(readJson(path));
    } catch (const std::exception& e) {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
}

MapData MapData::fromJson(const Json& j) {
    if (j.value("format", 0) != 2) {
        throw std::runtime_error("Map format must be 2 (run tools/migrate_maps.py on older maps)");
    }
    MapData m;
    m.id = j.at("id");
    m.name = j.value("name", m.id);
    m.geometry = j.value("geometry", m.geometry);
    m.grid = Grid::fromJson(j.at("grid"));
    if (m.id.empty() ||
        m.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) {
        throw std::runtime_error("Map ids use lowercase letters, digits, '_' and '-'");
    }
    const auto pointsJson = j.value("points", Json::object());
    for (const auto& [name, p] : pointsJson.items()) {
        MapPoint point;
        point.position = jsonVec3(p.at("position"));
        point.layer = p.value("layer", 0);
        if (p.contains("facing")) {
            point.facing = jsonVec2(p.at("facing"));
        }
        if (!m.grid.contains(static_cast<int>(std::floor(point.position.x)),
                             static_cast<int>(std::floor(point.position.z)), point.layer)) {
            throw std::runtime_error("Point '" + name + "' lies outside the grid");
        }
        m.points[name] = point;
    }
    std::set<std::string> ids;
    for (const auto& p : j.value("placements", Json::array())) {
        Placement placement;
        placement.id = p.at("id");
        placement.type = p.at("type");
        placement.position = jsonVec3(p.at("position"));
        placement.layer = p.value("layer", 0);
        placement.props = p.value("props", Json::object());
        if (p.contains("facing")) {
            placement.facing = jsonVec2(p.at("facing"));
        }
        if (placement.id.empty() || !ids.insert(placement.id).second) {
            throw std::runtime_error("Duplicate or empty placement id: " + placement.id);
        }
        m.placements.push_back(std::move(placement));
    }
    for (const auto& z : j.value("zones", Json::array())) {
        Zone zone;
        zone.id = z.at("id");
        zone.type = z.value("type", std::string("zone"));
        zone.min = jsonVec2(z.at("min"));
        zone.max = jsonVec2(z.at("max"));
        zone.layer = z.value("layer", -1);
        zone.props = z.value("props", Json::object());
        if (zone.min.x > zone.max.x || zone.min.y > zone.max.y) {
            throw std::runtime_error("Zone '" + zone.id + "' has min greater than max");
        }
        m.zones.push_back(std::move(zone));
    }
    for (const auto& l : j.value("lights", Json::array())) {
        m.lights.push_back(LightSettings::fromJson(l));
    }
    for (const auto& e : j.value("emitters", Json::array())) {
        m.emitters.push_back({jsonVec3(e.at("position")), EmitterSettings::fromJson(e)});
    }
    for (const auto& c : j.value("colliders", Json::array())) {
        Box box{jsonVec3(c.at("min")), jsonVec3(c.at("max"))};
        if (glm::any(glm::greaterThan(box.min, box.max))) {
            throw std::runtime_error("Collider min must not exceed max");
        }
        m.colliders.push_back(box);
    }
    m.environment = Environment::fromJson(j.value("environment", Json::object()));
    m.props = j.value("props", Json::object());
    return m;
}

Json MapData::toJson() const {
    Json points = Json::object(), placementsJson = Json::array(), zonesJson = Json::array(),
         lightsJson = Json::array(), emittersJson = Json::array(), collidersJson = Json::array();
    for (const auto& [name, p] : this->points) {
        points[name] = {
            {"position", fe::toJson(p.position)}, {"facing", fe::toJson(p.facing)}, {"layer", p.layer}};
    }
    for (const auto& p : placements) {
        placementsJson.push_back({{"id", p.id},
                                  {"type", p.type},
                                  {"position", fe::toJson(p.position)},
                                  {"facing", fe::toJson(p.facing)},
                                  {"layer", p.layer},
                                  {"props", p.props}});
    }
    for (const auto& z : zones) {
        zonesJson.push_back({{"id", z.id},
                             {"type", z.type},
                             {"min", fe::toJson(z.min)},
                             {"max", fe::toJson(z.max)},
                             {"layer", z.layer},
                             {"props", z.props}});
    }
    for (const auto& l : lights) {
        lightsJson.push_back(l.toJson());
    }
    for (const auto& e : emitters) {
        auto out = e.settings.toJson();
        out["position"] = fe::toJson(e.position);
        emittersJson.push_back(out);
    }
    for (const auto& c : colliders) {
        collidersJson.push_back({{"min", fe::toJson(c.min)}, {"max", fe::toJson(c.max)}});
    }
    return {{"format", 2},
            {"id", id},
            {"name", name},
            {"geometry", geometry},
            {"grid", grid.toJson()},
            {"points", points},
            {"placements", placementsJson},
            {"zones", zonesJson},
            {"lights", lightsJson},
            {"emitters", emittersJson},
            {"colliders", collidersJson},
            {"environment", environment.toJson()},
            {"props", props}};
}
} // namespace fe
