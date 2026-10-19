#include "wonder_trade/flash.h"

#include <nitro.h>
#include <string.h>
#include <mbedtls/private/sha256.h>

#include "system.h"
#include "wonder_trade/persistence.h"

enum {
    LOCK_FAILED = -20, UNSUPPORTED_FLASH = -21, READ_FAILED = -22,
    FOREIGN_SECTOR = -23, UNSAFE_STORAGE = -24
};

static int Capability(void)
{
    unsigned char descriptor[WT_PERSISTENCE_SIZE];
    return CARD_ReadFlash(WT_PERSISTENCE_ADDRESS, descriptor, sizeof(descriptor))
        && memcmp(descriptor, WT_PERSISTENCE_DESCRIPTOR, sizeof(descriptor)) == 0;
}

int WtFlash_ReadBank(void *arg, int bank, unsigned char record[4096])
{
    WtFlashAccess *access = arg;
    unsigned char page[256];
    u32 address, offset;
    int blank = -1, result = -1;
    s32 lockID;
    if (!access || !record || bank < 0 || bank > 1) return -1;
    address = bank == 0 ? 0x30000 : 0x70000;
    lockID = OS_GetLockID();
    if (lockID == OS_LOCK_ID_ERROR) { access->error = LOCK_FAILED; return -1; }
    CARD_LockBackup(lockID);
    if (CARD_GetCurrentBackupType() != CARD_BACKUP_TYPE_FLASH_4MBITS) {
        access->error = UNSUPPORTED_FLASH;
        goto done;
    }
    if (access->requireDurability && !Capability()) { access->error = UNSAFE_STORAGE; goto done; }
    if (!CARD_ReadFlash(address, record, 4096)) { access->error = READ_FAILED; goto done; }
    for (offset = 4096; offset < 0x10000; offset += sizeof(page)) {
        u32 i;
        if (!CARD_ReadFlash(address + offset, page, sizeof(page))) { access->error = READ_FAILED; goto done; }
        for (i = 0; i < sizeof(page); i++) {
            if (blank == -1) blank = page[i];
            if ((blank != 0 && blank != 255) || page[i] != blank) { access->error = FOREIGN_SECTOR; goto done; }
        }
    }
    result = 0;
done:
    CARD_UnlockBackup(lockID);
    OS_ReleaseLockID(lockID);
    return result;
}

int WtFlash_WriteBank(void *arg, int bank, const unsigned char record[4096])
{
    static const unsigned char uncommitted[4] = { 0, 0, 0, 0 };
    WtFlashAccess *access = arg;
    unsigned char page[256];
    u32 address, offset;
    int result = -1;
    s32 lockID;
    if (!access || !record || bank < 0 || bank > 1) return -1;
    address = bank == 0 ? 0x30000 : 0x70000;
    lockID = OS_GetLockID();
    if (lockID == OS_LOCK_ID_ERROR) { access->error = LOCK_FAILED; return -1; }
    SleepLock(SLEEP_TYPE_SAVE_DATA);
    CARD_LockBackup(lockID);
    if (CARD_GetCurrentBackupType() != CARD_BACKUP_TYPE_FLASH_4MBITS) { access->error = UNSUPPORTED_FLASH; goto done; }
    if (!Capability()) { access->error = UNSAFE_STORAGE; goto done; }
    if (!CARD_WriteAndVerifyFlash(address + 4092, uncommitted, sizeof(uncommitted))
        || !CARD_WriteAndVerifyFlash(address, record, 4092)
        || !CARD_WriteAndVerifyFlash(address + 4092, record + 4092, 4)) goto done;
    for (offset = 0; offset < 4096; offset += sizeof(page)) {
        if (!CARD_ReadFlash(address + offset, page, sizeof(page)) || memcmp(page, record + offset, sizeof(page)) != 0) goto done;
    }
    result = 0;
done:
    CARD_UnlockBackup(lockID);
    OS_ReleaseLockID(lockID);
    SleepUnlock(SLEEP_TYPE_SAVE_DATA);
    return result;
}

int WtFlash_Hash(const unsigned char *bytes, size_t length, unsigned char digest[32])
{
    /* Stack-only hashing is available before the TLS allocator/RNG starts. */
    return mbedtls_sha256(bytes, length, digest, 0);
}

int WtFlash_ReadRange(unsigned address, unsigned char *bytes, size_t length)
{
    s32 lockID;
    int result = -1;
    if (address > 0x80000 || length > 0x80000 - address) return -1;
    lockID = OS_GetLockID();
    if (lockID == OS_LOCK_ID_ERROR) return -1;
    CARD_LockBackup(lockID);
    if (CARD_GetCurrentBackupType() == CARD_BACKUP_TYPE_FLASH_4MBITS
        && Capability() && CARD_ReadFlash(address, bytes, length)) result = 0;
    CARD_UnlockBackup(lockID);
    OS_ReleaseLockID(lockID);
    return result;
}
