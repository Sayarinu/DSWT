#include "journal_seed.h"
#include "durable_save.h"
#include "wonder_trade/journal.h"

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

namespace platinumwt {
namespace {
constexpr std::size_t kSize = 524288, kBankSize = 65536;
constexpr std::array<std::size_t, 2> kBanks{0x30000, 0x70000};
using Bytes = std::vector<std::uint8_t>;

void Require(bool condition) { if (!condition) throw std::runtime_error("unsupported journal; preserve recovery evidence"); }

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

void CheckTail(const Bytes &bytes, unsigned bank) {
    const auto begin = bytes.begin() + kBanks.at(bank) + 4096;
    const auto end = bytes.begin() + kBanks.at(bank) + kBankSize;
    Require((*begin == 0 || *begin == 255) && std::all_of(begin, end, [&](auto byte) { return byte == *begin; }));
}

void CheckLayout(const Bytes &bytes, const std::string &target) {
    Require(target == "us-rev0" || target == "us-rev1");
    constexpr std::array<std::size_t, 2> offsets{0, 53036}, sizes{53036, 74212};
    for (unsigned block = 0; block < 2; ++block) {
        bool valid = false;
        for (const auto half : {0U, 262144U}) {
            const auto *body = bytes.data() + half + offsets[block];
            const auto *footer = body + sizes[block] - 20;
            valid |= Number(footer + 8, 4) == sizes[block] && Number(footer + 12, 4) == 0x20060623 &&
                footer[16] == block && Number(footer + 18, 2) == Crc(body, sizes[block] - 20);
        }
        Require(valid);
        CheckTail(bytes, block);
    }
}

int Hash(const unsigned char *bytes, std::size_t size, unsigned char digest[32]) {
    std::size_t length = 0;
    return EVP_Q_digest(nullptr, "SHA256", nullptr, bytes, size, digest, &length) == 1 && length == 32 ? 0 : -1;
}

struct Context {
    const std::string &path;
    DurableSave *save;
    Bytes image;
    WtJournal journal{};
    WtJournalEntry entry{};
    ~Context() {
        WtJournal_Close(&journal);
        OPENSSL_cleanse(&entry, sizeof(entry));
        OPENSSL_cleanse(image.data(), image.size());
    }
};

int ReadBank(void *arg, int bank, unsigned char record[4096]) {
    try {
        auto &context = *static_cast<Context *>(arg);
        Require(bank == 0 || bank == 1);
        const auto actual = ReadOwned(context.path);
        CheckTail(actual, static_cast<unsigned>(bank));
        std::memcpy(record, actual.data() + kBanks[bank], 4096);
        return 0;
    } catch (...) { return -1; }
}

int WriteBank(void *arg, int bank, const unsigned char record[4096]) {
    try {
        auto &context = *static_cast<Context *>(arg);
        Require(context.save && (bank == 0 || bank == 1));
        std::memcpy(context.image.data() + kBanks[bank], record, 4096);
        context.save->Commit(context.image.data(), context.image.size());
        return 0;
    } catch (...) { return -1; }
}

void Open(Context &context, const std::string &target) {
    context.image = ReadOwned(context.path);
    CheckLayout(context.image, target);
    WtJournalIO io{&context, ReadBank, WriteBank, Hash};
    Require(WtJournal_Open(&context.journal, io) == 0 && WtJournal_Read(&context.journal, &context.entry) == 0);
    Require(context.entry.generation != std::numeric_limits<std::uint64_t>::max());
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
    Context context{path, nullptr, {}};
    Open(context, target);
}

void RefreshJournalSeed(const std::string &path, const std::string &target) {
    DurableSave save(path);
    Context context{path, &save, {}};
    Open(context, target);
    auto seed = FreshSeed(context.entry.seed);
    ++context.entry.generation;
    std::copy(seed.begin(), seed.end(), context.entry.seed);
    OPENSSL_cleanse(seed.data(), seed.size());
    Require(WtJournal_Commit(&context.journal, &context.entry) == 0);
}
}
