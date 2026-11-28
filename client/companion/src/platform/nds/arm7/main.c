/* SPDX-License-Identifier: Zlib
 * Adapted from the BlocksDS v1.25.0 DSWiFi ARM7 core for bounded Slot-1
 * power transitions. Upstream notice in licensing/ds-companion-blocksds.txt.
 * Copyright (C) 2005 Michael Noland, Jason Rogers
 * Copyright (C) 2005-2015 Dave Murphy
 * Copyright (C) 2023 Antonio Niño Díaz
 */
#include <nds.h>
#include <nds/arm7/i2c.h>
#include <dswifi7.h>
#include "music/fss7.h"
#include "game/gts_music.h"
#include "platform/nds/menu_audio.h"

static int musicPlayer = -1;

static bool menuPlaying;
static const int8_t *menuPcm;
static unsigned menuBytes;
static const CpGtsMusic *volatile pendingMusic;

static void Music(void *address, void *context)
{
    (void)context;
    /* ARM9 holds the descriptor and buffers until the switch is acknowledged. */
    pendingMusic = address;
}

static void SwitchMusic(const CpGtsMusic *song)
{
    if (menuPlaying) {
        SCHANNEL_CR(0) = 0;
        menuPlaying = false;
    }
    if (musicPlayer >= 0) {
        timerStop(LIBNDS_DEFAULT_TIMER_MUSIC);
        Player_Free(musicPlayer);
        musicPlayer = -1;
    }
    if (song->sequence) {
        musicPlayer = Player_Alloc(64);
        if (musicPlayer >= 0) {
            if (Player_Setup(musicPlayer, song->sequence, song->bank, song->waves)) {
                Snd_Init();
                Player_Play(musicPlayer);
            } else {
                Player_Free(musicPlayer);
                musicPlayer = -1;
            }
        }
    }
    if (song->menuPcm) {
        menuPcm = song->menuPcm;
        menuBytes = song->bytes;
    }
    if (musicPlayer < 0 && menuPcm && (song->sequence || song->menuPcm)) {
        SCHANNEL_SOURCE(0) = (uint32_t)menuPcm;
        SCHANNEL_TIMER(0) = SOUND_FREQ(CP_MENU_SAMPLE_RATE);
        SCHANNEL_REPEAT_POINT(0) = 0;
        SCHANNEL_LENGTH(0) = menuBytes / 4;
        SCHANNEL_CR(0) = SCHANNEL_ENABLE | SOUND_FORMAT_8BIT | SOUND_REPEAT | SOUND_VOL(96) | SOUND_PAN(64);
        menuPlaying = true;
    }
}

static void MusicTick(void)
{
    enum { FADE_FRAMES = 30 };
    static unsigned level;
    const CpGtsMusic *next = pendingMusic;
    unsigned volume = musicPlayer >= 0 ? 84 : 112;
    if (next && next->menuPcm && menuPlaying) {
        pendingMusic = NULL;
        fifoSendValue32(FIFO_USER_03, 1);
        return;
    }
    if (next) {
        if (level) --level;
        REG_MASTER_VOLUME = level * volume / FADE_FRAMES;
        if (!level) {
            SwitchMusic(next);
            pendingMusic = NULL;
            fifoSendValue32(FIFO_USER_03, musicPlayer >= 0 || (next->menuPcm && menuPlaying));
        }
    } else if ((musicPlayer >= 0 || menuPlaying) && level < FADE_FRAMES) {
        REG_MASTER_VOLUME = ++level * volume / FADE_FRAMES;
    }
}

static volatile bool exiting;

static void PowerButton(void)
{
    exiting = true;
}

static void VBlank(void)
{
    MusicTick();
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
    fifoSetAddressHandler(FIFO_USER_03, Music, NULL);
    installSystemFIFO();
    if (isDSiMode()) {
        installCameraFIFO();
        fifoSetValue32Handler(FIFO_USER_01, SlotPower, NULL);
    }
    setPowerButtonCB(PowerButton);
    initClockIRQTimer(LIBNDS_DEFAULT_TIMER_RTC);
    irqSet(IRQ_VBLANK, VBlank);
    irqEnable(IRQ_VBLANK);
    while (!exiting) {
        if (isDSiMode() && fifoCheckValue32(FIFO_USER_02) && fifoGetValue32(FIFO_USER_02) == 1) {
            /* Keep the 3DS in TWL mode while the launcher title restarts. */
            u8 mode = i2cReadRegister(I2C_PM, I2CREGPM_MMCPWR);
            i2cWriteRegister(I2C_PM, I2CREGPM_MMCPWR, mode | 1);
            systemReboot();
        }
        swiWaitForVBlank();
    }
    return 0;
}
