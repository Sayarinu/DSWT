#include "journal_save.h"
#include "sha256.h"

static const uint32_t banks[2] = { 0x30000, 0x70000 };

int CpJournal_Hash(const unsigned char *data, size_t size, unsigned char digest[32])
{
    CpSaveSha256 hash;
    CpSaveSha256_Init(&hash);
    CpSaveSha256_Update(&hash, data, size);
    CpSaveSha256_Finish(&hash, digest);
    return 0;
}

static int Read(void *context, int bank, unsigned char *record)
{
    const CpFile *file = context;
    if (bank < 0 || bank > 1) return -1;
    return file->readAt(file->context, banks[bank], record, WT_JOURNAL_RECORD_SIZE);
}

static int NoWrite(void *context, int bank, const unsigned char *record)
{
    (void)context; (void)bank; (void)record;
    return -1;
}

CpJournalDetection CpJournal_Detect(const CpFile *file)
{
    unsigned char chunk[512];
    WtJournal journal;
    WtJournalIO io = { (void *)file, Read, NoWrite, CpJournal_Hash };
    uint64_t size;
    unsigned bank, offset, i;
    int allBlank = 1, result;
    if (!file || !file->readAt || !file->size
        || file->size(file->context, &size) != 0 || size != 0x80000) return CP_JOURNAL_REFUSE;
    for (bank = 0; bank < 2; ++bank) {
        int blank = 1, onlyBlankBytes = 1;
        unsigned char value = 0, tail = 0;
        for (offset = 0; offset < 0x10000; offset += sizeof(chunk)) {
            if (file->readAt(file->context, banks[bank] + offset, chunk, sizeof(chunk)) != 0)
                return CP_JOURNAL_REFUSE;
            if (offset == 0) {
                value = chunk[0];
                if (value != 0 && value != 255) blank = 0;
            }
            if (offset == WT_JOURNAL_RECORD_SIZE) tail = chunk[0];
            for (i = 0; i < sizeof(chunk); ++i) {
                if (chunk[i] != value) blank = 0;
                if (offset < WT_JOURNAL_RECORD_SIZE) {
                    if (chunk[i] != 0 && chunk[i] != 255) onlyBlankBytes = 0;
                } else if ((tail != 0 && tail != 255) || chunk[i] != tail) return CP_JOURNAL_REFUSE;
            }
        }
        /* A mixture of erase values is not a uniform unused flash area. */
        if (!blank && onlyBlankBytes) return CP_JOURNAL_REFUSE;
        if (!blank) allBlank = 0;
    }
    if (allBlank) return CP_JOURNAL_UNMODIFIED_NEVER_USED;
    result = WtJournal_Open(&journal, io);
    if (result == WT_JOURNAL_ERROR_READ || result == WT_JOURNAL_ERROR_SELF_TEST) return CP_JOURNAL_REFUSE;
    if (result == 0) {
        WtJournal_Close(&journal);
        return CP_JOURNAL_HAS_JOURNAL;
    }
    /* The core also recognizes an interrupted first EMPTY publication, when
     * neither mirror has a committed record for Open to select yet. */
    return WtJournal_Initialize(&journal, io, NULL) == WT_JOURNAL_NEEDS_SEED
        ? CP_JOURNAL_HAS_JOURNAL : CP_JOURNAL_REFUSE;
}

int CpJournal_Provision(const CpFile *file, WtJournal *journal, WtJournalIO io,
                        const unsigned char seed[32])
{
    if (!journal) return -1;
    WtJournal_Close(journal);
    if (!seed || CpJournal_Detect(file) != CP_JOURNAL_UNMODIFIED_NEVER_USED) return -1;
    return WtJournal_Initialize(journal, io, seed);
}
