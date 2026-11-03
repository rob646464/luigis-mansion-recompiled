#include "port/dol.h"

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <array>
#include <bit>
#include <cstring>

namespace port::dol {
namespace {

struct Section {
    uint32_t fileOffset;
    uint32_t addr;
    uint32_t size;
};

const uint8_t* sDol;
size_t sDolSize;
std::array<Section, 18> sSections;  // 7 text + 11 data
Version sVersion = Version::Unknown;

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

// Minimal SHA-1 (FIPS 180-1) used to identify the retail DOL and to key setup's caches.
std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint64_t bitLen = uint64_t(len) * 8;
    const size_t padded = ((len + 8) / 64 + 1) * 64;
    auto byteAt = [&](size_t i) -> uint8_t {
        if (i < len) return data[i];
        if (i == len) return 0x80;
        if (i >= padded - 8) return uint8_t(bitLen >> (8 * (padded - 1 - i)));
        return 0;
    };
    for (size_t chunk = 0; chunk < padded; chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(byteAt(chunk + i * 4)) << 24) | (uint32_t(byteAt(chunk + i * 4 + 1)) << 16) |
                   (uint32_t(byteAt(chunk + i * 4 + 2)) << 8) | byteAt(chunk + i * 4 + 3);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = std::rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = std::rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = uint8_t(h[i] >> 24);
        out[i * 4 + 1] = uint8_t(h[i] >> 16);
        out[i * 4 + 2] = uint8_t(h[i] >> 8);
        out[i * 4 + 3] = uint8_t(h[i]);
    }
    return out;
}

namespace {

// SHA-1 of each supported retail main.dol (from decomp/config/*/config.yml).
constexpr uint8_t kShaGLME01[20] = {0x77, 0xd4, 0x92, 0x25, 0xe7, 0xd2, 0x8b, 0x97, 0x77, 0x91,
                                    0x6c, 0xdb, 0xe5, 0x80, 0x93, 0x45, 0xc5, 0x70, 0xec, 0xbc};
constexpr uint8_t kShaGLMP01[20] = {0xa1, 0xe2, 0xbd, 0x82, 0x81, 0x37, 0x87, 0xcf, 0xe0, 0xb1,
                                    0x1a, 0xfb, 0x5d, 0x13, 0x8f, 0x08, 0xfd, 0xbd, 0x94, 0xf1};
constexpr uint8_t kShaGLMJ01[20] = {0x72, 0x20, 0x05, 0xea, 0x9c, 0x1e, 0xab, 0x54, 0xb1, 0x14,
                                    0xf8, 0x14, 0x73, 0x4d, 0x8f, 0x32, 0x7e, 0x56, 0x14, 0xee};  // (no decomp config: sys/main.dol of the user's dump)

}  // namespace

Version identify(const uint8_t* dol, size_t size) {
    if (!dol || size < 0x100) return Version::Unknown;
    const auto digest = sha1(dol, size);
    if (std::memcmp(digest.data(), kShaGLME01, 20) == 0) return Version::GLME01;
    if (std::memcmp(digest.data(), kShaGLMP01, 20) == 0) return Version::GLMP01;
    if (std::memcmp(digest.data(), kShaGLMJ01, 20) == 0) return Version::GLMJ01;
    return Version::Unknown;
}

Version load() {
    s32 size = 0;
    sDol = DVDGetDOLLocation(&size);
    sDolSize = size > 0 ? size_t(size) : 0;
    sVersion = Version::Unknown;
    if (!sDol || sDolSize < 0x100) {
        OSReport("[dol] no DOL found in disc image\n");
        return sVersion;
    }
    for (size_t i = 0; i < sSections.size(); ++i) {
        sSections[i] = {read_be32(sDol + i * 4), read_be32(sDol + 0x48 + i * 4), read_be32(sDol + 0x90 + i * 4)};
    }
    sVersion = identify(sDol, sDolSize);
    if (sVersion == Version::Unknown)
        OSReport("[dol] main.dol does not match a supported retail build (GLME01/GLMP01/GLMJ01 rev 0)\n");
    return sVersion;
}

Version version() { return sVersion; }

std::span<const uint8_t> bytes(uint32_t addr, size_t size) {
    for (const Section& s : sSections) {
        if (s.size != 0 && addr >= s.addr && addr - s.addr + size <= s.size && s.fileOffset + s.size <= sDolSize) {
            return {sDol + s.fileOffset + (addr - s.addr), size};
        }
    }
    return {};
}

uint8_t be8(uint32_t addr) {
    auto b = bytes(addr, 1);
    return b.empty() ? 0 : b[0];
}

uint16_t be16(uint32_t addr) {
    auto b = bytes(addr, 2);
    return b.empty() ? 0 : uint16_t((b[0] << 8) | b[1]);
}

uint32_t be32(uint32_t addr) {
    auto b = bytes(addr, 4);
    return b.empty() ? 0 : read_be32(b.data());
}

float bef32(uint32_t addr) { return std::bit_cast<float>(be32(addr)); }

}  // namespace port::dol
