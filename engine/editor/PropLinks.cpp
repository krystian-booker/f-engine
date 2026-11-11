#include "engine/editor/PropLinks.h"

namespace fe {
namespace {
std::string specType(const Json& spec) {
    if (!spec.is_object()) {
        return {};
    }
    const auto type = spec.find("type");
    return type != spec.end() && type->is_string() ? type->get<std::string>() : std::string();
}

void collect(const Json& spec, const Json& value, const std::string& path, const std::string& key, int depth,
             std::vector<PropLink>& out) {
    if (depth > 6) {
        return;
    }
    const auto type = specType(spec);
    if ((type == "module" || type == "map_id") && value.is_string() && !value.get<std::string>().empty()) {
        PropLink link;
        link.kind = type == "module" ? PropLink::Kind::Script : PropLink::Kind::Map;
        link.field = path;
        link.value = value.get<std::string>();
        const auto label = spec.find("label");
        link.label = label != spec.end() && label->is_string() ? label->get<std::string>() : key;
        out.push_back(std::move(link));
    } else if (type == "list" && value.is_array()) {
        const auto of = spec.find("of");
        if (of != spec.end()) {
            for (std::size_t i = 0; i < value.size(); ++i) {
                collect(*of, value[i], path + "/" + std::to_string(i + 1), key, depth + 1, out);
            }
        }
    } else if (type == "record" && value.is_object()) {
        const auto fields = spec.find("fields");
        if (fields != spec.end() && fields->is_object()) {
            for (const auto& [name, child] : fields->items()) {
                if (const auto found = value.find(name); found != value.end()) {
                    collect(child, *found, path + "/" + name, name, depth + 1, out);
                }
            }
        }
    } else if (type == "map" && value.is_object()) {
        const auto of = spec.find("of");
        if (of != spec.end()) {
            for (const auto& [name, child] : value.items()) {
                collect(*of, child, path + "/" + name, name, depth + 1, out);
            }
        }
    }
}
} // namespace

std::vector<PropLink> propLinks(const Json* fields, const Json& props) {
    std::vector<PropLink> out;
    if (!fields || !fields->is_object() || !props.is_object()) {
        return out;
    }
    for (const auto& [name, spec] : fields->items()) {
        if (const auto found = props.find(name); found != props.end()) {
            collect(spec, *found, name, name, 0, out);
        }
    }
    return out;
}

const Json* typeFields(const Json& schemas, const char* group, const std::string& type) {
    if (!schemas.is_object()) {
        return nullptr;
    }
    const auto types = schemas.find(group);
    if (types == schemas.end() || !types->is_object()) {
        return nullptr;
    }
    const auto spec = types->find(type);
    if (spec == types->end() || !spec->is_object()) {
        return nullptr;
    }
    const auto fields = spec->find("fields");
    return fields != spec->end() && fields->is_object() ? &*fields : nullptr;
}

const Json& defaultSchemas() {
    static const Json schemas = Json::parse(R"json({
      "entities": {
        "object": {"fields": {
          "model": {"type": "asset", "asset": "model", "optional": true},
          "sprite": {"type": "asset", "asset": "sprite", "optional": true},
          "solid": {"type": "boolean", "default": true},
          "script": {"type": "module", "folder": "", "optional": true}
        }}
      },
      "zones": {
        "area": {"fields": {
          "script": {"type": "module", "folder": "", "optional": true}
        }}
      },
      "defaults": {"entities": "object", "zones": "area"}
    })json");
    return schemas;
}
} // namespace fe
