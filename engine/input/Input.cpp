#include "engine/input/Input.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fe {
namespace { constexpr const char* names[]{"Up","Down","Left","Right","Confirm","Cancel","Menu","Previous","Next"}; }
Input::Input() : bindings_{SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT,SDL_SCANCODE_RETURN,SDL_SCANCODE_ESCAPE,SDL_SCANCODE_TAB,SDL_SCANCODE_Q,SDL_SCANCODE_E},
    buttons_{SDL_GAMEPAD_BUTTON_DPAD_UP,SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_LEFT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_EAST,SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER} {}
void Input::processEvent(const SDL_Event& event) {
    if (event.type==SDL_EVENT_WINDOW_FOCUS_LOST) {
        keys_.reset(); padButtons_.reset(); stick_={0,0}; pendingRelease_|=previous_; return;
    }
    if (event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) {
        const auto key=event.key.scancode;
        if (key<0 || key>=SDL_SCANCODE_COUNT) return;
        const bool down=event.type==SDL_EVENT_KEY_DOWN;
        keys_.set(static_cast<std::size_t>(key),down);
        if (down && !event.key.repeat) lastKey_=key;
        for (std::size_t i=0;i<ActionCount;++i) {
            bool matches=bindings_[i]==key;
            // WASD and Z/X remain convenient defaults until the user rebinds.
            matches|=(i==0 && bindings_[i]==SDL_SCANCODE_UP && key==SDL_SCANCODE_W) ||
                (i==1 && bindings_[i]==SDL_SCANCODE_DOWN && key==SDL_SCANCODE_S) ||
                (i==2 && bindings_[i]==SDL_SCANCODE_LEFT && key==SDL_SCANCODE_A) ||
                (i==3 && bindings_[i]==SDL_SCANCODE_RIGHT && key==SDL_SCANCODE_D) ||
                (i==4 && bindings_[i]==SDL_SCANCODE_RETURN && (key==SDL_SCANCODE_Z || key==SDL_SCANCODE_SPACE)) ||
                (i==5 && bindings_[i]==SDL_SCANCODE_ESCAPE && key==SDL_SCANCODE_X);
            if (matches && !event.key.repeat) (down?pendingPress_:pendingRelease_).set(i);
        }
    }
    if (event.type==SDL_EVENT_GAMEPAD_REMOVED && event.gdevice.which==activePad_) { activePad_=0; padButtons_.reset(); stick_={0,0}; }
    if (event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type==SDL_EVENT_GAMEPAD_BUTTON_UP) {
        if (activePad_ && activePad_!=event.gbutton.which) return;
        activePad_=event.gbutton.which;
        const auto button=event.gbutton.button;
        if (button>=SDL_GAMEPAD_BUTTON_COUNT) return;
        const bool down=event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN; padButtons_.set(button,down);
        for (std::size_t i=0;i<ActionCount;++i) if (buttons_[i]==button) (down?pendingPress_:pendingRelease_).set(i);
    }
    if (event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if (activePad_ && activePad_!=event.gaxis.which) return;
        activePad_=event.gaxis.which;
        float value=std::clamp(event.gaxis.value/32767.0f,-1.0f,1.0f);
        if (std::abs(value)<0.2f) value=0; else value=std::copysign((std::abs(value)-.2f)/.8f,value);
        if (event.gaxis.axis==SDL_GAMEPAD_AXIS_LEFTX) stick_.x=value;
        if (event.gaxis.axis==SDL_GAMEPAD_AXIS_LEFTY) stick_.y=value;
    }
}
ActionSnapshot Input::tick() {
    ActionSnapshot snapshot;
    for (std::size_t i=0;i<ActionCount;++i) snapshot.held[i]=keys_[bindings_[i]] || (buttons_[i]!=SDL_GAMEPAD_BUTTON_INVALID && padButtons_[buttons_[i]]);
    const SDL_Scancode aliases[]{SDL_SCANCODE_W,SDL_SCANCODE_S,SDL_SCANCODE_A,SDL_SCANCODE_D};
    const SDL_Scancode defaults[]{SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT};
    for (std::size_t i=0;i<4;++i) if (bindings_[i]==defaults[i]) snapshot.held[i]=snapshot.held[i]||keys_[aliases[i]];
    if (bindings_[4]==SDL_SCANCODE_RETURN) snapshot.held[4]=snapshot.held[4]||keys_[SDL_SCANCODE_Z]||keys_[SDL_SCANCODE_SPACE];
    if (bindings_[5]==SDL_SCANCODE_ESCAPE) snapshot.held[5]=snapshot.held[5]||keys_[SDL_SCANCODE_X];
    snapshot.movement={float(snapshot.down(Action::Right))-float(snapshot.down(Action::Left)),float(snapshot.down(Action::Down))-float(snapshot.down(Action::Up))};
    snapshot.held[0]=snapshot.held[0]||stick_.y<-.45f; snapshot.held[1]=snapshot.held[1]||stick_.y>.45f;
    snapshot.held[2]=snapshot.held[2]||stick_.x<-.45f; snapshot.held[3]=snapshot.held[3]||stick_.x>.45f;
    snapshot.pressed=(snapshot.held&~previous_)|pendingPress_;
    snapshot.released=(previous_&~snapshot.held)|pendingRelease_;
    if (glm::length(stick_)>glm::length(snapshot.movement)) snapshot.movement=stick_;
    if (glm::length(snapshot.movement)>1) snapshot.movement=glm::normalize(snapshot.movement);
    previous_=snapshot.held; pendingPress_.reset(); pendingRelease_.reset(); return snapshot;
}
void Input::rebind(Action action,SDL_Scancode key) {
    if (key<=SDL_SCANCODE_UNKNOWN || key>=SDL_SCANCODE_COUNT || key==SDL_SCANCODE_F1) throw std::invalid_argument("Invalid/reserved input key");
    const auto index=static_cast<std::size_t>(action); const auto old=bindings_.at(index);
    for (auto& current : bindings_) if (current==key) current=old;
    bindings_[index]=key; pendingPress_.reset(); previous_.reset();
}
SDL_Scancode Input::binding(Action action) const { return bindings_.at(static_cast<std::size_t>(action)); }
const char* Input::name(Action action) { return names[static_cast<std::size_t>(action)]; }
Json Input::toJson() const {
    Json result=Json::object();
    for (std::size_t i=0;i<ActionCount;++i) result[names[i]]=SDL_GetScancodeName(bindings_[i]);
    return result;
}
void Input::fromJson(const Json& data) {
    Input candidate=*this;
    for (std::size_t i=0;i<ActionCount;++i) if (data.contains(names[i])) candidate.rebind(static_cast<Action>(i),SDL_GetScancodeFromName(data.at(names[i]).get<std::string>().c_str()));
    *this=std::move(candidate);
}
bool InputRepeat::hit(const ActionSnapshot& input,Action action,double dt) {
    const auto i=static_cast<std::size_t>(action);
    if (!input.down(action)) { elapsed_[i]=0; next_[i]=.3; return input.hit(action); }
    if (input.hit(action)) { elapsed_[i]=0; next_[i]=.3; return true; }
    elapsed_[i]+=dt;
    if (elapsed_[i]>=next_[i]) { next_[i]=elapsed_[i]+.12; return true; }
    return false;
}
void InputRepeat::clear() { elapsed_.fill(0); next_.fill(.3); }
}
