#pragma once
#include "engine/editor/AssetCatalog.h"
#include <cstring>
#include <string>

// The ImGui drag-and-drop payload the Assets panel offers and the Scene
// viewport and the Inspector's pickers accept. Plain data: ImGui copies the
// payload bytes.
namespace fe::editorui {
inline constexpr const char* AssetPayloadType = "FE_ASSET";
struct AssetDragPayload {
    AssetKind kind = AssetKind::Other;
    char path[512] = {};
    static AssetDragPayload of(AssetKind kind, const std::string& path) {
        AssetDragPayload p;
        p.kind = kind;
        std::strncpy(p.path, path.c_str(), sizeof(p.path) - 1);
        return p;
    }
};
} // namespace fe::editorui
