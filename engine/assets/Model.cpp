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
            result.file = path.parent_path() / uri;
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
    material.alphaCutoff = source->alpha_mode == cgltf_alpha_mode_mask ? source->alpha_cutoff : 0.01f;
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
        }
    }
    if (!positions || positions->count > 10000000 || positions->type != cgltf_type_vec3) {
        throw std::runtime_error("Missing or invalid glTF positions");
    }
    const auto count = positions->count;
    if ((normals && normals->count != count) || (uvs && uvs->count != count) ||
        (colors && colors->count != count) || (tangents && tangents->count != count)) {
        throw std::runtime_error("glTF vertex attributes disagree in length");
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
            for (std::size_t k = 0; k < input->count; ++k) {
                float time = 0;
                readAccessor(input, k, &time, 1);
                out.times.push_back(time);
                std::array<float, 4> value{0, 0, 0, 1};
                // Cubic splines store in-tangent, value, out-tangent: keep the value.
                readAccessor(output, cubic ? k * 3 + 1 : k, value.data(), cgltf_num_components(output->type));
                out.values.emplace_back(value[0], value[1], value[2], value[3]);
            }
            if (!out.times.empty()) {
                animation.duration = std::max(animation.duration, out.times.back());
                animation.channels.push_back(std::move(out));
            }
        }
        model.animations.push_back(std::move(animation));
    }
}

glm::vec4 sample(const AnimationChannel& channel, float time) {
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
            node.translation = glm::vec3(matrix[3]);
            node.scale = {glm::length(glm::vec3(matrix[0])), glm::length(glm::vec3(matrix[1])),
                          glm::length(glm::vec3(matrix[2]))};
            node.rotation = glm::quat_cast(glm::mat3(glm::vec3(matrix[0]) / node.scale.x,
                                                     glm::vec3(matrix[1]) / node.scale.y,
                                                     glm::vec3(matrix[2]) / node.scale.z));
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
        if (source.skin) {
            throw std::runtime_error("Skinned glTF meshes are not supported; use sprites for characters: " +
                                     path.string());
        }
        if (source.mesh) {
            for (std::size_t p = 0; p < source.mesh->primitives_count; ++p) {
                auto part = readPrimitive(source.mesh->primitives[p], images, node.properties);
                part.node = static_cast<int>(n);
                model.parts.push_back(std::move(part));
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
    std::vector<glm::mat4> matrices;
    model.pose(-1, 0, matrices);
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
            if (part.node != n) {
                continue;
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

void Model::pose(int index, float time, std::vector<glm::mat4>& matrices) const {
    std::vector<glm::vec3> translation(nodes.size()), scale(nodes.size());
    std::vector<glm::quat> rotation(nodes.size());
    for (std::size_t n = 0; n < nodes.size(); ++n) {
        translation[n] = nodes[n].translation;
        rotation[n] = nodes[n].rotation;
        scale[n] = nodes[n].scale;
    }
    if (index >= 0 && index < static_cast<int>(animations.size())) {
        for (const auto& channel : animations[static_cast<std::size_t>(index)].channels) {
            const auto value = sample(channel, time);
            const auto n = static_cast<std::size_t>(channel.node);
            switch (channel.target) {
            case AnimationChannel::Target::Translation:
                translation[n] = glm::vec3(value);
                break;
            case AnimationChannel::Target::Rotation:
                rotation[n] = glm::quat(value.w, value.x, value.y, value.z);
                break;
            case AnimationChannel::Target::Scale:
                scale[n] = glm::vec3(value);
                break;
            }
        }
    }
    matrices.assign(nodes.size(), glm::mat4(1));
    for (const int n : order) {
        const auto i = static_cast<std::size_t>(n);
        const auto local = glm::translate(glm::mat4(1), translation[i]) * glm::mat4_cast(rotation[i]) *
                           glm::scale(glm::mat4(1), scale[i]);
        const int parent = nodes[i].parent;
        matrices[i] = parent >= 0 ? matrices[static_cast<std::size_t>(parent)] * local : local;
    }
}
} // namespace fe
