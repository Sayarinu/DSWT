#ifndef COMPANION_UI_UI_H
#define COMPANION_UI_UI_H

#include "ui/model.h"

/* The companion's screens as a state machine, after the patched game's
 * platinum/application.c: the same states, actions, messages and flow, plus
 * the companion's own save picker, game check and party list. It has no
 * platform code: the DS front end (ui/app.c) feeds it input once per frame
 * and draws whatever it describes, and host tests drive it with a fake model. */

typedef enum CpUiState {
    CP_UI_BOOT,          /* looking for saves, or checking the chosen one */
    CP_UI_SAVE_PICKER,   /* the .sav files and the games they belong to */
    CP_UI_GAME_CONFIRM,  /* "Platinum, English. Correct?" */
    CP_UI_RECOVERY,
    CP_UI_CONNECTING,
    CP_UI_MENU,
    CP_UI_PICKER,        /* the party, as the game's party menu lists it */
    CP_UI_CONFIRM,
    CP_UI_COLLECT_CONFIRM,
    CP_UI_CANCEL_CONFIRM,
    CP_UI_PINGING,
    CP_UI_RECEIVED,      /* the card of a Pokémon a completed trade delivered */
    CP_UI_MESSAGE,
    CP_UI_ERROR,
    CP_UI_RESTART,       /* closing the session before Retry reconnects */
    CP_UI_CLOSING,       /* closing the session before the save list */
    CP_UI_EXIT,          /* the companion should return to the loader */
    CP_UI_STATE_COUNT
} CpUiState;

/* Buttons, independent of the platform's key bits. */
enum {
    CP_UI_KEY_A = 1 << 0, CP_UI_KEY_B = 1 << 1, CP_UI_KEY_X = 1 << 2,
    CP_UI_KEY_UP = 1 << 3, CP_UI_KEY_DOWN = 1 << 4,
    CP_UI_KEY_LEFT = 1 << 5, CP_UI_KEY_RIGHT = 1 << 6, CP_UI_KEY_SELECT = 1 << 7
};

typedef struct CpUiInput {
    unsigned pressed; /* CP_UI_KEY_* newly pressed this frame */
    int touchPressed, touchHeld, touchX, touchY;
} CpUiInput;

/* The party list on the bottom screen: two columns of three panels, in party
 * order left to right, and a Back button below them. */
#define CP_UI_PANEL_LEFT 8
#define CP_UI_PANEL_TOP 32
#define CP_UI_PANEL_WIDTH 120
#define CP_UI_PANEL_HEIGHT 40
#define CP_UI_PANEL_STEP_X 128
#define CP_UI_PANEL_STEP_Y 40
#define CP_UI_PICKER_BACK_LEFT 160
#define CP_UI_PICKER_BACK_TOP 160
#define CP_UI_PICKER_BACK_WIDTH 88
#define CP_UI_PICKER_BACK_HEIGHT 24
/* The PICKER cursor value of the Back button. */
#define CP_UI_PICKER_BACK CP_UI_PARTY_MAX

/* At most this many bottom-screen buttons (WT_MENU_MAX_ACTIONS). */
#define CP_UI_MAX_ACTIONS 3

/* Rows of the save list that fit its window. */
#define CP_UI_SAVE_ROWS 5

typedef struct CpUi {
    CpUiState state;
    /* The message the top screen shows: a CompanionTextId. */
    int message;
    /* The highlighted bottom-screen button. */
    int cursor;
    /* The highlighted save (SAVE_PICKER) or party slot (PICKER), and the
     * first save row shown. */
    int listCursor, listTop;
    /* The party slot being offered (CONFIRM), or -1. */
    int selectedSlot;
    /* Where MESSAGE's Back goes. */
    CpUiState messageReturn;
    /* BOOT is checking a chosen save rather than looking for saves. */
    int loading;
    int touchBlocked;
    /* model->canLeave when last drawn. */
    int shownCanLeave;
    /* Set when the screens need drawing again; the front end clears it. */
    int dirty;
    /* Counts confirm sounds the front end should play (the game's SE on
     * every action). */
    unsigned sounds;
} CpUi;

/* Start at BOOT and ask the model for its saves. */
void CpUi_Start(CpUi *, CpUiModel *);

/* One frame: refresh the model, follow its progress and handle input. */
void CpUi_Frame(CpUi *, CpUiModel *, const CpUiInput *);

/* The bottom-screen buttons of the current state, as CompanionTextIds; returns
 * how many (0..CP_UI_MAX_ACTIONS). */
int CpUi_Actions(const CpUi *, const CpUiModel *, int labels[CP_UI_MAX_ACTIONS]);

/* The language the screens use: the loaded game's once a save is chosen,
 * otherwise the console's. */
CompanionLanguage CpUi_Language(const CpUi *, const CpUiModel *);
CompanionLanguage CpUi_GameLanguage(unsigned cpLanguage, CompanionLanguage fallback);

/* What the top screen shows besides its message. */
enum CpUiCard {
    CP_UI_CARD_NONE,     /* the message box alone, higher up */
    CP_UI_CARD_PARTY,    /* the card of party[listCursor] or party[selectedSlot] */
    CP_UI_CARD_RECEIVED, /* the received Pokémon's card */
    CP_UI_CARD_SAVES,    /* the save list */
    CP_UI_CARD_FULL_TEXT /* one framed body of text (errors, recovery, Info) */
};
enum CpUiCard CpUi_Card(const CpUi *, const CpUiModel *);

/* Something is in progress: the front end shows its spinner. */
int CpUi_Busy(const CpUi *);

/* The CompanionTextId of a party Pokémon's short ineligibility reason, or -1. */
int CpUi_IneligibleText(unsigned reason);

/* The party panel or Back button under a touch, or -1. */
int CpUi_PickerTouch(int x, int y, unsigned partyCount);

#endif
