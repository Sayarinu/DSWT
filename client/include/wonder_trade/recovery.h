#ifndef WONDER_TRADE_RECOVERY_H
#define WONDER_TRADE_RECOVERY_H

#include "wonder_trade/journal.h"

/* Initial RAM setup may clear a body, but no retail writes are authorized
 * until the read-only boot inspection completes. Active recovery is gated. */
int WtRecovery_Started(void);
int WtRecovery_CanWrite(void);
int WtRecovery_CanClear(void);
void WtRecovery_Begin(void);
void WtRecovery_Block(void);
/* Only the serialized game adapter may grant a retail write during recovery.
 * This never grants clearing or authorizes ordinary gameplay. */
void WtRecovery_InternalWrite(int);
void WtRecovery_Allow(void);
int WtRecovery_Check(WtJournalIO);

#endif
