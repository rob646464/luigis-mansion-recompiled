#pragma once
// Read-only access to a user's disc image (ISO, RVZ, GCZ, ... via nod) during setup,
// independent of the disc the game later runs from (Aurora's DVD layer).
#include "port/install.h"

#include <cstdint>
#include <string>
#include <vector>

struct NodHandle;

namespace setup {

class Disc {
public:
    Disc() = default;
    ~Disc();
    Disc(const Disc&) = delete;
    Disc& operator=(const Disc&) = delete;

    // Opens the image and identifies its main.dol. Returns false with a user-facing message.
    bool open(const std::string& utf8Path, std::string& error);
    port::GameVersion version() const { return version_; }
    const std::vector<std::uint8_t>& dol() const { return dol_; }
    std::string dolSha1() const;  // hex
    // Reads a whole file by disc path ("/Map/map2.szp"); false if missing.
    bool readFile(const char* path, std::vector<std::uint8_t>& out) const;
    // Size and CRC-32 (zlib's) of a file, read in pieces; false if missing or unreadable
    // (`found` tells which). For the disc image check (disc_check.h).
    bool fileCrc(const char* path, std::uint32_t& size, std::uint32_t& crc, bool& found) const;

private:
    NodHandle* disc_ = nullptr;
    NodHandle* part_ = nullptr;
    std::vector<std::uint8_t> dol_;
    port::GameVersion version_ = port::GameVersion::USA;
};

// Opens and identifies an image without keeping it open; for validating a picked file.
bool probeDisc(const std::string& utf8Path, port::GameVersion& version, std::string& error);

std::string hex(const std::uint8_t* data, size_t n);

}  // namespace setup
