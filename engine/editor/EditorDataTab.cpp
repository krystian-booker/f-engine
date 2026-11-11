#include "engine/core/Paths.h"
#include "engine/editor/Editor.h"
#include "engine/editor/EditorWidgets.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <imgui.h>
#include <algorithm>
#include <imgui_stdlib.h>

namespace fe {
using namespace editorui;

void Editor::saveTable(const Json& table) {
    const auto name = jsonString(table, "name", {});
    const auto file = jsonString(table, "file", {});
    if (file.empty()) {
        status_ = "Not saved: the table '" + name + "' names no file";
        return;
    }
    dataProblems_.clear();
    auto& lua = engine_.lua();
    // The game's own check (its editor.validate_table hook) when it has one.
    if (const auto validate = devtool("validate_table"); validate != sol::lua_nil) {
        const auto result = lua.invoke(validate, name, jsonToLua(lua.lua(), dataDocument_));
        if (!succeeded(result)) {
            return;
        }
        if (result.return_count() > 0 && !result.get<bool>(0)) {
            dataProblems_.push_back(result.return_count() > 1 ? result.get<std::string>(1)
                                                              : "Validation failed");
        }
    } else {
        // No game check: the one rule the editor itself relies on.
        for (std::size_t i = 0; i < dataDocument_.size(); ++i) {
            if (jsonString(dataDocument_[i], "id", {}).empty()) {
                dataProblems_.push_back("record " + std::to_string(i + 1) + ": every record needs a string id");
            }
        }
    }
    if (!dataProblems_.empty()) {
        status_ = "Not saved: " + std::to_string(dataProblems_.size()) + " validation problem(s)";
        return;
    }
    writeJson(resolveInside(lua.gameRoot(), file), dataDocument_);
    status_ = "Saved " + file + " (hot reloads)";
}

void Editor::drawDataTab() {
    if (ImGui::Button("Refresh schemas")) {
        refreshSchemas();
    }
    const auto tables = jsonArray(schemas_, "tables");
    if (ImGui::BeginCombo("Table", dataTable_.c_str())) {
        for (const auto& t : tables) {
            const auto name = jsonString(t, "name", {});
            const auto file = jsonString(t, "file", {});
            if (name.empty() || file.empty()) {
                continue;
            }
            if (ImGui::Selectable(name.c_str(), name == dataTable_)) {
                dataTable_ = name;
                dataRecord_ = -1;
                dataProblems_.clear();
                try {
                    dataDocument_ = readJson(resolveInside(engine_.lua().gameRoot(), file));
                } catch (const std::exception& error) {
                    dataDocument_ = Json();
                    status_ = std::string("Cannot read ") + file + ": " + error.what();
                }
            }
        }
        ImGui::EndCombo();
    }
    const auto table = std::find_if(tables.begin(), tables.end(),
                                    [&](const Json& t) { return jsonString(t, "name", {}) == dataTable_; });
    if (table == tables.end() || !dataDocument_.is_array()) {
        ImGui::TextDisabled("Pick a table (array files are editable here)");
        return;
    }
    ImGui::BeginChild("records", ImVec2(0, 160), ImGuiChildFlags_Borders);
    for (std::size_t i = 0; i < dataDocument_.size(); ++i) {
        // A record whose id is not a string (or missing) is listed by its index.
        const auto id = jsonString(dataDocument_[i], "id", std::to_string(i));
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(id.c_str(), dataRecord_ == static_cast<int>(i))) {
            dataRecord_ = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (ImGui::Button("Add record")) {
        dataDocument_.push_back({{"id", "new_" + std::to_string(dataDocument_.size())}});
        dataRecord_ = static_cast<int>(dataDocument_.size()) - 1;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(dataRecord_ < 0 || dataRecord_ >= static_cast<int>(dataDocument_.size()));
    if (ImGui::Button("Delete record")) {
        dataDocument_.erase(static_cast<std::size_t>(dataRecord_));
        dataRecord_ = -1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Validate and save")) {
        saveTable(*table);
    }
    for (const auto& problem : dataProblems_) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", problem.c_str());
    }
    if (dataRecord_ >= 0 && dataRecord_ < static_cast<int>(dataDocument_.size())) {
        auto& record = dataDocument_[static_cast<std::size_t>(dataRecord_)];
        const auto schema = jsonString(*table, "schema", {});
        if (!record.is_object() || schema.empty()) {
            editJson("Record", record, 240);
        } else {
            editProps(fieldsFor(schema, {"schemas"}), record);
        }
    }
}
} // namespace fe
