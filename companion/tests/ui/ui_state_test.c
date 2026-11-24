/* The companion UI's state machine against a fake model: every state's
 * buttons, and the flow application.c follows for each server outcome. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/rom.h"
#include "ui/ui.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "wonder_trade/menu_input.h"
#pragma GCC diagnostic pop
#include "wonder_trade/journal.h"
#include "wonder_trade/wire.h"

typedef struct Fake {
    CpUiModel model;
    int scans, loads, lastLoad, unloads, connects, trades, lastTrade, collects, lastCancel, closes, updates;
} Fake;

static void Scan(void *c) { ((Fake *)c)->scans++; }
static void Load(void *c, unsigned i) { ((Fake *)c)->loads++; ((Fake *)c)->lastLoad = (int)i; }
static void Unload(void *c) { ((Fake *)c)->unloads++; }
static void Connect(void *c) { ((Fake *)c)->connects++; }
static void Trade(void *c, int slot) { ((Fake *)c)->trades++; ((Fake *)c)->lastTrade = slot; }
static void Collect(void *c, int cancel) { ((Fake *)c)->collects++; ((Fake *)c)->lastCancel = cancel; }
static void Close(void *c) { ((Fake *)c)->closes++; }
static void Update(void *c) { ((Fake *)c)->updates++; }
static const CpUiOps kOps = { Scan, Load, Unload, Connect, Trade, Collect, Close, Update };

static Fake sFake;
static CpUi sUi;
static const CpUiInput kNone;

static CpUiModel *Reset(void)
{
    memset(&sFake, 0, sizeof(sFake));
    sFake.model.ops = &kOps;
    sFake.model.context = &sFake;
    sFake.model.loadMessage = -1;
    sFake.model.canLeave = 1;
    sFake.model.tradePhase = WT_JOURNAL_IDLE;
    sFake.model.systemLanguage = Companion_Language_de_DE;
    return &sFake.model;
}

static void Frame(void) { CpUi_Frame(&sUi, &sFake.model, &kNone); }

static void Press(unsigned keys)
{
    CpUiInput input = kNone;
    input.pressed = keys;
    CpUi_Frame(&sUi, &sFake.model, &input);
}

/* A tap: one release frame unblocks touch, then the press lands. */
static void Tap(int x, int y)
{
    CpUiInput input = kNone;
    CpUi_Frame(&sUi, &sFake.model, &input);
    input.touchPressed = input.touchHeld = 1;
    input.touchX = x;
    input.touchY = y;
    CpUi_Frame(&sUi, &sFake.model, &input);
}

static void Expect(CpUiState state, int message)
{
    if (sUi.state != state || sUi.message != message) {
        fprintf(stderr, "expected state %d message %d, got state %d message %d\n", state, message, sUi.state, sUi.message);
        assert(0);
    }
}

static void ExpectActions(int count, int a, int b, int c)
{
    int labels[CP_UI_MAX_ACTIONS] = { -1, -1, -1 }, expected[3] = { a, b, c }, i;
    assert(CpUi_Actions(&sUi, &sFake.model, labels) == count);
    for (i = 0; i < count; i++) assert(labels[i] == expected[i]);
}

static void AddSaves(unsigned count)
{
    unsigned i;
    sFake.model.saveCount = count;
    for (i = 0; i < count; i++) {
        snprintf(sFake.model.saves[i].name, CP_UI_SAVE_NAME_MAX, "save%u.sav", i);
        sFake.model.saves[i].version = CP_VERSION_PLATINUM;
        sFake.model.saves[i].language = CP_LANGUAGE_ENGLISH;
    }
}

static void AddParty(void)
{
    static const uint8_t reasons[] = { CP_UI_ELIGIBLE, CP_UI_ELIGIBLE, CP_UI_INELIGIBLE_EGG,
        CP_UI_INELIGIBLE_MAIL, CP_UI_INELIGIBLE_DUPLICATE, CP_UI_ELIGIBLE };
    unsigned i;
    sFake.model.partyCount = 6;
    for (i = 0; i < 6; i++) {
        sFake.model.party[i].species = (uint16_t)(387 + i);
        sFake.model.party[i].level = (uint8_t)(5 + i);
        sFake.model.party[i].ineligible = reasons[i];
    }
}

/* Boot, pick save 0, confirm the game and connect: the UI stops in PINGING
 * after the resume request. */
static void ReachPinging(void)
{
    CpUiModel *model = Reset();
    CpUi_Start(&sUi, model);
    AddSaves(2);
    AddParty();
    Frame();
    Press(CP_UI_KEY_A);
    sFake.model.gameVersion = CP_VERSION_PLATINUM;
    sFake.model.gameLanguage = CP_LANGUAGE_FRENCH;
    Frame();
    Expect(CP_UI_GAME_CONFIRM, Companion_Text_GameConfirm);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_CONNECTING, WonderTrade_Text_Connecting);
    sFake.model.net = CP_UI_NET_READY;
    Frame();
    Expect(CP_UI_PINGING, WonderTrade_Text_Pinging);
    assert(sFake.trades == 1 && sFake.lastTrade == -1);
}

static void Respond(void)
{
    sFake.model.net = CP_UI_NET_RESPONSE;
    Frame();
}

static void ReachMenu(unsigned phase)
{
    ReachPinging();
    sFake.model.tradePhase = phase;
    Respond();
    Expect(CP_UI_MENU, phase == WT_JOURNAL_IDLE ? WonderTrade_Text_Choose : WonderTrade_Text_Queued);
    sFake.model.net = CP_UI_NET_BUSY;
}

static void TestBootAndSaves(void)
{
    CpUiModel *model = Reset();
    CpUi_Start(&sUi, model);
    assert(sFake.scans == 1);
    Expect(CP_UI_BOOT, WonderTrade_Text_CheckingSave);
    assert(CpUi_Busy(&sUi));
    ExpectActions(0, 0, 0, 0);
    model->busy = 1;
    Frame();
    Expect(CP_UI_BOOT, WonderTrade_Text_CheckingSave);
    model->busy = 0;
    Frame();
    Expect(CP_UI_SAVE_PICKER, Companion_Text_NoSaves);
    ExpectActions(2, WonderTrade_Text_Retry, WonderTrade_Text_Exit, 0);
    assert(CpUi_Card(&sUi, model) == CP_UI_CARD_NONE);
    /* Retry looks again. */
    Press(CP_UI_KEY_A);
    assert(sFake.scans == 2);
    Expect(CP_UI_BOOT, WonderTrade_Text_CheckingSave);
    AddSaves(7);
    Frame();
    Expect(CP_UI_SAVE_PICKER, Companion_Text_SavePickerHint);
    ExpectActions(2, WonderTrade_Text_ConfirmAction, WonderTrade_Text_Exit, 0);
    assert(CpUi_Card(&sUi, model) == CP_UI_CARD_SAVES);
    assert(CpUi_Language(&sUi, model) == Companion_Language_de_DE);
    /* Up and Down move through the list, wrapping and scrolling. */
    Press(CP_UI_KEY_UP);
    assert(sUi.listCursor == 6 && sUi.listTop == 6 - CP_UI_SAVE_ROWS + 1 && sUi.cursor == 0);
    Press(CP_UI_KEY_DOWN);
    assert(sUi.listCursor == 0 && sUi.listTop == 0);
    Press(CP_UI_KEY_DOWN);
    Press(CP_UI_KEY_DOWN);
    /* A save that cannot be used says why and goes back to the list. */
    Press(CP_UI_KEY_A);
    assert(sFake.loads == 1 && sFake.lastLoad == 2);
    Expect(CP_UI_BOOT, WonderTrade_Text_CheckingSave);
    model->busy = 1;
    Frame();
    Expect(CP_UI_BOOT, WonderTrade_Text_CheckingSave);
    model->busy = 0;
    model->loadMessage = Companion_Text_MatchingRomMissing;
    Frame();
    Expect(CP_UI_MESSAGE, Companion_Text_MatchingRomMissing);
    assert(CpUi_Language(&sUi, model) == Companion_Language_de_DE);
    ExpectActions(1, WonderTrade_Text_Back, 0, 0);
    Press(CP_UI_KEY_B);
    assert(sFake.unloads == 1);
    Expect(CP_UI_SAVE_PICKER, Companion_Text_SavePickerHint);
    assert(sUi.listCursor == 2);
    /* A usable one asks to confirm the game, in the game's language. */
    model->loadMessage = -1;
    model->gameVersion = CP_VERSION_PLATINUM;
    model->gameLanguage = CP_LANGUAGE_JAPANESE;
    Press(CP_UI_KEY_A);
    Frame();
    Expect(CP_UI_GAME_CONFIRM, Companion_Text_GameConfirm);
    ExpectActions(2, WonderTrade_Text_ConfirmAction, WonderTrade_Text_Back, 0);
    assert(CpUi_Language(&sUi, model) == Companion_Language_ja_JP);
    Press(CP_UI_KEY_B);
    assert(sFake.unloads == 2);
    Expect(CP_UI_SAVE_PICKER, Companion_Text_SavePickerHint);
    /* B leaves the companion from the list. */
    Press(CP_UI_KEY_B);
    Expect(CP_UI_EXIT, WonderTrade_Text_Disconnecting);
}

static void TestTouchExitFromList(void)
{
    CpUiModel *model = Reset();
    CpUi_Start(&sUi, model);
    AddSaves(1);
    Frame();
    /* The first press after a screen opens is ignored until a release. */
    {
        CpUiInput input = kNone;
        input.touchPressed = input.touchHeld = 1;
        input.touchX = WT_MENU_LEFT + 4;
        input.touchY = WT_MENU_TOP + WT_MENU_SPACING + 4;
        CpUi_Frame(&sUi, model, &input);
        Expect(CP_UI_SAVE_PICKER, Companion_Text_SavePickerHint);
    }
    Tap(WT_MENU_LEFT + 4, WT_MENU_TOP + WT_MENU_SPACING + 4);
    Expect(CP_UI_EXIT, WonderTrade_Text_Disconnecting);
    assert(sUi.sounds == 1);
}

static void TestRecovery(void)
{
    CpUiModel *model = Reset();
    CpUi_Start(&sUi, model);
    AddSaves(1);
    Frame();
    Press(CP_UI_KEY_A);
    model->recovery = 1;
    Frame();
    Press(CP_UI_KEY_A);
    Expect(CP_UI_RECOVERY, WonderTrade_Text_RecoveryRequired);
    ExpectActions(1, WonderTrade_Text_RecoveryAction, 0, 0);
    assert(CpUi_Card(&sUi, model) == CP_UI_CARD_FULL_TEXT);
    Press(CP_UI_KEY_B | CP_UI_KEY_X);
    Expect(CP_UI_RECOVERY, WonderTrade_Text_RecoveryRequired);
    assert(sFake.connects == 0 && sFake.closes == 0);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_CONNECTING, WonderTrade_Text_Connecting);
    assert(sFake.connects == 1);
}

static void TestConnecting(void)
{
    ReachPinging();
    /* Connecting and pinging offer Cancel only while leaving is safe. */
    ExpectActions(1, WonderTrade_Text_Cancel, 0, 0);
    sFake.model.canLeave = 0;
    sUi.dirty = 0;
    Frame();
    assert(sUi.dirty);
    ExpectActions(0, 0, 0, 0);
    Press(CP_UI_KEY_B);
    Expect(CP_UI_PINGING, WonderTrade_Text_Pinging);
    assert(sFake.closes == 0);
    sFake.model.canLeave = 1;
    Press(CP_UI_KEY_B);
    Expect(CP_UI_CLOSING, WonderTrade_Text_Disconnecting);
    assert(sFake.closes == 1 && CpUi_Busy(&sUi));
    Frame();
    Expect(CP_UI_CLOSING, WonderTrade_Text_Disconnecting);
    sFake.model.net = CP_UI_NET_CLOSED;
    Frame();
    assert(sFake.unloads == 1);
    Expect(CP_UI_SAVE_PICKER, Companion_Text_SavePickerHint);

    /* A failed connection says so, with Retry and (when allowed) Exit. */
    ReachPinging();
    sUi.state = CP_UI_CONNECTING;
    sFake.model.net = CP_UI_NET_ERROR;
    sFake.model.error = -12;
    Frame();
    Expect(CP_UI_ERROR, WonderTrade_Text_ConnectionError);
    ExpectActions(2, WonderTrade_Text_Retry, WonderTrade_Text_Exit, 0);
    assert(CpUi_Card(&sUi, &sFake.model) == CP_UI_CARD_FULL_TEXT);
    sFake.model.canLeave = 0;
    ExpectActions(1, WonderTrade_Text_Retry, 0, 0);
    Press(CP_UI_KEY_B);
    Expect(CP_UI_ERROR, WonderTrade_Text_ConnectionError);
    /* Retry closes the session and starts it again once closed. */
    Press(CP_UI_KEY_A);
    Expect(CP_UI_RESTART, WonderTrade_Text_Disconnecting);
    assert(sFake.closes == 1);
    Frame();
    Expect(CP_UI_RESTART, WonderTrade_Text_Disconnecting);
    sFake.model.net = CP_UI_NET_CLOSED;
    Frame();
    Expect(CP_UI_CONNECTING, WonderTrade_Text_Connecting);
    assert(sFake.connects == 2);

    /* Exit from an error ends the session. */
    ReachPinging();
    sFake.model.net = CP_UI_NET_ERROR;
    Frame();
    Expect(CP_UI_ERROR, WonderTrade_Text_PingError);
    Press(CP_UI_KEY_DOWN);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_CLOSING, WonderTrade_Text_Disconnecting);
}

typedef struct Outcome {
    const char *name;
    int completed, returned, rejected, waiting, superseded, needsEntropy, refused, hasReceived;
    unsigned refusal, phase;
    CpUiState state;
    int message;
} Outcome;

static void TestOutcomes(void)
{
    static const Outcome outcomes[] = {
        { "completed", 1, 0, 0, 0, 0, 0, 0, 1, 0, WT_JOURNAL_IDLE, CP_UI_RECEIVED, WonderTrade_Text_Completed },
        { "completed, no card", 1, 0, 0, 0, 0, 0, 0, 0, 0, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_Completed },
        { "returned", 1, 1, 0, 0, 0, 0, 0, 0, 0, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_Returned },
        { "returned: egg", 1, 1, 0, 0, 0, 0, 0, 0, WT_REFUSAL_EGG, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_RefusedEgg },
        { "rejected", 0, 0, 1, 0, 0, 0, 0, 0, 0, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_Rejected },
        { "rejected: egg", 0, 0, 1, 0, 0, 0, 0, 0, WT_REFUSAL_EGG, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_RefusedEgg },
        { "rejected: mail", 0, 0, 1, 0, 0, 0, 0, 0, WT_REFUSAL_MAIL, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_RefusedMail },
        { "rejected: game", 0, 0, 1, 0, 0, 0, 0, 0, WT_REFUSAL_INCOMPATIBLE, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_RefusedGame },
        { "rejected: illegal", 0, 0, 1, 0, 0, 0, 0, 0, WT_REFUSAL_ILLEGAL, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_RefusedIllegal },
        { "rejected: name", 0, 0, 1, 0, 0, 0, 0, 0, WT_REFUSAL_NAME, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_RefusedName },
        { "superseded", 0, 0, 0, 0, 1, 0, 0, 0, 0, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_Superseded },
        /* A superseded copy still holding an offer reads like any other pending one. */
        { "superseded, pending", 0, 0, 0, 0, 1, 0, 0, 0, 0, WT_JOURNAL_QUEUED, CP_UI_MENU, WonderTrade_Text_Queued },
        { "waiting", 0, 0, 0, 1, 0, 0, 0, 0, 0, WT_JOURNAL_QUEUED, CP_UI_MESSAGE, WonderTrade_Text_Waiting },
        { "no room", 0, 0, 0, 0, 0, 0, 0, 0, 0, WT_JOURNAL_RECEIVED, CP_UI_MENU, WonderTrade_Text_NoRoom },
        { "no room to restore", 0, 0, 0, 0, 0, 0, 0, 0, 0, WT_JOURNAL_RESTORING, CP_UI_MENU, WonderTrade_Text_NoRoom },
        { "keep playing", 0, 0, 0, 0, 0, 1, 0, 0, 0, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_KeepPlaying },
        { "keep playing wins", 1, 0, 0, 0, 0, 1, 0, 1, 0, WT_JOURNAL_IDLE, CP_UI_MESSAGE, WonderTrade_Text_KeepPlaying },
        { "cannot hold", 0, 0, 0, 0, 0, 0, 1, 0, 0, WT_JOURNAL_QUEUED, CP_UI_MESSAGE, WonderTrade_Text_CannotHold },
        { "nothing pending", 0, 0, 0, 0, 0, 0, 0, 0, 0, WT_JOURNAL_IDLE, CP_UI_MENU, WonderTrade_Text_Choose },
        { "still queued", 0, 0, 0, 0, 0, 0, 0, 0, 0, WT_JOURNAL_QUEUED, CP_UI_MENU, WonderTrade_Text_Queued },
    };
    size_t i;
    for (i = 0; i < sizeof(outcomes) / sizeof(*outcomes); i++) {
        const Outcome *o = &outcomes[i];
        CpUiModel *model;
        ReachPinging();
        model = &sFake.model;
        model->completed = o->completed; model->returned = o->returned; model->rejected = o->rejected;
        model->waiting = o->waiting; model->superseded = o->superseded; model->needsEntropy = o->needsEntropy;
        model->refused = o->refused; model->hasReceived = o->hasReceived;
        model->refusal = (unsigned char)o->refusal; model->tradePhase = o->phase;
        Respond();
        if (sUi.state != o->state || sUi.message != o->message) fprintf(stderr, "outcome %s\n", o->name);
        Expect(o->state, o->message);
        assert(sUi.selectedSlot == -1);
        if (o->state == CP_UI_RECEIVED) {
            assert(CpUi_Card(&sUi, model) == CP_UI_CARD_RECEIVED);
            ExpectActions(1, WonderTrade_Text_Back, 0, 0);
        }
        if (o->state == CP_UI_MESSAGE || o->state == CP_UI_RECEIVED) {
            model->net = CP_UI_NET_BUSY;
            Press(CP_UI_KEY_B);
            /* Needing entropy leaves Wonder Trade, like the game; others go to the menu. */
            if (o->needsEntropy) Expect(CP_UI_CLOSING, WonderTrade_Text_Disconnecting);
            else Expect(CP_UI_MENU, o->phase == WT_JOURNAL_IDLE ? WonderTrade_Text_Choose : WonderTrade_Text_Queued);
        }
    }
}

static void TestMenu(void)
{
    /* Nothing pending: Wonder Trade, Info, Exit. */
    ReachMenu(WT_JOURNAL_IDLE);
    ExpectActions(3, WonderTrade_Text_Trade, WonderTrade_Text_Info, WonderTrade_Text_Exit);
    assert(!CpUi_Busy(&sUi));
    Press(CP_UI_KEY_DOWN);
    assert(sUi.cursor == 1);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_MESSAGE, WonderTrade_Text_Explanation);
    assert(CpUi_Card(&sUi, &sFake.model) == CP_UI_CARD_FULL_TEXT);
    /* X from a menu message leaves Wonder Trade. */
    Press(CP_UI_KEY_X);
    Expect(CP_UI_CLOSING, WonderTrade_Text_Disconnecting);

    ReachMenu(WT_JOURNAL_IDLE);
    Press(CP_UI_KEY_UP);
    assert(sUi.cursor == 2);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_CLOSING, WonderTrade_Text_Disconnecting);

    /* Pending: Collect result, Cancel offer, Exit. */
    ReachMenu(WT_JOURNAL_QUEUED);
    ExpectActions(3, WonderTrade_Text_Collect, WonderTrade_Text_CancelOffer, WonderTrade_Text_Exit);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_COLLECT_CONFIRM, WonderTrade_Text_ConfirmCollect);
    ExpectActions(2, WonderTrade_Text_ConfirmAction, WonderTrade_Text_Back, 0);
    Press(CP_UI_KEY_B);
    Expect(CP_UI_MENU, WonderTrade_Text_Queued);
    Press(CP_UI_KEY_A);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_PINGING, WonderTrade_Text_Pinging);
    assert(sFake.collects == 1 && sFake.lastCancel == 0);

    ReachMenu(WT_JOURNAL_QUEUED);
    Tap(WT_MENU_LEFT + 10, WT_MENU_TOP + WT_MENU_SPACING + 10);
    Expect(CP_UI_CANCEL_CONFIRM, WonderTrade_Text_ConfirmCancel);
    ExpectActions(2, WonderTrade_Text_ConfirmAction, WonderTrade_Text_Back, 0);
    /* Touching the second button is Back. */
    Tap(WT_MENU_LEFT + 10, WT_MENU_TOP + WT_MENU_SPACING + 10);
    Expect(CP_UI_MENU, WonderTrade_Text_Queued);
    Tap(WT_MENU_LEFT + 10, WT_MENU_TOP + WT_MENU_SPACING + 10);
    Tap(WT_MENU_LEFT + 10, WT_MENU_TOP + 10);
    Expect(CP_UI_PINGING, WonderTrade_Text_Pinging);
    assert(sFake.collects == 1 && sFake.lastCancel == 1);
    /* Touches between buttons do nothing. */
    ReachMenu(WT_JOURNAL_QUEUED);
    Tap(WT_MENU_LEFT + 10, WT_MENU_TOP + WT_MENU_HEIGHT + 2);
    Tap(WT_MENU_LEFT - 2, WT_MENU_TOP + 10);
    Expect(CP_UI_MENU, WonderTrade_Text_Queued);
}

static void TestPicker(void)
{
    ReachMenu(WT_JOURNAL_IDLE);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_PICKER, WonderTrade_Text_Choose);
    ExpectActions(0, 0, 0, 0);
    assert(sUi.partyCursor == 0 && CpUi_Card(&sUi, &sFake.model) == CP_UI_CARD_PARTY);
    Press(CP_UI_KEY_RIGHT);
    assert(sUi.partyCursor == 1);
    Press(CP_UI_KEY_DOWN);
    assert(sUi.partyCursor == 3);
    Press(CP_UI_KEY_DOWN);
    assert(sUi.partyCursor == 5);
    Press(CP_UI_KEY_DOWN);
    assert(sUi.partyCursor == CP_UI_PICKER_BACK);
    assert(CpUi_Card(&sUi, &sFake.model) == CP_UI_CARD_NONE);
    Press(CP_UI_KEY_RIGHT);
    assert(sUi.partyCursor == 0);
    Press(CP_UI_KEY_LEFT);
    assert(sUi.partyCursor == CP_UI_PICKER_BACK);
    Press(CP_UI_KEY_UP);
    assert(sUi.partyCursor == 5);
    /* An Egg cannot be offered; neither can a duplicate. */
    sUi.partyCursor = 2;
    Press(CP_UI_KEY_A);
    Expect(CP_UI_MESSAGE, WonderTrade_Text_Ineligible);
    Press(CP_UI_KEY_B);
    Expect(CP_UI_PICKER, WonderTrade_Text_Choose);
    assert(sUi.partyCursor == 2);
    sUi.partyCursor = 4;
    Press(CP_UI_KEY_A);
    Expect(CP_UI_MESSAGE, WonderTrade_Text_Duplicate);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_PICKER, WonderTrade_Text_Choose);
    /* Touch a panel: slot 1 is the right-hand top one. */
    Tap(CP_UI_PANEL_LEFT + CP_UI_PANEL_STEP_X + 5, CP_UI_PANEL_TOP + 5);
    Expect(CP_UI_CONFIRM, WonderTrade_Text_Confirm);
    assert(sUi.selectedSlot == 1 && CpUi_Card(&sUi, &sFake.model) == CP_UI_CARD_PARTY);
    ExpectActions(2, WonderTrade_Text_ConfirmAction, WonderTrade_Text_Back, 0);
    Press(CP_UI_KEY_B);
    Expect(CP_UI_PICKER, WonderTrade_Text_Choose);
    assert(sUi.selectedSlot == -1);
    sUi.partyCursor = 5;
    Press(CP_UI_KEY_A);
    Expect(CP_UI_CONFIRM, WonderTrade_Text_Confirm);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_PINGING, WonderTrade_Text_Pinging);
    assert(sFake.trades == 2 && sFake.lastTrade == 5);
    /* The picker's Back button and B go back to the menu. */
    ReachMenu(WT_JOURNAL_IDLE);
    Press(CP_UI_KEY_A);
    Tap(CP_UI_PICKER_BACK_LEFT + 2, CP_UI_PICKER_BACK_TOP + 2);
    Expect(CP_UI_MENU, WonderTrade_Text_Choose);
    Press(CP_UI_KEY_A);
    Press(CP_UI_KEY_B);
    Expect(CP_UI_MENU, WonderTrade_Text_Choose);
}

static void TestPickerTouch(void)
{
    int slot;
    for (slot = 0; slot < 6; slot++) {
        int x = CP_UI_PANEL_LEFT + (slot % 2) * CP_UI_PANEL_STEP_X, y = CP_UI_PANEL_TOP + (slot / 2) * CP_UI_PANEL_STEP_Y;
        assert(CpUi_PickerTouch(x, y, 6) == slot);
        assert(CpUi_PickerTouch(x + CP_UI_PANEL_WIDTH - 1, y + CP_UI_PANEL_HEIGHT - 1, 6) == slot);
        assert(CpUi_PickerTouch(x, y, (unsigned)slot) == -1);
    }
    assert(CpUi_PickerTouch(CP_UI_PANEL_LEFT + CP_UI_PANEL_WIDTH, CP_UI_PANEL_TOP, 6) == -1);
    assert(CpUi_PickerTouch(CP_UI_PANEL_LEFT - 1, CP_UI_PANEL_TOP, 6) == -1);
    assert(CpUi_PickerTouch(CP_UI_PICKER_BACK_LEFT, CP_UI_PICKER_BACK_TOP, 0) == CP_UI_PICKER_BACK);
    assert(CpUi_PickerTouch(CP_UI_PICKER_BACK_LEFT - 1, CP_UI_PICKER_BACK_TOP, 0) == -1);
}

static void TestTextIds(void)
{
    unsigned reason, language;
    int id;
    assert(CpUi_IneligibleText(CP_UI_ELIGIBLE) == -1);
    for (reason = CP_UI_INELIGIBLE_EGG; reason <= CP_UI_INELIGIBLE_OTHER + 1; reason++) {
        id = CpUi_IneligibleText(reason);
        assert(id >= 0 && id < Companion_Text_Count);
        for (language = 0; language < Companion_Language_Count; language++) assert(Companion_Text[id][language][0]);
    }
    assert(CpUi_GameLanguage(CP_LANGUAGE_KOREAN, Companion_Language_en_US) == Companion_Language_ko_KR);
    assert(CpUi_GameLanguage(6, Companion_Language_fr_FR) == Companion_Language_fr_FR);
}

/* The highlighted save is always one of the visible rows and a real save. */
static void ExpectVisibleSave(void)
{
    assert(sUi.listCursor >= 0 && sUi.listCursor < (int)sFake.model.saveCount);
    assert(sUi.listTop >= 0 && sUi.listTop <= sUi.listCursor && sUi.listCursor < sUi.listTop + CP_UI_SAVE_ROWS);
}

static void TestSaveCursorSurvivesThePicker(void)
{
    CpUiModel *model = Reset();
    CpUi_Start(&sUi, model);
    AddSaves(7);
    AddParty();
    Frame();
    Press(CP_UI_KEY_A);
    Frame();
    Press(CP_UI_KEY_A);
    model->net = CP_UI_NET_READY;
    Frame();
    model->net = CP_UI_NET_RESPONSE;
    Frame();
    Expect(CP_UI_MENU, WonderTrade_Text_Choose);
    model->net = CP_UI_NET_BUSY;
    /* In the party list, Up from the first slot reaches Back (cursor 6). */
    Press(CP_UI_KEY_A);
    Press(CP_UI_KEY_UP);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_MENU, WonderTrade_Text_Choose);
    Press(CP_UI_KEY_UP);
    Press(CP_UI_KEY_A);
    Expect(CP_UI_CLOSING, WonderTrade_Text_Disconnecting);
    model->net = CP_UI_NET_CLOSED;
    Frame();
    Expect(CP_UI_SAVE_PICKER, Companion_Text_SavePickerHint);
    ExpectVisibleSave();
    assert(sUi.listCursor == 0);
    Press(CP_UI_KEY_A);
    assert(sFake.lastLoad == 0);

    /* A list that shrinks while a save is open keeps the cursor on it. */
    model->busy = 0;
    model->loadMessage = Companion_Text_SaveInGameFirst;
    Frame();
    Press(CP_UI_KEY_B);
    Press(CP_UI_KEY_UP);
    Press(CP_UI_KEY_UP);
    assert(sUi.listCursor == 5 && sUi.listTop == 2);
    Press(CP_UI_KEY_A);
    Frame();
    Expect(CP_UI_MESSAGE, Companion_Text_SaveInGameFirst);
    model->saveCount = 4;
    Press(CP_UI_KEY_B);
    Expect(CP_UI_SAVE_PICKER, Companion_Text_SavePickerHint);
    ExpectVisibleSave();
}

int main(void)
{
    TestSaveCursorSurvivesThePicker();
    TestBootAndSaves();
    TestTouchExitFromList();
    TestRecovery();
    TestConnecting();
    TestOutcomes();
    TestMenu();
    TestPicker();
    TestPickerTouch();
    TestTextIds();
    puts("ui_state_test: ok");
    return 0;
}
