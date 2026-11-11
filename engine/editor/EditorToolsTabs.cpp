#include "engine/editor/Editor.h"
#include "engine/runtime/Engine.h"
#include "engine/script/LuaHost.h"
#include "engine/world/World.h"
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fe {
namespace {
// Lets the console's Up/Down keys walk the command history.
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
} // namespace

void Editor::drawConsoleTab() {
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

void Editor::drawProfilerTab(double frameMilliseconds, std::uint32_t drawCalls) {
    const auto& stats = engine_.stats();
    const auto now = Clock::now();
    if (now - profiled_ > std::chrono::milliseconds(500)) {
        profiled_ = now;
        taskCount_ = "-";
        if (const auto count = devtool("task_count"); count != sol::lua_nil) {
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

void Editor::drawLaunchTab() {
    const auto now = Clock::now();
    if (ImGui::Button("Refresh") || now - launchersListed_ > std::chrono::seconds(2)) {
        launchersListed_ = now;
        launchers_.clear();
        if (const auto list = devtool("list"); list != sol::lua_nil) {
            const auto result = engine_.lua().invoke(list);
            if (succeeded(result) && result.get_type() == sol::type::table) {
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
            if (const auto launch = devtool("launch");
                launch != sol::lua_nil && succeeded(engine_.lua().invoke(launch, name))) {
                paused_ = false;
            }
        }
    }
    if (launchers_.empty()) {
        ImGui::TextDisabled(
            "Launchers come from the editor.list() and editor.launch(name) hooks of main.lua");
    }
}
} // namespace fe
