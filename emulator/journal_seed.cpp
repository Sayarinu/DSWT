#include "journal_seed.h"
#include "durable_save.h"
#include "wonder_trade/journal.h"
#include "journal_bw.h"
#include "journal_b2w2.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dswt {
namespace {
constexpr std::size_t kSize = 524288;
using Bytes = std::vector<std::uint8_t>;

void Require(bool condition) { if (!condition) throw std::runtime_error("unsupported journal; preserve recovery evidence"); }

// The journal core of the target's game family (journal_bw.h, journal_b2w2.h).
struct JournalApi {
    int (*open)(WtJournal *, WtJournalIO);
    int (*read)(const WtJournal *, WtJournalEntry *);
    int (*commit)(WtJournal *, const WtJournalEntry *);
    void (*close)(WtJournal *);
};
constexpr JournalApi kGen4Journal{WtJournal_Open, WtJournal_Read, WtJournal_Commit, WtJournal_Close};
constexpr JournalApi kBWJournal{WtJournalBW_Open, WtJournalBW_Read, WtJournalBW_Commit, WtJournalBW_Close};
constexpr JournalApi kB2W2Journal{WtJournalB2W2_Open, WtJournalB2W2_Read, WtJournalB2W2_Commit, WtJournalB2W2_Close};

// Where a family keeps its two journal banks; every byte of a bank after its
// 4 KiB record must stay blank (protocol/save-layout*.json).
struct Banks {
    std::array<std::size_t, 2> offsets;
    std::size_t size;
};
constexpr Banks kGen4Banks{{0x30000, 0x70000}, 65536};
constexpr Banks kBWBanks{{0x74000, 0x75000}, 4096};
constexpr Banks kB2W2Banks{{0x6B000, 0x6C000}, 4096};

bool IsBW(const std::string &target) { return target == "black-us" || target == "white-us"; }
bool IsB2W2(const std::string &target) { return target == "black2-us" || target == "white2-us"; }
const Banks &BanksFor(const std::string &target) {
    return IsBW(target) ? kBWBanks : IsB2W2(target) ? kB2W2Banks : kGen4Banks;
}

Bytes ReadOwned(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    Require(fd >= 0);
    try {
        struct stat info{};
        Require(::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
                info.st_nlink == 1 && info.st_size == static_cast<off_t>(kSize));
        Bytes bytes(kSize);
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            Require(count > 0);
            offset += static_cast<std::size_t>(count);
        }
        const auto closed = ::close(fd);
        fd = -1;
        Require(closed == 0);
        return bytes;
    } catch (...) { if (fd >= 0) ::close(fd); throw; }
}

std::uint64_t Number(const std::uint8_t *bytes, unsigned size) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return value;
}

std::uint16_t Crc(const std::uint8_t *bytes, std::size_t size) {
    unsigned crc = 65535;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<unsigned>(bytes[i]) << 8;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
        crc &= 65535;
    }
    return static_cast<std::uint16_t>(crc);
}

void CheckTail(const Bytes &bytes, const Banks &banks, unsigned bank) {
    const auto begin = bytes.begin() + banks.offsets.at(bank) + 4096;
    const auto end = bytes.begin() + banks.offsets.at(bank) + banks.size;
    Require(std::all_of(begin, end, [&](auto byte) { return (byte == 0 || byte == 255) && byte == *begin; }));
}

// Retail block layouts from protocol/save-layout*.json: each game family's two
// blocks and the footer fields that close them.
struct Layout {
    std::array<std::size_t, 2> offsets, sizes;
    std::size_t footer, size_at, magic_at, block_at, block_width, crc_at;
};
constexpr Layout kPlatinum{{0, 53036}, {53036, 74212}, 20, 8, 12, 16, 1, 18};
constexpr Layout kDiamondPearl{{0, 49408}, {49408, 74208}, 20, 8, 12, 16, 1, 18};
constexpr Layout kHeartGoldSoulSilver{{0, 63232}, {63016, 74512}, 16, 4, 8, 12, 2, 14};

const Layout &LayoutFor(const std::string &target) {
    if (target == "us-rev0" || target == "us-rev1" || target == "europe" || target == "france" || target == "germany" ||
        target == "italy" || target == "spain" || target == "japan" || target == "platinum-korea") return kPlatinum;
    if (target == "diamond-us-rev5" || target == "pearl-us-rev5" || target == "diamond-europe-rev5" ||
        target == "pearl-europe-rev5" || target == "diamond-france" || target == "diamond-germany" ||
        target == "diamond-italy" || target == "diamond-spain" || target == "pearl-france" ||
        target == "pearl-germany" || target == "pearl-italy" || target == "pearl-spain" || target == "diamond-korea" ||
        target == "pearl-korea" || target == "diamond-japan" || target == "pearl-japan") return kDiamondPearl;
    Require(target == "heartgold-us-rev0" || target == "soulsilver-us-rev0" || target == "heartgold-europe" ||
            target == "soulsilver-europe" || target == "heartgold-france" || target == "heartgold-germany" ||
            target == "heartgold-italy" || target == "heartgold-spain" ||
            target == "soulsilver-france" || target == "soulsilver-germany" ||
            target == "soulsilver-italy" || target == "soulsilver-spain" || target == "heartgold-korea" ||
            target == "soulsilver-korea" || target == "heartgold-japan" || target == "soulsilver-japan");
    return kHeartGoldSoulSilver;
}

// Korean cartridges mark save blocks with their own magic (protocol/save-layout*.json).
std::uint32_t FooterMagic(const std::string &target) {
    const std::string korean = "-korea";
    const bool isKorean = target.size() > korean.size() &&
        target.compare(target.size() - korean.size(), korean.size(), korean) == 0;
    return isKorean ? 0x20070903 : 0x20060623;
}

// Generation V main saves (protocol/save-layout-bw.json and aletheia's
// _MainSaveDesc, protocol/save-layout-b2w2.json and pokebw2's
// src/system/save_table.c): two copies of the main save. Each ends with a CRC
// of every one of its blocks (padded to 4 bytes) and a 16-byte footer {count,
// size, magic, padding, CRC of the CRCs}. The trainer data block (27) holds a
// marker per extra save; marker 6 set means the game still reads the musical
// distribution area that holds the journal banks.
struct MainSave {
    std::array<std::size_t, 2> copies;
    std::size_t used, crcs, trainer, trainerSize;
};
constexpr MainSave kBWSave{{0, 0x24000}, 0x23F9C, (69 + 1) * 2, 0x19400, 0x68};
constexpr MainSave kB2W2Save{{0, 0x26000}, 0x25FA4, 74 * 2, 0x19400, 0xB0};
constexpr std::size_t kGenVMagic = 0x31053527, kTrainerBlock = 27;
constexpr std::size_t kMusicalMarker = 0x30 + 6 * 2, kMarkerSet = 0xC21E;

void CheckGenV(const Bytes &bytes, const MainSave &save) {
    bool found = false;
    for (const auto copy : save.copies) {
        const auto *table = bytes.data() + copy + save.used - 16 - save.crcs;
        const auto *footer = table + save.crcs;
        if (Number(footer + 4, 4) != save.used || Number(footer + 8, 4) != kGenVMagic ||
            Number(footer + 14, 2) != Crc(table, save.crcs)) continue;
        // A block may be current in either copy; the game loads the one whose CRC matches.
        const auto expected = Number(table + 2 * kTrainerBlock, 2);
        for (const auto other : save.copies) {
            const auto *trainer = bytes.data() + other + save.trainer;
            if (Number(trainer + save.trainerSize + 2, 2) != expected || Crc(trainer, save.trainerSize) != expected)
                continue;
            Require(Number(trainer + kMusicalMarker, 2) != kMarkerSet);
            found = true;
        }
    }
    Require(found);
}

void CheckLayout(const Bytes &bytes, const std::string &target) {
    const auto &banks = BanksFor(target);
    if (IsBW(target) || IsB2W2(target)) {
        CheckGenV(bytes, IsBW(target) ? kBWSave : kB2W2Save);
        for (unsigned bank = 0; bank < 2; ++bank) CheckTail(bytes, banks, bank);
        return;
    }
    const auto &layout = LayoutFor(target);
    const auto magic = FooterMagic(target);
    for (unsigned block = 0; block < 2; ++block) {
        bool valid = false;
        for (const auto half : {0U, 262144U}) {
            const auto size = layout.sizes[block];
            const auto *body = bytes.data() + half + layout.offsets[block];
            const auto *footer = body + size - layout.footer;
            valid |= Number(footer + layout.size_at, 4) == size && Number(footer + layout.magic_at, 4) == magic &&
                Number(footer + layout.block_at, layout.block_width) == block &&
                Number(footer + layout.crc_at, 2) == Crc(body, size - layout.footer);
        }
        Require(valid);
        CheckTail(bytes, banks, block);
    }
}

int Hash(const unsigned char *bytes, std::size_t size, unsigned char digest[32]) {
    std::size_t length = 0;
    return EVP_Q_digest(nullptr, "SHA256", nullptr, bytes, size, digest, &length) == 1 && length == 32 ? 0 : -1;
}

struct Context {
    const std::string &path;
    DurableSave *save;
    const Banks &banks;
    const JournalApi &api;
    Bytes image;
    WtJournal journal{};
    WtJournalEntry entry{};
    ~Context() {
        api.close(&journal);
        OPENSSL_cleanse(&entry, sizeof(entry));
        OPENSSL_cleanse(image.data(), image.size());
    }
};

int ReadBank(void *arg, int bank, unsigned char record[4096]) {
    try {
        auto &context = *static_cast<Context *>(arg);
        Require(bank == 0 || bank == 1);
        const auto actual = context.save ? context.save->Image() : ReadOwned(context.path);
        CheckTail(actual, context.banks, static_cast<unsigned>(bank));
        std::memcpy(record, actual.data() + context.banks.offsets[bank], 4096);
        return 0;
    } catch (...) { return -1; }
}

int WriteBank(void *arg, int bank, const unsigned char record[4096]) {
    try {
        auto &context = *static_cast<Context *>(arg);
        Require(context.save && (bank == 0 || bank == 1));
        std::memcpy(context.image.data() + context.banks.offsets[bank], record, 4096);
        context.save->Commit(context.image.data(), context.image.size());
        return 0;
    } catch (...) { return -1; }
}

void Open(Context &context, const std::string &target) {
    context.image = ReadOwned(context.path);
    CheckLayout(context.image, target);
    WtJournalIO io{&context, ReadBank, WriteBank, Hash};
    Require(context.api.open(&context.journal, io) == 0 && context.api.read(&context.journal, &context.entry) == 0);
    Require(context.entry.generation != std::numeric_limits<std::uint64_t>::max());
}

const JournalApi &JournalFor(const std::string &target) {
    return IsBW(target) ? kBWJournal : IsB2W2(target) ? kB2W2Journal : kGen4Journal;
}

std::array<unsigned char, 32> FreshSeed(const unsigned char previous[32]) {
    std::array<unsigned char, 32> seed{};
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::size_t offset = 0;
        while (offset < seed.size()) {
            const auto count = ::getrandom(seed.data() + offset, seed.size() - offset, 0);
            if (count < 0 && errno == EINTR) continue;
            Require(count > 0);
            offset += static_cast<std::size_t>(count);
        }
        if (std::any_of(seed.begin(), seed.end(), [](auto byte) { return byte != 0; }) &&
            std::memcmp(seed.data(), previous, seed.size()) != 0) return seed;
    }
    throw std::runtime_error("host entropy unavailable");
}
}

void InspectJournal(const std::string &path, const std::string &target) {
    Context context{path, nullptr, BanksFor(target), JournalFor(target), {}};
    Open(context, target);
}

void RefreshJournalSeed(const std::string &path, const std::string &target) {
    DurableSave save(path);
    Context context{path, &save, BanksFor(target), JournalFor(target), {}};
    Open(context, target);
    auto seed = FreshSeed(context.entry.seed);
    ++context.entry.generation;
    std::copy(seed.begin(), seed.end(), context.entry.seed);
    OPENSSL_cleanse(seed.data(), seed.size());
    Require(context.api.commit(&context.journal, &context.entry) == 0);
    save.Checkpoint();
    Require(ReadOwned(path) == save.Image());
}
}
