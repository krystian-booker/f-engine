#include "engine/editor/EditorWidgets.h"
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

namespace fe::editorui {
bool editVec3(const char* label, glm::vec3& v, float speed) {
    return ImGui::DragFloat3(label, glm::value_ptr(v), speed);
}
bool editColor(const char* label, glm::vec3& v) {
    return ImGui::ColorEdit3(label, glm::value_ptr(v), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
}
bool editColor(const char* label, glm::vec4& v) {
    return ImGui::ColorEdit4(label, glm::value_ptr(v), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
}
bool editChoice(const char* label, std::string& value, std::initializer_list<const char*> options) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.c_str())) {
        for (const auto* option : options) {
            if (ImGui::Selectable(option, value == option)) {
                value = option;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}
bool editBlend(const char* label, BlendMode& blend) {
    std::string name = blendModeName(blend);
    if (!editChoice(label, name, {"opaque", "alpha", "additive", "multiply", "premultiplied"})) {
        return false;
    }
    blend = blendModeFromName(name);
    return true;
}
bool editLayer(const char* label, int& layer, const Grid& grid, int lowest) {
    const bool changed = ImGui::InputInt(label, &layer);
    layer = std::clamp(layer, lowest, std::max(lowest, grid.layerCount() - 1));
    return changed;
}
} // namespace fe::editorui
