#include "engine/assets/Mesh.h"
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>

namespace fe {
Mesh Mesh::load(const std::filesystem::path& path) {
    cgltf_options options{}; cgltf_data* raw = nullptr;
    if (cgltf_parse_file(&options,path.string().c_str(),&raw) != cgltf_result_success)
        throw std::runtime_error("Cannot parse glTF " + path.string());
    std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data(raw,cgltf_free);
    if (cgltf_load_buffers(&options,data.get(),path.string().c_str()) != cgltf_result_success ||
        cgltf_validate(data.get()) != cgltf_result_success) throw std::runtime_error("Invalid glTF buffers: " + path.string());
    Mesh mesh; mesh.min = glm::vec3(std::numeric_limits<float>::max()); mesh.max = -mesh.min;
    std::map<const cgltf_image*,std::shared_ptr<Image>> images;
    auto image = [&](const cgltf_texture_view& view) -> std::shared_ptr<Image> {
        if (!view.texture || !view.texture->image) return {};
        const auto* source = view.texture->image;
        if (auto found = images.find(source); found != images.end()) return found->second;
        std::shared_ptr<Image> result;
        if (source->buffer_view) {
            const auto* bytes = cgltf_buffer_view_data(source->buffer_view);
            if (!bytes) throw std::runtime_error("Missing glTF embedded image buffer");
            result = std::make_shared<Image>(Image::decode({bytes,source->buffer_view->size}));
        } else if (source->uri && !std::string_view(source->uri).starts_with("data:"))
            result = std::make_shared<Image>(Image::load(path.parent_path()/source->uri));
        else throw std::runtime_error("glTF image must use a file URI or GLB buffer view");
        images[source] = result; return result;
    };
    // Traverse the selected scene only: unused export helper nodes stay hidden.
    std::vector<const cgltf_node*> nodes;
    std::function<void(const cgltf_node*)> visit = [&](const cgltf_node* n) {
        nodes.push_back(n); for (std::size_t i = 0; i < n->children_count; ++i) visit(n->children[i]);
    };
    const auto* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
    if (scene) for (std::size_t i = 0; i < scene->nodes_count; ++i) visit(scene->nodes[i]);
    else for (std::size_t i = 0; i < data->nodes_count; ++i) if (!data->nodes[i].parent) visit(&data->nodes[i]);
    for (const auto* node : nodes) {
        glm::mat4 transform(1); cgltf_node_transform_world(node,glm::value_ptr(transform));
        MeshMarker marker;marker.name=node->name?node->name:"node";marker.position=glm::vec3(transform*glm::vec4(0,0,0,1));
        marker.min=glm::vec3(std::numeric_limits<float>::max());marker.max=-marker.min;
        if (node->extras.data) {
            const auto properties=Json::parse(node->extras.data);
            if (properties.is_object()) marker.properties=properties.value("fengine",properties);
        }
        if (node->light) {
            const auto* light=node->light;Json config{{"color",{light->color[0],light->color[1],light->color[2]}},{"intensity",light->intensity},{"radius",light->range>0?light->range:5}};
            if (light->type==cgltf_light_type_directional) {
                const auto direction=glm::normalize(glm::vec3(transform*glm::vec4(0,0,-1,0)));
                config["type"]="directional";config["direction"]={direction.x,direction.y,direction.z};
            }
            marker.properties["light"]=std::move(config);
        }
        if (!node->mesh) {if (!marker.properties.empty()) mesh.markers.push_back(std::move(marker));continue;}
        if (node->skin) throw std::runtime_error("Skeletal glTF meshes are outside f-engine's static map format");
        const auto normalTransform = glm::inverseTranspose(glm::mat3(transform));
        for (std::size_t p = 0; p < node->mesh->primitives_count; ++p) {
            const auto& primitive = node->mesh->primitives[p];
            if (primitive.type != cgltf_primitive_type_triangles || primitive.has_draco_mesh_compression)
                throw std::runtime_error("Map glTF requires uncompressed triangles");
            const cgltf_accessor* positions=nullptr; const cgltf_accessor* normals=nullptr;
            const cgltf_accessor* uvs=nullptr; const cgltf_accessor* colors=nullptr;
            for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
                const auto& attr = primitive.attributes[a];
                if (attr.type == cgltf_attribute_type_position) positions = attr.data;
                if (attr.type == cgltf_attribute_type_normal) normals = attr.data;
                if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uvs = attr.data;
                if (attr.type == cgltf_attribute_type_color && attr.index == 0) colors = attr.data;
            }
            if (!positions || positions->count > 10000000) throw std::runtime_error("Missing/oversized glTF positions");
            if (positions->type!=cgltf_type_vec3 || (normals && (normals->type!=cgltf_type_vec3 || normals->count!=positions->count)) ||
                (uvs && (uvs->type!=cgltf_type_vec2 || uvs->count!=positions->count)) ||
                (colors && ((colors->type!=cgltf_type_vec3 && colors->type!=cgltf_type_vec4) || colors->count!=positions->count)))
                throw std::runtime_error("Invalid glTF vertex attribute type/count");
            MeshPart part; part.vertices.resize(positions->count);
            for (std::size_t v = 0; v < positions->count; ++v) {
                auto& vertex = part.vertices[v]; std::array<float,4> values{};
                cgltf_accessor_read_float(positions,v,values.data(),3);
                vertex.position = glm::vec3(transform*glm::vec4(values[0],values[1],values[2],1));
                if (normals) { cgltf_accessor_read_float(normals,v,values.data(),3); vertex.normal = glm::normalize(normalTransform*glm::vec3(values[0],values[1],values[2])); }
                if (uvs) { cgltf_accessor_read_float(uvs,v,values.data(),2); vertex.uv = {values[0],values[1]}; }
                if (colors) { values = {1,1,1,1}; cgltf_accessor_read_float(colors,v,values.data(),cgltf_num_components(colors->type)); vertex.color = glm::make_vec4(values.data()); }
                mesh.min = glm::min(mesh.min,vertex.position); mesh.max = glm::max(mesh.max,vertex.position);
                marker.min=glm::min(marker.min,vertex.position);marker.max=glm::max(marker.max,vertex.position);marker.hasBounds=true;
            }
            const std::size_t count = primitive.indices ? primitive.indices->count : positions->count;
            if (count % 3 != 0) throw std::runtime_error("Incomplete glTF triangle");
            part.indices.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const auto index = primitive.indices ? cgltf_accessor_read_index(primitive.indices,i) : i;
                if (index >= part.vertices.size()) throw std::runtime_error("glTF index outside vertex buffer");
                part.indices.push_back(static_cast<std::uint32_t>(index));
            }
            if (!normals) {
                for (auto& vertex : part.vertices) vertex.normal = glm::vec3(0);
                for (std::size_t i = 0; i < part.indices.size(); i += 3) {
                    auto& a = part.vertices[part.indices[i]]; auto& b = part.vertices[part.indices[i+1]]; auto& c = part.vertices[part.indices[i+2]];
                    const auto n = glm::cross(b.position-a.position,c.position-a.position);
                    a.normal += n; b.normal += n; c.normal += n;
                }
                for (auto& vertex : part.vertices) vertex.normal = glm::length(vertex.normal) > 0.00001f ? glm::normalize(vertex.normal) : glm::vec3(0,1,0);
            }
            if (const auto* material = primitive.material) {
                if (material->has_pbr_metallic_roughness) {
                    part.material.color = glm::make_vec4(material->pbr_metallic_roughness.base_color_factor);
                    part.material.albedo = image(material->pbr_metallic_roughness.base_color_texture);
                }
                part.material.normal = image(material->normal_texture);
                part.material.transparent = material->alpha_mode == cgltf_alpha_mode_blend;
                part.material.alphaCutoff = material->alpha_mode == cgltf_alpha_mode_mask ? material->alpha_cutoff : 0.01f;
                part.material.doubleSided = material->double_sided;
            }
            mesh.parts.push_back(std::move(part));
        }
        if (!marker.properties.empty()) mesh.markers.push_back(std::move(marker));
    }
    if (mesh.parts.empty()) throw std::runtime_error("glTF contains no visible map meshes: " + path.string());
    return mesh;
}
}
