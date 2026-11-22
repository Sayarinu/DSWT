#pragma once

// Where the virtual persistence page sits for a cartridge, by its header game
// code. Include after wonder_trade/persistence.h (src/DSWTPersistence.h in the
// emulator), which defines each family's address. Generation IV cartridges keep
// one address in every region; a Generation V family has its own, and only its
// verified cartridges (protocol/save-layout*.json) get a page. Any other game
// gets none: an address in its save could cover data it reads.

#include <cstdint>
#include <cstring>

#if !defined(WT_PERSISTENCE_ADDRESS_GEN4) || !defined(WT_PERSISTENCE_ADDRESS_BW) || !defined(WT_PERSISTENCE_ADDRESS_B2W2)
#error "Include the persistence header first"
#endif

namespace dswt {

constexpr std::uint32_t kNoPersistencePage = 0;

inline std::uint32_t PersistencePageAddress(const char code[4])
{
    // Diamond, Pearl, Platinum, HeartGold and SoulSilver.
    static const char gen4[][4] = {"ADA", "APA", "CPU", "IPK", "IPG"};
    for (const auto &game : gen4)
        if (std::memcmp(code, game, 3) == 0) return WT_PERSISTENCE_ADDRESS_GEN4;
    // Black and White (USA, Europe).
    if (std::memcmp(code, "IRBO", 4) == 0 || std::memcmp(code, "IRAO", 4) == 0) return WT_PERSISTENCE_ADDRESS_BW;
    // Black 2 and White 2 (USA, Europe).
    if (std::memcmp(code, "IREO", 4) == 0 || std::memcmp(code, "IRDO", 4) == 0) return WT_PERSISTENCE_ADDRESS_B2W2;
    return kNoPersistencePage;
}

} // namespace dswt
