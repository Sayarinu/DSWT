#ifndef COMPANION_PLATFORM_NDS_APP_PLATFORM_H
#define COMPANION_PLATFORM_NDS_APP_PLATFORM_H

/* The session model's platform on the DS (app/platform.h): FatFs on the SD
 * card (sd: in DSi mode, fat: through DLDI in DS mode), the worker cothread,
 * Wi-Fi with the console's saved connections, CpNet_Post and PSA crypto. It
 * also keeps the companion's log, <drive>_nds/dswt/companion-log.txt: what
 * the companion found and did, with support codes, never a credential, a
 * save ID or a passphrase. */

/* Mount the SD card once; later calls return the first result. Nonzero when mounted. */
int CpNdsCard_Mount(void);
/* The mounted card's root, such as "sd:/", or "" when none is mounted. */
const char *CpNdsCard_Root(void);

/* Nonzero when <drive>dswt-companion/diag.txt asks for the platform
 * diagnostic (its presence, as before the real screens existed). */
int CpNdsApp_DiagnosticRequested(void);

/* Start Wi-Fi and the entropy pool, then run the screens on the real model
 * until the player exits. Call after CpNdsEntropy_Start and CpNdsCard_Mount. */
void CpNdsApp_Run(void);
void CpNdsApp_ReturnToMenu(void);

#endif
