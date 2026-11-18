#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dswt {

// Linux host persistence for one exclusively managed 512 KiB save. A successful
// Commit has appended a sealed record of the changed 4 KiB blocks to the store's
// LOG and fsynced it. Checkpoint (also run on destruction, at a size threshold
// and by every recovery) folds the log into both checked snapshots, the head and
// the raw save. Recovery replays sealed records and drops only a torn final
// record, which no Commit ever acknowledged.
class DurableSave {
public:
    enum class Mode { Existing, Recover };
    explicit DurableSave(const std::string &path, Mode mode = Mode::Existing);
    ~DurableSave();
    void Commit(const std::uint8_t *data, std::size_t length);
    void Checkpoint();
    // The last committed image, as recovery would reproduce it.
    std::vector<std::uint8_t> Image() const;
    bool SupportsHostDurability() const;
    DurableSave(const DurableSave &) = delete;
    DurableSave &operator=(const DurableSave &) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dswt
