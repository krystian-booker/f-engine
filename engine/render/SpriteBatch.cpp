#include "engine/render/SpriteBatch.h"
#include "engine/render/SceneLighting.h"
#include <cstring>
#include <functional>

namespace fe {
namespace {
constexpr float OpaqueCutoff = 0.5f, BlendCutoff = 0.01f;
constexpr float SilhouetteOutput = 4;
constexpr std::uint64_t ShadowState = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
constexpr std::uint64_t SilhouetteState =
    BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_GREATER | AlphaBlend;
} // namespace

std::size_t SpriteBatch::MaterialHash::operator()(const Material& m) const {
    std::size_t hash = 0;
    const auto mix = [&](std::size_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    };
    for (const auto idx : {m.atlas.idx, m.palette.idx, m.normal.idx, m.emissive.idx, m.program.idx}) {
        mix(idx);
    }
    mix(std::hash<float>{}(m.texel.x));
    mix(std::hash<float>{}(m.texel.y));
    mix(m.paletteSwap ? 1 : 0);
    return hash;
}

void SpriteBatch::clear() {
    materials_.clear();
    materialIndex_.clear();
    quads_.clear();
    vertices_.clear();
    uploaded_ = false;
}

void SpriteBatch::add(const Material& material, const std::array<SpriteVertex, 4>& vertices,
                      const Quad& settings) {
    const auto [found, added] = materialIndex_.try_emplace(material, static_cast<int>(materials_.size()));
    if (added) {
        materials_.push_back(material);
    }
    Quad quad = settings;
    quad.material = found->second;
    quad.min = quad.max = vertices[0].position;
    for (const auto& v : vertices) {
        quad.min = glm::min(quad.min, v.position);
        quad.max = glm::max(quad.max, v.position);
    }
    quad.centre = (quad.min + quad.max) * 0.5f;
    quads_.push_back(quad);
    vertices_.insert(vertices_.end(), vertices.begin(), vertices.end());
}

bool SpriteBatch::upload() {
    const auto count = static_cast<std::uint32_t>(vertices_.size());
    uploaded_ = count > 0 && bgfx::getAvailTransientVertexBuffer(count, spriteLayout()) >= count;
    if (uploaded_) {
        bgfx::allocTransientVertexBuffer(&buffer_, count, spriteLayout());
        std::memcpy(buffer_.data, vertices_.data(), count * sizeof(SpriteVertex));
    }
    return uploaded_;
}

void SpriteBatch::draw(std::uint16_t view, const std::vector<int>& quads, bgfx::ProgramHandle program,
                       Pass pass, const Context& context, std::uint32_t& calls) const {
    if (!uploaded_) {
        return;
    }
    const auto& uniforms = context.uniforms;
    const auto* lighting = context.lighting;
    const auto quadAt = [&](std::size_t k) -> const Quad& {
        return quads_[static_cast<std::size_t>(quads[k])];
    };
    std::size_t start = 0;
    while (start < quads.size()) {
        const auto& first = quadAt(start);
        std::size_t end = start + 1;
        while (end < quads.size() && quadAt(end).material == first.material &&
               (pass != Pass::Transparent || quadAt(end).blend == first.blend)) {
            ++end;
        }
        const auto count = static_cast<std::uint32_t>((end - start) * 6);
        if (bgfx::getAvailTransientIndexBuffer(count, true) < count) {
            return;
        }
        indices_.clear();
        for (std::size_t k = start; k < end; ++k) {
            const auto base = static_cast<std::uint32_t>(quads[k]) * 4;
            for (const auto corner : {0u, 1u, 2u, 0u, 2u, 3u}) {
                indices_.push_back(base + corner);
            }
        }
        bgfx::TransientIndexBuffer ib;
        bgfx::allocTransientIndexBuffer(&ib, count, true);
        std::memcpy(ib.data, indices_.data(), count * sizeof(std::uint32_t));
        bgfx::setVertexBuffer(0, &buffer_);
        bgfx::setIndexBuffer(&ib);
        const auto& m = materials_[static_cast<std::size_t>(first.material)];
        bgfx::setTexture(0, uniforms.albedo.get(), m.atlas);
        std::uint64_t state = ShadowState;
        float cutoff = OpaqueCutoff, output = 0;
        switch (pass) {
        case Pass::Shadow:
            break;
        case Pass::Opaque:
            state = OpaqueState;
            break;
        case Pass::Transparent:
            state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LESS |
                    blendFunction(first.blend);
            cutoff = BlendCutoff;
            output = blendOutput(first.blend);
            break;
        case Pass::Silhouette:
            state = SilhouetteState;
            output = SilhouetteOutput;
            break;
        }
        if (lighting) {
            const bool normalMap = bgfx::isValid(m.normal), emissive = bgfx::isValid(m.emissive);
            bgfx::setTexture(1, uniforms.palette.get(), m.palette);
            bgfx::setTexture(5, uniforms.spriteNormal.get(), normalMap ? m.normal : context.flatNormal);
            bgfx::setTexture(6, uniforms.spriteEmissive.get(), emissive ? m.emissive : context.white);
            lighting->bind(uniforms);
            const glm::vec4 sprite[Uniforms::SpriteCount] = {
                glm::vec4(m.texel, m.paletteSwap ? 1.0f : 0.0f, cutoff),
                glm::vec4(output, normalMap ? 1.0f : 0.0f, emissive ? 1.0f : 0.0f, 0)};
            bgfx::setUniform(uniforms.sprite.get(), sprite, Uniforms::SpriteCount);
        } else {
            const glm::vec4 shadowMaterial[Uniforms::MaterialCount] = {
                glm::vec4(1), glm::vec4(cutoff, 0, 0, 0), glm::vec4(0)};
            bgfx::setUniform(uniforms.material.get(), shadowMaterial, Uniforms::MaterialCount);
        }
        bgfx::setState(state);
        const bool custom = bgfx::isValid(m.program) && (pass == Pass::Opaque || pass == Pass::Transparent);
        if (custom) {
            bgfx::setUniform(uniforms.info.get(), &context.info);
        }
        submitDraw(view, custom ? m.program : program, calls);
        start = end;
    }
}
} // namespace fe
