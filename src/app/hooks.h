#pragma once
#include "app/runtime.h"

// Each subsystem patches the loaded DLLs through one entry point, called by
// the loader before GameStart while no native thread is running.
namespace tmloader
{

void installSystemHooks(HMODULE module, bool game); // platform/system.cpp
void installNetworkHooks(HMODULE module);           // platform/network.cpp
void installDesktopHooks(HMODULE game);             // platform/desktop.cpp
void installAudioHooks(HMODULE game);               // platform/audio.cpp
void installInputHooks(HMODULE game);               // input/input.cpp
void installGraphicsHooks(HMODULE game);            // graphics/graphics.cpp
void initializeViewerSession(HMODULE viewer, Command* table);

} // namespace tmloader
