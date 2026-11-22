#include "../../client/tests/journal_fixture.h"
#include "wonder_trade/game_family.h"
#include <openssl/evp.h>
#include <cstring>
#include <iostream>
#include <string>

static int Hash(const unsigned char *bytes, std::size_t size, unsigned char digest[32]) {
    std::size_t length = 0;
    return EVP_Q_digest(nullptr, "SHA256", nullptr, bytes, size, digest, &length) == 1 && length == 32 ? 0 : -1;
}

namespace {
unsigned char banks[2][4096];

int ReadBank(void *, int bank, unsigned char record[4096]) {
    std::memcpy(record, banks[bank], 4096);
    return 0;
}

int WriteBank(void *, int bank, const unsigned char record[4096]) {
    std::memcpy(banks[bank], record, 4096);
    return 0;
}

// classify: two banks on stdin. "journal" when they hold a record, "unused"
// when they never held one and initialization (erasing what a cut first write
// left) then creates one, else "blocked".
int Classify() {
    WtJournal journal{};
    WtJournalIO io{nullptr, ReadBank, WriteBank, Hash};
    unsigned char seed[32];
    std::cin.read(reinterpret_cast<char *>(banks), sizeof(banks));
    if (std::cin.gcount() != static_cast<std::streamsize>(sizeof(banks))) return 2;
    std::memset(seed, 0x33, sizeof(seed));
    const char *state = "blocked";
    if (WtJournal_Open(&journal, io) == 0) state = "journal";
    else if (WtJournal_Unused(banks[0], banks[1], Hash) && WtJournal_Initialize(&journal, io, nullptr) == WT_JOURNAL_NEEDS_SEED
             && WtJournal_Initialize(&journal, io, seed) == 0 && WtJournal_Open(&journal, io) == 0) state = "unused";
    WtJournal_Close(&journal);
    std::cout << state << "\n";
    return std::cout ? 0 : 2;
}
}

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "classify") return Classify();
    if (argc != 2) return 2;
    WtJournalEntry entry{};
    unsigned char record[4096];
    if (WtTest_JournalEntry(static_cast<unsigned>(std::stoul(argv[1])), Hash, &entry) != 0) return 2;
    // The fixture's party Pokémon is a Generation IV one; a family with a
    // smaller party Pokémon keeps the leading bytes of the same image.
    if (entry.source.kind == 1 && entry.source.length != WT_GAME_PARTY_SIZE) {
        entry.source.length = WT_GAME_PARTY_SIZE;
        std::memset(entry.original + WT_GAME_PARTY_SIZE, 0, sizeof(entry.original) - WT_GAME_PARTY_SIZE);
        if (Hash(entry.original, WT_GAME_PARTY_SIZE, entry.originalDigest) != 0) return 2;
    }
    if (WtJournal_Encode(&entry, Hash, record) != 0) return 2;
    std::cout.write(reinterpret_cast<const char *>(record), sizeof(record));
    return std::cout ? 0 : 2;
}
