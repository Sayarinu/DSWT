#include <nds.h>

#include "platform/nds/app_platform.h"
#include "platform/nds/entropy_nds.h"
#include "platform/nds/screens.h"
#include "ui/app.h"
#include "ui/demo.h"
#include "ui/diagnostic.h"

/* The screens with the made-up demo model; `tour` walks through all of them. */
static void RunDemo(int tour)
{
    static CpUiDemo demo;
    CpUiDemo_Init(&demo, CpUiApp_SystemLanguage());
    CpUiApp_Run(&demo.model, tour ? &demo : NULL);
}

int main(void)
{
    CompanionScreens screens;
    int demo;
    systemCounterSetup();
#ifdef CP_UI_DEMO
    /* A build for looking at the screens: make CP_UI_DEMO=1 (buttons) or 2 (tour). */
    RunDemo(CP_UI_DEMO == 2);
    return 0;
#endif
    /* The SD card's mount time is one of the entropy pool's boot samples. */
    CpNdsEntropy_Start();
    CpNdsEntropy_Jitter();
    CpNdsCard_Mount();
    if (CpNdsApp_DiagnosticRequested()) {
        /* sd:/dswt-companion/diag.txt opens the platform diagnostic, and its
         * ui-demo switch the screens with made-up data. */
        CompanionScreens_Init(&screens);
        demo = CpDiagnostic_Run(&screens);
        if (demo) RunDemo(demo == CP_DIAGNOSTIC_UI_TOUR);
        return 0;
    }
    CpNdsApp_Run();
    return 0;
}
