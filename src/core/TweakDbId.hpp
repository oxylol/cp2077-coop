#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace coop
{
// The game's TweakDBID as a 64-bit value: CRC-32 of the record name in the low 32 bits, name length in the
// next 8 (the remaining bits are zero for static records). Lets tools and tests name records like the game
// does, e.g. TweakDbId("Vehicle.v_standard2_archer_hella_player"). [VERIFY] against TDBID.Create in-game (S1).
constexpr uint32_t Crc32(std::string_view aText)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (const char ch : aText)
    {
        crc ^= static_cast<uint8_t>(ch);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

constexpr uint64_t TweakDbId(std::string_view aName)
{
    return static_cast<uint64_t>(Crc32(aName)) | (static_cast<uint64_t>(aName.size() & 0xFF) << 32);
}

static_assert(Crc32("123456789") == 0xCBF43926u, "CRC-32 check value");
} // namespace coop
