#include "engine/editor/WalkBake.h"
#include "engine/assets/Model.h"
#include "engine/editor/MapEdits.h"
#include <algorithm>
#include <cmath>
#include <limits>

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
} // namespace

MeshRaycaster::MeshRaycaster(const Model& model) {
    min_ = glm::vec3(std::numeric_limits<float>::max());
    max_ = -min_;
    const bool posed = model.restPose.size() == model.nodes.size();
    for (const auto& part : model.parts) {
        const glm::mat4 m = posed && part.node >= 0 && part.node < static_cast<int>(model.restPose.size())
                                ? model.restPose[static_cast<std::size_t>(part.node)]
                                : glm::mat4(1);
        const bool water = part.material.kind == MaterialKind::Water;
        for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
            Triangle t;
            t.a = glm::vec3(m * glm::vec4(part.vertices[part.indices[i]].position, 1));
            t.b = glm::vec3(m * glm::vec4(part.vertices[part.indices[i + 1]].position, 1));
            t.c = glm::vec3(m * glm::vec4(part.vertices[part.indices[i + 2]].position, 1));
            const auto n = glm::cross(t.b - t.a, t.c - t.a);
            if (glm::length(n) < 1e-10f) {
                continue;
            }
            t.normal = glm::normalize(n);
            t.water = water;
            for (const auto& v : {t.a, t.b, t.c}) {
                min_ = glm::min(min_, v);
                max_ = glm::max(max_, v);
            }
            triangles_.push_back(t);
        }
    }
    if (triangles_.empty()) {
        min_ = max_ = glm::vec3(0);
        return;
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
// connect: roofs and ledges nothing leads to.
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
        if (cell.walkable && component[static_cast<std::size_t>(index)] != main) {
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
} // namespace

BakeResult bakeWalkability(const Model& model, const Grid& grid, const BakeOptions& options) {
    const MeshRaycaster mesh(model);
    BakeResult result;
    result.width = grid.width();
    result.depth = grid.depth();
    result.cells.resize(static_cast<std::size_t>(result.width) * static_cast<std::size_t>(result.depth));
    const int n = std::clamp(options.samples, 1, 16);
    const float slopeCos = std::cos(glm::radians(std::clamp(options.maxSlopeDegrees, 0.0f, 89.0f)));
    const float heightStep = std::max(options.heightStep, 0.01f);
    const auto rounded = [&](float value) { return std::round(value / heightStep) * heightStep; };
    std::vector<std::pair<int, Box>> quarters; // cell index, box
    struct Sample {
        bool hit = false, ground = false, water = false;
        float y = 0;
    };
    std::vector<Sample> samples(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
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
                    const float px = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / static_cast<float>(n);
                    const float pz = static_cast<float>(z) + (static_cast<float>(sz) + 0.5f) / static_cast<float>(n);
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
            int ground = 0, water = 0;
            for (const auto& s : samples) {
                ground += s.ground ? 1 : 0;
                water += s.hit && s.water ? 1 : 0;
            }
            const float total = static_cast<float>(n * n);
            cell.coverage = static_cast<float>(ground) / total;
            cell.water = water > ground && static_cast<float>(water) / total >= 0.5f;
            cell.height = rounded(floor);
            // 3. The verdict.
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
            } else if (cell.coverage >= options.coverage) {
                cell.walkable = true;
                cell.confidence = (cell.coverage - options.coverage) / std::max(1 - options.coverage, 0.01f);
                ++result.walkable;
            } else if (cell.coverage <= options.partialCoverage) {
                cell.walkable = false;
                cell.confidence = (options.partialCoverage - cell.coverage) / std::max(options.partialCoverage, 0.01f);
                ++result.blocked;
            } else {
                cell.walkable = true;
                cell.partial = true;
                cell.confidence = 0;
                ++result.partial;
                // The covered quarters become boxes bodies cannot enter.
                const float quarter = 1.0f / static_cast<float>(n);
                for (int sz = 0; sz < n; ++sz) {
                    for (int sx = 0; sx < n; ++sx) {
                        const auto& s = samples[static_cast<std::size_t>(sz * n + sx)];
                        if (s.ground) {
                            continue;
                        }
                        const float qx = static_cast<float>(x) + static_cast<float>(sx) * quarter;
                        const float qz = static_cast<float>(z) + static_cast<float>(sz) * quarter;
                        quarters.push_back({z * result.width + x,
                                            {{qx, cell.height - 0.05f, qz}, {qx + quarter, cell.height + 1.2f, qz + quarter}}});
                    }
                }
            }
            // 4. Ramps: heights that climb across the cell along one axis.
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
                if (x != 0 && z != 0 && x != result.width - 1 && z != result.depth - 1) {
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
        if (result.cells[static_cast<std::size_t>(index)].partial) {
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
    const auto waterBit = options.writeWater ? grid.tagBit("water", true) : 0u;
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
            if (options.writeWalkable && !(options.keepBlocked && !before.walkable)) {
                cell.walkable = baked.walkable;
            }
            if (options.writeHeights) {
                cell.height = baked.height;
            }
            if (options.writeRamps) {
                cell.ramp = baked.walkable ? baked.ramp : Ramp::None;
                cell.rise = baked.ramp == Ramp::None ? 1 : baked.rise;
            }
            if (waterBit) {
                cell.tags = baked.water ? (cell.tags | waterBit) : (cell.tags & ~waterBit);
            }
            if (!(cell == before)) {
                grid.setCell(p, cell);
                ++changed;
            }
        }
    }
    if (options.writeColliders && !result.colliders.empty()) {
        map.colliders.insert(map.colliders.end(), result.colliders.begin(), result.colliders.end());
        map.colliders = mergeBoxes(std::move(map.colliders));
    }
    return changed;
}
} // namespace fe
