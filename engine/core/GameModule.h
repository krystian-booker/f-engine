#pragma once
#include <memory>

namespace fe {
class GameSession;
// Optional native extension. A normal game needs only data and Lua scripts.
class GameModule {
public:
    virtual ~GameModule()=default;
    virtual void onStart(GameSession&) {}
    virtual void onFixedUpdate(GameSession&,double) {}
    virtual void onStop(GameSession&) {}
};
// Implement this factory in a target named fengine_game_module and pass its
// directory with -DFENGINE_GAME_MODULE=/path/to/module at configure time.
std::unique_ptr<GameModule> createGameModule();
}
