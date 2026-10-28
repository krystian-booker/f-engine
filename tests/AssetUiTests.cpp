#include <doctest/doctest.h>
#include "engine/assets/Image.h"
#include "engine/assets/Mesh.h"
#include "engine/assets/SpriteAnimation.h"
#include "engine/ui/Text.h"
#include "engine/world/Map.h"
#include "engine/editor/GridPicking.h"
#include "engine/battle/ActionSequence.h"
#include <algorithm>

TEST_CASE("sandbox data and glTF dioramas load with all database references valid") {
    const auto root=std::filesystem::path(FENGINE_SAMPLE_DIR);
    const auto db=fe::Database::load(root/"database.json");
    CHECK(db.actors.size()==2);
    for (const auto* name : {"town","grotto","battle_grass","battle_cave"}) {
        const auto map=fe::MapData::load(root/"maps"/name/"map.json");
        CHECK(map.id==name);
        const auto mesh=fe::Mesh::load(root/"maps"/name/map.geometry);
        CHECK_FALSE(mesh.parts.empty());
        REQUIRE_FALSE(mesh.markers.empty());auto imported=map;imported.importMarkers(mesh);CHECK(imported.spawns.contains("blender_entry"));
        for (const auto& part : mesh.parts) {
            CHECK(part.indices.size()%3==0);
            for (const auto& vertex : part.vertices) CHECK(glm::length(vertex.normal)==doctest::Approx(1));
        }
        for (const auto& entity : map.entities) for (const auto& page : entity.pages)
            if (!page.script.empty()) CHECK(std::filesystem::exists(root/page.script));
    }
}
TEST_CASE("Blender marker import is idempotent and rejects collisions blocking a spawn") {
    auto map=fe::MapData::fromJson({{"id","markers"},{"nav",{{"width",4},{"depth",4}}},{"spawns",{{"entry",{.5,0,.5}}}}});
    fe::Mesh geometry;fe::MeshMarker collision;collision.hasBounds=true;collision.min={2,0,2};collision.max={3,2,3};collision.properties={{"collision",true}};
    fe::MeshMarker light;light.position={1.5f,2,1.5f};light.properties={{"light",{{"radius",4},{"intensity",2},{"color",{1,.5,.25}}}}};
    geometry.markers={collision,light};map.importMarkers(geometry);map.importMarkers(geometry);
    CHECK_FALSE(map.nav.at(2,2).walkable);CHECK(map.lights.size()==1);
    collision.min={0,0,0};collision.max={1,2,1};geometry.markers={collision};
    CHECK_THROWS(map.importMarkers(geometry));CHECK(map.nav.at(0,0).walkable);
}
TEST_CASE("sprite animations decode their atlas and select direction and frame") {
    const auto root=std::filesystem::path(FENGINE_SAMPLE_DIR);
    const auto anim=fe::SpriteAnimation::load(root/"sprites/aster.anim.json");
    const auto image=fe::Image::load(root/"sprites"/anim.atlas);
    CHECK(image.width==128); CHECK(image.height==192);
    CHECK(anim.frame("walk",{0,1},0,0).rect.x==0);
    CHECK(anim.frame("walk",{0,1},0,0.17).rect.x==32);
    CHECK(anim.frame("idle",{0,-1},0,0).rect.y==96);
    CHECK(anim.frame("idle",{0,-1},90,0).rect.y==144);
    CHECK(anim.frame("missing",{0,1},0,0).rect.y==0);
    const auto events=anim.events("walk",0,0,0.5);
    CHECK(events.size()==2);
}
TEST_CASE("rich text preserves UTF-8 glyphs, expands variables and honors waits") {
    auto glyphs=fe::parseText("A{color:red}é{wait:12}{icon:potion}{player}",[](const auto& code) { return code=="player"?"Mira":""; });
    REQUIRE(glyphs.size()==8);
    CHECK(glyphs[1].text=="é"); CHECK(glyphs[1].color==0xff7070ff);
    CHECK(glyphs[2].wait==doctest::Approx(.2)); CHECK(glyphs[3].icon=="potion");
    fe::Typewriter writer; writer.set(glyphs,10);
    writer.update(.2); CHECK(writer.visible()==2);
    writer.update(.1); CHECK(writer.visible()==2);
    writer.update(.1); CHECK(writer.visible()==3);
    writer.finish(); CHECK(writer.complete());
    glyphs=fe::parseText("{unknown}");
    std::string literal; for (const auto& g : glyphs) literal+=g.text;
    CHECK(literal=="{unknown}");
}
TEST_CASE("menu cursor remembers selection, wraps and scrolls into view") {
    fe::MenuCursor cursor; cursor.setCount(10); cursor.move(-1);
    CHECK(cursor.selected()==9); CHECK(cursor.scrollOffset(4)==6);
    cursor.move(1); CHECK(cursor.selected()==0); CHECK(cursor.scrollOffset(4)==0);
    cursor.select(7); cursor.setCount(4); CHECK(cursor.selected()==3);
    cursor.setCount(0); CHECK_FALSE(cursor.move(1)); CHECK(cursor.selected()==0);
}
TEST_CASE("long dialogue wraps words and paginates without dropping content or codes") {
    auto source=fe::parseText("one two three four five six {color:red}é{wait:20}{icon:potion}");
    auto pages=fe::paginateText(source,9,2,[](const fe::TextGlyph& g){return g.text.empty() && g.icon.empty()?0.0f:1.0f;});
    REQUIRE(pages.size()==2);
    std::string firstPage;for (const auto& g:pages.front()) firstPage+=g.text;
    CHECK(firstPage=="one two \nthree ");
    std::string actual;int waits=0,icons=0;
    for (const auto& page:pages) for (const auto& g:page) {
        if (g.text!=" " && g.text!="\n") actual+=g.text;
        waits+=g.wait>0;icons+=!g.icon.empty();
    }
    CHECK(actual=="onetwothreefourfivesixé");CHECK(waits==1);CHECK(icons==1);
    CHECK(fe::paginateText({},1,1,[](const auto&){return 1.0f;}).size()==1);
    CHECK(fe::paginateText(fe::parseText("abcdefghij"),3,1,[](const auto&){return 1.0f;}).size()==4);
}
TEST_CASE("editor ray picking hits exact tile and ramp planes") {
    fe::NavGrid grid(4,4);grid.at(1,1).height=2;grid.at(2,1).ramp=fe::Ramp::East;
    auto hit=fe::pickGrid(grid,{1.5f,10,1.5f},{0,-1,0});
    REQUIRE(hit);CHECK(hit->cell==glm::ivec2(1,1));CHECK(hit->position.y==doctest::Approx(2));
    hit=fe::pickGrid(grid,{2.75f,10,1.5f},{0,-1,0});
    REQUIRE(hit);CHECK(hit->position.y==doctest::Approx(.75f));
    CHECK_FALSE(fe::pickGrid(grid,{-2,5,-2},{0,-1,0}));
    CHECK_FALSE(fe::pickGrid(grid,{1,5,1},{0,1,0}));
}
TEST_CASE("action sequencer emits crossed events once and preserves their order") {
    auto timelines=fe::ActionTimeline::load(std::filesystem::path(FENGINE_SAMPLE_DIR)/"timelines.json");
    fe::ActionSequence player;player.start(timelines.at("attack"));
    auto events=player.update(0);REQUIRE(events.size()==1);CHECK(events[0].command=="step");
    events=player.update(.5);REQUIRE(events.size()==3);CHECK(events[0].command=="animate");CHECK(events[1].command=="effect");CHECK(events[2].command=="sfx");
    events=player.update(10);REQUIRE(events.size()==1);CHECK(events[0].command=="return");CHECK(player.complete());
    CHECK(player.update(1).empty());
}
