#include "engine/assets/Model.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#define CGLTF_IMPLEMENTATION
#include <algorithm>
#include <array>
#include <cgltf.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>

namespace fe {
bool isShaderName(std::string_view name) {
    return !name.empty() && name.size() <= 64 &&
           name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string_view::npos;
}

namespace {
using GltfData = std::unique_ptr<cgltf_data, decltype(&cgltf_free)>;

// glTF files and their buffers may come from a pak archive.
cgltf_result readGltfFile(const cgltf_memory_options*, const cgltf_file_options*, const char* path,
                          cgltf_size* size, void** data) {
    try {
        const auto bytes = readFile(std::filesystem::path(path));
        void* copy = std::malloc(std::max<std::size_t>(bytes.size(), 1));
        if (!copy) {
            return cgltf_result_out_of_memory;
        }
        std::memcpy(copy, bytes.data(), bytes.size());
        *size = bytes.size();
        *data = copy;
        return cgltf_result_success;
    } catch (const std::exception&) {
        return cgltf_result_file_not_found;
    }
}

void releaseGltfFile(const cgltf_memory_options*, const cgltf_file_options*, void* data) {
    std::free(data);
}

GltfData parse(const std::filesystem::path& path) {
    cgltf_options options{};
    options.file.read = readGltfFile;
    options.file.release = releaseGltfFile;
    cgltf_data* raw = nullptr;
    if (cgltf_parse_file(&options, path.string().c_str(), &raw) != cgltf_result_success) {
        throw std::runtime_error("Cannot parse glTF " + path.string());
    }
    GltfData data(raw, cgltf_free);
    if (cgltf_load_buffers(&options, data.get(), path.string().c_str()) != cgltf_result_success ||
        cgltf_validate(data.get()) != cgltf_result_success) {
        throw std::runtime_error("Invalid glTF buffers: " + path.string());
    }
    return data;
}

struct ImageCache {
    const std::filesystem::path& path;
    std::map<const cgltf_image*, ModelImage> images;
    ModelImage operator()(const cgltf_texture_view& view) {
        if (!view.texture || !view.texture->image) {
            return {};
        }
        const auto* source = view.texture->image;
        if (const auto found = images.find(source); found != images.end()) {
            return found->second;
        }
        ModelImage result;
        if (source->buffer_view) {
            const auto* bytes = static_cast<const std::uint8_t*>(cgltf_buffer_view_data(source->buffer_view));
            if (!bytes) {
                throw std::runtime_error("Missing glTF embedded image buffer");
            }
            result.image = std::make_shared<Image>(Image::decode({bytes, source->buffer_view->size}));
        } else if (source->uri && !std::string_view(source->uri).starts_with("data:")) {
            std::string uri = source->uri;
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            // Confined to the model's folder: a shipped .pak must not reach
            // outside the game through a "../../" image URI.
            try {
                result.file = resolveInside(path.parent_path(), uri);
            } catch (const std::exception&) {
                throw std::runtime_error("glTF image '" + uri +
                                         "' lies outside the model's folder: " + path.string());
            }
            result.image = std::make_shared<Image>(Image::load(result.file));
        } else {
            throw std::runtime_error("glTF images must use a file URI or a GLB buffer view");
        }
        images[source] = result;
        return result;
    }
};

TextureSampler::Wrap wrapMode(cgltf_wrap_mode mode) {
    switch (mode) {
    case cgltf_wrap_mode_clamp_to_edge:
        return TextureSampler::Wrap::Clamp;
    case cgltf_wrap_mode_mirrored_repeat:
        return TextureSampler::Wrap::Mirror;
    case cgltf_wrap_mode_repeat:
        break;
    }
    return TextureSampler::Wrap::Repeat;
}

TextureSampler readSampler(const cgltf_texture_view& view) {
    TextureSampler out;
    if (!view.texture || !view.texture->sampler) {
        return out;
    }
    const auto& sampler = *view.texture->sampler;
    out.wrapU = wrapMode(sampler.wrap_s);
    out.wrapV = wrapMode(sampler.wrap_t);
    out.nearestMag = sampler.mag_filter == cgltf_filter_type_nearest;
    switch (sampler.min_filter) {
    case cgltf_filter_type_nearest:
        out.nearestMin = true;
        out.mips = TextureSampler::Mips::None;
        break;
    case cgltf_filter_type_linear:
        out.mips = TextureSampler::Mips::None;
        break;
    case cgltf_filter_type_nearest_mipmap_nearest:
        out.nearestMin = true;
        out.mips = TextureSampler::Mips::Nearest;
        break;
    case cgltf_filter_type_linear_mipmap_nearest:
        out.mips = TextureSampler::Mips::Nearest;
        break;
    case cgltf_filter_type_nearest_mipmap_linear:
        out.nearestMin = true;
        break;
    case cgltf_filter_type_undefined:
    case cgltf_filter_type_linear_mipmap_linear:
        break;
    }
    return out;
}

Material readMaterial(const cgltf_material* source, ImageCache& images, const Json& nodeProperties) {
    Material material;
    if (!source) {
        return material;
    }
    if (source->has_pbr_metallic_roughness) {
        material.color = glm::make_vec4(source->pbr_metallic_roughness.base_color_factor);
        material.albedo = images(source->pbr_metallic_roughness.base_color_texture);
        material.albedoSampler = readSampler(source->pbr_metallic_roughness.base_color_texture);
    }
    material.emissive = glm::make_vec3(source->emissive_factor) *
                        (source->has_emissive_strength ? source->emissive_strength.emissive_strength : 1.0f);
    material.emission = images(source->emissive_texture);
    material.emissionSampler = readSampler(source->emissive_texture);
    material.normal = images(source->normal_texture);
    material.normalSampler = readSampler(source->normal_texture);
    material.normalScale = source->normal_texture.scale;
    material.transparent = source->alpha_mode == cgltf_alpha_mode_blend;
    // OPAQUE ignores alpha, as the glTF spec says; BLEND skips what would be invisible.
    material.alphaCutoff = source->alpha_mode == cgltf_alpha_mode_mask    ? source->alpha_cutoff
                           : source->alpha_mode == cgltf_alpha_mode_blend ? 0.01f
                                                                          : 0.0f;
    material.doubleSided = source->double_sided;
    if (source->unlit) {
        material.kind = MaterialKind::Unlit;
    }
    std::string kind = nodeProperties.value("material", std::string{});
    Json shader = nodeProperties.value("shader", Json());
    if (source->extras.data) {
        const auto extras = Json::parse(source->extras.data, nullptr, false);
        if (extras.is_object()) {
            const auto& properties = extras.value("fengine", extras);
            kind = properties.value("material", kind);
            shader = properties.value("shader", shader);
        }
    }
    if (shader.is_string() && isShaderName(shader.get<std::string>())) {
        material.shader = shader.get<std::string>();
    } else if (!shader.is_null()) {
        log(LogLevel::Error, "Material '" + std::string(source->name ? source->name : "") +
                                 "' names an invalid shader " + shader.dump() +
                                 " (use a-z, 0-9, _ and -); the engine's shader is used");
    }
    if (kind == "water") {
        material.kind = MaterialKind::Water;
        material.transparent = true;
    } else if (kind == "unlit") {
        material.kind = MaterialKind::Unlit;
    }
    return material;
}

void readAccessor(const cgltf_accessor* accessor, std::size_t index, float* out, std::size_t count) {
    if (!cgltf_accessor_read_float(accessor, index, out, count)) {
        throw std::runtime_error("Unreadable glTF accessor");
    }
}

ModelPart readPrimitive(const cgltf_primitive& primitive, ImageCache& images, const Json& properties) {
    if (primitive.type != cgltf_primitive_type_triangles || primitive.has_draco_mesh_compression) {
        throw std::runtime_error("glTF meshes must be uncompressed triangles");
    }
    const cgltf_accessor* positions = nullptr;
    const cgltf_accessor* normals = nullptr;
    const cgltf_accessor* uvs = nullptr;
    const cgltf_accessor* colors = nullptr;
    const cgltf_accessor* tangents = nullptr;
    const cgltf_accessor* joints = nullptr;
    const cgltf_accessor* weights = nullptr;
    for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
        const auto& attribute = primitive.attributes[a];
        if (attribute.type == cgltf_attribute_type_position) {
            positions = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_normal) {
            normals = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_tangent) {
            tangents = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) {
            uvs = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_color && attribute.index == 0) {
            colors = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_joints && attribute.index == 0) {
            joints = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_weights && attribute.index == 0) {
            weights = attribute.data;
        }
    }
    if (!positions || positions->count > 10000000 || positions->type != cgltf_type_vec3) {
        throw std::runtime_error("Missing or invalid glTF positions");
    }
    const auto count = positions->count;
    if ((normals && normals->count != count) || (uvs && uvs->count != count) ||
        (colors && colors->count != count) || (tangents && tangents->count != count) ||
        (joints && joints->count != count) || (weights && weights->count != count)) {
        throw std::runtime_error("glTF vertex attributes disagree in length");
    }
    // Every read below copies an accessor's components into four floats, so
    // an accessor typed wider than its attribute allows (a MAT4 colour, say)
    // is refused here instead of overrunning them.
    const auto typed = [](const cgltf_accessor* accessor, std::initializer_list<cgltf_type> allowed) {
        return !accessor || std::find(allowed.begin(), allowed.end(), accessor->type) != allowed.end();
    };
    if (!typed(normals, {cgltf_type_vec3}) || !typed(tangents, {cgltf_type_vec4}) ||
        !typed(uvs, {cgltf_type_vec2}) || !typed(colors, {cgltf_type_vec3, cgltf_type_vec4}) ||
        !typed(joints, {cgltf_type_vec4}) || !typed(weights, {cgltf_type_vec4})) {
        throw std::runtime_error("A glTF vertex attribute has the wrong accessor type");
    }
    ModelPart part;
    part.vertices.resize(count);
    part.min = glm::vec3(std::numeric_limits<float>::max());
    part.max = -part.min;
    for (std::size_t v = 0; v < count; ++v) {
        auto& vertex = part.vertices[v];
        std::array<float, 4> values{1, 1, 1, 1};
        readAccessor(positions, v, values.data(), 3);
        vertex.position = {values[0], values[1], values[2]};
        part.min = glm::min(part.min, vertex.position);
        part.max = glm::max(part.max, vertex.position);
        if (normals) {
            readAccessor(normals, v, values.data(), 3);
            vertex.normal = glm::normalize(glm::vec3(values[0], values[1], values[2]));
        }
        if (tangents && normals) {
            readAccessor(tangents, v, values.data(), 4);
            vertex.tangent = glm::make_vec4(values.data());
        }
        if (uvs) {
            readAccessor(uvs, v, values.data(), 2);
            vertex.uv = {values[0], values[1]};
        }
        if (colors) {
            values = {1, 1, 1, 1};
            readAccessor(colors, v, values.data(), cgltf_num_components(colors->type));
            vertex.color = glm::make_vec4(values.data());
        }
    }
    if (joints && weights) {
        part.joints.resize(count);
        part.weights.resize(count);
        for (std::size_t v = 0; v < count; ++v) {
            std::array<cgltf_uint, 4> index{0, 0, 0, 0};
            if (!cgltf_accessor_read_uint(joints, v, index.data(), 4)) {
                throw std::runtime_error("Unreadable glTF joints");
            }
            std::array<float, 4> weight{0, 0, 0, 0};
            readAccessor(weights, v, weight.data(), 4);
            float sum = 0;
            for (std::size_t k = 0; k < 4; ++k) {
                if (index[k] > std::numeric_limits<std::uint16_t>::max()) {
                    throw std::runtime_error("glTF joint index out of range");
                }
                weight[k] = std::isfinite(weight[k]) ? std::max(0.0f, weight[k]) : 0.0f;
                sum += weight[k];
                part.joints[v][k] = static_cast<std::uint16_t>(index[k]);
            }
            // Weights sum to 1; a vertex bound to nothing follows its first joint.
            part.weights[v] = sum > 1e-6f ? glm::make_vec4(weight.data()) / sum : glm::vec4(1, 0, 0, 0);
        }
    }
    const std::size_t indexCount = primitive.indices ? primitive.indices->count : count;
    if (indexCount % 3 != 0) {
        throw std::runtime_error("Incomplete glTF triangle list");
    }
    part.indices.reserve(indexCount);
    for (std::size_t i = 0; i < indexCount; ++i) {
        const auto index = primitive.indices ? cgltf_accessor_read_index(primitive.indices, i) : i;
        if (index >= count) {
            throw std::runtime_error("glTF index outside its vertex buffer");
        }
        part.indices.push_back(static_cast<std::uint32_t>(index));
    }
    if (!normals) {
        for (auto& vertex : part.vertices) {
            vertex.normal = glm::vec3(0);
        }
        for (std::size_t i = 0; i < part.indices.size(); i += 3) {
            auto& a = part.vertices[part.indices[i]];
            auto& b = part.vertices[part.indices[i + 1]];
            auto& c = part.vertices[part.indices[i + 2]];
            const auto n = glm::cross(b.position - a.position, c.position - a.position);
            a.normal += n;
            b.normal += n;
            c.normal += n;
        }
        for (auto& vertex : part.vertices) {
            vertex.normal =
                glm::length(vertex.normal) > 1e-5f ? glm::normalize(vertex.normal) : glm::vec3(0, 1, 0);
        }
    }
    // Authored tangents only match authored normals.
    if (!tangents || !normals) {
        generateTangents(part.vertices, part.indices);
    }
    part.material = readMaterial(primitive.material, images, properties);
    part.hidden = properties.value("visible", true) == false;
    return part;
}

Json nodeProperties(const cgltf_node& node) {
    Json properties = Json::object();
    if (node.extras.data) {
        auto extras = Json::parse(node.extras.data, nullptr, false);
        if (extras.is_object()) {
            properties = extras.value("fengine", extras);
        }
    }
    if (node.light && node.light->type != cgltf_light_type_directional) {
        const auto* light = node.light;
        Json config{{"color", {light->color[0], light->color[1], light->color[2]}},
                    {"intensity", light->intensity},
                    {"radius", light->range > 0 ? light->range : 5}};
        config["type"] = light->type == cgltf_light_type_spot ? "spot" : "point";
        if (light->type == cgltf_light_type_spot) {
            // Engine spots need inner < outer <= 90 degrees; a hard-edged
            // glTF cone (inner == outer, Blender's blend 0) keeps a half-degree edge.
            const float outer = std::clamp(glm::degrees(light->spot_outer_cone_angle), 0.5f, 90.0f);
            config["inner_cone"] = std::clamp(glm::degrees(light->spot_inner_cone_angle), 0.0f, outer - 0.5f);
            config["outer_cone"] = outer;
        }
        properties["light"] = std::move(config);
    }
    return properties;
}

void readAnimations(const cgltf_data& data, Model& model) {
    for (std::size_t a = 0; a < data.animations_count; ++a) {
        const auto& source = data.animations[a];
        ModelAnimation animation;
        animation.name = source.name ? source.name : "animation" + std::to_string(a);
        for (std::size_t c = 0; c < source.channels_count; ++c) {
            const auto& channel = source.channels[c];
            if (!channel.target_node || channel.target_path == cgltf_animation_path_type_weights) {
                continue;
            }
            AnimationChannel out;
            out.node = static_cast<int>(channel.target_node - data.nodes);
            out.target = channel.target_path == cgltf_animation_path_type_rotation
                             ? AnimationChannel::Target::Rotation
                         : channel.target_path == cgltf_animation_path_type_scale
                             ? AnimationChannel::Target::Scale
                             : AnimationChannel::Target::Translation;
            out.step = channel.sampler->interpolation == cgltf_interpolation_type_step;
            const bool cubic = channel.sampler->interpolation == cgltf_interpolation_type_cubic_spline;
            const auto* input = channel.sampler->input;
            const auto* output = channel.sampler->output;
            // Translation and scale are VEC3, rotation VEC4; anything wider
            // would overrun the four floats a key is read into.
            const auto components = cgltf_num_components(output->type);
            if (components > 4 || output->count < input->count * (cubic ? 3 : 1)) {
                throw std::runtime_error("A glTF animation sampler has an invalid output accessor");
            }
            for (std::size_t k = 0; k < input->count; ++k) {
                float time = 0;
                readAccessor(input, k, &time, 1);
                out.times.push_back(time);
                std::array<float, 4> value{0, 0, 0, 1};
                // Cubic splines store in-tangent, value, out-tangent: keep the value.
                readAccessor(output, cubic ? k * 3 + 1 : k, value.data(), components);
                out.values.emplace_back(value[0], value[1], value[2], value[3]);
            }
            if (!out.times.empty()) {
                animation.duration = std::max(animation.duration, out.times.back());
                animation.channels.push_back(std::move(out));
            }
        }
        // pose() composes a node's local matrix from its run of channels.
        std::stable_sort(
            animation.channels.begin(), animation.channels.end(),
            [](const AnimationChannel& a, const AnimationChannel& b) { return a.node < b.node; });
        model.animations.push_back(std::move(animation));
    }
}

glm::vec4 sampleChannel(const AnimationChannel& channel, float time) {
    const auto& times = channel.times;
    if (time <= times.front()) {
        return channel.values.front();
    }
    if (time >= times.back()) {
        return channel.values.back();
    }
    const auto upper = std::upper_bound(times.begin(), times.end(), time);
    const auto i = static_cast<std::size_t>(upper - times.begin());
    const float t = (time - times[i - 1]) / std::max(1e-6f, times[i] - times[i - 1]);
    if (channel.step) {
        return channel.values[i - 1];
    }
    if (channel.target == AnimationChannel::Target::Rotation) {
        const auto& a = channel.values[i - 1];
        const auto& b = channel.values[i];
        const auto q = glm::slerp(glm::quat(a.w, a.x, a.y, a.z), glm::quat(b.w, b.x, b.y, b.z), t);
        return {q.x, q.y, q.z, q.w};
    }
    return glm::mix(channel.values[i - 1], channel.values[i], t);
}
} // namespace

void generateTangents(std::vector<MeshVertex>& vertices, const std::vector<std::uint32_t>& indices) {
    std::vector<glm::vec3> tangents(vertices.size(), glm::vec3(0)), bitangents(vertices.size(), glm::vec3(0));
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto ia = indices[i], ib = indices[i + 1], ic = indices[i + 2];
        const auto &a = vertices[ia], &b = vertices[ib], &c = vertices[ic];
        const glm::vec3 e1 = b.position - a.position, e2 = c.position - a.position;
        const glm::vec2 d1 = b.uv - a.uv, d2 = c.uv - a.uv;
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) {
            continue;
        }
        // Scaled by the triangle's area (|e1 x e2|) relative to its UV area, so
        // bigger triangles weigh more.
        const float area = glm::length(glm::cross(e1, e2));
        const float r = area / det;
        const glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r, bt = (e2 * d1.x - e1 * d2.x) * r;
        for (const auto index : {ia, ib, ic}) {
            tangents[index] += t;
            bitangents[index] += bt;
        }
    }
    for (std::size_t v = 0; v < vertices.size(); ++v) {
        auto& vertex = vertices[v];
        const auto n = vertex.normal;
        glm::vec3 t = tangents[v] - n * glm::dot(n, tangents[v]);
        if (glm::length(t) < 1e-6f) {
            const auto axis = std::abs(n.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            t = axis - n * glm::dot(n, axis);
        }
        t = glm::normalize(t);
        const float handedness = glm::dot(glm::cross(n, t), bitangents[v]) < 0 ? -1.0f : 1.0f;
        vertex.tangent = glm::vec4(t, handedness);
    }
}

Model Model::load(const std::filesystem::path& path) {
    const auto data = parse(path);
    Model model;
    ImageCache images{path, {}};
    model.nodes.resize(data->nodes_count);
    for (std::size_t n = 0; n < data->nodes_count; ++n) {
        const auto& source = data->nodes[n];
        auto& node = model.nodes[n];
        node.name = source.name ? source.name : "node" + std::to_string(n);
        node.parent = source.parent ? static_cast<int>(source.parent - data->nodes) : -1;
        node.properties = nodeProperties(source);
        if (source.has_matrix) {
            const auto matrix = glm::make_mat4(source.matrix);
            const glm::vec3 c0(matrix[0]), c1(matrix[1]), c2(matrix[2]);
            node.translation = glm::vec3(matrix[3]);
            node.scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
            // A mirrored node (negative determinant) keeps its mirroring as a
            // negative x scale, so the rotation below is a proper rotation.
            if (glm::dot(c0, glm::cross(c1, c2)) < 0) {
                node.scale.x = -node.scale.x;
            }
            node.rotation =
                glm::quat_cast(glm::mat3(c0 / node.scale.x, c1 / node.scale.y, c2 / node.scale.z));
        } else {
            if (source.has_translation) {
                node.translation = glm::make_vec3(source.translation);
            }
            if (source.has_rotation) {
                node.rotation =
                    glm::quat(source.rotation[3], source.rotation[0], source.rotation[1], source.rotation[2]);
            }
            if (source.has_scale) {
                node.scale = glm::make_vec3(source.scale);
            }
        }
        if (source.mesh) {
            const int skin = source.skin ? static_cast<int>(source.skin - data->skins) : -1;
            for (std::size_t p = 0; p < source.mesh->primitives_count; ++p) {
                auto part = readPrimitive(source.mesh->primitives[p], images, node.properties);
                part.node = static_cast<int>(n);
                if (skin >= 0 && !part.joints.empty()) {
                    const auto jointCount = source.skin->joints_count;
                    for (const auto& indices : part.joints) {
                        for (const auto index : indices) {
                            if (index >= jointCount) {
                                throw std::runtime_error("A glTF vertex names a joint outside its skin: " +
                                                         path.string());
                            }
                        }
                    }
                    part.skin = skin;
                } else {
                    // Not skinned, or a skinned node whose mesh carries no
                    // weights: drawn rigidly with its node.
                    part.joints.clear();
                    part.weights.clear();
                }
                model.parts.push_back(std::move(part));
            }
        }
    }
    model.skins.resize(data->skins_count);
    for (std::size_t k = 0; k < data->skins_count; ++k) {
        const auto& source = data->skins[k];
        auto& skin = model.skins[k];
        if (source.joints_count == 0 || source.joints_count > 1024) {
            throw std::runtime_error("A glTF skin needs 1 to 1024 joints: " + path.string());
        }
        if (source.inverse_bind_matrices && (source.inverse_bind_matrices->type != cgltf_type_mat4 ||
                                             source.inverse_bind_matrices->count < source.joints_count)) {
            throw std::runtime_error("A glTF skin has invalid inverse bind matrices: " + path.string());
        }
        skin.joints.reserve(source.joints_count);
        skin.inverseBind.resize(source.joints_count, glm::mat4(1));
        for (std::size_t j = 0; j < source.joints_count; ++j) {
            skin.joints.push_back(static_cast<int>(source.joints[j] - data->nodes));
            if (source.inverse_bind_matrices) {
                std::array<float, 16> values{};
                readAccessor(source.inverse_bind_matrices, j, values.data(), 16);
                skin.inverseBind[j] = glm::make_mat4(values.data());
            }
        }
    }
    // Only the selected scene is shown; nodes outside it are hidden.
    std::function<void(int)> visit = [&](int n) {
        model.order.push_back(n);
        const auto& source = data->nodes[static_cast<std::size_t>(n)];
        for (std::size_t c = 0; c < source.children_count; ++c) {
            visit(static_cast<int>(source.children[c] - data->nodes));
        }
    };
    const auto* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
    if (scene) {
        for (std::size_t i = 0; i < scene->nodes_count; ++i) {
            visit(static_cast<int>(scene->nodes[i] - data->nodes));
        }
    } else {
        for (std::size_t i = 0; i < data->nodes_count; ++i) {
            if (!data->nodes[i].parent) {
                visit(static_cast<int>(i));
            }
        }
    }
    std::vector<bool> shown(model.nodes.size(), false);
    for (const int n : model.order) {
        shown[static_cast<std::size_t>(n)] = true;
    }
    for (auto& part : model.parts) {
        part.hidden = part.hidden || !shown[static_cast<std::size_t>(part.node)];
    }
    readAnimations(*data, model);
    model.bakeRestPose();
    const auto& matrices = model.restPose;
    model.min = glm::vec3(std::numeric_limits<float>::max());
    model.max = -model.min;
    for (const int n : model.order) {
        const auto& node = model.nodes[static_cast<std::size_t>(n)];
        ModelMarker marker;
        marker.name = node.name;
        marker.position = glm::vec3(matrices[static_cast<std::size_t>(n)][3]);
        marker.properties = node.properties;
        if (marker.properties.contains("light") && marker.properties["light"].value("type", "") == "spot") {
            // glTF lights shine down the node's local -Z axis.
            const auto forward = -glm::normalize(glm::vec3(matrices[static_cast<std::size_t>(n)][2]));
            marker.properties["light"]["direction"] = {forward.x, forward.y, forward.z};
        }
        marker.min = glm::vec3(std::numeric_limits<float>::max());
        marker.max = -marker.min;
        for (const auto& part : model.parts) {
            if (part.node != n || part.skin >= 0) {
                continue; // a skinned part is placed by its joints (below), not by this node
            }
            for (const auto& vertex : part.vertices) {
                const auto world =
                    glm::vec3(matrices[static_cast<std::size_t>(n)] * glm::vec4(vertex.position, 1));
                marker.min = glm::min(marker.min, world);
                marker.max = glm::max(marker.max, world);
                marker.hasBounds = true;
                if (!part.hidden) {
                    model.min = glm::min(model.min, world);
                    model.max = glm::max(model.max, world);
                }
            }
        }
        if (!marker.properties.empty()) {
            model.markers.push_back(std::move(marker));
        }
    }
    // Skinned parts: their bounds are those of the rest pose, in model space.
    std::vector<glm::mat4> joints;
    std::vector<MeshVertex> skinnedRest;
    for (auto& part : model.parts) {
        if (part.skin < 0) {
            continue;
        }
        model.jointMatrices(part.skin, matrices, joints);
        skinVertices(part, joints, skinnedRest);
        part.min = glm::vec3(std::numeric_limits<float>::max());
        part.max = -part.min;
        for (const auto& vertex : skinnedRest) {
            part.min = glm::min(part.min, vertex.position);
            part.max = glm::max(part.max, vertex.position);
        }
        if (!part.hidden && !skinnedRest.empty()) {
            model.min = glm::min(model.min, part.min);
            model.max = glm::max(model.max, part.max);
        }
    }
    if (model.min.x > model.max.x) {
        model.min = model.max = glm::vec3(0);
    }
    return model;
}

int Model::animation(const std::string& name) const {
    for (std::size_t i = 0; i < animations.size(); ++i) {
        if (animations[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

namespace {
glm::mat4 localMatrix(glm::vec3 translation, const glm::quat& rotation, glm::vec3 scale) {
    return glm::translate(glm::mat4(1), translation) * glm::mat4_cast(rotation) *
           glm::scale(glm::mat4(1), scale);
}
} // namespace

void Model::bakeRestPose() {
    restLocal.resize(nodes.size());
    for (std::size_t n = 0; n < nodes.size(); ++n) {
        restLocal[n] = localMatrix(nodes[n].translation, nodes[n].rotation, nodes[n].scale);
    }
    restPose.clear();
    pose(-1, 0, restPose);
}

void Model::pose(int index, float time, std::vector<glm::mat4>& matrices) const {
    const bool animated = index >= 0 && index < static_cast<int>(animations.size());
    if (!animated && restPose.size() == nodes.size()) {
        matrices = restPose;
        return;
    }
    // Local matrices first, then each node's world matrix in place: parents
    // come before children in `order`, so a parent is already world space.
    matrices.resize(nodes.size());
    if (restLocal.size() == nodes.size()) {
        std::copy(restLocal.begin(), restLocal.end(), matrices.begin());
    } else {
        for (std::size_t n = 0; n < nodes.size(); ++n) {
            matrices[n] = localMatrix(nodes[n].translation, nodes[n].rotation, nodes[n].scale);
        }
    }
    if (animated) {
        const auto& channels = animations[static_cast<std::size_t>(index)].channels;
        for (std::size_t c = 0; c < channels.size();) {
            const int n = channels[c].node;
            const auto& node = nodes[static_cast<std::size_t>(n)];
            glm::vec3 translation = node.translation, scale = node.scale;
            glm::quat rotation = node.rotation;
            for (; c < channels.size() && channels[c].node == n; ++c) {
                const auto value = sampleChannel(channels[c], time);
                switch (channels[c].target) {
                case AnimationChannel::Target::Translation:
                    translation = glm::vec3(value);
                    break;
                case AnimationChannel::Target::Rotation:
                    rotation = glm::quat(value.w, value.x, value.y, value.z);
                    break;
                case AnimationChannel::Target::Scale:
                    scale = glm::vec3(value);
                    break;
                }
            }
            matrices[static_cast<std::size_t>(n)] = localMatrix(translation, rotation, scale);
        }
    }
    for (const int n : order) {
        const auto i = static_cast<std::size_t>(n);
        const int parent = nodes[i].parent;
        if (parent >= 0) {
            matrices[i] = matrices[static_cast<std::size_t>(parent)] * matrices[i];
        }
    }
}

void Model::sample(int index, float time, std::vector<NodePose>& locals) const {
    locals.resize(nodes.size());
    for (std::size_t n = 0; n < nodes.size(); ++n) {
        locals[n] = {nodes[n].translation, nodes[n].rotation, nodes[n].scale};
    }
    if (index < 0 || index >= static_cast<int>(animations.size())) {
        return;
    }
    for (const auto& channel : animations[static_cast<std::size_t>(index)].channels) {
        const auto value = sampleChannel(channel, time);
        auto& local = locals[static_cast<std::size_t>(channel.node)];
        switch (channel.target) {
        case AnimationChannel::Target::Translation:
            local.translation = glm::vec3(value);
            break;
        case AnimationChannel::Target::Rotation:
            local.rotation = glm::quat(value.w, value.x, value.y, value.z);
            break;
        case AnimationChannel::Target::Scale:
            local.scale = glm::vec3(value);
            break;
        }
    }
}

void Model::compose(const std::vector<NodePose>& locals, std::vector<glm::mat4>& matrices) const {
    matrices.resize(nodes.size());
    for (std::size_t n = 0; n < nodes.size() && n < locals.size(); ++n) {
        matrices[n] = localMatrix(locals[n].translation, locals[n].rotation, locals[n].scale);
    }
    for (const int n : order) {
        const auto i = static_cast<std::size_t>(n);
        const int parent = nodes[i].parent;
        if (parent >= 0) {
            matrices[i] = matrices[static_cast<std::size_t>(parent)] * matrices[i];
        }
    }
}

void Model::blendPoses(std::vector<NodePose>& into, const std::vector<NodePose>& other, float weight) {
    const float w = std::clamp(weight, 0.0f, 1.0f);
    for (std::size_t n = 0; n < into.size() && n < other.size(); ++n) {
        into[n].translation = glm::mix(into[n].translation, other[n].translation, w);
        into[n].scale = glm::mix(into[n].scale, other[n].scale, w);
        into[n].rotation = glm::normalize(glm::slerp(into[n].rotation, other[n].rotation, w));
    }
}

void Model::jointMatrices(int skin, const std::vector<glm::mat4>& pose,
                          std::vector<glm::mat4>& matrices) const {
    const auto& source = skins.at(static_cast<std::size_t>(skin));
    matrices.resize(source.joints.size());
    for (std::size_t j = 0; j < source.joints.size(); ++j) {
        matrices[j] = pose[static_cast<std::size_t>(source.joints[j])] * source.inverseBind[j];
    }
}

void skinVertices(const ModelPart& part, const std::vector<glm::mat4>& joints, std::vector<MeshVertex>& out) {
    out.resize(part.vertices.size());
    for (std::size_t v = 0; v < part.vertices.size(); ++v) {
        const auto& source = part.vertices[v];
        const auto& index = part.joints[v];
        const auto& weight = part.weights[v];
        // Linear blend skinning: one matrix per vertex, the weighted sum of its joints'.
        glm::mat4 blended = joints[index[0]] * weight[0];
        for (int k = 1; k < 4; ++k) {
            if (weight[k] > 0) {
                blended += joints[index[static_cast<std::size_t>(k)]] * weight[k];
            }
        }
        const glm::mat3 linear(blended);
        auto& vertex = out[v];
        vertex = source;
        vertex.position = glm::vec3(blended * glm::vec4(source.position, 1));
        const auto normal = linear * source.normal;
        const auto tangent = linear * glm::vec3(source.tangent);
        vertex.normal = glm::dot(normal, normal) > 1e-12f ? glm::normalize(normal) : source.normal;
        vertex.tangent = glm::vec4(glm::dot(tangent, tangent) > 1e-12f ? glm::normalize(tangent)
                                                                       : glm::vec3(source.tangent),
                                   source.tangent.w);
    }
}

bool sameMaterial(const Material& a, const Material& b) {
    const auto sameSampler = [](const TextureSampler& x, const TextureSampler& y) {
        return x.wrapU == y.wrapU && x.wrapV == y.wrapV && x.nearestMag == y.nearestMag &&
               x.nearestMin == y.nearestMin && x.mips == y.mips;
    };
    // A sampler only matters where there is an image to sample.
    const auto sameTexture = [&](const ModelImage& x, const TextureSampler& xs, const ModelImage& y,
                                 const TextureSampler& ys) {
        return x.image == y.image && (!x.image || sameSampler(xs, ys));
    };
    return a.color == b.color && a.emissive == b.emissive &&
           sameTexture(a.albedo, a.albedoSampler, b.albedo, b.albedoSampler) &&
           sameTexture(a.normal, a.normalSampler, b.normal, b.normalSampler) &&
           sameTexture(a.emission, a.emissionSampler, b.emission, b.emissionSampler) &&
           a.normalScale == b.normalScale && a.transparent == b.transparent &&
           a.doubleSided == b.doubleSided && a.alphaCutoff == b.alphaCutoff && a.kind == b.kind &&
           a.shader == b.shader;
}

std::vector<BakedMesh> bakeGeometry(const Model& model) {
    std::vector<BakedMesh> meshes;
    std::vector<glm::mat4> rest, joints;
    model.pose(-1, 0, rest);
    std::vector<MeshVertex> skinned;
    for (const auto& part : model.parts) {
        if (part.hidden || part.indices.empty()) {
            continue;
        }
        // Water blends like a transparent material whatever its alpha mode says.
        const bool sorted = part.material.transparent || part.material.kind == MaterialKind::Water;
        auto target = meshes.end();
        if (!sorted) {
            target = std::find_if(meshes.begin(), meshes.end(), [&](const BakedMesh& mesh) {
                return sameMaterial(*mesh.material, part.material);
            });
        }
        if (target == meshes.end()) {
            meshes.push_back({&part.material, {}, {}});
            target = meshes.end() - 1;
        }
        auto& mesh = *target;
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        bool mirrored = false;
        if (part.skin >= 0) {
            model.jointMatrices(part.skin, rest, joints);
            skinVertices(part, joints, skinned);
            mesh.vertices.insert(mesh.vertices.end(), skinned.begin(), skinned.end());
        } else {
            const auto& matrix = rest[static_cast<std::size_t>(part.node)];
            const glm::mat3 linear(matrix);
            const auto normalMatrix = glm::transpose(glm::inverse(linear));
            mirrored = glm::determinant(linear) < 0;
            mesh.vertices.reserve(mesh.vertices.size() + part.vertices.size());
            for (auto v : part.vertices) {
                v.position = glm::vec3(matrix * glm::vec4(v.position, 1));
                v.normal = glm::normalize(normalMatrix * v.normal);
                v.tangent = glm::vec4(glm::normalize(linear * glm::vec3(v.tangent)), v.tangent.w);
                mesh.vertices.push_back(v);
            }
        }
        mesh.indices.reserve(mesh.indices.size() + part.indices.size());
        for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
            mesh.indices.push_back(base + part.indices[i]);
            mesh.indices.push_back(base + part.indices[i + (mirrored ? 2 : 1)]);
            mesh.indices.push_back(base + part.indices[i + (mirrored ? 1 : 2)]);
        }
    }
    return meshes;
}
} // namespace fe
