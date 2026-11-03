// Inputs of first-run setup, embedded in the executable so they always match the runtime
// they were built with: the symbol maps (decomp CC0 metadata; the Japanese one generated with
// decomp-toolkit), the hook list, the PAL/Japanese version maps, and the freestanding sources
// compiled into the game module. The arrays are generated at build time by
// tools/recomp/lmembed.cpp (CMakeLists.txt, LM_EMBEDDED_FILES).
#include "setup/embedded.h"

namespace setup::embedded {
namespace data {
#define LM_EMBEDDED(name)       \
    extern const char name[];   \
    extern const unsigned long long name##Size;
LM_EMBEDDED(kSymbolsUs)
LM_EMBEDDED(kSplitsUs)
LM_EMBEDDED(kSymbolsPal)
LM_EMBEDDED(kVersionMapPal)
LM_EMBEDDED(kSymbolsJp)
LM_EMBEDDED(kVersionMapJp)
LM_EMBEDDED(kHooks)
LM_EMBEDDED(kCtxHeader)
LM_EMBEDDED(kModuleAbiHeader)
LM_EMBEDDED(kModuleRt)
LM_EMBEDDED(kRecompilerSource)
#undef LM_EMBEDDED
}  // namespace data

namespace {
std::string_view view(const char* p, unsigned long long n) { return {p, size_t(n)}; }
}  // namespace

using namespace data;
using port::GameVersion;

std::string_view symbols(GameVersion v) {
    switch (v) {
    case GameVersion::PAL: return view(kSymbolsPal, kSymbolsPalSize);
    case GameVersion::JPN: return view(kSymbolsJp, kSymbolsJpSize);
    default: return view(kSymbolsUs, kSymbolsUsSize);
    }
}
std::string_view splits(GameVersion v) { return v == GameVersion::USA ? view(kSplitsUs, kSplitsUsSize) : std::string_view{}; }
std::string_view versionMap(GameVersion v) {
    switch (v) {
    case GameVersion::PAL: return view(kVersionMapPal, kVersionMapPalSize);
    case GameVersion::JPN: return view(kVersionMapJp, kVersionMapJpSize);
    default: return {};
    }
}
std::string_view hooks() { return view(kHooks, kHooksSize); }
std::string_view ctxHeader() { return view(kCtxHeader, kCtxHeaderSize); }
std::string_view moduleAbiHeader() { return view(kModuleAbiHeader, kModuleAbiHeaderSize); }
std::string_view moduleRt() { return view(kModuleRt, kModuleRtSize); }
std::string_view recompilerSource() { return view(kRecompilerSource, kRecompilerSourceSize); }

}  // namespace setup::embedded
