#pragma once
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fe {
// One square of baked map geometry: the source vertices it uses (each once,
// in first-use order) and its triangles as indices into that list.
struct MeshChunk {
    glm::ivec2 cell{0};
    std::vector<std::uint32_t> vertices; // indices into the source vertices
    std::vector<std::uint32_t> indices;  // into `vertices`
};

// Splits a triangle list into `size` x `size` squares on the XZ plane by
// triangle centre, ordered by cell x, then z. Each chunk remaps only the
// vertices it uses, so memory grows with each chunk's own vertices, not with
// the whole mesh once per chunk.
inline std::vector<MeshChunk> splitChunks(const std::vector<glm::vec3>& positions,
                                          const std::vector<std::uint32_t>& indices, float size) {
    struct Builder {
        MeshChunk chunk;
        std::unordered_map<std::uint32_t, std::uint32_t> remap;
    };
    std::map<std::pair<int, int>, Builder> chunks;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        glm::vec3 centre(0);
        for (std::size_t k = 0; k < 3; ++k) {
            centre += positions[indices[i + k]] / 3.0f;
        }
        const std::pair cell(static_cast<int>(std::floor(centre.x / size)),
                             static_cast<int>(std::floor(centre.z / size)));
        auto& builder = chunks[cell];
        builder.chunk.cell = {cell.first, cell.second};
        for (std::size_t k = 0; k < 3; ++k) {
            const auto vertex = indices[i + k];
            const auto [found, added] =
                builder.remap.try_emplace(vertex, static_cast<std::uint32_t>(builder.chunk.vertices.size()));
            if (added) {
                builder.chunk.vertices.push_back(vertex);
            }
            builder.chunk.indices.push_back(found->second);
        }
    }
    std::vector<MeshChunk> result;
    result.reserve(chunks.size());
    for (auto& [cell, builder] : chunks) {
        result.push_back(std::move(builder.chunk));
    }
    return result;
}
} // namespace fe
