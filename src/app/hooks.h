#pragma once
#include "app/runtime.h"

// Each subsystem patches the loaded DLLs through one entry point, called by
// the loader before GameStart while no native thread is running.
namespace tmloader
{

void installSystemHooks(HMODULE module, bool game);
void installNetworkHooks(HMODULE module);
void installDesktopHooks(HMODULE game);
void installAudioHooks(HMODULE game);
void installInputHooks(HMODULE game);
void installGraphicsHooks(HMODULE game);
void initializeViewerSession(HMODULE viewer, Command* table);

} // namespace tmloader
