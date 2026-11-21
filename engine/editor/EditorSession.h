#pragma once
#include "engine/core/Json.h"
#include "engine/editor/MapEdits.h"
#include "engine/editor/Selection.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fe {
class Engine;
class World;

// What an editor does to a project's scenes, without any UI: which scene is
// open, what is selected, every edit with its undo step, and saving. The
// editor application's panels are views of one session; tests drive it
// directly.
//
// A session edits in an engine that does not run the game (the edit engine):
// the open scene is a preview world (ScenePreview.h) whose placements are
// stood in for by simple entities. The game's entry module is loaded only to
// read its `editor.describe` hook, which says what the game's placement
// types, zones and data tables are.
class EditorSession {
public:
    // What changed, for views to refresh by.
    enum Change : unsigned {
        SceneChanged = 1u << 0,     // another scene (or none) is open
        StructureChanged = 1u << 1, // objects added, removed, renamed or reordered
        ValuesChanged = 1u << 2,    // an object's values (a move, a typed edit)
        SelectionChanged = 1u << 3,
        SchemasChanged = 1u << 4,
        SavedChanged = 1u << 5, // the unsaved flag may have flipped
    };

    explicit EditorSession(Engine& engine);
    ~EditorSession();
    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    [[nodiscard]] Engine& engine() { return engine_; }
    [[nodiscard]] const std::filesystem::path& gameRoot() const;

    // Scenes: the folders under maps/ holding a map.json, sorted.
    [[nodiscard]] std::vector<std::string> scenes() const;
    // Every operation that can fail returns whether it worked and reports why
    // not through onStatus.
    bool openScene(const std::string& id);
    void closeScene();
    [[nodiscard]] const std::shared_ptr<World>& world() const { return world_; }
    [[nodiscard]] const MapData* map() const;
    [[nodiscard]] std::string sceneId() const;
    [[nodiscard]] bool unsaved() const;
    bool save();
    // Writes a blank scene of width x depth cells and opens it.
    bool createScene(const std::string& id, const std::string& name, int width, int depth);
    // Saves the open scene under another id (a copy of its folder with the
    // current edits) and opens the copy.
    bool saveSceneAs(const std::string& id);
    bool renameScene(const std::string& id);
    // Removes a scene's folder; closes it first when it is the open one.
    bool deleteScene(const std::string& id);

    // The game's description of its types (PropLinks.h), or the editor's
    // default one while the game describes nothing.
    [[nodiscard]] const Json& schemas() const { return schemas_; }
    void refreshSchemas();
    // The {field: spec} object of a type in the first group that has it.
    [[nodiscard]] const Json* fieldsFor(const std::string& type,
                                        std::initializer_list<const char*> groups) const;
    [[nodiscard]] std::vector<std::string> typeNames(const char* group) const;
    [[nodiscard]] std::string defaultType(const char* group) const;

    // Selection: one primary object and any number of extras.
    [[nodiscard]] const Selection& selection() const { return selection_; }
    [[nodiscard]] const std::vector<Selection>& extras() const { return extras_; }
    [[nodiscard]] std::vector<Selection> selected() const; // extras, then the primary
    [[nodiscard]] bool isSelected(const Selection& selection) const;
    void select(const Selection& selection);
    void toggle(const Selection& selection); // Ctrl+click
    void selectAll(const std::vector<Selection>& all);

    // Applies an edit to the open scene as one undo step. With `gesture` the
    // edit joins the step in progress (the rest of a drag); endGesture() ends
    // it. Views are told through onChanged.
    void edit(const std::function<void(MapData&)>& change, bool gesture = false,
              unsigned changed = ValuesChanged);
    void endGesture();
    void undo();
    void redo();
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;

    // New objects stand at `at` (snapped to the grid where the kind sits on
    // it) and become the selection. `name` names a point; other kinds get a
    // fresh id.
    Selection add(Selection::Kind kind, glm::vec3 at, const std::string& name = {});
    // A placement of `type`, or of the game's default type when empty.
    Selection addPlacement(const std::string& type, glm::vec3 at);
    // Adds a placement made elsewhere (an asset dropped on the scene).
    Selection place(const Placement& placement);
    void deleteSelection();
    void duplicateSelection();
    // Moves the selection's gizmo transform (and the extras by the same
    // offset); a gesture when part of a drag.
    bool setTransform(const Selection& selection, const SelectionTransform& transform, bool gesture);
    bool renamePoint(const std::string& from, const std::string& to);

    // Walkability. A scene's walkable cells are derived from its geometry and
    // placed models (the bake); cells painted by hand are kept as overrides
    // the bake respects. A scene never baked is painted directly.
    // Bakes the whole scene, or the cells {x, z, width, depth}; joins the
    // undo step in progress with `join`.
    bool bake(std::optional<glm::ivec4> region = std::nullopt, bool join = false);
    // Paints the square of cells around `centre` with the brush, or with
    // `clear` removes their overrides so the bake's own values return.
    void paintWalkability(GridPos centre, int radius, const GridBrush& brush, bool clear, bool gesture);
    // The same for every cell of the rectangle between two cells, and for
    // the connected cells that share the start cell's walkability (a flood
    // fill; ramps cannot be flooded). Each is one undo step.
    void paintWalkabilityRect(GridPos a, GridPos b, const GridBrush& brush, bool clear);
    void floodWalkability(GridPos start, const GridBrush& brush);

    // Replaces the scene's geometry with a .glb/.gltf file (copied into the
    // scene's folder), saves the scene and bakes its walkability from it.
    bool importGeometry(const std::filesystem::path& file);
    // Adds the spawn points and lights a Blender export marks in the scene's
    // geometry; returns how many, and says what did not fit through onStatus.
    int importGeometryMarkers();

    // An object as the map file holds it, and a typed edit of it: the edited
    // JSON replaces the object when the map still loads with it. Returns the
    // loader's complaint, empty when applied.
    [[nodiscard]] Json objectJson(const Selection& selection) const;
    std::string setObjectJson(const Selection& selection, const Json& value);
    // The same for any part of the map file by JSON pointer: "/environment",
    // "/props", "/name".
    [[nodiscard]] Json mapJson(const std::string& pointer) const;
    std::string setMapJson(const std::string& pointer, const Json& value, unsigned changed = ValuesChanged);

    // The grid layer new objects go on and ground picks use.
    int layer = 0;
    // Counts edits, undos and scene changes: a view that caches what it
    // derived from the scene compares it.
    [[nodiscard]] std::uint64_t revision() const { return revision_; }

    std::function<void(unsigned changes)> onChanged;
    std::function<void(const std::string&)> onStatus;

private:
    void changed(unsigned changes);
    void status(const std::string& text) const;
    void respawnChanged(const MapData& before);
    [[nodiscard]] std::string pointerFor(const Selection& selection) const;
    void dropInvalidSelection();
    [[nodiscard]] std::string bakedSignature() const;
    void autoRebake();

    Engine& engine_;
    std::shared_ptr<World> world_;
    MapEditHistory history_;
    Json schemas_;
    Selection selection_;
    std::vector<Selection> extras_;
    bool gesture_ = false;
    std::string bakedSignature_; // the models the last bake saw, and where
    std::uint64_t revision_ = 0;
};
} // namespace fe
