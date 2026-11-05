#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dswt {

// Linux host persistence for one exclusively managed 512 KiB save. Successful
// Commit means both recovery snapshots, the head and the raw save are fsynced.
class DurableSave {
public:
    enum class Mode { Existing, Recover };
    explicit DurableSave(const std::string &path, Mode mode = Mode::Existing);
    ~DurableSave();
    void Commit(const std::uint8_t *data, std::size_t length);
    bool SupportsHostDurability() const;
    DurableSave(const DurableSave &) = delete;
    DurableSave &operator=(const DurableSave &) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dswt
