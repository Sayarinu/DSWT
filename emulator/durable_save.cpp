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
    Bytes bytes{'P','W','T','H','D','0','0','1'};
    Append(bytes, head.generation); Append(bytes, head.lineage); Append(bytes, head.digest); Append(bytes, head.previous);
    Seal(bytes);
    return bytes;
}
Head DecodeHead(const Bytes &bytes) {
    Require(bytes.size() == 128 && Magic(bytes, "DSWHD001") && Sealed(bytes));
    return {Generation(bytes), Field<16>(bytes, 16), Field<32>(bytes, 32), Field<32>(bytes, 64)};
}
Bytes Snapshot(const Head &head, const Bytes &data) {
    Bytes bytes{'P','W','T','H','S','0','0','1'};
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
} // namespace

struct DurableSave::Impl {
    Descriptor parent, lock;
    std::unique_ptr<Descriptor> store;
    std::string name, store_name;
    Head head;
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
            Descriptor raw(::openat(parent.Get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
            Sync(raw.Get());
            if (::renameat(parent.Get(), staging.c_str(), parent.Get(), store_name.c_str()) != 0) Fail();
            Sync(parent.Get());
        } else {
            store = std::make_unique<Descriptor>(::openat(parent.Get(), store_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            const auto info = Info(store->Get(), true);
            Require((info.st_mode & 0077) == 0);
            Recover(mode);
        }
    }

    void Recover(Mode mode) {
        head = DecodeHead(Read(store->Get(), "HEAD", 128));
        Bytes partial_first, partial_second;
        const auto first = LoadSnapshot(store->Get(), "A", head, partial_first);
        const auto second = LoadSnapshot(store->Get(), "B", head, partial_second);
        Require(first || second);
        const auto &selected = !first ? *second : !second || Generation(*first) >= Generation(*second) ? *first : *second;
        // Repair truncation only when the surviving checked image proves every
        // remaining byte. Unknown/foreign evidence is never overwritten.
        Require(std::equal(partial_first.begin(), partial_first.end(), selected.begin()) &&
            std::equal(partial_second.begin(), partial_second.end(), selected.begin()));
        const auto generation = Generation(selected);
        Require(generation >= head.generation && generation - head.generation <= 1);
        if (first && second) {
            const auto low = std::min(Generation(*first), Generation(*second));
            Require(generation - low <= 1 && (Generation(*first) != Generation(*second) || *first == *second));
        }
        const auto digest = Field<32>(selected, 32);
        if (generation == head.generation) Require(digest == head.digest);
        const auto raw = Read(parent.Get(), name, kSaveSize);
        if (raw.size() == kSaveSize) {
            const auto hash = Hash(raw);
            Require(hash == head.digest || hash == head.previous || hash == digest);
        } else {
            const auto prefix = [&](const std::optional<Bytes> &snapshot) {
                return snapshot && std::equal(raw.begin(), raw.end(), snapshot->begin() + 64);
            };
            Require(prefix(first) || prefix(second));
        }
        if (mode == Mode::Existing) {
            Require(first && second && *first == *second && generation == head.generation && raw.size() == kSaveSize && Hash(raw) == digest);
            return;
        }
        if (generation != head.generation) head.previous = head.digest;
        head.generation = generation; head.digest = digest;
        const Bytes data(selected.begin() + 64, selected.begin() + 64 + kSaveSize);
        Publish(store->Get(), parent.Get(), name, head, data);
    }

    void Commit(const std::uint8_t *data, std::size_t length) {
        std::lock_guard<std::mutex> guard(mutex);
        Require(usable && data && length == kSaveSize && head.generation != std::numeric_limits<std::uint64_t>::max());
        usable = false;
        const auto expected = head;
        Recover(Mode::Existing);
        Require(head.generation == expected.generation && head.lineage == expected.lineage && head.digest == expected.digest && head.previous == expected.previous);
        const Bytes bytes(data, data + length);
        Head next = head;
        ++next.generation; next.previous = head.digest; next.digest = Hash(bytes);
        Publish(store->Get(), parent.Get(), name, next, bytes);
        head = next;
        usable = true;
    }
};

DurableSave::DurableSave(const std::string &path, Mode mode)
    : impl_(std::make_unique<Impl>(std::filesystem::absolute(path).lexically_normal().string(), mode)) {}
DurableSave::~DurableSave() = default;
void DurableSave::Commit(const std::uint8_t *data, std::size_t length) { impl_->Commit(data, length); }
bool DurableSave::SupportsHostDurability() const {
    struct statfs filesystem{};
    if (::fstatfs(impl_->parent.Get(), &filesystem) != 0) Fail();
    const auto type = static_cast<unsigned long>(filesystem.f_type);
    return type == static_cast<unsigned long>(EXT4_SUPER_MAGIC) ||
        type == static_cast<unsigned long>(BTRFS_SUPER_MAGIC) || type == static_cast<unsigned long>(XFS_SUPER_MAGIC);
}

} // namespace dswt
