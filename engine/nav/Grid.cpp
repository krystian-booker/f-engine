#include "engine/nav/Grid.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace fe {
namespace {
constexpr char RampCodes[] = ".nesw";

template<class Read> void readRows(const Json& rows, int width, int depth, const char* what, Read&& read) {
    if (!rows.is_array() || static_cast<int>(rows.size()) != depth) {
        throw std::runtime_error(std::string("Grid ") + what + " needs one row per depth line");
    }
    for (int z = 0; z < depth; ++z) {
        read(z, rows[static_cast<std::size_t>(z)].get<std::string>());
    }
    (void)width;
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
    return static_cast<int>(layers_.size()) - 1;
}

bool Grid::contains(int x, int z, int layer) const {
    return x >= 0 && z >= 0 && x < width_ && z < depth_ && layer >= 0 && layer < layerCount();
}

GridCell& Grid::at(int x, int z, int layer) {
    if (!contains(x, z, layer)) {
        throw std::out_of_range("Grid cell outside the map");
    }
    return layers_[static_cast<std::size_t>(layer)].cells[index(x, z)];
}

const GridCell& Grid::at(int x, int z, int layer) const {
    if (!contains(x, z, layer)) {
        throw std::out_of_range("Grid cell outside the map");
    }
    return layers_[static_cast<std::size_t>(layer)].cells[index(x, z)];
}

float Grid::heightAt(glm::vec2 p, int layer) const {
    const int x = static_cast<int>(std::floor(p.x));
    const int z = static_cast<int>(std::floor(p.y));
    if (!contains(x, z, layer)) {
        return 0;
    }
    const auto& cell = at(x, z, layer);
    const float fx = p.x - static_cast<float>(x);
    const float fz = p.y - static_cast<float>(z);
    switch (cell.ramp) {
    case Ramp::North:
        return cell.height + 1 - fz;
    case Ramp::East:
        return cell.height + fx;
    case Ramp::South:
        return cell.height + fz;
    case Ramp::West:
        return cell.height + 1 - fx;
    case Ramp::None:
        break;
    }
    return cell.height;
}

float Grid::edgeHeight(GridPos cell, int dx, int dz) const {
    const glm::vec2 centre(static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.z) + 0.5f);
    const glm::vec2 edge = centre + glm::vec2(static_cast<float>(dx), static_cast<float>(dz)) * 0.499f;
    return heightAt(edge, cell.layer);
}

bool Grid::canStand(glm::vec2 p, float radius, int layer) const {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || radius < 0 || radius > 0.49f) {
        return false;
    }
    const int x0 = static_cast<int>(std::floor(p.x - radius));
    const int x1 = static_cast<int>(std::floor(p.x + radius));
    const int z0 = static_cast<int>(std::floor(p.y - radius));
    const int z1 = static_cast<int>(std::floor(p.y + radius));
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            if (contains(x, z, layer) && at(x, z, layer).walkable) {
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

std::optional<int> Grid::linkedLayer(int x, int z, int layer) const {
    for (const auto& link : links_) {
        if (link.x == x && link.z == z) {
            if (link.from == layer) {
                return link.to;
            }
            if (link.to == layer) {
                return link.from;
            }
        }
    }
    return std::nullopt;
}

const GridLink* Grid::linkAt(int x, int z, int layer) const {
    for (const auto& link : links_) {
        if (link.x == x && link.z == z && (link.from == layer || link.to == layer)) {
            return &link;
        }
    }
    return nullptr;
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
    const auto found = cellProps_.find({p.x, p.z, p.layer});
    return found == cellProps_.end() ? nullptr : &found->second;
}

void Grid::setCellProps(GridPos p, Json props) {
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
            readRows(source.at("height"), width, depth, "height", [&](int z, const std::string& row) {
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
        auto charRows = [&](const char* key, auto&& apply) {
            if (!source.contains(key)) {
                return;
            }
            readRows(source.at(key), width, depth, key, [&](int z, const std::string& row) {
                if (static_cast<int>(row.size()) != width) {
                    throw std::runtime_error(std::string("Grid ") + key + " row " + std::to_string(z) +
                                             " needs " + std::to_string(width) + " characters");
                }
                for (int x = 0; x < width; ++x) {
                    apply(cells[grid.index(x, z)], row[static_cast<std::size_t>(x)]);
                }
            });
        };
        charRows("walk", [](GridCell& cell, char c) { cell.walkable = c != '0' && c != '#'; });
        charRows("ramp", [](GridCell& cell, char c) {
            const auto* found = std::strchr(RampCodes, c);
            if (!found || c == '\0') {
                throw std::runtime_error(std::string("Unknown ramp code '") + c + "'");
            }
            cell.ramp = static_cast<Ramp>(found - RampCodes);
        });
        if (source.contains("tags")) {
            readRows(source.at("tags"), width, depth, "tags", [&](int z, const std::string& row) {
                const auto words = splitWords(row);
                if (static_cast<int>(words.size()) != width) {
                    throw std::runtime_error("Grid tag row " + std::to_string(z) + " needs " +
                                             std::to_string(width) + " values");
                }
                for (int x = 0; x < width; ++x) {
                    cells[grid.index(x, z)].tags = static_cast<std::uint32_t>(
                        std::stoul(words[static_cast<std::size_t>(x)], nullptr, 16));
                }
            });
        }
    }
    for (const auto& link : data.value("links", Json::array())) {
        GridLink value{link.at("x"), link.at("z"), link.at("from"), link.at("to"), {0, 0}};
        const auto direction = link.value("dir", std::string{});
        if (!direction.empty()) {
            const auto* code = std::strchr(RampCodes, direction[0]);
            if (!code || direction.size() != 1 || direction[0] == '.') {
                throw std::runtime_error("Grid link dir must be one of n, e, s, w");
            }
            static const glm::ivec2 Directions[] = {{0, 0}, {0, -1}, {1, 0}, {0, 1}, {-1, 0}};
            value.direction = Directions[code - RampCodes];
        }
        if (!grid.contains(value.x, value.z, value.from) || !grid.contains(value.x, value.z, value.to)) {
            throw std::runtime_error("Grid link outside the map");
        }
        grid.links_.push_back(value);
    }
    for (const auto& entry : data.value("cell_props", Json::array())) {
        const GridPos p{entry.at("x"), entry.at("z"), entry.value("layer", 0)};
        if (!grid.contains(p)) {
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
        const bool noTags = std::all_of(layer.cells.begin(), layer.cells.end(),
                                        [](const GridCell& c) { return c.tags == 0; });
        Json heights = Json::array(), walk = Json::array(), ramps = Json::array(), tags = Json::array();
        for (int z = 0; z < depth_; ++z) {
            std::string heightRow, walkRow, rampRow, tagRow;
            for (int x = 0; x < width_; ++x) {
                const auto& cell = layer.cells[index(x, z)];
                heightRow += (x ? " " : "") + formatNumber(cell.height);
                walkRow += cell.walkable ? '1' : '0';
                rampRow += RampCodes[static_cast<int>(cell.ramp)];
                std::ostringstream hex;
                hex << std::hex << cell.tags;
                tagRow += (x ? " " : "") + hex.str();
            }
            heights.push_back(heightRow);
            walk.push_back(walkRow);
            ramps.push_back(rampRow);
            tags.push_back(tagRow);
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
        if (!noTags) {
            out["tags"] = tags;
        }
        layers.push_back(std::move(out));
    }
    Json links = Json::array();
    for (const auto& link : links_) {
        Json out{{"x", link.x}, {"z", link.z}, {"from", link.from}, {"to", link.to}};
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
