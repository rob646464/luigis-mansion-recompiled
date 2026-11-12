Luigi's Mansion Recompiled 1.1.5 - Windows x64
============================================

Discord: news, help and bug reports for all my projects: https://discord.gg/cM4chZtgh7

A native PC port of Luigi's Mansion (GameCube). No game data is included: the game is
built on your computer from your own disc image the first time you start it.


REQUIREMENTS
------------
- 64-bit Windows 10 or 11.
- A graphics card with Direct3D 12 support.
- About 2 GB of free disk space (the game module built at first start, shader caches).
- Your own Luigi's Mansion disc image (ISO, GCM, RVZ, WIA, GCZ, CISO, NFS, TGC):
    USA (GLME01), European (GLMP01) and/or Japanese (GLMJ01) release.
  Other games, other revisions and modified images are rejected.


FIRST RUN
---------
1. Unpack the archive anywhere you like.
2. Run LuigisMansionRecomp.exe.
3. Pick your USA and/or European disc image with "Browse...", then "Install". The game
   code is translated and compiled for your PC with the bundled compiler (llvm\ folder),
   which takes about 20 to 60 seconds, then the shaders are prepared once ("Preparing
   shaders"). Later starts go straight to the game.
With both discs you choose which release to play; the European disc also unlocks the
European Hidden Mansion. F1 > Game > "Add or change discs..." reopens setup later.
Updates: the game tells you when a new release is out; F1 > Game > Updates downloads,
verifies and installs it into this folder when you click (settings and saves stay).


KEYS
----
F1    options menu (Esc closes): Visuals, Sound, Controls, Enhancements (General /
      Gameplay / Unused content / Debug), Game
F3    info overlay (map / room / position)
F4    model viewer

Default keyboard: WASD move, arrows C-stick, E/Q/R/F = A/B/X/Y, Left Ctrl = Z,
Enter = START, Left Shift = L, Space = R, 1-4 = D-pad. Gamepads (including GameCube
controller adapters) are detected automatically; remap everything in F1 > Controls.


HIGHLIGHTS
----------
- Any resolution, widescreen and ultrawide (Hor+, HUD anchored to the screen edges).
- High frame rates: the game logic stays at its original 30 updates per second and every
  frame in between is interpolated (F1 > Visuals: frame-rate slider up to 320, "Match
  display refresh rate", "Original" for the console's 30). VRR / G-SYNC / FreeSync aware.
- Accurate GameCube rendering (EFB copies, depth textures, fog, indirect effects), with
  optional Enhanced lighting (per-pixel and colored lights, soft shadows, realistic
  flashlight, bloom) in F1 > Visuals.
- The European "Hidden Mansion" with its boss changes (needs the European disc; with
  both discs E. Gadd's lab offers the USA and the European Hidden Mansion).
- Restored unused and beta content in F1 > Enhancements > Unused content (all off by
  default): Game Boy Horror camera, beta ghost shading, early Bogmire, poison and other
  Luigi states, jukebox, gallery, 2-player mode and more.


WHERE FILES GO
--------------
    %APPDATA%\Luigi's Mansion Recompiled\            settings.ini (F1 menu), install.ini (discs)
    %APPDATA%\Luigi's Mansion Recompiled\game\       the game module built from your disc
    %APPDATA%\Luigi's Mansion Recompiled\USA\Card A  memory card saves (Dolphin-compatible .gci;
                                                     European: EUR\Card A)
Versions up to 1.0.1 used %APPDATA%\Luigi's Mansion PC; the first start of this version
renames it, so your settings and saves carry over.


PORTABLE MODE
-------------
To keep everything (settings, saves, the game module, shader caches) inside the game
folder instead of %APPDATA%, create an empty file named portable.txt (or a folder named
portable) next to LuigisMansionRecomp.exe before you start the game. Everything then goes
into the folder "portable" there: move the whole game folder to another drive or PC and
it keeps working; delete it and nothing is left behind. Keep your disc images inside the
game folder too, so they move along (a disc that is no longer at its old path is looked
for there by name). Another PC only prepares the shaders again.
An existing installation can be switched in F1 > Game > Data folder: "Make this
installation portable..." moves your data into the portable folder and restarts, "Move
data back to the system folder..." does the reverse. The data is copied and checked
before anything is removed, so a failed move changes nothing.
Portable mode needs a game folder you can write to: inside Program Files it stays off
(the setup screen and F1 > Game say so) and the data stays in %APPDATA%.
Updates keep a portable installation portable.


KNOWN LIMITS
------------
- 2-player mode (Unused content) is unfinished: Area 1, Chauncey and Boolossus work with
  two players; the rest of the game is not verified yet.
- A few very fast motions (Chauncey's quickest spin turns, the first frame of water spray
  drops) appear at the game's own 30 updates per second when interpolating.


WHAT IS IN THIS FOLDER
----------------------
    LuigisMansionRecomp.exe   the game program (runtime, recompiler, setup)
    *.dll                     runtime libraries (SDL3, Dawn/WebGPU, DirectX shader
                              compiler, nod, libpng, zlib)
    pipelines.bin             shader precache list
    llvm\bin\                 clang and lld-link, used once to build the game module from
                              your disc (LLVM, Apache-2.0 WITH LLVM-exception,
                              llvm\LLVM-LICENSE.TXT)
    NOTICE.md                 third-party notices
