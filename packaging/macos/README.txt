Luigi's Mansion Recompiled - macOS (Apple silicon)
==================================================

A native port of Luigi's Mansion (GameCube). No game data is included: the game is
built on your Mac from your own disc image the first time you start it.


REQUIREMENTS
------------
- A Mac with Apple silicon (M1 or newer) running macOS 13 Ventura or newer.
- About 2 GB of free disk space (the game module built at first start, shader caches).
- Your own Luigi's Mansion disc image (ISO, GCM, RVZ, WIA, GCZ, CISO, NFS, TGC):
    USA (GLME01), European (GLMP01) and/or Japanese (GLMJ01) release.


HOW TO RUN
----------
1. Unpack the archive anywhere you like (for example your Applications or Games folder).
2. The program is not notarized by Apple. Before the first start, open Terminal and run
   (drag the folder onto the Terminal window to type its path):
       xattr -dr com.apple.quarantine "<the LuigisMansionRecompiled folder>"
   Or: right-click LuigisMansionRecomp, choose Open, then Open again in the warning.
3. Start LuigisMansionRecomp (double-click, or ./LuigisMansionRecomp in Terminal).
4. The first start shows the setup screen: pick your disc image(s) with "Browse...",
   then "Install". The game code is translated and compiled for your Mac with the bundled
   compiler (llvm/bin), and the shaders are prepared. Later starts go straight to the game.

Press F1 in the game for every option (graphics, controls, DSU controllers, enhancements).


ABOUT THIS BUILD
----------------
The macOS version is built and packaged automatically (GitHub Actions) from the same source as
the Windows and Linux releases. It is not signed or notarized by Apple, which is why step 2 is
needed, and the developers do not test it on a Mac themselves: please report problems on the
project's GitHub issues page. Updates: F1 > Game > Updates, as on the other systems.


WHERE FILES GO
--------------
Settings:           ~/Library/Application Support/Luigi's Mansion Recompiled
Game module, saves: ~/Library/Application Support/Luigi's Mansion Recompiled
Shader caches:      ~/Library/Caches/Luigi's Mansion Recompiled
A "portable" folder (or portable.txt) next to the program keeps everything in there instead.
