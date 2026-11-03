#pragma once
#include "setup/disc.h"

#include <filesystem>
#include <string>

namespace setup {

// Writes the PAL Hidden Mansion overlay for the US base into `out` (replacing it), and the
// USA Hidden Mansion overlay for the PAL base into `out`/usa.
bool buildHiddenMansionOverlay(const Disc& us, const Disc& pal, const std::filesystem::path& out, std::string& error);

// Per-version mansions (docs/VERSION_DIFFERENCES.md "Lab menu design"), for two or more
// installed discs: `out`/<ID>/... each disc's rules files (map2/map4/rooms/Bowser model) and
// `out`/lab/<ID>/Event/event08.szp, E. Gadd's lab for that base asking which release's
// "The Mansion" / "The Hidden Mansion" (flags 60/76, flag 8 for the PAL Hidden Mansion).
// `discs`: the installed ones (any order). Replaces `out`.
bool buildVersionsOverlay(const std::vector<const Disc*>& discs, const std::filesystem::path& out, std::string& error);

}  // namespace setup
