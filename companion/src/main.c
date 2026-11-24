#include <nds.h>

#include "platform/nds/screens.h"
#include "ui/diagnostic.h"

int main(void)
{
    CompanionScreens screens;
    systemCounterSetup();
    CompanionScreens_Init(&screens);
    CpDiagnostic_Run(&screens);
    return 0;
}
