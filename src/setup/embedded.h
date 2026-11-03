#pragma once
#include "port/install.h"

#include <string_view>

namespace setup::embedded {

// decomp/config/<GLME01|GLMP01_00|GLMJ01>/symbols.txt (the Japanese table: tools/recomp/gen_jp_symbols.py)
std::string_view symbols(port::GameVersion v);
std::string_view splits(port::GameVersion v);      // USA only (the other configs have no splits)
std::string_view versionMap(port::GameVersion v);  // tools/recomp/GLMP01_map.txt / GLMJ01_map.txt (not USA)
std::string_view hooks();               // tools/recomp/hooks.txt
std::string_view ctxHeader();           // src/recomp/ctx.h
std::string_view moduleAbiHeader();     // src/recomp/module_abi.h
std::string_view moduleRt();            // src/recomp/module_rt.cpp
std::string_view recompilerSource();    // src/recompiler/recompiler.cpp (part of the cache key)

}  // namespace setup::embedded
