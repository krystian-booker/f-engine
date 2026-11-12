#pragma once
#include "engine/core/Json.h"
#include <string>
#include <vector>

namespace fe {
// What an authored object's props link to. The editor knows no prop by name:
// a game's schemas (its editor.describe hook) say what its fields are, and a
// field typed `module` is a script, one typed `map_id` another map. Badges in
// the Scene, the Scripts panel and the asset catalog's "used by" and
// "missing" lists all come from here.
struct PropLink {
    enum class Kind { Script, Map };
    Kind kind = Kind::Script;
    std::string field; // where in the props: "script", "steps/2/script"
    std::string value; // the module name or map id; may name nothing that exists
    std::string label; // the field spec's `label`, else the field's own key
};

// The links of `props` under a type's field specs ({field: spec}, or null for
// none). Lists, records and maps of specs are followed.
[[nodiscard]] std::vector<PropLink> propLinks(const Json* fields, const Json& props);

// The {field: spec} object of `type` in a group of a description ("entities",
// "zones", "schemas"), or null.
[[nodiscard]] const Json* typeFields(const Json& schemas, const char* group, const std::string& type);

// What the editor works with while a game describes nothing: one placement
// type, "object" (a model or a sprite, solid or not, an optional script), and
// one zone type, "area" (an optional script). Data like any description, so
// a new project gets forms and pickers before it has written a hook.
[[nodiscard]] const Json& defaultSchemas();
} // namespace fe
