#ifndef WT_TEST_COMPANION_ENGINE_RENAME_H
#define WT_TEST_COMPANION_ENGINE_RENAME_H

/* Force-included (-include) into the DS Companion's build of engine.c
 * (WT_ABANDON_UNSENT) that delivery_test links beside the games' own, so a
 * companion can write a journal the games' engine then reads. Renames every
 * external symbol of engine.c. */
#define WtEngine_Open CpEngine_Open
#define WtEngine_Close CpEngine_Close
#define WtEngine_Enroll CpEngine_Enroll
#define WtEngine_Deposit CpEngine_Deposit
#define WtEngine_Request CpEngine_Request
#define WtEngine_Settle CpEngine_Settle
#define WtEngine_Reply CpEngine_Reply
#define WtEngine_EscrowInventories CpEngine_EscrowInventories

#endif
