#ifndef COMPANION_UI_APP_H
#define COMPANION_UI_APP_H

#include "ui/demo.h"
#include "ui/model.h"

/* The DS front end of the screens: sets up both engines' backgrounds, loads
 * the tiles and palettes, then runs the state machine against `model` once a
 * frame until it reaches CP_UI_EXIT. The backdrop's drift and the spinner run
 * from the VBlank interrupt, so they keep moving while a TLS handshake holds
 * the main thread (see platform/nds/worker.h). `tour` (or NULL) supplies the
 * input instead of the buttons, for the demo model's walk through every screen. */
void CpUiApp_Run(CpUiModel *model, CpUiDemo *tour);

/* The console's language setting, for the screens before a save is chosen. */
CompanionLanguage CpUiApp_SystemLanguage(void);

#endif
