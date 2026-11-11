#include "engine/core/Paths.h"
#include "engine/editor/Editor.h"
#include "engine/editor/EditorDragDrop.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/editor/MapFiles.h"
#include "engine/render/FrameRenderer.h"
#include "engine/render/GpuAssets.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include <IconsFontAwesome6.h>
#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

namespace {
bool containsNoCase(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    const auto lower = [](std::string s) {
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    return lower(text).find(lower(needle)) != std::string::npos;
}
} // namespace

bool Editor::drawThumbnail(const std::string& path, AssetKind kind, float size) {
    if (!gpu_ || path.empty()) {
        return false;
    }
    try {
        if (kind == AssetKind::Sprite) {
            const auto* sheet = gpu_->spriteSheet(path);
            if (!sheet || !sheet->animation || !bgfx::isValid(sheet->color) || sheet->size.x <= 0) {
                return false;
            }
            const auto& clip = sheet->animation->clip("idle");
            if (clip.directions.empty() || clip.directions.front().empty()) {
                return false;
            }
            const auto& rect = clip.directions.front().front().rect;
            const glm::vec2 atlas(sheet->size);
            const ImVec2 uv0(static_cast<float>(rect.x) / atlas.x, static_cast<float>(rect.y) / atlas.y);
            const ImVec2 uv1(static_cast<float>(rect.x + rect.z) / atlas.x, static_cast<float>(rect.y + rect.w) / atlas.y);
            const float aspect = rect.w > 0 ? static_cast<float>(rect.z) / static_cast<float>(rect.w) : 1;
            ImGui::Image(static_cast<ImTextureID>(sheet->color.idx + 1), ImVec2(size * aspect, size), uv0, uv1);
            return true;
        }
        if (kind == AssetKind::Model) {
            const auto handle = frame_ ? frame_->keptTarget(path) : bgfx::TextureHandle BGFX_INVALID_HANDLE;
            if (!bgfx::isValid(handle)) {
                requestModelThumbnail(path);
                return false;
            }
            const bool flip = bgfx::getCaps()->originBottomLeft;
            ImGui::Image(static_cast<ImTextureID>(handle.idx + 1), ImVec2(size, size), ImVec2(0, flip ? 1 : 0),
                         ImVec2(1, flip ? 0 : 1));
            return true;
        }
        if (kind == AssetKind::Image) {
            const auto handle = gpu_->image(path, TextureUse::Ui);
            const auto dimensions = gpu_->imageSize(path);
            if (!bgfx::isValid(handle) || dimensions.x <= 0 || dimensions.y <= 0) {
                return false;
            }
            const float aspect = static_cast<float>(dimensions.x) / static_cast<float>(dimensions.y);
            const ImVec2 shown = aspect >= 1 ? ImVec2(size, size / aspect) : ImVec2(size * aspect, size);
            ImGui::Image(static_cast<ImTextureID>(handle.idx + 1), shown);
            return true;
        }
    } catch (const std::exception&) {
        return false;
    }
    return false;
}

bool Editor::choicePicker(const char* label, std::string& value, const std::vector<std::string>& candidates,
                          const std::vector<std::string>& labels, const char* hint, bool optional, AssetKind thumbnail) {
    bool changed = false;
    ImGui::PushID(label);
    const bool known = value.empty() || std::find(candidates.begin(), candidates.end(), value) != candidates.end();
    if (!known) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.6f, 0.4f, 1));
    }
    const float thumb = ImGui::GetFrameHeight();
    if (thumbnail != AssetKind::Other && !value.empty() && drawThumbnail(value, thumbnail, thumb)) {
        ImGui::SameLine();
    }
    const auto shown = value.empty() ? std::string(hint) : value;
    if (ImGui::BeginCombo(label, shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        auto* storage = ImGui::GetStateStorage();
        const auto filterId = ImGui::GetID("filter");
        static std::map<ImGuiID, std::string> filters; // per combo, kept while it is open
        auto& filter = filters[filterId];
        if (ImGui::IsWindowAppearing()) {
            filter.clear();
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS " Type to filter", &filter);
        storage->SetInt(filterId, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup(); // one Escape closes the picker, filter focused or not
        }
        if (optional && ImGui::Selectable("(none)", value.empty())) {
            value.clear();
            changed = true;
        }
        int shownCount = 0;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            const auto& text = i < labels.size() ? labels[i] : candidate;
            if (!containsNoCase(text, filter) && !containsNoCase(candidate, filter)) {
                continue;
            }
            if (++shownCount > 400) {
                ImGui::TextDisabled("... type more to narrow the list");
                break;
            }
            ImGui::PushID(static_cast<int>(i));
            if (thumbnail != AssetKind::Other && drawThumbnail(candidate, thumbnail, thumb)) {
                ImGui::SameLine();
            }
            if (ImGui::Selectable(text.c_str(), candidate == value)) {
                value = candidate;
                changed = true;
            }
            ImGui::PopID();
        }
        if (shownCount == 0) {
            ImGui::TextDisabled("Nothing matches");
        }
        ImGui::EndCombo();
    }
    if (!known) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("'%s' is not in the list: the file or id may be missing", value.c_str());
        }
    }
    // Drops from the Assets panel.
    if (thumbnail != AssetKind::Other && ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload(AssetPayloadType)) {
            const auto& asset = *static_cast<const AssetDragPayload*>(payload->Data);
            if (asset.kind == thumbnail || thumbnail == AssetKind::Audio) {
                value = asset.path;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##pickeredit")) {
        ImGui::TextDisabled("Exact value");
        ImGui::SetNextItemWidth(300);
        if (ImGui::InputText("##exact", &value, ImGuiInputTextFlags_EnterReturnsTrue)) {
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered() && known && !value.empty()) {
        ImGui::SetTooltip("%s\nRight-click to type a value", value.c_str());
    }
    ImGui::PopID();
    return changed;
}

bool Editor::assetPicker(const char* label, std::string& value, AssetKind kind, bool optional) {
    std::vector<std::string> candidates, labels;
    if (kind == AssetKind::Audio) {
        for (const auto& id : catalog_.audioIds()) {
            candidates.push_back(id.id);
            labels.push_back(id.group + ": " + id.id + "  (" + id.file + ")");
        }
    }
    for (const auto* entry : catalog_.ofKind(kind)) {
        if (!entry->sprite.empty() && kind == AssetKind::Image) {
            continue; // atlases are part of their sprite
        }
        candidates.push_back(entry->path);
        labels.push_back(entry->path);
    }
    const char* hints[] = {"(pick a sprite)", "(pick an image)", "(pick a model)", "(pick a sound)", "(pick a LUT)",
                           "(pick a script)", "(pick a map)", "(pick a table)", "(pick a shader)", "(pick a font)",
                           "(pick a file)", "(pick a file)"};
    return choicePicker(label, value, candidates, labels, hints[static_cast<int>(kind)], optional,
                        kind == AssetKind::Sprite || kind == AssetKind::Image ? kind : kind == AssetKind::Audio ? kind : kind);
}

bool Editor::modulePicker(const char* label, std::string& value, const std::string& folder, bool optional) {
    const auto candidates = catalog_.modules(folder);
    return choicePicker(label, value, candidates, candidates, "(pick a module)", optional, AssetKind::Other);
}

bool Editor::mapPicker(const char* label, std::string& value, bool optional) {
    std::vector<std::string> ids, labels;
    for (const auto& map : catalog_.maps()) {
        ids.push_back(map.id);
        labels.push_back(map.id + "  (" + map.name + ", " + std::to_string(map.width) + "x" + std::to_string(map.depth) + ")");
    }
    return choicePicker(label, value, ids, labels, "(pick a map)", optional, AssetKind::Other);
}

std::vector<std::string> Editor::mapPoints(const std::string& mapId) {
    auto& cached = mapPoints_[mapId];
    if (Clock::now() - cached.first > std::chrono::seconds(2)) {
        cached.first = Clock::now();
        cached.second.clear();
        try {
            const auto json = parseJson(readFile(resolveInside(engine_.lua().gameRoot(), "maps/" + mapId + "/map.json")));
            if (const auto points = json.find("points"); points != json.end() && points->is_object()) {
                for (const auto& [name, point] : (*points).items()) {
                    cached.second.push_back(name);
                }
            }
        } catch (const std::exception&) {
        }
    }
    return cached.second;
}

bool Editor::pointPicker(const char* label, std::string& value, const std::string& mapId, bool optional) {
    const auto candidates = mapId.empty() ? std::vector<std::string>{} : mapPoints(mapId);
    return choicePicker(label, value, candidates, candidates,
                        mapId.empty() ? "(pick a map first)" : "(pick a point)", optional, AssetKind::Other);
}

std::vector<std::string> Editor::recordIds(const std::string& table) {
    auto& cached = recordIds_[table];
    if (Clock::now() - cached.first > std::chrono::seconds(2)) {
        cached.first = Clock::now();
        cached.second.clear();
        // The ids of the records in the table's file, as the game's
        // editor.describe hook names it ({name, file} under "tables").
        try {
            for (const auto& described : jsonArray(schemas_, "tables")) {
                if (jsonString(described, "name", {}) != table) {
                    continue;
                }
                const auto records = readJson(resolveInside(engine_.lua().gameRoot(), jsonString(described, "file", {})));
                if (records.is_array()) {
                    for (const auto& record : records) {
                        if (const auto id = jsonString(record, "id", {}); !id.empty()) {
                            cached.second.push_back(id);
                        }
                    }
                } else if (records.is_object()) {
                    for (const auto& [key, value] : records.items()) {
                        cached.second.push_back(key);
                    }
                }
            }
        } catch (const std::exception&) {
        }
        std::sort(cached.second.begin(), cached.second.end());
    }
    return cached.second;
}

bool Editor::refPicker(const char* label, std::string& value, const std::string& table, bool optional) {
    const auto candidates = recordIds(table);
    const auto hint = "(pick a " + table + " id)";
    return choicePicker(label, value, candidates, candidates, hint.c_str(), optional, AssetKind::Other);
}

bool Editor::clipPicker(const char* label, std::string& value, const std::string& animation) {
    std::vector<std::string> clips;
    if (const auto sheet = engine_.assets().trySprite(animation)) {
        for (const auto& [name, clip] : sheet->clips) {
            clips.push_back(name);
        }
    }
    if (clips.empty()) {
        return ImGui::InputText(label, &value);
    }
    return choicePicker(label, value, clips, clips, "(pick a clip)", false, AssetKind::Other);
}

void Editor::requestModelThumbnail(const std::string& path) {
    if (!frame_ || thumbnailsDrawn_.contains(path) ||
        std::find(thumbnailQueue_.begin(), thumbnailQueue_.end(), path) != thumbnailQueue_.end()) {
        return;
    }
    thumbnailQueue_.push_back(path);
}

// One model per frame is posed in a hidden world and drawn into a small
// render target the frame renderer keeps under the model's path.
void Editor::queueThumbnailDraws(std::vector<WorldDraw>& draws) {
    if (!frame_ || !gpu_) {
        return;
    }
    if (gpu_->revision() != thumbnailRevision_) {
        // Assets were reloaded: draw them again when asked.
        thumbnailRevision_ = gpu_->revision();
        frame_->forgetKeptTargets();
        thumbnailsDrawn_.clear();
    }
    if (thumbnailQueue_.empty()) {
        return;
    }
    const auto path = thumbnailQueue_.front();
    thumbnailQueue_.erase(thumbnailQueue_.begin());
    thumbnailsDrawn_.insert(path); // one attempt; a model that fails to load keeps its icon
    const auto model = engine_.assets().tryModel(path);
    if (!model) {
        return;
    }
    if (!thumbnailWorld_) {
        auto map = makeBlankMap("editor_thumbnail", "thumbnail", 2, 2);
        map.environment.sky.mode = "color";
        map.environment.sky.top = map.environment.sky.horizon = map.environment.sky.bottom = {0.16f, 0.17f, 0.2f};
        map.environment.shadows = false;
        map.environment.fogDensity = 0;
        map.environment.post.bloom = false;
        map.environment.post.depthOfField = false;
        map.environment.post.ssao = false;
        map.environment.post.vignette = false;
        map.environment.post.colorGrade = false;
        thumbnailWorld_ = std::make_shared<World>("editor:thumbnail", std::move(map), engine_.assets(), 1);
        thumbnailEntity_ = thumbnailWorld_->spawn("thumbnail", {0, 0, 0});
        thumbnailWorld_->registry().emplace<MeshInstance>(thumbnailEntity_);
    }
    auto& mesh = thumbnailWorld_->registry().get<MeshInstance>(thumbnailEntity_);
    mesh.model = path;
    // A three-quarter view that frames the model's bounds.
    EditorCamera camera;
    camera.camera.yaw = 35;
    camera.camera.pitch = 28;
    camera.camera.fov = 30;
    camera.frame(model->min, model->max);
    WorldDraw draw;
    draw.world = thumbnailWorld_;
    draw.rect = Rect{0, 0, 96, 96};
    draw.target = "editor:thumbnail";
    draw.camera = camera.camera;
    draw.snapCamera = true;
    draws.push_back(std::move(draw));
    frame_->keepTarget("editor:thumbnail", path);
}
} // namespace fe
