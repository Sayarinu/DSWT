#ifndef COMPANION_UI_DIAGNOSTIC_H
#define COMPANION_UI_DIAGNOSTIC_H

#include "platform/nds/screens.h"

/* A plain boot screen that exercises the DSi platform layer: the entropy
 * pool, a read-only open of a save on the SD card, Wi-Fi with the console's
 * saved connections, and POST /wt/v1 STATUS requests for a random, never
 * enrolled save (the server answers UNAUTHENTICATED and writes nothing).
 * The real UI replaces it (LUNE-872).
 *
 * Optional settings, one key=value per line, in <drive>/dswt-companion/diag.txt:
 *   sav=<path>        the save to open read-only (default: the first .sav
 *                     file in <drive>/dswt-companion/)
 *   attempts=<n>      STATUS requests to send, 1 to 100 (default 1)
 *   ssid=<name>       connect to this network with the passphrase below
 *   passphrase=<key>  instead of the console's saved connections
 *   write-test=1      only for a dummy file named test.sav: write its journal
 *                     banks in place and verify them from the card
 * Everything shown is also appended to <drive>/dswt-companion/diag-log.txt. */
void CpDiagnostic_Run(CompanionScreens *);

#endif
