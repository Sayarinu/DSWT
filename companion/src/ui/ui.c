#include "ui/ui.h"

#include <string.h>

#include "game/rom.h"
#include "wonder_trade/journal.h"
#include "wonder_trade/menu_input.h"
#include "wonder_trade/wire.h"

_Static_assert(CP_UI_MAX_ACTIONS == WT_MENU_MAX_ACTIONS, "the game's button count");

static int Pending(const CpUiModel *model)
{
    return model->tradePhase != WT_JOURNAL_IDLE;
}

static void Show(CpUi *ui, CpUiState state, int message)
{
    ui->state = state;
    ui->message = message;
    ui->cursor = 0;
    ui->touchBlocked = 1;
    ui->dirty = 1;
}

static void ShowMessage(CpUi *ui, int message, CpUiState back)
{
    ui->messageReturn = back;
    Show(ui, CP_UI_MESSAGE, message);
}

static int MenuMessage(const CpUiModel *model)
{
    return Pending(model) ? WonderTrade_Text_Queued : WonderTrade_Text_Choose;
}

/* Keep the highlighted save a real one and on a visible row, whatever the
 * list's length; returns nonzero when that moved it. */
static int ClampSaves(CpUi *ui, const CpUiModel *model)
{
    int count = (int)model->saveCount, cursor = ui->listCursor, top = ui->listTop;
    if (cursor >= count) cursor = count - 1;
    if (cursor < 0) cursor = 0;
    if (top > cursor) top = cursor;
    if (cursor >= top + CP_UI_SAVE_ROWS) top = cursor - CP_UI_SAVE_ROWS + 1;
    if (top < 0) top = 0;
    if (cursor == ui->listCursor && top == ui->listTop) return 0;
    ui->listCursor = cursor;
    ui->listTop = top;
    return 1;
}

static void ShowSaves(CpUi *ui, CpUiModel *model)
{
    ClampSaves(ui, model);
    Show(ui, CP_UI_SAVE_PICKER, model->saveCount ? Companion_Text_SavePickerHint : Companion_Text_NoSaves);
}

void CpUi_Start(CpUi *ui, CpUiModel *model)
{
    memset(ui, 0, sizeof(*ui));
    ui->selectedSlot = -1;
    Show(ui, CP_UI_BOOT, WonderTrade_Text_CheckingSave);
    model->ops->scan(model->context);
}

CompanionLanguage CpUi_GameLanguage(unsigned language, CompanionLanguage fallback)
{
    switch (language) {
    case CP_LANGUAGE_JAPANESE: return Companion_Language_ja_JP;
    case CP_LANGUAGE_ENGLISH: return Companion_Language_en_US;
    case CP_LANGUAGE_FRENCH: return Companion_Language_fr_FR;
    case CP_LANGUAGE_ITALIAN: return Companion_Language_it_IT;
    case CP_LANGUAGE_GERMAN: return Companion_Language_de_DE;
    case CP_LANGUAGE_SPANISH: return Companion_Language_es_ES;
    case CP_LANGUAGE_KOREAN: return Companion_Language_ko_KR;
    default: return fallback;
    }
}

CompanionLanguage CpUi_Language(const CpUi *ui, const CpUiModel *model)
{
    int chosen = ui->state != CP_UI_SAVE_PICKER && ui->state != CP_UI_EXIT
        && !(ui->state == CP_UI_BOOT && !ui->loading)
        && !(ui->state == CP_UI_MESSAGE && ui->messageReturn == CP_UI_SAVE_PICKER);
    return chosen ? CpUi_GameLanguage(model->gameLanguage, model->systemLanguage) : model->systemLanguage;
}

int CpUi_Actions(const CpUi *ui, const CpUiModel *model, int labels[CP_UI_MAX_ACTIONS])
{
    switch (ui->state) {
    case CP_UI_SAVE_PICKER:
        if (!model->saveCount) {
            labels[0] = WonderTrade_Text_Retry;
            labels[1] = WonderTrade_Text_Exit;
            return 2;
        }
        labels[0] = WonderTrade_Text_ConfirmAction;
        labels[1] = WonderTrade_Text_Exit;
        return 2;
    case CP_UI_MENU:
        labels[0] = Pending(model) ? WonderTrade_Text_Collect : WonderTrade_Text_Trade;
        labels[1] = Pending(model) ? WonderTrade_Text_CancelOffer : WonderTrade_Text_Info;
        labels[2] = WonderTrade_Text_Exit;
        return 3;
    case CP_UI_GAME_CONFIRM:
    case CP_UI_CONFIRM:
    case CP_UI_COLLECT_CONFIRM:
    case CP_UI_CANCEL_CONFIRM:
        labels[0] = WonderTrade_Text_ConfirmAction;
        labels[1] = WonderTrade_Text_Back;
        return 2;
    case CP_UI_ERROR:
        labels[0] = WonderTrade_Text_Retry;
        if (!model->canLeave) return 1;
        labels[1] = WonderTrade_Text_Exit;
        return 2;
    case CP_UI_MESSAGE:
    case CP_UI_RECEIVED:
        labels[0] = WonderTrade_Text_Back;
        return 1;
    case CP_UI_RECOVERY:
        labels[0] = WonderTrade_Text_RecoveryAction;
        return 1;
    case CP_UI_CONNECTING:
    case CP_UI_PINGING:
        if (!model->canLeave) return 0;
        labels[0] = WonderTrade_Text_Cancel;
        return 1;
    default:
        /* PICKER draws its own panels and Back button. */
        return 0;
    }
}

enum CpUiCard CpUi_Card(const CpUi *ui, const CpUiModel *model)
{
    switch (ui->state) {
    case CP_UI_SAVE_PICKER:
        return model->saveCount ? CP_UI_CARD_SAVES : CP_UI_CARD_NONE;
    case CP_UI_PICKER:
        return ui->partyCursor < (int)model->partyCount ? CP_UI_CARD_PARTY : CP_UI_CARD_NONE;
    case CP_UI_CONFIRM:
        return ui->selectedSlot >= 0 && ui->selectedSlot < (int)model->partyCount ? CP_UI_CARD_PARTY : CP_UI_CARD_NONE;
    case CP_UI_RECEIVED:
        return CP_UI_CARD_RECEIVED;
    case CP_UI_ERROR:
    case CP_UI_RECOVERY:
        return CP_UI_CARD_FULL_TEXT;
    case CP_UI_MESSAGE:
        return ui->message == WonderTrade_Text_Explanation ? CP_UI_CARD_FULL_TEXT : CP_UI_CARD_NONE;
    default:
        return CP_UI_CARD_NONE;
    }
}

int CpUi_Busy(const CpUi *ui)
{
    return ui->state == CP_UI_BOOT || ui->state == CP_UI_CONNECTING || ui->state == CP_UI_PINGING
        || ui->state == CP_UI_RESTART || ui->state == CP_UI_CLOSING;
}

int CpUi_IneligibleText(unsigned reason)
{
    switch (reason) {
    case CP_UI_ELIGIBLE: return -1;
    case CP_UI_INELIGIBLE_EGG: return Companion_Text_ReasonEgg;
    case CP_UI_INELIGIBLE_MAIL: return Companion_Text_ReasonMail;
    case CP_UI_INELIGIBLE_CAPSULE: return Companion_Text_ReasonCapsule;
    case CP_UI_INELIGIBLE_LAST_USABLE: return Companion_Text_ReasonLastUsable;
    case CP_UI_INELIGIBLE_DUPLICATE: return Companion_Text_ReasonDuplicate;
    default: return Companion_Text_ReasonOther;
    }
}

int CpUi_PickerTouch(int x, int y, unsigned partyCount)
{
    int column, row, slot;
    if (x >= CP_UI_PICKER_BACK_LEFT && x < CP_UI_PICKER_BACK_LEFT + CP_UI_PICKER_BACK_WIDTH
        && y >= CP_UI_PICKER_BACK_TOP && y < CP_UI_PICKER_BACK_TOP + CP_UI_PICKER_BACK_HEIGHT) return CP_UI_PICKER_BACK;
    if (x < CP_UI_PANEL_LEFT || y < CP_UI_PANEL_TOP) return -1;
    column = (x - CP_UI_PANEL_LEFT) / CP_UI_PANEL_STEP_X;
    row = (y - CP_UI_PANEL_TOP) / CP_UI_PANEL_STEP_Y;
    if (column > 1 || row > 2 || (x - CP_UI_PANEL_LEFT) % CP_UI_PANEL_STEP_X >= CP_UI_PANEL_WIDTH
        || (y - CP_UI_PANEL_TOP) % CP_UI_PANEL_STEP_Y >= CP_UI_PANEL_HEIGHT) return -1;
    slot = row * 2 + column;
    return slot < (int)partyCount ? slot : -1;
}

/* Says why the server refused or handed back the offer, when it said. */
static int RefusalText(unsigned refusal, int fallback)
{
    switch (refusal) {
    case WT_REFUSAL_EGG: return WonderTrade_Text_RefusedEgg;
    case WT_REFUSAL_MAIL: return WonderTrade_Text_RefusedMail;
    case WT_REFUSAL_INCOMPATIBLE: return WonderTrade_Text_RefusedGame;
    case WT_REFUSAL_ILLEGAL: return WonderTrade_Text_RefusedIllegal;
    case WT_REFUSAL_NAME: return WonderTrade_Text_RefusedName;
    default: return fallback;
    }
}

static void Close(CpUi *ui, CpUiModel *model)
{
    if (!model->canLeave) return;
    model->ops->close(model->context);
    Show(ui, CP_UI_CLOSING, WonderTrade_Text_Disconnecting);
}

static void Connect(CpUi *ui, CpUiModel *model)
{
    model->ops->connect(model->context);
    Show(ui, CP_UI_CONNECTING, WonderTrade_Text_Connecting);
}

static void Back(CpUi *ui, CpUiModel *model)
{
    switch (ui->state) {
    case CP_UI_SAVE_PICKER:
        Show(ui, CP_UI_EXIT, WonderTrade_Text_Disconnecting);
        break;
    case CP_UI_GAME_CONFIRM:
        model->ops->unload(model->context);
        ShowSaves(ui, model);
        break;
    case CP_UI_RECOVERY:
        /* The game offers only "Connect and retry" here. */
        break;
    case CP_UI_PICKER:
        Show(ui, CP_UI_MENU, WonderTrade_Text_Choose);
        break;
    case CP_UI_CONFIRM:
        ui->selectedSlot = -1;
        Show(ui, CP_UI_PICKER, WonderTrade_Text_Choose);
        break;
    case CP_UI_COLLECT_CONFIRM:
    case CP_UI_CANCEL_CONFIRM:
        Show(ui, CP_UI_MENU, WonderTrade_Text_Queued);
        break;
    case CP_UI_MESSAGE:
    case CP_UI_RECEIVED:
        if (ui->state == CP_UI_MESSAGE && ui->messageReturn == CP_UI_SAVE_PICKER) {
            model->ops->unload(model->context);
            ShowSaves(ui, model);
        } else if (ui->state == CP_UI_MESSAGE && ui->messageReturn == CP_UI_PICKER) {
            Show(ui, CP_UI_PICKER, WonderTrade_Text_Choose);
        } else if (!model->needsEntropy) {
            Show(ui, CP_UI_MENU, MenuMessage(model));
        } else {
            Close(ui, model);
        }
        break;
    default:
        Close(ui, model);
        break;
    }
}

static void Pick(CpUi *ui, CpUiModel *model, int slot)
{
    const CpUiPokemon *mon;
    if (slot == CP_UI_PICKER_BACK) {
        Back(ui, model);
        return;
    }
    if (slot < 0 || slot >= (int)model->partyCount) return;
    mon = &model->party[slot];
    if (mon->ineligible == CP_UI_INELIGIBLE_DUPLICATE) {
        ShowMessage(ui, WonderTrade_Text_Duplicate, CP_UI_PICKER);
    } else if (mon->ineligible != CP_UI_ELIGIBLE) {
        ShowMessage(ui, WonderTrade_Text_Ineligible, CP_UI_PICKER);
    } else {
        ui->selectedSlot = slot;
        Show(ui, CP_UI_CONFIRM, WonderTrade_Text_Confirm);
    }
}

static void Activate(CpUi *ui, CpUiModel *model, int choice)
{
    switch (ui->state) {
    case CP_UI_SAVE_PICKER:
        if (choice == 1) {
            Back(ui, model);
        } else if (!model->saveCount) {
            Show(ui, CP_UI_BOOT, WonderTrade_Text_CheckingSave);
            ui->loading = 0;
            model->ops->scan(model->context);
        } else {
            ui->loading = 1;
            model->ops->load(model->context, (unsigned)ui->listCursor);
            Show(ui, CP_UI_BOOT, WonderTrade_Text_CheckingSave);
        }
        break;
    case CP_UI_GAME_CONFIRM:
        if (choice == 1) Back(ui, model);
        else if (model->recovery) Show(ui, CP_UI_RECOVERY, WonderTrade_Text_RecoveryRequired);
        else Connect(ui, model);
        break;
    case CP_UI_RECOVERY:
        Connect(ui, model);
        break;
    case CP_UI_CONNECTING:
    case CP_UI_PINGING:
        Close(ui, model);
        break;
    case CP_UI_CONFIRM:
    case CP_UI_COLLECT_CONFIRM:
    case CP_UI_CANCEL_CONFIRM:
        if (choice == 1) {
            Back(ui, model);
        } else {
            if (ui->state == CP_UI_CONFIRM) model->ops->trade(model->context, ui->selectedSlot);
            else model->ops->collect(model->context, ui->state == CP_UI_CANCEL_CONFIRM);
            Show(ui, CP_UI_PINGING, WonderTrade_Text_Pinging);
        }
        break;
    case CP_UI_MESSAGE:
    case CP_UI_RECEIVED:
        Back(ui, model);
        break;
    case CP_UI_ERROR:
        if (choice == 0) {
            model->ops->close(model->context);
            Show(ui, CP_UI_RESTART, WonderTrade_Text_Disconnecting);
        } else {
            Close(ui, model);
        }
        break;
    case CP_UI_MENU:
        if (choice == 0) {
            if (!Pending(model)) {
                ui->partyCursor = 0;
                Show(ui, CP_UI_PICKER, WonderTrade_Text_Choose);
            } else {
                Show(ui, CP_UI_COLLECT_CONFIRM, WonderTrade_Text_ConfirmCollect);
            }
        } else if (choice == 1) {
            if (!Pending(model)) ShowMessage(ui, WonderTrade_Text_Explanation, CP_UI_MENU);
            else Show(ui, CP_UI_CANCEL_CONFIRM, WonderTrade_Text_ConfirmCancel);
        } else {
            Close(ui, model);
        }
        break;
    default:
        break;
    }
}

/* The game's WT_NET_RESPONSE handling (application.c 815-840). */
static void Respond(CpUi *ui, CpUiModel *model)
{
    ui->selectedSlot = -1;
    if (model->needsEntropy) ShowMessage(ui, WonderTrade_Text_KeepPlaying, CP_UI_MENU);
    else if (model->completed && model->returned)
        ShowMessage(ui, RefusalText(model->refusal, WonderTrade_Text_Returned), CP_UI_MENU);
    else if (model->completed) {
        /* The companion has no trade animation: the card shows at once. */
        if (model->hasReceived) Show(ui, CP_UI_RECEIVED, WonderTrade_Text_Completed);
        else ShowMessage(ui, WonderTrade_Text_Completed, CP_UI_MENU);
    } else if (model->rejected)
        ShowMessage(ui, RefusalText(model->refusal, WonderTrade_Text_Rejected), CP_UI_MENU);
    else if (model->superseded && !Pending(model)) ShowMessage(ui, WonderTrade_Text_Superseded, CP_UI_MENU);
    else if (model->refused) ShowMessage(ui, WonderTrade_Text_CannotHold, CP_UI_MENU);
    else if (model->waiting) ShowMessage(ui, WonderTrade_Text_Waiting, CP_UI_MENU);
    else if (model->tradePhase == WT_JOURNAL_RECEIVED || model->tradePhase == WT_JOURNAL_RESTORING)
        Show(ui, CP_UI_MENU, WonderTrade_Text_NoRoom);
    else Show(ui, CP_UI_MENU, MenuMessage(model));
}

static void MoveList(CpUi *ui, int count, int step)
{
    if (count <= 0) return;
    ui->listCursor = (ui->listCursor + step + count) % count;
    if (ui->listCursor < ui->listTop) ui->listTop = ui->listCursor;
    if (ui->listCursor >= ui->listTop + CP_UI_SAVE_ROWS) ui->listTop = ui->listCursor - CP_UI_SAVE_ROWS + 1;
    ui->sounds++;
    ui->dirty = 1;
}

static void PickerInput(CpUi *ui, CpUiModel *model, const CpUiInput *input)
{
    int count = (int)model->partyCount, touched = -1, step = 0;
    if (!input->touchHeld) ui->touchBlocked = 0;
    else if (!ui->touchBlocked && input->touchPressed) touched = CpUi_PickerTouch(input->touchX, input->touchY, model->partyCount);
    if (touched >= 0) {
        ui->sounds++;
        ui->partyCursor = touched;
        Pick(ui, model, touched);
        return;
    }
    if (input->pressed & (CP_UI_KEY_B | CP_UI_KEY_X)) {
        Back(ui, model);
        return;
    }
    if (input->pressed & CP_UI_KEY_A) {
        ui->sounds++;
        Pick(ui, model, ui->partyCursor);
        return;
    }
    if (input->pressed & CP_UI_KEY_LEFT) step = -1;
    else if (input->pressed & CP_UI_KEY_RIGHT) step = 1;
    else if (input->pressed & CP_UI_KEY_UP) step = -2;
    else if (input->pressed & CP_UI_KEY_DOWN) step = 2;
    if (!step) return;
    /* The party slots and Back, in reading order. */
    if (ui->partyCursor == CP_UI_PICKER_BACK) ui->partyCursor = step < 0 ? count - 1 : 0;
    else if (ui->partyCursor + step >= count || ui->partyCursor + step < 0) ui->partyCursor = CP_UI_PICKER_BACK;
    else ui->partyCursor += step;
    if (ui->partyCursor < 0) ui->partyCursor = CP_UI_PICKER_BACK;
    ui->sounds++;
    ui->dirty = 1;
}

static void HandleInput(CpUi *ui, CpUiModel *model, const CpUiInput *input)
{
    int labels[CP_UI_MAX_ACTIONS];
    int count, touched;
    if (ui->state == CP_UI_PICKER) {
        PickerInput(ui, model, input);
        return;
    }
    count = CpUi_Actions(ui, model, labels);
    touched = WtMenu_Touch(&ui->touchBlocked, input->touchPressed, input->touchHeld,
        input->touchX, input->touchY, count);
    if (ui->state != CP_UI_RECOVERY && (input->pressed & (CP_UI_KEY_B | CP_UI_KEY_X))) {
        if (ui->state == CP_UI_MESSAGE && (input->pressed & CP_UI_KEY_X) && ui->messageReturn == CP_UI_MENU) Close(ui, model);
        else Back(ui, model);
        return;
    }
    if (!count) return;
    if (touched >= 0) {
        ui->sounds++;
        ui->cursor = touched;
        Activate(ui, model, touched);
    } else if (ui->state == CP_UI_SAVE_PICKER && model->saveCount && (input->pressed & (CP_UI_KEY_UP | CP_UI_KEY_DOWN))) {
        /* Up and Down choose a save; A confirms it, B leaves. */
        MoveList(ui, (int)model->saveCount, (input->pressed & CP_UI_KEY_UP) ? -1 : 1);
    } else if (input->pressed & (CP_UI_KEY_UP | CP_UI_KEY_DOWN)) {
        ui->cursor = (ui->cursor + ((input->pressed & CP_UI_KEY_UP) ? count - 1 : 1)) % count;
        ui->sounds++;
        ui->dirty = 1;
    } else if (input->pressed & CP_UI_KEY_A) {
        ui->sounds++;
        Activate(ui, model, ui->cursor);
    }
}

void CpUi_Frame(CpUi *ui, CpUiModel *model, const CpUiInput *input)
{
    int labels[CP_UI_MAX_ACTIONS];
    model->ops->update(model->context);
    /* Cancel and Exit come and go with canLeave. */
    if (ui->shownCanLeave != model->canLeave) {
        ui->shownCanLeave = model->canLeave;
        ui->dirty = 1;
    }

    switch (ui->state) {
    case CP_UI_BOOT:
        if (model->busy) return;
        if (!ui->loading) {
            ShowSaves(ui, model);
        } else {
            ui->loading = 0;
            if (model->loadMessage >= 0) ShowMessage(ui, model->loadMessage, CP_UI_SAVE_PICKER);
            else Show(ui, CP_UI_GAME_CONFIRM, Companion_Text_GameConfirm);
        }
        return;
    case CP_UI_EXIT:
        return;
    case CP_UI_SAVE_PICKER:
        if (ClampSaves(ui, model)) ui->dirty = 1;
        break;
    case CP_UI_CLOSING:
        if (model->net == CP_UI_NET_CLOSED) {
            model->ops->unload(model->context);
            ShowSaves(ui, model);
        }
        return;
    case CP_UI_RESTART:
        if (model->net == CP_UI_NET_CLOSED) Connect(ui, model);
        return;
    case CP_UI_CONNECTING:
    case CP_UI_PINGING:
        HandleInput(ui, model, input);
        if (ui->state == CP_UI_CLOSING) return;
        if (model->net == CP_UI_NET_ERROR) {
            Show(ui, CP_UI_ERROR, ui->state == CP_UI_CONNECTING ? WonderTrade_Text_ConnectionError : WonderTrade_Text_PingError);
        } else if (ui->state == CP_UI_CONNECTING && model->net == CP_UI_NET_READY) {
            model->ops->trade(model->context, -1);
            Show(ui, CP_UI_PINGING, WonderTrade_Text_Pinging);
        } else if (ui->state == CP_UI_PINGING && model->net == CP_UI_NET_RESPONSE) {
            Respond(ui, model);
        }
        return;
    default:
        break;
    }
    HandleInput(ui, model, input);
    if (ui->cursor >= CpUi_Actions(ui, model, labels)) ui->cursor = 0;
}
