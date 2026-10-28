#include "engine/assets/Image.h"
#include "engine/assets/SpriteAnimation.h"
#include "engine/assets/Mesh.h"
#include "engine/core/GameConfig.h"
#include "engine/rpg/Database.h"
#include "engine/world/Map.h"
#include "engine/battle/ActionSequence.h"
#include "engine/core/Json.h"
#include <algorithm>
#include <iostream>
#include <numeric>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {
using fe::Json;
void validate(const std::filesystem::path& folder) {
    const auto root=std::filesystem::weakly_canonical(folder);const auto config=fe::GameConfig::load(root);
    const auto db=fe::Database::load(root/"database.json");
    const auto initial=fe::readJson(root/"initial_state.json");
    fe::GameState state;for (const auto& id:initial.at("party")) state.addActor(db,id);
    fe::ActionTimeline::load(root/"timelines.json");
    std::unique_ptr<lua_State,decltype(&lua_close)> lua(luaL_newstate(),lua_close);
    if (!lua) throw std::runtime_error("Cannot allocate Lua validation state");
    int maps=0,sprites=0,scripts=0;
    for (const auto& entry:std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;const auto file=entry.path();
        if (file.extension()==".lua") {
            if (luaL_loadfilex(lua.get(),file.string().c_str(),"t")!=LUA_OK) throw std::runtime_error(file.string()+": "+lua_tostring(lua.get(),-1));
            lua_settop(lua.get(),0);++scripts;
        }
        if (file.filename()=="map.json" && file.parent_path().parent_path()==root/"maps") {
            const auto map=fe::MapData::load(file);fe::Mesh::load(file.parent_path()/map.geometry);
            for (const auto& [region,id]:map.encounters) {(void)region;if (!db.encounters.contains(id)) throw std::runtime_error("Unknown encounter table: "+id);}
            for (const auto& entity:map.entities) for (const auto& page:entity.pages)
                if (!page.script.empty() && !std::filesystem::is_regular_file(root/page.script)) throw std::runtime_error("Missing event script: "+page.script);
            ++maps;
        }
        if (file.filename().string().ends_with(".anim.json")) {
            const auto animation=fe::SpriteAnimation::load(file);const auto image=fe::Image::load(file.parent_path()/animation.atlas);
            for (const auto& [name,clip]:animation.clips) for (const auto& direction:clip.directions) for (const auto& frame:direction)
                if (frame.rect.x+frame.rect.z>image.width || frame.rect.y+frame.rect.w>image.height) throw std::runtime_error("Sprite rect exceeds atlas: "+name);
            ++sprites;
        }
    }
    const auto start=fe::MapData::load(root/"maps"/config.startMap/"map.json");
    if (!start.spawns.contains(config.startSpawn)) throw std::runtime_error("Missing starting spawn");
    for (const auto& [id,troop]:db.troops) if (!std::filesystem::is_regular_file(root/"maps"/troop.stage/"map.json")) throw std::runtime_error("Missing battle stage for "+id);
    for (const auto& [id,actor]:db.actors) if (!actor.sprite.empty() && !std::filesystem::is_regular_file(root/actor.sprite)) throw std::runtime_error("Missing actor sprite: "+id);
    for (const auto& [id,enemy]:db.enemies) if (!enemy.sprite.empty() && !std::filesystem::is_regular_file(root/enemy.sprite)) throw std::runtime_error("Missing enemy sprite: "+id);
    std::cout<<"Validated "<<maps<<" maps, "<<sprites<<" sprites, "<<scripts<<" Lua scripts and the RPG database\n";
}
void atlas(const std::filesystem::path& manifest,const std::filesystem::path& output) {
    const auto data=fe::readJson(manifest);
    const int width=data.value("width",512),height=data.value("height",512),padding=data.value("padding",1),directions=data.value("directions",4);
    if (width<1 || height<1 || width>8192 || height>8192 || padding<0 || padding>16 || (directions!=1 && directions!=4 && directions!=8))
        throw std::runtime_error("Invalid atlas dimensions, padding, or directions");
    const auto& frames=data.at("frames");
    if (!frames.is_array() || frames.empty() || frames.size()>4096) throw std::runtime_error("Atlas needs 1..4096 frames");
    std::vector<fe::Image> images;
    for (const auto& frame:frames) images.push_back(fe::Image::load(manifest.parent_path()/frame.at("file").get<std::string>()));
    std::vector<std::size_t> order(images.size());std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return images[a].height>images[b].height;});
    std::vector<glm::ivec4> rectangles(images.size());int x=0,y=0,rowHeight=0;
    fe::Image atlas;atlas.width=width;atlas.height=height;atlas.rgba.resize(static_cast<std::size_t>(width)*height*4);
    for (auto i:order) {
        const auto& image=images[i];const int w=image.width+padding*2,h=image.height+padding*2;
        if (w>width || h>height) throw std::runtime_error("Frame does not fit the atlas");
        if (x+w>width) {x=0;y+=rowHeight;rowHeight=0;}
        if (y+h>height) throw std::runtime_error("Atlas is full; increase its dimensions");
        rectangles[i]={x+padding,y+padding,image.width,image.height};
        for (int py=-padding;py<image.height+padding;++py) for (int px=-padding;px<image.width+padding;++px) {
            const auto source=(std::size_t(std::clamp(py,0,image.height-1))*image.width+std::clamp(px,0,image.width-1))*4;
            const auto target=(std::size_t(y+padding+py)*width+x+padding+px)*4;
            std::copy_n(image.rgba.data()+source,4,atlas.rgba.data()+target);
        }
        x+=w;rowHeight=std::max(rowHeight,h);
    }
    Json animation{{"atlas","atlas.png"},{"pixels_per_unit",data.value("pixels_per_unit",32)},{"clips",Json::object()}};
    Json regions=Json::object();
    for (std::size_t i=0;i<frames.size();++i) {
        const auto& frame=frames[i];const auto clip=frame.value("clip",std::string("idle"));const int direction=frame.value("direction",0);
        if (direction<0 || direction>=directions) throw std::runtime_error("Invalid frame direction");
        auto& target=animation["clips"][clip];
        if (!target.contains("directions")) {target["directions"]=Json::array();for (int d=0;d<directions;++d) target["directions"].push_back(Json::array());}
        const auto r=rectangles[i];Json record{{"rect",{r.x,r.y,r.z,r.w}},{"duration",frame.value("duration",.15)}};
        if (frame.contains("event")) record["event"]=frame.at("event");
        target["directions"][direction].push_back(record);
        regions[frame.value("id",std::to_string(i))]=record["rect"];
    }
    fe::SpriteAnimation::fromJson(animation);
    atlas.savePng(output/"atlas.png");fe::writeJson(output/"atlas.anim.json",animation);fe::writeJson(output/"regions.json",regions);
}
void animation(const std::filesystem::path& manifest,const std::filesystem::path& output) {
    const auto source=fe::readJson(manifest);const auto atlasName=source.at("atlas").get<std::string>();
    const auto image=fe::Image::load(manifest.parent_path()/atlasName);
    const int w=source.at("frame_width"),h=source.at("frame_height"),directions=source.value("directions",4);
    if (w<1 || h<1 || (directions!=1 && directions!=4 && directions!=8)) throw std::runtime_error("Invalid sheet dimensions/directions");
    Json result{{"atlas",std::filesystem::path(atlasName).filename().string()},{"pixels_per_unit",source.value("pixels_per_unit",32)},{"clips",Json::object()}};
    for (const auto& [name,clip]:source.at("clips").items()) {
        auto& out=result["clips"][name];out["loop"]=clip.value("loop",true);out["directions"]=Json::array();
        for (int direction=0;direction<directions;++direction) {
            Json frames=Json::array();
            for (const auto& frame:clip.at("frames")) {
                const int column=frame.get<int>()+clip.value("column_offset",0),row=direction+clip.value("row_offset",0);
                if (column<0 || row<0 || column>=image.width/w || row>=image.height/h) throw std::runtime_error("Animation frame exceeds sprite sheet");
                frames.push_back({{"rect",{column*w,row*h,w,h}},{"duration",clip.value("duration",.15)}});
            }
            out["directions"].push_back(frames);
        }
    }
    fe::SpriteAnimation::fromJson(result);
    if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path());
    const auto destination=output.parent_path()/std::filesystem::path(atlasName).filename();
    if (std::filesystem::weakly_canonical(manifest.parent_path()/atlasName)!=std::filesystem::weakly_canonical(destination))
        std::filesystem::copy_file(manifest.parent_path()/atlasName,destination,std::filesystem::copy_options::overwrite_existing);
    fe::writeJson(output,result);
}
}
int main(int argc,char** argv) {
    try {
        if (argc==3 && std::string_view(argv[1])=="validate") {validate(argv[2]);return 0;}
        if (argc!=4) {
            std::cout<<"Usage: fengine_assetc atlas manifest.json output-directory\n"
                        "       fengine_assetc anim sheet.json output.anim.json\n"
                        "       fengine_assetc validate game-directory\n";
            return argc==2 && std::string_view(argv[1])=="--help"?0:1;
        }
        const std::string command=argv[1];
        if (command=="atlas") atlas(argv[2],argv[3]);
        else if (command=="anim") animation(argv[2],argv[3]);
        else throw std::runtime_error("Unknown asset command: "+command);
        return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
