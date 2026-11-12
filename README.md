# Luigi's Mansion Recompiled

**Discord:** news, help and bug reports for all my projects: https://discord.gg/cM4chZtgh7

**No game data is included.** On first start the game asks for your own disc image (ISO, RVZ and or an extracted game folder) of the USA release (GLME01) and/or the European release (GLMP01). It is recommended to use both the USA and PAL versions of the game to experience every feature available in this port. Once the games are selected, it then builds the game for your PC with the bundled compiler, which takes about 20 to 60 seconds on a decent PC.


## Features
- **Any resolution, widescreen and ultrawide:** Hor+, with HUD elements anchored to the screen edges and widescreen fixes for full-screen scenes.
- **High frame rates:** game logic stays at the original 30 updates per second, and the frames in between are interpolated from source-tagged objects. Frame rate goes up to 320 fps, with "Match display refresh rate" and VRR/G-SYNC aware pacing. "Original" keeps the console's 30 fps.
- **Accurate GameCube rendering:** Better than Dolphin. EFB copies including copy-to-RAM, depth textures, real-time shadows, indirect effects, 6-bit blending and copy filters. A shader precache prepares everything before the game starts, so there are no compile stutters.
- **Optional Enhanced lighting:** per-pixel and colored lights, soft shadows, a realistic flashlight beam, bloom and ghosts cast a glowing light.
- **The European Hidden Mansion,** including its boss changes. With both discs, E. Gadd's lab offers the USA and the European Hidden Mansion.
- **Restored unused content,** all off by default: Toggles to re-enable various cut content, a model viewer, warps to unused rooms and more.
- **Options for EVERYTHING:** an F1 options menu for everything, difficulty settings, fun tweaks and toggles, rendering options, etc

## Known issues
- The 2-player mode (Unused content) is unfinished. The plan is to finish it soon, but for now it's mostly in the state it was left in in the original code.
