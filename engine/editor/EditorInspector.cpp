#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include "engine/world/World.h"
#include <IconsFontAwesome6.h>
#include <imgui.h>

namespace fe {
using namespace editorui;

void Editor::drawInspector() {
    const auto w = world();
    if (!w) {
        ImGui::TextDisabled("No world loaded");
        return;
    }
    if (w.get() != historyWorld_) {
        // A reloaded world: its history stays only if the map is the one it left.
        history_.sync(*w);
        historyWorld_ = w.get();
    }
    if (selection_.empty()) {
        ImGui::TextDisabled("Click something in the Scene or the Hierarchy.");
        ImGui::TextDisabled("Drag assets from the Assets panel into the Scene to place them.");
        return;
    }
    if (!selectionValid(*w, selection_)) {
        ImGui::TextDisabled("The selection no longer exists.");
        selection_ = {};
        return;
    }
    const auto& map = w->authored();
    if (!extra_.empty()) {
        ImGui::TextDisabled("%zu objects selected: they move, duplicate and delete together. Showing the first.",
                            extra_.size() + 1);
    }
    ImGui::Text("%s", selectionLabel(*w, selection_).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_CROSSHAIRS)) {
        frameSelection(*w, alpha_);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Frame in the Scene (F)");
    }
    if (selection_.kind != Selection::Kind::Entity) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_CLONE)) {
            duplicateSelection(*w);
            return;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Duplicate (Ctrl+D)");
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_TRASH)) {
        deleteSelection(*w);
        return;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Delete (Del)");
    }
    ImGui::Separator();
    ImGui::PushID(selectionKindName(selection_.kind));
    switch (selection_.kind) {
    case Selection::Kind::Placement:
        drawPlacementForm(*w, static_cast<std::size_t>(placementIndex(map, selection_.id)));
        break;
    case Selection::Kind::Zone: drawZoneForm(*w, static_cast<std::size_t>(zoneIndex(map, selection_.id))); break;
    case Selection::Kind::Point: drawPointForm(*w, selection_.id); break;
    case Selection::Kind::Light: drawLightForm(*w, static_cast<std::size_t>(selection_.index)); break;
    case Selection::Kind::Emitter: drawEmitterForm(*w, static_cast<std::size_t>(selection_.index)); break;
    case Selection::Kind::Collider: drawColliderForm(*w, static_cast<std::size_t>(selection_.index)); break;
    case Selection::Kind::Entity: drawEntity(*w, selection_.entity); break;
    case Selection::Kind::None: break;
    }
    ImGui::PopID();
}
} // namespace fe
