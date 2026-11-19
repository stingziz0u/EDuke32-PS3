# EDuke32-PS3

A native homebrew port of [EDuke32](https://www.eduke32.com/) and its
sibling VoidSW for the PS3: Duke Nukem 3D and Shadow Warrior, each in its
own package.

## Lineage

EDuke32 is the long-running, actively maintained source port of Duke Nukem
3D, built on the source code 3D Realms released in 2003 and Ken Silverman's
Build engine. Its repository also hosts VoidSW, the Shadow Warrior port
(from Jonathon Fowler's JFSW). This project adds a full PS3 platform layer
underneath both (video on the RSX, audio, input, system integration). In
short:

```
Duke Nukem 3D (3D Realms, 1996)  -> EDuke32 -> EDuke32-PS3 (this project)
Shadow Warrior (3D Realms, 1997) -> VoidSW  -> EDuke32-PS3 (this project)
```

One engine, one PS3 layer, two programs and two packages.

## Tested games

Only **Duke Nukem 3D** (with *Duke It Out in D.C.*, *Duke Caribbean: Life's
a Beach* and *Duke: Nuclear Winter*) and **Shadow Warrior** (with *Wanton
Destruction* and *Twin Dragon*) have been tested on real hardware.

Everything else EDuke32 and VoidSW support should work the same way, but
hasn't been tested: NAM, Napalm, WWII GI, other versions of the games'
data, and mods.

## Features

- **One package per game, one launcher**: the base game, its add-ons and
  your mods in a single install. At start a menu lists what is installed
  (skipped when there is only one thing to play); Quit from the game's menu
  comes back to it, and so does an error, shown under the list.
- **Add-ons and mods**: the game's data in `USRDIR` or in `USRDIR/addons`
  (less clutter), each add-on in its own folder inside `addons` if you
  like. Add-ons are recognized by their file name, so versions EDuke32
  doesn't list by checksum (CD, GOG, Steam, repacks) work too, and they run
  on top of whichever full version of the base game you have. Any other
  `.grp`, `.zip` or `.pk3` in `addons` shows up on the launcher's MODS tab.
- **Separate saves per game**: each base game, add-on and mod keeps its own
  saves in `USRDIR/saves/<name>/`. Each save has its picture in the
  Load/Save menus, drawn by the software renderer at 320x200 (the GPU's
  frame can't be read back), as in Shadow Warrior.
- **Video**: EDuke32's Polymost renderer drawn by the RSX, at a stable 60
  fps. The internal resolution (up to 1280x720) is scaled by the RSX to
  720p output. Frame rate limit (60 or 30 fps) and an FPS display are in
  the video settings, and a **TV screen fit** (70-100 %) shrinks the
  picture for TVs that cut its edges (overscan): Duke's Options -> Display
  Setup, Shadow Warrior's Options -> Screen Menu, which also has the
  **Texture Filter** (smooth or sharp textures).
- **Audio**: 48000 Hz on the PS3's audio port. Duke's MIDI music through
  the OPL3 emulation or a **SoundFont** (`.sf2`), synthesized on an SPU so
  it costs the game nothing. Shadow Warrior's CD soundtrack as OGG or FLAC
  files.
- **Input**: DualShock 3 for movement, look and every action, all
  rebindable from the in-game menus, plus an optional **lock-on** (unbound
  by default). The PC-only settings (keyboard, mouse, display mode) are
  gone, and the on-screen texts name the DS3's buttons.
- **File names in any case**: `DUKE3D.GRP`, `duke3d.grp` and `Duke3D.grp`
  are all found, even though the PS3's file system tells them apart.
- Clean exit to the XMB from the game's Quit option or the PS button.

## Requirements

- A PS3 capable of running homebrew (HEN or CFW).
- Your own legally-owned copy of the game(s). **No game data is included
  in this repository or in any release build** -- see
  [Installation](#installation).
- To build from source: the [ps3dev / PSL1GHT](https://github.com/ps3dev)
  toolchain (`ppu-gcc`, `spu-gcc`, PSL1GHT SDK, `make_self_npdrm`,
  `pkg.py`).

## Installation

### Duke Nukem 3D

Install `eduke32-ps3-1.1.pkg` from the XMB (shows up as "EDuke32"). This
creates `/dev_hdd0/game/DUKE3DPS3/USRDIR/` with an empty `addons/` folder.
By FTP, place your own game data there:

```
DUKE3DPS3/USRDIR/DUKE3D.GRP            <- required (Atomic Edition, 1.3D or shareware)
DUKE3DPS3/USRDIR/addons/DUKEDC.GRP     <- optional, Duke It Out in D.C.
DUKE3DPS3/USRDIR/addons/VACATION.GRP   <- optional, Duke Caribbean: Life's a Beach
DUKE3DPS3/USRDIR/addons/NWINTER.GRP    <- optional, Duke: Nuclear Winter
DUKE3DPS3/USRDIR/addons/NWINTER.CON    <- (with Nuclear Winter, if your copy
                                           has it as a separate file)
DUKE3DPS3/USRDIR/GeneralUser-GS.sf2    <- optional, any SoundFont for the music
```

An add-on needs every file its release came with: some keep their `.CON`
scripts inside the `.GRP`, others (Nuclear Winter) next to it. NAM
(`NAM.GRP`), Napalm (`NAPALM.GRP`) and WWII GI (`WW2GI.GRP`) go in the same
folders.

For the music, Options -> Sound Setup -> Music synth picks "Original" (the
OPL3 emulation) or "SoundFont"; the first `.sf2` in `USRDIR` is used.

### Shadow Warrior

Install `voidsw-ps3-1.1.pkg` from the XMB (shows up as "VoidSW"). This
creates `/dev_hdd0/game/SHADOWPS3/USRDIR/` with empty `addons/` and
`music/` folders. By FTP:

```
SHADOWPS3/USRDIR/SW.GRP                <- required (registered 1.2 or shareware)
SHADOWPS3/USRDIR/addons/WT.GRP         <- optional, Wanton Destruction
SHADOWPS3/USRDIR/addons/TD.GRP         <- optional, Twin Dragon
SHADOWPS3/USRDIR/music/track02.ogg     <- optional, the CD soundtrack:
...                                       track02 .. track14, .ogg or .flac
```

The GOG release (Shadow Warrior Classic Redux) has all of these: `SW.GRP`,
the add-ons, and the soundtrack in its own `music` folder, whose files go
in `music/` as they are.

## Building from source

1. Install the [ps3dev toolchain](https://github.com/ps3dev/ps3toolchain)
   (third-party, not part of this project -- these are its own official
   instructions):

   ```bash
   git clone https://github.com/ps3dev/ps3toolchain.git
   cd ps3toolchain
   export PS3DEV=/usr/local/ps3dev
   export PSL1GHT=$PS3DEV
   export PATH="$PATH:$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin"
   ./toolchain.sh
   ```

2. Clone this repository and build:

   ```bash
   git clone https://github.com/stingziz0u/EDuke32-PS3.git
   cd EDuke32-PS3
   make -C ps3 -j$(nproc)            # -> ps3/eduke32-ps3-1.1.pkg
   make -C ps3 GAME=sw -j$(nproc)    # -> ps3/voidsw-ps3-1.1.pkg
   ```

The SoundFont synthesizer's SPU program needs `spu-gcc`; without it, or
with `SPU=0`, the SoundFont is synthesized on the PPU instead.

The `ps3/` directory holds everything specific to this port:

- **`Makefile`** -- builds the engine, the game (`GAME=duke3d`, the
  default, or `GAME=sw`), the PS3 layer and the SPU program, and packages
  them.
- **`source/`** -- the PS3 layer: `ps3layer.cpp` (the engine's platform
  layer: video modes, the DualShock 3, events, the XMB), `ps3_video.c` (the
  RSX), `gl/` (ps3gl, the OpenGL subset Polymost draws with, on the RSX),
  `driver_ps3.cpp` (the audio port), `ps3_sf2spu.c` (the SoundFont on an
  SPU), `ps3_launcher.c`, `ps3_stdio.c` (files), the log.
- **`spu/`** -- the SoundFont synthesizer's SPU program.
- **`ICON0.PNG`, `ICON0-SW.PNG`** -- the XMB icons.

### Logs

The release build writes nothing to the HDD but your saves and settings.
For troubleshooting, put a file named `logs.txt` (any content, even empty)
in the game's `USRDIR`: the next start writes `eduke32-ps3.log` /
`voidsw-ps3.log` (the PS3 layer, with fps and memory figures every 10
seconds, the previous run kept as `.old.log`) and the engine's own
`eduke32.log` / `voidsw.log` next to it. Delete `logs.txt` to turn them off
again.

## Controls

| Action | Duke Nukem 3D | Shadow Warrior |
|---|---|---|
| Move / Strafe | Left stick | Left stick |
| Turn / Look | Right stick | Right stick |
| Fire | R2 | R2 |
| Jump | L2 | L2 |
| Open / Use | Cross | Cross |
| Use item | Circle, Square | Square |
| Quick kick | Triangle | -- |
| Crouch | L1 (R3: toggle) | L1, R3 |
| Run | L3 | L3 |
| Alternate fire | R1 | R1 |
| Prev / Next weapon | D-pad up / down | D-pad up / down |
| Prev / Next item | D-pad left / right | D-pad left / right |
| Map | Select | Select |
| Lock on | *(unbound)* | *(unbound)* |
| Menu / pause | Start | Start |

In the menus: Cross confirms, Circle goes back, Start closes them.
Everything except Start is rebindable: Duke from Options -> Control Setup,
Shadow Warrior from Options -> Controller (Buttons, and Sticks for the
analog settings and L2/R2). **Lock on**, while held, turns the view to the
nearest enemy in front of you that can be seen and follows it; moving and
strafing stay free.

Sticks: Shadow Warrior's Options -> Controller -> Sticks sets each axis'
scale (1.00 by default), dead zone and saturation, and **Invert** flips it
(on the right stick's up/down page: inverted look). In Duke it is Options
-> Control Setup -> Controller Setup -> "Inverted aiming".

## Known limitations

- No multiplayer.
- DualShock 3 only: no USB keyboard or mouse.
- No in-game console (it needs a keyboard).

## Credits

- [EDuke32](https://www.eduke32.com/) by Richard "TerminX" Gobeille, Evan
  "Hendricks266" Ramos and contributors, including VoidSW -- the engine and
  games this project is built on.
- Ken Silverman, for the Build engine.
- Jonathon Fowler, for JFDuke3D and JFSW, the ports EDuke32 and VoidSW grew
  from.
- 3D Realms, for releasing the source code of Duke Nukem 3D and Shadow
  Warrior.
- Bernhard Schelling's TinySoundFont, the SoundFont synthesizer EDuke32
  uses (and this port runs on an SPU).

## License

GPLv2, inherited from EDuke32 (see
[`package/common/gpl-2.0.txt`](package/common/gpl-2.0.txt)); the Build
engine under its own license (see
[`package/common/buildlic.txt`](package/common/buildlic.txt)).

No game data files (`.GRP`, music, or otherwise) are included in this
repository. You need your own legally-owned copy of the game(s).

## AI disclosure

This project's code was written collaboratively with Claude (Anthropic),
working through this port with me in real time over many sessions.
Every bit of testing and debugging were made by me on real hardware.
