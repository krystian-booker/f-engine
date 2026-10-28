#include "engine/world/World.h"
#include "engine/core/Log.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fe {
World::World(MapData map, GameState& state, const Database& db, ScriptEngine& scripts)
    : map_(std::move(map)), state_(state), db_(db), scripts_(scripts) { rebuildEntities(); }
void World::rebuildEntities() {
    entities_.clear(); names_.clear();
    player_ = entities_.create(); names_["player"] = player_;
    entities_.emplace<EntityName>(player_,"player");
    entities_.emplace<Transform>(player_);
    entities_.emplace<Mover>(player_);
    std::string sprite;
    if (!state_.party.empty()) sprite = db_.actors.at(state_.party.front()).sprite;
    entities_.emplace<SpriteRenderer>(player_,sprite);
    entities_.emplace<CameraTarget>(player_);
    for (std::size_t i = 0; i < map_.entities.size(); ++i) {
        const auto& def = map_.entities[i];
        auto e = entities_.create(); names_[def.id] = e;
        entities_.emplace<EntityName>(e,def.id);
        entities_.emplace<Transform>(e,def.position,def.position,def.facing);
        entities_.emplace<Mover>(e,def.speed,def.radius,def.solid);
        entities_.emplace<SpriteRenderer>(e,def.sprite,def.clip);
        entities_.emplace<Interactable>(e,i);
        entities_.emplace<NpcRoutine>(e);
    }
    for (const auto& light : map_.lights) {
        auto e = entities_.create(); entities_.emplace<Light>(e,light);
    }
    updatePages();
}
entt::entity World::entity(const std::string& id) const {
    auto found = names_.find(id);
    return found == names_.end() ? entt::null : found->second;
}
Transform& World::transform(const std::string& id) {
    const auto e = entity(id);
    if (e == entt::null) throw std::runtime_error("Unknown field entity: " + id);
    return entities_.get<Transform>(e);
}
glm::vec3 World::playerPosition() const { return entities_.get<Transform>(player_).position; }
void World::spawn(const std::string& name) {
    auto found = map_.spawns.find(name);
    if (found == map_.spawns.end()) throw std::runtime_error("Unknown spawn '" + name + "' on map " + map_.id);
    restorePosition(found->second); state_.spawn = name;
}
void World::restorePosition(glm::vec3 p) {
    if (!map_.nav.canStand({p.x,p.z})) throw std::runtime_error("Saved player position is not walkable");
    p.y = map_.nav.heightAt({p.x,p.z});
    auto& t = entities_.get<Transform>(player_); t.position = t.previous = p;
    entities_.get<Mover>(player_).hasDestination=false;
    state_.position = p; state_.map = map_.id;
}
void World::updatePages() {
    for (auto e : entities_.view<Interactable,SpriteRenderer,Mover>()) {
        auto& trigger = entities_.get<Interactable>(e);
        auto& sprite = entities_.get<SpriteRenderer>(e);
        auto& mover = entities_.get<Mover>(e);
        const auto& def = map_.entities.at(trigger.definition);
        int page = -1;
        for (int i = 0; i < static_cast<int>(def.pages.size()); ++i) if (def.pages[i].matches(state_)) page = i;
        if (page != trigger.page) {
            // A chest may set its opened flag before saying its final line.
            // Let that event finish across a page change. Parallel routines,
            // however, stop as soon as their page is no longer active.
            if (trigger.page >= 0 && trigger.page < static_cast<int>(def.pages.size()) &&
                def.pages[trigger.page].trigger == "parallel") scripts_.cancel(trigger.task);
            if (!scripts_.running(trigger.task)) trigger.task = 0;
            trigger.page = page;
        }
        if (page >= 0) { sprite.visible = true; sprite.animation = def.pages[page].sprite; mover.solid = def.pages[page].solid; }
        else { sprite.visible = def.pages.empty(); sprite.animation = def.sprite; mover.solid = def.solid && sprite.visible; }
    }
}
bool World::overlapsSolid(entt::entity self, glm::vec3 p) const {
    const auto& mover = entities_.get<Mover>(self);
    for (auto e : entities_.view<Transform,Mover>()) {
        if (e == self || !entities_.get<Mover>(e).solid) continue;
        const auto other = entities_.get<Transform>(e).position;
        if (std::abs(other.y - p.y) > 0.6f) continue;
        const auto d = glm::vec2(other.x - p.x,other.z - p.z);
        const auto radius = mover.radius + entities_.get<Mover>(e).radius;
        if (glm::dot(d,d) < radius * radius) return true;
    }
    return false;
}
void World::move(entt::entity e, glm::vec2 delta, bool ignoreEntities) {
    auto& t = entities_.get<Transform>(e); const auto& mover = entities_.get<Mover>(e);
    if (glm::length(delta) > 0.0001f) t.facing = glm::normalize(delta);
    const int steps = std::max(1,static_cast<int>(std::ceil(glm::length(delta) / 0.1f)));
    for (int i = 0; i < steps; ++i) {
        auto next = map_.nav.move(t.position,delta/static_cast<float>(steps),mover.radius);
        if (ignoreEntities || !overlapsSolid(e,next)) t.position = next;
        else {
            next = map_.nav.move(t.position,{delta.x/steps,0},mover.radius);
            if (!overlapsSolid(e,next)) t.position = next;
            next = map_.nav.move(t.position,{0,delta.y/steps},mover.radius);
            if (!overlapsSolid(e,next)) t.position = next;
        }
    }
    auto& sprite = entities_.get<SpriteRenderer>(e);
    const std::string clip=glm::length(t.position - t.previous) > 0.001f ? "walk" : "idle";
    if (sprite.clip!=clip) {sprite.clip=clip;sprite.elapsed=0;sprite.previousElapsed=-1e-8;}
}
bool World::moveEntity(const std::string& id, glm::vec2 target, double dt, bool ignoreEntities) {
    const auto e = entity(id);
    if (e == entt::null) throw std::runtime_error("Unknown field entity: " + id);
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || target.x<0 || target.y<0 || target.x>=map_.nav.width() || target.y>=map_.nav.depth())
        throw std::runtime_error("Movement target is outside the nav grid");
    const auto p = entities_.get<Transform>(e).position;
    auto& mover=entities_.get<Mover>(e);
    if (glm::length(target-glm::vec2(p.x,p.z))<.025f) {mover.hasDestination=false;return true;}
    if (!mover.hasDestination || glm::length(mover.destination-target)>.01f) {
        mover.destination=target;mover.hasDestination=true;
        mover.path=map_.nav.findPath({int(std::floor(p.x)),int(std::floor(p.z))},{int(std::floor(target.x)),int(std::floor(target.y))});
    }
    glm::vec2 waypoint=target;
    while (!mover.path.empty()) {
        waypoint=glm::vec2(mover.path.front())+.5f;
        if (glm::length(waypoint-glm::vec2(p.x,p.z))>.025f) break;
        mover.path.erase(mover.path.begin());waypoint=target;
    }
    if (mover.path.empty() && glm::ivec2(std::floor(p.x),std::floor(p.z))!=glm::ivec2(std::floor(target.x),std::floor(target.y))) return false;
    auto delta = waypoint - glm::vec2(p.x,p.z);
    const float distance = glm::length(delta), step = mover.speed * static_cast<float>(dt);
    if (distance > step && distance > 0) delta *= step / distance;
    move(e,delta,ignoreEntities);
    return glm::length(glm::vec2(entities_.get<Transform>(e).position.x,entities_.get<Transform>(e).position.z) - target) < 0.025f;
}
void World::faceEntity(const std::string& id, glm::vec2 direction) {
    if (glm::length(direction) > 0.001f) transform(id).facing = glm::normalize(direction);
}
void World::startEvent(entt::entity e) {
    auto& t = entities_.get<Interactable>(e);
    if (t.page < 0 || scripts_.running(t.task)) return;
    const auto& page = map_.entities[t.definition].pages[t.page];
    if (page.script.empty()) return;
    try { t.task = scripts_.start(page.script,entities_.get<EntityName>(e).id,page.trigger != "parallel"); }
    catch (const std::exception& error) { log(LogLevel::Error,error.what()); }
}
void World::interact() {
    const auto& p = entities_.get<Transform>(player_);
    float best = 2.1f; entt::entity selected = entt::null;
    const glm::vec2 origin(p.position.x,p.position.z);
    const auto ahead = origin + p.facing;
    const int cx = static_cast<int>(std::floor(ahead.x)), cz = static_cast<int>(std::floor(ahead.y));
    const bool counter = map_.nav.contains(cx,cz) && (map_.nav.at(cx,cz).flags & Counter);
    for (auto e : entities_.view<Transform,Interactable>()) {
        const auto& t = entities_.get<Interactable>(e);
        if (t.page < 0 || map_.entities[t.definition].pages[t.page].trigger != "action") continue;
        const auto position = entities_.get<Transform>(e).position;
        const auto delta = glm::vec2(position.x,position.z) - origin; const auto distance = glm::length(delta);
        if (distance > (counter ? 2.1f : 1.2f) || distance >= best || (distance > 0.1f && glm::dot(delta/distance,p.facing) < 0.35f)) continue;
        best = distance; selected = e;
    }
    if (selected != entt::null) startEvent(selected);
}
std::optional<std::string> World::update(double dt, glm::vec2 movement, bool action, bool paused) {
    if (!std::isfinite(dt) || dt <= 0) return {};
    if (!prepared_) beginTick(dt);prepared_=false;
    updatePages();
    if (paused) return {};
    const auto old = playerPosition();
    if (!scripts_.blocksField()) {
        if (glm::length(movement) > 1) movement = glm::normalize(movement);
        move(player_,movement * entities_.get<Mover>(player_).speed * static_cast<float>(dt));
        if (action) interact();
    }
    for (auto e : entities_.view<Interactable,NpcRoutine,Transform>()) {
        auto& t = entities_.get<Interactable>(e); const auto& def = map_.entities[t.definition];
        if (t.page >= 0) {
            const auto& page = def.pages[t.page];
            const auto delta = playerPosition() - entities_.get<Transform>(e).position;
            const bool touching = glm::length(glm::vec2(delta.x,delta.z)) < def.radius + 0.45f && std::abs(delta.y) < 0.6f;
            if ((page.trigger == "touch" && touching && !t.touching && !scripts_.blocksField()) ||
                page.trigger == "parallel" || (page.trigger == "autorun" && !scripts_.blocksField())) startEvent(e);
            t.touching = touching;
        }
        if (!scripts_.blocksField() && !scripts_.running(t.task) && !def.route.empty() && def.routine == "patrol") {
            auto& routine = entities_.get<NpcRoutine>(e);
            if (moveEntity(def.id,def.route[routine.point % def.route.size()],dt)) routine.point = (routine.point+1) % def.route.size();
        }
        if (!scripts_.blocksField() && !scripts_.running(t.task) && def.routine=="wander") {
            auto& routine=entities_.get<NpcRoutine>(e);routine.wait-=dt;
            if (!routine.moving && routine.wait<=0) {
                const auto p=entities_.get<Transform>(e).position;
                routine.target={std::floor(p.x)+.5f+int(random_()%5)-2,std::floor(p.z)+.5f+int(random_()%5)-2};
                routine.moving=map_.nav.canStand(routine.target);routine.moveTime=0;
                if (!routine.moving) routine.wait=.5;
            }
            if (routine.moving) {
                routine.moveTime+=dt;
                if (moveEntity(def.id,routine.target,dt) || routine.moveTime>3) {routine.moving=false;routine.wait=.75+double(random_())/4294967296.0;}
            }
        }
    }
    for (auto e:entities_.view<Transform,Mover,SpriteRenderer>()) {
        auto& sprite=entities_.get<SpriteRenderer>(e);const auto& t=entities_.get<Transform>(e);
        if (sprite.clip=="walk" && glm::length(t.position-t.previous)<.001f) {sprite.clip="idle";sprite.elapsed=0;sprite.previousElapsed=-1e-8;}
    }
    const auto p = playerPosition(); state_.position = p; state_.map = map_.id;
    const double distance = glm::length(p-old);
    const int x = static_cast<int>(std::floor(p.x)), z = static_cast<int>(std::floor(p.z));
    if (!map_.nav.contains(x,z) || distance <= 0) return {};
    const auto& cell = map_.nav.at(x,z);
    if (cell.flags & DamageFloor) {
        damageDistance_ += distance;
        if (damageDistance_ >= 1) { damageDistance_ -= 1; for (const auto& id : state_.party) { auto& a = state_.actor(id); if (a.hp > 1) --a.hp; } }
    }
    auto table = map_.encounters.find(cell.region);
    if (scripts_.blocksField() || table == map_.encounters.end()) return {};
    if (!db_.encounters.contains(table->second)) throw std::runtime_error("Unknown encounter table " + table->second);
    const auto& encounter = db_.encounters.at(table->second);
    if (encounterTable_ != table->second || encounterThreshold_ == 0) {
        encounterTable_ = table->second; encounterDistance_ = 0;
        encounterThreshold_ = encounter.distance * (0.6 + static_cast<double>(random_())/4294967296.0*0.8);
    }
    encounterDistance_ += distance;
    if (encounterDistance_ < encounterThreshold_) return {};
    encounterThreshold_ = 0;
    int total = 0; for (const auto& [troop,weight] : encounter.troops) { (void)troop; total += weight; }
    int roll = static_cast<int>(random_() % static_cast<std::uint32_t>(total));
    for (const auto& [troop,weight] : encounter.troops) { if (roll < weight) return troop; roll -= weight; }
    return {};
}
void World::beginTick(double dt) {
    for (auto e:entities_.view<Transform>()) {auto& t=entities_.get<Transform>(e);t.previous=t.position;}
    for (auto e:entities_.view<SpriteRenderer>()) {auto& sprite=entities_.get<SpriteRenderer>(e);sprite.previousElapsed=sprite.elapsed;sprite.elapsed+=dt;}
    prepared_=true;
}
}
