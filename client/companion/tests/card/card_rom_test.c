#include "sim_card.h"
#include "game/portrait.h"
#include "../posix_reader.h"
#include "../synthetic_rom.h"

#include <stdio.h>
#include <unistd.h>

static void Compare(const CpRomFile *file, const CpRomFile *card, uint32_t at, uint32_t length)
{
    uint8_t a[8191], b[8191];
    while (length) {
        unsigned bytes = length < sizeof a ? length : sizeof a;
        assert(CpRomFile_Read(file, at, a, bytes) == CP_OK);
        assert(CpRomFile_Read(card, at, b, bytes) == CP_OK);
        assert(memcmp(a, b, bytes) == 0);
        at += bytes;
        length -= bytes;
    }
}

static void CompareArchive(const CpNarc *file, const CpNarc *card)
{
    assert(file->file.offset == card->file.offset && file->file.size == card->file.size);
    Compare(&file->file, &card->file, 0, file->file.size);
}

/* An already initialized card, with neither the cold-boot keys nor a reset
 * available. Compare the exact header slices and every archive the app uses. */
static void CheckHandoff(const CpReader *image, int catalogReference)
{
    CpCardRom card;
    CpReader reader = {0};
    SimCard sim;
    SimCard_Init(&sim, image, 0xc001ffc2);
    sim.mode = 2;
    sim.bus.loadKey = NULL;
    sim.bus.reset = NULL;
    if (!catalogReference) {
        assert(CpCardRom_OpenHandoff(&card, &sim.bus, 0, &reader) < 0 && !card.active);
        return;
    }
    uint8_t byte;
    for (unsigned pass = 0; pass < 2; ++pass) {
        sim.twl = pass && sim.twlStart;
        sim.id = pass ? 0x80007fc2 : 0xc001ffc2;
        uint32_t control = pass ? 0x20416657 : 0x20000000;
        assert(CpCardRom_OpenHandoff(&card, &sim.bus, control, &reader) == 0);
        assert(card.active && card.inherited && sim.resets == 0);
        CpRomFile actual, reference;
        CpRomFile_Whole(&reader, &actual);
        CpRomFile_Whole(image, &reference);
        Compare(&reference, &actual, 0, 32);
        Compare(&reference, &actual, 0x40, 16);
        assert(reader.readAt(&card, 0x4000, &byte, 1) < 0);
        assert(reader.readAt(&card, 0x1f, card.page, 2) < 0);
        assert(reader.readAt(&card, actual.size, &byte, 1) < 0);
        CpGame a, b;
        assert(CpGame_Open(image, &a) == CP_OK && CpGame_Open(&reader, &b) == CP_OK);
        assert(a.rom.buildCode == b.rom.buildCode);
        CompareArchive(&a.personal, &b.personal);
        CompareArchive(&a.growth, &b.growth);
        CompareArchive(&a.evolution, &b.evolution);
        CompareArchive(&a.items, &b.items);
        CompareArchive(&a.moves, &b.moves);
        CompareArchive(&a.messages, &b.messages);
        CompareArchive(&a.font, &b.font);
        CpPortrait expected = {0}, actualPortrait = {0};
        assert(CpPortrait_Load(&a, 25, 0, 0, 0, 0, &expected) == CP_OK && expected.ready);
        assert(CpPortrait_Load(&b, 25, 0, 0, 0, 0, &actualPortrait) == CP_OK && actualPortrait.ready);
        assert(!memcmp(&expected, &actualPortrait, sizeof(expected)));
    }
    sim.present = 0;
    assert(CpCardRom_Check(&card) < 0 && !card.active);
    sim.present = 1;
    assert(reader.readAt(&card, 0, &byte, 1) < 0);
    uint8_t fatOffset[4];
    assert(image->readAt(image->context, 0x48, fatOffset, sizeof(fatOffset)) == 0);
    sim.corruptPage = CpLe32(fatOffset) & ~(uint32_t)(CP_CARD_PAGE - 1);
    assert(CpCardRom_OpenHandoff(&card, &sim.bus, 0, &reader) < 0 && !card.active);
    sim.corruptPage = 0;
    sim.id = UINT32_MAX;
    assert(CpCardRom_OpenHandoff(&card, &sim.bus, 0, &reader) < 0);
    puts("Handoff: portrait, seven archives and identity matched without keys or resets; removal invalidated reader");
}

static void CheckImage(const char *path, uint32_t id, int faults)
{
    PosixReader file;
    CpCardRom card;
    CpReader reader;
    SimCard sim;
    CpRomFile left, right;
    CpGame a, b;
    assert(PosixReader_Open(&file, path) == 0);
    CheckHandoff(&file.reader, !faults);
    SimCard_Init(&sim, &file.reader, id);
    assert(CpCardRom_Open(&card, &sim.bus, &reader) == 0);
    assert(card.id == id && sim.resets == (card.twlStart ? 2u : 1u));
    assert(CpGame_Open(&file.reader, &a) == CP_OK && CpGame_Open(&reader, &b) == CP_OK);
    assert(a.rom.buildCode == b.rom.buildCode);
    CompareArchive(&a.personal, &b.personal);
    CompareArchive(&a.growth, &b.growth);
    CompareArchive(&a.evolution, &b.evolution);
    CompareArchive(&a.items, &b.items);
    CompareArchive(&a.moves, &b.moves);
    CompareArchive(&a.messages, &b.messages);
    CompareArchive(&a.font, &b.font);
    CpRomFile_Whole(&file.reader, &left);
    CpRomFile_Whole(&reader, &right);
    assert(left.size == right.size);
    Compare(&left, &right, 1, 0x9000);
    if (card.twlStart) Compare(&left, &right, card.twlStart - 0x3001, 0x8000);
    Compare(&left, &right, left.size - 513, 513);
    assert(reader.readAt(&card, left.size, NULL, 0) == 0);
    uint8_t byte;
    assert(reader.readAt(&card, left.size, &byte, 1) < 0);
    assert(reader.readAt(&card, UINT32_MAX, &byte, 2) < 0);

    if (faults) {
        CpCardRom other;
        CpReader otherReader;
        assert(CpCardRom_Open(&other, &sim.bus, &otherReader) == 0);
        assert(reader.readAt(&card, 0, &byte, 1) < 0);
        CpCardRom_Close(&other);
        assert(CpCardRom_Open(&card, &sim.bus, &reader) == 0);
        sim.present = 0;
        assert(reader.readAt(&card, 0, &byte, 1) < 0);
        sim.present = 1;
        assert(reader.readAt(&card, 0, &byte, 1) < 0 && reader.size(&card) == 0);

        assert(CpCardRom_Open(&card, &sim.bus, &reader) == 0);
        ++sim.id;
        assert(CpCardRom_Check(&card) < 0);
        --sim.id;
        assert(CpCardRom_Check(&card) < 0);

        assert(CpCardRom_Open(&card, &sim.bus, &reader) == 0);
        sim.failPage = 1;
        assert(reader.readAt(&card, 0x8001, &byte, 1) < 0);
        sim.failPage = 0;
        assert(reader.readAt(&card, 0, &byte, 1) < 0);

        if (sim.twlStart) {
            sim.resets = 0;
            sim.changeHeader = 1;
            assert(CpCardRom_Open(&card, &sim.bus, &reader) < 0);
        }
        sim.present = 0;
        assert(CpCardRom_Open(&card, &sim.bus, &reader) < 0);
    }
    CpCardRom_Close(&card);
    assert(CpCardRom_Check(&card) < 0);
    PosixReader_Close(&file);
    printf("%.4s rev %u: 7 archives and ROM boundary reads match\n", a.rom.gameCode, a.rom.revision);
}

/* The existing CI fixture packs its FNT directly after the short header.
 * Give it the physical cartridge layout, relocating only absolute offsets. */
static void CheckSynthetic(const char *code, int twl, uint32_t id)
{
    Bytes rom = twl ? SyntheticGen5(code, 0) : SyntheticGame(code, 0);
    const unsigned delta = 0x8000 - CP_CARD_PAGE;
    unsigned exponent = 0;
    while ((0x20000u << exponent) < rom.size + delta + 0x10000) ++exponent;
    uint32_t capacity = 0x20000u << exponent;
    Reserve(&rom, capacity);
    memmove(rom.data + 0x8000, rom.data + CP_CARD_PAGE, rom.size - CP_CARD_PAGE);
    memset(rom.data + CP_CARD_PAGE, 0, delta);
    memset(rom.data + rom.size + delta, 0xfd, capacity - rom.size - delta);
    rom.size = capacity;
    rom.data[0x14] = exponent;
    rom.data[0x12] = twl ? 2 : 0;
    if (twl) Set32(&rom, 0x1c0, capacity - 0x8000);
    memcpy(rom.data + 0x4000, "\xff\xde\xff\xe7\xff\xde\xff\xe7", 8);
    Set32(&rom, 0x40, CpLe32(rom.data + 0x40) + delta);
    uint32_t fat = CpLe32(rom.data + 0x48) + delta;
    Set32(&rom, 0x48, fat);
    for (unsigned i = 0; i < CpLe32(rom.data + 0x4c); i += 4)
        Set32(&rom, fat + i, CpLe32(rom.data + fat + i) + delta);

    char path[] = "/tmp/dswt-card-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "wb");
    assert(file && fwrite(rom.data, 1, rom.size, file) == rom.size);
    assert(fclose(file) == 0);
    free(rom.data);
    CheckImage(path, id, 1);
    assert(unlink(path) == 0);
}

/* Independent known answers from the OpenSSL BF_encrypt primitive, using
 * the synthetic table below and the GBATEK key schedule for game CPUE. */
static void CheckKeySchedule(void)
{
    static const uint64_t expected[] = {
        UINT64_C(0x1f157ec5a89a5f23), UINT64_C(0x4b07ae25a8ef6415), UINT64_C(0x54595ff08b4a1b06)
    };
    uint64_t plain = UINT64_C(0x1000000000000001);
    for (unsigned level = 1; level <= 3; ++level) {
        uint32_t key[CP_CARD_KEY_WORDS];
        for (unsigned i = 0; i < CP_CARD_KEY_WORDS; ++i) key[i] = 0xabcdef01u * (i + 1);
        CpCardKey_Init(key, CpLe32((const uint8_t *)"CPUE"), level);
        assert(CpCardKey_Crypt(key, plain, 0) == expected[level - 1]);
        assert(CpCardKey_Crypt(key, expected[level - 1], 1) == plain);
    }
}

int main(int argc, char **argv)
{
    CheckKeySchedule();
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) CheckImage(argv[i], 0x80007fc2, 0);
        printf("%d owned images passed\n", argc - 1);
        return 0;
    }
    static const char *const codes[] = { "ADAE", "APAJ", "CPUE", "IPKE", "IPGK", "IRBO", "IRAO", "IREO", "IRDO" };
    for (unsigned i = 0; i < sizeof codes / sizeof *codes; ++i)
        CheckSynthetic(codes[i], i >= 5, i % 2 ? 0x80007fc2 : 0x00003fc2);
    return 0;
}
