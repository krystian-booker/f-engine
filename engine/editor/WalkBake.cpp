#include "engine/editor/WalkBake.h"
#include "engine/assets/Model.h"
#include "engine/core/AssetCache.h"
#include "engine/core/Paths.h"
#include "engine/editor/MapEdits.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <sstream>
#include <limits>
#include <set>

namespace fe {
const std::vector<int> MeshRaycaster::None;

namespace {
// Möller–Trumbore: distance along a ray to a triangle, if it is hit.
std::optional<float> rayTriangle(glm::vec3 origin, glm::vec3 direction, glm::vec3 a, glm::vec3 b, glm::vec3 c) {
    const auto e1 = b - a, e2 = c - a;
    const auto p = glm::cross(direction, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-9f) {
        return std::nullopt;
    }
    const float inv = 1 / det;
    const auto s = origin - a;
    const float u = glm::dot(s, p) * inv;
    if (u < -1e-5f || u > 1 + 1e-5f) {
        return std::nullopt;
    }
    const auto q = glm::cross(s, e1);
    const float v = glm::dot(direction, q) * inv;
    if (v < -1e-5f || u + v > 1 + 1e-5f) {
        return std::nullopt;
    }
    const float t = glm::dot(e2, q) * inv;
    return t > 1e-6f ? std::optional(t) : std::nullopt;
}

float cross2(glm::vec2 a, glm::vec2 b) {
    return a.x * b.y - a.y * b.x;
}

bool pointInTriangle(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    const float d1 = cross2(b - a, p - a), d2 = cross2(c - b, p - b), d3 = cross2(a - c, p - c);
    const bool negative = d1 < 0 || d2 < 0 || d3 < 0, positive = d1 > 0 || d2 > 0 || d3 > 0;
    return !(negative && positive);
}

bool segmentsCross(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d) {
    const float d1 = cross2(d - c, a - c), d2 = cross2(d - c, b - c);
    const float d3 = cross2(b - a, c - a), d4 = cross2(b - a, d - a);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

// Whether a triangle's xz projection overlaps the rectangle lo..hi: a vertex
// inside it, a corner inside the triangle, or crossing edges.
bool triangleOverlapsRect(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 lo, glm::vec2 hi) {
    for (const auto& v : {a, b, c}) {
        if (v.x >= lo.x && v.x <= hi.x && v.y >= lo.y && v.y <= hi.y) {
            return true;
        }
    }
    const glm::vec2 corners[4] = {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}};
    for (const auto& corner : corners) {
        if (pointInTriangle(corner, a, b, c)) {
            return true;
        }
    }
    const glm::vec2 edges[3][2] = {{a, b}, {b, c}, {c, a}};
    for (const auto& edge : edges) {
        for (int i = 0; i < 4; ++i) {
            if (segmentsCross(edge[0], edge[1], corners[i], corners[(i + 1) % 4])) {
                return true;
            }
        }
    }
    return false;
}
} // namespace

std::vector<const ModelPart*> collisionParts(const Model& model) {
    std::vector<bool> marked(model.nodes.size(), false);
    bool any = false;
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const auto& properties = model.nodes[i].properties;
        const auto it = properties.find("collision");
        if (it != properties.end() && !it->is_null() && !(it->is_boolean() && !it->get<bool>())) {
            marked[i] = true;
            any = true;
        }
    }
    if (any) {
        // Children of a collision node are collision geometry too (parents come first in `order`).
        for (const int index : model.order) {
            const auto& node = model.nodes[static_cast<std::size_t>(index)];
            if (node.parent >= 0 && marked[static_cast<std::size_t>(node.parent)]) {
                marked[static_cast<std::size_t>(index)] = true;
            }
        }
    }
    std::vector<const ModelPart*> parts;
    for (const auto& part : model.parts) {
        const bool inRange = part.node >= 0 && part.node < static_cast<int>(marked.size());
        if (any ? (inRange && marked[static_cast<std::size_t>(part.node)]) : !part.hidden) {
            parts.push_back(&part);
        }
    }
    return parts;
}

bool hasCollisionNodes(const Model& model) {
    return std::any_of(model.nodes.begin(), model.nodes.end(), [](const ModelNode& node) {
        const auto it = node.properties.find("collision");
        return it != node.properties.end() && !it->is_null() && !(it->is_boolean() && !it->get<bool>());
    });
}

glm::mat4 placementMatrix(const Placement& placement) {
    return glm::translate(glm::mat4(1), placement.position) *
           glm::rotate(glm::mat4(1), glm::radians(placement.yaw), glm::vec3(0, 1, 0)) *
           glm::scale(glm::mat4(1), placement.scale);
}

const std::string* bakedModelOf(const Placement& placement) {
    const auto model = placement.props.find("model");
    if (model == placement.props.end() || !model->is_string()) {
        return nullptr;
    }
    const auto solid = placement.props.find("solid");
    if (solid != placement.props.end() && solid->is_boolean() && !solid->get<bool>()) {
        return nullptr;
    }
    if (placement.props.contains("pages")) {
        return nullptr;
    }
    return &model->get_ref<const std::string&>();
}

BakeScene sceneForMap(const MapData& map, AssetCache& assets, const std::string& geometryPath) {
    BakeScene scene;
    if (const auto terrain = assets.tryModel(geometryPath)) {
        scene.pieces.push_back({terrain.get(), glm::mat4(1)});
        scene.models.push_back(terrain);
    }
    for (const auto& placement : map.placements) {
        const auto path = bakedModelOf(placement);
        if (!path) {
            continue;
        }
        if (const auto model = assets.tryModel(*path)) {
            scene.pieces.push_back({model.get(), placementMatrix(placement)});
            scene.models.push_back(model);
        } else {
            scene.problems.push_back(placement.id + ": model not found: " + *path);
        }
    }
    return scene;
}

MeshRaycaster::MeshRaycaster(const Model& model) {
    add(model, glm::mat4(1));
    finish();
}

MeshRaycaster::MeshRaycaster(std::span<const BakePiece> scene) {
    for (const auto& piece : scene) {
        if (piece.model) {
            add(*piece.model, piece.transform);
        }
    }
    finish();
}

void MeshRaycaster::add(const Model& model, const glm::mat4& transform) {
    const bool posed = model.restPose.size() == model.nodes.size();
    for (const auto* part : collisionParts(model)) {
        const glm::mat4 m =
            transform * (posed && part->node >= 0 && part->node < static_cast<int>(model.restPose.size())
                             ? model.restPose[static_cast<std::size_t>(part->node)]
                             : glm::mat4(1));
        const bool water = part->material.kind == MaterialKind::Water;
        for (std::size_t i = 0; i + 2 < part->indices.size(); i += 3) {
            Triangle t;
            t.a = glm::vec3(m * glm::vec4(part->vertices[part->indices[i]].position, 1));
            t.b = glm::vec3(m * glm::vec4(part->vertices[part->indices[i + 1]].position, 1));
            t.c = glm::vec3(m * glm::vec4(part->vertices[part->indices[i + 2]].position, 1));
            const auto n = glm::cross(t.b - t.a, t.c - t.a);
            if (glm::length(n) < 1e-10f) {
                continue;
            }
            t.normal = glm::normalize(n);
            t.water = water;
            triangles_.push_back(t);
        }
    }
}

void MeshRaycaster::finish() {
    if (triangles_.empty()) {
        min_ = max_ = glm::vec3(0);
        return;
    }
    min_ = glm::vec3(std::numeric_limits<float>::max());
    max_ = -min_;
    for (const auto& t : triangles_) {
        for (const auto& v : {t.a, t.b, t.c}) {
            min_ = glm::min(min_, v);
            max_ = glm::max(max_, v);
        }
    }
    bucketsX_ = std::clamp(static_cast<int>(std::ceil(max_.x - min_.x)) + 1, 1, 4096);
    bucketsZ_ = std::clamp(static_cast<int>(std::ceil(max_.z - min_.z)) + 1, 1, 4096);
    buckets_.resize(static_cast<std::size_t>(bucketsX_) * static_cast<std::size_t>(bucketsZ_));
    for (int index = 0; index < static_cast<int>(triangles_.size()); ++index) {
        const auto& t = triangles_[static_cast<std::size_t>(index)];
        const auto lo = glm::min(t.a, glm::min(t.b, t.c)), hi = glm::max(t.a, glm::max(t.b, t.c));
        const int x0 = std::clamp(static_cast<int>(std::floor(lo.x - min_.x)), 0, bucketsX_ - 1);
        const int x1 = std::clamp(static_cast<int>(std::floor(hi.x - min_.x)), 0, bucketsX_ - 1);
        const int z0 = std::clamp(static_cast<int>(std::floor(lo.z - min_.z)), 0, bucketsZ_ - 1);
        const int z1 = std::clamp(static_cast<int>(std::floor(hi.z - min_.z)), 0, bucketsZ_ - 1);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                buckets_[static_cast<std::size_t>(z * bucketsX_ + x)].push_back(index);
            }
        }
    }
}

const std::vector<int>& MeshRaycaster::bucket(float x, float z) const {
    if (buckets_.empty()) {
        return None;
    }
    const int bx = static_cast<int>(std::floor(x - min_.x)), bz = static_cast<int>(std::floor(z - min_.z));
    if (bx < 0 || bz < 0 || bx >= bucketsX_ || bz >= bucketsZ_) {
        return None;
    }
    return buckets_[static_cast<std::size_t>(bz * bucketsX_ + bx)];
}

template<class Visit> bool MeshRaycaster::anyBucket(glm::vec2 lo, glm::vec2 hi, Visit&& visit) const {
    if (buckets_.empty()) {
        return false;
    }
    const int x0 = std::clamp(static_cast<int>(std::floor(lo.x - min_.x)), 0, bucketsX_ - 1);
    const int x1 = std::clamp(static_cast<int>(std::floor(hi.x - min_.x)), 0, bucketsX_ - 1);
    const int z0 = std::clamp(static_cast<int>(std::floor(lo.y - min_.z)), 0, bucketsZ_ - 1);
    const int z1 = std::clamp(static_cast<int>(std::floor(hi.y - min_.z)), 0, bucketsZ_ - 1);
    if (hi.x < min_.x || lo.x > max_.x || hi.y < min_.z || lo.y > max_.z) {
        return false;
    }
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            for (const int index : buckets_[static_cast<std::size_t>(z * bucketsX_ + x)]) {
                if (visit(triangles_[static_cast<std::size_t>(index)])) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::optional<MeshRaycaster::Hit> MeshRaycaster::highest(float x, float z, float minY, float maxY) const {
    const glm::vec3 origin(x, std::min(maxY, max_.y + 1), z), down(0, -1, 0);
    std::optional<Hit> best;
    for (const int index : bucket(x, z)) {
        const auto& t = triangles_[static_cast<std::size_t>(index)];
        const auto hit = rayTriangle(origin, down, t.a, t.b, t.c);
        if (!hit) {
            continue;
        }
        const float y = origin.y - *hit;
        if (y < minY || y > maxY) {
            continue;
        }
        if (!best || y > best->y) {
            best = Hit{y, t.normal.y < 0 ? -t.normal : t.normal, t.water};
        }
    }
    return best;
}

bool MeshRaycaster::blockedAbove(float x, float y, float z, float distance) const {
    const glm::vec3 origin(x, y, z), up(0, 1, 0);
    for (const int index : bucket(x, z)) {
        const auto& t = triangles_[static_cast<std::size_t>(index)];
        if (const auto hit = rayTriangle(origin, up, t.a, t.b, t.c); hit && *hit <= distance) {
            return true;
        }
    }
    return false;
}

bool MeshRaycaster::obstacleIn(glm::vec2 lo, glm::vec2 hi, float minY, float maxY, float levelCos) const {
    return anyBucket(lo, hi, [&](const Triangle& t) {
        if (std::abs(t.normal.y) >= levelCos) {
            return false; // ground, a ceiling or a table top: the rays judge those
        }
        const float top = std::max(t.a.y, std::max(t.b.y, t.c.y));
        const float bottom = std::min(t.a.y, std::min(t.b.y, t.c.y));
        if (top <= minY || bottom >= maxY) {
            return false;
        }
        return triangleOverlapsRect({t.a.x, t.a.z}, {t.b.x, t.b.z}, {t.c.x, t.c.z}, lo, hi);
    });
}

namespace {
// The height of a baked cell's edge towards a neighbour (dx, dz), ramps included.
float bakedEdge(const BakeCell& cell, int dx, int dz) {
    const float half = cell.rise * 0.5f;
    switch (cell.ramp) {
    case Ramp::East: return cell.height + (dx > 0 ? cell.rise : dx < 0 ? 0 : half);
    case Ramp::West: return cell.height + (dx < 0 ? cell.rise : dx > 0 ? 0 : half);
    case Ramp::South: return cell.height + (dz > 0 ? cell.rise : dz < 0 ? 0 : half);
    case Ramp::North: return cell.height + (dz < 0 ? cell.rise : dz > 0 ? 0 : half);
    case Ramp::None: break;
    }
    return cell.height;
}

// Blocks walkable cells outside the largest region that steps of `step`
// connect: roofs and ledges nothing leads to. Cells the bake did not sample
// (outside its region, seeded from the grid) take part in the connectivity
// but are never changed.
void pruneIslands(BakeResult& result, float step) {
    const int w = result.width, d = result.depth;
    std::vector<int> component(static_cast<std::size_t>(w * d), -1);
    std::vector<int> sizes;
    std::vector<int> stack;
    for (int start = 0; start < w * d; ++start) {
        if (component[static_cast<std::size_t>(start)] >= 0 || !result.cells[static_cast<std::size_t>(start)].walkable) {
            continue;
        }
        const int id = static_cast<int>(sizes.size());
        sizes.push_back(0);
        stack.assign(1, start);
        component[static_cast<std::size_t>(start)] = id;
        while (!stack.empty()) {
            const int index = stack.back();
            stack.pop_back();
            ++sizes[static_cast<std::size_t>(id)];
            const int x = index % w, z = index / w;
            const auto& cell = result.cells[static_cast<std::size_t>(index)];
            const int steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& s : steps) {
                const int nx = x + s[0], nz = z + s[1];
                if (nx < 0 || nz < 0 || nx >= w || nz >= d) {
                    continue;
                }
                const int next = nz * w + nx;
                const auto& other = result.cells[static_cast<std::size_t>(next)];
                if (component[static_cast<std::size_t>(next)] >= 0 || !other.walkable) {
                    continue;
                }
                if (std::abs(bakedEdge(cell, s[0], s[1]) - bakedEdge(other, -s[0], -s[1])) > step + 1e-4f) {
                    continue;
                }
                component[static_cast<std::size_t>(next)] = id;
                stack.push_back(next);
            }
        }
    }
    if (sizes.empty()) {
        return;
    }
    const int main = static_cast<int>(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
    for (int index = 0; index < w * d; ++index) {
        auto& cell = result.cells[static_cast<std::size_t>(index)];
        if (cell.hit && cell.walkable && component[static_cast<std::size_t>(index)] != main) {
            cell.walkable = false;
            cell.island = true;
            cell.confidence = 1;
            if (cell.partial) {
                cell.partial = false;
                --result.partial;
            } else {
                --result.walkable;
            }
            ++result.blocked;
            ++result.islands;
        }
    }
}

// A quarter-cell square shrunk a little, so geometry lying exactly on a cell
// or quarter boundary (a house wall on a whole-cell footprint) belongs to
// the cell it faces, not to its neighbour too.
constexpr float QuarterShrink = 0.02f;
} // namespace

BakeResult bakeWalkability(const Model& model, const Grid& grid, const BakeOptions& options) {
    const BakePiece piece{&model, glm::mat4(1)};
    return bakeWalkability(std::span<const BakePiece>(&piece, 1), grid, options);
}

BakeResult bakeWalkability(std::span<const BakePiece> scene, const Grid& grid, const BakeOptions& options) {
    const MeshRaycaster mesh(scene);
    BakeResult result;
    result.width = grid.width();
    result.depth = grid.depth();
    result.cells.resize(static_cast<std::size_t>(result.width) * static_cast<std::size_t>(result.depth));
    const int n = std::clamp(options.samples, 1, 16);
    const float slopeCos = std::cos(glm::radians(std::clamp(options.maxSlopeDegrees, 0.0f, 89.0f)));
    const float heightStep = std::max(options.heightStep, 0.01f);
    const auto rounded = [&](float value) { return std::round(value / heightStep) * heightStep; };
    const bool seeded = options.region && options.layer >= 0 && options.layer < grid.layerCount();
    if (seeded) {
        // Cells outside the region keep what the grid says, so island pruning
        // and the edge ring see the whole map; they are never written.
        for (int z = 0; z < result.depth; ++z) {
            for (int x = 0; x < result.width; ++x) {
                if (options.inRegion(x, z)) {
                    continue;
                }
                const auto& g = grid.at(x, z, options.layer);
                auto& cell = result.at(x, z);
                cell.walkable = g.walkable;
                cell.height = g.height;
                cell.ramp = g.ramp;
                cell.rise = g.rise;
            }
        }
    }
    std::vector<std::pair<int, Box>> quarters; // cell index, box
    struct Sample {
        bool hit = false, ground = false, water = false, drop = false;
        float y = 0;
    };
    std::vector<Sample> samples(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    const float quarter = 1.0f / static_cast<float>(n);
    for (int z = 0; z < result.depth; ++z) {
        for (int x = 0; x < result.width; ++x) {
            auto& cell = result.at(x, z);
            if (!options.inRegion(x, z)) {
                continue;
            }
            // 1. Sample the cell.
            std::vector<float> heights;
            int hits = 0;
            for (int sz = 0; sz < n; ++sz) {
                for (int sx = 0; sx < n; ++sx) {
                    auto& s = samples[static_cast<std::size_t>(sz * n + sx)];
                    s = {};
                    const float px = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) * quarter;
                    const float pz = static_cast<float>(z) + (static_cast<float>(sz) + 0.5f) * quarter;
                    const auto hit = mesh.highest(px, pz, options.minHeight, options.maxHeight);
                    if (!hit) {
                        continue;
                    }
                    s.hit = true;
                    s.y = hit->y;
                    s.water = hit->water;
                    ++hits;
                    const bool level = hit->normal.y >= slopeCos;
                    const bool headroom = !mesh.blockedAbove(px, hit->y + 0.05f, pz, options.clearance);
                    s.ground = level && headroom && !hit->water;
                    if (s.ground) {
                        heights.push_back(hit->y);
                    }
                }
            }
            cell.hit = hits > 0;
            if (!cell.hit) {
                ++result.unsampled;
                continue;
            }
            // 2. The floor: the largest cluster of ground heights (the lowest
            // when equal); ground far from it is another floor (a post's top).
            float floor = 0;
            if (!heights.empty()) {
                std::sort(heights.begin(), heights.end());
                std::size_t bestStart = 0, bestCount = 0, start = 0;
                for (std::size_t i = 1; i <= heights.size(); ++i) {
                    if (i == heights.size() || heights[i] - heights[i - 1] > options.step) {
                        if (i - start > bestCount) {
                            bestCount = i - start;
                            bestStart = start;
                        }
                        start = i;
                    }
                }
                floor = heights[bestStart + bestCount / 2];
                for (auto& s : samples) {
                    if (s.ground && std::abs(s.y - floor) > options.step) {
                        s.ground = false;
                    }
                }
            }
            // 3. Anything standing in a quarter, however thin, makes it not
            // ground; a quarter well below the floor is a drop.
            for (int sz = 0; sz < n; ++sz) {
                for (int sx = 0; sx < n; ++sx) {
                    auto& s = samples[static_cast<std::size_t>(sz * n + sx)];
                    const glm::vec2 lo(static_cast<float>(x) + static_cast<float>(sx) * quarter + QuarterShrink,
                                       static_cast<float>(z) + static_cast<float>(sz) * quarter + QuarterShrink);
                    const glm::vec2 hi = lo + glm::vec2(quarter - 2 * QuarterShrink);
                    if (s.ground && mesh.obstacleIn(lo, hi, floor + 0.05f, floor + options.clearance, slopeCos)) {
                        s.ground = false;
                    }
                    s.drop = !s.ground && (!s.hit || (!s.water && s.y < floor - options.step));
                }
            }
            int ground = 0, water = 0;
            for (const auto& s : samples) {
                ground += s.ground ? 1 : 0;
                water += s.hit && s.water ? 1 : 0;
            }
            const float total = static_cast<float>(n * n);
            cell.coverage = static_cast<float>(ground) / total;
            cell.water = water > ground && static_cast<float>(water) / total >= 0.5f;
            cell.height = rounded(floor);
            // 4. The verdict.
            if (cell.water) {
                cell.walkable = false;
                cell.confidence = 1;
                std::vector<float> levels;
                for (const auto& s : samples) {
                    if (s.hit && s.water) {
                        levels.push_back(s.y);
                    }
                }
                std::sort(levels.begin(), levels.end());
                cell.height = rounded(levels[levels.size() / 2]);
                ++result.blocked;
            } else if (cell.coverage >= options.minCoverage) {
                cell.walkable = true;
                cell.partial = ground < n * n;
                cell.confidence = std::clamp((cell.coverage - options.minCoverage) / std::max(1 - options.minCoverage, 0.01f), 0.0f, 1.0f);
                if (cell.partial) {
                    ++result.partial;
                } else {
                    ++result.walkable;
                }
                // The quarters that are not ground become boxes bodies cannot enter.
                for (int sz = 0; sz < n; ++sz) {
                    for (int sx = 0; sx < n; ++sx) {
                        const auto& s = samples[static_cast<std::size_t>(sz * n + sx)];
                        if (s.ground || (s.drop && !options.boxDrops)) {
                            continue;
                        }
                        const float qx = static_cast<float>(x) + static_cast<float>(sx) * quarter;
                        const float qz = static_cast<float>(z) + static_cast<float>(sz) * quarter;
                        quarters.push_back({z * result.width + x,
                                            {{qx, cell.height - 0.05f, qz}, {qx + quarter, cell.height + 1.2f, qz + quarter}}});
                    }
                }
            } else {
                cell.walkable = false;
                cell.confidence = std::clamp((options.minCoverage - cell.coverage) / std::max(options.minCoverage, 0.01f), 0.0f, 1.0f);
                ++result.blocked;
            }
            // 5. Ramps: heights that climb across the cell along one axis.
            if (cell.walkable && ground >= n * 2 && n >= 2) {
                float left = 0, right = 0, front = 0, back = 0;
                int nl = 0, nr = 0, nf = 0, nb = 0;
                for (int sz = 0; sz < n; ++sz) {
                    for (int sx = 0; sx < n; ++sx) {
                        const auto& s = samples[static_cast<std::size_t>(sz * n + sx)];
                        if (!s.ground) {
                            continue;
                        }
                        if (sx < n / 2) {
                            left += s.y;
                            ++nl;
                        } else if (sx >= (n + 1) / 2) {
                            right += s.y;
                            ++nr;
                        }
                        if (sz < n / 2) {
                            front += s.y;
                            ++nf;
                        } else if (sz >= (n + 1) / 2) {
                            back += s.y;
                            ++nb;
                        }
                    }
                }
                const float dx = nl && nr ? right / static_cast<float>(nr) - left / static_cast<float>(nl) : 0;
                const float dz = nf && nb ? back / static_cast<float>(nb) - front / static_cast<float>(nf) : 0;
                // The halves' means sit half a cell apart: the full rise is twice their difference.
                const float riseX = dx * 2, riseZ = dz * 2;
                if (std::max(std::abs(riseX), std::abs(riseZ)) >= heightStep) {
                    if (std::abs(riseX) >= std::abs(riseZ)) {
                        cell.ramp = riseX > 0 ? Ramp::East : Ramp::West;
                        cell.rise = rounded(std::abs(riseX));
                    } else {
                        cell.ramp = riseZ > 0 ? Ramp::South : Ramp::North;
                        cell.rise = rounded(std::abs(riseZ));
                    }
                    // A ramp's height is its low edge.
                    cell.height = rounded(floor - cell.rise * 0.5f);
                }
            }
        }
    }
    if (options.blockEdges) {
        for (int z = 0; z < result.depth; ++z) {
            for (int x = 0; x < result.width; ++x) {
                if ((x != 0 && z != 0 && x != result.width - 1 && z != result.depth - 1) || !options.inRegion(x, z)) {
                    continue;
                }
                auto& cell = result.at(x, z);
                if (cell.hit && cell.walkable) {
                    cell.walkable = false;
                    cell.confidence = 1;
                    if (cell.partial) {
                        cell.partial = false;
                        --result.partial;
                    } else {
                        --result.walkable;
                    }
                    ++result.blocked;
                }
            }
        }
    }
    if (options.pruneIslands) {
        pruneIslands(result, options.step);
    }
    std::vector<Box> boxes;
    for (const auto& [index, box] : quarters) {
        if (result.cells[static_cast<std::size_t>(index)].walkable) {
            boxes.push_back(box);
        }
    }
    result.colliders = mergeBoxes(std::move(boxes));
    return result;
}

int applyBake(MapData& map, const BakeResult& result, const BakeOptions& options) {
    auto& grid = map.grid;
    if (options.layer < 0 || options.layer >= grid.layerCount() || result.width != grid.width() ||
        result.depth != grid.depth()) {
        return 0;
    }
    const auto waterBit = grid.tagBit("water", true);
    int changed = 0;
    for (int z = 0; z < result.depth; ++z) {
        for (int x = 0; x < result.width; ++x) {
            if (!options.inRegion(x, z)) {
                continue;
            }
            const auto& baked = result.at(x, z);
            if (!baked.hit) {
                continue; // nothing seen: leave the cell as it is
            }
            const GridPos p{x, z, options.layer};
            auto cell = grid.at(p);
            const auto before = cell;
            cell.walkable = baked.walkable;
            cell.height = baked.height;
            cell.ramp = baked.walkable ? baked.ramp : Ramp::None;
            cell.rise = cell.ramp == Ramp::None ? 1 : baked.rise;
            if (waterBit) {
                cell.tags = baked.water ? (cell.tags | waterBit) : (cell.tags & ~waterBit);
            }
            if (!(cell == before)) {
                grid.setCell(p, cell);
                ++changed;
            }
        }
    }
    std::vector<Box> boxes;
    if (options.region) {
        // Keep the derived boxes outside the region; the bake brings the rest.
        const glm::vec2 lo(static_cast<float>(options.region->x), static_cast<float>(options.region->y));
        const glm::vec2 hi = lo + glm::vec2(static_cast<float>(options.region->z), static_cast<float>(options.region->w));
        for (const auto& box : map.bake.colliders) {
            for (const auto& piece : cutBox(box, lo, hi)) {
                boxes.push_back(piece);
            }
        }
    }
    boxes.insert(boxes.end(), result.colliders.begin(), result.colliders.end());
    map.bake.colliders = mergeBoxes(std::move(boxes));
    changed += applyOverrides(map, options.region);
    return changed;
}

int applyOverrides(MapData& map, std::optional<glm::ivec4> region) {
    auto& grid = map.grid;
    const auto inRegion = [&](int x, int z) {
        return !region || (x >= region->x && z >= region->y && x < region->x + region->z && z < region->y + region->w);
    };
    int changed = 0;
    for (const auto& over : map.bake.overrides) {
        if (over.layer < 0 || over.layer >= grid.layerCount()) {
            continue;
        }
        for (int z = over.z; z < over.z + over.d; ++z) {
            for (int x = over.x; x < over.x + over.w; ++x) {
                if (!grid.inRange(x, z, over.layer) || !inRegion(x, z)) {
                    continue;
                }
                const GridPos p{x, z, over.layer};
                auto cell = grid.at(p);
                const auto before = cell;
                if (over.walkable) {
                    cell.walkable = *over.walkable;
                }
                if (over.height) {
                    cell.height = *over.height;
                }
                if (over.ramp) {
                    cell.ramp = *over.ramp;
                    cell.rise = cell.ramp == Ramp::None ? 1 : over.rise;
                }
                for (const auto& tag : over.tags) {
                    cell.tags |= grid.tagBit(tag, true);
                }
                for (const auto& tag : over.untags) {
                    cell.tags &= ~grid.tagMask({tag});
                }
                if (!(cell == before)) {
                    grid.setCell(p, cell);
                    ++changed;
                }
            }
        }
    }
    return changed;
}

namespace {
void writeBrush(CellOverride& over, const GridBrush& brush) {
    switch (brush.mode) {
    case GridBrush::Mode::Walkable: over.walkable = brush.walkable; break;
    case GridBrush::Mode::Height: over.height = brush.height; break;
    case GridBrush::Mode::Ramp:
        over.ramp = brush.ramp;
        over.rise = 1;
        break;
    case GridBrush::Mode::AddTag:
        if (std::find(over.tags.begin(), over.tags.end(), brush.tag) == over.tags.end()) {
            over.tags.push_back(brush.tag);
        }
        std::erase(over.untags, brush.tag);
        break;
    case GridBrush::Mode::RemoveTag:
        if (std::find(over.untags.begin(), over.untags.end(), brush.tag) == over.untags.end()) {
            over.untags.push_back(brush.tag);
        }
        std::erase(over.tags, brush.tag);
        break;
    }
}

bool covers(const CellOverride& over, GridPos cell) {
    return over.layer == cell.layer && cell.x >= over.x && cell.x < over.x + over.w && cell.z >= over.z &&
           cell.z < over.z + over.d;
}
} // namespace

bool setOverride(MapData& map, GridPos cell, const GridBrush& brush) {
    if (!map.grid.inRange(cell) || ((brush.mode == GridBrush::Mode::AddTag || brush.mode == GridBrush::Mode::RemoveTag) && brush.tag.empty())) {
        return false;
    }
    auto& overrides = map.bake.overrides;
    // The latest override covering the cell, when it is this cell alone, takes the stroke.
    CellOverride* target = nullptr;
    for (auto it = overrides.rbegin(); it != overrides.rend(); ++it) {
        if (covers(*it, cell)) {
            if (it->w == 1 && it->d == 1) {
                target = &*it;
            }
            break;
        }
    }
    if (!target) {
        overrides.push_back({cell.x, cell.z, cell.layer, 1, 1, std::nullopt, std::nullopt, std::nullopt, 1, {}, {}});
        target = &overrides.back();
    }
    writeBrush(*target, brush);
    applyOverrides(map, glm::ivec4(cell.x, cell.z, 1, 1));
    return true;
}

bool setOverrideRect(MapData& map, GridPos a, GridPos b, const GridBrush& brush) {
    if (a.layer != b.layer || !map.grid.inRange(a) || !map.grid.inRange(b) ||
        ((brush.mode == GridBrush::Mode::AddTag || brush.mode == GridBrush::Mode::RemoveTag) && brush.tag.empty())) {
        return false;
    }
    CellOverride over{std::min(a.x, b.x), std::min(a.z, b.z), a.layer, std::abs(a.x - b.x) + 1, std::abs(a.z - b.z) + 1,
                      std::nullopt,       std::nullopt,       std::nullopt, 1, {}, {}};
    writeBrush(over, brush);
    map.bake.overrides.push_back(over);
    applyOverrides(map, glm::ivec4(over.x, over.z, over.w, over.d));
    return true;
}

bool clearOverride(MapData& map, GridPos cell) {
    auto& overrides = map.bake.overrides;
    std::vector<CellOverride> kept;
    bool changed = false;
    for (const auto& over : overrides) {
        if (!covers(over, cell)) {
            kept.push_back(over);
            continue;
        }
        changed = true;
        // The rectangle minus the cell: the rows above and below it, then the
        // cells left and right of it in its row.
        if (cell.z > over.z) {
            auto top = over;
            top.d = cell.z - over.z;
            kept.push_back(top);
        }
        if (cell.z + 1 < over.z + over.d) {
            auto bottom = over;
            bottom.z = cell.z + 1;
            bottom.d = over.z + over.d - bottom.z;
            kept.push_back(bottom);
        }
        if (cell.x > over.x) {
            auto left = over;
            left.z = cell.z;
            left.d = 1;
            left.w = cell.x - over.x;
            kept.push_back(left);
        }
        if (cell.x + 1 < over.x + over.w) {
            auto right = over;
            right.z = cell.z;
            right.d = 1;
            right.x = cell.x + 1;
            right.w = over.x + over.w - right.x;
            kept.push_back(right);
        }
    }
    if (changed) {
        overrides = std::move(kept);
    }
    return changed;
}

std::vector<GridPos> overriddenCells(const MapData& map, int layer) {
    std::vector<GridPos> cells;
    std::set<std::pair<int, int>> seen;
    for (const auto& over : map.bake.overrides) {
        if (over.layer != layer) {
            continue;
        }
        for (int z = over.z; z < over.z + over.d; ++z) {
            for (int x = over.x; x < over.x + over.w; ++x) {
                if (map.grid.inRange(x, z, layer) && seen.insert({x, z}).second) {
                    cells.push_back({x, z, layer});
                }
            }
        }
    }
    return cells;
}

BakeOptions BakeOptions::fromJson(const Json& json) {
    BakeOptions o;
    if (!json.is_object()) {
        return o;
    }
    o.layer = json.value("layer", o.layer);
    o.samples = json.value("samples", o.samples);
    o.maxSlopeDegrees = json.value("max_slope", o.maxSlopeDegrees);
    o.clearance = json.value("clearance", o.clearance);
    o.step = json.value("step", o.step);
    o.minCoverage = json.value("min_coverage", o.minCoverage);
    o.heightStep = json.value("height_step", o.heightStep);
    o.pruneIslands = json.value("prune_islands", o.pruneIslands);
    o.blockEdges = json.value("block_edges", o.blockEdges);
    o.boxDrops = json.value("box_drops", o.boxDrops);
    return o;
}

Json BakeOptions::toJson() const {
    return {{"layer", layer},
            {"samples", samples},
            {"max_slope", maxSlopeDegrees},
            {"clearance", clearance},
            {"step", step},
            {"min_coverage", minCoverage},
            {"height_step", heightStep},
            {"prune_islands", pruneIslands},
            {"block_edges", blockEdges},
            {"box_drops", boxDrops}};
}

std::string bakeFingerprint(const MapData& map, const std::string& geometryPath,
                            const std::function<std::optional<std::string>(const std::string&)>& readFile) {
    // FNV-1a over every input, each one terminated so fields cannot run together.
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&](const std::string& text) {
        for (const unsigned char c : text) {
            hash = (hash ^ c) * 1099511628211ULL;
        }
        hash = (hash ^ 0xffU) * 1099511628211ULL;
    };
    mix(geometryPath);
    mix(readFile(geometryPath).value_or("<missing>"));
    std::set<std::string> models;
    for (const auto& p : map.placements) {
        if (const auto model = bakedModelOf(p)) {
            const Json canonical = {{"id", p.id},       {"model", *model}, {"position", toJson(p.position)},
                                    {"yaw", p.yaw},     {"scale", toJson(p.scale)}, {"layer", p.layer}};
            mix(canonical.dump());
            models.insert(*model);
        }
    }
    for (const auto& model : models) {
        mix(model);
        mix(readFile(model).value_or("<missing>"));
    }
    mix(map.bake.options.dump());
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = "0123456789abcdef"[hash & 0xf];
        hash >>= 4;
    }
    return out;
}

std::function<std::optional<std::string>(const std::string&)> gameFileReader(const std::filesystem::path& gameRoot) {
    return [gameRoot](const std::string& relative) -> std::optional<std::string> {
        try {
            std::ifstream in(resolveInside(gameRoot, relative), std::ios::binary);
            if (!in) {
                return std::nullopt;
            }
            std::ostringstream out;
            out << in.rdbuf();
            return out.str();
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
}

std::string geometryPathOf(const MapData& map) {
    return "maps/" + map.id + "/" + map.geometry;
}

glm::ivec4 bakeRegionFor(const Model& model, const Placement& placement, const Grid& grid) {
    const auto box = boxForModel(model, placement);
    const int x0 = std::clamp(floorCell(box.min.x) - 1, 0, grid.width() - 1);
    const int z0 = std::clamp(floorCell(box.min.z) - 1, 0, grid.depth() - 1);
    const int x1 = std::clamp(floorCell(box.max.x) + 1, 0, grid.width() - 1);
    const int z1 = std::clamp(floorCell(box.max.z) + 1, 0, grid.depth() - 1);
    return {x0, z0, x1 - x0 + 1, z1 - z0 + 1};
}

glm::ivec4 unionRegion(glm::ivec4 a, glm::ivec4 b) {
    const int x0 = std::min(a.x, b.x), z0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.z, b.x + b.z), z1 = std::max(a.y + a.w, b.y + b.w);
    return {x0, z0, x1 - x0, z1 - z0};
}
} // namespace fe
