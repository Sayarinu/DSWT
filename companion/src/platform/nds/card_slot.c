#include "platform/nds/card_rom.h"

#include <nds.h>
#include <stdio.h>
#include <string.h>

/* GBATEK, SCFG_MC (DSi cartridge power), ROMCTRL and Gamecard Bus.
 * Only ROM transfers are enabled; AUXSPIDATA is never touched. */
int CpNdsSlot_Present(void)
{
    return isDSiMode() && !(REG_SCFG_MC & SCFG_MC_EJECTED);
}

static int Powered(void)
{
    return (REG_SCFG_MC & (SCFG_MC_PWR_MASK | SCFG_MC_EJECTED)) == SCFG_MC_PWR_ON;
}

static int WaitPower(unsigned state)
{
    uint64_t end = systemCounterGetTicks() + systemCounterMsecsToTicks(1200);
    do {
        if (!CpNdsSlot_Present()) return -1;
        if ((REG_SCFG_MC & SCFG_MC_PWR_MASK) == state) return 0;
    } while (systemCounterGetTicks() < end);
    return -1;
}

static void Delay(void *context, unsigned us)
{
    (void)context;
    uint64_t end = systemCounterGetTicks() + systemCounterUsecsToTicks(us) + 1;
    while (systemCounterGetTicks() < end) { }
}

static int Reset(void *context)
{
    if (!CpNdsSlot_Present()) return -1;
    sysSetCardOwner(true);
    unsigned state = REG_SCFG_MC & SCFG_MC_PWR_MASK;
    if (state != SCFG_MC_PWR_OFF && state != SCFG_MC_PWR_REQUEST_OFF &&
        !fifoSendValue32(FIFO_USER_01, SCFG_MC_PWR_REQUEST_OFF)) return -1;
    if (WaitPower(SCFG_MC_PWR_OFF) != 0) return -1;
    Delay(context, 1000);
    if (!fifoSendValue32(FIFO_USER_01, SCFG_MC_PWR_RESET) || WaitPower(SCFG_MC_PWR_RESET) != 0) return -1;
    Delay(context, 10000);
    if (!fifoSendValue32(FIFO_USER_01, SCFG_MC_PWR_ON) || WaitPower(SCFG_MC_PWR_ON) != 0) return -1;
    Delay(context, 27000);
    REG_AUXSPICNT = CARD_ENABLE;
    REG_ROMCTRL = CARD_nRESET;
    Delay(context, 270000);
    return Powered() ? 0 : -1;
}

static int Transfer(void *context, uint64_t command, uint32_t flags, void *out, unsigned bytes)
{
    unsigned length, done = 0;
    uint8_t *dest = out;
    (void)context;
    switch (bytes) {
    case 0: length = 0; break;
    case 4: length = 7; break;
    case CP_CARD_PAGE: length = 1; break;
    case 4096: length = 4; break;
    case 8192: length = 5; break;
    default: return -1;
    }
    if (!Powered() || (REG_ROMCTRL & CARD_BUSY)) return -1;
    for (unsigned i = 0; i < 8; ++i) REG_CARD_COMMAND[i] = command >> (56 - 8 * i);
    uint64_t end = systemCounterGetTicks() + systemCounterMsecsToTicks(250);
    REG_ROMCTRL = CARD_ACTIVATE | CARD_nRESET | CARD_BLK_SIZE(length) | (flags & ~CARD_WR);
    do {
        uint32_t status = REG_ROMCTRL;
        if (status & CARD_DATA_READY) {
            uint32_t word = REG_CARD_DATA_RD;
            if (done >= bytes) return -1;
            if (dest) memcpy(dest + done, &word, sizeof word);
            done += 4;
        }
        if (!(status & CARD_BUSY)) return done == bytes && Powered() ? 0 : -1;
    } while (Powered() && systemCounterGetTicks() < end);
    return -1;
}

static void Seed(void *context, uint64_t seed0)
{
    (void)context;
    REG_CARD_1B0 = seed0;
    REG_CARD_1B8 = seed0 >> 32;
    REG_CARD_1B4 = 0x879b9b05u;
    REG_CARD_1BA = 0x5c;
    REG_ROMCTRL = CARD_nRESET | CARD_SEC_SEED | CARD_SEC_EN | CARD_SEC_DAT;
}

static int ReadTable(const char *path, long offset, uint32_t *table)
{
    /* GBATEK, Initial Encryption Values. Read just the required table from
     * the owner's BIOS dump on SD. Neither protected BIOS is mapped here. */
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    int ok = fseek(file, offset, SEEK_SET) == 0 &&
        fread(table, sizeof(*table), CP_CARD_KEY_WORDS, file) == CP_CARD_KEY_WORDS;
    fclose(file);
    return ok ? 0 : -1;
}

static int LoadKey(void *context, int twl, uint32_t *table)
{
    (void)context;
    if (twl) return ReadTable("sd:/dswt-companion/bios7i.bin", 0xc6d0, table);
    if (ReadTable("sd:/dswt-companion/bios7.bin", 0x30, table) == 0) return 0;
    return ReadTable("sd:/dswt-companion/bios9i.bin", 0x99a0, table);
}

int CpNdsSlot_Open(CpCardRom *card, CpReader *reader)
{
    static CpCardBus bus = { NULL, Reset, Transfer, Seed, Delay, LoadKey, 0 };
    return CpCardRom_Open(card, &bus, reader);
}
