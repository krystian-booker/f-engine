#pragma once
#include "engine/core/Json.h"
#include "engine/world/Components.h"
#include "engine/world/MapData.h"
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fe {
class World;
struct Model;

// Editor map editing without any UI, so it runs in headless tests.
//
// A world keeps two copies of its map: `authored()` (the file plus editor
// edits) and `map()` (what scripts see and change at run time). Editor edits
// go to both; saving writes `authored()`; undo snapshots are of `authored()`.

// Applies one edit to the authored map and to the live map, then applies the
// live map's colliders and emitters to the world (World::rebuildStatic).
void applyEdit(World& world, const std::function<void(MapData&)>& edit);

// Undo and redo stacks, one pair per world name and map id, so undo never
// writes one world's map into another. Stacks outlive the world: reloading a
// world with the same name and map keeps its history as long as the reloaded
// map is the one the history left (the edits were saved). A reload that
// brings back a different map (unsaved edits discarded, the file changed
// elsewhere) drops the history, so undo and redo never restore discarded
// edits.
class MapEditHistory {
public:
    static constexpr std::size_t Limit = 64;
    // Applies an edit like applyEdit. With `step` the authored map is first
    // recorded as a new undo step (and redo cleared); without it the edit
    // joins the current step (the rest of a drag or paint stroke) and the
    // map is not serialised: `head` is marked stale until settle() (or the
    // next step, undo or redo) brings it up to date, so a stroke costs one
    // edit per frame, not a map serialisation.
    void apply(World& world, const std::function<void(MapData&)>& edit, bool step = true);
    // Brings `head` up to date after a gesture (the editor calls it when the
    // mouse is released). A world replaced while a gesture's head is stale
    // compares against the stale head, so its history is dropped: the
    // conservative outcome.
    void settle(const World& world);
    // Restores the previous (or next) authored map. Sections of the live map
    // are replaced only where the authored map changes, so run-time changes
    // to other sections survive. Throws if the snapshot is not a valid map.
    bool undo(World& world);
    bool redo(World& world);
    // Drops the history of the world's name and map when its authored map is
    // not the one the history last left. apply (at the start of a gesture),
    // undo and redo do this first.
    void sync(const World& world);
    [[nodiscard]] std::size_t undoCount(const World& world) const;
    [[nodiscard]] std::size_t redoCount(const World& world) const;

private:
    struct Stacks {
        std::deque<Json> undo, redo;
        Json head;                   // the authored map after the last step, undo, redo or settle
        const World* world = nullptr; // the world object the last edit touched
        bool headStale = false;      // edits joined a gesture since `head` was taken
    };
    Stacks& stacksFor(World& world, bool check);
    static std::string key(const World& world);
    static void restore(World& world, const Json& snapshot);
    std::map<std::string, Stacks> stacks_;
};

// What the editor's grid brush paints.
struct GridBrush {
    enum class Mode { Walkable, Height, Ramp, AddTag, RemoveTag };
    Mode mode = Mode::Walkable;
    bool walkable = true;
    float height = 0;
    Ramp ramp = Ramp::None;
    std::string tag;
};
// Why the brush cannot paint this grid (unknown layer, tag palette full), or
// an empty string.
[[nodiscard]] std::string brushProblem(const Grid& grid, const GridBrush& brush, int layer);
// Paints a square of cells (radius 0 = one cell); returns cells changed.
// Call brushProblem first: an invalid brush paints nothing.
int paint(Grid& grid, const GridBrush& brush, GridPos centre, int radius);

// Why an edited value would make the map unloadable (the same checks the map
// loader makes), or an empty string. The editor applies an edit only when
// there is no problem, so Save never writes a map that cannot be loaded.
[[nodiscard]] std::string editProblem(const LightSettings& light);
[[nodiscard]] std::string editProblem(const EmitterSettings& emitter);
[[nodiscard]] std::string editProblem(const Environment& environment);
// Step links join two different layers at a cell; ladders need a direction
// and both ends inside the grid.
[[nodiscard]] std::string editProblem(const Grid& grid, const GridLink& link);
// Zone ids are unique within a map and not empty. `index` is the zone being
// edited (its own id is not a duplicate); past the end for a new zone.
[[nodiscard]] std::string editProblem(const std::vector<Zone>& zones, const Zone& zone, std::size_t index);

// Appends a flicker curve key half a second after the last one (scale 1).
void addFlickerKey(LightSettings& light);

// Replaces one part of an environment's map JSON, named by a JSON pointer
// ("/sky/backdrops", "/post/custom"), and reads the environment back; throws
// with the loader's message when the result is invalid.
[[nodiscard]] Environment withEnvironmentJson(const Environment& environment, const std::string& pointer,
                                              const Json& value);

// The cells a zone covers, on the zone's layer; zones on every layer (-1)
// are shown on `fallbackLayer`.
[[nodiscard]] std::vector<GridPos> zoneCells(const Zone& zone, const Grid& grid, int fallbackLayer);

// Run-time entity edits (the World tab). A collider edit replaces the
// component through the registry, so collision queries reach a grown radius
// at once, also while the world is paused (no tick between edit and query).
void editCollider(World& world, entt::entity entity, const Collider& collider);
// A transform edit writes the transform and re-indexes the collision hash
// (World::refreshCollision), so queries see the move at once.
void editTransform(World& world, entt::entity entity, glm::vec3 position, int layer);

namespace editorui {
// Typed reads of JSON that scripts or files supplied (schemas, records,
// props): the fallback when the key is missing or holds another type, where
// Json::value() would throw and abort the panel.
[[nodiscard]] std::string jsonString(const Json& object, const char* key, std::string fallback);
[[nodiscard]] double jsonNumber(const Json& object, const char* key, double fallback);
[[nodiscard]] bool jsonBool(const Json& object, const char* key, bool fallback);
[[nodiscard]] Json jsonObject(const Json& object, const char* key); // {} unless an object
[[nodiscard]] Json jsonArray(const Json& object, const char* key);  // [] unless an array
} // namespace editorui

// Bakes Blender markers (collision boxes, spawn points, lights) into a map.
// Markers that don't fit (unknown layer, outside the grid) are skipped and
// described in `problems`. Returns the number of markers imported.
int importMarkers(MapData& map, const Model& model, std::vector<std::string>& problems);
} // namespace fe
