#include "engine/core/GameModule.h"
#include "engine/core/Log.h"
#include "engine/scene/GameSession.h"
#include "engine/rpg/GameState.h"

namespace fe {
class LoggingModule final : public GameModule {
public:
    void onStart(GameSession& game) override {
        game.state().flags["native_module_loaded"]=true;
        log(LogLevel::Info,"Native game module started");
    }
    void onFixedUpdate(GameSession& game,double) override {
        game.state().variables["native_ticks"]+=1;
    }
    void onStop(GameSession&) override {log(LogLevel::Info,"Native game module stopped");}
};
std::unique_ptr<GameModule> createGameModule() {return std::make_unique<LoggingModule>();}
}
