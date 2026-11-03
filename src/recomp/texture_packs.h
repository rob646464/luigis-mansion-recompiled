#pragma once
// Texture packs (F1 > Visuals > Texture packs; docs/TEXTURE_PACKS.md): Dolphin-format replacement
// textures (tex1_<w>x<h>[_m]_<hash>[_<tlut>]_<fmt>.dds/.png) from <data folder>/texture_packs/<pack>/,
// loaded through Aurora's texture replacement (lib/gfx/texture_replacement.cpp), and the texture
// dumper for pack makers (Aurora 0360) into <data folder>/texture_dump/<game id>/.

namespace recomp::texpacks {

void init(const char* gameId);  // after the settings are loaded: scans the packs and loads them (background)
void shutdown();                // before aurora_shutdown: stops the loader thread, dumper off
void hostFrame();               // host thread, every frame: test aids (LM_TEXPACK_AT), periodic log
bool drawMenu();                // the Texture packs sub-tab; true when a saved setting changed
void memReport();               // one "[mem] texture packs: ..." line (mem_report.cpp)

}  // namespace recomp::texpacks
