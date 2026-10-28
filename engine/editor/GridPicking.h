#pragma once
#include "engine/world/NavGrid.h"
#include <optional>

namespace fe {
struct GridHit { glm::ivec2 cell{0};glm::vec3 position{0};float distance=0; };
std::optional<GridHit> pickGrid(const NavGrid& grid,glm::vec3 rayOrigin,glm::vec3 rayDirection);
}
