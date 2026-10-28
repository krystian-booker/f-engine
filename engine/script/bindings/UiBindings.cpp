#include "engine/runtime/Engine.h"
#include "engine/script/bindings/Convert.h"

namespace fe {
namespace {
TextOptions textOptions(const sol::optional<sol::table>& source) {
    TextOptions o;
    if (!source) {
        return o;
    }
    const auto& t = *source;
    o.font = lua::get<std::string>(t, "font", o.font);
    o.size = lua::get(t, "size", o.size);
    o.color = lua::rgba(t["color"], o.color);
    o.width = lua::get(t, "width", o.width);
    o.lineGap = lua::get(t, "line_gap", o.lineGap);
    const auto align = lua::get<std::string>(t, "align", "left");
    o.align = align == "center" ? TextAlign::Center : align == "right" ? TextAlign::Right : TextAlign::Left;
    sol::optional<double> visible = t["visible"];
    if (visible) {
        o.visible = static_cast<std::size_t>(std::max(0.0, *visible));
    }
    o.shadow = lua::rgba(t["shadow"], 0);
    o.outline = lua::rgba(t["outline"], 0);
    return o;
}

ImageOptions imageOptions(const sol::optional<sol::table>& source) {
    ImageOptions o;
    if (!source) {
        return o;
    }
    const auto& t = *source;
    if (t["src"].valid()) {
        sol::table src = t["src"];
        o.source = {lua::component(src, "x", 1, 0), lua::component(src, "y", 2, 0),
                    lua::component(src, "w", 3, 0), lua::component(src, "h", 4, 0)};
    }
    o.color = lua::rgba(t["color"], o.color);
    o.flipX = lua::get(t, "flip_x", false);
    o.flipY = lua::get(t, "flip_y", false);
    o.linear = lua::get(t, "linear", false);
    return o;
}
} // namespace

void bindText(Engine& engine) {
    auto& host = engine.lua();
    auto& text = engine.text();
    ModuleBinder(host, engine.docs(), "fe.text",
                 "Fonts, colors, icons and markup. Markup tags: {color:x} {icon:id} {wait:s} {speed:cps}; "
                 "other {tags} are passed to the resolver set with fe.text.resolver.")
        .fn(
            "font",
            [&engine, &host](const std::string& name, sol::table spec) {
                engine.assets().defineFont(name, luaToJson(spec));
                (void)host;
            },
            "fun(name: string, spec: {files: string[], size?: number, pixel?: boolean})",
            "Define a font: .fnt bitmap fonts and/or .ttf/.otf faces, first match wins per character.")
        .fn(
            "color",
            [&text](const std::string& name, sol::object value) {
                text.palette().define(name, lua::rgba(value));
            },
            "fun(name: string, color: any)", "Name a color for {color:name}.")
        .fn(
            "icon",
            [&text](const std::string& id, sol::table spec) {
                IconDef icon;
                icon.image = spec.get<std::string>("image");
                if (spec["rect"].valid()) {
                    sol::table r = spec["rect"];
                    icon.rect = {lua::component(r, "x", 1, 0), lua::component(r, "y", 2, 0),
                                 lua::component(r, "w", 3, 0), lua::component(r, "h", 4, 0)};
                }
                text.defineIcon(id, icon);
            },
            "fun(id: string, spec: {image: string, rect?: number[]})", "Register an {icon:id} image.")
        .fn(
            "resolver",
            [&text, &host](sol::object fn) {
                if (fn.get_type() != sol::type::function) {
                    text.setResolver({});
                    return;
                }
                // Anchor on the main thread: the caller may be a coroutine that dies.
                sol::main_protected_function callback = fn;
                text.setResolver([callback, &host](std::string_view tag) -> std::optional<std::string> {
                    auto result = callback(std::string(tag));
                    if (!result.valid()) {
                        const sol::error error = result;
                        host.report(error.what());
                        return std::nullopt;
                    }
                    sol::optional<std::string> value = result;
                    return value ? std::optional<std::string>(*value) : std::nullopt;
                });
            },
            "fun(resolver: fun(tag: string): string?)", "Resolve custom tags such as {player} or {var:gold}.")
        .fn(
            "units",
            [&text](sol::this_state state, const std::string& markup) {
                sol::state_view lua(state);
                const auto glyphs = text.parse(markup, 0xffffffff);
                auto waits = lua.create_table();
                auto speeds = lua.create_table();
                for (std::size_t i = 0; i < glyphs.size(); ++i) {
                    if (glyphs[i].wait > 0) {
                        waits[i + 1] = glyphs[i].wait;
                    }
                    if (glyphs[i].speed > 0) {
                        speeds[i + 1] = glyphs[i].speed;
                    }
                }
                return lua.create_table_with("count", glyphs.size(), "wait", waits, "speed", speeds);
            },
            "fun(markup: string): {count: integer, wait: table<integer, number>, speed: table<integer, "
            "number>}",
            "Reveal units for typewriter effects.")
        .fn(
            "measure",
            [&text](const std::string& markup, sol::optional<sol::table> options) {
                const auto layout = text.layout(markup, textOptions(options));
                return std::make_tuple(layout.width, layout.height, layout.lines);
            },
            "fun(markup: string, options?: table): number, number, integer", "Width, height and line count.")
        .fn(
            "paginate",
            [&text](const std::string& markup, sol::table options, int lines) {
                return sol::as_table(text.paginate(markup, textOptions(options), lines));
            },
            "fun(markup: string, options: table, lines: integer): string[]",
            "Split text into pages that fit `lines` wrapped lines of `options.width`.")
        .fn(
            "plain",
            [&text](const std::string& markup) {
                std::string out;
                for (const auto& g : text.parse(markup, 0xffffffff)) {
                    if (g.codepoint) {
                        out += utf8(g.codepoint);
                    }
                }
                return out;
            },
            "fun(markup: string): string", "Markup with tags removed.");
}

void bindCanvas(Engine& engine) {
    auto& host = engine.lua();
    auto& canvas = engine.canvas();
    ModuleBinder(
        host, engine.docs(), "fe.canvas",
        "2D drawing in virtual pixels (origin top-left). All game UI is built from these calls each frame.")
        .fn(
            "size", [&canvas] { return std::make_tuple(canvas.width(), canvas.height()); },
            "fun(): integer, integer", "Canvas size in virtual pixels.")
        .fn(
            "rect",
            [&canvas](float x, float y, float w, float h, sol::object color) {
                canvas.rect({x, y, w, h}, lua::rgba(color));
            },
            "fun(x: number, y: number, w: number, h: number, color?: any)", "Filled rectangle.")
        .fn(
            "line",
            [&canvas](float x0, float y0, float x1, float y1, sol::object color,
                      sol::optional<float> thickness) {
                canvas.line({x0, y0}, {x1, y1}, lua::rgba(color), thickness.value_or(1));
            },
            "fun(x0: number, y0: number, x1: number, y1: number, color?: any, thickness?: number)",
            "Line segment.")
        .fn(
            "image",
            [&canvas](const std::string& path, float x, float y, sol::optional<sol::table> options) {
                Rect area{x, y, 0, 0};
                if (options) {
                    area.w = lua::get(*options, "w", 0.0f);
                    area.h = lua::get(*options, "h", 0.0f);
                }
                canvas.image(path, area, imageOptions(options));
            },
            "fun(path: string, x: number, y: number, options?: {w?: number, h?: number, src?: number[], "
            "color?: any, "
            "flip_x?: boolean, flip_y?: boolean, linear?: boolean})",
            "Draw an image or part of one.")
        .fn(
            "nine_slice",
            [&canvas](const std::string& path, float x, float y, float w, float h, sol::object border,
                      sol::optional<sol::table> options) {
                glm::vec4 b(8);
                if (border.get_type() == sol::type::number) {
                    b = glm::vec4(border.as<float>());
                } else if (border.get_type() == sol::type::table) {
                    sol::table t = border;
                    b = {lua::component(t, "l", 1, 8), lua::component(t, "t", 2, 8),
                         lua::component(t, "r", 3, 8), lua::component(t, "b", 4, 8)};
                }
                canvas.nineSlice(path, {x, y, w, h}, b, imageOptions(options));
            },
            "fun(path: string, x: number, y: number, w: number, h: number, border: number|number[], "
            "options?: table)",
            "Stretchable window frame.")
        .fn(
            "text",
            [&canvas](const std::string& markup, float x, float y, sol::optional<sol::table> options) {
                const auto size = canvas.text(markup, {x, y}, textOptions(options));
                return std::make_tuple(size.x, size.y);
            },
            "fun(markup: string, x: number, y: number, options?: {font?: string, size?: number, color?: any, "
            "width?: number, align?: string, visible?: integer, shadow?: any, outline?: any, line_gap?: "
            "number}): number, number",
            "Draw text; returns its size.")
        .fn(
            "push_clip", [&canvas](float x, float y, float w, float h) { canvas.pushClip({x, y, w, h}); },
            "fun(x: number, y: number, w: number, h: number)",
            "Clip drawing to a rectangle (nested clips intersect).")
        .fn(
            "pop_clip", [&canvas] { canvas.popClip(); }, "fun()", "Restore the previous clip.")
        .fn(
            "push",
            [&canvas](sol::table t) {
                Transform2D transform;
                transform.translate = {lua::get(t, "x", 0.0f), lua::get(t, "y", 0.0f)};
                const float scale = lua::get(t, "scale", 1.0f);
                transform.scale = {lua::get(t, "sx", scale), lua::get(t, "sy", scale)};
                transform.origin = {lua::get(t, "ox", 0.0f), lua::get(t, "oy", 0.0f)};
                transform.rotation = lua::get(t, "rotation", 0.0f);
                transform.alpha = lua::get(t, "alpha", 1.0f);
                canvas.pushTransform(transform);
            },
            "fun(transform: {x?: number, y?: number, scale?: number, sx?: number, sy?: number, ox?: number, "
            "oy?: number, rotation?: number, alpha?: number})",
            "Push a 2D transform and opacity (UI animation).")
        .fn(
            "pop", [&canvas] { canvas.popTransform(); }, "fun()", "Pop the last transform.")
        .fn(
            "layer", [&canvas](int layer) { canvas.setLayer(layer); }, "fun(layer: integer)",
            "Draw subsequent calls on a layer; higher layers draw on top.")
        .fn(
            "additive", [&canvas](bool on) { canvas.setAdditive(on); }, "fun(enabled: boolean)",
            "Additive blending for subsequent calls (glows).")
        .fn(
            "quad_count", [&canvas] { return canvas.quads().size(); }, "fun(): integer",
            "Quads recorded this frame (tests and profiling).");
}
} // namespace fe
