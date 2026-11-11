// EDuke32-PS3: what the PS3 layer asks of Duke Nukem 3D (game.cpp).
//
// Copyright (C) 2026 the EDuke32-PS3 contributors. GPL v2 or later, as
// the rest of EDuke32.

#include "duke3d.h"

// The XMB's "Quit Game" (ps3layer.cpp): the settings are saved and sound
// and music stopped, as when quitting from the menu, without the screens
// the menu's quit shows. PS3_Exit does the rest.
extern "C" void PS3_GameQuit(void)
{
    G_Shutdown();
}
