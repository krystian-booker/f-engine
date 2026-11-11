#include "engine/assets/Image.h"
#include "engine/assets/Model.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/editor/Editor.h"
#include "engine/editor/EditorDragDrop.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/render/GpuAssets.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <IconsFontAwesome6.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <functional>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

namespace {
const char* iconFor(AssetKind kind) {
    switch (kind) {
    case AssetKind::Sprite: return ICON_FA_PERSON_WALKING;
    case AssetKind::Image: return ICON_FA_IMAGE;
    case AssetKind::Model: return ICON_FA_CUBE;
    case AssetKind::Audio: return ICON_FA_MUSIC;
    case AssetKind::Lut: return ICON_FA_PALETTE;
    case AssetKind::Script: return ICON_FA_SCROLL;
    case AssetKind::Map: return ICON_FA_MAP;
    case AssetKind::Data: return ICON_FA_TABLE;
    case AssetKind::Shader: return ICON_FA_WAND_MAGIC_SPARKLES;
    case AssetKind::Font: return ICON_FA_FONT;
    case AssetKind::Text: return ICON_FA_FILE_LINES;
    case AssetKind::Other: break;
    }
    return ICON_FA_FILE;
}
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
// Where an imported file of a kind goes when no folder is chosen.
std::string defaultFolderFor(AssetKind kind) {
    switch (kind) {
    case AssetKind::Sprite:
    case AssetKind::Image: return "sprites";
    case AssetKind::Model: return "fx";
    case AssetKind::Audio: return "audio";
    case AssetKind::Lut: return "ui";
    case AssetKind::Script: return "events";
    case AssetKind::Shader: return "shaders";
    default: return "";
    }
}
std::string kilobytes(std::uintmax_t bytes) {
    return std::to_string((bytes + 1023) / 1024) + " KB";
}

// The SDL file dialog reports on its own thread; the result is queued for
// the editor's next frame.
void SDLCALL dialogDone(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* weak = static_cast<std::weak_ptr<ImportQueue>*>(userdata);
    if (const auto queue = weak->lock()) {
        std::lock_guard lock(queue->mutex);
        ++queue->returned;
        if (!filelist) {
            ++queue->failed; // SDL could not show the dialog (SDL_GetError says why)
            log(LogLevel::Warning, std::string("File dialog failed: ") + SDL_GetError());
        } else if (!*filelist) {
            ++queue->cancelled;
        } else {
            for (const char* const* file = filelist; *file; ++file) {
                queue->files.emplace_back(*file);
            }
        }
    }
    delete weak;
}
} // namespace

void Editor::openImportDialog(ImportQueue::Purpose purpose) {
    static const SDL_DialogFileFilter assets[] = {
        {"Game assets", "png;jpg;jpeg;tga;glb;gltf;wav;ogg;mp3;flac;cube;json;lua"},
        {"Images and sprites", "png;jpg;jpeg;tga;json"},
        {"Models", "glb;gltf"},
        {"Audio", "wav;ogg;mp3;flac"},
        {"All files", "*"}};
    static const SDL_DialogFileFilter models[] = {{"glTF models", "glb;gltf"}};
    {
        std::lock_guard lock(importQueue_->mutex);
        importQueue_->folder = assetFolder_;
        importQueue_->purpose = purpose;
    }
    const bool geometry = purpose != ImportQueue::Purpose::Assets;
    // With the window the dialog is a sheet and the frame loop keeps running;
    // without one SDL runs it modally and the whole app waits.
    SDL_ShowOpenFileDialog(dialogDone, new std::weak_ptr<ImportQueue>(importQueue_), window_,
                           geometry ? models : assets, geometry ? 1 : 5, nullptr, !geometry);
}

std::vector<std::filesystem::path> Editor::takeChosenFiles(ImportQueue::Purpose& purpose, std::string& folder) {
    std::vector<std::filesystem::path> chosen;
    std::lock_guard lock(importQueue_->mutex);
    chosen.swap(importQueue_->files);
    purpose = importQueue_->purpose;
    folder = importQueue_->folder;
    return chosen;
}

void Editor::importGeometry(World& w, const std::filesystem::path& file) {
    const auto extension = file.extension().string();
    if (extension != ".glb" && extension != ".gltf") {
        status_ = "Geometry must be a .glb or .gltf file";
        return;
    }
    std::error_code error;
    const auto name = "geometry" + extension;
    const auto folder = engine_.lua().gameRoot() / "maps" / w.authored().id;
    const auto target = folder / name;
    if (!std::filesystem::equivalent(file, target, error)) {
        std::filesystem::create_directories(folder, error);
        std::filesystem::copy_file(file, target, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            status_ = "Could not copy the geometry: " + error.message();
            return;
        }
    }
    if (w.authored().geometry != name) {
        // The map names its geometry file; a placeholder of the other kind goes.
        const auto previous = folder / w.authored().geometry;
        gesture_ = false;
        edit(w, [&](MapData& m) { m.geometry = name; });
        gesture_ = false;
        if (previous != target && previous.filename().string().starts_with("geometry.")) {
            std::filesystem::remove(previous, error);
        }
    }
    engine_.assets().release(w.geometryPath());
    importMarkers(w);
    if (w.authored().bake.autoBake && !w.authored().bake.empty()) {
        rebake(w, std::nullopt, false);
    }
    status_ = "Imported " + file.filename().string() + " as the map's geometry; " + status_ +
              ". Save the map to keep the markers.";
}

void Editor::importFiles(const std::vector<std::filesystem::path>& files, const std::string& folder) {
    const auto& root = engine_.lua().gameRoot();
    int imported = 0;
    std::string firstImage;
    std::vector<std::filesystem::path> queue = files;
    std::error_code error;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const auto source = queue[i];
        if (!std::filesystem::is_regular_file(source, error)) {
            status_ = "Not a file: " + source.string();
            continue;
        }
        const auto name = source.filename().string();
        const auto kind = assetKindOf(name);
        const auto destinationFolder = folder.empty() || folder.starts_with("@fe/") ? defaultFolderFor(kind) : folder;
        std::filesystem::path target;
        try {
            target = destinationFolder.empty() ? root / name : resolveInside(root, destinationFolder + "/" + name);
        } catch (const std::exception& problem) {
            status_ = problem.what();
            continue;
        }
        if (std::filesystem::exists(target, error)) {
            if (std::filesystem::equivalent(source, target, error)) {
                continue; // already where it belongs
            }
            // Keep the existing file: number the newcomer.
            const auto stem = target.stem().string(), extension = target.extension().string();
            for (int n = 2;; ++n) {
                auto numbered = target.parent_path() / (stem + "_" + std::to_string(n) + extension);
                if (!std::filesystem::exists(numbered, error)) {
                    target = numbered;
                    break;
                }
            }
        }
        std::filesystem::create_directories(target.parent_path(), error);
        const auto temporary = target.parent_path() / (target.filename().string() + ".importing");
        if (!std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing, error)) {
            status_ = "Could not copy " + name + ": " + error.message();
            continue;
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            status_ = "Could not place " + name + ": " + error.message();
            std::filesystem::remove(temporary, error);
            continue;
        }
        ++imported;
        const auto relative = relativeTo(root, target);
        // A sprite brings its images; an image brings its sprite description when one sits beside it.
        if (kind == AssetKind::Sprite) {
            try {
                const auto json = parseJson(readFile(source));
                for (const char* key : {"atlas", "normal", "emissive"}) {
                    const auto image = json.value(key, std::string());
                    if (!image.empty()) {
                        const auto sibling = source.parent_path() / image;
                        if (std::filesystem::is_regular_file(sibling, error) &&
                            std::find(queue.begin(), queue.end(), sibling) == queue.end()) {
                            queue.push_back(sibling);
                        }
                    }
                }
            } catch (const std::exception&) {
            }
        } else if (kind == AssetKind::Image) {
            const auto stem = source.stem().string();
            const auto description = source.parent_path() / (stem + ".anim.json");
            if (std::filesystem::is_regular_file(description, error)) {
                if (std::find(queue.begin(), queue.end(), description) == queue.end()) {
                    queue.push_back(description);
                }
            } else if (firstImage.empty() && destinationFolder == "sprites") {
                firstImage = relative;
            }
        }
    }
    catalog_.scan();
    status_ = "Imported " + std::to_string(imported) + " file(s)";
    if (!firstImage.empty() && !catalog_.find(firstImage.substr(0, firstImage.size() - 4) + ".anim.json")) {
        // A loose image in sprites/: offer to describe it as a sprite sheet.
        wizard_ = {};
        wizard_.open = true;
        wizard_.image = firstImage;
        wizard_.spec.atlas = std::filesystem::path(firstImage).filename().string();
        wizard_.spec.clips = {{"idle", {0}, true, 0.3, 0, 0}};
        if (const auto image = engine_.assets().tryImage(firstImage)) {
            wizard_.imageWidth = image->width;
            wizard_.imageHeight = image->height;
            wizard_.spec.directions = sheetRows(wizard_.spec, image->height) >= 4 ? 4 : 1;
        }
    }
}

void Editor::drawSpriteWizard() {
    if (!wizard_.open) {
        return;
    }
    ImGui::OpenPopup("Sprite sheet");
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Sprite sheet", &wizard_.open, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    auto& spec = wizard_.spec;
    ImGui::Text("%s  (%d x %d pixels)", wizard_.image.c_str(), wizard_.imageWidth, wizard_.imageHeight);
    ImGui::TextDisabled("Describe the frames on this image so it can be used as a sprite (.anim.json).");
    ImGui::BeginChild("form", ImVec2(300, 300));
    ImGui::DragInt("Frame width", &spec.frameWidth, 1, 1, 1024);
    ImGui::DragInt("Frame height", &spec.frameHeight, 1, 1, 1024);
    const char* directions[] = {"1 (one row)", "4 (S, W, N, E)", "8"};
    int directionIndex = spec.directions == 1 ? 0 : spec.directions == 8 ? 2 : 1;
    if (ImGui::Combo("Directions", &directionIndex, directions, 3)) {
        spec.directions = directionIndex == 0 ? 1 : directionIndex == 2 ? 8 : 4;
    }
    ImGui::DragInt("Pixels per unit", &spec.pixelsPerUnit, 1, 1, 512);
    const int columns = sheetColumns(spec, wizard_.imageWidth), rows = sheetRows(spec, wizard_.imageHeight);
    ImGui::TextDisabled("%d columns x %d rows of frames", columns, rows);
    ImGui::SeparatorText("Clips");
    for (std::size_t i = 0; i < spec.clips.size(); ++i) {
        auto& clip = spec.clips[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(90);
        ImGui::InputText("##name", &clip.name);
        ImGui::SameLine();
        std::string frames;
        for (const int f : clip.frames) {
            frames += (frames.empty() ? "" : ",") + std::to_string(f);
        }
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputText("##frames", &frames)) {
            clip.frames.clear();
            std::string number;
            for (const char c : frames + ",") {
                if (std::isdigit(static_cast<unsigned char>(c))) {
                    number += c;
                } else if (!number.empty()) {
                    clip.frames.push_back(std::stoi(number));
                    number.clear();
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Column numbers, in play order (0,1,2,1)");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40);
        ImGui::DragInt("##row", &clip.rowOffset, 0.1f, 0, 64);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("First row of this clip");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        float duration = static_cast<float>(clip.duration);
        if (ImGui::DragFloat("##dur", &duration, 0.01f, 0.01f, 5, "%.2fs")) {
            clip.duration = duration;
        }
        ImGui::SameLine();
        ImGui::Checkbox("##loop", &clip.loop);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Loop");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            spec.clips.erase(spec.clips.begin() + static_cast<std::ptrdiff_t>(i));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton(ICON_FA_PLUS " Clip")) {
        const int nextRow = spec.clips.empty() ? 0 : spec.clips.back().rowOffset + spec.directions;
        spec.clips.push_back({"walk", {0, 1, 2, 1}, true, 0.15, nextRow, 0});
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Standard set")) {
        spec.clips = {{"idle", {0}, true, 0.3, 0, 0},
                      {"walk", {0, 1, 2, 1}, true, 0.15, 0, 0},
                      {"attack", {0, 1, 2}, false, 0.1, spec.directions, 0},
                      {"hurt", {0}, false, 0.3, spec.directions * 2, 0},
                      {"ko", {0}, false, 0.3, spec.directions * 2, 1}};
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // The image with the frame grid over it.
    ImGui::BeginChild("preview", ImVec2(0, 300), ImGuiChildFlags_Borders);
    if (wizard_.imageWidth > 0 && wizard_.imageHeight > 0) {
        const float scale = std::min(280.0f / static_cast<float>(wizard_.imageWidth),
                                     280.0f / static_cast<float>(wizard_.imageHeight));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size(static_cast<float>(wizard_.imageWidth) * scale, static_cast<float>(wizard_.imageHeight) * scale);
        if (!drawThumbnail(wizard_.image, AssetKind::Image, size.y)) {
            ImGui::Dummy(size);
        }
        auto* draw = ImGui::GetWindowDrawList();
        const auto line = ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 0, 0.6f));
        for (int c = 0; c <= columns; ++c) {
            const float x = origin.x + static_cast<float>(c * spec.frameWidth) * scale;
            draw->AddLine({x, origin.y}, {x, origin.y + static_cast<float>(rows * spec.frameHeight) * scale}, line);
        }
        for (int r = 0; r <= rows; ++r) {
            const float y = origin.y + static_cast<float>(r * spec.frameHeight) * scale;
            draw->AddLine({origin.x, y}, {origin.x + static_cast<float>(columns * spec.frameWidth) * scale, y}, line);
        }
    }
    ImGui::EndChild();
    // Checked every frame, so the form says what is wrong before Create.
    std::string problem = wizard_.problem;
    try {
        static_cast<void>(buildSheetAnimation(spec, wizard_.imageWidth, wizard_.imageHeight));
    } catch (const std::exception& error) {
        problem = error.what();
        if (spec.directions > sheetRows(spec, wizard_.imageHeight)) {
            problem += " (each clip needs one row per direction: pick fewer directions or smaller frames)";
        }
    }
    if (!problem.empty()) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", problem.c_str());
    }
    const auto output = wizard_.image.substr(0, wizard_.image.size() - std::filesystem::path(wizard_.image).extension().string().size()) + ".anim.json";
    ImGui::BeginDisabled(!problem.empty());
    const bool create = ImGui::Button((ICON_FA_CHECK " Create " + output).c_str());
    ImGui::EndDisabled();
    if (create) {
        try {
            const auto json = buildSheetAnimation(spec, wizard_.imageWidth, wizard_.imageHeight);
            writeJson(resolveInside(engine_.lua().gameRoot(), output), json);
            catalog_.scan();
            assetSelected_ = output;
            status_ = "Wrote " + output;
            wizard_.open = false;
            ImGui::CloseCurrentPopup();
        } catch (const std::exception& error) {
            wizard_.problem = error.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        wizard_.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Editor::drawAssetDetails(const AssetEntry& entry) {
    ImGui::Text("%s %s", iconFor(entry.kind), entry.path.c_str());
    ImGui::TextDisabled("%s, %s", assetKindName(entry.kind), kilobytes(entry.size).c_str());
    if (ImGui::SmallButton(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE " Open")) {
        const auto file = std::filesystem::absolute(catalog_.fileOf(entry.path)).string();
        SDL_OpenURL(("file://" + file).c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open with the system's application for this file type");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_COPY " Copy path")) {
        ImGui::SetClipboardText(entry.path.c_str());
    }
    if (entry.kind == AssetKind::Audio) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_PLAY " Play")) {
            engine_.lua().evaluate("fe.audio.sound(\"" + entry.path + "\")");
        }
    }
    if (entry.kind == AssetKind::Image && entry.sprite.empty() && !entry.path.starts_with("@fe/")) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_PERSON_WALKING " Make a sprite...")) {
            wizard_ = {};
            wizard_.open = true;
            wizard_.image = entry.path;
            wizard_.spec.atlas = std::filesystem::path(entry.path).filename().string();
            wizard_.spec.clips = {{"idle", {0}, true, 0.3, 0, 0}};
            if (const auto image = engine_.assets().tryImage(entry.path)) {
                wizard_.imageWidth = image->width;
                wizard_.imageHeight = image->height;
                wizard_.spec.directions = sheetRows(wizard_.spec, image->height) >= 4 ? 4 : 1;
            }
        }
    }
    ImGui::Separator();
    switch (entry.kind) {
    case AssetKind::Sprite:
        drawThumbnail(entry.path, AssetKind::Sprite, 96);
        if (const auto sheet = engine_.assets().trySprite(entry.path)) {
            ImGui::Text("Atlas %s, %d pixels per unit", sheet->atlas.c_str(), sheet->pixelsPerUnit);
            if (!sheet->normal.empty()) {
                ImGui::Text("Normal map %s", sheet->normal.c_str());
            }
            if (!sheet->emissive.empty()) {
                ImGui::Text("Emissive %s", sheet->emissive.c_str());
            }
            if (ImGui::BeginTable("clips", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Clip");
                ImGui::TableSetupColumn("Rows");
                ImGui::TableSetupColumn("Frames");
                ImGui::TableSetupColumn("Loop");
                ImGui::TableHeadersRow();
                for (const auto& [name, clip] : sheet->clips) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", clip.directions.size());
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu (%.2fs)", clip.directions.empty() ? 0 : clip.directions.front().size(), clip.length(0));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(clip.loop ? "yes" : "no");
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "The sprite does not load (see the log)");
        }
        break;
    case AssetKind::Image:
        drawThumbnail(entry.path, AssetKind::Image, 160);
        if (const auto image = engine_.assets().tryImage(entry.path)) {
            ImGui::Text("%d x %d pixels", image->width, image->height);
        }
        if (!entry.sprite.empty()) {
            ImGui::TextDisabled("Part of the sprite %s", entry.sprite.c_str());
        }
        break;
    case AssetKind::Model:
        drawThumbnail(entry.path, AssetKind::Model, 160);
        if (const auto model = engine_.assets().tryModel(entry.path)) {
            ImGui::Text("%zu nodes, %zu parts, %zu animations, %zu markers", model->nodes.size(), model->parts.size(),
                        model->animations.size(), model->markers.size());
            ImGui::Text("Bounds %.2f x %.2f x %.2f units", model->max.x - model->min.x, model->max.y - model->min.y,
                        model->max.z - model->min.z);
            for (const auto& animation : model->animations) {
                ImGui::BulletText("animation %s (%.2fs)", animation.name.c_str(), animation.duration);
            }
            for (const auto& marker : model->markers) {
                ImGui::BulletText("marker %s %s", marker.name.c_str(), marker.properties.dump().c_str());
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "The model does not load (see the log)");
        }
        break;
    case AssetKind::Map:
        for (const auto& map : catalog_.maps()) {
            if (map.path == entry.path) {
                ImGui::Text("%s: %d x %d cells", map.name.c_str(), map.width, map.depth);
                if (ImGui::Button(ICON_FA_PLAY " Open this map")) {
                    const auto id = map.id;
                    confirmDiscard(world().get(), "open another map", [this, id] { openMap(id); });
                }
            }
        }
        break;
    default: break;
    }
    const auto& users = catalog_.references(entry.path);
    ImGui::SeparatorText(users.empty() ? "Not used by any map or table" : "Used by");
    for (const auto& user : users) {
        ImGui::BulletText("%s  %s", user.file.c_str(), user.where.c_str());
    }
}

void Editor::drawAssetsPanel() {
    // Files dropped from the OS onto this panel (the Scene handles its own).
    if (!droppedFiles_.empty() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        std::vector<std::filesystem::path> files;
        for (const auto& [file, point] : droppedFiles_) {
            files.push_back(file);
        }
        importFiles(files, assetFolder_);
        droppedFiles_.clear();
    }
    if (ImGui::Button(ICON_FA_FILE_IMPORT " Import...")) {
        openImportDialog();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copy files into the game folder (%s). You can also drop files onto this panel.",
                          assetFolder_.empty() ? "a folder chosen by type" : assetFolder_.c_str());
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##assetfilter", ICON_FA_MAGNIFYING_GLASS " Search", &assetFilter_);
    ImGui::SameLine();
    const char* kinds[] = {"All", "Sprites", "Images", "Models", "Audio", "LUTs", "Scripts", "Maps", "Data"};
    const AssetKind kindOf[] = {AssetKind::Other, AssetKind::Sprite, AssetKind::Image, AssetKind::Model, AssetKind::Audio,
                                AssetKind::Lut,   AssetKind::Script, AssetKind::Map,   AssetKind::Data};
    int kindIndex = assetKindFilter_ + 1;
    ImGui::SetNextItemWidth(90);
    if (ImGui::Combo("##kind", &kindIndex, kinds, 9)) {
        assetKindFilter_ = kindIndex - 1;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(assetGrid_ ? ICON_FA_LIST : ICON_FA_TABLE_CELLS)) {
        assetGrid_ = !assetGrid_;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ARROWS_ROTATE)) {
        catalog_.scan();
    }
    ImGui::SameLine();
    const auto missing = catalog_.missing();
    if (!missing.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.6f, 0.3f, 1));
        const auto label = std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + std::to_string(missing.size()) + " missing";
        if (ImGui::SmallButton(label.c_str())) {
            ImGui::OpenPopup("Missing references");
        }
        ImGui::PopStyleColor();
        if (ImGui::BeginPopup("Missing references")) {
            ImGui::TextDisabled("Files, modules, maps and audio ids that maps and tables name but no file supplies");
            for (const auto& [target, user] : missing) {
                ImGui::BulletText("%s  <- %s (%s)", target.c_str(), user.file.c_str(), user.where.c_str());
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    }
    ImGui::TextDisabled("%zu files", catalog_.entries().size());
    // Folder tree, files, details.
    ImGui::BeginChild("folders", ImVec2(150, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    if (ImGui::Selectable(ICON_FA_FOLDER " (game)", assetFolder_.empty())) {
        assetFolder_.clear();
    }
    std::function<void(const std::string&)> tree = [&](const std::string& parent) {
        for (const auto& name : catalog_.folders(parent)) {
            const auto path = parent.empty() ? name : parent + "/" + name;
            if (path == "tests" || path == "replays") {
                continue;
            }
            const bool hasChildren = !catalog_.folders(path).empty();
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (!hasChildren) {
                flags |= ImGuiTreeNodeFlags_Leaf;
            }
            if (path == assetFolder_) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            const bool openNode = ImGui::TreeNodeEx((std::string(ICON_FA_FOLDER " ") + name).c_str(), flags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                assetFolder_ = path;
            }
            if (openNode) {
                tree(path);
                ImGui::TreePop();
            }
        }
    };
    tree("");
    tree("@fe"); // the engine's own assets
    ImGui::EndChild();
    ImGui::SameLine();
    const AssetEntry* selected = catalog_.find(assetSelected_);
    const float detailsWidth = selected ? 260.0f : 0.0f;
    ImGui::BeginChild("files", ImVec2(-detailsWidth, 0), ImGuiChildFlags_Borders);
    const std::string prefix = assetFolder_.empty() ? std::string() : assetFolder_ + "/";
    const float cell = 84;
    const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
    int shown = 0;
    for (const auto& entry : catalog_.entries()) {
        if (!entry.path.starts_with(prefix) || entry.path.starts_with("tests/") || entry.path.starts_with("replays/")) {
            continue;
        }
        if (assetFolder_.empty() && entry.path.starts_with("@fe/")) {
            continue; // library assets show under their own folder
        }
        if (!entry.sprite.empty() && assetKindFilter_ < 0) {
            continue; // an atlas belongs to its sprite
        }
        if (assetKindFilter_ >= 0 && entry.kind != kindOf[assetKindFilter_ + 1]) {
            continue;
        }
        if (!containsNoCase(entry.path, assetFilter_)) {
            continue;
        }
        const auto rest = entry.path.substr(prefix.size());
        const bool isSelected = assetSelected_ == entry.path;
        ImGui::PushID(entry.path.c_str());
        // The drag source must follow the selectable item itself (not a group).
        const auto dragSource = [&] {
            if ((entry.kind == AssetKind::Sprite || entry.kind == AssetKind::Model || entry.kind == AssetKind::Image ||
                 entry.kind == AssetKind::Audio || entry.kind == AssetKind::Lut) &&
                ImGui::BeginDragDropSource()) {
                const auto payload = AssetDragPayload::of(entry.kind, entry.path);
                ImGui::SetDragDropPayload(AssetPayloadType, &payload, sizeof(payload));
                ImGui::Text("%s %s", iconFor(entry.kind), entry.path.c_str());
                ImGui::EndDragDropSource();
            }
        };
        if (assetGrid_) {
            if (shown % perRow != 0) {
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            const auto top = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##cell", isSelected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cell - 8, cell + 12))) {
                assetSelected_ = entry.path;
            }
            dragSource();
            const bool hovered = ImGui::IsItemHovered();
            auto* draw = ImGui::GetWindowDrawList();
            ImGui::SetCursorScreenPos(ImVec2(top.x + 4, top.y + 4));
            ImGui::BeginGroup();
            const bool thumb = (entry.kind == AssetKind::Sprite || entry.kind == AssetKind::Image ||
                                entry.kind == AssetKind::Model) &&
                               drawThumbnail(entry.path, entry.kind, cell - 36);
            if (!thumb) {
                ImGui::SetWindowFontScale(2.2f);
                ImGui::TextDisabled("%s", iconFor(entry.kind));
                ImGui::SetWindowFontScale(1);
            }
            ImGui::EndGroup();
            auto name = std::filesystem::path(rest).filename().string();
            if (const auto dot = name.find('.'); dot != std::string::npos && entry.kind != AssetKind::Other) {
                name.resize(dot); // the icon or thumbnail says what kind it is
            }
            const auto label = name.size() > 12 ? name.substr(0, 11) + "~" : name;
            draw->AddText(ImVec2(top.x + 4, top.y + cell - 10), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
            ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + cell + 14));
            ImGui::EndGroup();
            if (hovered) {
                ImGui::SetTooltip("%s\n%s, %s", entry.path.c_str(), assetKindName(entry.kind), kilobytes(entry.size).c_str());
            }
        } else {
            const auto label = std::string(iconFor(entry.kind)) + " " + rest;
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                assetSelected_ = entry.path;
            }
            dragSource();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s, %s", assetKindName(entry.kind), kilobytes(entry.size).c_str());
            }
        }
        ++shown;
        if (ImGui::BeginPopupContextItem("##assetmenu")) {
            assetSelected_ = entry.path;
            if (ImGui::MenuItem(ICON_FA_COPY " Copy path")) {
                ImGui::SetClipboardText(entry.path.c_str());
            }
            if (ImGui::MenuItem(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE " Open externally")) {
                SDL_OpenURL(("file://" + std::filesystem::absolute(catalog_.fileOf(entry.path)).string()).c_str());
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (shown == 0) {
        ImGui::TextDisabled("No files here. Import some, or drop them onto this panel.");
    }
    ImGui::EndChild();
    if (selected) {
        ImGui::SameLine();
        ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_Borders);
        drawAssetDetails(*selected);
        ImGui::EndChild();
    }
    drawSpriteWizard();
}
} // namespace fe
