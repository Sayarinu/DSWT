/* LUNE-881: the Black/White and Black 2/White 2 journal banks in the musical
 * distribution save (protocol/save-layout-bw.json, -b2w2 journal_banks):
 * detection, a musical's leftovers erased before the journal and never
 * written over, a cut erase that repeats, and the Generation V journal. */
#include "core/journal_family.h"
#include "save/journal_host_io.h"
#include "save/journal_save.h"
#include "save/musical_area.h"
#include "save/save_image.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char bytes[CP_SAVE_FILE_SIZE];
static unsigned writes, failWrite;

static int Read(void *arg, uint32_t offset, void *data, size_t size)
{
    (void)arg;
    if (offset > sizeof(bytes) || size > sizeof(bytes) - offset) return -1;
    memcpy(data, bytes + offset, size);
    return 0;
}

static int Write(void *arg, uint32_t offset, const void *data, size_t size)
{
    (void)arg;
    if (++writes == failWrite) {
        memcpy(bytes + offset, data, size / 2);
        return -1;
    }
    memcpy(bytes + offset, data, size);
    return 0;
}

static int Sync(void *arg) { (void)arg; return 0; }
static int Size(void *arg, uint64_t *size) { (void)arg; *size = sizeof(bytes); return 0; }
static CpFile file = { NULL, Read, Write, Sync, Size };

static const unsigned char seed[32] = { 9, 8, 7, 6, 5, 4, 3, 2, 1 };

static void Game(const char *name, const CpSaveLayout *layout, uint32_t first, uint32_t second)
{
    const CpJournalArea *area = CpSaveJournal_Area(layout);
    CpJournalHostStore host;
    WtJournalIO io = CpJournalHostIOArea(&host, &file, area);
    WtJournal journal;
    WtJournalEntry entry;
    unsigned cut, blank;
    assert(area->bank[0] == first && area->bank[1] == second && area->reserved == 0x1000);
    CpCore_SelectFamily(CP_CORE_GEN5);
    /* Blank banks, 0x00 or 0xFF, with anything around them. */
    for (blank = 0; blank < 2; ++blank) {
        memset(bytes, 0x5A, sizeof(bytes));
        memset(bytes + first, blank ? 0xFF : 0, 0x1000);
        memset(bytes + second, blank ? 0xFF : 0, 0x1000);
        writes = failWrite = 0;
        assert(CpMusicalArea_Inspect(&file, area) == CP_MUSICAL_FREE);
        assert(CpSaveJournal_DetectArea(&file, area) == CP_SAVE_JOURNAL_UNUSED && writes == 0);
        assert(CpSaveJournal_ProvisionArea(&file, area, &journal, io, seed) == 0);
        assert(memcmp(bytes + first, "DSWJ", 4) == 0 && memcmp(bytes + second, "DSWJ", 4) == 0);
        /* Nothing outside the banks changed. */
        assert(bytes[first - 1] == 0x5A && bytes[second + 0x1000] == 0x5A);
        assert(CpMusicalArea_Inspect(&file, area) == CP_MUSICAL_JOURNAL);
        assert(CpSaveJournal_DetectArea(&file, area) == CP_SAVE_JOURNAL_HAS_JOURNAL);
        assert(WtJournal_Open(&journal, io) == 0 && WtJournal_Read(&journal, &entry) == 0);
        assert(entry.phase == WT_JOURNAL_EMPTY);
        WtJournal_Close(&journal);
        /* A journal is never erased, and the Generation IV banks are elsewhere. */
        assert(CpMusicalArea_Erase(&file, area) == 0 && memcmp(bytes + first, "DSWJ", 4) == 0);
        assert(CpSaveJournal_DetectArea(&file, NULL) != CP_SAVE_JOURNAL_HAS_JOURNAL);
    }
    /* A musical's leftovers: unused, never provisioned over, erased first;
     * an erase cut at any write leaves leftovers or blank banks, and repeats. */
    for (cut = 0; cut <= 2; ++cut) {
        unsigned i;
        memset(bytes, 0x5A, sizeof(bytes));
        for (i = 0; i < 0x2000; ++i) bytes[first + i] = (unsigned char)(i * 13 + 1);
        writes = 0; failWrite = cut;
        assert(CpMusicalArea_Inspect(&file, area) == CP_MUSICAL_LEFTOVER);
        assert(CpSaveJournal_DetectArea(&file, area) == CP_SAVE_JOURNAL_UNUSED);
        assert(CpSaveJournal_ProvisionArea(&file, area, &journal, io, seed) != 0 && writes == 0);
        if (cut) {
            assert(CpMusicalArea_Erase(&file, area) != 0);
            assert(CpMusicalArea_Inspect(&file, area) != CP_MUSICAL_JOURNAL);
            writes = failWrite = 0;
        }
        assert(CpMusicalArea_Erase(&file, area) == 0);
        assert(CpMusicalArea_Inspect(&file, area) == CP_MUSICAL_FREE);
        assert(bytes[first - 1] == 0x5A && bytes[second + 0x1000] == 0x5A);
        assert(CpSaveJournal_ProvisionArea(&file, area, &journal, io, seed) == 0);
        assert(CpSaveJournal_DetectArea(&file, area) == CP_SAVE_JOURNAL_HAS_JOURNAL);
    }
    /* A bank starting with the magic but holding no valid record is refused. */
    memset(bytes + first + 8, 0x33, 64);
    memset(bytes + second + 8, 0x33, 64);
    assert(CpSaveJournal_DetectArea(&file, area) == CP_SAVE_JOURNAL_REFUSE);
    CpCore_SelectFamily(CP_CORE_GEN4);
    printf("PASS %s journal banks at 0x%X and 0x%X: blank banks provision, leftovers are erased first, a cut erase repeats\n",
           name, (unsigned)first, (unsigned)second);
}

int main(void)
{
    Game("Black/White", &CpSaveLayout_BlackWhite, 0x74000, 0x75000);
    Game("Black 2/White 2", &CpSaveLayout_Black2White2, 0x6B000, 0x6C000);
    assert(CpSaveJournal_Area(&CpSaveLayout_Platinum) == &CpJournalArea_Gen4);
    return 0;
}
