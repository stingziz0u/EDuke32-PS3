// VoidSW-PS3: what the PS3 layer asks of Shadow Warrior (game.cpp).
//
// Copyright (C) 2026 the EDuke32-PS3 contributors. GPL v2 or later, as
// the rest of EDuke32.

#include "build.h"
#include "game.h"
#include "sounds.h"

void TermSetup(void);

// The XMB's "Quit Game" (ps3layer.cpp): the settings saved and the sound
// stopped, as TerminateGame does when quitting from the menu, without its
// screens. PS3_Exit does the rest.
extern "C" void PS3_GameQuit(void)
{
    TermSetup();
    UnInitSound();
}

// The autotest (USRDIR/autotest.txt) is Duke's only (ps3_game_duke3d.cpp):
// here the flag is read and nothing more.
int ps3_autotest;
