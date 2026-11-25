#include "journal_save.h"
#include "sha256.h"
#include "platform/journal_io.h"

int CpSaveJournal_Hash(const unsigned char *data, size_t size, unsigned char digest[32])
{
    CpSaveSha256 hash;
    CpSaveSha256_Init(&hash);
    CpSaveSha256_Update(&hash, data, size);
    CpSaveSha256_Finish(&hash, digest);
    return 0;
}

static int Read(void *context, uint32_t offset, void *bytes, size_t size)
{
    const CpFile *file = context;
    return file->readAt(file->context, offset, bytes, size);
}

static int Size(void *context, uint32_t *size)
{
    const CpFile *file = context;
    uint64_t length;
    if (file->size(file->context, &length) != 0 || length > UINT32_MAX) return -1;
    *size = (uint32_t)length;
    return 0;
}

static int NoWrite(void *context, int bank, const unsigned char *record)
{
    (void)context; (void)bank; (void)record;
    return -1;
}

CpSaveJournalDetection CpSaveJournal_Detect(const CpFile *file)
{
    WtJournal journal;
    CpPlatformFile platform = { (void *)file, Read, NULL, NULL, Size, NULL };
    CpJournalStore store = { &platform, NULL, 0, CpSaveJournal_Hash };
    WtJournalIO io = { &store, CpJournal_ReadBank, NoWrite, CpSaveJournal_Hash };
    int result;
    if (!file || !file->readAt || !file->size) return CP_SAVE_JOURNAL_REFUSE;
    result = WtJournal_Open(&journal, io);
    if (result == WT_JOURNAL_ERROR_READ || result == WT_JOURNAL_ERROR_SELF_TEST) return CP_SAVE_JOURNAL_REFUSE;
    if (result == 0) {
        WtJournal_Close(&journal);
        return CP_SAVE_JOURNAL_HAS_JOURNAL;
    }
    /* A cut first enrollment write reads as blank (CpJournal_ReadBank).
     * Initialize(NULL) checks WtJournal_Unused without writing: like the game,
     * a cut first write can be erased and initialized with fresh entropy. */
    return WtJournal_Initialize(&journal, io, NULL) == WT_JOURNAL_NEEDS_SEED
        ? CP_SAVE_JOURNAL_UNUSED : CP_SAVE_JOURNAL_REFUSE;
}

int CpSaveJournal_Provision(const CpFile *file, WtJournal *journal, WtJournalIO io,
                        const unsigned char seed[32])
{
    if (!journal) return -1;
    WtJournal_Close(journal);
    if (!seed || CpSaveJournal_Detect(file) != CP_SAVE_JOURNAL_UNUSED) return -1;
    return WtJournal_Initialize(journal, io, seed);
}
