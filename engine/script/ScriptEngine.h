#pragma once
#include "engine/battle/Battle.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace fe {
struct ScriptRequest {
    std::uint64_t task = 0;
    std::string event;
    std::string command;
    Json arguments = Json::array();
};
struct ScriptResponse { bool complete = false; Json value; };
class ScriptEngine {
public:
    using Handler = std::function<ScriptResponse(const ScriptRequest&, double)>;
    ScriptEngine(std::filesystem::path gameRoot, GameState& state, const Database& database);
    ~ScriptEngine();
    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;
    void setHandler(Handler handler);
    std::uint64_t start(const std::string& file, const std::string& event = "", bool blocking = true);
    void update(double dt);
    void cancel(std::uint64_t id);
    void cancelAll();
    void cancelAllExcept(std::uint64_t id);
    [[nodiscard]] bool running(std::uint64_t id) const;
    [[nodiscard]] bool blocksField() const;
    [[nodiscard]] std::size_t taskCount() const;
    void loadRules(const std::string& file);
    [[nodiscard]] double formula(const std::string& name, const Battler& user, const Battler& target, int power);
    [[nodiscard]] BattleAction ai(const Battle& battle, int index);
    [[nodiscard]] std::vector<EffectDef> effect(const std::string& name,const Battler& user,const Battler& target,int power);
    [[nodiscard]] const std::string& lastError() const;
    bool executeConsole(const std::string& code);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
