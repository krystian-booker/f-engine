#pragma once
#include "engine/core/Json.h"
#include <map>
#include <string>
#include <vector>

namespace fe {
struct SequenceEvent { double time=0; std::string command; Json parameters; };
struct ActionTimeline {
    std::vector<SequenceEvent> events;
    double duration=0;
    static std::map<std::string,ActionTimeline> load(const std::filesystem::path& file);
};
class ActionSequence {
public:
    void start(ActionTimeline timeline);
    std::vector<SequenceEvent> update(double dt);
    [[nodiscard]] bool complete() const { return elapsed_>=timeline_.duration && next_>=timeline_.events.size(); }
    [[nodiscard]] double elapsed() const { return elapsed_; }
private:
    ActionTimeline timeline_;
    double elapsed_=0;
    std::size_t next_=0;
};
}
