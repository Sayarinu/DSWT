#define _POSIX_C_SOURCE 200809L
#include "probe.h"
#include "wonder_trade/pokemon_codec.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <psa/crypto.h>

int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &length) == PSA_SUCCESS && length == 32 ? 0 : -1;
}

int HostRandom(unsigned char *bytes, size_t size)
{
    int fd = open("/dev/urandom", O_RDONLY);
    int result = fd >= 0 && read(fd, bytes, size) == (ssize_t)size ? 0 : -1;
    if (fd >= 0) close(fd);
    return result;
}

const char *Phase(unsigned phase)
{
    static const char *names[] = { "empty", "enrolling", "idle", "escrow", "submit_ready", "queued",
        "received", "applying", "applied", "restoring", "recovery_required" };
    return phase < sizeof(names) / sizeof(*names) ? names[phase] : "unknown";
}

void Hex(const unsigned char *bytes, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i) printf("%02x", bytes[i]);
}

int Checkpoint(Probe *probe, const char *name)
{
    int hit = probe->checkpoint && strcmp(name, probe->checkpoint) == 0 && ++probe->hits == probe->occurrence;
    printf("{\"checkpoint\":\"%s\",\"hit\":%s}\n", name, hit ? "true" : "false");
    fflush(stdout);
    if (hit && strcmp(probe->fault, "kill") == 0) raise(SIGSTOP); /* Supervisor sends SIGKILL. */
    return hit && strcmp(probe->fault, "error") == 0 ? -1 : 0;
}

static int CheckedFile(int directory, const char *name, size_t size)
{
    struct stat info;
    int fd = openat(directory, name, O_RDWR | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1
        || info.st_uid != getuid() || (info.st_mode & 077) || info.st_size != (off_t)size) {
        close(fd); return -1;
    }
    return fd;
}

int OpenFiles(Probe *probe, const char *path)
{
    struct stat info;
    probe->directory = probe->lock = probe->journal = probe->inventory[0] = probe->inventory[1] = -1;
    probe->directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (probe->directory < 0 || fstat(probe->directory, &info) != 0 || info.st_uid != getuid() || (info.st_mode & 077)) return -1;
    probe->lock = CheckedFile(probe->directory, "lock", 0);
    if (probe->lock < 0 || flock(probe->lock, LOCK_EX | LOCK_NB) != 0) return -1;
    probe->journal = CheckedFile(probe->directory, "journal.bin", 8192);
    probe->inventory[0] = CheckedFile(probe->directory, "inventory-0.bin", INVENTORY_SIZE);
    probe->inventory[1] = CheckedFile(probe->directory, "inventory-1.bin", INVENTORY_SIZE);
    probe->full = fstatat(probe->directory, "full", &info, AT_SYMLINK_NOFOLLOW) == 0;
    return probe->journal < 0 || probe->inventory[0] < 0 || probe->inventory[1] < 0 ? -1 : 0;
}

void CloseFiles(Probe *probe)
{
    int fds[] = {probe->inventory[0], probe->inventory[1], probe->journal, probe->lock, probe->directory};
    unsigned i;
    for (i = 0; i < sizeof(fds) / sizeof(*fds); ++i) if (fds[i] >= 0) close(fds[i]);
}

static int Create(int directory, const char *name, const unsigned char *bytes, size_t size)
{
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    int result;
    if (fd < 0) return -1;
    result = write(fd, bytes, size) == (ssize_t)size && fsync(fd) == 0 ? 0 : -1;
    if (close(fd) != 0) result = -1;
    return result;
}

int Initialize(const char *path, const char *fixture)
{
    unsigned char pk4[137], inventory[INVENTORY_SIZE] = {0}, banks[8192];
    WtJournalEntry entry = {0};
    int directory, input = open(fixture, O_RDONLY | O_NOFOLLOW), result = -1;
    if (input < 0) return -1;
    if (read(input, pk4, sizeof(pk4)) != 136) { close(input); return -1; }
    close(input);
    memcpy(inventory, "DSWINV01", 8);
    if (WtPokemon_Encode(pk4, inventory + 8) != 0 || Hash(inventory, INVENTORY_BODY, inventory + INVENTORY_BODY) != 0
        || HostRandom(entry.seed, 32) != 0 || WtJournal_Encode(&entry, Hash, banks) != 0) return -1;
    memcpy(banks + 4096, banks, 4096);
    if (mkdir(path, 0700) != 0) return -1;
    directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory >= 0) {
        if (Create(directory, "lock", banks, 0) == 0 && Create(directory, "journal.bin", banks, sizeof(banks)) == 0
            && Create(directory, "inventory-0.bin", inventory, sizeof(inventory)) == 0
            && Create(directory, "inventory-1.bin", inventory, sizeof(inventory)) == 0 && fsync(directory) == 0) result = 0;
        close(directory);
    }
    return result;
}

int ReadBank(void *arg, int bank, unsigned char record[4096])
{
    Probe *probe = arg;
    return pread(probe->journal, record, 4096, bank * 4096) == 4096 ? 0 : -1;
}

static int Stage(Probe *probe, int fd, off_t offset, const unsigned char *bytes, size_t size, const char *name)
{
    char checkpoint[96];
    snprintf(checkpoint, sizeof(checkpoint), "%s.before", name);
    if (Checkpoint(probe, checkpoint) != 0 || pwrite(fd, bytes, size, offset) != (ssize_t)size) return -1;
    snprintf(checkpoint, sizeof(checkpoint), "%s.written", name);
    if (Checkpoint(probe, checkpoint) != 0 || fsync(fd) != 0) return -1;
    snprintf(checkpoint, sizeof(checkpoint), "%s.after", name);
    return Checkpoint(probe, checkpoint);
}

int WriteBank(void *arg, int bank, const unsigned char record[4096])
{
    Probe *probe = arg;
    char label[96];
    unsigned char zero[4] = {0}, check[4096];
    const char *phase = Phase(record[6]);
    snprintf(label, sizeof(label), "journal.%s.%d.invalidate", phase, bank);
    if (Stage(probe, probe->journal, bank * 4096 + 4092, zero, 4, label) != 0) return -1;
    snprintf(label, sizeof(label), "journal.%s.%d.body", phase, bank);
    if (Stage(probe, probe->journal, bank * 4096, record, 4092, label) != 0) return -1;
    snprintf(label, sizeof(label), "journal.%s.%d.commit", phase, bank);
    if (Stage(probe, probe->journal, bank * 4096 + 4092, record + 4092, 4, label) != 0) return -1;
    return ReadBank(probe, bank, check) == 0 && memcmp(check, record, 4096) == 0 ? 0 : -1;
}

static int ReadInventory(Probe *probe, int bank, unsigned char bytes[INVENTORY_SIZE])
{
    unsigned char digest[32];
    return pread(probe->inventory[bank], bytes, INVENTORY_SIZE, 0) == INVENTORY_SIZE
        && memcmp(bytes, "DSWINV01", 8) == 0 && Hash(bytes, INVENTORY_BODY, digest) == 0
        && memcmp(digest, bytes + INVENTORY_BODY, 32) == 0 ? 0 : -1;
}

static int Empty(const unsigned char *bytes, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i) if (bytes[i]) return 0;
    return 1;
}

static int SaveInventory(Probe *probe, unsigned char bytes[INVENTORY_SIZE], const char *action)
{
    unsigned char zero[32] = {0}, check[INVENTORY_SIZE];
    int bank;
    char label[96];
    if (Hash(bytes, INVENTORY_BODY, bytes + INVENTORY_BODY) != 0) return -1;
    for (bank = 0; bank < 2; ++bank) {
        snprintf(label, sizeof(label), "inventory.%s.%d.invalidate", action, bank);
        if (Stage(probe, probe->inventory[bank], INVENTORY_BODY, zero, 32, label) != 0) return -1;
        snprintf(label, sizeof(label), "inventory.%s.%d.body", action, bank);
        if (Stage(probe, probe->inventory[bank], 0, bytes, INVENTORY_BODY, label) != 0) return -1;
        snprintf(label, sizeof(label), "inventory.%s.%d.commit", action, bank);
        if (Stage(probe, probe->inventory[bank], INVENTORY_BODY, bytes + INVENTORY_BODY, 32, label) != 0
            || ReadInventory(probe, bank, check) != 0 || memcmp(bytes, check, INVENTORY_SIZE) != 0) return -1;
    }
    return 0;
}

int Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    Probe *probe = arg;
    unsigned char banks[2][INVENTORY_SIZE], target[INVENTORY_SIZE], canonical[136];
    int valid[2], bank, selected = -1;
    const unsigned char *digest;
    if (slot >= INVENTORY_SLOTS) return -1;
    for (bank = 0; bank < 2; ++bank) valid[bank] = ReadInventory(probe, bank, banks[bank]) == 0;
    if (step == WT_INVENTORY_PREPARE_REMOVE || step == WT_INVENTORY_PREPARE_RECEIVE) {
        if (!valid[0] || !valid[1] || memcmp(banks[0], banks[1], INVENTORY_SIZE) != 0) return -1;
        memcpy(target, banks[0], INVENTORY_SIZE);
        memcpy(entry->inventoryBefore, target + INVENTORY_BODY, 32);
        if (step == WT_INVENTORY_PREPARE_REMOVE) {
            if (Empty(target + 8 + slot * 236, 236) || WtPokemon_Decode(target + 8 + slot * 236, entry->outgoing) != 0) return -1;
            entry->source = (WtJournalLocator){1, 0, (unsigned char)slot, 236};
            memcpy(entry->original, target + 8 + slot * 236, 236);
            if (Hash(entry->original, 236, entry->originalDigest) != 0) return -1;
            memset(target + 8 + slot * 236, 0, 236);
        } else {
            if (probe->full) return 1;
            for (slot = 0; slot < INVENTORY_SLOTS && !Empty(target + 8 + slot * 236, 236); ++slot) {}
            if (slot == INVENTORY_SLOTS) return 1;
            entry->destination = (WtJournalLocator){1, 0, (unsigned char)slot, 236};
            memcpy(entry->destinationBefore, target + 8 + slot * 236, 236);
            memset(entry->destinationAfter, 0, 236);
            if (entry->deliveryKind == WT_DELIVERY_RETURN) memcpy(entry->destinationAfter, entry->original, 236);
            else if (WtPokemon_Encode(entry->incoming, entry->destinationAfter) != 0) return -1;
            if (WtPokemon_Decode(entry->destinationAfter, canonical) != 0 || memcmp(canonical, entry->incoming, 136) != 0
                || Hash(canonical, 136, entry->appliedDigest) != 0) return -1;
            memcpy(target + 8 + slot * 236, entry->destinationAfter, 236);
        }
        return Hash(target, INVENTORY_BODY, entry->inventoryAfter);
    }
    digest = step == WT_INVENTORY_RESTORE ? entry->inventoryBefore : entry->inventoryAfter;
    for (bank = 0; bank < 2; ++bank) if (valid[bank]) {
        if (memcmp(banks[bank] + INVENTORY_BODY, entry->inventoryBefore, 32) != 0
            && memcmp(banks[bank] + INVENTORY_BODY, entry->inventoryAfter, 32) != 0) return -1;
        selected = bank;
    }
    if (selected < 0) return -1;
    if (step == WT_INVENTORY_VERIFY_REMOVED || step == WT_INVENTORY_VERIFY_RECEIVED)
        return valid[0] && valid[1] && memcmp(banks[0], banks[1], INVENTORY_SIZE) == 0
            && memcmp(banks[0] + INVENTORY_BODY, digest, 32) == 0 ? 0 : -1;
    memcpy(target, banks[selected], INVENTORY_SIZE);
    if (step == WT_INVENTORY_REMOVE) memset(target + 8 + entry->source.slot * 236, 0, 236);
    else if (step == WT_INVENTORY_RESTORE) memcpy(target + 8 + entry->source.slot * 236, entry->original, 236);
    else if (step == WT_INVENTORY_RECEIVE) memcpy(target + 8 + entry->destination.slot * 236, entry->destinationAfter, 236);
    else return -1;
    if (Hash(target, INVENTORY_BODY, target + INVENTORY_BODY) != 0 || memcmp(target + INVENTORY_BODY, digest, 32) != 0) return -1;
    return SaveInventory(probe, target, step == WT_INVENTORY_REMOVE ? "remove" : step == WT_INVENTORY_RESTORE ? "restore" : "receive");
}

int Report(Probe *probe, int result)
{
    WtJournal journal;
    WtJournalEntry entry;
    WtJournalIO io = {probe, ReadBank, WriteBank, Hash};
    unsigned char bytes[INVENTORY_SIZE], canonical[136], digest[32], request[256];
    int bank, slot, first;
    if (WtJournal_Open(&journal, io) != 0 || WtJournal_Read(&journal, &entry) != 0) return -1;
    printf("{\"summary\":true,\"result\":%d,\"blocked\":%s,\"phase\":\"%s\",\"save_id\":\"", result,
        probe->engine.blocked ? "true" : "false", Phase(entry.phase)); Hex(entry.saveID, 16);
    printf("\",\"next_counter\":%llu,\"event_cursor\":%llu,\"generation\":%llu,\"operation_id\":\"",
        (unsigned long long)entry.nextCounter, (unsigned long long)entry.eventCursor, (unsigned long long)entry.generation);
    Hex(entry.requestLength ? entry.request + 8 : entry.lastOperation, 16);
    printf("\",\"pending_sha256\":\"");
    if (entry.requestLength) { Hash(entry.request, entry.requestLength, digest); Hex(digest, 32); }
    printf("\",\"request_sha256\":\"");
    if (entry.requestLength) {
        memcpy(request, entry.request, 40); memcpy(request + 40, entry.request + 72, entry.requestLength - 72);
        Hash(request, entry.requestLength - 32, digest); Hex(digest, 32);
    }
    printf("\",\"trade_id\":\""); Hex(entry.trade, 16);
    printf("\",\"receipt_id\":\""); Hex(entry.receipt, 16);
    printf("\",\"incoming_sha256\":\""); Hex(entry.incomingDigest, 32);
    printf("\",\"inventory\":[");
    for (bank = 0; bank < 2; ++bank) {
        if (bank) printf(",");
        if (ReadInventory(probe, bank, bytes) != 0) { printf("null"); continue; }
        printf("["); first = 1;
        for (slot = 0; slot < INVENTORY_SLOTS; ++slot) if (!Empty(bytes + 8 + slot * 236, 236)) {
            if (WtPokemon_Decode(bytes + 8 + slot * 236, canonical) != 0 || Hash(canonical, 136, digest) != 0) return -1;
            printf("%s\"", first ? "" : ","); Hex(digest, 32); printf("\""); first = 0;
        }
        printf("]");
    }
    printf("]}\n");
    return 0;
}
