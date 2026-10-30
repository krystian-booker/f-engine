#pragma once
#include "engine/editor/Selection.h"
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

namespace fe {
class World;

// World-space marks for things the scene does not show: zone rectangles,
// spawn points with their facing, light radii and cones, emitters, collider
// boxes, trigger cylinders, placements that have no entity, and the outline
// of the selection. The viewport projects them and draws them in 2D.
struct HandleBatch {
    struct Line {
        glm::vec3 a{0}, b{0};
        glm::vec4 color{1};
        float thickness = 1;
    };
    struct Icon {
        enum class Shape { Diamond, Circle, Flag, Square, Sun };
        glm::vec3 position{0};
        Shape shape = Shape::Diamond;
        glm::vec4 color{1};
        Selection target; // what clicking it selects
        float radiusPixels = 6;
    };
    struct Label {
        glm::vec3 position{0};
        std::string text;
        glm::vec4 color{1};
    };
    std::vector<Line> lines;
    std::vector<Icon> icons;
    std::vector<Label> labels;
    void clear() {
        lines.clear();
        icons.clear();
        labels.clear();
    }
};

struct HandleOptions {
    bool zones = true, points = true, lights = true, emitters = true, colliders = true, triggers = true,
         placements = true, labels = true;
    bool bakedBoxes = true; // the boxes the walkability bake derived (not selectable)
    // Script badges over placements that have pages: a label per trigger
    // kind ("action", "touch", "auto", "parallel"); empty = no badges.
    std::map<std::string, std::string> triggerBadges;
    int layer = 0; // zones on every layer (-1) are drawn at this layer's height
};

// Colours the handles use, so the Hierarchy can show the same ones.
namespace handleColors {
inline constexpr glm::vec4 ZoneColor{1, 0.85f, 0.2f, 0.9f};
inline constexpr glm::vec4 PointColor{0.3f, 1, 0.5f, 0.95f};
inline constexpr glm::vec4 LightColor{1, 0.95f, 0.5f, 0.8f};
inline constexpr glm::vec4 EmitterColor{1, 0.5f, 0.9f, 0.9f};
inline constexpr glm::vec4 ColliderColor{1, 0.35f, 0.3f, 0.8f};
inline constexpr glm::vec4 BakedBoxColor{1, 0.6f, 0.1f, 0.45f};
inline constexpr glm::vec4 TriggerColor{0.3f, 0.8f, 1, 0.9f};
inline constexpr glm::vec4 PlacementColor{0.75f, 0.75f, 0.75f, 0.9f};
inline constexpr glm::vec4 Selected{1, 0.6f, 0.1f, 1};
inline constexpr glm::vec4 Hovered{1, 1, 1, 0.9f};
} // namespace handleColors

void buildHandles(World& world, const HandleOptions& options, const Selection& selected, const Selection& hovered,
                  double alpha, HandleBatch& out, const std::vector<Selection>& extras = {});
// Twelve edges of a box.
void addBoxLines(HandleBatch& out, glm::vec3 min, glm::vec3 max, glm::vec4 color, float thickness = 1);
} // namespace fe
