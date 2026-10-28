#include "engine/core/Paths.h"
#include <stdexcept>
#include <string>

namespace fe {
std::filesystem::path resolveInside(const std::filesystem::path& root, std::string_view relative) {
    if (relative.empty()) {
        throw std::runtime_error("Empty path");
    }
    const auto base = std::filesystem::weakly_canonical(root);
    const auto candidate = std::filesystem::weakly_canonical(base / std::filesystem::path(relative));
    const auto inside = candidate.lexically_relative(base);
    if (inside.empty() || inside.is_absolute() || *inside.begin() == "..") {
        throw std::runtime_error("Path escapes the game folder: " + std::string(relative));
    }
    return candidate;
}

std::string relativeTo(const std::filesystem::path& root, const std::filesystem::path& file) {
    const auto inside =
        std::filesystem::weakly_canonical(file).lexically_relative(std::filesystem::weakly_canonical(root));
    if (inside.empty() || *inside.begin() == "..") {
        return {};
    }
    return inside.generic_string();
}
} // namespace fe
