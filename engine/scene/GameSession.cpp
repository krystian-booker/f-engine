#include "engine/scene/GameSession.h"
#include "engine/audio/Audio.h"
#include "engine/battle/ActionSequence.h"
#include "engine/core/Log.h"
#include "engine/input/Input.h"
#include "engine/render/WorldRenderer.h"
#include "engine/scene/SceneStack.h"
#include "engine/ui/Canvas.h"
#include "engine/world/World.h"
#include "engine/world/Particles.h"
#include "engine/assets/SpriteAnimation.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <deque>
#include <iomanip>
#include <optional>
#include <sstream>
#include <set>

namespace fe {
namespace {
std::string number(int value) { return std::to_string(value); }
std::string clockText(double seconds) {
    const auto minutes=static_cast<int>(seconds)/60;
    std::ostringstream text; text<<minutes/60<<':'<<std::setw(2)<<std::setfill('0')<<minutes%60; return text.str();
}
std::filesystem::path gamePath(const std::filesystem::path& root,const std::string& relative) {
    const auto candidate=std::filesystem::weakly_canonical(root/relative),base=std::filesystem::weakly_canonical(root);
    const auto path=candidate.lexically_relative(base);
    if (path.empty() || path.is_absolute() || *path.begin()=="..") throw std::runtime_error("Asset must be inside the game folder");
    return candidate;
}
}
struct GameSession::Impl {
    enum class Screen { Field, Title, Dialogue, Choice, Menu, Shop, Battle };
    struct Layer final : Scene {
        Impl& game; Screen screen;
        Layer(Impl& g,Screen s) : game(g),screen(s) {}
        void enter() override { game.layers.push_back(screen); }
        void leave() override { if (!game.layers.empty()) game.layers.pop_back(); }
        void update(double dt) override { game.updateLayer(screen,dt); }
        void render(double alpha) override { game.renderLayer(screen,alpha); }
        bool blocksRender() const override { return screen==Screen::Battle || screen==Screen::Field; }
    };
    struct Popup { int target=0,amount=0; bool heal=false; std::string text; double age=0; };
    struct Movement { std::string entity; glm::vec2 target{0}; double elapsed=0; };
    std::filesystem::path root,userData,shaderRoot;
    GameConfig config;
    Database db;
    GameState state;
    SaveSystem saves;
    ScriptEngine scripts;
    Audio audio;
    Input input;
    InputRepeat repeat;
    ActionSnapshot actions;
    WorldRenderer graphics;
    Canvas ui;
    std::unique_ptr<World> field;
    SceneStack scenes;
    std::vector<Screen> layers;
    std::map<std::string,MenuCursor> cursors;
    std::string message,status,menuPage="Main",selectedItem,shopId;
    int menuActor=0,menuTarget=0;
    bool shopSelling=false,shopFinished=false;
    std::uint64_t shopOwner=0,dialogueOwner=0;
    std::string speaker,portrait;
    Typewriter dialogue;
    std::vector<std::vector<TextGlyph>> dialoguePages;
    std::size_t dialoguePage=0;
    std::vector<std::string> choices;
    std::string choiceTitle,choiceLayout="choice_window";
    bool dialogueFinished=false;
    int choiceResult=0;
    std::optional<Action> rebinding;
    std::map<std::uint64_t,Movement> movements;
    std::optional<CameraSettings> cameraOverride;
    std::optional<glm::vec3> cameraTarget;
    std::string cameraEntity;
    std::uint64_t cameraOwner=0;
    double cameraRemaining=0;
    float fade=0,fadeGoal=0,fadeRate=0,flash=0;
    double shakeTime=0;float shakeAmplitude=.1f;
    std::uint64_t fadeOwner=0;
    std::string fadeCommand;
    AtbMode atbMode=AtbMode::Wait;
    std::unique_ptr<Battle> battle;
    std::unique_ptr<World> stage;
    std::vector<entt::entity> stageActors;
    std::vector<glm::vec3> stagePositions;
    std::vector<glm::vec3> stageDestinations;
    std::vector<int> visualHp;
    std::vector<NumberTween> healthTweens;
    std::vector<Popup> popups;
    std::map<std::string,ActionTimeline> timelines;
    std::map<std::string,SpriteAnimation> animations;
    std::set<std::string> failedAnimations;
    std::map<std::string,std::string> soundEvents;
    std::string displayedLeader;
    std::filesystem::path pendingDatabase;
    ActionSequence sequence;
    std::vector<BattleEvent> presentation;
    bool presenting=false,effectsShown=false,battleFinished=false;
    int presentingUser=-1,commandUser=-1;
    std::string battleMenu="Command",battleSelection;
    ActionKind battleKind=ActionKind::Skill;
    std::uint64_t battleOwner=0;
    BattleOutcome battleResult=BattleOutcome::Ongoing;
    int viewportWidth=1280,viewportHeight=720;
    double frameDt=1.0/60;
    explicit Impl(std::filesystem::path folder,std::filesystem::path storage,std::filesystem::path shaders,GameConfig cfg,bool mute,bool skipTitle)
        : root(std::filesystem::weakly_canonical(folder)),userData(std::move(storage)),shaderRoot(std::move(shaders)),config(std::move(cfg)),
          db(Database::load(root/"database.json")),saves(userData/"saves"),scripts(root,state,db),audio(mute),graphics(root,shaderRoot,config.pixelsPerUnit),ui(root,config.virtualWidth,config.virtualHeight) {
        newGame();
        scripts.loadRules("scripts/rules.lua");
        scripts.setHandler([this](const auto& request,double dt) { return handle(request,dt); });
        timelines=ActionTimeline::load(root/"timelines.json");
        if (std::filesystem::exists(root/"audio/events.json")) soundEvents=readJson(root/"audio/events.json").get<std::map<std::string,std::string>>();
        if (std::filesystem::exists(userData/"config.json")) {
            try {
                const auto preferences=readJson(userData/"config.json");
                input.fromJson(preferences.value("keys",Json::object()));
                audio.setVolume(AudioBus::Master,preferences.value("master",1.0f));
                audio.setVolume(AudioBus::Music,preferences.value("music",.65f));
                audio.setVolume(AudioBus::Effects,preferences.value("effects",.8f));
                atbMode=preferences.value("atb_wait",true)?AtbMode::Wait:AtbMode::Active;
            } catch (const std::exception& e) { log(LogLevel::Warning,e.what()); }
        }
        push(Screen::Field); if (!skipTitle) push(Screen::Title);
    }
    ~Impl() { scripts.cancelAll(); scenes.clear(); scenes.update(0); }
    void push(Screen screen) { scenes.push(std::make_unique<Layer>(*this,screen)); repeat.clear(); }
    [[nodiscard]] bool has(Screen screen) const { return std::find(layers.begin(),layers.end(),screen)!=layers.end(); }
    bool hit(Action action) { return repeat.hit(actions,action,frameDt); }
    void navigate(MenuCursor& cursor) { if (hit(Action::Up)) cursor.move(-1); if (hit(Action::Down)) cursor.move(1); }
    void newGame();
    void warp(const std::string& map,const std::string& spawn,bool restore=false);
    void loadSlot(int slot);
    void savePreferences();
    ScriptResponse handle(const ScriptRequest& request,double dt);
    void update(double dt,bool paused);
    void updateLayer(Screen screen,double dt);
    void renderLayer(Screen screen,double alpha);
    void updateMenu();
    void drawMenu();
    void updateShop();
    void drawShop();
    void startBattle(const std::string& troop,std::uint64_t owner=0);
    void updateBattle(double dt);
    void drawBattle(double alpha);
    void beginPresentation(std::vector<BattleEvent> events);
    void showEffects();
    void animationEvents(World& world);
    void closeBattle();
    void partyPanel(Rect rectangle,bool detail=true);
    void reloadMap();
    void reloadDatabase(const std::filesystem::path& path);
    std::string substitute(const std::string& code) const;
};
void GameSession::Impl::newGame() {
    state=GameState{};
    const auto initial=readJson(root/"initial_state.json");
    for (const auto& actor:initial.at("party")) state.addActor(db,actor);
    state.setParty(initial.at("party").get<std::vector<std::string>>());
    state.gold=initial.value("gold",0);
    const auto items=initial.value("inventory",Json::object());
    for (const auto& [id,count]:items.items()) if (!state.addItem(db,id,count.get<int>())) throw std::runtime_error("Invalid initial inventory: " + id);
    warp(config.startMap,config.startSpawn);
    state.validate(db);
}
void GameSession::Impl::warp(const std::string& id,const std::string& spawn,bool restore) {
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
        throw std::runtime_error("Map ids must use letters, numbers, underscores or dashes");
    auto map=MapData::load(root/"maps"/id/"map.json");
    if (map.id!=id) throw std::runtime_error("Map folder and id disagree: " + id);
    auto replacement=std::make_unique<World>(std::move(map),state,db,scripts);
    if (restore) replacement->restorePosition(state.position); else replacement->spawn(spawn);
    field=std::move(replacement); movements.clear(); cameraOverride.reset();cameraTarget.reset();cameraEntity.clear();cameraOwner=0;
    graphics.updateCamera(*field,0,true);
    if (!field->map().bgm.empty()) audio.playBgm(gamePath(root,field->map().bgm));
}
void GameSession::Impl::loadSlot(int slot) {
    auto loaded=saves.load(slot,db);
    // Validate the map and position before replacing the live game state.
    const auto map=MapData::load(gamePath(root,"maps/"+loaded.map+"/map.json"));
    if (!map.nav.canStand({loaded.position.x,loaded.position.z})) throw std::runtime_error("Save position no longer exists on this map");
    scripts.cancelAll(); state=std::move(loaded); warp(state.map,state.spawn,true);
    dialogueOwner=shopOwner=battleOwner=0; dialogueFinished=shopFinished=false;
    battle.reset(); stage.reset(); scenes.clear(); push(Screen::Field); menuPage="Main"; fade=fadeGoal=0;
    status="Loaded slot "+number(slot);
}
void GameSession::Impl::savePreferences() {
    writeJson(userData/"config.json",{{"keys",input.toJson()},{"master",audio.volume(AudioBus::Master)},
        {"music",audio.volume(AudioBus::Music)},{"effects",audio.volume(AudioBus::Effects)},{"atb_wait",atbMode==AtbMode::Wait}});
}
std::string GameSession::Impl::substitute(const std::string& code) const {
    if (code=="player") return state.party.empty()?"Hero":db.actors.at(state.party.front()).name;
    if (code.starts_with("var:")) return number(static_cast<int>(state.variable(code.substr(4))));
    return "";
}
void GameSession::Impl::update(double dt,bool paused) {
    frameDt=dt; actions=input.tick(); audio.update(dt);
    if (paused) return;
    field->beginTick(dt);
    if (fade!=fadeGoal) {
        const auto distance=fadeRate*static_cast<float>(dt);
        fade=fade<fadeGoal?std::min(fadeGoal,fade+distance):std::max(fadeGoal,fade-distance);
    }
    flash=std::max(0.0f,flash-static_cast<float>(dt)*2);
    shakeTime=std::max(0.0,shakeTime-dt);
    dialogue.update(dt);
    if (!has(Screen::Title)) {
        state.playtime+=dt;
        if (!has(Screen::Menu)) scripts.update(dt);
        if (dialogueOwner && !scripts.running(dialogueOwner)) {dialogueOwner=0;dialogueFinished=false;scenes.pop();}
        if (shopOwner && !scripts.running(shopOwner)) {shopOwner=0;shopFinished=false;scenes.pop();}
        if (cameraOwner && !scripts.running(cameraOwner)) cameraOwner=0;
        if (fadeOwner && !scripts.running(fadeOwner)) fadeOwner=0;
    }
    scenes.update(dt);
    if (!state.party.empty() && (displayedLeader!=state.party.front() || field->registry().get<SpriteRenderer>(field->player()).animation!=db.actors.at(state.party.front()).sprite)) {
        displayedLeader=state.party.front();auto& sprite=field->registry().get<SpriteRenderer>(field->player());sprite.animation=db.actors.at(displayedLeader).sprite;sprite.elapsed=0;
    }
    updateParticles(field->registry(),dt);animationEvents(*field);
    if (stage) {updateParticles(stage->registry(),dt);animationEvents(*stage);}
    if (!has(Screen::Battle)) {
        if (cameraOverride || cameraTarget || !cameraEntity.empty()) {
            const auto target=!cameraEntity.empty()?field->transform(cameraEntity).position:cameraTarget.value_or(field->playerPosition());
            graphics.camera().update(target,cameraOverride.value_or(field->map().cameraAt(target)),dt);
        }
        else graphics.updateCamera(*field,dt);
    }
    graphics.camera().setVisualOffset(shakeTime>0?glm::vec3(std::sin(state.playtime*91)*shakeAmplitude,std::cos(state.playtime*71)*shakeAmplitude,0):glm::vec3(0));
}
void GameSession::Impl::updateLayer(Screen screen,double dt) {
    switch (screen) {
    case Screen::Field: {
        if (actions.hit(Action::Menu) && !scripts.blocksField()) { menuPage="Main"; status.clear(); push(Screen::Menu); return; }
        const float yaw=glm::radians(graphics.camera().yaw());
        const auto motion=glm::vec2(std::cos(yaw),-std::sin(yaw))*actions.movement.x+glm::vec2(std::sin(yaw),std::cos(yaw))*actions.movement.y;
        if (auto encounter=field->update(dt,motion,actions.hit(Action::Confirm))) startBattle(*encounter);
        break;
    }
    case Screen::Title: {
        auto& cursor=cursors["Title"]; cursor.setCount(2); navigate(cursor);
        if (actions.hit(Action::Confirm)) {
            if (cursor.selected()==0) {scenes.pop();status.clear();}
            else {menuPage="Load";cursors["Load"].select(0);push(Screen::Menu);}
        }
        break;
    }
    case Screen::Dialogue:
        if (actions.hit(Action::Confirm)||actions.hit(Action::Cancel)) {
            if (!dialogue.complete()) dialogue.finish();
            else if (dialoguePage+1<dialoguePages.size()) dialogue.set(dialoguePages[++dialoguePage]);
            else dialogueFinished=true;
        }
        break;
    case Screen::Choice: {
        auto& cursor=cursors["Choice"]; cursor.setCount(static_cast<int>(choices.size())); navigate(cursor);
        if (actions.hit(Action::Confirm)) { choiceResult=cursor.selected()+1; dialogueFinished=true; }
        if (actions.hit(Action::Cancel)) { choiceResult=static_cast<int>(choices.size()); dialogueFinished=true; }
        break;
    }
    case Screen::Menu: updateMenu(); break;
    case Screen::Shop: updateShop(); break;
    case Screen::Battle: updateBattle(dt); break;
    }
}
void GameSession::Impl::renderLayer(Screen screen,double alpha) {
    switch (screen) {
    case Screen::Field:
        graphics.render(*field,alpha,viewportWidth,viewportHeight);
        if (layers.size()==1) {
            ui.window(ui.layout("field_hint",{8,float(ui.height()-22),260,15}));
            ui.text("Move: arrows/WASD  Confirm: Z/Enter  Menu: Tab",14,float(ui.height()-18),0xe4e9f4ff,7);
        }
        break;
    case Screen::Title: {
        const float x=float(ui.width())*.5f;
        ui.fillScreen(0x0b122960);
        ui.window({x-125,48,250,155});
        ui.text(config.title,x-110,65,0xffdf9fff,12);
        ui.text("Adventure awaits",x-42,89,0xc9d8ecff,8);
        ui.list({"New adventure","Continue"},cursors["Title"],{x-66,125,150,40},2);
#if FENGINE_DEV
        ui.text("F1: developer tools",x-51,181,0xb7c5ddff,7);
#endif
        if (!status.empty()) ui.text(status,12,float(ui.height()-40),0xffb3a0ff,7);
        break;
    }
    case Screen::Dialogue: {
        auto r=ui.layout("dialogue",{8,float(ui.height()-85),float(ui.width()-16),76}); ui.window(r);
        if (!speaker.empty()) ui.text(speaker,r.x+12,r.y+8,0xffdc8aff,9);
        float inset=12;
        if (!portrait.empty()) { ui.image(portrait,{r.x+8,r.y+20,40,48}); inset=56; }
        ui.rich(dialogue.glyphs(),dialogue.visible(),{r.x+inset,r.y+24,r.w-inset-12,r.h-32},9);
        if (dialogue.complete()) ui.text("v",r.x+r.w-16,r.y+r.h-13,0xffdc8aff,8);
        break;
    }
    case Screen::Choice: {
        const float extra=choiceTitle.empty()?0.0f:16.0f;const float height=static_cast<float>(choices.size()*13+16)+extra;
        Rect r=ui.layout(choiceLayout,{float(ui.width()-210),float(ui.height())-height-16,200,height});ui.window(r);
        if (!choiceTitle.empty()) ui.text(choiceTitle,r.x+8,r.y+7,0xffdf9fff,9);
        ui.list(choices,cursors["Choice"],{r.x+8,r.y+8+extra,r.w-16,r.h-16-extra},static_cast<int>(choices.size()));break;
    }
    case Screen::Menu: drawMenu(); break;
    case Screen::Shop: drawShop(); break;
    case Screen::Battle: drawBattle(alpha); break;
    }
}
ScriptResponse GameSession::Impl::handle(const ScriptRequest& r,double dt) {
    const auto& a=r.arguments;
    if (r.command=="say" || r.command=="choice" || r.command=="inn" || r.command=="menu") {
        if (battle || (shopOwner && shopOwner!=r.task)) return {};
        if (!dialogueOwner) {
            dialogueOwner=r.task; dialogueFinished=false; choiceResult=0;
            if (r.command=="say") {
                speaker=a.at(0).get<std::string>(); portrait=a.size()>2 && a[2].is_string()?a[2].get<std::string>():"";
                const auto r=ui.layout("dialogue",{8,float(ui.height()-85),float(ui.width()-16),76});
                const float inset=portrait.empty()?12.0f:56.0f;
                dialoguePages=ui.pages(parseText(a.at(1).get<std::string>(),[this](const auto& code) { return substitute(code); }),{0,0,r.w-inset-12,r.h-32},9);
                dialoguePage=0;dialogue.set(dialoguePages.front());push(Screen::Dialogue);
            } else {
                choiceTitle.clear();choiceLayout="choice_window";
                if (r.command=="menu") {const auto& settings=a.at(0);choices=settings.at("options").get<std::vector<std::string>>();choiceTitle=settings.value("title",std::string{});choiceLayout=settings.value("layout",choiceLayout);}
                else choices=r.command=="inn"?std::vector<std::string>{"Rest for "+number(a.at(0).get<int>())+" gold","Leave"}:a.at(0).get<std::vector<std::string>>();
                if (choices.empty() || choices.size()>12) throw std::runtime_error("Choices must contain 1..12 entries");
                cursors["Choice"].select(0); push(Screen::Choice);
            }
        }
        if (dialogueOwner!=r.task || !dialogueFinished) return {};
        dialogueOwner=0; dialogueFinished=false; scenes.pop();
        if (r.command=="inn") {
            const int price=a.at(0).get<int>(); const bool rested=choiceResult==1 && price>=0 && state.gold>=price;
            if (rested) { state.gold-=price;state.rest(db); } return {true,rested};
        }
        return {true,r.command=="choice" || r.command=="menu"?Json(choiceResult):Json()};
    }
    if (r.command=="face") {
        const auto id=a.at(0).get<std::string>(); glm::vec2 direction{0,1};
        if (a.at(1).is_string()) {
            const auto target=a.at(1).get<std::string>();
            if (target=="player") { const auto p=field->playerPosition()-field->transform(id).position; direction={p.x,p.z}; }
            else if (target=="north") direction={0,-1}; else if (target=="west") direction={-1,0}; else if (target=="east") direction={1,0};
        } else direction={a[1].at(0).get<float>(),a[1].at(1).get<float>()};
        field->faceEntity(id,direction); return {true,{}};
    }
    if (r.command=="move") {
        if (!movements.contains(r.task)) {
            Movement movement; movement.entity=a.at(0).get<std::string>();
            const auto p=field->transform(movement.entity).position; const auto& destination=a.at(1);
            movement.target={destination.value("x",p.x+destination.value("dx",0.0f)),destination.value("z",p.z+destination.value("dz",destination.value("dy",0.0f)))};
            movements[r.task]=movement;
        }
        auto& movement=movements.at(r.task); movement.elapsed+=dt;
        if (movement.elapsed>30) { movements.erase(r.task); throw std::runtime_error("Scripted movement blocked for 30 seconds"); }
        const bool done=field->moveEntity(movement.entity,movement.target,dt);
        if (done) movements.erase(r.task); return {done,{}};
    }
    if (r.command=="animate") {
        const auto e=field->entity(a.at(0)); if (e==entt::null) throw std::runtime_error("Unknown animation entity");
        auto& sprite=field->registry().get<SpriteRenderer>(e); sprite.clip=a.at(1); sprite.elapsed=0;sprite.previousElapsed=-1e-8;return {true,{}};
    }
    if (r.command=="fade_out" || r.command=="fade_in") {
        if (fadeOwner && fadeOwner!=r.task) return {};
        if (fadeOwner!=r.task || fadeCommand!=r.command) {
            fadeOwner=r.task; fadeCommand=r.command; fadeGoal=r.command=="fade_out"?1.0f:0.0f;
            const float seconds=a.at(0).get<float>(); if (!std::isfinite(seconds) || seconds<0) throw std::runtime_error("Invalid fade duration");
            fadeRate=seconds>0?std::abs(fadeGoal-fade)/seconds:10000; if (seconds==0) fade=fadeGoal;
        }
        if (fade==fadeGoal) { fadeOwner=0; return {true,{}}; } return {};
    }
    if (r.command=="warp") { scripts.cancelAllExcept(r.task);warp(a.at(0),a.at(1));return {true,{}}; }
    if (r.command=="battle") {
        if (dialogueOwner || shopOwner) return {};
        if (!battleOwner) { startBattle(a.at(0),r.task); battleOwner=r.task; }
        if (battleOwner!=r.task || !battleFinished) return {};
        battleOwner=0; battleFinished=false;
        return {true,battleResult==BattleOutcome::Victory?"victory":battleResult==BattleOutcome::Defeat?"defeat":"escaped"};
    }
    if (r.command=="shop") {
        if (dialogueOwner || battle) return {};
        if (!shopOwner) {shopId=a.at(0);if (!db.shops.contains(shopId)) throw std::runtime_error("Unknown shop: "+shopId);shopOwner=r.task;shopFinished=false;shopSelling=false;push(Screen::Shop);}
        if (shopOwner!=r.task || !shopFinished) return {};
        shopOwner=0; shopFinished=false; return {true,{}};
    }
    if (r.command=="bgm") {
        BgmOptions options;
        if (a.size()>1 && a[1].is_object()) { options.fadeSeconds=a[1].value("fade",.75);options.loopStart=a[1].value("loop_start",0.0);options.loopEnd=a[1].value("loop_end",0.0); }
        audio.playBgm(gamePath(root,a.at(0)),options);return {true,{}};
    }
    if (r.command=="sfx") { audio.playSfx(gamePath(root,a.at(0)));return {true,{}}; }
    if (r.command=="stop_bgm") {audio.stopBgm(a.at(0).get<double>());return {true,{}};}
    if (r.command=="save") {if (battle) throw std::runtime_error("Cannot save during a battle");state.validate(db);saves.save(a.at(0).get<int>(),state);return {true,{}};}
    if (r.command=="load") {loadSlot(a.at(0).get<int>());return {true,{}};}
    if (r.command=="input") {
        const auto name=a.at(0).get<std::string>();
        for (std::size_t i=0;i<ActionCount;++i) if (name==Input::name(static_cast<Action>(i))) return {actions.hit(static_cast<Action>(i)),true};
        throw std::runtime_error("Unknown input action: "+name);
    }
    if (r.command=="camera") {
        const auto& settings=a.at(0);
        if (cameraOwner && cameraOwner!=r.task) return {};
        if (!cameraOwner) {
            cameraOwner=r.task;
            cameraRemaining=settings.value("duration",0.0);
            if (!std::isfinite(cameraRemaining) || cameraRemaining<0 || cameraRemaining>60) throw std::runtime_error("Invalid camera duration");
        if (settings.value("reset",false)) {cameraOverride.reset();cameraTarget.reset();cameraEntity.clear();}
        else {
            auto camera=field->map().cameraAt(field->playerPosition());
            camera.pitch=settings.value("pitch",camera.pitch);camera.yaw=settings.value("yaw",camera.yaw);camera.distance=settings.value("distance",camera.distance);camera.fov=settings.value("fov",camera.fov);
            if (camera.pitch<5 || camera.pitch>85 || camera.distance<=0 || camera.fov<5 || camera.fov>100) throw std::runtime_error("Invalid scripted camera");
            if (settings.contains("target")) {
                if (settings.at("target").is_string()) {cameraEntity=settings.at("target");(void)field->transform(cameraEntity);cameraTarget.reset();}
                else {const auto& p=settings.at("target");cameraTarget=glm::vec3(p.at(0).get<float>(),p.at(1).get<float>(),p.at(2).get<float>());cameraEntity.clear();}
                if (!settings.value("respect_bounds",false)) {camera.min=glm::vec2(-10000);camera.max=glm::vec2(10000);}
            }
            cameraOverride=camera;
        }
        }
        cameraRemaining-=dt;
        if (cameraRemaining<=0) {cameraOwner=0;return {true,{}};}return {};
    }
    if (r.command=="screen_fx") {
        const auto& settings=a.at(0); auto& post=field->map().post;
        post.bloom=settings.value("bloom",post.bloom);post.depthOfField=settings.value("depth_of_field",post.depthOfField);post.vignette=settings.value("vignette",post.vignette);
        flash=settings.value("flash",flash);
        if (settings.contains("shake")) {const auto& shake=settings.at("shake");shakeTime=shake.value("duration",.3);shakeAmplitude=shake.value("amplitude",.1f);}
        if (settings.contains("particles")) {
            const auto& data=settings.at("particles");ParticleBurst burst;burst.sprite=data.value("sprite",config.battleEffectSprite);
            const auto entity=data.value("entity",std::string("player"));burst.position=field->transform(entity).position+glm::vec3(0,.7f,0);
            burst.count=data.value("count",12);burst.speed=data.value("speed",1.5f);burst.size=data.value("size",.25f);burst.lifetime=data.value("lifetime",.7);
            emitParticles(field->registry(),burst,static_cast<std::uint32_t>(r.task));
        }
        return {true,{}};
    }
    throw std::runtime_error("Unknown engine script command: " + r.command);
}
void GameSession::Impl::partyPanel(Rect r,bool detail) {
    ui.window(r);
    const int columns=state.party.size()>2?2:1;const int rows=(static_cast<int>(state.party.size())+columns-1)/columns;
    const float cellWidth=(r.w-20)/columns,cellHeight=(r.h-20)/std::max(1,rows);int index=0;
    for (const auto& id:state.party) {
        const float x=r.x+10+(index%columns)*cellWidth,y=r.y+10+(index/columns)*cellHeight;
        const auto& a=state.actor(id);const auto s=state.stats(db,a);
        ui.text(db.actors.at(id).name,x,y,0xffdf9fff,10);
        ui.text("Lv "+number(a.level),x+cellWidth-36,y,0xc5d5eeff,8);
        ui.text("HP "+number(a.hp)+" / "+number(s.hp),x,y+15,0xe5edfaff,8);
        ui.gauge({x,y+27,cellWidth-10,5},float(a.hp)/s.hp,0x70d4a3ff);
        if (detail) {
            ui.text("MP "+number(a.mp)+" / "+number(s.mp),x,y+37,0xc8d7ffff,8);
            ui.gauge({x,y+49,cellWidth-10,5},s.mp?float(a.mp)/s.mp:0,0x82aaf7ff);
        }
        ++index;
    }
}
void GameSession::Impl::updateMenu() {
    auto& cursor=cursors[menuPage];
    if (rebinding) {
        const auto key=input.lastKey();
        if (key!=SDL_SCANCODE_UNKNOWN) {
            try {
                if (key!=SDL_SCANCODE_ESCAPE) input.rebind(*rebinding,key);
                rebinding.reset();input.clearLastKey();savePreferences();status="Controls saved";
            } catch (const std::exception& e) { status=e.what();input.clearLastKey(); }
        }
        return;
    }
    if (actions.hit(Action::Cancel) || actions.hit(Action::Menu)) {
        if (menuPage=="Load" && has(Screen::Title)) scenes.pop();
        else if (menuPage=="Main") { savePreferences();scenes.pop(); }
        else { menuPage="Main";status.clear(); }
        return;
    }
    if (menuPage!="Main" && menuPage!="Config" && menuPage!="Save" && menuPage!="Load") {
        if (actions.hit(Action::Previous)) menuActor=(menuActor+int(state.party.size())-1)%int(state.party.size());
        if (actions.hit(Action::Next)) menuActor=(menuActor+1)%int(state.party.size());
    }
    navigate(cursor);
    try {
        if (menuPage=="Main") {
            const std::vector<std::string> pages{"Items","Magic","Equip","Status","Config","Save"};cursor.setCount(static_cast<int>(pages.size()));
            if (actions.hit(Action::Confirm)) { menuPage=pages[cursor.selected()];status.clear(); }
        } else if (menuPage=="Items") {
            std::vector<std::string> items;for (const auto& [id,count]:state.inventory) if (count>0 && db.items.at(id).type!="equipment") items.push_back(id);
            cursor.setCount(static_cast<int>(items.size()));
            if (actions.hit(Action::Confirm) && !items.empty()) status=state.useItem(db,items[cursor.selected()],state.party[menuActor])?"Used "+db.items.at(items[cursor.selected()]).name:"That has no effect";
        } else if (menuPage=="Magic") {
            auto& caster=state.actor(state.party[menuActor]);cursor.setCount(static_cast<int>(caster.skills.size()));
            if (hit(Action::Left)) menuTarget=(menuTarget+int(state.party.size())-1)%int(state.party.size());
            if (hit(Action::Right)) menuTarget=(menuTarget+1)%int(state.party.size());
            if (actions.hit(Action::Confirm) && !caster.skills.empty()) {
                const auto& skill=db.skills.at(caster.skills[cursor.selected()]);bool changed=false;
                if (caster.hp<=0 || caster.mp<skill.mpCost) { status="Not enough MP or caster is KO";return; }
                if (skill.target!="ally" && skill.target!="all_allies" && skill.target!="dead_ally" && skill.target!="self") { status="That spell is for battle";return; }
                std::vector<std::string> targets=skill.target=="all_allies"?state.party:std::vector<std::string>{skill.target=="self"?caster.id:state.party[menuTarget]};
                for (const auto& id:targets) {
                    auto& target=state.actor(id);const auto stats=state.stats(db,target);
                    for (const auto& effect:skill.effects) {
                        if (effect.type=="heal" && target.hp>0) {const int hp=std::min(stats.hp,target.hp+effect.power);changed|=hp!=target.hp;target.hp=hp;}
                        if (effect.type=="revive" && target.hp==0) {target.hp=std::clamp(effect.power,1,stats.hp);changed=true;}
                        if (effect.type=="remove_state") changed|=target.states.erase(effect.state)>0;
                    }
                }
                if (changed) {caster.mp-=skill.mpCost;status="Cast "+skill.name;} else status="That has no effect";
            }
        } else if (menuPage=="Equip") {
            std::vector<std::string> items;for (const auto& [id,count]:state.inventory) if (count>0 && db.items.at(id).type=="equipment") items.push_back(id);
            cursor.setCount(static_cast<int>(items.size())+2);
            if (actions.hit(Action::Confirm)) {
                const auto selected=cursor.selected();const auto& id=state.party[menuActor];
                const bool equipped=selected<2?state.equip(db,id,selected==0?"weapon":"armor",""):state.equip(db,id,db.items.at(items[selected-2]).slot,items[selected-2]);
                status=equipped?"Equipment updated":"Cannot equip that item";
            }
        } else if (menuPage=="Config") {
            cursor.setCount(static_cast<int>(ActionCount)+4);
            const int selected=cursor.selected();
            if (selected<3) {
                const auto bus=static_cast<AudioBus>(selected);
                if (hit(Action::Left)) audio.setVolume(bus,audio.volume(bus)-.05f);
                if (hit(Action::Right)) audio.setVolume(bus,audio.volume(bus)+.05f);
            } else if (selected==3) {
                if (actions.hit(Action::Confirm)||hit(Action::Left)||hit(Action::Right)) atbMode=atbMode==AtbMode::Wait?AtbMode::Active:AtbMode::Wait;
            } else if (actions.hit(Action::Confirm)) {rebinding=static_cast<Action>(selected-4);input.clearLastKey();status="Press a key (Escape cancels)";}
        } else if (menuPage=="Save" || menuPage=="Load") {
            cursor.setCount(menuPage=="Load"?3:6);
            if (actions.hit(Action::Confirm)) {
                const int selected=cursor.selected();
                if (menuPage=="Load") loadSlot(selected+1);
                else if (selected<3) {state.validate(db);saves.save(selected+1,state);status="Saved slot "+number(selected+1);}
                else loadSlot(selected-2);
            }
        }
    } catch (const std::exception& e) {status=e.what();log(LogLevel::Error,status);}
}
void GameSession::Impl::drawMenu() {
    const float w=float(ui.width()),h=float(ui.height());
    ui.fillScreen(0x07102180);
    const auto header=ui.layout("menu_header",{8,8,w-16,20});ui.window(header);ui.text(menuPage,header.x+9,header.y+6,0xffdf9fff,9);
    ui.text(number(state.gold)+" gold    "+clockText(state.playtime),header.x+header.w-122,header.y+6,0xe5edfaff,8);
    auto& cursor=cursors[menuPage];
    if (menuPage=="Main") {
        const auto navigation=ui.layout("menu_navigation",{8,32,108,h-65});ui.window(navigation);
        ui.list({"Items","Magic","Equip","Status","Config","Save"},cursor,{navigation.x+8,navigation.y+11,navigation.w-16,navigation.h-20},8);
        partyPanel(ui.layout("menu_party",{122,32,w-130,h-65}));
    } else if (menuPage=="Items" || menuPage=="Magic" || menuPage=="Equip") {
        const auto& actor=state.actor(state.party[menuActor]);
        const auto list=ui.layout("menu_list",{8,32,w*.56f,h-65});ui.window(list);std::vector<std::string> labels;
        if (menuPage=="Items") for (const auto& [id,count]:state.inventory) if (count>0 && db.items.at(id).type!="equipment") labels.push_back(db.items.at(id).name+"  x"+number(count));
        if (menuPage=="Magic") {
            for (const auto& id:actor.skills) labels.push_back(db.skills.at(id).name+"  "+number(db.skills.at(id).mpCost)+" MP");
            ui.text("Caster: "+db.actors.at(actor.id).name+"  Target: "+db.actors.at(state.party[menuTarget]).name,list.x+8,list.y+5,0xffdf9fff,7);
        }
        if (menuPage=="Equip") {
            labels={"Unequip weapon","Unequip armor"};
            for (const auto& [id,count]:state.inventory) if (count>0 && db.items.at(id).type=="equipment") labels.push_back(db.items.at(id).name+" x"+number(count));
        }
        const float top=menuPage=="Magic"?20.0f:10.0f;
        ui.list(labels,cursor,{list.x+8,list.y+top,list.w-16,list.h-top-10},std::max(1,int((list.h-top-10)/13)));
        const auto actorRect=ui.layout("menu_actor",{w*.56f+22,32,w*.44f-30,h-65});const float x=actorRect.x;ui.window(actorRect);
        const auto attributes=state.stats(db,actor);
        ui.text(db.actors.at(actor.id).name,x+8,actorRect.y+11,0xffdf9fff,10);
        ui.text("HP "+number(actor.hp)+" / "+number(attributes.hp),x+8,actorRect.y+29);
        ui.text("MP "+number(actor.mp)+" / "+number(attributes.mp),x+8,actorRect.y+42);
        if (menuPage=="Equip") {
            float y=actorRect.y+64;for (const auto& [slot,id]:actor.equipment) {ui.text(slot+": "+db.items.at(id).name,x+8,y,0xd5deefff,7);y+=13;}
            ui.text("Attack "+number(attributes.attack)+"  Defense "+number(attributes.defense),x+8,y+8,0xd5deefff,7);
        }
        ui.text("Q/E: change actor",x+8,actorRect.y+actorRect.h-24,0xb4c5e4ff,7);
    } else if (menuPage=="Status") {
        const auto& a=state.actor(state.party[menuActor]);const auto s=state.stats(db,a);
        const auto panel=ui.layout("status_panel",{8,32,w-16,h-65});ui.window(panel);ui.text(db.actors.at(a.id).name+"   Level "+number(a.level),panel.x+12,panel.y+12,0xffdf9fff,12);
        ui.text("Job: "+(a.job.empty()?"None":db.jobs.at(a.job).name)+"   EXP: "+number(a.exp),panel.x+12,panel.y+33);
        const auto attributes=s.json();float y=panel.y+53;for (const auto& [stat,value]:attributes.items()) {ui.text(stat+": "+number(value.get<int>()),panel.x+12,y);y+=13;}
        y=panel.y+54;for (const auto& [id,duration]:a.states) {ui.text(db.states.at(id).name+" ("+number(duration)+")",panel.x+panel.w*.5f,y,0xffb99fff);y+=13;}
        ui.text("Q/E: change actor",panel.x+panel.w-107,panel.y+panel.h-16,0xb4c5e4ff,7);
    } else if (menuPage=="Config") {
        const auto panel=ui.layout("config_panel",{8,32,w-16,h-65});ui.window(panel);
        std::vector<std::string> labels{"Master volume: "+number(int(audio.volume(AudioBus::Master)*100))+"%",
            "Music volume: "+number(int(audio.volume(AudioBus::Music)*100))+"%","Effects volume: "+number(int(audio.volume(AudioBus::Effects)*100))+"%",
            std::string("ATB mode: ")+(atbMode==AtbMode::Wait?"Wait":"Active")};
        for (std::size_t i=0;i<ActionCount;++i) labels.push_back(std::string(Input::name(static_cast<Action>(i)))+": "+SDL_GetScancodeName(input.binding(static_cast<Action>(i))));
        ui.list(labels,cursor,{panel.x+10,panel.y+9,panel.w-20,panel.h-15},std::max(1,int((panel.h-18)/13)));
    } else if (menuPage=="Save" || menuPage=="Load") {
        const auto panel=ui.layout("save_panel",{8,32,w-16,h-65});ui.window(panel);std::vector<std::string> labels;
        if (menuPage=="Save") for (int i=1;i<=3;++i) labels.push_back("Save slot "+number(i)+(saves.exists(i)?" (occupied)":" (empty)"));
        for (int i=1;i<=3;++i) labels.push_back("Load slot "+number(i)+(saves.exists(i)?"":" (empty)"));
        ui.list(labels,cursor,{panel.x+12,panel.y+13,panel.w-24,panel.h-20},menuPage=="Load"?3:6);
    }
    const auto footer=ui.layout("menu_footer",{8,h-28,w-16,20});ui.window(footer);ui.text(status.empty()?"Confirm: choose    Cancel: back":status,footer.x+8,footer.y+6,0xc5d5eeff,7);
}
void GameSession::Impl::updateShop() {
    if (actions.hit(Action::Cancel)||actions.hit(Action::Menu)) {shopFinished=true;scenes.pop();return;}
    if (actions.hit(Action::Left)||actions.hit(Action::Right)) {shopSelling=!shopSelling;cursors["Shop"].select(0);}
    auto& cursor=cursors["Shop"];navigate(cursor);
    std::vector<std::string> items;
    if (shopSelling) {for (const auto& [id,count]:state.inventory) if (count>0 && db.items.at(id).type!="key") items.push_back(id);}
    else items=db.shops.at(shopId).items;
    cursor.setCount(static_cast<int>(items.size()));
    if (actions.hit(Action::Confirm) && !items.empty()) {
        const auto& id=items[cursor.selected()];
        const bool success=shopSelling?state.sell(db,shopId,id):state.buy(db,shopId,id);
        status=success?(shopSelling?"Sold ":"Bought ")+db.items.at(id).name:"Not enough gold, items, or inventory space";
    }
}
void GameSession::Impl::drawShop() {
    const float w=float(ui.width()),h=float(ui.height());const auto panel=ui.layout("shop_window",{14,18,w-28,h-36});ui.window(panel);
    ui.text(std::string("SHOP   ")+(shopSelling?"Sell":"Buy"),panel.x+10,panel.y+11,0xffdf9fff,11);ui.text(number(state.gold)+" gold",panel.x+panel.w-76,panel.y+12,0xffdf9fff);
    const auto& shop=db.shops.at(shopId);std::vector<std::string> labels;
    if (shopSelling) {
        for (const auto& [id,count]:state.inventory) if (count>0 && db.items.at(id).type!="key") labels.push_back(db.items.at(id).name+" x"+number(count)+"   "+number(int(std::floor(db.items.at(id).price*shop.sellRate)))+" G");
    } else for (const auto& id:shop.items) labels.push_back(db.items.at(id).name+"   "+number(int(std::ceil(db.items.at(id).price*shop.buyRate)))+" G");
    ui.list(labels,cursors["Shop"],{panel.x+12,panel.y+36,panel.w-24,panel.h-54},std::max(1,int((panel.h-74)/13)));
    ui.text(status.empty()?"Left/Right: buy/sell   Confirm: trade   Cancel: leave":status,panel.x+10,panel.y+panel.h-16,0xc5d5eeff,7);
}
void GameSession::Impl::startBattle(const std::string& troopId,std::uint64_t owner) {
    if (battle) return;
    auto serial=state.variable("battle_serial")+1;state.variables["battle_serial"]=serial;
    auto replacement=std::make_unique<Battle>(db,state,troopId,static_cast<std::uint32_t>(serial));
    replacement->mode=atbMode;
    replacement->setFormula([this](const auto& name,const auto& a,const auto& target,int power) {return scripts.formula(name,a,target,power);});
    replacement->setAi([this](const auto& current,int index) {return scripts.ai(current,index);});
    replacement->setScriptEffect([this](const auto& name,const auto& a,const auto& target,int power) {return scripts.effect(name,a,target,power);});
    const auto& troop=db.troops.at(troopId);
    auto map=MapData::load(gamePath(root,"maps/"+troop.stage+"/map.json"));
    map.entities.clear();map.encounters.clear();map.camera.pitch=27;map.camera.distance=24;map.camera.fov=30;map.camera.yaw=0;
    map.camera.min={0,0};map.camera.max={16,14};
    auto backdrop=std::make_unique<World>(std::move(map),state,db,scripts);
    // The battle stage is a presentation world, so never write its spawn into
    // the field save state. Only the visible stage player's transform changes.
    auto& stagePlayer=backdrop->registry().get<Transform>(backdrop->player());stagePlayer.position=stagePlayer.previous={8,0,7};
    backdrop->registry().get<SpriteRenderer>(backdrop->player()).visible=false;
    std::vector<entt::entity> actors;std::vector<glm::vec3> positions;std::vector<int> health;
    int allies=0,enemies=0;
    for (const auto& b:replacement->battlers()) {
        const int index=b.enemy?enemies++:allies++;
        const float partyZ=9.2f-std::max(0,static_cast<int>(state.party.size())-2)*1.3f;
        const glm::vec3 position=b.enemy?glm::vec3(3.7f+(index/4)*1.25f,0,9.3f-(index%4)*1.25f):glm::vec3(11.5f+index*.35f,0,partyZ-index*1.15f);
        const auto e=backdrop->registry().create();actors.push_back(e);positions.push_back(position);health.push_back(b.hp);
        backdrop->registry().emplace<Transform>(e,position,position,b.enemy?glm::vec2(1,0):glm::vec2(-1,0));
        backdrop->registry().emplace<SpriteRenderer>(e,b.sprite);
    }
    battle=std::move(replacement);stage=std::move(backdrop);stageActors=std::move(actors);stagePositions=std::move(positions);visualHp=std::move(health);
    healthTweens.clear();for (int hp:visualHp) healthTweens.emplace_back(hp);
    stageDestinations=stagePositions;
    presenting=false;battleFinished=false;battleResult=BattleOutcome::Ongoing;battleOwner=owner;commandUser=-1;battleMenu="Command";popups.clear();status.clear();
    audio.playBgm(gamePath(root,"audio/battle.wav"));graphics.updateCamera(*stage,0,true);push(Screen::Battle);
}
void GameSession::Impl::beginPresentation(std::vector<BattleEvent> events) {
    if (events.empty()) return;
    presentation=std::move(events);presenting=true;effectsShown=false;presentingUser=-1;
    std::string timeline;
    for (const auto& event:presentation) if (event.kind==BattleEventKind::Action) {
        presentingUser=event.user;status=event.text;
        timeline="attack";if (db.skills.contains(event.actionId)) timeline=db.skills.at(event.actionId).timeline;
        break;
    }
    if (timeline.empty() && !presentation.empty() && !presentation.front().text.empty()) status=presentation.front().text;
    if (timelines.contains(timeline)) sequence.start(timelines.at(timeline));
    else sequence.start({{{0,"effect",Json::object()},{.35,"return",Json::object()}},.5});
}
void GameSession::Impl::showEffects() {
    if (effectsShown) return;effectsShown=true;
    for (const auto& event:presentation) {
        if (event.target<0 || event.target>=static_cast<int>(visualHp.size())) continue;
        const auto& b=battle->battlers()[event.target];
        if (event.kind==BattleEventKind::Damage) {visualHp[event.target]=std::max(0,visualHp[event.target]-event.amount);popups.push_back({event.target,event.amount,false,event.text,0});}
        if (event.kind==BattleEventKind::Heal) {
            if (event.text!="MP") visualHp[event.target]=std::min(b.stats.hp,visualHp[event.target]+event.amount);
            popups.push_back({event.target,event.amount,true,event.text,0});
        }
        if (event.kind==BattleEventKind::Miss || event.kind==BattleEventKind::State) popups.push_back({event.target,0,false,event.text,0});
        stage->registry().get<SpriteRenderer>(stageActors[event.target]).visible=visualHp[event.target]>0;
        healthTweens[event.target].target(visualHp[event.target]);
        if (event.kind==BattleEventKind::Damage || event.kind==BattleEventKind::Heal) {
            ParticleBurst burst;burst.sprite=config.battleEffectSprite;burst.position=stagePositions[event.target]+glm::vec3(0,.65f,0);
            burst.color=event.kind==BattleEventKind::Heal?glm::vec4(.4f,2,.8f,1):glm::vec4(2,1.1f,.4f,1);
            emitParticles(stage->registry(),burst,static_cast<std::uint32_t>(event.target+state.variable("battle_serial")*13));
        }
    }
}
void GameSession::Impl::updateBattle(double dt) {
    if (!battle || !stage) return;
    stage->update(dt,{},false,true);
    for (auto& tween:healthTweens) tween.update(dt);
    for (auto& popup:popups) popup.age+=dt;
    std::erase_if(popups,[](const auto& popup){return popup.age>1.25;});
    for (std::size_t i=0;i<stageActors.size();++i) {
        auto& t=stage->registry().get<Transform>(stageActors[i]);
        t.position=glm::mix(t.position,stageDestinations[i],static_cast<float>(1-std::exp(-dt*12)));
    }
    if (presenting) {
        for (const auto& event:sequence.update(dt)) {
            if (event.command=="effect") showEffects();
            if (event.command=="flash") flash=.45f;
            if (event.command=="shake") {shakeTime=event.parameters.value("duration",.3);shakeAmplitude=event.parameters.value("amplitude",.1f);}
            if (event.command=="sfx" && event.parameters.contains("file")) audio.playSfx(gamePath(root,event.parameters.at("file")));
            if (presentingUser>=0) {
                auto& sprite=stage->registry().get<SpriteRenderer>(stageActors[presentingUser]);
                if (event.command=="step") {stageDestinations[presentingUser]=stagePositions[presentingUser];stageDestinations[presentingUser].x+=(battle->battlers()[presentingUser].enemy?1:-1)*event.parameters.value("distance",.7f);}
                if (event.command=="animate") {sprite.clip=event.parameters.value("clip","attack");sprite.elapsed=0;sprite.previousElapsed=-1e-8;}
                if (event.command=="return") {sprite.clip="idle";stageDestinations[presentingUser]=stagePositions[presentingUser];}
            }
        }
        if (sequence.complete()) {showEffects();presenting=false;presentation.clear();}
        return;
    }
    if (battle->outcome()!=BattleOutcome::Ongoing) {
        if (actions.hit(Action::Confirm)||actions.hit(Action::Cancel)) closeBattle();
        return;
    }
    if (commandUser<0 || !battle->battlers()[commandUser].ready()) {
        commandUser=-1;
        for (std::size_t i=0;i<battle->battlers().size();++i) if (!battle->battlers()[i].enemy && battle->battlers()[i].ready()) {commandUser=static_cast<int>(i);battleMenu="Command";break;}
    }
    if (commandUser>=0) {
        auto& cursor=cursors["Battle"+battleMenu];navigate(cursor);
        const auto& user=battle->battlers()[commandUser];
        if (actions.hit(Action::Cancel) && battleMenu!="Command") battleMenu="Command";
        else if (battleMenu=="Command") {
            cursor.setCount(5);
            if (actions.hit(Action::Confirm)) {
                const int selected=cursor.selected();
                if (selected==0) {battleKind=ActionKind::Skill;battleSelection="attack";battleMenu="Target";cursors["BattleTarget"].select(0);}
                if (selected==1) battleMenu="Skills";
                if (selected==2) battleMenu="Items";
                if (selected>=3 && battle->submit({commandUser,selected==3?ActionKind::Guard:ActionKind::Escape,"",-1})) commandUser=-1;
            }
        } else if (battleMenu=="Skills" || battleMenu=="Items") {
            std::vector<std::string> choices;
            if (battleMenu=="Skills") choices=user.skills;
            else for (const auto& [id,count]:state.inventory) if (count>0 && db.items.at(id).type=="consumable") choices.push_back(id);
            cursor.setCount(static_cast<int>(choices.size()));
            if (actions.hit(Action::Confirm) && !choices.empty()) {
                battleKind=battleMenu=="Skills"?ActionKind::Skill:ActionKind::Item;battleSelection=choices[cursor.selected()];battleMenu="Target";cursors["BattleTarget"].select(0);
            }
        } else if (battleMenu=="Target") {
            std::string targetType="ally";
            if (battleKind==ActionKind::Skill) targetType=db.skills.at(battleSelection).target;
            else for (const auto& effect:db.items.at(battleSelection).effects) if (effect.type=="revive") targetType="dead_ally";
            const auto candidates=battle->targets(commandUser,targetType);cursor.setCount(static_cast<int>(candidates.size()));
            if (actions.hit(Action::Confirm) && !candidates.empty()) {
                if (battle->submit({commandUser,battleKind,battleSelection,candidates[cursor.selected()]})) {commandUser=-1;battleMenu="Command";status.clear();}
                else status="Not enough MP/items, or that command is unavailable";
            }
        }
    }
    battle->update(dt,commandUser>=0);
    beginPresentation(battle->takeEvents());
}
void GameSession::Impl::animationEvents(World& world) {
    for (auto e:world.registry().view<Transform,SpriteRenderer>()) {
        const auto& sprite=world.registry().get<SpriteRenderer>(e);if (!sprite.visible || sprite.animation.empty()) continue;
        if (failedAnimations.contains(sprite.animation)) continue;
        try {
            if (!animations.contains(sprite.animation)) animations[sprite.animation]=SpriteAnimation::load(gamePath(root,sprite.animation));
            const auto& animation=animations.at(sprite.animation);
            const auto events=animation.events(sprite.clip,animation.direction(sprite.clip,world.registry().get<Transform>(e).facing,graphics.camera().yaw()),sprite.previousElapsed,sprite.elapsed);
            for (const auto& event:events) {
                if (auto found=soundEvents.find(event);found!=soundEvents.end()) audio.playSfx(gamePath(root,found->second),.3f);
                else if (event.starts_with("sfx:")) audio.playSfx(gamePath(root,event.substr(4)));
            }
        } catch (const std::exception& error) {failedAnimations.insert(sprite.animation);log(LogLevel::Error,error.what());}
    }
}
void GameSession::Impl::drawBattle(double alpha) {
    if (!battle || !stage) return;
    graphics.render(*stage,alpha,viewportWidth,viewportHeight);
    const float w=float(ui.width()),h=float(ui.height());
    const auto& battlers=battle->battlers();
    auto projected=[&](int target) {
        const auto p=stage->registry().get<Transform>(stageActors[target]).position+glm::vec3(0,1.6f,0);
        const auto clip=graphics.camera().projection(float(viewportWidth)/viewportHeight,bgfx::getCaps()->homogeneousDepth)*graphics.camera().view()*glm::vec4(p,1);
        return ui.fromViewport({clip.x/clip.w*.5f+.5f,-clip.y/clip.w*.5f+.5f});
    };
    for (const auto& popup:popups) {
        auto position=projected(popup.target);position.y-=static_cast<float>(popup.age)*15;
        const auto label=popup.amount?number(popup.amount):popup.text;
        ui.text(label,position.x-8,position.y,popup.heal?0x95ffc0ff:0xffe9d2ff,12);
    }
    const int partyCount=static_cast<int>(std::count_if(battlers.begin(),battlers.end(),[](const auto& b){return !b.enemy;}));
    const float partyHeight=partyCount*24.0f+20;
    const auto partyRect=ui.layout("battle_party",{w*.46f,h-partyHeight-8,w*.54f-8,partyHeight});ui.window(partyRect);
    int row=0;
    for (std::size_t i=0;i<battlers.size();++i) if (!battlers[i].enemy) {
        const auto& b=battlers[i];const float y=partyRect.y+9+row*24;
        ui.text(b.name,partyRect.x+9,y,int(i)==commandUser?0xffdf8aff:0xe5edfaff,8);
        ui.text("HP "+number(static_cast<int>(std::round(healthTweens[i].value())))+"  MP "+number(b.mp),partyRect.x+68,y,0xe5edfaff,7);
        ui.gauge({partyRect.x+9,y+12,partyRect.w-20,4},static_cast<float>(b.gauge),b.ready()?0xffd47aff:0x88aff2ff);++row;
    }
    if (battle->outcome()!=BattleOutcome::Ongoing && !presenting) {
        ui.window({w*.5f-130,36,260,95});
        const auto outcome=battle->outcome();
        ui.text(outcome==BattleOutcome::Victory?"VICTORY":outcome==BattleOutcome::Defeat?"THE PARTY HAS FALLEN":"ESCAPED",w*.5f-72,49,0xffdf8aff,12);
        if (outcome==BattleOutcome::Victory) {
            const auto& rewards=battle->rewards();ui.text(number(rewards.experience)+" EXP    "+number(rewards.gold)+" gold",w*.5f-65,72);
            float y=86;for (const auto& id:rewards.drops) {ui.text(db.items.at(id).name,w*.5f-65,y,0xcbe2faff,7);y+=10;}
        }
        ui.text("Confirm to continue",w*.5f-48,112,0xcbe2faff,7);return;
    }
    if (!status.empty()) {ui.window({8,8,w-16,19});ui.text(status,17,13,0xffdf8aff,8);}
    if (commandUser<0 || presenting) return;
    auto& cursor=cursors["Battle"+battleMenu];std::vector<std::string> labels;
    if (battleMenu=="Command") labels={"Attack","Skills","Items","Guard","Escape"};
    if (battleMenu=="Skills") for (const auto& id:battlers[commandUser].skills) labels.push_back(db.skills.at(id).name+" "+number(db.skills.at(id).mpCost)+" MP");
    if (battleMenu=="Items") for (const auto& [id,count]:state.inventory) if (count>0 && db.items.at(id).type=="consumable") labels.push_back(db.items.at(id).name+" x"+number(count));
    if (battleMenu=="Target") {
        std::string type=battleKind==ActionKind::Skill?db.skills.at(battleSelection).target:"ally";
        if (battleKind==ActionKind::Item) for (const auto& effect:db.items.at(battleSelection).effects) if (effect.type=="revive") type="dead_ally";
        const auto targets=battle->targets(commandUser,type);
        for (int index:targets) labels.push_back(battlers[index].name);
        if (!targets.empty()) {
            cursor.setCount(static_cast<int>(targets.size()));const auto p=projected(targets[cursor.selected()]);ui.text("v",p.x-3,p.y-12,0xffdf8aff,12);
        }
    }
    const int rows=std::min(6,std::max(1,int(labels.size())));
    const float menuHeight=float(rows*13+14);const auto menuRect=ui.layout("battle_command",{8,h-menuHeight-8,w*.43f-8,menuHeight});ui.window(menuRect);
    ui.list(labels,cursor,{menuRect.x+9,menuRect.y+8,menuRect.w-16,menuRect.h-12},rows);
}
void GameSession::Impl::closeBattle() {
    battleResult=battle->outcome();battleFinished=true;const auto result=battleResult;
    battle.reset();stage.reset();stageActors.clear();popups.clear();scenes.pop();commandUser=-1;
    if (!pendingDatabase.empty()) {
        const auto path=pendingDatabase;pendingDatabase.clear();
        try {reloadDatabase(path);} catch (const std::exception& error) {log(LogLevel::Error,error.what());}
    }
    graphics.updateCamera(*field,0,true);
    if (!field->map().bgm.empty()) audio.playBgm(gamePath(root,field->map().bgm));
    if (result==BattleOutcome::Defeat) {
        scripts.cancelAll();battleOwner=0;newGame();scenes.clear();push(Screen::Field);push(Screen::Title);status="The party fell. A saved adventure can still be continued.";
    }
}
void GameSession::Impl::reloadMap() {
    const auto position=state.position;
    auto replacement=MapData::load(root/"maps"/field->map().id/"map.json");
    if (!replacement.nav.canStand({position.x,position.z})) throw std::runtime_error("Edited map blocks the current player position");
    auto world=std::make_unique<World>(std::move(replacement),state,db,scripts);world->restorePosition(position);
    scripts.cancelAll();dialogueOwner=shopOwner=battleOwner=0;movements.clear();battle.reset();stage.reset();
    field=std::move(world);scenes.clear();push(Screen::Field);graphics.updateCamera(*field,0,true);ui.reload();
}
void GameSession::Impl::reloadDatabase(const std::filesystem::path& path) {
    auto replacement=Database::load(path);state.validate(replacement);
    for (const auto& [region,table]:field->map().encounters) {(void)region;if (!replacement.encounters.contains(table)) throw std::runtime_error("Current map references missing encounter table "+table);}
    db=std::move(replacement);
}

GameSession::GameSession(std::filesystem::path root,std::filesystem::path data,std::filesystem::path shaders,GameConfig config,bool mute,bool skipTitle)
    : impl_(std::make_unique<Impl>(std::move(root),std::move(data),std::move(shaders),std::move(config),mute,skipTitle)) {}
GameSession::~GameSession()=default;
void GameSession::processEvent(const SDL_Event& event) { impl_->input.processEvent(event); }
void GameSession::update(double dt,bool paused) { impl_->update(dt,paused); }
void GameSession::render(double alpha,int width,int height) {
    auto& p=*impl_;p.viewportWidth=width;p.viewportHeight=height;p.ui.begin();p.scenes.render(alpha);
    if (p.flash>0) p.ui.fillScreen(0xc3e8ff00|static_cast<std::uint32_t>(std::clamp(p.flash,0.0f,1.0f)*180));
    if (p.fade>0) p.ui.fillScreen(static_cast<std::uint32_t>(std::clamp(p.fade,0.0f,1.0f)*255));
}
World& GameSession::world() { return *impl_->field; }
GameState& GameSession::state() { return impl_->state; }
Database& GameSession::database() { return impl_->db; }
ScriptEngine& GameSession::scripts() { return impl_->scripts; }
WorldRenderer& GameSession::renderer() { return impl_->graphics; }
const std::filesystem::path& GameSession::gameRoot() const { return impl_->root; }
void GameSession::startBattle(const std::string& troop) { impl_->startBattle(troop); }
void GameSession::reloadMap() { impl_->reloadMap(); }
void GameSession::rebuildEntities() {
    auto& p=*impl_;const auto position=p.field->playerPosition();
    p.scripts.cancelAll();p.dialogueOwner=p.shopOwner=p.battleOwner=0;p.battle.reset();p.stage.reset();p.movements.clear();
    p.field->rebuildEntities();
    if (p.field->map().nav.canStand({position.x,position.z})) p.field->restorePosition(position);
    else p.field->spawn(p.field->map().spawns.begin()->first);
    p.scenes.clear();p.push(Impl::Screen::Field);
}
void GameSession::playFrom(glm::vec3 position) {
    auto& p=*impl_;p.field->restorePosition(position);p.scripts.cancelAll();p.dialogueOwner=p.shopOwner=p.battleOwner=0;
    p.battle.reset();p.stage.reset();p.movements.clear();p.scenes.clear();p.push(Impl::Screen::Field);p.graphics.updateCamera(*p.field,0,true);
}
void GameSession::reload(const std::filesystem::path& file) {
    auto& p=*impl_;
    try {
        if (file.filename()=="database.json") {
            if (p.battle) {p.pendingDatabase=file;log(LogLevel::Info,"Database reload queued until battle ends");return;}
            p.reloadDatabase(file);
        } else if (file==p.root/"maps"/p.field->map().id/"map.json") p.reloadMap();
        else if (file.filename()=="rules.lua") p.scripts.loadRules("scripts/rules.lua");
        else if (file.filename()=="timelines.json") p.timelines=ActionTimeline::load(file);
        else if (file.filename()=="events.json" && file.parent_path()==p.root/"audio") p.soundEvents=readJson(file).get<std::map<std::string,std::string>>();
        else if (file.parent_path()==p.root/"ui") p.ui.reload();
        p.graphics.reload(file);
        p.audio.reload(file);
        if (file.extension()==".json") {
            const auto key=file.lexically_relative(p.root).generic_string();p.animations.erase(key);p.failedAnimations.erase(key);
        }
    } catch (const std::exception& e) { p.status=e.what();log(LogLevel::Error,"Reload kept existing data: "+p.status); }
}
}
