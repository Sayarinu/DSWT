#include "journal_save.h"
#include "musical_area.h"
#include "save_image.h"
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

const CpJournalArea *CpSaveJournal_Area(const CpSaveLayout *layout)
{
    if (layout == &CpSaveLayout_BlackWhite) return &CpJournalArea_BlackWhite;
    if (layout == &CpSaveLayout_Black2White2) return &CpJournalArea_Black2White2;
    return &CpJournalArea_Gen4;
}

static int Gen5(const CpJournalArea *area)
{
    return area == &CpJournalArea_BlackWhite || area == &CpJournalArea_Black2White2;
}

CpSaveJournalDetection CpSaveJournal_Detect(const CpFile *file)
{
    return CpSaveJournal_DetectArea(file, NULL);
}

CpSaveJournalDetection CpSaveJournal_DetectArea(const CpFile *file, const CpJournalArea *area)
{
    WtJournal journal;
    CpPlatformFile platform = { (void *)file, Read, NULL, NULL, Size, NULL };
    CpJournalStore store = { &platform, NULL, 0, CpSaveJournal_Hash, area };
    WtJournalIO io = { &store, CpJournal_ReadBank, NoWrite, CpSaveJournal_Hash };
    int result;
    if (!file || !file->readAt || !file->size) return CP_SAVE_JOURNAL_REFUSE;
    if (Gen5(area)) {
        int state = CpMusicalArea_Inspect(file, area);
        if (state < 0) return CP_SAVE_JOURNAL_REFUSE;
        /* Nothing a journal wrote: the patched game too erases it first. */
        if (state == CP_MUSICAL_LEFTOVER) return CP_SAVE_JOURNAL_UNUSED;
    }
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
    return CpSaveJournal_ProvisionArea(file, NULL, journal, io, seed);
}

int CpSaveJournal_ProvisionArea(const CpFile *file, const CpJournalArea *area, WtJournal *journal, WtJournalIO io,
                                const unsigned char seed[32])
{
    if (!journal) return -1;
    WtJournal_Close(journal);
    if (!seed || CpSaveJournal_DetectArea(file, area) != CP_SAVE_JOURNAL_UNUSED) return -1;
    /* A musical's leftovers are erased first, never written over. */
    if (Gen5(area) && CpMusicalArea_Inspect(file, area) != CP_MUSICAL_FREE) return -1;
    return WtJournal_Initialize(journal, io, seed);
}
