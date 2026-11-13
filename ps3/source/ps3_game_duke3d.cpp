// EDuke32-PS3: what the PS3 layer asks of Duke Nukem 3D (game.cpp).
//
// Copyright (C) 2026 the EDuke32-PS3 contributors. GPL v2 or later, as
// the rest of EDuke32.

#include "duke3d.h"
#include "savegame.h"

#include <malloc.h>
#include <math.h>

extern "C" void PS3_Log(const char *fmt, ...);

// The XMB's "Quit Game" (ps3layer.cpp): the settings are saved and sound
// and music stopped, as when quitting from the menu, without the screens
// the menu's quit shows. PS3_Exit does the rest.
extern "C" void PS3_GameQuit(void)
{
    G_Shutdown();
}

// ---------------------------------------------------------------------
// Autotest: USRDIR/autotest.txt (as reVC-PS3's sequence). The game starts
// in E1L1 (ps3layer.cpp adds -v1 -l1 -s4) and plays every level of every
// episode by itself, about a minute and a half each:
//
//   - god mode, all weapons, ammo and keys; walks (running), strafes,
//     turns (and turns away when stuck), fires, changes weapons, opens
//     doors / presses switches, jumps;
//   - at 25 s a save (autotest.esv), at 30 s it's loaded back;
//   - at 40 s god mode off and the player killed; Open until the level
//     starts again;
//   - then the next level (as the warp cheat: no bonus screen).
//
// Every step goes to the log ([autotest] lines), and every level's end
// with the heap in use: a hang or a crash leaves the last step it got to.
// ---------------------------------------------------------------------

int ps3_autotest;   // ps3layer.cpp sets it

static struct
{
    int      started;       // the first level is running
    int      done;          // every level played
    int      vol, lev;      // the level being played
    uint32_t levelStartMs;  // real time, for the budget
    int      setup, saved, loaded, killed, deaths;
    int32_t  lastClock;     // totalclock: it starts again from 0 on a (re)start
    uint32_t stuckMs;       // when the player last moved
    vec3_t   stuckPos;
    uint32_t turnUntilMs;
    int      turnDir;
    uint32_t lastOpenMs, lastWeaponMs, lastJumpMs;
} at;

static constexpr uint32_t AT_LEVEL_MS = 90000;   // real time per level, at most
static constexpr int AT_KEYMOVE = 40;            // player.cpp NORMALKEYMOVE

static uint32_t AT_Ms(void) { return timerGetTicks(); }

static int AT_NextLevel(int &vol, int &lev)
{
    for (int v = vol, l = lev + 1; v < g_volumeCnt && v < MAXVOLUMES; v++, l = 0)
        for (; l < MAXLEVELS; l++)
            // named in the CON files and really there (E1L9 is in GAME.CON's
            // list but not in the GRP: the autotest stopped at its error)
            if (g_mapInfo[v * MAXLEVELS + l].filename != NULL &&
                testkopen(g_mapInfo[v * MAXLEVELS + l].filename, 0))
            {
                vol = v;
                lev = l;
                return 1;
            }

    return 0;
}

static void AT_GiveAll(DukePlayer_t *const pPlayer)
{
    for (int w = PISTOL_WEAPON; w < MAX_WEAPONS; w++)
    {
        pPlayer->gotweapon |= (1 << w);
        P_AddAmmo(pPlayer, w, pPlayer->max_ammo_amount[w]);
    }

    pPlayer->got_access = 7;
}

static void AT_LevelReport(const char *why)
{
    struct mallinfo const mi = mallinfo();
    int sprites = 0;

    for (int i = 0; i < MAXSPRITES; i++)
        if (sprite[i].statnum < MAXSTATUS)
            sprites++;

    PS3_Log("[autotest] E%dL%d %s after %u s: %d death(s), %d sprites, heap %u KB in use",
            at.vol + 1, at.lev + 1, why, (unsigned)((AT_Ms() - at.levelStartMs) / 1000), at.deaths,
            sprites, (unsigned)(mi.uordblks >> 10));
}

// game.cpp's main loop, every pass (next to G_DoCheats)
void PS3_Autotest_Tick(void)
{
    if (!ps3_autotest || at.done)
        return;

    auto const pPlayer = g_player[myconnectindex].ps;

    // the level was finished (the bot hit the exit): no bonus screen or
    // episode ending waiting for a button, straight to the next one
    if (at.started && (pPlayer->gm & MODE_EOL))
    {
        int vol = at.vol, lev = at.lev;

        pPlayer->gm &= ~MODE_EOL;
        AT_LevelReport("finished (exit reached)");

        if (!AT_NextLevel(vol, lev))
        {
            at.done = 1;
            ud.god = 0;
            PS3_Log("[autotest] DONE: every level played");
            pPlayer->gm |= MODE_EOL;   // the game's own ending
            return;
        }

        ud.m_volume_number = ud.volume_number = vol;
        ud.m_level_number = ud.level_number = lev;
        pPlayer->gm |= MODE_RESTART;
        PS3_Log("[autotest] next: E%dL%d", vol + 1, lev + 1);
        return;
    }

    if (!(pPlayer->gm & MODE_GAME) || (pPlayer->gm & (MODE_MENU | MODE_RESTART | MODE_EOL)))
        return;

    // a level (re)started, or another one
    if (!at.started || at.vol != ud.volume_number || at.lev != ud.level_number)
    {
        if (at.started)
            AT_LevelReport("left");

        at.started = 1;
        at.vol = ud.volume_number;
        at.lev = ud.level_number;
        at.levelStartMs = AT_Ms();
        at.setup = at.saved = at.loaded = at.killed = at.deaths = 0;
        at.lastClock = (int32_t)totalclock;
        at.stuckMs = AT_Ms();
        at.stuckPos = pPlayer->pos;

        PS3_Log("[autotest] E%dL%d (%s) start", at.vol + 1, at.lev + 1,
                g_mapInfo[at.vol * MAXLEVELS + at.lev].filename);
    }

    int32_t const clock = (int32_t)totalclock;

    if (clock < at.lastClock)
    {
        PS3_Log("[autotest] E%dL%d restarted (game clock %d -> %d)", at.vol + 1, at.lev + 1, at.lastClock, clock);
        at.setup = 0;
    }

    at.lastClock = clock;

    uint32_t const levelMs = AT_Ms() - at.levelStartMs;
    int const gameSec = clock / TICRATE;
    int const alive = sprite[pPlayer->i].extra > 0;

    // god mode and everything, again after a load or a restart
    if (!at.setup && alive)
    {
        AT_GiveAll(pPlayer);
        at.setup = 1;
        PS3_Log("[autotest] E%dL%d: god mode, all weapons and keys", at.vol + 1, at.lev + 1);
    }

    if (!at.killed && alive)
        ud.god = 1;

    if (!at.saved && gameSec >= 25 && alive)
    {
        savebrief_t sv("autotest");
        Bstrcpy(sv.path, "autotest.esv");

        at.saved = 1;
        PS3_Log("[autotest] E%dL%d: saving", at.vol + 1, at.lev + 1);
        G_SavePlayerMaybeMulti(sv);
        PS3_Log("[autotest] E%dL%d: saved", at.vol + 1, at.lev + 1);
        return;
    }

    if (at.saved && !at.loaded && levelMs >= 30000 && alive)
    {
        savebrief_t sv("autotest");
        Bstrcpy(sv.path, "autotest.esv");

        at.loaded = 1;
        PS3_Log("[autotest] E%dL%d: loading", at.vol + 1, at.lev + 1);
        int const r = G_LoadPlayerMaybeMulti(sv);
        PS3_Log("[autotest] E%dL%d: loaded (%d)", at.vol + 1, at.lev + 1, r);
        at.lastClock = (int32_t)totalclock;
        at.setup = 0;
        return;
    }

    if (at.loaded && !at.killed && levelMs >= 40000 && alive)
    {
        at.killed = 1;
        at.deaths++;
        ud.god = 0;
        PS3_Log("[autotest] E%dL%d: killing the player", at.vol + 1, at.lev + 1);
        P_QuickKill(pPlayer);
        return;
    }

    if (levelMs >= AT_LEVEL_MS || (at.killed && alive && levelMs >= 60000))
    {
        int vol = at.vol, lev = at.lev;

        AT_LevelReport("done");

        if (!AT_NextLevel(vol, lev))
        {
            at.done = 1;
            ud.god = 0;
            PS3_Log("[autotest] DONE: every level played");
            return;
        }

        ud.m_volume_number = ud.volume_number = vol;
        ud.m_level_number = ud.level_number = lev;
        pPlayer->gm |= MODE_RESTART;
        PS3_Log("[autotest] next: E%dL%d", vol + 1, lev + 1);
    }
}

// P_GetInput, every frame in the game: the hands on the pad
void PS3_Autotest_Input(int const playerNum, input_t &input)
{
    if (!ps3_autotest || at.done || !at.started || playerNum != myconnectindex)
        return;

    auto const pPlayer = g_player[playerNum].ps;
    uint32_t const now = AT_Ms();
    float const t = (now - at.levelStartMs) * 0.001f;

    if (sprite[pPlayer->i].extra <= 0)
    {
        // dead: Open now and then, to start again
        if (now - at.lastOpenMs > 700)
        {
            localInput.bits |= BIT(SK_OPEN);
            at.lastOpenMs = now;
        }

        return;
    }

    // stuck (hardly moved for a second): turn away for half a second
    if (FindDistance2D(pPlayer->pos.x - at.stuckPos.x, pPlayer->pos.y - at.stuckPos.y) > 256)
    {
        at.stuckPos = pPlayer->pos;
        at.stuckMs = now;
    }
    else if (now - at.stuckMs > 1000 && now > at.turnUntilMs)
    {
        at.turnUntilMs = now + 500;
        at.turnDir = (now & 1) ? 1 : -1;
        at.stuckMs = now;
    }

    input.fvel = AT_KEYMOVE * 2;
    input.svel = (int16_t)(sinf(t * 0.7f) * AT_KEYMOVE);

    float turn = sinf(t * 0.9f) * 6.f;

    if (now < at.turnUntilMs)
        turn = at.turnDir * 40.f;

    input.q16avel = fix16_from_float(turn);
    input.q16horz = fix16_from_float(sinf(t * 0.5f) * 1.5f);

    // fire two seconds of every three
    if (fmodf(t, 3.f) < 2.f)
        localInput.bits |= BIT(SK_FIRE);

    if (now - at.lastOpenMs > 1500)
    {
        localInput.bits |= BIT(SK_OPEN);
        at.lastOpenMs = now;
    }

    if (now - at.lastWeaponMs > 5000 && (localInput.bits & SK_WEAPON_MASK) == 0)
    {
        localInput.bits |= 12 << SK_WEAPON_BITS;   // next weapon
        at.lastWeaponMs = now;
    }

    if (now - at.lastJumpMs > 7000)
    {
        localInput.bits |= BIT(SK_JUMP);
        at.lastJumpMs = now;
    }
}
