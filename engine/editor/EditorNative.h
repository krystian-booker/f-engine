#pragma once
struct SDL_Window;

namespace fe {
// Dismisses a file dialog attached to the window as if the user cancelled
// it (macOS: the sheet's Cancel). For scripted walkthroughs of the real OS
// dialog; false when none is open or the platform has no way to do it.
bool cancelNativeFileDialog(SDL_Window* window);
} // namespace fe
