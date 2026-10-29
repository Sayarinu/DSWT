#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
std::atomic<unsigned> serial{0};
std::atomic<bool> phase_ready{false};

void ObservePhase(const void *bytes, std::size_t size) {
    const auto *phase = std::getenv("PWT_FAULT_JOURNAL_PHASE");
    if (!phase || phase_ready.load() || !std::getenv("PWT_FAULT_ARMED")) return;
    const auto *data = static_cast<const unsigned char *>(bytes);
    const std::size_t prefix = size == 524288 + 96 && std::memcmp(data, "PWTHS001", 8) == 0 ? 64 : 0;
    if (size != 524288 + (prefix ? 96 : 0)) return;
    for (const auto bank : {0x30000U, 0x70000U}) {
        const auto *record = data + prefix + bank;
        // A test selector, not journal validation or ownership authority.
        // The host/SDK verifiers still check the complete published records.
        const unsigned value = record[6] | (static_cast<unsigned>(record[7]) << 8);
        if (std::memcmp(record, "PWTJ\1\0", 6) == 0 && std::memcmp(record + 4092, "CMIT", 4) == 0 &&
            value == std::strtoul(phase, nullptr, 10)) {
            phase_ready.store(true);
            ::syscall(SYS_write, STDERR_FILENO, "PHASE_READY\n", 12);
            return;
        }
    }
}

bool Hit(int fd, bool directory = false) {
    if (!std::getenv("PWT_FAULT_ARMED")) return false;
    if (std::getenv("PWT_FAULT_JOURNAL_PHASE") && !phase_ready.load()) return false;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || (!S_ISREG(info.st_mode) && !(directory && S_ISDIR(info.st_mode)))) return false;
    // The real Qt process also writes configuration/firmware. Restrict optional
    // GUI fault injection to this save's atomic-publication files/directories.
    if (const auto *save = std::getenv("PWT_FAULT_SAVE")) {
        char target[4096];
        const auto descriptor = "/proc/self/fd/" + std::to_string(fd);
        const auto length = ::readlink(descriptor.c_str(), target, sizeof(target));
        if (length < 0 || static_cast<std::size_t>(length) == sizeof(target)) return false;
        const std::string path(target, static_cast<std::size_t>(length)), raw(save);
        const auto parent = raw.substr(0, raw.find_last_of('/')), store = raw + ".platinumwt";
        if (S_ISDIR(info.st_mode)) {
            if (path != parent && path != store) return false;
        } else if (path.rfind(parent + "/.platinumwt-pending-", 0) != 0 &&
                   path.rfind(store + "/.platinumwt-pending-", 0) != 0) return false;
    }
    const auto *selected = std::getenv("PWT_FAULT_AT");
    return selected && ++serial == std::strtoul(selected, nullptr, 10);
}
bool Before(bool hit) {
    if (!hit) return false;
    const auto *mode = std::getenv("PWT_FAULT_MODE");
    if (mode && std::strcmp(mode, "error") == 0) { errno = ENOSPC; return true; }
    if (!mode || std::strcmp(mode, "after") != 0) {
        ::syscall(SYS_write, STDERR_FILENO, "STOPPED\n", 8);
        ::raise(SIGSTOP);
    }
    return false;
}
void After(bool hit) {
    const auto *mode = std::getenv("PWT_FAULT_MODE");
    if (hit && mode && std::strcmp(mode, "after") == 0) {
        ::syscall(SYS_write, STDERR_FILENO, "STOPPED\n", 8);
        ::raise(SIGSTOP);
    }
}
} // namespace

extern "C" ssize_t getrandom(void *bytes, size_t size, unsigned flags) {
    static auto real = reinterpret_cast<decltype(&getrandom)>(::dlsym(RTLD_NEXT, "getrandom"));
    const auto *mode = std::getenv("PWT_SEED_RANDOM_MODE");
    if (size == 32 && mode) {
        if (std::strcmp(mode, "error") == 0) { errno = EIO; return -1; }
        if (std::strcmp(mode, "zero") == 0 || std::strcmp(mode, "same") == 0) {
            std::memset(bytes, std::strcmp(mode, "same") == 0 ? 4 : 0, size);
            return static_cast<ssize_t>(size);
        }
    }
    return real(bytes, size, flags);
}

extern "C" ssize_t write(int fd, const void *bytes, size_t size) {
    static auto real = reinterpret_cast<decltype(&write)>(::dlsym(RTLD_NEXT, "write"));
    ObservePhase(bytes, size);
    const auto hit = Hit(fd);
    if (Before(hit)) return -1;
    const auto result = real(fd, bytes, size); After(hit); return result;
}
extern "C" int fsync(int fd) {
    static auto real = reinterpret_cast<decltype(&fsync)>(::dlsym(RTLD_NEXT, "fsync"));
    const auto hit = Hit(fd, true);
    if (Before(hit)) return -1;
    const auto result = real(fd); After(hit); return result;
}
extern "C" int close(int fd) {
    static auto real = reinterpret_cast<decltype(&close)>(::dlsym(RTLD_NEXT, "close"));
    const auto flags = ::fcntl(fd, F_GETFL);
    const auto hit = flags >= 0 && (flags & O_ACCMODE) == O_WRONLY && Hit(fd);
    if (Before(hit)) return -1;
    const auto result = real(fd); After(hit); return result;
}
extern "C" int renameat(int olddir, const char *oldname, int newdir, const char *newname) {
    static auto real = reinterpret_cast<decltype(&renameat)>(::dlsym(RTLD_NEXT, "renameat"));
    const auto hit = Hit(olddir, true);
    if (Before(hit)) return -1;
    const auto result = real(olddir, oldname, newdir, newname); After(hit); return result;
}
