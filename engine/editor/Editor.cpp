#include "engine/editor/Editor.h"
#include "engine/core/Log.h"
#include "engine/render/WorldRenderer.h"
#include "engine/scene/GameSession.h"
#include "engine/world/World.h"
#include "engine/assets/Mesh.h"
#include <imgui.h>
#include <bgfx/bgfx.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace fe {
namespace {
bool editString(const char* label,std::string& value,bool applyOnEnter=false) {
    std::array<char,1024> buffer{};std::snprintf(buffer.data(),buffer.size(),"%s",value.c_str());
    if (!ImGui::InputText(label,buffer.data(),buffer.size(),applyOnEnter?ImGuiInputTextFlags_EnterReturnsTrue:0)) return false;
    value=buffer.data();return true;
}
void cameraControls(CameraSettings& camera) {
    ImGui::SliderFloat("Pitch",&camera.pitch,5,85);ImGui::SliderFloat("Yaw",&camera.yaw,-180,180);
    ImGui::SliderFloat("Distance",&camera.distance,3,60);ImGui::SliderFloat("FOV",&camera.fov,10,80);
    ImGui::DragFloat2("Follow minimum",glm::value_ptr(camera.min),.1f);
    ImGui::DragFloat2("Follow maximum",glm::value_ptr(camera.max),.1f);
}
}
Editor::Editor(GameSession& game):game_(game) {}
void Editor::checkpoint() {
    if (undo_.size()>=64) undo_.erase(undo_.begin());undo_.push_back(game_.world().map().toJson());redo_.clear();dirty_=true;
}
void Editor::restore(const Json& source) {
    game_.world().map()=MapData::fromJson(source);game_.rebuildEntities();dirty_=true;
}
void Editor::save() {
    try {
        const auto& map=game_.world().map();
        // Round-trip validation runs before replacing the authored file.
        const auto source=map.toJson();MapData::fromJson(source);
        writeJson(game_.gameRoot()/"maps"/map.id/"map.json",source);dirty_=false;status_="Map saved";
    } catch (const std::exception& error) {status_=error.what();log(LogLevel::Error,status_);}
}
void Editor::draw(std::uint64_t ticks) {
    if (!open_) return;
    if (mapId_!=game_.world().map().id) {mapId_=game_.world().map().id;undo_.clear();redo_.clear();dirty_=false;entity_=0;}
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x-390,20},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({370,std::max(300.0f,std::min(650.0f,ImGui::GetIO().DisplaySize.y-40))},ImGuiCond_FirstUseEver);
    if (ImGui::Begin("f-engine Editor (F1)",&open_)) {
        ImGui::Text("%s%s",game_.world().map().name.c_str(),dirty_?" *":"");
        ImGui::Checkbox("Pause game",&paused_);ImGui::SameLine();ImGui::Checkbox("Nav overlay",&showGrid_);
        if (ImGui::Button("Save map")) save();ImGui::SameLine();
        if (ImGui::Button("Undo") && !undo_.empty()) {
            try {redo_.push_back(game_.world().map().toJson());auto source=undo_.back();undo_.pop_back();restore(source);}
            catch (const std::exception& error) {status_=error.what();}
        }
        ImGui::SameLine();
        if (ImGui::Button("Redo") && !redo_.empty()) {
            try {undo_.push_back(game_.world().map().toJson());auto source=redo_.back();redo_.pop_back();restore(source);}
            catch (const std::exception& error) {status_=error.what();}
        }
        if (!status_.empty()) ImGui::TextWrapped("%s",status_.c_str());
        if (ImGui::BeginTabBar("Tools")) {
            if (ImGui::BeginTabItem("Map")) {drawMap();ImGui::EndTabItem();}
            if (ImGui::BeginTabItem("Entities")) {drawEntities();ImGui::EndTabItem();}
            if (ImGui::BeginTabItem("Light")) {drawLighting();ImGui::EndTabItem();}
            if (ImGui::BeginTabItem("Camera")) {drawCamera();ImGui::EndTabItem();}
            if (ImGui::BeginTabItem("Inspect")) {drawInspect();ImGui::EndTabItem();}
            if (ImGui::BeginTabItem("Battle")) {drawBattle();ImGui::EndTabItem();}
            if (ImGui::BeginTabItem("DB")) {drawDatabase();ImGui::EndTabItem();}
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::Text("%s | %.2f ms | %.1f FPS",bgfx::getRendererName(bgfx::getRendererType()),ImGui::GetIO().DeltaTime*1000,ImGui::GetIO().Framerate);
        ImGui::Text("60 Hz tick %llu",static_cast<unsigned long long>(ticks));ImGui::Checkbox("ImGui demo",&showDemo_);
    }
    ImGui::End();
    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
    if (showGrid_) drawGrid();
    if (paused_) game_.renderer().updateCamera(game_.world(),ImGui::GetIO().DeltaTime);
}
void Editor::drawMap() {
    auto& map=game_.world().map();
    ImGui::Text("Grid: %d x %d",map.nav.width(),map.nav.depth());
    ImGui::Checkbox("Paint with left mouse",&painting_);
    const char* brushes[]{"Walkability","Height","Encounter region","Tile flags","Ramp"};ImGui::Combo("Brush",&brush_,brushes,5);
    if (brush_==0) ImGui::Checkbox("Walkable",&walkable_);
    if (brush_==1) ImGui::DragFloat("Height (tiles)",&height_,.05f,-100,100);
    if (brush_==2) ImGui::InputInt("Region",&region_);
    if (brush_==3) {
        bool water=(flags_&Water)!=0,counter=(flags_&Counter)!=0,damage=(flags_&DamageFloor)!=0;
        ImGui::Checkbox("Water",&water);ImGui::Checkbox("Counter",&counter);ImGui::Checkbox("Damage floor",&damage);
        flags_=(water?Water:0)|(counter?Counter:0)|(damage?DamageFloor:0);
    }
    if (brush_==4) {const char* ramps[]{"None","North","East","South","West"};ImGui::Combo("Ramp direction",&ramp_,ramps,5);}
    if (hover_) {
        ImGui::Text("Cell %d, %d | height %.2f",hover_->cell.x,hover_->cell.y,hover_->position.y);
        if (ImGui::Button("Play from highlighted cell")) {
            try {game_.playFrom({hover_->cell.x+.5f,hover_->position.y,hover_->cell.y+.5f});paused_=false;painting_=false;}
            catch (const std::exception& error) {status_=error.what();}
        }
    }
    ImGui::TextWrapped("Paint the grid in the world view. Changes are live; Save map writes map.json. Undo records each brush stroke.");
    if (ImGui::Button("Import Blender/glTF markers")) {
        try {const auto geometry=Mesh::load(game_.gameRoot()/"maps"/map.id/map.geometry);checkpoint();map.importMarkers(geometry);game_.rebuildEntities();status_="Imported collision, lights and spawns";}
        catch (const std::exception& error) {status_=error.what();}
    }
    if (ImGui::CollapsingHeader("Spawns")) {
        for (auto& [name,position]:map.spawns) dirty_|=ImGui::DragFloat3(name.c_str(),glm::value_ptr(position),.1f);
        ImGui::InputText("Spawn name",newSpawn_,sizeof(newSpawn_));
        if (ImGui::Button("Add spawn at player") && newSpawn_[0]) {checkpoint();map.spawns[newSpawn_]=game_.world().playerPosition();newSpawn_[0]=0;}
    }
    if (ImGui::CollapsingHeader("Encounter regions")) {
        for (auto& [region,table]:map.encounters) {ImGui::PushID(region);ImGui::Text("Region %d",region);ImGui::SameLine();editString("Table",table);ImGui::PopID();}
        if (ImGui::Button("Assign current region")) {checkpoint();if (!game_.database().encounters.empty()) map.encounters[std::max(0,region_)]=game_.database().encounters.begin()->first;}
    }
}
void Editor::drawEntities() {
    auto& map=game_.world().map();
    if (ImGui::Button("Place NPC at player")) {
        checkpoint();MapEntity entity;int i=1;
        do {entity.id="npc_"+std::to_string(i++);} while (std::any_of(map.entities.begin(),map.entities.end(),[&](const auto& other){return other.id==entity.id;}));
        entity.name="New NPC";entity.position=game_.world().playerPosition()+glm::vec3(1,0,0);
        if (!game_.database().actors.empty()) entity.sprite=game_.database().actors.begin()->second.sprite;
        entity.pages.push_back({Json::object(),"","action",entity.sprite,true});map.entities.push_back(entity);entity_=static_cast<int>(map.entities.size())-1;game_.rebuildEntities();
    }
    if (map.entities.empty()) {ImGui::TextUnformatted("No entities on this map.");return;}
    entity_=std::clamp(entity_,0,static_cast<int>(map.entities.size())-1);
    if (ImGui::BeginCombo("Entity",map.entities[entity_].id.c_str())) {
        for (int i=0;i<static_cast<int>(map.entities.size());++i) if (ImGui::Selectable(map.entities[i].id.c_str(),i==entity_)) entity_=i;
        ImGui::EndCombo();
    }
    auto& e=map.entities[entity_];bool changed=false;
    changed|=editString("Name",e.name);changed|=editString("Sprite .anim.json (Enter)",e.sprite,true);
    changed|=ImGui::DragFloat3("Position",glm::value_ptr(e.position),.05f);
    changed|=ImGui::DragFloat2("Facing",glm::value_ptr(e.facing),.05f,-1,1);
    changed|=ImGui::SliderFloat("Speed",&e.speed,0,8);changed|=ImGui::SliderFloat("Radius",&e.radius,0,.49f);changed|=ImGui::Checkbox("Solid",&e.solid);
    int routine=e.routine=="patrol"?1:e.routine=="wander"?2:0;const char* routines[]{"Stationary","Patrol","Wander"};
    if (ImGui::Combo("Routine",&routine,routines,3)) {e.routine=routine==1?"patrol":routine==2?"wander":"stationary";changed=true;}
    if (routine==1) {
        for (std::size_t i=0;i<e.route.size();++i) {ImGui::PushID(static_cast<int>(i));changed|=ImGui::DragFloat2("Waypoint",glm::value_ptr(e.route[i]),.1f);ImGui::PopID();}
        if (ImGui::Button("Add waypoint at player")) {const auto p=game_.world().playerPosition();e.route.emplace_back(p.x,p.z);changed=true;}
    }
    for (std::size_t i=0;i<e.pages.size();++i) {
        ImGui::PushID(static_cast<int>(i));auto& page=e.pages[i];
        if (ImGui::TreeNode("Page","Event page %zu (last matching wins)",i+1)) {
            changed|=editString("Script (Enter)",page.script,true);changed|=editString("Page sprite (Enter)",page.sprite,true);changed|=ImGui::Checkbox("Page solid",&page.solid);
            const char* types[]{"action","touch","autorun","parallel"};int type=0;for (int t=0;t<4;++t) if (page.trigger==types[t]) type=t;
            if (ImGui::Combo("Trigger",&type,types,4)) {page.trigger=types[type];changed=true;}
            std::array<char,2048> conditions{};std::snprintf(conditions.data(),conditions.size(),"%s",page.conditions.dump().c_str());
            if (ImGui::InputText("Conditions JSON",conditions.data(),conditions.size(),ImGuiInputTextFlags_EnterReturnsTrue)) {
                try {auto data=Json::parse(conditions.data());if (!data.is_object()) throw std::runtime_error("Conditions need an object");page.conditions=std::move(data);changed=true;}
                catch (const std::exception& error) {status_=error.what();}
            }
            ImGui::TextWrapped("Example: {\"flags\":{\"opened\":true}} or {\"variables\":[{\"id\":\"chapter\",\"op\":\">=\",\"value\":2}]}");
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add event page")) {checkpoint();e.pages.push_back({Json::object(),"","action",e.sprite,e.solid});changed=true;}
    ImGui::SameLine();
    if (ImGui::Button("Delete entity")) {checkpoint();map.entities.erase(map.entities.begin()+entity_);game_.rebuildEntities();return;}
    if (changed) {dirty_=true;game_.rebuildEntities();}
}
void Editor::drawLighting() {
    auto& map=game_.world().map();bool changed=false;
    changed|=ImGui::ColorEdit3("Ambient",glm::value_ptr(map.ambient));changed|=ImGui::ColorEdit3("Sun color",glm::value_ptr(map.sunColor));
    changed|=ImGui::DragFloat3("Sun direction",glm::value_ptr(map.sunDirection),.02f,-1,1);
    if (glm::length(map.sunDirection)<.01f) map.sunDirection={-.6f,-1,-.4f};
    changed|=ImGui::ColorEdit3("Fog color",glm::value_ptr(map.fogColor));changed|=ImGui::SliderFloat("Fog density",&map.fogDensity,0,.12f);
    if (ImGui::Button("Add point light at player")) {checkpoint();MapLight light;light.position=game_.world().playerPosition()+glm::vec3(0,2,0);map.lights.push_back(light);}
    for (std::size_t i=0;i<map.lights.size();++i) {
        ImGui::PushID(static_cast<int>(i));auto& light=map.lights[i];
        if (ImGui::TreeNode("Point light","Point light %zu",i+1)) {
            changed|=ImGui::DragFloat3("Position",glm::value_ptr(light.position),.05f);changed|=ImGui::ColorEdit3("Color",glm::value_ptr(light.color));
            changed|=ImGui::SliderFloat("Radius",&light.radius,.1f,30);changed|=ImGui::SliderFloat("Intensity",&light.intensity,0,10);
            if (ImGui::Button("Remove light")) {checkpoint();map.lights.erase(map.lights.begin()+static_cast<std::ptrdiff_t>(i));ImGui::TreePop();ImGui::PopID();break;}
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::SeparatorText("Post processing");
    changed|=ImGui::Checkbox("Shadows",&game_.renderer().shadows);
    changed|=ImGui::Checkbox("Bloom",&map.post.bloom);changed|=ImGui::SliderFloat("Bloom strength",&map.post.bloomStrength,0,2);
    changed|=ImGui::Checkbox("Tilt-shift DOF",&map.post.depthOfField);changed|=ImGui::SliderFloat("Focus band",&map.post.focus,0,1);changed|=ImGui::SliderFloat("Focus range",&map.post.focusRange,.01f,.5f);
    changed|=ImGui::SliderFloat("Focus distance (0 = camera)",&map.post.focusDistance,0,60);changed|=ImGui::SliderFloat("Lens aperture",&map.post.aperture,0,5);
    changed|=ImGui::Checkbox("Vignette",&map.post.vignette);changed|=ImGui::Checkbox("3D LUT",&map.post.colorGrade);
    changed|=editString("LUT .cube path (Enter)",map.post.lut,true);changed|=ImGui::SliderFloat("Exposure",&map.post.exposure,.1f,3);
    dirty_|=changed;
}
void Editor::drawCamera() {
    auto& map=game_.world().map();cameraControls(map.camera);
    if (ImGui::IsAnyItemActive()) dirty_=true;
    if (ImGui::Button("Add camera zone at player")) {
        checkpoint();const auto p=game_.world().playerPosition();CameraZone zone;zone.min={p.x-2,p.z-2};zone.max={p.x+2,p.z+2};zone.camera=map.camera;map.cameraZones.push_back(zone);
    }
    for (std::size_t i=0;i<map.cameraZones.size();++i) {
        ImGui::PushID(static_cast<int>(i));auto& zone=map.cameraZones[i];
        if (ImGui::TreeNode("Zone","Camera zone %zu",i+1)) {
            dirty_|=ImGui::DragFloat2("Zone minimum",glm::value_ptr(zone.min),.1f);dirty_|=ImGui::DragFloat2("Zone maximum",glm::value_ptr(zone.max),.1f);
            cameraControls(zone.camera);if (ImGui::IsAnyItemActive()) dirty_=true;
            if (ImGui::Button("Remove zone")) {checkpoint();map.cameraZones.erase(map.cameraZones.begin()+static_cast<std::ptrdiff_t>(i));ImGui::TreePop();ImGui::PopID();break;}
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}
void Editor::drawInspect() {
    auto& state=game_.state();
    if (ImGui::CollapsingHeader("Flags",ImGuiTreeNodeFlags_DefaultOpen)) {
        for (auto& [id,value]:state.flags) ImGui::Checkbox(id.c_str(),&value);
        ImGui::InputText("New flag",newFlag_,sizeof(newFlag_));
        if (ImGui::Button("Add flag") && newFlag_[0]) {state.flags[newFlag_]=true;newFlag_[0]=0;}
    }
    if (ImGui::CollapsingHeader("Variables")) {
        for (auto& [id,value]:state.variables) ImGui::InputDouble(id.c_str(),&value);
        ImGui::InputText("New variable",newVariable_,sizeof(newVariable_));
        if (ImGui::Button("Add variable") && newVariable_[0]) {state.variables[newVariable_]=0;newVariable_[0]=0;}
    }
    if (ImGui::CollapsingHeader("ECS entities")) {
        auto& registry=game_.world().registry();
        for (auto e:registry.view<EntityName,Transform>()) {
            const auto& name=registry.get<EntityName>(e);auto& t=registry.get<Transform>(e);
            if (ImGui::TreeNode(name.id.c_str())) {ImGui::Text("Handle: %u",entt::to_integral(e));ImGui::DragFloat3("Position",glm::value_ptr(t.position),.05f);ImGui::Text("Y %.2f",t.position.y);ImGui::TreePop();}
        }
    }
    ImGui::SeparatorText("Lua console");
    ImGui::InputTextMultiline("##Lua",console_,sizeof(console_),{-1,100});
    if (ImGui::Button("Execute Lua")) status_=game_.scripts().executeConsole(console_)?"Lua executed":game_.scripts().lastError();
    ImGui::TextWrapped("%zu running event(s). %s",game_.scripts().taskCount(),game_.scripts().lastError().c_str());
}
void Editor::drawBattle() {
    const auto& troops=game_.database().troops;
    if (troops.empty()) {ImGui::TextUnformatted("No troops in the database.");return;}
    std::vector<std::string> ids;for (const auto& [id,definition]:troops) {(void)definition;ids.push_back(id);}
    troop_=std::clamp(troop_,0,static_cast<int>(ids.size())-1);
    if (ImGui::BeginCombo("Troop",ids[troop_].c_str())) {
        for (int i=0;i<static_cast<int>(ids.size());++i) if (ImGui::Selectable(ids[i].c_str(),i==troop_)) troop_=i;
        ImGui::EndCombo();
    }
    if (ImGui::Button("Fight selected troop")) {
        try {game_.startBattle(ids[troop_]);paused_=false;open_=false;}
        catch (const std::exception& error) {status_=error.what();}
    }
    ImGui::SameLine();if (ImGui::Button("Restore party")) game_.state().rest(game_.database());
    ImGui::TextWrapped("Runs the real ATB core, Lua formulas and enemy AI. Rewards and resource changes affect the current game state.");
}
void Editor::drawDatabase() {
    ImGui::TextUnformatted("Validated database (read-only)");
    for (const auto& [table,entries]:game_.database().source().items()) if (ImGui::TreeNode(table.c_str())) {
        for (const auto& entry:entries) {
            const auto name=entry.value("name",entry.value("id",std::string("entry")));
            if (ImGui::TreeNode(name.c_str())) {const auto text=entry.dump(2);ImGui::TextUnformatted(text.c_str());ImGui::TreePop();}
        }
        ImGui::TreePop();
    }
}
void Editor::drawGrid() {
    auto& world=game_.world();auto& grid=world.map().nav;
    const auto display=ImGui::GetIO().DisplaySize;
    const auto view=game_.renderer().camera().view(),projection=game_.renderer().camera().projection(display.x/display.y,bgfx::getCaps()->homogeneousDepth);
    const auto vp=projection*view,inverse=glm::inverse(vp);
    const auto mouse=ImGui::GetIO().MousePos;
    if (!ImGui::GetIO().WantCaptureMouse && mouse.x>=0 && mouse.y>=0) {
        const glm::vec2 ndc(mouse.x/display.x*2-1,1-mouse.y/display.y*2);
        auto near=inverse*glm::vec4(ndc,bgfx::getCaps()->homogeneousDepth?-1.0f:0.0f,1),far=inverse*glm::vec4(ndc,1,1);
        near/=near.w;far/=far.w;hover_=pickGrid(grid,glm::vec3(near),glm::vec3(far-near));
        if (painting_ && hover_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) checkpoint();
            auto& cell=grid.at(hover_->cell.x,hover_->cell.y);
            if (brush_==0) cell.walkable=walkable_;
            if (brush_==1) cell.height=height_;
            if (brush_==2) cell.region=std::max(0,region_);
            if (brush_==3) cell.flags=flags_;
            if (brush_==4) cell.ramp=static_cast<Ramp>(ramp_);
            dirty_=true;
        }
    }
    auto project=[&](glm::vec3 p,ImVec2& screen) {
        const auto clip=vp*glm::vec4(p,1);if (clip.w<=0) return false;
        screen={(clip.x/clip.w*.5f+.5f)*display.x,(-clip.y/clip.w*.5f+.5f)*display.y};return true;
    };
    auto* draw=ImGui::GetBackgroundDrawList();const auto player=world.playerPosition();
    const int minX=std::max(0,int(player.x)-25),maxX=std::min(grid.width(),int(player.x)+26),minZ=std::max(0,int(player.z)-25),maxZ=std::min(grid.depth(),int(player.z)+26);
    for (int z=minZ;z<maxZ;++z) for (int x=minX;x<maxX;++x) {
        const glm::vec2 corners[]{{x+.001f,z+.001f},{x+.999f,z+.001f},{x+.999f,z+.999f},{x+.001f,z+.999f}};
        ImVec2 points[4];bool visible=true;
        for (int i=0;i<4;++i) visible&=project({corners[i].x,grid.heightAt(corners[i])+.025f,corners[i].y},points[i]);
        if (!visible) continue;
        const bool hovered=hover_ && hover_->cell==glm::ivec2(x,z);
        const auto color=hovered?IM_COL32(255,220,80,120):grid.at(x,z).walkable?IM_COL32(50,220,130,26):IM_COL32(255,70,80,65);
        draw->AddConvexPolyFilled(points,4,color);draw->AddPolyline(points,4,IM_COL32(190,220,255,80),ImDrawFlags_Closed,1);
    }
}
}
