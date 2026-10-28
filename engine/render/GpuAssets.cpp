#include "engine/render/GpuAssets.h"
#include "engine/core/Log.h"
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace fe {
namespace {
constexpr std::uint64_t PointSampler =
    BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
constexpr float ChunkSize = 8.0f;

const char* backendDirectory() {
    switch (bgfx::getRendererType()) {
    case bgfx::RendererType::Metal:
        return "metal";
    case bgfx::RendererType::Direct3D11:
    case bgfx::RendererType::Direct3D12:
        return "dx11";
    case bgfx::RendererType::Vulkan:
        return "spirv";
    case bgfx::RendererType::OpenGL:
        return "glsl";
    default:
        throw std::runtime_error("Unsupported shader backend");
    }
}

bgfx::ShaderHandle loadShader(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("Cannot open shader " + file.string());
    }
    const auto length = stream.tellg();
    if (length <= 0 || length > 64 * 1024 * 1024) {
        throw std::runtime_error("Invalid shader size: " + file.string());
    }
    stream.seekg(0);
    std::vector<char> bytes(static_cast<std::size_t>(length));
    if (!stream.read(bytes.data(), length)) {
        throw std::runtime_error("Cannot read shader " + file.string());
    }
    auto shader = bgfx::createShader(bgfx::copy(bytes.data(), static_cast<std::uint32_t>(bytes.size())));
    if (!bgfx::isValid(shader)) {
        throw std::runtime_error("Invalid shader " + file.string());
    }
    bgfx::setName(shader, file.filename().string().c_str());
    return shader;
}

std::vector<std::uint8_t> identityLut() {
    constexpr int size = 16;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size * size * size * 4));
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                const auto i = static_cast<std::size_t>(((b * size + g) * size + r) * 4);
                bytes[i] = static_cast<std::uint8_t>(r * 255 / (size - 1));
                bytes[i + 1] = static_cast<std::uint8_t>(g * 255 / (size - 1));
                bytes[i + 2] = static_cast<std::uint8_t>(b * 255 / (size - 1));
                bytes[i + 3] = 255;
            }
        }
    }
    return bytes;
}

std::vector<std::uint8_t> readCube(const std::filesystem::path& file) {
    std::ifstream stream(file);
    if (!stream) {
        throw std::runtime_error("Cannot open LUT " + file.string());
    }
    int inputSize = 0;
    std::vector<glm::vec3> source;
    for (std::string line; std::getline(stream, line);) {
        std::istringstream values(line);
        std::string token;
        values >> token;
        if (token.empty() || token[0] == '#' || token == "TITLE" || token == "DOMAIN_MIN" ||
            token == "DOMAIN_MAX") {
            continue;
        }
        if (token == "LUT_3D_SIZE") {
            values >> inputSize;
            continue;
        }
        glm::vec3 color;
        color.r = std::stof(token);
        if (values >> color.g >> color.b) {
            source.push_back(color);
        }
    }
    if (inputSize < 2 || inputSize > 64 ||
        source.size() != static_cast<std::size_t>(inputSize * inputSize * inputSize)) {
        throw std::runtime_error("Incomplete .cube LUT: " + file.string());
    }
    constexpr int size = 16;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size * size * size * 4));
    auto at = [&](int x, int y, int z) {
        return source[static_cast<std::size_t>((z * inputSize + y) * inputSize + x)];
    };
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                const glm::vec3 coordinate = glm::vec3(r, g, b) / float(size - 1) * float(inputSize - 1);
                const glm::ivec3 lo(coordinate);
                const glm::ivec3 hi = glm::min(lo + 1, glm::ivec3(inputSize - 1));
                const glm::vec3 f = glm::fract(coordinate);
                const auto color =
                    glm::mix(glm::mix(glm::mix(at(lo.x, lo.y, lo.z), at(hi.x, lo.y, lo.z), f.x),
                                      glm::mix(at(lo.x, hi.y, lo.z), at(hi.x, hi.y, lo.z), f.x), f.y),
                             glm::mix(glm::mix(at(lo.x, lo.y, hi.z), at(hi.x, lo.y, hi.z), f.x),
                                      glm::mix(at(lo.x, hi.y, hi.z), at(hi.x, hi.y, hi.z), f.x), f.y),
                             f.z);
                const auto i = static_cast<std::size_t>(((b * size + g) * size + r) * 4);
                for (int c = 0; c < 3; ++c) {
                    bytes[i + static_cast<std::size_t>(c)] =
                        static_cast<std::uint8_t>(glm::clamp(color[c], 0.0f, 1.0f) * 255);
                }
                bytes[i + 3] = 255;
            }
        }
    }
    return bytes;
}
} // namespace

const bgfx::VertexLayout& meshLayout() {
    static const bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
            .end();
        return l;
    }();
    return layout;
}

bgfx::ProgramHandle loadProgram(const std::filesystem::path& root, const char* vertex, const char* fragment) {
    const auto directory = root / backendDirectory();
    auto header = [](const std::filesystem::path& path) {
        std::array<unsigned char, 12> bytes{};
        std::ifstream stream(path, std::ios::binary);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
            throw std::runtime_error("Invalid shader header: " + path.string());
        }
        return bytes;
    };
    const auto vs = header(directory / (std::string(vertex) + ".bin"));
    const auto fs = header(directory / (std::string(fragment) + ".bin"));
    // Shader binaries carry a magic tag and the varying hash the pair must share.
    if (vs[0] != 'V' || fs[0] != 'F' || vs[3] != fs[3] ||
        !std::equal(vs.begin() + 8, vs.begin() + 12, fs.begin() + 4)) {
        throw std::runtime_error(std::string("Shader interfaces do not match: ") + vertex + "/" + fragment);
    }
    auto vsHandle = loadShader(directory / (std::string(vertex) + ".bin"));
    bgfx::ShaderHandle fsHandle = BGFX_INVALID_HANDLE;
    try {
        fsHandle = loadShader(directory / (std::string(fragment) + ".bin"));
    } catch (...) {
        bgfx::destroy(vsHandle);
        throw;
    }
    auto program = bgfx::createProgram(vsHandle, fsHandle, true);
    if (!bgfx::isValid(program)) {
        throw std::runtime_error(std::string("Cannot link GPU program ") + vertex + "/" + fragment);
    }
    return program;
}

GpuAssets::GpuAssets(AssetCache& assets) : assets_(assets) {
    white_ = Texture(upload(Image::solid(255, 255, 255), false, false));
    flatNormal_ = Texture(upload(Image::solid(128, 128, 255), false, false));
}

bgfx::TextureHandle GpuAssets::upload(const Image& image, bool linear, bool srgb) {
    const auto handle =
        bgfx::createTexture2D(static_cast<std::uint16_t>(image.width),
                              static_cast<std::uint16_t>(image.height), false, 1, bgfx::TextureFormat::RGBA8,
                              BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | (linear ? 0 : PointSampler) |
                                  (srgb ? BGFX_TEXTURE_SRGB : 0),
                              bgfx::copy(image.rgba.data(), static_cast<std::uint32_t>(image.rgba.size())));
    if (!bgfx::isValid(handle)) {
        throw std::runtime_error("Could not create a GPU texture");
    }
    return handle;
}

void GpuAssets::sync() {
    if (generation_ == assets_.generation()) {
        return;
    }
    generation_ = assets_.generation();
    images_.clear();
    models_.clear();
    modelTextures_.clear();
    luts_.clear();
    failed_.clear();
    // Font pages are versioned individually; drop pages of fonts that went away.
    fontPages_.clear();
}

bgfx::TextureHandle GpuAssets::image(const std::string& path, bool linear) {
    const auto key = std::make_pair(path, linear);
    if (const auto found = images_.find(key); found != images_.end()) {
        return found->second.get();
    }
    const auto image = assets_.tryImage(path);
    if (!image) {
        return white_.get();
    }
    return images_.emplace(key, Texture(upload(*image, linear, true))).first->second.get();
}

glm::ivec2 GpuAssets::imageSize(const std::string& path) {
    const auto image = assets_.tryImage(path);
    return image ? glm::ivec2(image->width, image->height) : glm::ivec2(0);
}

bgfx::TextureHandle GpuAssets::fontPage(const Font* font, int page) {
    const auto key = std::make_pair(font, page);
    const auto version = font->pageVersion(static_cast<std::size_t>(page));
    auto found = fontPages_.find(key);
    if (found != fontPages_.end() && found->second.first == version) {
        return found->second.second.get();
    }
    const auto& image = font->page(static_cast<std::size_t>(page));
    auto& entry = fontPages_[key];
    entry.first = version;
    entry.second = Texture(upload(image, !font->pixel(), false));
    return entry.second.get();
}

bgfx::TextureHandle GpuAssets::lut(const std::string& path) {
    if (const auto found = luts_.find(path); found != luts_.end()) {
        return found->second.get();
    }
    std::vector<std::uint8_t> bytes;
    try {
        bytes = path.empty() ? identityLut() : readCube(assets_.file(path));
    } catch (const std::exception& error) {
        if (failed_.insert(path).second) {
            log(LogLevel::Error, error.what());
        }
        bytes = identityLut();
    }
    auto handle = bgfx::createTexture3D(16, 16, 16, false, bgfx::TextureFormat::RGBA8,
                                        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
                                        bgfx::copy(bytes.data(), static_cast<std::uint32_t>(bytes.size())));
    return luts_.emplace(path, Texture(handle)).first->second.get();
}

const GpuModel* GpuAssets::model(const std::string& path, bool bakeChunks) {
    const auto key = std::make_pair(path, bakeChunks);
    if (const auto found = models_.find(key); found != models_.end()) {
        return found->second.get();
    }
    auto source = assets_.tryModel(path);
    if (!source) {
        return nullptr;
    }
    auto gpu = std::make_unique<GpuModel>();
    gpu->source = source;
    gpu->baked = bakeChunks;
    std::vector<glm::mat4> rest;
    source->pose(-1, 0, rest);
    auto texture = [&](const std::shared_ptr<Image>& image, bool srgb) {
        if (!image) {
            return srgb ? white_.get() : flatNormal_.get();
        }
        auto& slot = modelTextures_[image.get()];
        if (!slot.valid()) {
            slot = Texture(upload(*image, false, srgb));
        }
        return slot.get();
    };
    auto emit = [&](const ModelPart& part, std::vector<MeshVertex> vertices,
                    std::vector<std::uint32_t> indices) {
        if (indices.empty()) {
            return;
        }
        GpuPart out;
        out.min = glm::vec3(std::numeric_limits<float>::max());
        out.max = -out.min;
        for (const auto& v : vertices) {
            out.min = glm::min(out.min, v.position);
            out.max = glm::max(out.max, v.position);
        }
        out.vertices = VertexBuffer(bgfx::createVertexBuffer(
            bgfx::copy(vertices.data(), static_cast<std::uint32_t>(vertices.size() * sizeof(MeshVertex))),
            meshLayout()));
        out.indices = IndexBuffer(bgfx::createIndexBuffer(
            bgfx::copy(indices.data(), static_cast<std::uint32_t>(indices.size() * sizeof(std::uint32_t))),
            BGFX_BUFFER_INDEX32));
        out.albedo = texture(part.material.albedo, true);
        out.normal = texture(part.material.normal, false);
        out.material = part.material;
        out.node = part.node;
        gpu->parts.push_back(std::move(out));
    };
    for (const auto& part : source->parts) {
        if (part.hidden) {
            continue;
        }
        if (!bakeChunks) {
            emit(part, part.vertices, part.indices);
            continue;
        }
        // Bake to world space and bucket triangles into chunks by centroid.
        const auto& matrix = rest[static_cast<std::size_t>(part.node)];
        const auto normalMatrix = glm::transpose(glm::inverse(glm::mat3(matrix)));
        std::map<std::pair<int, int>, std::pair<std::vector<MeshVertex>, std::vector<std::uint32_t>>> chunks;
        for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
            MeshVertex tri[3];
            glm::vec3 centre(0);
            for (int k = 0; k < 3; ++k) {
                tri[k] = part.vertices[part.indices[i + static_cast<std::size_t>(k)]];
                tri[k].position = glm::vec3(matrix * glm::vec4(tri[k].position, 1));
                tri[k].normal = glm::normalize(normalMatrix * tri[k].normal);
                centre += tri[k].position / 3.0f;
            }
            auto& chunk = chunks[{static_cast<int>(std::floor(centre.x / ChunkSize)),
                                  static_cast<int>(std::floor(centre.z / ChunkSize))}];
            for (const auto& v : tri) {
                chunk.second.push_back(static_cast<std::uint32_t>(chunk.first.size()));
                chunk.first.push_back(v);
            }
        }
        for (auto& [cell, chunk] : chunks) {
            (void)cell;
            emit(part, std::move(chunk.first), std::move(chunk.second));
        }
    }
    return models_.emplace(key, std::move(gpu)).first->second.get();
}
} // namespace fe
