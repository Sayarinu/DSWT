#include <nds.h>

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
    /* Until the session driver (LUNE-871) supplies the real model, the
     * platform diagnostic stays the default; diag.txt's ui-demo opens the
     * screens instead. */
    CompanionScreens_Init(&screens);
    demo = CpDiagnostic_Run(&screens);
    if (demo) RunDemo(demo == CP_DIAGNOSTIC_UI_TOUR);
    return 0;
}
