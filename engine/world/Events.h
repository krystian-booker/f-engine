#pragma once
#include "engine/nav/Grid.h"
#include <deque>
#include <entt/entt.hpp>
#include <string>
#include <utility>
#include <vector>

namespace fe {
struct WorldEvent {
    enum class Kind {
        ContactBegin,
        ContactEnd,
        Arrived,
        Blocked,
        Landed,
        JumpStart,
        ClimbStart,
        ClimbEnd,
        AnimationEvent,
        AnimationFinished,
        ZoneEnter,
        ZoneExit,
        CellEnter // a mover's cell (x, z, layer) changed; `cell` is the new one
    };
    WorldEvent() = default;
    WorldEvent(Kind kind, entt::entity a = entt::null, entt::entity b = entt::null, std::string name = {},
               GridPos cell = {})
        : kind(kind), a(a), b(b), name(std::move(name)), cell(cell) {
    }
    Kind kind{};
    entt::entity a = entt::null, b = entt::null;
    std::string name; // animation event name, clip, or zone id
    GridPos cell;     // CellEnter: the cell entered
};

// World events waiting for scripts. The queue is bounded: when nobody drains
// it, the oldest events are dropped once it holds Capacity events, with one
// warning per overflow.
class EventQueue {
public:
    static constexpr std::size_t Capacity = 4096;
    void push(WorldEvent event);
    std::vector<WorldEvent> take();
    [[nodiscard]] std::size_t size() const { return events_.size(); }
    [[nodiscard]] std::size_t dropped() const { return dropped_; }

private:
    std::deque<WorldEvent> events_;
    std::size_t dropped_ = 0; // since the last take()
};
} // namespace fe
