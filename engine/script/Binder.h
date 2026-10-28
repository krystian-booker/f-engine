#pragma once
#include "engine/script/LuaHost.h"
#include <filesystem>
#include <string>
#include <vector>

namespace fe {
// Documentation for every binding, written out as LuaLS annotations
// (lualib/types/fengine.d.lua) and Markdown (docs/api.md), so editor
// autocomplete and docs always match the registered functions.
class BindingDocs {
public:
    struct Entry {
        std::string owner; // "fe.world" or a class name
        std::string name, signature, doc;
        bool method = false, field = false;
    };
    struct Owner {
        std::string name, doc;
        bool isClass = false;
    };
    void owner(const std::string& name, const std::string& doc, bool isClass);
    void add(Entry entry) { entries_.push_back(std::move(entry)); }
    void writeStubs(const std::filesystem::path& file) const;
    void writeMarkdown(const std::filesystem::path& file) const;

private:
    std::vector<Owner> owners_;
    std::vector<Entry> entries_;
};

// Registers functions on a module table such as fe.world.
class ModuleBinder {
public:
    ModuleBinder(LuaHost& host, BindingDocs& docs, const std::string& name, const std::string& doc)
        : table_(host.module(name)), docs_(docs), name_(name) {
        docs_.owner(name, doc, false);
    }
    // `signature` is a LuaLS function type, e.g. "fun(id: string): Entity?".
    template<class F>
    ModuleBinder& fn(const char* name, F&& function, const char* signature, const char* doc) {
        table_.set_function(name, std::forward<F>(function));
        docs_.add({name_, name, signature, doc, false, false});
        return *this;
    }
    template<class V> ModuleBinder& value(const char* name, V&& value, const char* type, const char* doc) {
        table_[name] = std::forward<V>(value);
        docs_.add({name_, name, type, doc, false, true});
        return *this;
    }
    [[nodiscard]] sol::table table() const { return table_; }

private:
    sol::table table_;
    BindingDocs& docs_;
    std::string name_;
};

// Registers a usertype with documented methods and properties.
template<class T> class TypeBinder {
public:
    TypeBinder(LuaHost& host, BindingDocs& docs, const std::string& name, const std::string& doc)
        : type_(host.lua().new_usertype<T>(name, sol::no_constructor)), docs_(docs), name_(name) {
        docs_.owner(name, doc, true);
    }
    template<class F>
    TypeBinder& method(const char* name, F&& function, const char* signature, const char* doc) {
        type_.set_function(name, std::forward<F>(function));
        docs_.add({name_, name, signature, doc, true, false});
        return *this;
    }
    template<class G, class S>
    TypeBinder& property(const char* name, G&& get, S&& set, const char* type, const char* doc) {
        type_.set(name, sol::property(std::forward<G>(get), std::forward<S>(set)));
        docs_.add({name_, name, type, doc, false, true});
        return *this;
    }
    template<class G> TypeBinder& readonly(const char* name, G&& get, const char* type, const char* doc) {
        type_.set(name, sol::readonly_property(std::forward<G>(get)));
        docs_.add({name_, name, type, doc, false, true});
        return *this;
    }
    sol::usertype<T>& raw() { return type_; }

private:
    sol::usertype<T> type_;
    BindingDocs& docs_;
    std::string name_;
};
} // namespace fe
