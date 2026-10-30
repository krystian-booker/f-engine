// LuaHost::BindingCall and LuaHost::Callout: holding and releasing the
// garbage collector around engine bindings. This is the only file that reaches
// into Lua's global state, so it is the only file that includes Lua's internal
// headers.
//
// The public API's LUA_GCSTOP/LUA_GCRESTART would also reset the collector's
// debt, forcing a collection after every binding call. Only the GCSTPUSR bit
// of global_State::gcstp is touched (scripts cannot stop the collector
// themselves: collectgarbage only counts and steps), so the collector's own
// stops while it runs finalizers or closes the state (GCSTPGC, GCSTPCLS) are
// left alone.
#include "engine/script/LuaHost.h"

// The gcstp byte and its GCSTPUSR bit exist in this form in Lua 5.4 only
// (lstate.h: `lu_byte gcstp;`, lgc.h: `#define GCSTPUSR 1`). A Lua upgrade
// must revisit this file; the patches in cmake/LuaPatches.cmake fail loudly
// on their own, this guard does the same for the hold.
static_assert(LUA_VERSION_NUM == 504, "GcHold.cpp touches global_State::gcstp (GCSTPUSR), a Lua 5.4 "
                                      "internal; check lgc.h and lstate.h before building against another Lua");

// Included last: these headers define short macros.
#include "lgc.h"
#include "lstate.h"

static_assert(GCSTPUSR == 1, "Lua renamed or renumbered the user stop bit of global_State::gcstp");

namespace fe {
LuaHost::BindingCall::BindingCall(LuaHost& host) : state_(host.lua_ ? host.lua_->lua_state() : nullptr) {
    if (host.closing_ || !state_) {
        throw std::runtime_error("engine is shutting down");
    }
    global_State* g = G(state_);
    if ((g->gcstp & GCSTPUSR) == 0) {
        g->gcstp |= GCSTPUSR;
        held_ = true;
    }
}

LuaHost::BindingCall::~BindingCall() {
    if (held_) {
        G(state_)->gcstp &= static_cast<lu_byte>(~GCSTPUSR);
    }
}

LuaHost::Callout::Callout(LuaHost& host) : state_(host.lua_ ? host.lua_->lua_state() : nullptr) {
    if (!state_) {
        return;
    }
    global_State* g = G(state_);
    if ((g->gcstp & GCSTPUSR) != 0) {
        g->gcstp &= static_cast<lu_byte>(~GCSTPUSR);
        released_ = true;
    }
}

LuaHost::Callout::~Callout() {
    if (released_) {
        G(state_)->gcstp |= GCSTPUSR;
    }
}
} // namespace fe
