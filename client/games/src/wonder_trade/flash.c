#include "wonder_trade/journal_layout.h"
#include "wonder_trade/flash.h"

#include <nitro.h>
#include <string.h>
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>

#include "wonder_trade/game_adapter.h"

enum {
    LOCK_FAILED = -20, UNSUPPORTED_FLASH = -21, READ_FAILED = -22,
    FOREIGN_SECTOR = -23
};

/* Called with the backup device locked. */
static int Supported(void)
{
    u32 type = (u32)CARD_GetCurrentBackupType();
    return type == gWtGameConfig.flashType[0] || type == gWtGameConfig.flashType[1];
}

int WtFlash_ReadBank(void *arg, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    WtFlashAccess *access = arg;
    unsigned char page[WT_FLASH_PAGE_SIZE];
    u32 address, offset;
    int blank = -1, result = -1;
    s32 lockID;
    if (!access || !record || bank < 0 || bank > 1) return -1;
    address = gWtGameConfig.journalBank[bank];
    lockID = OS_GetLockID();
    if (lockID == OS_LOCK_ID_ERROR) { access->error = LOCK_FAILED; return -1; }
    CARD_LockBackup(lockID);
    if (!Supported()) {
        access->error = UNSUPPORTED_FLASH;
        goto done;
    }
    if (!CARD_ReadFlash(address, record, WT_JOURNAL_RECORD_SIZE)) { access->error = READ_FAILED; goto done; }
    for (offset = WT_JOURNAL_RECORD_SIZE; offset < WT_GAME_BANK_RESERVED_SIZE; offset += sizeof(page)) {
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

int WtFlash_WriteBank(void *arg, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    static const unsigned char uncommitted[WT_JOURNAL_COMMIT_SIZE] = { 0, 0, 0, 0 };
    WtFlashAccess *access = arg;
    unsigned char page[WT_FLASH_PAGE_SIZE];
    u32 address, offset;
    int result = -1;
    s32 lockID;
    if (!access || !record || bank < 0 || bank > 1) return -1;
    address = gWtGameConfig.journalBank[bank];
    lockID = OS_GetLockID();
    if (lockID == OS_LOCK_ID_ERROR) { access->error = LOCK_FAILED; return -1; }
    WtGame_HoldAwake(TRUE);
    CARD_LockBackup(lockID);
    if (!Supported()) { access->error = UNSUPPORTED_FLASH; goto done; }
    if (!CARD_WriteAndVerifyFlash(address + WT_REC_COMMIT, uncommitted, sizeof(uncommitted))
        || !CARD_WriteAndVerifyFlash(address, record, WT_REC_COMMIT)
        || !CARD_WriteAndVerifyFlash(address + WT_REC_COMMIT, record + WT_REC_COMMIT, WT_JOURNAL_COMMIT_SIZE)) goto done;
    for (offset = 0; offset < WT_JOURNAL_RECORD_SIZE; offset += sizeof(page)) {
        if (!CARD_ReadFlash(address + offset, page, sizeof(page)) || memcmp(page, record + offset, sizeof(page)) != 0) goto done;
    }
    result = 0;
done:
    CARD_UnlockBackup(lockID);
    OS_ReleaseLockID(lockID);
    WtGame_HoldAwake(FALSE);
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
    if (Supported()
        && CARD_ReadFlash(address, bytes, length)) result = 0;
    CARD_UnlockBackup(lockID);
    OS_ReleaseLockID(lockID);
    return result;
}
