-- Editor forms: what fe.devtools.describe() hands the map editor's inspectors.
local devtools = require("fe.devtools")
local field = require("fe.field")

local looks = { "opaque", "alpha", "additive", "multiply", "premultiplied" }

return {
  ["asset, module, map and point schema kinds reach the editor and check that their targets exist"] = function(t)
    local schema = require("fe.schema")
    local data = require("fe.data")
    schema.define("editor_poster", {
      id = "string",
      sprite = { type = { asset = "sprite" } },
      portrait = { type = { asset = "image" }, optional = true },
      music = { type = { asset = "audio" }, optional = true },
      script = { type = { module = "events" }, optional = true },
      map = { type = { map_id = true }, optional = true },
      point = { type = { point = "map" }, optional = true },
    })
    local described = fe.json.decode(devtools.describe()).schemas.editor_poster.fields
    t.eq(described.sprite.type, "asset")
    t.eq(described.sprite.asset, "sprite")
    t.eq(described.script.type, "module")
    t.eq(described.script.folder, "events")
    t.eq(described.map.type, "map_id")
    t.eq(described.point.type, "point")
    t.eq(described.point.map_field, "map")
    -- Built-in placement types declare their pickers too.
    local npc = fe.json.decode(devtools.describe()).entities.npc.fields
    t.eq(npc.sprite.type, "asset")
    t.eq(npc.script.type, "module")
    t.eq(npc.pages.type, "list")
    t.eq(npc.pages.of.type, "record")
    t.eq(npc.pages.of.fields.trigger.type, "enum")
    local door = fe.json.decode(devtools.describe()).entities.door.fields
    t.eq(door.map.type, "map_id")
    t.eq(door.point.type, "point")
    -- Validation: strings are accepted by type, and missing targets are reported.
    local ok, errors = data.validate("posters", {
      { id = "good", sprite = "@fe/assets/ui/spark.anim.json", map = "arena", script = "fe.task" },
      { id = "bad", sprite = "sprites/nobody.anim.json", map = "nowhere", script = "events.ghost" },
      { id = "typed", sprite = 7 },
    }, "editor_poster")
    t.ok(not ok)
    local text = table.concat(errors, "\n")
    t.ok(text:find("nobody.anim.json", 1, true), "the missing sprite is named: " .. text)
    t.ok(text:find("map not found: nowhere", 1, true), "the missing map is named")
    t.ok(text:find("module not found: events.ghost", 1, true), "the missing module is named")
    t.ok(text:find("typed.sprite: expected a string", 1, true), "a non-string is a type error")
    t.ok(not text:find("good", 1, true), "the record whose targets exist passes: " .. text)
  end,

  ["the editor can list and cancel tasks and read recent field events"] = function(t)
    local task = require("fe.task")
    local job = task.named("editor_probe", function() task.wait(100) end)
    local listed = fe.json.decode(devtools.tasks())
    local found
    for _, entry in ipairs(listed) do
      if entry.name == "editor_probe" then found = entry end
    end
    t.ok(found ~= nil, "the probe task is listed")
    t.eq(found.status, "running")
    t.ok(found.index >= 1)
    devtools.cancel_task(found.index)
    t.eq(job.status, "cancelled")
    t.eq(type(fe.json.decode(devtools.recent_events())), "table")
    t.eq(devtools.inspect_entity("nobody"), "null")
  end,

  ["encounter zones describe their table and rate for the editor"] = function(t)
    require("fe.encounter")
    local zone = fe.json.decode(devtools.describe()).zones.encounter
    t.ok(zone ~= nil, "the encounter zone type is described")
    t.eq(zone.fields.table.type, "ref")
    t.eq(zone.fields.table.table, "encounters")
    t.eq(zone.fields.rate.type, "number")
    t.eq(zone.fields.rate.default, 1)
    t.eq(zone.fields.rate.min, 0)
  end,

  ["a placement type can give its sprite's blend, mode, silhouette and pixel snap to the editor"] = function(t)
    -- The pattern docs/authoring.md shows for sprite looks set per placement.
    field.define("editor_glow", function(world, p)
      local props = p.props
      return world:spawn({ id = p.id, position = p.position, layer = p.layer,
        sprite = { animation = props.sprite, blend = props.blend, mode = props.mode,
                   silhouette = props.silhouette, pixel_snap = props.pixel_snap } })
    end, {
      sprite = { type = "string", doc = "Sprite .anim.json" },
      blend = { enum = looks, optional = true },
      mode = { enum = { "upright", "facing", "flat", "decal" }, optional = true },
      silhouette = { type = "any", optional = true, doc = "true, false or a colour {r, g, b, a}" },
      pixel_snap = { type = "boolean", optional = true },
    }, "A sprite with an authored look.")
    local fields = fe.json.decode(devtools.describe()).entities.editor_glow.fields
    t.eq(fields.blend.type, "enum")
    t.eq(fields.blend.options, looks)
    t.eq(fields.mode.options[4], "decal")
    t.eq(fields.pixel_snap.type, "boolean")
    t.ok(fields.pixel_snap.optional)

    local world = fe.world.load("editor_glow", "arena")
    local entity = field.types.editor_glow(world, { id = "glow", type = "editor_glow", layer = 0,
      position = { x = 3.5, y = 0, z = 5.5 },
      props = { sprite = "@fe/assets/ui/spark.anim.json", blend = "additive", mode = "decal", silhouette = true,
                pixel_snap = true } })
    local sprite = entity:sprite()
    t.eq(sprite.blend, "additive")
    t.eq(sprite.mode, "decal")
    t.eq(sprite.pixel_snap, true)
    t.ok(sprite.silhouette[4] > 0)
    local plain = field.types.editor_glow(world, { id = "plain", type = "editor_glow", layer = 0,
      position = { x = 4.5, y = 0, z = 5.5 }, props = { sprite = "@fe/assets/ui/spark.anim.json" } })
    t.eq(plain:sprite().pixel_snap, nil, "left out, the environment's pixel_snap applies")
    field.types.editor_glow = nil
    fe.world.unload("editor_glow")
  end,
}
