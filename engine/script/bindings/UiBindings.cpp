#include "engine/runtime/Engine.h"
#include "engine/script/bindings/Convert.h"

namespace fe {
namespace {
constexpr std::pair<const char*, TextAlign> Alignments[] = {
    {"left", TextAlign::Left}, {"center", TextAlign::Center}, {"right", TextAlign::Right}};

TextOptions textOptions(const std::optional<sol::table>& source) {
    TextOptions o;
    if (!source) {
        return o;
    }
    const auto& t = *source;
    o.font = lua::get<std::string>(t, "font", o.font);
    o.size = lua::get(t, "size", o.size);
    if (!(o.size >= 0 && o.size <= 1024)) {
        throw std::runtime_error("Text size must be 0 to 1024 (0 = the font's own size)");
    }
    o.color = lua::rgba(lua::field(t, "color"), o.color);
    o.width = lua::within(lua::get(t, "width", o.width), 0.0f, lua::VectorBound, "Text width");
    o.lineGap = lua::get(t, "line_gap", o.lineGap);
    o.align = lua::choice(lua::get<std::string>(t, "align", "left"), Alignments, "text align");
    if (const auto visible = lua::opt<std::int64_t>(lua::field(t, "visible"), "Field 'visible'")) {
        if (*visible < static_cast<std::int64_t>(std::min<std::size_t>(o.visible, INT64_MAX))) {
            o.visible = static_cast<std::size_t>(std::max<std::int64_t>(0, *visible));
        }
    }
    o.shadow = lua::rgba(lua::field(t, "shadow"), 0);
    o.outline = lua::rgba(lua::field(t, "outline"), 0);
    return o;
}

ImageOptions imageOptions(const std::optional<sol::table>& source) {
    ImageOptions o;
    if (!source) {
        return o;
    }
    const auto& t = *source;
    if (const auto src = lua::field(t, "src"); !lua::absent(src)) {
        const auto r = lua::rect(src);
        o.source = {r.x, r.y, r.z, r.w};
    }
    o.color = lua::rgba(lua::field(t, "color"), o.color);
    o.flipX = lua::get(t, "flip_x", false);
    o.flipY = lua::get(t, "flip_y", false);
    o.linear = lua::get(t, "linear", false);
    return o;
}
} // namespace

namespace {
// The script's tag resolver. TextSystem holds one wrapper around it for the
// engine's lifetime, so fe.text.resolver replacing or removing the function
// never destroys the code that is running it.
struct ResolverSlot {
    std::optional<sol::main_protected_function> function;
    int depth = 0;                     // nested resolver calls in progress
    std::optional<std::string> failed; // an error every nested call re-raises
};

constexpr int MaxResolverDepth = 8;

MarkupResolver resolverWrapper(LuaHost& host, const std::shared_ptr<ResolverSlot>& slot) {
    return [&host, slot](std::string_view tag) -> std::optional<std::string> {
        if (slot->failed) {
            throw std::runtime_error(*slot->failed);
        }
        if (!slot->function) {
            return std::nullopt;
        }
        if (slot->depth >= MaxResolverDepth) {
            slot->failed = "The text resolver nests more than " + std::to_string(MaxResolverDepth) +
                           " calls deep (it resolves markup with tags through fe.text; {" + std::string(tag) +
                           "} was being resolved)";
            throw std::runtime_error(*slot->failed);
        }
        // A copy: the resolver may replace or remove itself while it runs.
        const auto callback = *slot->function;
        struct Depth {
            ResolverSlot& slot;
            explicit Depth(ResolverSlot& s) : slot(s) { ++slot.depth; }
            ~Depth() {
                if (--slot.depth == 0) {
                    slot.failed.reset();
                }
            }
            Depth(const Depth&) = delete;
            Depth& operator=(const Depth&) = delete;
        } depth(*slot);
        auto result = [&] {
            const LuaHost::Callout callout(host);
            return callback(std::string(tag));
        }();
        if (!result.valid()) {
            const sol::error error = result;
            // A nested failure or the watchdog stops the whole resolution: the
            // error reaches the outermost fe.text call once instead of being
            // reported (and the tag retried) at every level.
            if (slot->failed || host.watchdogExpired()) {
                if (!slot->failed) {
                    slot->failed = error.what();
                }
                throw std::runtime_error(*slot->failed);
            }
            host.report(error.what());
            return std::nullopt;
        }
        const sol::object value = result;
        if (lua::absent(value)) {
            return std::nullopt;
        }
        if (value.get_type() != sol::type::string) {
            host.report("The text resolver returned " + lua::describe(value) + " for {" + std::string(tag) +
                        "}; it must return a string or nil");
            return std::nullopt;
        }
        return value.as<std::string>();
    };
}
} // namespace

void bindText(Engine& engine) {
    auto& host = engine.lua();
    auto& text = engine.text();
    const auto resolver = std::make_shared<ResolverSlot>();
    text.setResolver(resolverWrapper(host, resolver));
    // The binding below outlives the Lua state (LuaHost::keep); its function goes first.
    host.atClose([resolver] { resolver->function.reset(); });
    ModuleBinder(host, engine.docs(), "fe.text",
                 "Fonts, colors, icons and markup. Markup tags: {color:x} {icon:id} {wait:s} {speed:cps} "
                 "({speed:0} = the rest at once) {size:scale}...{/size} {b}...{/b} {shake}...{/shake} "
                 "{wave}...{/wave} {ruby:base|reading}; other {tags} are passed to the resolver set with "
                 "fe.text.resolver. Lines break at spaces and between CJK characters (kinsoku rules); "
                 "right-to-left scripts are not supported.")
        .fn(
            "font",
            [&engine](const std::string& name, sol::table spec) {
                auto data = luaToJson(spec);
                if (data.contains("bold")) {
                    engine.assets().defineFont(name + ".bold", data.at("bold"));
                    data.erase("bold");
                }
                engine.assets().defineFont(name, data);
            },
            "fun(name: string, spec: {files: string[], size?: number, pixel?: boolean, bold?: {files: "
            "string[], "
            "size?: number, pixel?: boolean}})",
            "Define a font: .fnt bitmap fonts and/or .ttf/.otf faces, first match wins per character. `bold` "
            "defines the {b} face (the font '<name>.bold'); without one, bold text is drawn twice, 1px "
            "apart.")
        .fn(
            "color",
            [&text](const std::string& name, sol::object value) {
                text.palette().define(name, lua::rgba(value));
            },
            "fun(name: string, color: Color)", "Name a color for {color:name}.")
        .fn(
            "icon",
            [&text](const std::string& id, sol::table spec) {
                IconDef icon;
                icon.image = lua::as<std::string>(lua::field(spec, "image"), "Icon 'image'");
                if (const auto rect = lua::field(spec, "rect"); !lua::absent(rect)) {
                    const auto r = lua::rect(rect);
                    icon.rect = {r.x, r.y, r.z, r.w};
                }
                text.defineIcon(id, icon);
            },
            "fun(id: string, spec: {image: string, rect?: Rect})", "Register an {icon:id} image.")
        .fn(
            "resolver",
            [resolver, &host](sol::object fn) {
                if (lua::absent(fn)) {
                    resolver->function.reset();
                    return;
                }
                if (fn.get_type() != sol::type::function) {
                    throw std::runtime_error("resolver must be a function or nil, got " + lua::describe(fn));
                }
                // Anchor on the main thread: the caller may be a coroutine that dies.
                // The host's message handler turns any error object into a string.
                resolver->function = sol::main_protected_function(fn, host.errorHandler());
            },
            "fun(resolver: (fun(tag: string): string?)|nil)",
            "Resolve custom tags such as {player} or {var:name}; nil removes the resolver. It runs under a C "
            "call (from fe.text, fe.canvas.text and the frame's text layout) and must not yield: a coroutine.yield "
            "in it raises 'attempt to yield across a C-call boundary'. A resolver may "
            "replace or remove itself; the call in progress finishes with the old one. Its errors are "
            "reported and the tag is shown as written. A resolver that resolves markup with tags through "
            "fe.text or fe.canvas.text nests: more than 8 nested resolver calls raise one error from the "
            "outermost fe.text or fe.canvas.text call, as does the watchdog stopping a resolver. Tags in "
            "a replacement resolve too, up to 4 levels deep; replacements totalling more than 1 MiB "
            "(1048576 bytes) for one call raise an error.")
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
                    if (glyphs[i].speed >= 0) {
                        speeds[i + 1] = glyphs[i].speed;
                    }
                }
                return lua.create_table_with("count", glyphs.size(), "wait", waits, "speed", speeds);
            },
            "fun(markup: string): {count: integer, wait: table<integer, number>, speed: table<integer, "
            "number>}",
            "Reveal units for typewriter effects. `speed[i]` is the reveal speed from unit i on (0 = show "
            "the rest at once); ruby readings appear with their base.")
        .fn(
            "measure",
            [&text](const std::string& markup, sol::object options) {
                const auto layout = text.layout(markup, textOptions(lua::options(options, "text options")));
                return std::make_tuple(layout.width, layout.height, layout.lines);
            },
            "fun(markup: string, options?: TextOptions): number, number, integer",
            "Width, height and line count.")
        .fn(
            "paginate",
            [&text](const std::string& markup, sol::table options, int lines) {
                return sol::as_table(
                    text.paginate(markup, textOptions(options), lua::atLeast(lines, 1, "lines")));
            },
            "fun(markup: string, options: TextOptions, lines: integer): string[]",
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
            "fun(x: number, y: number, w: number, h: number, color?: Color)", "Filled rectangle.")
        .fn(
            "line",
            [&canvas](float x0, float y0, float x1, float y1, sol::object color, sol::object thickness) {
                canvas.line(
                    {x0, y0}, {x1, y1}, lua::rgba(color),
                    lua::atLeast(lua::opt<float>(thickness, "thickness").value_or(1), 0.0f, "thickness"));
            },
            "fun(x0: number, y0: number, x1: number, y1: number, color?: Color, thickness?: number)",
            "Line segment.")
        .fn(
            "image",
            [&canvas](const std::string& path, float x, float y, sol::object settings) {
                const auto options = lua::options(settings, "image options");
                Rect area{x, y, 0, 0};
                if (options) {
                    area.w = lua::get(*options, "w", 0.0f);
                    area.h = lua::get(*options, "h", 0.0f);
                }
                canvas.image(path, area, imageOptions(options));
            },
            "fun(path: string, x: number, y: number, options?: {w?: number, h?: number, src?: Rect, color?: "
            "Color, "
            "flip_x?: boolean, flip_y?: boolean, linear?: boolean})",
            "Draw an image or part of one. 'rt:<name>' draws the render target a world drew into this frame "
            "(world:draw{target = name}); its size is the draw's rect in canvas pixels.")
        .fn(
            "nine_slice",
            [&canvas](const std::string& path, float x, float y, float w, float h, sol::object border,
                      sol::object options) {
                glm::vec4 b;
                if (border.get_type() == sol::type::number) {
                    b = glm::vec4(lua::as<float>(border, "border"));
                } else {
                    const auto t =
                        lua::vectorTable(border, "a border (a number or {l, t, r, b})", {"l", "t", "r", "b"});
                    b = {lua::component(t, "l", 1, 8), lua::component(t, "t", 2, 8),
                         lua::component(t, "r", 3, 8), lua::component(t, "b", 4, 8)};
                }
                if (glm::any(glm::lessThan(b, glm::vec4(0)))) {
                    throw std::runtime_error("nine_slice borders must not be negative");
                }
                canvas.nineSlice(path, {x, y, w, h}, b, imageOptions(lua::options(options, "image options")));
            },
            "fun(path: string, x: number, y: number, w: number, h: number, border: number|{l?: number, t?: "
            "number, r?: number, b?: number}, options?: {src?: Rect, color?: Color, flip_x?: boolean, "
            "flip_y?: "
            "boolean, linear?: boolean})",
            "Stretchable window frame; `border` is the edge width in image pixels (one number, or each side; "
            "missing sides are 8).")
        .fn(
            "text",
            [&canvas](const std::string& markup, float x, float y, sol::object options) {
                const auto size =
                    canvas.text(markup, {x, y}, textOptions(lua::options(options, "text options")));
                return std::make_tuple(size.x, size.y);
            },
            "fun(markup: string, x: number, y: number, options?: TextOptions): number, number",
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
                // Scripts give degrees; the canvas works in radians.
                transform.translate = {lua::get(t, "x", 0.0f), lua::get(t, "y", 0.0f)};
                const float scale = lua::get(t, "scale", 1.0f);
                transform.scale = {lua::get(t, "sx", scale), lua::get(t, "sy", scale)};
                transform.origin = {lua::get(t, "ox", 0.0f), lua::get(t, "oy", 0.0f)};
                transform.rotation = glm::radians(lua::get(t, "rotation", 0.0f));
                transform.alpha = lua::get(t, "alpha", 1.0f);
                canvas.pushTransform(transform);
            },
            "fun(transform: {x?: number, y?: number, scale?: number, sx?: number, sy?: number, ox?: number, "
            "oy?: number, rotation?: number, alpha?: number})",
            "Push a 2D transform and opacity (UI animation): translation, scale about the origin (`ox`, "
            "`oy`), `rotation` in degrees (clockwise on screen) about the origin, and `alpha` (held within "
            "0..1, "
            "multiplied with the enclosing transforms').")
        .fn(
            "pop", [&canvas] { canvas.popTransform(); }, "fun()", "Pop the last transform.")
        .fn(
            "set_layer", [&canvas](int layer) { canvas.setLayer(layer); }, "fun(layer: integer)",
            "Draw subsequent calls on a layer; higher layers draw on top.")
        .fn(
            "set_additive", [&canvas](bool on) { canvas.setAdditive(on); }, "fun(enabled: boolean)",
            "Additive blending for subsequent calls (glows).")
        .fn(
            "quad_count", [&canvas] { return canvas.quads().size(); }, "fun(): integer",
            "Quads recorded this frame (tests and profiling).");
}
} // namespace fe
