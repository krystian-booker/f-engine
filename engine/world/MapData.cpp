#include "engine/world/MapData.h"
#include "engine/assets/Model.h"
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace fe {
namespace {
// A JSON number as a float. Settings tables from scripts reach these readers
// as JSON, so a value beyond the float range is an error rather than an
// undefined conversion.
float jsonFloat(const Json& j) {
    const double value = j.get<double>();
    if (!std::isfinite(value) || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
        throw std::runtime_error("Expected a finite number, got " + j.dump());
    }
    return static_cast<float>(value);
}
} // namespace

glm::vec2 jsonVec2(const Json& j) {
    if (!j.is_array() || j.size() != 2) {
        throw std::runtime_error("Expected [x, y]");
    }
    const glm::vec2 v(jsonFloat(j[0]), jsonFloat(j[1]));
    if (!std::isfinite(v.x) || !std::isfinite(v.y)) {
        throw std::runtime_error("Non-finite coordinates");
    }
    return v;
}

glm::vec3 jsonVec3(const Json& j) {
    if (!j.is_array() || j.size() != 3) {
        throw std::runtime_error("Expected [x, y, z]");
    }
    const glm::vec3 v(jsonFloat(j[0]), jsonFloat(j[1]), jsonFloat(j[2]));
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        throw std::runtime_error("Non-finite coordinates");
    }
    return v;
}

glm::vec4 jsonVec4(const Json& j) {
    if (!j.is_array() || (j.size() != 3 && j.size() != 4)) {
        throw std::runtime_error("Expected [r, g, b] or [r, g, b, a]");
    }
    return {jsonFloat(j[0]), jsonFloat(j[1]), jsonFloat(j[2]), j.size() == 4 ? jsonFloat(j[3]) : 1.0f};
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
// Numbers are range-checked: a whole number outside an integer field's type,
// or a fraction, is an error instead of wrapping or truncating.
template<class T> void read(const Json& j, const char* key, T& value) {
    if (!j.contains(key)) {
        return;
    }
    const auto& v = j.at(key);
    if constexpr (std::is_floating_point_v<T>) {
        if (v.is_number()) {
            value = jsonFloat(v);
            return;
        }
    } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        if (v.is_number()) {
            const double number = v.get<double>();
            const bool fits =
                v.is_number_unsigned()
                    ? v.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<T>::max())
                : v.is_number_integer()
                    ? v.get<std::int64_t>() >= static_cast<std::int64_t>(std::numeric_limits<T>::min()) &&
                          v.get<std::int64_t>() <= static_cast<std::int64_t>(std::numeric_limits<T>::max())
                    : std::floor(number) == number &&
                          number >= static_cast<double>(std::numeric_limits<T>::min()) &&
                          number <= static_cast<double>(std::numeric_limits<T>::max());
            if (!fits) {
                throw std::runtime_error(std::string("'") + key + "' must be an integer from " +
                                         std::to_string(std::numeric_limits<T>::min()) + " to " +
                                         std::to_string(std::numeric_limits<T>::max()) + ", got " + v.dump());
            }
            value = v.is_number_float() ? static_cast<T>(number) : v.get<T>();
            return;
        }
    }
    value = v.get<T>();
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
float noise(double t, float seed) {
    return static_cast<float>(std::sin(t * 1.7 + seed) * 0.5 + std::sin(t * 4.3 + seed * 2.1) * 0.3 +
                              std::sin(t * 9.1 + seed * 0.7) * 0.2);
}

float sampleCurve(const std::vector<glm::vec2>& keys, double time) {
    const double period = keys.back().x;
    const auto t = static_cast<float>(period > 0 ? std::fmod(time, period) : 0.0);
    if (t <= keys.front().x) {
        return keys.front().y;
    }
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].x) {
            const auto& a = keys[i - 1];
            const auto& b = keys[i];
            return glm::mix(a.y, b.y, (t - a.x) / std::max(1e-6f, b.x - a.x));
        }
    }
    return keys.back().y;
}
} // namespace

const char* blendModeName(BlendMode mode) {
    switch (mode) {
    case BlendMode::Alpha:
        return "alpha";
    case BlendMode::Additive:
        return "additive";
    case BlendMode::Multiply:
        return "multiply";
    case BlendMode::Premultiplied:
        return "premultiplied";
    case BlendMode::Opaque:
        break;
    }
    return "opaque";
}

BlendMode blendModeFromName(const std::string& name) {
    for (const auto mode : {BlendMode::Opaque, BlendMode::Alpha, BlendMode::Additive, BlendMode::Multiply,
                            BlendMode::Premultiplied}) {
        if (name == blendModeName(mode)) {
            return mode;
        }
    }
    throw std::runtime_error("Unknown blend mode '" + name +
                             "' (alpha, additive, multiply, premultiplied or opaque)");
}

BlendMode blendModeFromJson(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? BlendMode::Alpha : BlendMode::Opaque;
    }
    return blendModeFromName(value.get<std::string>());
}

float LightSettings::flickerScale(double time, float seed) const {
    float scale = flickerCurve.empty() ? 1.0f : sampleCurve(flickerCurve, time);
    if (flicker > 0) {
        scale *= std::max(0.0f, 1 + flicker * noise(time * flickerSpeed, seed));
    }
    return scale;
}

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
    for (const auto& key : j.value("flicker_curve", Json::array())) {
        const auto k = jsonVec2(key);
        if (k.x < 0 || k.y < 0 || (!light.flickerCurve.empty() && k.x <= light.flickerCurve.back().x)) {
            throw std::runtime_error("flicker_curve keys are [seconds, scale] with increasing times and "
                                     "non-negative values");
        }
        light.flickerCurve.push_back(k);
    }
    read(j, "shadows", light.castShadows);
    if (light.radius <= 0 || light.radius > MaxLightRadius || light.intensity < 0 ||
        glm::length(light.direction) < 1e-4f) {
        throw std::runtime_error("Invalid light radius (above 0, at most 1000), intensity or direction");
    }
    if (light.type == LightType::Spot &&
        !(light.innerCone >= 0 && light.innerCone < light.outerCone && light.outerCone <= 90)) {
        throw std::runtime_error("A spot light needs 0 <= inner_cone < outer_cone <= 90 (degrees)");
    }
    return light;
}

Json LightSettings::toJson() const {
    Json curve = Json::array();
    for (const auto& key : flickerCurve) {
        curve.push_back(fe::toJson(key));
    }
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
            {"flicker_curve", curve},
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
    if (j.contains("blend")) {
        e.blend = blendModeFromJson(j.at("blend"));
    }
    read(j, "unlit", e.unlit);
    read(j, "world_space", e.worldSpace);
    read(j, "distort", e.distort);
    read(j, "max", e.maxParticles);
    if (e.distort < 0 || e.rate < 0 || e.rate > 5000 || e.lifetime <= 0 || e.lifetime > 120 || e.size <= 0 ||
        e.burst < 0 || e.burst > 4096 || e.maxParticles < 1 || e.maxParticles > 16384) {
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
            {"blend", blendModeName(blend)},
            {"unlit", unlit},
            {"world_space", worldSpace},
            {"distort", distort},
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
    read(j, "fog_max", env.fogMax);
    read(j, "shadows", env.shadows);
    read(j, "shadow_distance", env.shadowDistance);
    read(j, "shadow_strength", env.shadowStrength);
    read(j, "shadow_bias", env.shadowBias);
    read(j, "sprite_light_height", env.spriteLightHeight);
    read(j, "anisotropic", env.anisotropic);
    read(j, "pixel_snap", env.pixelSnap);
    if (glm::length(env.sunDirection) < 1e-4f || env.fogDensity < 0 || env.shadowDistance <= 0 ||
        env.fogMax < 0 || env.fogMax > 1 || env.shadowStrength < 0 || env.shadowStrength > 1 ||
        env.shadowBias < 0) {
        throw std::runtime_error(
            "Invalid sun direction, fog density, fog_max, shadow distance, strength or bias");
    }
    const auto water = j.value("water", Json::object());
    read(water, "wave_scale", env.water.waveScale);
    read(water, "wave_speed", env.water.waveSpeed);
    read(water, "fresnel", env.water.fresnel);
    read(water, "specular", env.water.specular);
    readVec(water, "deep_color", env.water.deepColor);
    read(water, "depth_falloff", env.water.depthFalloff);
    read(water, "reflection", env.water.reflection);
    read(water, "refraction", env.water.refraction);
    if (env.water.depthFalloff < 0 || env.water.reflection < 0 || env.water.reflection > 1 ||
        env.water.refraction < 0) {
        throw std::runtime_error("Water depth_falloff and refraction must not be negative and reflection "
                                 "lies in 0..1");
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
    if (env.weather.intensity < 0) {
        throw std::runtime_error("weather.intensity must not be negative");
    }
    const auto post = j.value("post", Json::object());
    auto& p = env.post;
    read(post, "bloom", p.bloom);
    read(post, "depth_of_field", p.depthOfField);
    read(post, "vignette", p.vignette);
    read(post, "color_grade", p.colorGrade);
    read(post, "bloom_strength", p.bloomStrength);
    read(post, "bloom_threshold", p.bloomThreshold);
    read(post, "bloom_knee", p.bloomKnee);
    read(post, "bloom_radius", p.bloomRadius);
    read(post, "bloom_levels", p.bloomLevels);
    read(post, "dof_mode", p.dofMode);
    read(post, "dof_radius", p.dofRadius);
    read(post, "ssao", p.ssao);
    read(post, "ssao_radius", p.ssaoRadius);
    read(post, "ssao_strength", p.ssaoStrength);
    read(post, "ssao_bias", p.ssaoBias);
    read(post, "antialias", p.antialias);
    read(post, "chromatic_aberration", p.chromaticAberration);
    read(post, "distortion", p.distortion);
    read(post, "focus", p.focus);
    read(post, "focus_range", p.focusRange);
    read(post, "focus_distance", p.focusDistance);
    read(post, "aperture", p.aperture);
    read(post, "exposure", p.exposure);
    read(post, "vignette_strength", p.vignetteStrength);
    read(post, "tonemap", p.tonemap);
    read(post, "lut", p.lut);
    read(post, "lut_blend_target", p.lutBlendTarget);
    read(post, "lut_blend", p.lutBlend);
    read(post, "pixel_scale", p.pixelScale);
    for (const auto& c : post.value("custom", Json::array())) {
        CustomPost custom;
        read(c, "shader", custom.shader);
        if (c.contains("params") && !c.at("params").empty()) { // Lua turns {} into an object
            if (!c.at("params").is_array()) {
                throw std::runtime_error("post.custom params must be a list of numbers");
            }
            for (const auto& value : c.at("params")) {
                custom.params.push_back(jsonFloat(value));
            }
        }
        if (!isShaderName(custom.shader)) {
            throw std::runtime_error("post.custom shader names use a-z, 0-9, _ and - (at most 64)");
        }
        if (custom.params.size() > 16) {
            throw std::runtime_error("post.custom params take at most 16 numbers");
        }
        p.custom.push_back(std::move(custom));
    }
    if (p.exposure <= 0) {
        throw std::runtime_error("Invalid exposure");
    }
    if (!(p.pixelScale >= 1 && p.pixelScale <= 8)) {
        throw std::runtime_error("post.pixel_scale is a number from 1 to 8");
    }
    if (p.focus < 0 || p.focus > 1 || p.focusRange < 0 || p.focusRange > 1) {
        throw std::runtime_error("post.focus and post.focus_range lie in 0..1");
    }
    if (p.bloomLevels < 1 || p.bloomLevels > 8 || p.bloomRadius < 0 || p.bloomRadius > 1 || p.bloomKnee < 0 ||
        p.bloomKnee > 1 || p.dofRadius < 0 || p.dofRadius > 32 || p.ssaoRadius <= 0 || p.custom.size() > 8) {
        throw std::runtime_error("Invalid bloom_levels (1-8), bloom_radius or bloom_knee (0-1), dof_radius "
                                 "(0-32), ssao_radius or custom pass count (at most 8)");
    }
    if (p.tonemap != "aces" && p.tonemap != "none") {
        throw std::runtime_error("post.tonemap must be \"aces\" or \"none\"");
    }
    if (p.dofMode != "depth" && p.dofMode != "tilt" && p.dofMode != "both") {
        throw std::runtime_error("post.dof_mode must be \"depth\", \"tilt\" or \"both\"");
    }
    if (p.antialias != "auto" && p.antialias != "fxaa" && p.antialias != "none") {
        throw std::runtime_error("post.antialias must be \"auto\", \"fxaa\" or \"none\"");
    }
    return env;
}

Json Environment::toJson() const {
    Json custom = Json::array();
    for (const auto& c : post.custom) {
        custom.push_back({{"shader", c.shader}, {"params", c.params}});
    }
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
            {"fog_max", fogMax},
            {"shadows", shadows},
            {"shadow_distance", shadowDistance},
            {"shadow_strength", shadowStrength},
            {"shadow_bias", shadowBias},
            {"sprite_light_height", spriteLightHeight},
            {"anisotropic", anisotropic},
            {"pixel_snap", pixelSnap},
            {"water",
             {{"wave_scale", water.waveScale},
              {"wave_speed", water.waveSpeed},
              {"fresnel", water.fresnel},
              {"specular", water.specular},
              {"deep_color", fe::toJson(water.deepColor)},
              {"depth_falloff", water.depthFalloff},
              {"reflection", water.reflection},
              {"refraction", water.refraction}}},
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
              {"bloom_knee", post.bloomKnee},
              {"bloom_radius", post.bloomRadius},
              {"bloom_levels", post.bloomLevels},
              {"dof_mode", post.dofMode},
              {"dof_radius", post.dofRadius},
              {"ssao", post.ssao},
              {"ssao_radius", post.ssaoRadius},
              {"ssao_strength", post.ssaoStrength},
              {"ssao_bias", post.ssaoBias},
              {"antialias", post.antialias},
              {"chromatic_aberration", post.chromaticAberration},
              {"distortion", post.distortion},
              {"custom", custom},
              {"focus", post.focus},
              {"focus_range", post.focusRange},
              {"focus_distance", post.focusDistance},
              {"aperture", post.aperture},
              {"exposure", post.exposure},
              {"vignette_strength", post.vignetteStrength},
              {"tonemap", post.tonemap},
              {"lut", post.lut},
              {"lut_blend_target", post.lutBlendTarget},
              {"lut_blend", post.lutBlend},
              {"pixel_scale", post.pixelScale == std::floor(post.pixelScale)
                                  ? Json(static_cast<int>(post.pixelScale))
                                  : Json(post.pixelScale)}}}};
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
        if (!m.grid.inRange(floorCell(point.position.x), floorCell(point.position.z), point.layer)) {
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
        placement.yaw = p.value("yaw", 0.0f);
        if (const auto scale = p.find("scale"); scale != p.end()) {
            placement.scale = scale->is_number() ? glm::vec3(scale->get<float>()) : jsonVec3(*scale);
        }
        if (!std::isfinite(placement.yaw) || !std::isfinite(placement.scale.x) ||
            !std::isfinite(placement.scale.y) || !std::isfinite(placement.scale.z)) {
            throw std::runtime_error("Placement '" + placement.id + "' has a non-finite yaw or scale");
        }
        if (placement.id.empty() || !ids.insert(placement.id).second) {
            throw std::runtime_error("Duplicate or empty placement id: " + placement.id);
        }
        if (placement.layer < 0 || placement.layer >= m.grid.layerCount()) {
            throw std::runtime_error("Placement '" + placement.id + "' uses a layer the grid does not have");
        }
        m.placements.push_back(std::move(placement));
    }
    std::set<std::string> zoneIds;
    for (const auto& z : j.value("zones", Json::array())) {
        Zone zone;
        zone.id = z.at("id");
        if (zone.id.empty() || !zoneIds.insert(zone.id).second) {
            throw std::runtime_error("Duplicate or empty zone id: " + zone.id);
        }
        zone.type = z.value("type", std::string("zone"));
        zone.min = jsonVec2(z.at("min"));
        zone.max = jsonVec2(z.at("max"));
        zone.layer = z.value("layer", -1);
        zone.props = z.value("props", Json::object());
        if (zone.min.x > zone.max.x || zone.min.y > zone.max.y) {
            throw std::runtime_error("Zone '" + zone.id + "' has min greater than max");
        }
        if (zone.layer < -1 || zone.layer >= m.grid.layerCount()) {
            throw std::runtime_error("Zone '" + zone.id + "' uses a layer the grid does not have (-1 = all)");
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
    if (const auto bake = j.find("bake"); bake != j.end() && bake->is_object()) {
        m.bake.autoBake = bake->value("auto", true);
        m.bake.fingerprint = bake->value("fingerprint", std::string());
        m.bake.options = bake->value("options", Json::object());
        for (const auto& c : bake->value("colliders", Json::array())) {
            Box box{jsonVec3(c.at("min")), jsonVec3(c.at("max"))};
            if (glm::any(glm::greaterThan(box.min, box.max))) {
                throw std::runtime_error("Baked collider min must not exceed max");
            }
            m.bake.colliders.push_back(box);
        }
        for (const auto& o : bake->value("overrides", Json::array())) {
            CellOverride over;
            over.x = o.at("x");
            over.z = o.at("z");
            over.layer = o.value("layer", 0);
            over.w = o.value("w", 1);
            over.d = o.value("d", 1);
            const auto where = "Bake override at (" + std::to_string(over.x) + ", " + std::to_string(over.z) + ")";
            if (over.w < 1 || over.d < 1 || !m.grid.inRange(over.x, over.z, over.layer) ||
                !m.grid.inRange(over.x + over.w - 1, over.z + over.d - 1, over.layer)) {
                throw std::runtime_error(where + " lies outside the grid");
            }
            if (o.contains("walkable")) {
                over.walkable = o.at("walkable").get<bool>();
            }
            if (o.contains("height")) {
                over.height = o.at("height").get<float>();
            }
            if (o.contains("ramp")) {
                const auto name = o.at("ramp").get<std::string>();
                const auto ramp = name.size() == 1 ? rampFromCode(name[0]) : rampFromName(name);
                if (!ramp) {
                    throw std::runtime_error(where + ": unknown ramp '" + name + "'");
                }
                over.ramp = *ramp;
            }
            over.rise = o.value("rise", 1.0f);
            if ((over.height && !std::isfinite(*over.height)) || !std::isfinite(over.rise)) {
                throw std::runtime_error(where + " has a non-finite height or rise");
            }
            for (const auto& tag : o.value("tags", Json::array())) {
                over.tags.push_back(tag.get<std::string>());
            }
            for (const auto& tag : o.value("untags", Json::array())) {
                over.untags.push_back(tag.get<std::string>());
            }
            m.bake.overrides.push_back(std::move(over));
        }
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
        Json out = {{"id", p.id},
                    {"type", p.type},
                    {"position", fe::toJson(p.position)},
                    {"facing", fe::toJson(p.facing)},
                    {"layer", p.layer},
                    {"props", p.props}};
        // Absent when at their defaults, so maps without them re-serialise unchanged.
        if (p.yaw != 0) {
            out["yaw"] = p.yaw;
        }
        if (p.scale != glm::vec3(1)) {
            out["scale"] = p.scale.x == p.scale.y && p.scale.y == p.scale.z ? Json(p.scale.x) : fe::toJson(p.scale);
        }
        placementsJson.push_back(std::move(out));
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
    Json out = {{"format", 2},
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
    // Absent until a map is baked, so hand-made maps re-serialise unchanged.
    if (!bake.empty()) {
        Json bakedColliders = Json::array(), overrides = Json::array();
        for (const auto& c : bake.colliders) {
            bakedColliders.push_back({{"min", fe::toJson(c.min)}, {"max", fe::toJson(c.max)}});
        }
        for (const auto& o : bake.overrides) {
            Json over = {{"x", o.x}, {"z", o.z}, {"layer", o.layer}};
            if (o.w != 1 || o.d != 1) {
                over["w"] = o.w;
                over["d"] = o.d;
            }
            if (o.walkable) {
                over["walkable"] = *o.walkable;
            }
            if (o.height) {
                over["height"] = *o.height;
            }
            if (o.ramp) {
                over["ramp"] = std::string(1, rampCode(*o.ramp));
                over["rise"] = o.rise;
            }
            if (!o.tags.empty()) {
                over["tags"] = o.tags;
            }
            if (!o.untags.empty()) {
                over["untags"] = o.untags;
            }
            overrides.push_back(std::move(over));
        }
        out["bake"] = {{"auto", bake.autoBake},
                       {"fingerprint", bake.fingerprint},
                       {"options", bake.options},
                       {"colliders", bakedColliders},
                       {"overrides", overrides}};
    }
    return out;
}
} // namespace fe
