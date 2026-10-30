#pragma once
#include "engine/script/LuaHost.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace fe {
// Documentation for every binding, written out as LuaLS annotations
// (lualib/types/fengine.d.lua) and Markdown (docs/api.md), so editor
// autocomplete and docs always match the registered functions.
class BindingDocs {
public:
    enum class Kind { Function, Method, Field, ReadOnlyField, Operator };
    struct Entry {
        std::string owner; // "fe.world" or a class name
        std::string name, signature, doc;
        Kind kind = Kind::Function;
        // Parameters the C++ reads (without self and sol::this_state), or -1
        // when unknown; add() checks it against the signature's count.
        int arity = -1;
    };
    struct Owner {
        std::string name, doc;
        bool isClass = false;
    };
    // A named type used in signatures, e.g. Cell = "{x: integer, z: integer, layer?: integer}".
    struct Alias {
        std::string name, type, doc;
    };
    void owner(const std::string& name, const std::string& doc, bool isClass);
    void alias(Alias alias) { aliases_.push_back(std::move(alias)); }
    // Records an entry. A function or method whose LuaLS signature ("fun(...)")
    // declares a different number of parameters than its C++ reads is a
    // documentation bug: development builds (FENGINE_DEV) throw
    // std::logic_error here, at registration, and every build refuses to
    // write the API files (problems()), so tools/ci/check_api.py fails too.
    void add(Entry entry);
    [[nodiscard]] const std::vector<std::string>& problems() const { return problems_; }
    void writeStubs(const std::filesystem::path& file) const;
    void writeMarkdown(const std::filesystem::path& file) const;
    // Parameters a LuaLS function type declares: "fun(a: x, b?: {c: y, d: z}): r" has 2.
    static int signatureArity(const std::string& signature);

private:
    void checkWritable() const;
    std::vector<Alias> aliases_;
    std::vector<Owner> owners_;
    std::vector<Entry> entries_;
    std::vector<std::string> problems_;
};

namespace detail {
// The call signature of a lambda or function, so a wrapper can take exactly
// the same parameters (sol2 reads the arguments from it).
template<class F> struct Signature : Signature<decltype(&F::operator())> {};
template<class C, class R, class... A> struct Signature<R (C::*)(A...) const> {
    using Type = R(A...);
};
template<class R, class... A> struct Signature<R (*)(A...)> {
    using Type = R(A...);
};
template<class R, class... A> struct Signature<R(A...)> {
    using Type = R(A...);
};

// How many parameters a binding reads from Lua: its C++ parameters without
// sol::this_state (which sol2 supplies). A method's object counts as one.
template<class T> struct ParameterCount;
template<class R, class... A> struct ParameterCount<R(A...)> {
    static constexpr int value =
        ((std::is_same_v<std::remove_cvref_t<A>, sol::this_state> ? 0 : 1) + ... + 0);
};

// Raises "'fe.app.title' takes no arguments (got 1)", naming the set_ twin of
// a getter when the owner module has one.
[[noreturn]] void noArguments(LuaHost& host, const std::string& function, bool method, int got);

template<class F, class R, class... A> auto guard(LuaHost& host, F function, R (*)(A...)) {
    return [&host, function = std::move(function)](A... args) -> R {
        const LuaHost::BindingCall call(host);
        return function(std::forward<A>(args)...);
    };
}

// The documented name of an engine type (TypeBinder records it).
template<class T> std::string& engineTypeName() {
    static std::string name;
    return name;
}

template<class T> struct OptionalValue : std::false_type {};
template<class T> struct OptionalValue<sol::optional<T>> : std::true_type {
    using Type = T;
};

// Parameters other than engine objects are converted to values.
template<class T>
constexpr bool byValue =
    std::is_arithmetic_v<T> || std::is_same_v<T, std::string> || std::is_same_v<T, sol::object> ||
    std::is_same_v<T, sol::table> || std::is_same_v<T, sol::this_state> || OptionalValue<T>::value;

template<class T> std::string expectedName() {
    if constexpr (OptionalValue<T>::value) {
        return expectedName<typename OptionalValue<T>::Type>() + " or nil";
    } else if constexpr (std::is_same_v<T, bool>) {
        return "boolean";
    } else if constexpr (std::is_integral_v<T>) {
        return "integer";
    } else if constexpr (std::is_floating_point_v<T>) {
        return "number";
    } else if constexpr (std::is_same_v<T, std::string>) {
        return "string";
    } else if constexpr (std::is_same_v<T, sol::table>) {
        return "table";
    } else {
        return engineTypeName<T>().empty() ? std::string("engine object") : engineTypeName<T>();
    }
}

// Raises "bad argument #n to 'fe.world.load' (string expected, got table)".
[[noreturn]] void badArgument(lua_State* state, const std::string& function, int index,
                              const std::string& expected);

// What an integer parameter of type T accepts, for error messages.
template<class T> std::string integerRange() {
    if constexpr (sizeof(T) >= sizeof(lua_Integer) && std::is_signed_v<T>) {
        return "integer";
    } else {
        return "integer from " + std::to_string(std::numeric_limits<T>::min()) + " to " +
               std::to_string(std::numeric_limits<T>::max());
    }
}

// sol2 accepts any number for a numeric parameter and converts it with a
// C++ cast, so 2^32 + 1 would reach an int parameter as 1 and 1e300 a float
// one as infinity. Integers must fit the parameter's type and numbers must be
// finite and fit it; anything else raises.
template<class T> void checkNumber(lua_State* state, int index, const std::string& function) {
    if constexpr (OptionalValue<T>::value) {
        if (!lua_isnoneornil(state, index)) {
            checkNumber<typename OptionalValue<T>::Type>(state, index, function);
        }
    } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        int exact = 0;
        const lua_Integer value = lua_tointegerx(state, index, &exact);
        bool fits = exact != 0;
        if constexpr (std::is_signed_v<T>) {
            fits = fits && value >= static_cast<lua_Integer>(std::numeric_limits<T>::min()) &&
                   value <= static_cast<lua_Integer>(std::numeric_limits<T>::max());
        } else {
            fits = fits && value >= 0 &&
                   static_cast<std::uint64_t>(value) <=
                       static_cast<std::uint64_t>(std::numeric_limits<T>::max());
        }
        if (!fits) {
            badArgument(state, function, index, integerRange<T>());
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        const lua_Number value = lua_tonumber(state, index);
        if (!std::isfinite(value) ||
            std::abs(value) > static_cast<lua_Number>(std::numeric_limits<T>::max())) {
            badArgument(state, function, index, "finite number");
        }
    }
}

template<class A>
using Stored =
    std::conditional_t<byValue<std::remove_cvref_t<A>>, std::remove_cvref_t<A>, std::remove_cvref_t<A>&>;

template<class A> Stored<A> argument(lua_State* state, int& next, const std::string& function) {
    using T = std::remove_cvref_t<A>;
    if constexpr (std::is_same_v<T, sol::this_state>) {
        return sol::this_state{state};
    } else {
        const int index = next++;
        if constexpr (!std::is_same_v<T, sol::object>) {
            const auto quiet = [](lua_State*, int, sol::type, sol::type, auto&&...) noexcept {
                return 0;
            };
            if (!sol::stack::check<T>(state, index, quiet)) {
                badArgument(state, function, index, expectedName<T>());
            }
            checkNumber<T>(state, index, function);
        }
        if constexpr (byValue<T>) {
            return sol::stack::get<T>(state, index);
        } else {
            return sol::stack::get<T&>(state, index);
        }
    }
}

// Like guard, but the wrapper reads the arguments itself, so a wrong one
// raises an error naming the documented types instead of sol2's C++ names. A
// function or method that takes no arguments rejects any it is given, so a
// getter called with a setter's old spelling raises instead of only reading.
template<class F, class R, class... A>
auto convertingGuard(LuaHost& host, std::string function, bool method, F implementation, R (*)(A...)) {
    constexpr int count = ParameterCount<R(A...)>::value;
    return [&host, function = std::move(function), method, implementation = std::move(implementation)](
               [[maybe_unused]] sol::this_state state, sol::variadic_args) -> R {
        const LuaHost::BindingCall call(host);
        (void)method; // only read for functions and methods without arguments
        if constexpr (count == 0 || count == 1) {
            if (count == (method ? 1 : 0) && lua_gettop(state) > count) {
                noArguments(host, function, method, lua_gettop(state) - count);
            }
        }
        // Unused when the function takes no arguments.
        [[maybe_unused]] int next = 1;
        // Braced initialisation reads the arguments left to right.
        std::tuple<Stored<A>...> arguments{argument<A>(state, next, function)...};
        return std::apply(implementation, arguments);
    };
}
} // namespace detail

// `function` wrapped so every call runs inside a LuaHost::BindingCall: it
// refuses once the engine is shutting down, and no finalizer runs Lua code
// while it executes. Every binding registered through the binders below is
// wrapped this way.
template<class F> auto guarded(LuaHost& host, F&& function) {
    using Function = std::decay_t<F>;
    using Type = typename detail::Signature<Function>::Type;
    return detail::guard(host, Function(std::forward<F>(function)), static_cast<Type*>(nullptr));
}

// guarded, also reading the arguments (functions and methods; `name` is how
// errors name the function, e.g. "fe.world.load" or "World:spawn"; a method's
// first parameter is the object).
template<class F> auto guardedCall(LuaHost& host, std::string name, bool method, F&& function) {
    using Function = std::decay_t<F>;
    using Type = typename detail::Signature<Function>::Type;
    return detail::convertingGuard(host, std::move(name), method, Function(std::forward<F>(function)),
                                   static_cast<Type*>(nullptr));
}

// The arguments a function or method reads from Lua (see ParameterCount).
template<class F> constexpr int arity(bool method) {
    return detail::ParameterCount<typename detail::Signature<std::decay_t<F>>::Type>::value - (method ? 1 : 0);
}

// Registers functions on a module table such as fe.world.
class ModuleBinder {
public:
    ModuleBinder(LuaHost& host, BindingDocs& docs, const std::string& name, const std::string& doc)
        : host_(host), table_(host.module(name)), docs_(docs), name_(name) {
        docs_.owner(name, doc, false);
    }
    // `signature` is a LuaLS function type, e.g. "fun(id: string): Entity?".
    template<class F>
    ModuleBinder& fn(const char* name, F&& function, const char* signature, const char* doc) {
        // Lua holds only a pointer; the host owns the function (LuaHost::keep).
        auto* stored =
            host_.keep(guardedCall(host_, name_ + "." + name, false, std::forward<F>(function)));
        auto call = [stored](sol::this_state state, sol::variadic_args args) -> decltype(auto) {
            return (*stored)(state, args);
        };
        static_assert(std::is_trivially_destructible_v<decltype(call)>);
        table_.set_function(name, call);
        docs_.add({name_, name, signature, doc, BindingDocs::Kind::Function, arity<F>(false)});
        return *this;
    }

private:
    LuaHost& host_;
    sol::table table_;
    BindingDocs& docs_;
    std::string name_;
};

// Registers a usertype with documented methods and properties. Only these are
// visible to scripts once LuaHost::sealTypes runs.
template<class T> class TypeBinder {
public:
    TypeBinder(LuaHost& host, BindingDocs& docs, const std::string& name, const std::string& doc)
        : host_(host), type_(host.lua().new_usertype<T>(name, sol::no_constructor)),
          members_(host.exposeType(name, sol::usertype_traits<T>::metatable())), docs_(docs), name_(name) {
        docs_.owner(name, doc, true);
        detail::engineTypeName<T>() = name;
        // "<Type>: 0x...", like tables; a type may document its own with op().
        type_[sol::meta_function::to_string] = guarded(host_, [name](const T& self) {
            char address[32];
            std::snprintf(address, sizeof(address), "%p", static_cast<const void*>(&self));
            return name + ": " + address;
        });
    }
    template<class F>
    TypeBinder& method(const char* name, F&& function, const char* signature, const char* doc) {
        type_.set_function(name, guardedCall(host_, name_ + ":" + name, true, std::forward<F>(function)));
        members_.readable.insert(name);
        docs_.add({name_, name, signature, doc, BindingDocs::Kind::Method, arity<F>(true)});
        return *this;
    }
    template<class G, class S>
    TypeBinder& property(const char* name, G&& get, S&& set, const char* type, const char* doc) {
        type_.set(name,
                  sol::property(guarded(host_, std::forward<G>(get)), guarded(host_, std::forward<S>(set))));
        members_.readable.insert(name);
        members_.writable.insert(name);
        docs_.add({name_, name, type, doc, BindingDocs::Kind::Field});
        return *this;
    }
    template<class G> TypeBinder& readonly(const char* name, G&& get, const char* type, const char* doc) {
        type_.set(name, sol::readonly_property(guarded(host_, std::forward<G>(get))));
        members_.readable.insert(name);
        docs_.add({name_, name, type, doc, BindingDocs::Kind::ReadOnlyField});
        return *this;
    }
    // A metamethod; `syntax` is how scripts use it, e.g. "a == b".
    template<class F>
    TypeBinder& op(sol::meta_function function, F&& implementation, const char* syntax, const char* doc) {
        type_[function] = guarded(host_, std::forward<F>(implementation));
        docs_.add({name_, syntax, "", doc, BindingDocs::Kind::Operator});
        return *this;
    }

private:
    LuaHost& host_;
    sol::usertype<T> type_;
    LuaHost::TypeMembers& members_;
    BindingDocs& docs_;
    std::string name_;
};
} // namespace fe
