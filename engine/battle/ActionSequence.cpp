#include "engine/battle/ActionSequence.h"
#include <cmath>
#include <set>
#include <stdexcept>

namespace fe {
std::map<std::string,ActionTimeline> ActionTimeline::load(const std::filesystem::path& file) {
    std::map<std::string,ActionTimeline> result;
    const auto source=readJson(file);
    const std::set<std::string> commands{"step","animate","effect","return","flash","sfx","shake"};
    for (const auto& [name,list] : source.items()) {
        ActionTimeline timeline; int effects=0;
        for (const auto& e : list) {
            SequenceEvent event; event.time=e.at("time"); event.command=e.at("command"); event.parameters=e;
            if (!std::isfinite(event.time) || event.time<0 || event.time>60 || !commands.contains(event.command) ||
                (!timeline.events.empty() && event.time<timeline.events.back().time)) throw std::runtime_error("Invalid action timeline " + name);
            effects+=event.command=="effect"; timeline.events.push_back(std::move(event));
        }
        if (timeline.events.empty() || effects!=1) throw std::runtime_error("Timeline must contain exactly one effect: " + name);
        timeline.duration=timeline.events.back().time+.15; result[name]=std::move(timeline);
    }
    return result;
}
void ActionSequence::start(ActionTimeline timeline) { timeline_=std::move(timeline);elapsed_=0;next_=0; }
std::vector<SequenceEvent> ActionSequence::update(double dt) {
    std::vector<SequenceEvent> result;
    if (!std::isfinite(dt) || dt<0) return result;
    elapsed_+=dt;
    while (next_<timeline_.events.size() && timeline_.events[next_].time<=elapsed_+1e-12) result.push_back(timeline_.events[next_++]);
    return result;
}
}
