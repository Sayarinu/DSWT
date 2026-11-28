#ifndef COMPANION_UI_DEMO_H
#define COMPANION_UI_DEMO_H

#include "ui/ui.h"

/* A scripted model with made-up saves, a made-up party and a made-up server,
 * for looking at every screen without a save, a ROM or a network: ui-demo=1
 * in diag.txt, or a ROM built with CP_UI_DEMO. It never opens a file or a
 * socket. Each trade or collect answers with the next outcome in turn
 * (CpUiDemo_SetOutcome picks one). */

enum CpUiDemoOutcome {
    CP_UI_DEMO_WAITING,      /* no partner yet: the offer stays queued */
    CP_UI_DEMO_COMPLETED,    /* a Pokémon arrived */
    CP_UI_DEMO_RETURNED,     /* the offer was cancelled and handed back */
    CP_UI_DEMO_REJECTED,     /* the server refused the offer (legality check) */
    CP_UI_DEMO_SUPERSEDED,   /* another copy of the save traded first */
    CP_UI_DEMO_NO_ROOM,      /* the result waits for space in the party or PC */
    CP_UI_DEMO_KEEP_PLAYING, /* the save needs more play for entropy */
    CP_UI_DEMO_CANNOT_HOLD,  /* this game cannot hold the delivered Pokémon */
    CP_UI_DEMO_ERROR,        /* the exchange failed */
    CP_UI_DEMO_OUTCOMES
};

typedef struct CpUiDemo {
    CpUiModel model;
    unsigned frame, due;
    int job;
    int outcome;
    int failConnect; /* the next connect fails */
    int noSaves;     /* the next scan finds nothing */
    /* Wi-Fi settings: made-up networks found over a second or two, and joins
     * that succeed unless failJoin is set. failWifi makes the next connect
     * find no saved network. */
    int failWifi, failJoin, failForget;
    unsigned scanStart, joinDue;
    int joining;
    unsigned loading;
    /* The scripted tour (CpUiDemo_Tour). */
    unsigned step, stepFrame;
} CpUiDemo;

void CpUiDemo_Init(CpUiDemo *, CompanionLanguage uiLanguage);
void CpUiDemo_SetOutcome(CpUiDemo *, enum CpUiDemoOutcome);

/* The input of one frame of a fixed walk through every screen, holding each
 * for a few seconds; returns 0 once the walk is over. */
int CpUiDemo_Tour(CpUiDemo *, CpUiInput *);

#endif
