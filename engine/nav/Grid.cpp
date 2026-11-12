#include "engine/nav/Grid.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace fe {
namespace {
constexpr char RampCodes[] = ".nesw";
constexpr const char* RampNames[] = {"none", "north", "east", "south", "west"};
constexpr glm::ivec2 RampSteps[] = {{0, 0}, {0, -1}, {1, 0}, {0, 1}, {-1, 0}};

template<class Read> void readRows(const Json& rows, int depth, const char* what, Read&& read) {
    if (!rows.is_array() || static_cast<int>(rows.size()) != depth) {
        throw std::runtime_error(std::string("Grid ") + what + " needs one row per depth line");
    }
    for (int z = 0; z < depth; ++z) {
        read(z, rows[static_cast<std::size_t>(z)].get<std::string>());
    }
}

std::vector<std::string> splitWords(const std::string& row) {
    std::istringstream stream(row);
    std::vector<std::string> words;
    for (std::string word; stream >> word;) {
        words.push_back(word);
    }
    return words;
}

std::string formatNumber(float value) {
    std::ostringstream stream;
    stream << std::round(value * 1000.0f) / 1000.0f;
    return stream.str();
}
} // namespace

const char* rampName(Ramp ramp) {
    return RampNames[static_cast<int>(ramp)];
}

std::optional<Ramp> rampFromName(std::string_view name) {
    for (int i = 0; i < 5; ++i) {
        if (name == RampNames[i]) {
            return static_cast<Ramp>(i);
        }
    }
    return std::nullopt;
}

char rampCode(Ramp ramp) {
    return RampCodes[static_cast<int>(ramp)];
}

std::optional<Ramp> rampFromCode(char code) {
    for (int i = 0; i < 5; ++i) {
        if (code == RampCodes[i]) {
            return static_cast<Ramp>(i);
        }
    }
    return std::nullopt;
}

glm::ivec2 rampDirection(Ramp ramp) {
    return RampSteps[static_cast<int>(ramp)];
}

Ramp rampFromDirection(glm::ivec2 step) {
    for (int i = 1; i < RampCount; ++i) {
        if (step == RampSteps[i]) {
            return static_cast<Ramp>(i);
        }
    }
    return Ramp::None;
}

Grid::Grid(int width, int depth, int layers) : width_(width), depth_(depth) {
    if (width < 1 || depth < 1 || width > 4096 || depth > 4096 || layers < 1 || layers > 16) {
        throw std::runtime_error("Grid dimensions must be 1..4096 with 1..16 layers");
    }
    for (int i = 0; i < layers; ++i) {
        addLayer(i == 0 ? "ground" : "layer" + std::to_string(i));
    }
}

int Grid::addLayer(const std::string& name) {
    if (layers_.size() >= 16) {
        throw std::runtime_error("A grid supports at most 16 layers");
    }
    Layer layer{name,
                std::vector<GridCell>(static_cast<std::size_t>(width_) * static_cast<std::size_t>(depth_))};
    if (!layers_.empty()) {
        for (auto& cell : layer.cells) {
            cell.walkable = false; // upper layers start empty
        }
    }
    layers_.push_back(std::move(layer));
    ++revision_;
    ++topology_;
    return static_cast<int>(layers_.size()) - 1;
}

bool Grid::contains(int x, int z, int layer) const {
    if (wrap_) {
        return layer >= 0 && layer < layerCount();
    }
    return inRange(x, z, layer);
}

void Grid::setWrap(bool wrap) {
    if (wrap_ == wrap) {
        return;
    }
    wrap_ = wrap;
    ++revision_; // the seam joins or splits components
    ++topology_;
}

GridCell& Grid::at(int x, int z, int layer) {
    if (!contains(x, z, layer)) {
        throw std::out_of_range("Grid cell outside the map");
    }
    ++revision_; // the caller may write through the reference
    ++topology_;
    return layers_[static_cast<std::size_t>(layer)].cells[index(x, z)];
}

void Grid::setCell(GridPos p, const GridCell& cell) {
    if (!contains(p)) {
        throw std::out_of_range("Grid cell outside the map");
    }
    auto& current = layers_[static_cast<std::size_t>(p.layer)].cells[index(p.x, p.z)];
    if (current == cell) {
        return;
    }
    // Connectivity (component()) follows walkability alone: a cell whose
    // height or tags change, a lift's deck say, relabels nothing.
    if (current.walkable != cell.walkable) {
        ++topology_;
    }
    current = cell;
    ++revision_;
}

int Grid::component(GridPos p) const {
    if (!contains(p) || !at(p).walkable) {
        return -1;
    }
    if (!componentsBuilt_ || componentsRevision_ != topology_) {
        buildComponents();
    }
    const auto perLayer = static_cast<std::size_t>(width_) * static_cast<std::size_t>(depth_);
    return components_[static_cast<std::size_t>(p.layer) * perLayer + index(p.x, p.z)];
}

// Union-find over every cell of every layer: 4-neighbours on a layer, then
// the two ends of every link. Labels are the root slots, so they are stable
// for one build and comparable only with each other.
void Grid::buildComponents() const {
    const auto perLayer = static_cast<std::size_t>(width_) * static_cast<std::size_t>(depth_);
    const auto total = perLayer * layers_.size();
    std::vector<std::int32_t> parent(total);
    std::iota(parent.begin(), parent.end(), 0);
    const auto find = [&](std::int32_t i) {
        while (parent[static_cast<std::size_t>(i)] != i) {
            auto& up = parent[static_cast<std::size_t>(i)];
            up = parent[static_cast<std::size_t>(up)];
            i = up;
        }
        return i;
    };
    const auto slot = [&](GridPos p) {
        return static_cast<std::int32_t>(static_cast<std::size_t>(p.layer) * perLayer + index(p.x, p.z));
    };
    const auto walkable = [&](GridPos p) {
        return contains(p) && layers_[static_cast<std::size_t>(p.layer)].cells[index(p.x, p.z)].walkable;
    };
    const auto join = [&](GridPos a, GridPos b) {
        if (!walkable(a) || !walkable(b)) {
            return;
        }
        const auto ra = find(slot(a)), rb = find(slot(b));
        if (ra != rb) {
            parent[static_cast<std::size_t>(std::max(ra, rb))] = std::min(ra, rb);
        }
    };
    for (int l = 0; l < layerCount(); ++l) {
        for (int z = 0; z < depth_; ++z) {
            for (int x = 0; x < width_; ++x) {
                join({x, z, l}, {x + 1, z, l});
                join({x, z, l}, {x, z + 1, l});
            }
        }
    }
    for (const auto& link : links_) {
        if (link.kind == LinkKind::Ladder) {
            join(link.foot(), link.top());
        } else {
            join({link.x, link.z, link.from}, {link.x, link.z, link.to});
        }
    }
    components_.assign(total, -1);
    for (std::size_t i = 0; i < total; ++i) {
        if (layers_[i / perLayer].cells[i % perLayer].walkable) {
            components_[i] = find(static_cast<std::int32_t>(i));
        }
    }
    componentsBuilt_ = true;
    componentsRevision_ = topology_;
}

const GridCell& Grid::at(int x, int z, int layer) const {
    if (!contains(x, z, layer)) {
        throw std::out_of_range("Grid cell outside the map");
    }
    return layers_[static_cast<std::size_t>(layer)].cells[index(x, z)];
}

float Grid::heightAt(glm::vec2 p, int layer) const {
    const int x = floorCell(p.x);
    const int z = floorCell(p.y);
    if (!contains(x, z, layer)) {
        return 0;
    }
    return heightIn({x, z, layer}, p);
}

float Grid::heightIn(GridPos c, glm::vec2 p) const {
    const auto& cell = at(c);
    const float fx = std::clamp(p.x - static_cast<float>(c.x), 0.0f, 1.0f);
    const float fz = std::clamp(p.y - static_cast<float>(c.z), 0.0f, 1.0f);
    switch (cell.ramp) {
    case Ramp::North:
        return cell.height + cell.rise * (1 - fz);
    case Ramp::East:
        return cell.height + cell.rise * fx;
    case Ramp::South:
        return cell.height + cell.rise * fz;
    case Ramp::West:
        return cell.height + cell.rise * (1 - fx);
    case Ramp::None:
        break;
    }
    return cell.height;
}

float Grid::edgeHeight(GridPos cell, int dx, int dz) const {
    const glm::vec2 centre(static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.z) + 0.5f);
    return heightIn(cell, centre + glm::vec2(static_cast<float>(dx), static_cast<float>(dz)) * 0.5f);
}

bool Grid::canStand(glm::vec2 p, float radius, int layer, int footprint, bool fly, std::uint32_t blockedTags,
                    std::uint32_t requiredTags) const {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || radius < 0 || radius > 0.49f) {
        return false;
    }
    const auto open = [&](int x, int z) {
        if (!contains(x, z, layer)) {
            return false;
        }
        const auto& cell = at(x, z, layer);
        return (fly || cell.walkable) && !(cell.tags & blockedTags) &&
               (!requiredTags || (cell.tags & requiredTags));
    };
    const int anchorX = floorCell(p.x);
    const int anchorZ = floorCell(p.y);
    for (int dz = 0; dz < footprint; ++dz) {
        for (int dx = 0; dx < footprint; ++dx) {
            if (!open(anchorX + dx, anchorZ + dz)) {
                return false;
            }
        }
    }
    const int x0 = floorCell(p.x - radius);
    const int x1 = floorCell(p.x + radius);
    const int z0 = floorCell(p.y - radius);
    const int z1 = floorCell(p.y + radius);
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            if (open(x, z)) {
                continue;
            }
            const auto nearest = glm::clamp(p, glm::vec2(x, z), glm::vec2(x + 1, z + 1));
            const auto offset = nearest - p;
            if (glm::dot(offset, offset) <= radius * radius + 1e-7f) {
                return false;
            }
        }
    }
    return true;
}

bool Grid::walkableAt(glm::vec2 p, int layer) const {
    const int x = floorCell(p.x);
    const int z = floorCell(p.y);
    return contains(x, z, layer) && at(x, z, layer).walkable;
}

void Grid::setLinks(std::vector<GridLink> links) {
    links_ = std::move(links);
    indexLinks();
    ++revision_;
    ++topology_;
}

void Grid::indexLinks() {
    linkIndex_.clear();
    ladderIndex_.clear();
    for (std::size_t i = 0; i < links_.size(); ++i) {
        const auto& link = links_[i];
        if (link.kind == LinkKind::Ladder) {
            for (const auto end : {link.foot(), link.top()}) {
                if (contains(end.x, end.z)) {
                    ladderIndex_.emplace_back(index(end.x, end.z), i);
                }
            }
        } else if (contains(link.x, link.z)) {
            linkIndex_.emplace_back(index(link.x, link.z), i);
        }
    }
    std::sort(linkIndex_.begin(), linkIndex_.end());
    std::sort(ladderIndex_.begin(), ladderIndex_.end());
}

const GridLink* Grid::ladderStep(GridPos from, glm::ivec2 step) const {
    if (ladderIndex_.empty() || !contains(from.x, from.z)) {
        return nullptr;
    }
    const auto cell = index(from.x, from.z);
    const auto here = wrapCell(from);
    for (auto it =
             std::lower_bound(ladderIndex_.begin(), ladderIndex_.end(), std::make_pair(cell, std::size_t{0}));
         it != ladderIndex_.end() && it->first == cell; ++it) {
        const auto& link = links_[it->second];
        if ((here == wrapCell(link.foot()) && step == link.direction) ||
            (here == wrapCell(link.top()) && step == -link.direction)) {
            return &link;
        }
    }
    return nullptr;
}

const GridLink* Grid::linkAt(int x, int z, int layer) const {
    const GridLink* found = nullptr;
    anyLinkAt(x, z, layer, [&](const GridLink& link) {
        found = &link;
        return true;
    });
    return found;
}

int Grid::traverseLink(glm::vec2 p, int layer, glm::vec2 heading, float maxUp, float maxDown,
                       bool layerEnds) const {
    const int x = floorCell(p.x);
    const int z = floorCell(p.y);
    if (!contains(x, z, layer)) {
        return layer;
    }
    const auto cell = index(x, z);
    for (auto it =
             std::lower_bound(linkIndex_.begin(), linkIndex_.end(), std::make_pair(cell, std::size_t{0}));
         it != linkIndex_.end() && it->first == cell; ++it) {
        const auto& link = links_[it->second];
        if (link.from != layer && link.to != layer) {
            continue;
        }
        const int other = link.other(layer);
        bool wanted = layerEnds;
        if (link.direction != glm::ivec2(0)) {
            const float along = glm::dot(heading, glm::vec2(link.direction));
            wanted = other == link.to ? along > 1e-5f : along < -1e-5f;
        }
        if (!wanted || !contains(x, z, other) || !at(x, z, other).walkable) {
            continue;
        }
        const float rise = heightIn({x, z, other}, p) - heightIn({x, z, layer}, p);
        if (rise <= std::min(LinkGap, maxUp) + 1e-4f && -rise <= std::min(LinkGap, maxDown) + 1e-4f) {
            return other;
        }
    }
    return layer;
}

std::uint32_t Grid::tagBit(const std::string& name, bool create) {
    const auto found = std::find(tagNames_.begin(), tagNames_.end(), name);
    if (found != tagNames_.end()) {
        return 1u << static_cast<unsigned>(found - tagNames_.begin());
    }
    if (!create) {
        return 0;
    }
    if (tagNames_.size() >= 32) {
        throw std::runtime_error("A grid supports at most 32 distinct tags");
    }
    tagNames_.push_back(name);
    return 1u << static_cast<unsigned>(tagNames_.size() - 1);
}

std::uint32_t Grid::tagMask(const std::vector<std::string>& names) const {
    std::uint32_t mask = 0;
    for (const auto& name : names) {
        const auto found = std::find(tagNames_.begin(), tagNames_.end(), name);
        if (found != tagNames_.end()) {
            mask |= 1u << static_cast<unsigned>(found - tagNames_.begin());
        }
    }
    return mask;
}

std::vector<std::string> Grid::tagsOf(std::uint32_t mask) const {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < tagNames_.size(); ++i) {
        if (mask & (1u << i)) {
            names.push_back(tagNames_[i]);
        }
    }
    return names;
}

const Json* Grid::cellProps(GridPos p) const {
    p = wrapCell(p);
    const auto found = cellProps_.find({p.x, p.z, p.layer});
    return found == cellProps_.end() ? nullptr : &found->second;
}

void Grid::setCellProps(GridPos p, Json props) {
    p = wrapCell(p);
    if (props.is_null() || props.empty()) {
        cellProps_.erase({p.x, p.z, p.layer});
    } else {
        cellProps_[{p.x, p.z, p.layer}] = std::move(props);
    }
}

Grid Grid::fromJson(const Json& data) {
    const int width = data.at("width").get<int>();
    const int depth = data.at("depth").get<int>();
    const auto& layers = data.at("layers");
    if (!layers.is_array() || layers.empty()) {
        throw std::runtime_error("Grid needs at least one layer");
    }
    Grid grid(width, depth, 1);
    grid.wrap_ = data.value("wrap", false);
    grid.tagNames_ = data.value("tags", std::vector<std::string>{});
    if (grid.tagNames_.size() > 32) {
        throw std::runtime_error("A grid supports at most 32 distinct tags");
    }
    for (std::size_t l = 0; l < layers.size(); ++l) {
        const auto& source = layers[l];
        if (l > 0) {
            grid.addLayer(source.value("name", "layer" + std::to_string(l)));
        } else {
            grid.layers_[0].name = source.value("name", "ground");
        }
        auto& cells = grid.layers_[l].cells;
        const float heightFill = source.value("height_fill", 0.0f);
        const bool walkFill = source.value("walk_fill", l == 0);
        for (auto& cell : cells) {
            cell.height = heightFill;
            cell.walkable = walkFill;
        }
        if (source.contains("height")) {
            readRows(source.at("height"), depth, "height", [&](int z, const std::string& row) {
                const auto words = splitWords(row);
                if (static_cast<int>(words.size()) != width) {
                    throw std::runtime_error("Grid height row " + std::to_string(z) + " needs " +
                                             std::to_string(width) + " values");
                }
                for (int x = 0; x < width; ++x) {
                    const float value = std::stof(words[static_cast<std::size_t>(x)]);
                    if (!std::isfinite(value)) {
                        throw std::runtime_error("Non-finite grid height");
                    }
                    cells[grid.index(x, z)].height = value;
                }
            });
        }
        if (source.contains("rise")) {
            readRows(source.at("rise"), depth, "rise", [&](int z, const std::string& row) {
                const auto words = splitWords(row);
                if (static_cast<int>(words.size()) != width) {
                    throw std::runtime_error("Grid rise row " + std::to_string(z) + " needs " +
                                             std::to_string(width) + " values");
                }
                for (int x = 0; x < width; ++x) {
                    const float value = std::stof(words[static_cast<std::size_t>(x)]);
                    if (!std::isfinite(value) || value < 0) {
                        throw std::runtime_error("Grid ramp rise must be a finite, non-negative number");
                    }
                    cells[grid.index(x, z)].rise = value;
                }
            });
        }
        auto charRows = [&](const char* key, auto&& apply) {
            if (!source.contains(key)) {
                return;
            }
            readRows(source.at(key), depth, key, [&](int z, const std::string& row) {
                if (static_cast<int>(row.size()) != width) {
                    throw std::runtime_error(std::string("Grid ") + key + " row " + std::to_string(z) +
                                             " needs " + std::to_string(width) + " characters");
                }
                for (int x = 0; x < width; ++x) {
                    apply(cells[grid.index(x, z)], row[static_cast<std::size_t>(x)]);
                }
            });
        };
        charRows("walk", [](GridCell& cell, char c) {
            if (c != '0' && c != '1' && c != '#') {
                throw std::runtime_error(std::string("Grid walk rows use 1, 0 or #, got '") + c + "'");
            }
            cell.walkable = c == '1';
        });
        charRows("ramp", [](GridCell& cell, char c) {
            const auto ramp = rampFromCode(c);
            if (!ramp) {
                throw std::runtime_error(std::string("Unknown ramp code '") + c + "'");
            }
            cell.ramp = *ramp;
        });
        if (source.contains("tags")) {
            readRows(source.at("tags"), depth, "tags", [&](int z, const std::string& row) {
                const auto words = splitWords(row);
                if (static_cast<int>(words.size()) != width) {
                    throw std::runtime_error("Grid tag row " + std::to_string(z) + " needs " +
                                             std::to_string(width) + " values");
                }
                for (int x = 0; x < width; ++x) {
                    const auto mask = std::stoul(words[static_cast<std::size_t>(x)], nullptr, 16);
                    if (mask > 0xffffffffUL ||
                        (grid.tagNames_.size() < 32 && (mask >> grid.tagNames_.size()) != 0)) {
                        throw std::runtime_error("Grid tag row " + std::to_string(z) + " uses tag bits the " +
                                                 std::to_string(grid.tagNames_.size()) +
                                                 "-entry tag list does not have");
                    }
                    cells[grid.index(x, z)].tags = static_cast<std::uint32_t>(mask);
                }
            });
        }
    }
    for (const auto& link : data.value("links", Json::array())) {
        GridLink value{link.at("x"), link.at("z"), link.at("from"), link.at("to"), {0, 0}};
        const auto kind = link.value("kind", std::string("step"));
        if (kind != "step" && kind != "ladder") {
            throw std::runtime_error("Grid link kind must be step or ladder");
        }
        value.kind = kind == "ladder" ? LinkKind::Ladder : LinkKind::Step;
        const auto direction = link.value("dir", std::string{});
        if (!direction.empty()) {
            const auto code = rampFromCode(direction[0]);
            if (!code || direction.size() != 1 || *code == Ramp::None) {
                throw std::runtime_error("Grid link dir must be one of n, e, s, w");
            }
            value.direction = rampDirection(*code);
        }
        if (value.kind == LinkKind::Ladder) {
            if (value.direction == glm::ivec2(0)) {
                throw std::runtime_error("A ladder link needs a dir (the way up)");
            }
            // Both ends inside the rectangle: a ladder may not straddle the
            // seam of a wrapping grid.
            if (!grid.inRange(value.foot()) || !grid.inRange(value.top())) {
                throw std::runtime_error("Grid ladder outside the map");
            }
        } else {
            if (!grid.inRange(value.x, value.z, value.from) || !grid.inRange(value.x, value.z, value.to)) {
                throw std::runtime_error("Grid link outside the map");
            }
            if (value.from == value.to) {
                throw std::runtime_error("Grid link must join two different layers");
            }
        }
        grid.links_.push_back(value);
    }
    grid.indexLinks();
    for (const auto& entry : data.value("cell_props", Json::array())) {
        const GridPos p{entry.at("x"), entry.at("z"), entry.value("layer", 0)};
        if (!grid.inRange(p)) {
            throw std::runtime_error("Grid cell_props entry outside the map");
        }
        grid.setCellProps(p, entry.at("props"));
    }
    return grid;
}

Json Grid::toJson() const {
    Json layers = Json::array();
    for (const auto& layer : layers_) {
        Json out{{"name", layer.name}};
        const auto& first = layer.cells.front();
        const bool flat = std::all_of(layer.cells.begin(), layer.cells.end(),
                                      [&](const GridCell& c) { return c.height == first.height; });
        const bool uniformWalk = std::all_of(layer.cells.begin(), layer.cells.end(),
                                             [&](const GridCell& c) { return c.walkable == first.walkable; });
        const bool noRamps = std::all_of(layer.cells.begin(), layer.cells.end(),
                                         [](const GridCell& c) { return c.ramp == Ramp::None; });
        const bool unitRise = std::all_of(layer.cells.begin(), layer.cells.end(),
                                          [](const GridCell& c) { return c.rise == 1; });
        const bool noTags = std::all_of(layer.cells.begin(), layer.cells.end(),
                                        [](const GridCell& c) { return c.tags == 0; });
        Json heights = Json::array(), walk = Json::array(), ramps = Json::array(), tags = Json::array(),
             rises = Json::array();
        for (int z = 0; z < depth_; ++z) {
            std::string heightRow, walkRow, rampRow, tagRow, riseRow;
            for (int x = 0; x < width_; ++x) {
                const auto& cell = layer.cells[index(x, z)];
                heightRow += (x ? " " : "") + formatNumber(cell.height);
                riseRow += (x ? " " : "") + formatNumber(cell.rise);
                walkRow += cell.walkable ? '1' : '0';
                rampRow += rampCode(cell.ramp);
                std::ostringstream hex;
                hex << std::hex << cell.tags;
                tagRow += (x ? " " : "") + hex.str();
            }
            heights.push_back(heightRow);
            walk.push_back(walkRow);
            ramps.push_back(rampRow);
            tags.push_back(tagRow);
            rises.push_back(riseRow);
        }
        if (flat) {
            out["height_fill"] = first.height;
        } else {
            out["height"] = heights;
        }
        if (uniformWalk) {
            out["walk_fill"] = first.walkable;
        } else {
            out["walk"] = walk;
        }
        if (!noRamps) {
            out["ramp"] = ramps;
        }
        if (!unitRise) {
            out["rise"] = rises;
        }
        if (!noTags) {
            out["tags"] = tags;
        }
        layers.push_back(std::move(out));
    }
    Json links = Json::array();
    for (const auto& link : links_) {
        Json out{{"x", link.x}, {"z", link.z}, {"from", link.from}, {"to", link.to}};
        if (link.kind == LinkKind::Ladder) {
            out["kind"] = "ladder";
        }
        if (link.direction != glm::ivec2(0)) {
            out["dir"] = link.direction.y < 0   ? "n"
                         : link.direction.x > 0 ? "e"
                         : link.direction.y > 0 ? "s"
                                                : "w";
        }
        links.push_back(out);
    }
    Json props = Json::array();
    for (const auto& [key, value] : cellProps_) {
        props.push_back({{"x", std::get<0>(key)},
                         {"z", std::get<1>(key)},
                         {"layer", std::get<2>(key)},
                         {"props", value}});
    }
    Json result{{"width", width_}, {"depth", depth_}, {"layers", layers}};
    if (wrap_) {
        result["wrap"] = true;
    }
    if (!tagNames_.empty()) {
        result["tags"] = tagNames_;
    }
    if (!links.empty()) {
        result["links"] = links;
    }
    if (!props.empty()) {
        result["cell_props"] = props;
    }
    return result;
}
} // namespace fe
