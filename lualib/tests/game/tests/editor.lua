-- Editor forms: what fe.devtools.describe() hands the map editor's inspectors.
local devtools = require("fe.devtools")
local field = require("fe.field")

local looks = { "opaque", "alpha", "additive", "multiply", "premultiplied" }

return {
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
