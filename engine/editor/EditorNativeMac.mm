#include "engine/editor/EditorNative.h"
#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>

namespace fe {
bool cancelNativeFileDialog(SDL_Window* window) {
    if (!window) {
        return false;
    }
    NSWindow* cocoa = (__bridge NSWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!cocoa) {
        return false;
    }
    NSWindow* sheet = [cocoa attachedSheet];
    if (sheet && [sheet isKindOfClass:[NSSavePanel class]]) {
        [(NSSavePanel*)sheet cancel:nil];
        return true;
    }
    return false;
}
} // namespace fe
