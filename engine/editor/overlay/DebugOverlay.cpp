#include "engine/editor/overlay/DebugOverlay.h"
#include "engine/core/Log.h"
#include "engine/editor/MapEdits.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/ComponentInfo.h"
#include "engine/world/World.h"
#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fe {
namespace {
using editorui::jsonNumber;
using editorui::jsonString;

// A function of the `editor` table of the game's entry module, or nil.
sol::object hook(Engine& engine, const char* name) {
    if (!engine.hasMain()) {
        return sol::lua_nil;
    }
    try {
        const auto module = engine.lua().require(engine.config().entry);
        if (module.get_type() == sol::type::table) {
            const auto hooks = module.as<sol::table>().raw_get<sol::object>("editor");
            if (hooks.get_type() == sol::type::table) {
                sol::object found = hooks.as<sol::table>().raw_get<sol::object>(name);
                if (found.get_type() == sol::type::function) {
                    return found;
                }
            }
        }
    } catch (const std::exception&) {
        // The entry module failed to load; the error popup already says so.
    }
    return sol::lua_nil;
}

struct HistoryState {
    std::vector<std::string>* history;
    int* position;
};
int consoleCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory) {
        return 0;
    }
    auto& state = *static_cast<HistoryState*>(data->UserData);
    const int count = static_cast<int>(state.history->size());
    if (count == 0) {
        return 0;
    }
    int& position = *state.position;
    if (data->EventKey == ImGuiKey_UpArrow) {
        position = position < 0 ? count - 1 : std::max(0, position - 1);
    } else if (data->EventKey == ImGuiKey_DownArrow && position >= 0) {
        position = position + 1 < count ? position + 1 : -1;
    }
    data->DeleteChars(0, data->BufTextLen);
    if (position >= 0) {
        data->InsertChars(0, (*state.history)[static_cast<std::size_t>(position)].c_str());
    }
    return 0;
}

// Widgets for one JSON value by its own type; true when the user changed it.
bool editValue(const char* label, Json& value, bool readOnly) {
    ImGui::PushID(label);
    ImGui::BeginDisabled(readOnly);
    bool changed = false;
    if (value.is_boolean()) {
        bool on = value.get<bool>();
        if (ImGui::Checkbox(label, &on)) {
            value = on;
            changed = true;
        }
    } else if (value.is_number_integer()) {
        int number = static_cast<int>(value.get<long long>());
        if (ImGui::InputInt(label, &number, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
            value = number;
            changed = true;
        }
    } else if (value.is_number()) {
        double number = value.get<double>();
        if (ImGui::InputDouble(label, &number, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            value = number;
            changed = true;
        }
    } else if (value.is_string()) {
        auto text = value.get<std::string>();
        if (ImGui::InputText(label, &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
            value = text;
            changed = true;
        }
    } else if (value.is_array() && !value.empty() && value.size() <= 4 &&
               std::all_of(value.begin(), value.end(), [](const Json& v) { return v.is_number(); })) {
        float numbers[4] = {};
        for (std::size_t i = 0; i < value.size(); ++i) {
            numbers[i] = value[i].get<float>();
        }
        if (ImGui::InputScalarN(label, ImGuiDataType_Float, numbers, static_cast<int>(value.size()), nullptr,
                                nullptr, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            for (std::size_t i = 0; i < value.size(); ++i) {
                value[i] = numbers[i];
            }
            changed = true;
        }
    } else if (value.is_object() || value.is_array()) {
        if (ImGui::TreeNode(label)) {
            if (value.is_object()) {
                for (auto field = value.begin(); field != value.end(); ++field) {
                    changed = editValue(field.key().c_str(), *field, readOnly) || changed;
                }
            } else {
                for (std::size_t i = 0; i < value.size(); ++i) {
                    changed = editValue(std::to_string(i + 1).c_str(), value[i], readOnly) || changed;
                }
            }
            ImGui::TreePop();
        }
    } else {
        ImGui::TextDisabled("%s: %s", label, value.dump().c_str());
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}
} // namespace

DebugOverlay::DebugOverlay(Engine& engine) : engine_(engine) {
}

// ImGui outlives the overlay (the renderer owns it) and writes its settings
// when it goes: they are written now, and ImGui forgets the path, which
// points into this object.
DebugOverlay::~DebugOverlay() {
    if (!layoutFile_.empty() && ImGui::GetCurrentContext()) {
        ImGui::SaveIniSettingsToDisk(layoutFile_.c_str());
        ImGui::GetIO().IniFilename = nullptr;
    }
}

void DebugOverlay::setLayoutFile(const std::filesystem::path& file) {
    layoutFile_ = file.string();
    ImGui::GetIO().IniFilename = layoutFile_.c_str();
}

bool DebugOverlay::capturesInput() const {
    if (!open_) {
        return false;
    }
    const auto& io = ImGui::GetIO();
    return io.WantCaptureKeyboard || io.WantCaptureMouse;
}

bool DebugOverlay::takeStep() {
    const bool step = step_;
    step_ = false;
    return step;
}

void DebugOverlay::drawErrors() {
    const auto& errors = engine_.lua().errors();
    if (errors.empty() || errors.size() == errorsSeen_) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620, 220), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Script error")) {
        ImGui::TextWrapped("%s", errors.back().message.c_str());
        if (ImGui::Button("Dismiss")) {
            errorsSeen_ = errors.size();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) {
            engine_.lua().clearErrors();
            errorsSeen_ = 0;
        }
        ImGui::SameLine();
        ImGui::Text("%zu error(s)", errors.size());
    }
    ImGui::End();
}

void DebugOverlay::draw(double frameMilliseconds, std::uint32_t drawCalls) {
    if (!open_) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(10, 240), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug (F1)", &open_)) {
        if (ImGui::BeginTabBar("debug")) {
            if (ImGui::BeginTabItem("Entities")) {
                drawEntities();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Console")) {
                drawConsole();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Profiler")) {
                drawProfiler(frameMilliseconds, drawCalls);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tasks")) {
                drawTasks();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Launch")) {
                drawLaunch();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// Every named entity of a world; the chosen one with all its components.
void DebugOverlay::drawEntities() {
    const auto& worlds = engine_.worlds();
    if (worlds.empty()) {
        ImGui::TextDisabled("The game has no world loaded");
        return;
    }
    if (!worlds.contains(worldName_)) {
        const auto focus = engine_.focusWorld();
        worldName_ = focus ? focus->name() : worlds.begin()->first;
    }
    if (ImGui::BeginCombo("World", worldName_.c_str())) {
        for (const auto& [name, world] : worlds) {
            if (ImGui::Selectable(name.c_str(), name == worldName_)) {
                worldName_ = name;
                selected_ = {};
            }
        }
        ImGui::EndCombo();
    }
    auto& world = *worlds.at(worldName_);
    ImGui::InputTextWithHint("##filter", "Filter by name", &filter_);
    ImGui::BeginChild("list", ImVec2(0, 150), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    for (const auto entity : world.registry().view<const Name>()) {
        const auto& id = world.registry().get<const Name>(entity).id;
        if (!filter_.empty() && id.find(filter_) == std::string::npos) {
            continue;
        }
        const auto row = Selection::ofEntity(world, entity);
        ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
        if (ImGui::Selectable(id.empty() ? "(unnamed)" : id.c_str(), row == selected_)) {
            selected_ = row;
            problem_.clear();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (selected_.kind != Selection::Kind::Entity || !world.alive(selected_.entity, selected_.serial)) {
        ImGui::TextDisabled("Select an entity to see its components");
        return;
    }
    if (!problem_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.45f, 0.3f, 1), "%s", problem_.c_str());
    }
    ImGui::TextDisabled("Enter applies an edited value");
    for (const auto* info : componentsOf(world, selected_.entity)) {
        if (!ImGui::CollapsingHeader(info->name, ImGuiTreeNodeFlags_DefaultOpen)) {
            continue;
        }
        ImGui::PushID(info->name);
        Json value = info->read(world, selected_.entity);
        bool changed = false;
        if (value.is_object()) {
            for (auto field = value.begin(); field != value.end(); ++field) {
                const bool readOnly = std::find(info->readOnly.begin(), info->readOnly.end(), field.key()) !=
                                      info->readOnly.end();
                changed = editValue(field.key().c_str(), *field, readOnly) || changed;
            }
        }
        if (changed) {
            problem_ = info->write(world, selected_.entity, value);
        }
        ImGui::PopID();
    }
}

void DebugOverlay::drawConsole() {
    ImGui::BeginChild("log", ImVec2(0, -30), ImGuiChildFlags_Borders);
    for (const auto& line : consoleLog_) {
        ImGui::TextWrapped("%s", line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    HistoryState state{&consoleHistory_, &historyPosition_};
    if (ImGui::InputText("Lua", &consoleInput_,
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                         consoleCallback, &state)) {
        if (!consoleInput_.empty()) {
            consoleLog_.push_back("> " + consoleInput_);
            const auto result = engine_.lua().evaluate(consoleInput_);
            consoleLog_.push_back(result.ok ? result.output : "error: " + result.output);
            if (consoleHistory_.empty() || consoleHistory_.back() != consoleInput_) {
                consoleHistory_.push_back(consoleInput_);
            }
        }
        consoleInput_.clear();
        historyPosition_ = -1;
        ImGui::SetKeyboardFocusHere(-1);
    }
}

void DebugOverlay::drawProfiler(double frameMilliseconds, std::uint32_t drawCalls) {
    ImGui::Checkbox("Pause the game", &paused_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!paused_);
    if (ImGui::Button("Step one tick")) {
        step_ = true;
    }
    ImGui::EndDisabled();
    const auto& stats = engine_.stats();
    const auto now = Clock::now();
    if (now - polled_ > std::chrono::milliseconds(500)) {
        polled_ = now;
        taskCount_ = "-";
        if (const auto count = hook(engine_, "task_count"); count != sol::lua_nil) {
            const auto result = engine_.lua().invoke(count);
            if (result.valid() && result.get_type() == sol::type::number) {
                taskCount_ = std::to_string(result.get<long long>());
            }
        }
    }
    ImGui::Text("Frame %.2f ms (%.0f fps)", frameMilliseconds,
                frameMilliseconds > 0 ? 1000.0 / frameMilliseconds : 0.0);
    ImGui::Text("Lua update %.2f ms, draw %.2f ms", stats.luaUpdate, stats.luaDraw);
    ImGui::Text("World draw calls %u", drawCalls);
    ImGui::Text("Canvas quads %zu", engine_.canvas().quads().size());
    ImGui::Text("Tasks %s, Lua memory %zu KB", taskCount_.c_str(), engine_.lua().lua().memory_used() / 1024);
    for (const auto& [name, w] : engine_.worlds()) {
        ImGui::Text("World %s: %zu entities, %zu particles, %zu lights", name.c_str(),
                    w->registry().view<Transform>().size(), w->particles().size(), w->activeLights().size());
    }
    if (!stats.zones.empty()) {
        ImGui::SeparatorText("Script zones");
        for (const auto& [name, ms] : stats.zones) {
            ImGui::Text("%s: %.3f ms", name.c_str(), ms);
        }
    }
}

// The coroutines the game says it is running (its editor.tasks hook).
void DebugOverlay::drawTasks() {
    const auto list = hook(engine_, "tasks");
    if (list == sol::lua_nil) {
        ImGui::TextDisabled("Tasks come from the editor.tasks() hook of the game's entry module");
        return;
    }
    const auto result = engine_.lua().invoke(list);
    if (result.valid() && result.get_type() == sol::type::string) {
        const auto parsed = Json::parse(result.get<std::string>(), nullptr, false);
        tasks_ = parsed.is_array() ? parsed : Json::array();
    }
    if (tasks_.empty()) {
        ImGui::TextDisabled("No task is running");
    }
    for (const auto& task : tasks_) {
        const int index = static_cast<int>(jsonNumber(task, "index", 0));
        ImGui::PushID(index);
        if (ImGui::SmallButton("Cancel")) {
            if (const auto cancel = hook(engine_, "cancel_task"); cancel != sol::lua_nil) {
                (void)engine_.lua().invoke(cancel, index);
            }
        }
        ImGui::SameLine();
        ImGui::Text("%s  %s  %.1fs", jsonString(task, "name", "").c_str(),
                    jsonString(task, "status", "").c_str(), jsonNumber(task, "age", 0));
        ImGui::PopID();
    }
}

// Jumps straight into a situation (the game's editor.list and editor.launch).
void DebugOverlay::drawLaunch() {
    const auto now = Clock::now();
    static Clock::time_point listed{};
    if (ImGui::Button("Refresh") || now - listed > std::chrono::seconds(2)) {
        listed = now;
        launchers_.clear();
        if (const auto list = hook(engine_, "list"); list != sol::lua_nil) {
            const auto result = engine_.lua().invoke(list);
            if (result.valid() && result.get_type() == sol::type::table) {
                for (const auto& [index, name] : result.get<sol::table>()) {
                    if (name.is<std::string>()) {
                        launchers_.push_back(name.as<std::string>());
                    }
                }
            }
        }
    }
    for (const auto& name : launchers_) {
        if (ImGui::Button(name.c_str(), ImVec2(-1, 0))) {
            if (const auto launch = hook(engine_, "launch"); launch != sol::lua_nil) {
                const auto result = engine_.lua().invoke(launch, name);
                if (result.valid()) {
                    paused_ = false;
                } else {
                    const sol::error error = result;
                    log(LogLevel::Error, std::string("editor.launch: ") + error.what());
                }
            }
        }
    }
    if (launchers_.empty()) {
        ImGui::TextDisabled(
            "Launchers come from the editor.list() and editor.launch(name) hooks of main.lua");
    }
}
} // namespace fe
