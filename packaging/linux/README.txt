Luigi's Mansion Recompiled 1.1.5 - Linux x86_64
=============================================

A native PC port of Luigi's Mansion (GameCube). No game data is included: the game is
built on your computer from your own disc image the first time you start it.


REQUIREMENTS
------------
- A 64-bit x86 PC (x86_64 / amd64) running Linux.
- glibc 2.34 or newer and the C++ runtime of GCC 12 or newer (libstdc++.so.6). Any
  current distribution has these: Ubuntu 22.04+, Linux Mint 21+, Debian 12+, Fedora 36+,
  Arch, openSUSE Tumbleweed, SteamOS 3.5+, Pop!_OS 22.04+.
  Check with:  ldd --version
- A graphics driver with Vulkan support (this is what the game uses):
    AMD / Intel: Mesa's Vulkan drivers (Ubuntu/Debian: mesa-vulkan-drivers;
                 Fedora: mesa-vulkan-drivers; Arch: vulkan-radeon / vulkan-intel)
    NVIDIA:      the proprietary driver (it includes Vulkan)
  Check with:  vulkaninfo --summary   (package vulkan-tools)
- X11 or Wayland desktop, and PulseAudio or PipeWire (the default on every desktop) for sound.
- About 2 GB of free disk space (the game module built at first start, shader caches).
- Your own Luigi's Mansion disc image (ISO, GCM, RVZ, WIA, GCZ, CISO, NFS, TGC):
    USA (GLME01), European (GLMP01) and/or Japanese (GLMJ01) release.
  Other games, other revisions and modified images are rejected.


HOW TO RUN
----------
1. Unpack the archive anywhere you like (for example your home folder):
       tar xzf LuigisMansionRecompiled-linux-x86_64.tar.gz
   (or unzip LuigisMansionRecompiled-linux-x86_64.zip)
2. Start the game from that folder:
       ./LuigisMansionRecompiled.sh
   or double-click LuigisMansionRecompiled.sh in your file manager ("Run" / "Execute").
3. The first start shows the setup screen: pick your disc image(s) with "Browse...",
   then "Install". The game code is translated and compiled for your PC (about 20 to 60
   seconds, depending on your CPU) and the shaders are prepared. Later starts go straight
   to the game.

If you unpacked the .zip and the game does not start ("Permission denied"), the
executable bit was lost while unzipping. Run this once inside the folder:
       chmod +x LuigisMansionRecompiled.sh LuigisMansionRecomp llvm/bin/clang llvm/bin/ld.lld
(the .tar.gz keeps the permissions, so prefer it).

Useful options (add them after ./LuigisMansionRecompiled.sh):
    --setup          open the setup screen again (add or change disc images)
    --fullscreen     start in full screen

In the game, press F1 for the options menu (graphics, sound, controls, enhancements);
Esc closes it. Controllers are detected automatically; bindings are in F1 > Controls.
Updates: the game tells you when a new release is out; F1 > Game > Updates downloads,
verifies and installs it into this folder when you click (needs libcurl, settings and
saves stay).


WHERE FILES GO
--------------
    ~/.config/Luigi's Mansion Recompiled/       settings.ini (F1 menu), install.ini (disc paths)
    ~/.local/share/Luigi's Mansion Recompiled/  game/ (the game module built from your disc),
                                                USA/Card A, EUR/Card A (memory card saves),
                                                hidden_mansion/, screenshots/, last_run.log
    ~/.cache/Luigi's Mansion Recompiled/        shader and pipeline caches (safe to delete)
($XDG_CONFIG_HOME, $XDG_DATA_HOME and $XDG_CACHE_HOME are used instead when they are set.)
Versions up to 1.0.1 used folders named "Luigi's Mansion PC"; the first start of this
version renames them, so your settings and saves carry over.

To start over completely, delete those three folders. Your disc image is only read,
never modified or copied.


PORTABLE MODE
-------------
To keep everything (settings, saves, the game module, shader caches, the launcher's logs)
inside the game folder instead of the three folders above, create an empty file named
portable.txt (or a folder named portable) next to LuigisMansionRecompiled.sh before you
start the game:
       touch portable.txt
Everything then goes into the folder "portable" there: move the whole game folder to
another drive or PC and it keeps working; delete it and nothing is left behind. Keep your
disc images inside the game folder too, so they move along (a disc that is no longer at
its old path is looked for there by name). Another PC only prepares the shaders again.
LM_PORTABLE=1 ./LuigisMansionRecompiled.sh does the same for one start.
An existing installation can be switched in F1 > Game > Data folder: "Make this
installation portable..." moves your data into the portable folder and restarts, "Move
data back to the system folder..." does the reverse. The data is copied and checked
before anything is removed, so a failed move changes nothing.
Portable mode needs a game folder you can write to; otherwise it stays off (the setup
screen and F1 > Game say so). Updates keep a portable installation portable.


STEAM DECK
----------
In game mode (Gamescope) VSync is always on, so the frame rate is the screen's refresh rate
or an even fraction of it. A rate that does not divide it evenly is shown at the fraction
below it: 60 fps on the 90 Hz Steam Deck OLED runs at 45 fps (the game says so once).
For 60 fps set the refresh rate to 60 Hz in the Quick Access menu (Performance > Refresh
rate), or pick 90 fps / "Match display refresh rate" in F1 > Visuals. The LCD Steam Deck
refreshes at 60 Hz, so 30 and 60 fps work as they are.


REPORTING PROBLEMS
------------------
Every start through LuigisMansionRecompiled.sh writes a log:
    ~/.local/share/Luigi's Mansion Recompiled/last_run.log      (the previous one: previous_run.log)
    (portable mode: portable/last_run.log in the game folder)
Please send that file, a description of what happened, your distribution, your graphics
card and driver (the log's "Graphics adapter information" lines show them).
If the game crashes, the log ends with "=== signal ..." and a list of addresses; that part
is the most useful.

Troubleshooting:
- "No Vulkan adapter" / black window: install the Vulkan driver (see REQUIREMENTS). As a
  fallback you can try OpenGL ES:  LM_BACKEND=gles ./LuigisMansionRecompiled.sh
- No sound: check that PulseAudio or PipeWire (pipewire-pulse) is running.
- "Browse..." does nothing / says the file dialog could not be opened: your desktop has no
  file-chooser service (xdg-desktop-portal, or the zenity program). Type or paste the full
  path of the disc image into the box next to "Browse..." and press "Use", or start once with
       ./LuigisMansionRecompiled.sh --us-disc /path/to/your/game.iso     (European disc: --pal-disc)
- Force X11 instead of Wayland:  SDL_VIDEO_DRIVER=x11 ./LuigisMansionRecompiled.sh
- "The bundled compiler ... is missing": the llvm/ folder next to the game is incomplete;
  unpack the archive again.


WHAT IS IN THIS FOLDER
----------------------
    LuigisMansionRecompiled.sh  launcher (writes the log, passes options on)
    LuigisMansionRecomp      the game program (runtime, recompiler, setup)
    pipelines.bin            shader precache list
    llvm/bin/clang, ld.lld   the compiler used once to build the game module from your disc
                             (LLVM, Apache-2.0 WITH LLVM-exception, llvm/LLVM-LICENSE.TXT)
    llvm/lib/libxml2.so.2    small stand-in library the bundled linker needs to start
    NOTICE.md                third-party notices
