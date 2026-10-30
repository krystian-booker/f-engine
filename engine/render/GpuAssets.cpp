#include "engine/render/GpuAssets.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#include "engine/render/MeshChunks.h"
#include "engine/render/RenderCommon.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <tuple>

namespace fe {
namespace {
constexpr float ChunkSize = 8.0f;

struct Upload {
    bool srgb = false, mips = false;
    std::uint64_t flags = 0;
};

Upload uploadFor(TextureUse use) {
    constexpr std::uint64_t point = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
    constexpr std::uint64_t clamp = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    switch (use) {
    case TextureUse::Ui:
    case TextureUse::SpriteIndex:
        return {false, false, point | clamp};
    case TextureUse::UiSmooth:
        return {false, false, clamp};
    case TextureUse::Sprite:
        return {true, false, point | clamp};
    case TextureUse::Scenery:
        return {true, true, clamp};
    case TextureUse::Panorama:
        return {true, false, BGFX_SAMPLER_V_CLAMP};
    }
    return {};
}

std::uint64_t nextRevision() {
    static std::atomic<std::uint64_t> next{1};
    return next++;
}

bgfx::TextureHandle upload(const Image& image, const Upload& how) {
    const bgfx::Memory* memory = nullptr;
    if (how.mips) {
        std::vector<Image> levels;
        std::size_t bytes = image.rgba.size();
        for (const Image* level = &image; level->width > 1 || level->height > 1; level = &levels.back()) {
            levels.push_back(level->halved(how.srgb));
            bytes += levels.back().rgba.size();
        }
        memory = bgfx::alloc(static_cast<std::uint32_t>(bytes));
        auto* out = memory->data;
        std::memcpy(out, image.rgba.data(), image.rgba.size());
        out += image.rgba.size();
        for (const auto& level : levels) {
            std::memcpy(out, level.rgba.data(), level.rgba.size());
            out += level.rgba.size();
        }
    } else {
        memory = bgfx::copy(image.rgba.data(), static_cast<std::uint32_t>(image.rgba.size()));
    }
    const auto handle = bgfx::createTexture2D(
        static_cast<std::uint16_t>(image.width), static_cast<std::uint16_t>(image.height), how.mips, 1,
        bgfx::TextureFormat::RGBA8, how.flags | (how.srgb ? BGFX_TEXTURE_SRGB : 0), memory);
    if (!bgfx::isValid(handle)) {
        throw std::runtime_error("Could not create a GPU texture");
    }
    return handle;
}

bgfx::TextureFormat::Enum cookedFormat(KtxInfo::Format format) {
    switch (format) {
    case KtxInfo::Format::Rgba8:
        return bgfx::TextureFormat::RGBA8;
    case KtxInfo::Format::Bc1:
        return bgfx::TextureFormat::BC1;
    case KtxInfo::Format::Bc3:
        return bgfx::TextureFormat::BC3;
    case KtxInfo::Format::Bc7:
        return bgfx::TextureFormat::BC7;
    case KtxInfo::Format::Other:
        break;
    }
    return bgfx::TextureFormat::Unknown;
}

bool cookedUse(TextureUse use) {
    return use == TextureUse::Scenery || use == TextureUse::Panorama;
}

std::uint32_t wrapFlags(TextureSampler::Wrap wrap, std::uint32_t clamp, std::uint32_t mirror) {
    switch (wrap) {
    case TextureSampler::Wrap::Clamp:
        return clamp;
    case TextureSampler::Wrap::Mirror:
        return mirror;
    case TextureSampler::Wrap::Repeat:
        break;
    }
    return 0;
}

bgfx::ShaderHandle loadShader(const std::filesystem::path& file, const std::string& bytes) {
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
    std::istringstream stream(readFile(file));
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

std::string readShaderBinary(const std::filesystem::path& file) {
    if (!isFile(file)) {
        throw std::runtime_error("Cannot open shader " + file.string());
    }
    auto bytes = readFile(file);
    if (bytes.size() < 12 || bytes.size() > 64 * 1024 * 1024) {
        throw std::runtime_error("Invalid shader size: " + file.string());
    }
    return bytes;
}

const char* shaderBackend() {
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

bool shaderInterfacesMatch(const std::string& vertex, const std::string& fragment) {
    // Shader binaries carry a magic tag and the varying hash the pair must share.
    return vertex.size() >= 12 && fragment.size() >= 12 && vertex[0] == 'V' && fragment[0] == 'F' &&
           vertex[3] == fragment[3] &&
           std::equal(vertex.begin() + 8, vertex.begin() + 12, fragment.begin() + 4);
}

bgfx::ProgramHandle loadProgram(const std::filesystem::path& root, const char* vertex, const char* fragment) {
    const auto directory = root / shaderBackend();
    return loadProgram(directory / (std::string(vertex) + ".bin"),
                       directory / (std::string(fragment) + ".bin"));
}

bgfx::ProgramHandle loadProgram(const std::filesystem::path& vertex, const std::filesystem::path& fragment) {
    const auto vs = readShaderBinary(vertex);
    const auto fs = readShaderBinary(fragment);
    if (!shaderInterfacesMatch(vs, fs)) {
        throw std::runtime_error("Shader interfaces do not match: " + vertex.filename().string() + "/" +
                                 fragment.filename().string());
    }
    auto vsHandle = loadShader(vertex, vs);
    bgfx::ShaderHandle fsHandle = BGFX_INVALID_HANDLE;
    try {
        fsHandle = loadShader(fragment, fs);
    } catch (...) {
        bgfx::destroy(vsHandle);
        throw;
    }
    auto program = bgfx::createProgram(vsHandle, fsHandle, true);
    if (!bgfx::isValid(program)) {
        throw std::runtime_error("Cannot link GPU program " + vertex.filename().string() + "/" +
                                 fragment.filename().string());
    }
    return program;
}

std::uint32_t samplerFlags(const TextureSampler& sampler, bool anisotropic) {
    std::uint32_t flags = wrapFlags(sampler.wrapU, BGFX_SAMPLER_U_CLAMP, BGFX_SAMPLER_U_MIRROR) |
                          wrapFlags(sampler.wrapV, BGFX_SAMPLER_V_CLAMP, BGFX_SAMPLER_V_MIRROR);
    if (sampler.nearestMag) {
        flags |= BGFX_SAMPLER_MAG_POINT;
    }
    if (sampler.nearestMin) {
        flags |= BGFX_SAMPLER_MIN_POINT;
    }
    if (sampler.mips == TextureSampler::Mips::Nearest) {
        flags |= BGFX_SAMPLER_MIP_POINT;
    }
    if (anisotropic && !sampler.nearestMin && sampler.mips != TextureSampler::Mips::None) {
        flags |= BGFX_SAMPLER_MIN_ANISOTROPIC | (sampler.nearestMag ? 0 : BGFX_SAMPLER_MAG_ANISOTROPIC);
    }
    return flags;
}

GpuAssets::GpuAssets(AssetCache& assets) : assets_(assets), revision_(nextRevision()) {
    white_ = Texture(upload(Image::solid(255, 255, 255), uploadFor(TextureUse::Ui)));
    flatNormal_ = Texture(upload(Image::solid(128, 128, 255), uploadFor(TextureUse::Ui)));
}

CookedTexture GpuAssets::cookedState(const std::filesystem::path& original) {
    CookedTexture state;
    state.file = cookedTexture(original);
    if (!state.file.empty()) {
        std::error_code error;
        state.modified = std::filesystem::last_write_time(state.file, error);
    }
    return state;
}

bool GpuAssets::uploadCooked(const std::filesystem::path& original, bool srgb, std::uint64_t flags,
                             Texture& texture, CookedTexture& cooked) {
    cooked = cookedState(original);
    if (cooked.file.empty()) {
        return false;
    }
    std::string file;
    try {
        file = readFile(cooked.file);
    } catch (const std::exception& error) {
        log(LogLevel::Warning, error.what());
        return false;
    }
    const std::span bytes(reinterpret_cast<const std::uint8_t*>(file.data()), file.size());
    const auto header = readKtxHeader(bytes);
    const auto format = header ? cookedFormat(header->format) : bgfx::TextureFormat::Unknown;
    const auto supported = format == bgfx::TextureFormat::Unknown
                               ? 0
                               : bgfx::getCaps()->formats[format] &
                                     (srgb ? BGFX_CAPS_FORMAT_TEXTURE_2D_SRGB : BGFX_CAPS_FORMAT_TEXTURE_2D);
    if (!supported) {
        if (failed_.insert(cooked.file.string()).second) {
            log(LogLevel::Warning, "Using " + original.filename().string() + ": the GPU cannot sample " +
                                       cooked.file.filename().string());
        }
        return false;
    }
    const auto handle =
        bgfx::createTexture(bgfx::copy(bytes.data(), static_cast<std::uint32_t>(bytes.size())),
                            flags | (srgb ? BGFX_TEXTURE_SRGB : 0));
    if (!bgfx::isValid(handle)) {
        return false;
    }
    texture = Texture(handle);
    return true;
}

void GpuAssets::sync() {
    const bool filesChanged = fileChanges_ != assets_.fileChanges();
    const bool assetsChanged = generation_ != assets_.generation();
    if (!filesChanged && !assetsChanged) {
        return;
    }
    fileChanges_ = assets_.fileChanges();
    generation_ = assets_.generation();
    revision_ = nextRevision();
    if (filesChanged) {
        // .cube files and cooked textures are not CPU assets, so their files are checked here.
        std::erase_if(luts_, [](const auto& entry) {
            std::error_code error;
            return !entry.second.file.empty() &&
                   std::filesystem::last_write_time(entry.second.file, error) != entry.second.modified;
        });
        for (auto& [path, uses] : images_) {
            for (auto& entry : uses) {
                if (!entry.original.empty() && cookedState(entry.original) != entry.cooked) {
                    entry = {};
                }
            }
        }
        bool stale = false;
        for (auto& [key, texture] : modelTextures_) {
            if (!texture->original.empty() && cookedState(texture->original) != texture->cooked) {
                texture->stale = true;
                stale = true;
            }
        }
        if (stale) {
            // Only the models that use a changed texture re-upload; the rest keep theirs.
            std::erase_if(models_, [](const auto& entry) {
                for (const auto& part : entry.second->parts) {
                    for (const auto& texture : part.textures) {
                        if (texture && texture->stale) {
                            return true;
                        }
                    }
                }
                return false;
            });
            std::erase_if(modelTextures_, [](const auto& entry) { return entry.second->stale; });
        }
    }
    // An asset the cache no longer holds (evicted) or replaced (reloaded)
    // takes its GPU copy with it. loaded() starts no load, so an evicted
    // asset stays evicted until something uses it again.
    const auto imageGone = [&](const std::string& path, const std::shared_ptr<const Image>& image) {
        return !assets_.loaded(path) || assets_.tryImage(path) != image;
    };
    for (auto& [path, uses] : images_) {
        const bool loaded = assets_.loaded(path);
        const auto current = loaded ? assets_.tryImage(path) : nullptr;
        for (auto& entry : uses) {
            if (entry.source && (!loaded || entry.source != current)) {
                entry = {};
            }
        }
    }
    std::erase_if(sheets_, [&](const auto& entry) {
        const auto& sheet = entry.second;
        const auto replaced = [&](const std::string& path, const std::shared_ptr<const Image>& image) {
            return !path.empty() && imageGone(path, image);
        };
        return !assets_.loaded(entry.first) || assets_.trySprite(entry.first) != sheet.animation ||
               (sheet.animation &&
                (replaced(sheet.atlasPath, sheet.atlas) || replaced(sheet.normalPath, sheet.normalImage) ||
                 replaced(sheet.emissivePath, sheet.emissiveImage)));
    });
    std::erase_if(models_, [&](const auto& entry) {
        return !assets_.loaded(entry.first.first) || assets_.tryModel(entry.first.first) != entry.second->source;
    });
    // A texture no model part holds any more belonged to dropped models only.
    std::erase_if(modelTextures_, [](const auto& entry) { return entry.second.use_count() == 1; });
    failed_.clear();
    if (assetsChanged) {
        // Redefined fonts are new objects that may reuse a freed address, so
        // cached pages cannot be matched to their font; they re-upload on use.
        fontPages_.clear();
    }
}

bgfx::TextureHandle GpuAssets::image(const std::string& path, TextureUse use) {
    auto found = images_.find(path);
    if (found == images_.end()) {
        found = images_.emplace(path, std::array<ImageEntry, TextureUseCount>{}).first;
    }
    auto& entry = found->second[static_cast<std::size_t>(use)];
    if (entry.texture.valid()) {
        return entry.texture.get();
    }
    const auto source = assets_.tryImage(path);
    if (!source) {
        return white_.get();
    }
    entry.source = source;
    const auto how = uploadFor(use);
    if (cookedUse(use)) {
        entry.original = assets_.file(path);
        if (uploadCooked(entry.original, how.srgb, how.flags, entry.texture, entry.cooked)) {
            return entry.texture.get();
        }
    }
    entry.texture = Texture(upload(*source, how));
    return entry.texture.get();
}

glm::ivec2 GpuAssets::imageSize(const std::string& path) {
    const auto image = assets_.tryImage(path);
    return image ? glm::ivec2(image->width, image->height) : glm::ivec2(0);
}

const SpriteSheet* GpuAssets::spriteSheet(const std::string& animationPath) {
    if (const auto found = sheets_.find(animationPath); found != sheets_.end()) {
        return found->second.atlas ? &found->second : nullptr;
    }
    auto& sheet = sheets_[animationPath];
    sheet.animation = assets_.trySprite(animationPath);
    if (!sheet.animation) {
        return nullptr;
    }
    sheet.atlasPath =
        (std::filesystem::path(animationPath).parent_path() / sheet.animation->atlas).generic_string();
    sheet.atlas = assets_.tryImage(sheet.atlasPath);
    if (!sheet.atlas) {
        return nullptr;
    }
    sheet.size = {sheet.atlas->width, sheet.atlas->height};
    sheet.color = image(sheet.atlasPath, TextureUse::Sprite);
    const auto folder = std::filesystem::path(animationPath).parent_path();
    const auto sameLayout = [&](const std::string& name, std::string& path,
                                std::shared_ptr<const Image>& source) {
        if (name.empty()) {
            return false;
        }
        path = (folder / name).generic_string();
        source = assets_.tryImage(path);
        if (source && (source->width != sheet.size.x || source->height != sheet.size.y)) {
            if (failed_.insert(path).second) {
                log(LogLevel::Error, path + " must be the size of its sprite atlas");
            }
            return false;
        }
        return source != nullptr;
    };
    if (sameLayout(sheet.animation->normal, sheet.normalPath, sheet.normalImage)) {
        sheet.normal = image(sheet.normalPath, TextureUse::SpriteIndex);
    }
    if (sameLayout(sheet.animation->emissive, sheet.emissivePath, sheet.emissiveImage)) {
        sheet.emissive = image(sheet.emissivePath, TextureUse::Sprite);
    }
    return &sheet;
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
    entry.second = Texture(upload(image, uploadFor(font->pixel() ? TextureUse::Ui : TextureUse::UiSmooth)));
    return entry.second.get();
}

bgfx::TextureHandle GpuAssets::lut(const std::string& path) {
    if (const auto found = luts_.find(path); found != luts_.end()) {
        return found->second.texture.get();
    }
    std::vector<std::uint8_t> bytes;
    LutEntry entry;
    try {
        if (!path.empty()) {
            std::error_code error;
            entry.file = assets_.file(path);
            entry.modified = std::filesystem::last_write_time(entry.file, error);
        }
        bytes = path.empty() ? identityLut() : readCube(entry.file);
    } catch (const std::exception& error) {
        if (failed_.insert(path).second) {
            log(LogLevel::Error, error.what());
        }
        bytes = identityLut();
    }
    entry.texture =
        Texture(bgfx::createTexture3D(16, 16, 16, false, bgfx::TextureFormat::RGBA8,
                                      BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
                                      bgfx::copy(bytes.data(), static_cast<std::uint32_t>(bytes.size()))));
    return luts_.emplace(path, std::move(entry)).first->second.texture.get();
}

std::shared_ptr<const ModelTexture> GpuAssets::modelTexture(const ModelImage& image, bool srgb, bool mips) {
    if (!image) {
        return nullptr;
    }
    // Every model that references the same file shares the upload; an
    // embedded (.glb) image is its own, identified by its address, which
    // stays unique while the entry holds the image.
    const std::string key = image.file.empty()
                                ? "#" + std::to_string(reinterpret_cast<std::uintptr_t>(image.image.get()))
                                : image.file.generic_string();
    auto& entry = modelTextures_[{key, srgb, mips}];
    if (!entry) {
        entry = std::make_shared<ModelTexture>();
        if (image.file.empty()) {
            entry->embedded = image.image;
        } else {
            entry->original = image.file;
        }
        // A cooked file carries a mip chain, so a sampler without mips takes the PNG.
        if (!mips || !uploadCooked(image.file, srgb, 0, entry->texture, entry->cooked)) {
            entry->texture = Texture(upload(*image.image, {srgb, mips, 0}));
        }
    }
    return entry;
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
    auto emit = [&](const ModelPart& part, const std::vector<WorldVertex>& vertices,
                    const std::vector<std::uint32_t>& indices, glm::vec3 min, glm::vec3 max) {
        if (indices.empty()) {
            return;
        }
        GpuPart out;
        out.vertices = VertexBuffer(bgfx::createVertexBuffer(
            bgfx::copy(vertices.data(), static_cast<std::uint32_t>(vertices.size() * sizeof(WorldVertex))),
            worldLayout()));
        if (vertices.size() <= std::numeric_limits<std::uint16_t>::max()) {
            const std::vector<std::uint16_t> narrow(indices.begin(), indices.end());
            out.indices = IndexBuffer(bgfx::createIndexBuffer(bgfx::copy(
                narrow.data(), static_cast<std::uint32_t>(narrow.size() * sizeof(std::uint16_t)))));
        } else {
            out.indices = IndexBuffer(bgfx::createIndexBuffer(
                bgfx::copy(indices.data(),
                           static_cast<std::uint32_t>(indices.size() * sizeof(std::uint32_t))),
                BGFX_BUFFER_INDEX32));
        }
        const auto& m = part.material;
        const auto mips = [](const TextureSampler& sampler) {
            return sampler.mips != TextureSampler::Mips::None;
        };
        out.textures = {modelTexture(m.albedo, true, mips(m.albedoSampler)),
                        modelTexture(m.normal, false, mips(m.normalSampler)),
                        modelTexture(m.emission, true, mips(m.emissionSampler))};
        out.albedo = out.textures[0] ? out.textures[0]->texture.get() : white_.get();
        out.normal = out.textures[1] ? out.textures[1]->texture.get() : flatNormal_.get();
        out.emission = out.textures[2] ? out.textures[2]->texture.get() : white_.get();
        out.material = part.material;
        out.node = part.node;
        out.min = min;
        out.max = max;
        gpu->parts.push_back(std::move(out));
    };
    auto pack = [](const MeshVertex& v) {
        return WorldVertex::pack(v.position, v.normal, v.uv, v.color, v.tangent);
    };
    std::vector<glm::mat4> rest;
    source->pose(-1, 0, rest);
    for (const auto& part : source->parts) {
        if (part.hidden) {
            continue;
        }
        if (!bakeChunks) {
            std::vector<WorldVertex> vertices;
            vertices.reserve(part.vertices.size());
            for (const auto& v : part.vertices) {
                vertices.push_back(pack(v));
            }
            emit(part, vertices, part.indices, part.min, part.max);
            continue;
        }
        const auto& matrix = rest[static_cast<std::size_t>(part.node)];
        const auto normalMatrix = glm::transpose(glm::inverse(glm::mat3(matrix)));
        std::vector<MeshVertex> world(part.vertices);
        for (auto& v : world) {
            v.position = glm::vec3(matrix * glm::vec4(v.position, 1));
            v.normal = glm::normalize(normalMatrix * v.normal);
            v.tangent = glm::vec4(glm::normalize(glm::mat3(matrix) * glm::vec3(v.tangent)), v.tangent.w);
        }
        std::vector<glm::vec3> positions;
        positions.reserve(world.size());
        for (const auto& v : world) {
            positions.push_back(v.position);
        }
        std::vector<WorldVertex> vertices;
        for (const auto& chunk : splitChunks(positions, part.indices, ChunkSize)) {
            vertices.clear();
            glm::vec3 min(std::numeric_limits<float>::max()), max(-std::numeric_limits<float>::max());
            for (const auto source : chunk.vertices) {
                vertices.push_back(pack(world[source]));
                min = glm::min(min, world[source].position);
                max = glm::max(max, world[source].position);
            }
            emit(part, vertices, chunk.indices, min, max);
        }
    }
    return models_.emplace(key, std::move(gpu)).first->second.get();
}
} // namespace fe
