/* SPDX-License-Identifier: Zlib
 * Adapted from the BlocksDS v1.25.0 DSWiFi ARM7 core for bounded Slot-1
 * power transitions. Upstream notice in licensing/ds-companion-blocksds.txt.
 * Copyright (C) 2005 Michael Noland, Jason Rogers
 * Copyright (C) 2005-2015 Dave Murphy
 * Copyright (C) 2023 Antonio Niño Díaz
 */
#include <nds.h>
#include <dswifi7.h>

static volatile bool exiting;

static void PowerButton(void)
{
    exiting = true;
}

static void VBlank(void)
{
    inputGetAndSend();
    Wifi_Update();
}

/* SCFG_MC is writable only on ARM7. ARM9 times each transition and its
 * settling delay. Never wait for the cartridge inside an ARM7 interrupt. */
static void SlotPower(u32 state, void *context)
{
    (void)context;
    u16 status = REG_SCFG_MC;
    if (status & SCFG_MC_EJECTED) return;
    switch (state) {
    case SCFG_MC_PWR_REQUEST_OFF:
    case SCFG_MC_PWR_RESET:
    case SCFG_MC_PWR_ON:
        REG_SCFG_MC = (status & ~SCFG_MC_PWR_MASK) | state;
        break;
    }
}

int main(void)
{
    enableSound();
    readUserSettings();
    ledBlink(0);
    touchInit();
    irqInit();
    fifoInit();
    installWifiFIFO();
    installSoundFIFO();
    installSystemFIFO();
    if (isDSiMode()) {
        installCameraFIFO();
        fifoSetValue32Handler(FIFO_USER_01, SlotPower, NULL);
    }
    setPowerButtonCB(PowerButton);
    initClockIRQTimer(LIBNDS_DEFAULT_TIMER_RTC);
    irqSet(IRQ_VBLANK, VBlank);
    irqEnable(IRQ_VBLANK);
    while (!exiting && (REG_KEYINPUT & (KEY_SELECT | KEY_START | KEY_L | KEY_R)))
        swiWaitForVBlank();
    return 0;
}
