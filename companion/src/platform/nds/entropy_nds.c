#include "platform/nds/entropy_nds.h"

#include <string.h>
#include <time.h>

#include <nds.h>

#include "platform/entropy_pool.h"
#include "platform/nds/wifi.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/game_adapter.h"

#define MIC_BYTES 2048

static CpEntropyBoot sBoot;
static int sMicrophone;
static unsigned sBytes;
static uint16_t sMic[MIC_BYTES / 2] __attribute__((aligned(32)));

typedef struct Timing {
    uint64_t ticks;
    uint16_t timers[4], vcount;
} Timing;

static void ReadTiming(Timing *timing)
{
    timing->ticks = systemCounterGetTicks();
    timing->timers[0] = TIMER_DATA(0);
    timing->timers[1] = TIMER_DATA(1);
    timing->timers[2] = TIMER_DATA(2);
    timing->timers[3] = TIMER_DATA(3);
    timing->vcount = REG_VCOUNT;
}

void CpNdsEntropy_Jitter(void)
{
    Timing timing;
    ReadTiming(&timing);
    CpEntropyBoot_Add(&sBoot, &timing, sizeof(timing));
}

static void MicDone(void *buffer, int length)
{
    (void)buffer; (void)length;
}

/* A short recording through the ARM7 core's microphone support. The samples
 * are mostly noise floor; any variation they add is welcome but not counted
 * on. */
static void Microphone(void)
{
    unsigned i, changes = 0, frames;
    memset(sMic, 0, sizeof(sMic));
    DC_FlushRange(sMic, sizeof(sMic));
    if (!soundMicRecord(sMic, sizeof(sMic), MicFormat_12Bit, 16000, MicDone)) return;
    for (frames = 0; frames < 6; frames++) swiWaitForVBlank();
    soundMicOff();
    DC_InvalidateRange(sMic, sizeof(sMic));
    for (i = 1; i < MIC_BYTES / 2; i++) changes += sMic[i] != sMic[i - 1];
    CpEntropyBoot_Add(&sBoot, sMic, sizeof(sMic));
    sMicrophone = changes > 0;
    memset(sMic, 0, sizeof(sMic));
}

void CpNdsEntropy_Start(void)
{
    time_t now = time(NULL);
    CpEntropyBoot_Start(&sBoot);
    sMicrophone = 0;
    CpNdsEntropy_Jitter();
    CpEntropyBoot_Add(&sBoot, &now, sizeof(now));
    Microphone();
    CpNdsEntropy_Jitter();
}

void CpNdsEntropy_Wifi(void)
{
    unsigned char mac[6];
    CpWifi_Mac(mac);
    CpEntropyBoot_Add(&sBoot, mac, sizeof(mac));
    CpNdsEntropy_Jitter();
}

int CpNdsEntropy_Finish(void)
{
    CpNdsEntropy_Jitter();
    sBytes = (unsigned)sBoot.total;
    /* Stack-only SHA-256: the TLS allocator is not set up at boot. */
    return CpEntropyBoot_Finish(&sBoot, WtFlash_Hash);
}

void CpNdsEntropy_Frame(void)
{
    WtEntropySample sample;
    Timing timing;
    memset(&sample, 0, sizeof(sample));
    ReadTiming(&timing);
    memcpy(sample.low, &timing, sizeof(timing) < sizeof(sample.low) ? sizeof(timing) : sizeof(sample.low));
    WtGame_SampleInput(&sample);
    CpEntropy_Frame(&sample);
    memset(&sample, 0, sizeof(sample));
}

void CpNdsEntropy_Report(unsigned *bytes, int *microphone)
{
    if (bytes) *bytes = sBytes;
    if (microphone) *microphone = sMicrophone;
}
