#pragma once
// Static recompiler: GameCube main.dol (PowerPC 750CL/Gekko) -> C++ (docs/RECOMPILER.md).
//
// Used both by the build (tools/recomp/lmrecompile.cpp, invoked by CMake for the dev
// target lmrecomp) and by LuigisMansionRecomp.exe at first-run setup (docs/SETUP.md).
// It runs on the user's own executable; the output is never committed.
//
// Function boundaries and jump tables come from the decomp's symbol map
// (decomp/config/<version>/symbols.txt, CC0). Functions in SDK translation units are
// emitted as calls into the HLE layer (src/recomp/hle_*.cpp) instead of being
// translated.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lmrc {

// Generated sources are split into this many files (balanced by instruction count) so
// the build can list them without running the generator first.
constexpr int kChunks = 32;

struct Input {
    std::vector<std::uint8_t> dol;  // the retail main.dol
    std::string version;            // "GLME01", "GLMP01" or "GLMJ01"
    std::string symbols;            // symbols.txt contents
    std::string splits;             // splits.txt contents (SDK translation-unit ranges)
    std::string hooks;              // tools/recomp/hooks.txt contents
    // Optional (non-US bases): "ADDR NAME TU" per line, from tools/recomp/gen_version_map.py.
    // Gives SDK functions their translation unit (the PAL splits are not mapped) and
    // hooked/HLE functions their canonical (US) names.
    std::string versionMap;
};

struct Output {
    std::vector<std::pair<std::string, std::string>> files;  // name -> contents
    std::string summary;
    unsigned functions = 0;
};

// Returns false (with `error`) if the inputs are unusable.
bool recompile(const Input& in, Output& out, std::string& error);

// Names of the generated files, in order (funcs.h, funcs_NNN.cpp..., func_table.cpp).
std::vector<std::string> outputFileNames();

}  // namespace lmrc
