#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
std::atomic<unsigned> serial{0};
bool Hit(int fd, bool directory = false) {
    if (!std::getenv("PWT_FAULT_ARMED")) return false;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || (!S_ISREG(info.st_mode) && !(directory && S_ISDIR(info.st_mode)))) return false;
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

extern "C" ssize_t write(int fd, const void *bytes, size_t size) {
    static auto real = reinterpret_cast<decltype(&write)>(::dlsym(RTLD_NEXT, "write"));
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
