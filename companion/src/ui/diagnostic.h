#ifndef COMPANION_UI_DIAGNOSTIC_H
#define COMPANION_UI_DIAGNOSTIC_H

#include "platform/nds/screens.h"

/* A plain boot screen that exercises the DSi platform layer: the entropy
 * pool, a read-only open of a save on the SD card, Wi-Fi with the console's
 * saved connections, and POST /wt/v1 STATUS requests for a random, never
 * enrolled save (the server answers UNAUTHENTICATED and writes nothing).
 * The companion opens it instead of the screens whenever
 * <drive>/dswt-companion/diag.txt exists (main.c).
 *
 * Optional settings, one key=value per line, in that diag.txt:
 *   sav=<path>        the save to open read-only (default: the first .sav
 *                     file in <drive>/dswt-companion/)
 *   attempts=<n>      STATUS requests to send, 1 to 100 (default 1)
 *   ssid=<name>       connect to this network with the passphrase below
 *   passphrase=<key>  instead of the console's saved connections
 *   write-test=1      only for a dummy file named test.sav: write its journal
 *                     banks in place and verify them from the card
 *   card-rom=1        check Slot-1 game data and card removal (standalone);
 *                     reads bios7.bin (or bios9i.bin) and, for TWL, bios7i.bin
 *                     from sd:/dswt-companion/
 *   ui-demo=1         show the screens (ui/) with the made-up demo model
 *                     instead; ui-demo=tour walks through every screen by itself
 * Everything shown is also appended to <drive>/dswt-companion/diag-log.txt.
 * Returns 0, or the CP_DIAGNOSTIC_UI_* that diag.txt asked for. */
enum { CP_DIAGNOSTIC_UI_DEMO = 1, CP_DIAGNOSTIC_UI_TOUR = 2 };
int CpDiagnostic_Run(CompanionScreens *);

#endif
