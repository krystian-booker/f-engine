#pragma once
#include "engine/world/NavGrid.h"
#include "engine/rpg/GameState.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace fe {
struct Mesh;
struct EventPage {
    Json conditions = Json::object();
    std::string script, trigger = "action", sprite;
    bool solid = true;
    [[nodiscard]] bool matches(const GameState& state) const;
};
struct MapEntity {
    std::string id, name, sprite, clip = "idle";
    glm::vec3 position{0};
    glm::vec2 facing{0,1};
    float speed = 1.5f, radius = 0.25f;
    bool solid = true;
    std::string routine = "stationary";
    std::vector<glm::vec2> route;
    std::vector<EventPage> pages;
};
struct MapLight {
    glm::vec3 position{0,2,0}, color{1,0.75f,0.45f};
    float radius = 5, intensity = 2;
};
struct CameraSettings {
    float pitch = 42, yaw = 0, distance = 17, fov = 30;
    glm::vec2 min{-10000}, max{10000};
};
struct CameraZone { glm::vec2 min{0}, max{0}; CameraSettings camera; };
struct PostSettings {
    bool bloom = true, depthOfField = true, vignette = true, colorGrade = true;
    float bloomStrength = 0.3f, focus = 0.55f, focusRange = 0.25f, exposure = 1;
    float focusDistance=0,aperture=1.5f;
    std::string lut;
};
struct MapData {
    std::string id, name, geometry = "geometry.glb", bgm;
    NavGrid nav;
    std::map<std::string,glm::vec3> spawns;
    std::vector<MapEntity> entities;
    std::vector<MapLight> lights;
    std::vector<CameraZone> cameraZones;
    CameraSettings camera;
    PostSettings post;
    glm::vec3 ambient{0.3f,0.35f,0.45f}, sunColor{1,0.88f,0.7f}, sunDirection{-0.6f,-1,-0.4f};
    glm::vec3 fogColor{0.12f,0.17f,0.23f};
    float fogDensity = 0.025f;
    std::map<int,std::string> encounters;
    static MapData load(const std::filesystem::path& path);
    static MapData fromJson(const Json& data);
    [[nodiscard]] Json toJson() const;
    [[nodiscard]] const CameraSettings& cameraAt(glm::vec3 position) const;
    void importMarkers(const Mesh& geometry);
};
}
