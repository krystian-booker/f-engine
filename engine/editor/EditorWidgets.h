#pragma once
#include "engine/nav/Grid.h"
#include "engine/editor/MapEdits.h"
#include "engine/world/MapData.h"
#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

// ImGui form helpers shared by the editor's panels (Editor*.cpp). The typed
// JSON reads (jsonString...) are in MapEdits.h, which needs no UI.
namespace fe::editorui {
bool editVec3(const char* label, glm::vec3& v, float speed = 0.05f);
bool editColor(const char* label, glm::vec3& v);
bool editColor(const char* label, glm::vec4& v);
bool editChoice(const char* label, std::string& value, std::initializer_list<const char*> options);
bool editBlend(const char* label, BlendMode& blend);
// Clamps the layer to the grid's layers (from `lowest`).
bool editLayer(const char* label, int& layer, const Grid& grid, int lowest = 0);
// `prefix` followed by the first number no record (placement, zone) uses as an id.
template<class Record> std::string uniqueId(const std::string& prefix, const std::vector<Record>& records) {
    for (std::size_t n = records.size() + 1;; ++n) {
        const auto id = prefix + std::to_string(n);
        if (std::none_of(records.begin(), records.end(), [&](const auto& r) { return r.id == id; })) {
            return id;
        }
    }
}

// Keeps a box's min at or below its max (maps reject inverted boxes).
template<class Vec> void order(Vec& min, Vec& max) {
    const auto low = glm::min(min, max);
    max = glm::max(min, max);
    min = low;
}
template<class T> bool validIndex(int index, const std::vector<T>& list) {
    return index >= 0 && index < static_cast<int>(list.size());
}
} // namespace fe::editorui
