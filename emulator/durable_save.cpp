#include "durable_save.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace dswt {
namespace {
constexpr std::size_t kSaveSize = 524288;
using Bytes = std::vector<std::uint8_t>;
using Digest = std::array<std::uint8_t, 32>;
using Lineage = std::array<std::uint8_t, 16>;

[[noreturn]] void Fail() { throw std::system_error(errno, std::generic_category(), "durable save I/O failed"); }
void Require(bool condition) { if (!condition) throw std::runtime_error("inconsistent durable save; preserve recovery files"); }

class Descriptor {
public:
    explicit Descriptor(int fd = -1) : fd_(fd) { if (fd < 0) Fail(); }
    ~Descriptor() { if (fd_ >= 0) ::close(fd_); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int Get() const { return fd_; }
    int Release() { return std::exchange(fd_, -1); }
private:
    int fd_;
};

void Sync(int fd) { while (::fsync(fd) != 0) { if (errno != EINTR) Fail(); } }

struct stat Info(int fd, bool directory) {
    struct stat info{};
    if (::fstat(fd, &info) != 0) Fail();
    Require(info.st_uid == ::geteuid() && (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode)) &&
        (directory ? (info.st_mode & 0022) == 0 : info.st_nlink == 1));
    return info;
}

Bytes Read(int directory, const std::string &name, std::size_t maximum) {
    Descriptor file(::openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    const auto info = Info(file.Get(), false);
    Require(info.st_size >= 0 && static_cast<std::uint64_t>(info.st_size) <= maximum);
    Bytes bytes(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(file.Get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) Fail();
        Require(count > 0);
        offset += static_cast<std::size_t>(count);
    }
    return bytes;
}

Digest Hash(const std::uint8_t *data, std::size_t length) {
    Digest digest{};
    std::size_t size = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, data, length, digest.data(), &size) != 1 || size != digest.size())
        throw std::runtime_error("durable save hashing unavailable");
    return digest;
}
Digest Hash(const Bytes &bytes) { return Hash(bytes.data(), bytes.size()); }

Lineage Random() {
    Lineage value{};
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto count = ::getrandom(value.data() + offset, value.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) Fail();
        Require(count > 0);
        offset += static_cast<std::size_t>(count);
    }
    Require(std::any_of(value.begin(), value.end(), [](auto byte) { return byte != 0; }));
    return value;
}

std::string Temporary() {
    const auto random = Random();
    std::string result = ".dswt-pending-";
    constexpr char digits[] = "0123456789abcdef";
    for (auto byte : random) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}

void Atomic(int directory, const std::string &name, const Bytes &bytes) {
    const auto temporary = Temporary();
    Descriptor file(::openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(file.Get(), bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) Fail();
            Require(count > 0);
            offset += static_cast<std::size_t>(count);
        }
        Sync(file.Get());
        if (::close(file.Release()) != 0) Fail();
        if (::renameat(directory, temporary.c_str(), directory, name.c_str()) != 0) Fail();
        Sync(directory);
    } catch (...) {
        ::unlinkat(directory, temporary.c_str(), 0);
        throw;
    }
}

void Append(Bytes &bytes, std::uint64_t number) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(number >> (i * 8)));
}
template <std::size_t Size> void Append(Bytes &bytes, const std::array<std::uint8_t, Size> &value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
}
template <std::size_t Size> std::array<std::uint8_t, Size> Field(const Bytes &bytes, std::size_t offset) {
    Require(offset <= bytes.size() && Size <= bytes.size() - offset);
    std::array<std::uint8_t, Size> value{};
    std::copy_n(bytes.begin() + offset, Size, value.begin());
    return value;
}
std::uint64_t Generation(const Bytes &bytes) {
    const auto value = Field<8>(bytes, 8);
    std::uint64_t generation = 0;
    for (unsigned i = 0; i < 8; ++i) generation |= static_cast<std::uint64_t>(value[i]) << (i * 8);
    return generation;
}
bool Magic(const Bytes &bytes, const char *magic) {
    return bytes.size() >= 8 && std::memcmp(bytes.data(), magic, 8) == 0;
}
void Seal(Bytes &bytes) { Append(bytes, Hash(bytes)); }
bool Sealed(const Bytes &bytes) {
    return bytes.size() >= 32 && Field<32>(bytes, bytes.size() - 32) == Hash(bytes.data(), bytes.size() - 32);
}

struct Head {
    std::uint64_t generation = 0;
    Lineage lineage{};
    Digest digest{}, previous{};
};
Bytes EncodeHead(const Head &head) {
    Bytes bytes{'D','S','W','H','D','0','0','1'};
    Append(bytes, head.generation); Append(bytes, head.lineage); Append(bytes, head.digest); Append(bytes, head.previous);
    Seal(bytes);
    return bytes;
}
Head DecodeHead(const Bytes &bytes) {
    Require(bytes.size() == 128 && Magic(bytes, "DSWHD001") && Sealed(bytes));
    return {Generation(bytes), Field<16>(bytes, 16), Field<32>(bytes, 32), Field<32>(bytes, 64)};
}
Bytes Snapshot(const Head &head, const Bytes &data) {
    Bytes bytes{'D','S','W','H','S','0','0','1'};
    Append(bytes, head.generation); Append(bytes, head.lineage); Append(bytes, head.digest);
    bytes.insert(bytes.end(), data.begin(), data.end());
    Seal(bytes);
    return bytes;
}
std::optional<Bytes> LoadSnapshot(int directory, const char *name, const Head &head, Bytes &partial) {
    Bytes bytes;
    try { bytes = Read(directory, name, kSaveSize + 96); }
    catch (const std::system_error &error) { if (error.code().value() == ENOENT) return {}; throw; }
    // A future format is evidence, not permission for an older build to repair it.
    Require(std::equal(bytes.begin(), bytes.begin() + std::min<std::size_t>(bytes.size(), 8), "DSWHS001"));
    if (bytes.size() != kSaveSize + 96) { partial = std::move(bytes); return {}; }
    Require(Sealed(bytes));
    Require(Field<16>(bytes, 16) == head.lineage);
    Require(Field<32>(bytes, 32) == Hash(bytes.data() + 64, kSaveSize));
    return bytes;
}

void Publish(int store, int parent, const std::string &name, const Head &head, const Bytes &data) {
    const auto snapshot = Snapshot(head, data);
    Atomic(store, "A", snapshot);
    Atomic(store, "B", snapshot);
    Atomic(store, "HEAD", EncodeHead(head));
    Atomic(parent, name, data);
}

// The write log: a sealed header naming the generation it starts from, then one
// sealed record per Commit holding the changed 4 KiB blocks and the digest of
// the whole image they produce.
constexpr std::size_t kBlock = 4096, kLogHeader = 96, kRecordFixed = 8 + 4 + 8 + 32 + 4 + 32;
constexpr std::size_t kLogCheckpoint = 16u << 20, kLogMaximum = 64u << 20;
constexpr char kLogMagic[] = "DSWLG001", kRecordMagic[] = "DSWLR001";

std::uint32_t U32(const Bytes &bytes, std::size_t offset) {
    const auto value = Field<4>(bytes, offset);
    return value[0] | value[1] << 8 | value[2] << 16 | static_cast<std::uint32_t>(value[3]) << 24;
}
void Append32(Bytes &bytes, std::uint32_t number) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(number >> (i * 8)));
}
Bytes EncodeLogHeader(const Lineage &lineage, std::uint64_t base, const Digest &digest) {
    Bytes bytes(kLogMagic, kLogMagic + 8);
    Append(bytes, lineage); Append(bytes, base); Append(bytes, digest);
    Seal(bytes);
    return bytes;
}

struct Record {
    std::uint64_t generation;
    Digest digest;
    std::vector<std::pair<std::uint32_t, Bytes>> blocks;
};
Bytes EncodeRecord(const Record &record) {
    Bytes bytes(kRecordMagic, kRecordMagic + 8);
    std::size_t total = kRecordFixed;
    for (const auto &block : record.blocks) total += 8 + block.second.size();
    Append32(bytes, static_cast<std::uint32_t>(total));
    Append(bytes, record.generation); Append(bytes, record.digest);
    Append32(bytes, static_cast<std::uint32_t>(record.blocks.size()));
    for (const auto &block : record.blocks) {
        Append32(bytes, block.first); Append32(bytes, static_cast<std::uint32_t>(block.second.size()));
        bytes.insert(bytes.end(), block.second.begin(), block.second.end());
    }
    Seal(bytes);
    Require(bytes.size() == total);
    return bytes;
}

struct Log {
    Lineage lineage{};
    std::uint64_t base = 0;
    Digest digest{};
    std::vector<Record> records;
    bool torn = false;
};
// Every record must be complete and sealed, except that a torn final record (a
// write no Commit acknowledged) is dropped. Anything else is preserved evidence.
Log DecodeLog(const Bytes &bytes) {
    Require(bytes.size() >= kLogHeader && Magic(bytes, kLogMagic));
    const Bytes header(bytes.begin(), bytes.begin() + kLogHeader);
    Require(Sealed(header));
    Log log{Field<16>(header, 8), Generation(Bytes(header.begin() + 16, header.end())), Field<32>(header, 32), {}, false};
    std::size_t at = kLogHeader;
    while (at < bytes.size()) {
        const auto left = bytes.size() - at;
        const auto tail = [&] { log.torn = true; at = bytes.size(); };
        if (left < 12) {
            const auto known = std::min<std::size_t>(left, 8);
            Require(std::equal(bytes.begin() + at, bytes.begin() + at + known, kRecordMagic) ||
                std::all_of(bytes.begin() + at, bytes.end(), [](auto b) { return b == 0; }));
            tail(); continue;
        }
        if (!std::equal(bytes.begin() + at, bytes.begin() + at + 8, kRecordMagic)) {
            Require(std::all_of(bytes.begin() + at, bytes.end(), [](auto b) { return b == 0; }));
            tail(); continue;
        }
        const auto total = U32(bytes, at + 8);
        Require(total >= kRecordFixed && total <= kRecordFixed + 8 * (kSaveSize / kBlock) + kSaveSize);
        if (total > left) { tail(); continue; }
        const Bytes record(bytes.begin() + at, bytes.begin() + at + total);
        if (!Sealed(record)) { Require(at + total == bytes.size()); tail(); continue; }
        Record parsed{Generation(Bytes(record.begin() + 4, record.end())), Field<32>(record, 20), {}};
        const auto count = U32(record, 52);
        std::size_t offset = 56;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto start = U32(record, offset), length = U32(record, offset + 4);
            offset += 8;
            Require(length > 0 && length % kBlock == 0 && start % kBlock == 0 && start <= kSaveSize &&
                length <= kSaveSize - start && offset + length <= total - 32);
            parsed.blocks.emplace_back(start, Bytes(record.begin() + offset, record.begin() + offset + length));
            offset += length;
        }
        Require(offset == total - 32 && count > 0);
        Require(parsed.generation == log.base + log.records.size() + 1);
        log.records.push_back(std::move(parsed));
        at += total;
    }
    return log;
}
} // namespace

struct DurableSave::Impl {
    Descriptor parent, lock;
    std::unique_ptr<Descriptor> store, log;
    std::string name, store_name;
    Head head;
    Bytes image;
    std::uint64_t generation = 0;
    Digest digest{};
    std::size_t log_size = 0, pending = 0;
    std::mutex mutex;
    bool usable = true;

    Impl(const std::string &path, Mode mode)
        : parent(::open(std::filesystem::path(path).parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)),
          lock(::open((path + ".dswt.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)),
          name(std::filesystem::path(path).filename().string()), store_name(name + ".dswt") {
        Info(parent.Get(), true); Info(lock.Get(), false);
        Require(!name.empty() && name != "." && name != "..");
        // The launcher passes its held lock to the emulator. The preparation
        // utility acquires it independently and releases it before launch.
        const auto *inherited = std::getenv("DSWT_SAVE_LOCK_FD");
        if (inherited) {
            std::size_t end = 0;
            const auto fd = std::stoi(inherited, &end);
            Require(end == std::strlen(inherited) && fd >= 3);
            const auto passed = Info(fd, false), own = Info(lock.Get(), false);
            Require(passed.st_dev == own.st_dev && passed.st_ino == own.st_ino);
            if (::flock(fd, LOCK_EX | LOCK_NB) != 0) Fail();
            if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) Fail();
        } else if (::flock(lock.Get(), LOCK_EX | LOCK_NB) != 0) Fail();
        struct stat found{};
        if (::fstatat(parent.Get(), store_name.c_str(), &found, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) Fail();
            Require(mode == Mode::Recover);
            const auto data = Read(parent.Get(), name, kSaveSize);
            Require(data.size() == kSaveSize);
            head.lineage = Random(); head.digest = Hash(data); head.previous = head.digest;
            const auto staging = Temporary();
            if (::mkdirat(parent.Get(), staging.c_str(), 0700) != 0) Fail();
            store = std::make_unique<Descriptor>(::openat(parent.Get(), staging.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            const auto snapshot = Snapshot(head, data);
            Atomic(store->Get(), "A", snapshot); Atomic(store->Get(), "B", snapshot); Atomic(store->Get(), "HEAD", EncodeHead(head));
            Atomic(store->Get(), "LOG", EncodeLogHeader(head.lineage, head.generation, head.digest));
            Descriptor raw(::openat(parent.Get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
            Sync(raw.Get());
            if (::renameat(parent.Get(), staging.c_str(), parent.Get(), store_name.c_str()) != 0) Fail();
            Sync(parent.Get());
            store = std::make_unique<Descriptor>(::openat(parent.Get(), store_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            image = data; generation = head.generation; digest = head.digest;
            OpenLog();
        } else {
            store = std::make_unique<Descriptor>(::openat(parent.Get(), store_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            const auto info = Info(store->Get(), true);
            Require((info.st_mode & 0077) == 0);
            Recover(mode);
        }
    }

    ~Impl() {
        // Best effort: every committed record is already durable in the log.
        try { std::lock_guard<std::mutex> guard(mutex); if (usable && pending) Compact(); } catch (...) {}
    }

    void OpenLog() {
        log = std::make_unique<Descriptor>(::openat(store->Get(), "LOG", O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW));
        const auto info = Info(log->Get(), false);
        log_size = static_cast<std::size_t>(info.st_size);
    }

    void Recover(Mode mode) {
        head = DecodeHead(Read(store->Get(), "HEAD", 128));
        Bytes partial_first, partial_second;
        const auto first = LoadSnapshot(store->Get(), "A", head, partial_first);
        const auto second = LoadSnapshot(store->Get(), "B", head, partial_second);
        Require(first || second);
        std::optional<Log> found;
        try { found = DecodeLog(Read(store->Get(), "LOG", kLogMaximum)); }
        catch (const std::system_error &error) { if (error.code().value() != ENOENT) throw; }
        // A snapshot or head ahead of the last checkpoint must be proven by a logged
        // record. Without a log (an older store), only one publication can be in flight.
        const auto proven = [&](std::uint64_t at, const Digest &value) {
            if (at == head.generation) return value == head.digest;
            if (!found) return at == head.generation + 1;
            if (at < found->base || at - found->base > found->records.size()) return false;
            return at == found->base ? value == found->digest : found->records[at - found->base - 1].digest == value;
        };
        if (found) {
            Require(found->lineage == head.lineage && found->base <= head.generation);
            if (head.generation != found->base) {
                Require(head.generation - found->base <= found->records.size() &&
                    found->records[head.generation - found->base - 1].digest == head.digest);
            } else Require(found->digest == head.digest);
        }
        for (const auto *snapshot : {&first, &second}) {
            if (*snapshot) Require(Generation(**snapshot) >= head.generation && proven(Generation(**snapshot), Field<32>(**snapshot, 32)));
        }
        if (first && second && Generation(*first) == Generation(*second)) Require(*first == *second);
        const auto &selected = !first ? *second : !second || Generation(*first) >= Generation(*second) ? *first : *second;
        // Repair truncation only when the surviving checked image proves every
        // remaining byte. Unknown/foreign evidence is never overwritten.
        Require(std::equal(partial_first.begin(), partial_first.end(), selected.begin()) &&
            std::equal(partial_second.begin(), partial_second.end(), selected.begin()));
        const auto selected_digest = Field<32>(selected, 32);
        const auto raw = Read(parent.Get(), name, kSaveSize);
        if (raw.size() == kSaveSize) {
            const auto hash = Hash(raw);
            Require(hash == head.digest || hash == head.previous || hash == selected_digest);
        } else {
            const auto prefix = [&](const std::optional<Bytes> &snapshot) {
                return snapshot && std::equal(raw.begin(), raw.end(), snapshot->begin() + 64);
            };
            Require(prefix(first) || prefix(second));
        }
        image.assign(selected.begin() + 64, selected.begin() + 64 + kSaveSize);
        generation = Generation(selected); digest = selected_digest;
        if (found) {
            for (const auto &record : found->records) {
                if (record.generation <= generation) continue;
                Require(record.generation == generation + 1);
                for (const auto &block : record.blocks) std::copy(block.second.begin(), block.second.end(), image.begin() + block.first);
                Require(Hash(image) == record.digest);
                generation = record.generation; digest = record.digest;
            }
        }
        if (mode == Mode::Existing) {
            Require(first && second && *first == *second && generation == head.generation && Generation(selected) == generation &&
                raw.size() == kSaveSize && Hash(raw) == digest && found && found->base == generation &&
                found->records.empty() && !found->torn);
            OpenLog();
            return;
        }
        Compact();
    }

    // Fold the log into the checked snapshots, the head and the raw save, then
    // start a new log from that generation.
    void Compact() {
        Head next = head;
        if (generation != head.generation) next.previous = head.digest;
        next.generation = generation; next.digest = digest;
        if (log && ::close(log->Release()) != 0) Fail();
        log.reset();
        Publish(store->Get(), parent.Get(), name, next, image);
        head = next;
        Atomic(store->Get(), "LOG", EncodeLogHeader(head.lineage, head.generation, head.digest));
        OpenLog();
        pending = 0;
    }

    void Commit(const std::uint8_t *data, std::size_t length) {
        std::lock_guard<std::mutex> guard(mutex);
        Require(usable && data && length == kSaveSize && log && generation != std::numeric_limits<std::uint64_t>::max());
        usable = false;
        Record record{generation + 1, {}, {}};
        for (std::size_t at = 0; at < kSaveSize; at += kBlock) {
            if (std::memcmp(data + at, image.data() + at, kBlock) == 0) continue;
            if (!record.blocks.empty() && record.blocks.back().first + record.blocks.back().second.size() == at)
                record.blocks.back().second.insert(record.blocks.back().second.end(), data + at, data + at + kBlock);
            else record.blocks.emplace_back(static_cast<std::uint32_t>(at), Bytes(data + at, data + at + kBlock));
        }
        if (record.blocks.empty()) { usable = true; return; }
        record.digest = Hash(data, length);
        const auto bytes = EncodeRecord(record);
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(log->Get(), bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) Fail();
            Require(count > 0);
            offset += static_cast<std::size_t>(count);
        }
        Sync(log->Get());
        std::copy(data, data + length, image.begin());
        generation = record.generation; digest = record.digest;
        log_size += bytes.size(); ++pending;
        if (log_size > kLogCheckpoint) Compact();
        usable = true;
    }

    void Checkpoint() {
        std::lock_guard<std::mutex> guard(mutex);
        Require(usable);
        if (!pending) return;
        usable = false;
        Compact();
        usable = true;
    }
};

DurableSave::DurableSave(const std::string &path, Mode mode)
    : impl_(std::make_unique<Impl>(std::filesystem::absolute(path).lexically_normal().string(), mode)) {}
DurableSave::~DurableSave() = default;
void DurableSave::Commit(const std::uint8_t *data, std::size_t length) { impl_->Commit(data, length); }
void DurableSave::Checkpoint() { impl_->Checkpoint(); }
std::vector<std::uint8_t> DurableSave::Image() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    Require(impl_->usable);
    return impl_->image;
}
bool DurableSave::SupportsHostDurability() const {
    struct statfs filesystem{};
    if (::fstatfs(impl_->parent.Get(), &filesystem) != 0) Fail();
    const auto type = static_cast<unsigned long>(filesystem.f_type);
    return type == static_cast<unsigned long>(EXT4_SUPER_MAGIC) ||
        type == static_cast<unsigned long>(BTRFS_SUPER_MAGIC) || type == static_cast<unsigned long>(XFS_SUPER_MAGIC);
}

} // namespace dswt
