#pragma once
#include "engine/nav/Grid.h"
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

namespace fe {
// Tactical presentation data: highlighted cells, path arrows and the grid
// cursor. Scripts write it; the renderer draws it.
struct OverlayLayer {
    std::vector<GridPos> cells;
    glm::vec4 color{0.3f, 0.6f, 1, 0.45f};
    float pulse = 0;
    int order = 0;
    bool visible = true, border = true;
};

struct OverlayPath {
    std::vector<GridPos> cells;
    glm::vec4 color{1, 0.9f, 0.4f, 0.9f};
};

struct OverlayCursor {
    bool visible = false;
    GridPos cell;
    glm::vec4 color{1, 1, 1, 0.9f};
};

struct Overlays {
    std::map<std::string, OverlayLayer> layers;
    std::map<std::string, OverlayPath> paths;
    OverlayCursor cursor;
};
} // namespace fe
