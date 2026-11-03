#pragma once
// First-run setup of LuigisMansionRecomp.exe (docs/SETUP.md): asks for the user's disc
// images, builds the game module(s) and the PAL Hidden Mansion overlay into
// %APPDATA%\Luigi's Mansion Recompiled, and says what to launch.
#include "port/install.h"

#include <filesystem>
#include <string>

namespace setup {

struct LaunchInfo {
    port::GameVersion version = port::GameVersion::USA;
    std::string disc;              // UTF-8 path of the disc image to run
    std::filesystem::path module;  // game module DLL for that disc
};

struct Options {
    bool forceUi = false;   // --setup: show the setup screen even if everything is ready
    bool autoStart = false; // --setup-auto: start installing without waiting for a click (testing)
    std::string usDisc, palDisc, jpnDisc;  // --us-disc / --pal-disc / --jpn-disc: prefill the disc pickers
};

// Requires an initialised Aurora window (the UI is Dear ImGui). Returns false if the user
// quit. Fills port::install() (discs, readiness flags) for the F1 menu.
bool run(const Options& options, LaunchInfo& launch);

}  // namespace setup
