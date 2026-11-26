#ifndef COMPANION_PLATFORM_NDS_ENTROPY_NDS_H
#define COMPANION_PLATFORM_NDS_ENTROPY_NDS_H

/* Feeds the companion's entropy pool (platform/entropy_pool.h) on the DS.
 * There is no hardware random number generator: the boot sample is timer and
 * scanline readings taken around slow, variable hardware steps (SD and Wi-Fi
 * start-up), the real-time clock, the MAC address and a short microphone
 * recording. Each frame then adds the input and timer readings; the core
 * credits only input changes. The DSi console ID is left out: libnds offers
 * it only on the ARM7, and the stock ARM7 core does not pass it on.
 *
 * Call order: Start, Jitter around each slow step, Wifi once Wi-Fi is on,
 * Finish, then Frame once per VBlank from the main thread. */

void CpNdsEntropy_Start(void);
/* Record timing readings now; call before and after a slow step. */
void CpNdsEntropy_Jitter(void);
/* Add the MAC address; call after CpWifi_Init. */
void CpNdsEntropy_Wifi(void);
/* Hash the boot sample into the pool. Zero on success. */
int CpNdsEntropy_Finish(void);
/* Sample this frame's input; call after scanKeys(). */
void CpNdsEntropy_Frame(void);
/* Bytes the boot sample collected, and whether the microphone recording
 * varied, for diagnostics. */
void CpNdsEntropy_Report(unsigned *bytes, int *microphone);

#endif
