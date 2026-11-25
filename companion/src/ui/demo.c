#include "ui/demo.h"

#include <stdio.h>
#include <string.h>

#include "game/rom.h"
#include "ui/canvas.h"
#include "wonder_trade/journal.h"
#include "wonder_trade/wire.h"

enum { JOB_NONE, JOB_SCAN, JOB_LOAD, JOB_CONNECT, JOB_RESUME, JOB_TRADE, JOB_COLLECT, JOB_CANCEL, JOB_CLOSE };

/* Frames each made-up job takes. */
#define SCAN_FRAMES 90
#define LOAD_FRAMES 60
#define CONNECT_FRAMES 90
#define EXCHANGE_FRAMES 90
#define CLOSE_FRAMES 45
#define DEMO_ERROR_CODE (-12)
/* The real model's codes for "no saved network worked" and a rejected join. */
#define DEMO_WIFI_CODE (-20)
#define DEMO_JOIN_CODE (-26)
#define WIFI_FIRST_FRAMES 45
#define WIFI_ALL_FRAMES 90
#define JOIN_FRAMES 90

static const struct {
    const char *ssid;
    uint8_t security, bars;
} kNetworks[] = {
    { "Lune Home", CP_WIFI_SECURITY_WPA2, 3 },
    { "Lune Guest", CP_WIFI_SECURITY_OPEN, 2 },
    { "OldRouter-TKIP", CP_WIFI_SECURITY_WPA, 2 },
    { "Campus-Secure", CP_WIFI_SECURITY_ENTERPRISE, 1 },
    { "Retro DS", CP_WIFI_SECURITY_WEP, 3 },
    { "Caf\xc3\xa9 Wi-Fi", CP_WIFI_SECURITY_WPA2, 1 },
    { "Neighbours-5G-Extended-Network", CP_WIFI_SECURITY_WPA2, 0 },
};

static const struct {
    const char *name;
    uint8_t status, version, language;
} kSaves[] = {
    { "Pokemon Platinum.sav", CP_UI_SAVE_READY, CP_VERSION_PLATINUM, CP_LANGUAGE_ENGLISH },
    { "Pokemon Diamond.sav", CP_UI_SAVE_NO_ROM, 0, 0 },
    { "Pokemon HeartGold (JP).sav", CP_UI_SAVE_READY, CP_VERSION_HEARTGOLD, CP_LANGUAGE_JAPANESE },
};

static const struct {
    uint16_t species;
    const char *speciesName, *nickname;
    uint8_t level, gender, shiny, isEgg, ineligible;
    uint16_t item;
    const char *itemName;
} kParty[] = {
    { 387, "TURTWIG", "TURTWIG", 6, CP_UI_MALE, 0, 0, CP_UI_ELIGIBLE, 155, "Oran Berry" },
    { 396, "STARLY", "Pip", 2, CP_UI_FEMALE, 0, 0, CP_UI_ELIGIBLE, 0, "" },
    { 0, "EGG", "Egg", 1, CP_UI_GENDERLESS, 0, 1, CP_UI_INELIGIBLE_EGG, 0, "" },
    { 403, "SHINX", "SHINX", 9, CP_UI_MALE, 0, 0, CP_UI_INELIGIBLE_MAIL, 137, "Grass Mail" },
    { 399, "BIDOOF", "BIDOOF", 4, CP_UI_FEMALE, 0, 0, CP_UI_INELIGIBLE_DUPLICATE, 0, "" },
    { 401, "KRICKETOT", "Cricket", 5, CP_UI_MALE, 1, 0, CP_UI_ELIGIBLE, 217, "Quick Claw" },
};

static void Begin(CpUiDemo *demo, int job, unsigned frames)
{
    demo->job = job;
    demo->due = demo->frame + frames;
    if (job != JOB_SCAN && job != JOB_LOAD) demo->model.net = CP_UI_NET_BUSY;
}

static void Scan(void *context)
{
    CpUiDemo *demo = context;
    demo->model.busy = 1;
    Begin(demo, JOB_SCAN, SCAN_FRAMES);
}

static void Load(void *context, unsigned index)
{
    CpUiDemo *demo = context;
    demo->model.busy = 1;
    demo->loading = index;
    Begin(demo, JOB_LOAD, LOAD_FRAMES);
}

static void Unload(void *context)
{
    CpUiDemo *demo = context;
    demo->model.partyCount = 0;
    demo->model.loadMessage = -1;
    demo->model.gameVersion = demo->model.gameLanguage = 0;
}

static void Connect(void *context)
{
    CpUiDemo *demo = context;
    demo->model.canLeave = 1;
    demo->model.wifiError = 0;
    Begin(demo, JOB_CONNECT, CONNECT_FRAMES);
}

/* ---- Wi-Fi settings ---------------------------------------------------------- */

static void WifiScan(void *context)
{
    CpUiDemo *demo = context;
    demo->joining = 0;
    demo->model.wifi.state = CP_UI_NETWORK_SCANNING;
    demo->model.wifi.count = 0;
    demo->model.wifi.revision++;
    demo->scanStart = demo->frame;
}

static void WifiJoin(void *context, const char *ssid, unsigned security, const char *key)
{
    CpUiDemo *demo = context;
    (void)security;
    (void)key;
    snprintf(demo->model.wifi.savedSsid, sizeof(demo->model.wifi.savedSsid), "%s", ssid);
    demo->model.wifi.state = CP_UI_NETWORK_JOINING;
    demo->joining = 1;
    demo->joinDue = demo->frame + JOIN_FRAMES;
}

static void WifiForget(void *context)
{
    CpUiDemo *demo = context;
    demo->model.wifi.saved = 0;
    demo->model.wifi.savedSsid[0] = 0;
}

static void WifiLeave(void *context)
{
    CpUiDemo *demo = context;
    demo->joining = 0;
    demo->model.wifi.state = CP_UI_NETWORK_IDLE;
}

static void WifiUpdate(CpUiDemo *demo)
{
    CpUiModel *model = &demo->model;
    if (model->wifi.state == CP_UI_NETWORK_SCANNING) {
        unsigned elapsed = demo->frame - demo->scanStart, count = 0, i;
        if (elapsed >= WIFI_ALL_FRAMES) count = sizeof(kNetworks) / sizeof(*kNetworks);
        else if (elapsed >= WIFI_FIRST_FRAMES) count = 3;
        if (count != model->wifi.count) {
            for (i = 0; i < count; i++) {
                memset(&model->wifi.networks[i], 0, sizeof(model->wifi.networks[i]));
                snprintf(model->wifi.networks[i].ssid, sizeof(model->wifi.networks[i].ssid), "%s", kNetworks[i].ssid);
                model->wifi.networks[i].security = kNetworks[i].security;
                model->wifi.networks[i].bars = kNetworks[i].bars;
            }
            model->wifi.count = count;
            model->wifi.revision++;
        }
    }
    if (demo->joining && (int)(demo->frame - demo->joinDue) >= 0) {
        demo->joining = 0;
        if (demo->failJoin) {
            demo->failJoin = 0;
            model->wifi.state = CP_UI_NETWORK_FAILED;
            model->wifi.failure = CP_WIFI_FAILURE_REJECTED;
            model->wifi.code = DEMO_JOIN_CODE;
            model->wifi.saved = 0;
        } else {
            model->wifi.state = CP_UI_NETWORK_JOINED;
            model->wifi.saved = 1;
            model->wifi.saveFailed = 0;
        }
    }
}

static void ClearOutcome(CpUiModel *model)
{
    model->completed = model->returned = model->rejected = model->waiting = 0;
    model->superseded = model->needsEntropy = model->refused = model->hasReceived = 0;
    model->refusal = WT_REFUSAL_NONE;
}

static void Trade(void *context, int slot)
{
    CpUiDemo *demo = context;
    ClearOutcome(&demo->model);
    /* An offer cannot be abandoned once it is under way, as in the game. */
    demo->model.canLeave = slot < 0;
    Begin(demo, slot < 0 ? JOB_RESUME : JOB_TRADE, EXCHANGE_FRAMES);
}

static void Collect(void *context, int cancel)
{
    CpUiDemo *demo = context;
    ClearOutcome(&demo->model);
    demo->model.canLeave = 0;
    Begin(demo, cancel ? JOB_CANCEL : JOB_COLLECT, EXCHANGE_FRAMES);
}

static void Close(void *context)
{
    CpUiDemo *demo = context;
    Begin(demo, JOB_CLOSE, CLOSE_FRAMES);
}

static void LoadSave(CpUiDemo *demo)
{
    CpUiModel *model = &demo->model;
    unsigned i;
    model->busy = 0;
    model->loadMessage = -1;
    if (kSaves[demo->loading].status == CP_UI_SAVE_NO_ROM) {
        model->loadMessage = Companion_Text_MatchingRomMissing;
        return;
    }
    model->gameVersion = kSaves[demo->loading].version;
    model->gameLanguage = kSaves[demo->loading].language;
    model->recovery = demo->loading == 2;
    model->tradePhase = WT_JOURNAL_IDLE;
    model->partyCount = sizeof(kParty) / sizeof(*kParty);
    for (i = 0; i < model->partyCount; i++) {
        CpUiPokemon *mon = &model->party[i];
        memset(mon, 0, sizeof(*mon));
        mon->species = kParty[i].species;
        mon->level = kParty[i].level;
        mon->gender = kParty[i].gender;
        mon->shiny = kParty[i].shiny;
        mon->isEgg = kParty[i].isEgg;
        mon->ineligible = kParty[i].ineligible;
        mon->heldItem = kParty[i].item;
        CpGameText_FromAscii(mon->speciesName, CP_UI_NAME_MAX, kParty[i].speciesName);
        CpGameText_FromAscii(mon->nickname, CP_UI_NAME_MAX, kParty[i].nickname);
        CpGameText_FromAscii(mon->itemName, CP_UI_ITEM_NAME_MAX, kParty[i].itemName);
    }
}

/* What the made-up server answers. */
static void Answer(CpUiDemo *demo, int job)
{
    CpUiModel *model = &demo->model;
    int outcome = demo->outcome;
    demo->outcome = (demo->outcome + 1) % CP_UI_DEMO_OUTCOMES;
    model->net = CP_UI_NET_RESPONSE;
    model->canLeave = 1;
    switch (outcome) {
    case CP_UI_DEMO_WAITING:
        model->waiting = 1;
        model->tradePhase = WT_JOURNAL_QUEUED;
        break;
    case CP_UI_DEMO_COMPLETED:
        model->completed = model->hasReceived = 1;
        model->tradePhase = WT_JOURNAL_IDLE;
        memset(&model->received, 0, sizeof(model->received));
        model->received.species = 161;
        model->received.gender = CP_UI_FEMALE;
        model->received.shiny = 1;
        model->received.hasNickname = 1;
        CpGameText_FromAscii(model->received.speciesName, CP_UI_NAME_MAX, "SENTRET");
        CpGameText_FromAscii(model->received.nickname, CP_UI_NAME_MAX, "Scout");
        CpGameText_FromAscii(model->received.originalTrainer, CP_UI_TRAINER_MAX, "LUNE");
        break;
    case CP_UI_DEMO_RETURNED:
        model->completed = model->returned = 1;
        model->tradePhase = WT_JOURNAL_IDLE;
        break;
    case CP_UI_DEMO_REJECTED:
        model->rejected = 1;
        model->refusal = WT_REFUSAL_ILLEGAL;
        model->tradePhase = WT_JOURNAL_IDLE;
        break;
    case CP_UI_DEMO_SUPERSEDED:
        model->superseded = 1;
        model->tradePhase = WT_JOURNAL_IDLE;
        break;
    case CP_UI_DEMO_NO_ROOM:
        model->tradePhase = WT_JOURNAL_RECEIVED;
        break;
    case CP_UI_DEMO_KEEP_PLAYING:
        model->needsEntropy = 1;
        break;
    case CP_UI_DEMO_CANNOT_HOLD:
        model->refused = 1;
        model->tradePhase = WT_JOURNAL_QUEUED;
        break;
    default:
        model->net = CP_UI_NET_ERROR;
        model->error = DEMO_ERROR_CODE;
        /* An offer in flight keeps the player here until it is resolved. */
        model->canLeave = job == JOB_RESUME;
        break;
    }
}

static void Update(void *context)
{
    CpUiDemo *demo = context;
    CpUiModel *model = &demo->model;
    unsigned i;
    int job = demo->job;
    demo->frame++;
    WifiUpdate(demo);
    if (job == JOB_NONE || (int)(demo->frame - demo->due) < 0) return;
    demo->job = JOB_NONE;
    switch (job) {
    case JOB_SCAN:
        model->busy = 0;
        model->saveCount = demo->noSaves ? 0 : sizeof(kSaves) / sizeof(*kSaves);
        demo->noSaves = 0;
        for (i = 0; i < model->saveCount; i++) {
            memset(&model->saves[i], 0, sizeof(model->saves[i]));
            strncpy(model->saves[i].name, kSaves[i].name, CP_UI_SAVE_NAME_MAX - 1);
            model->saves[i].status = kSaves[i].status;
            model->saves[i].version = kSaves[i].version;
            model->saves[i].language = kSaves[i].language;
        }
        break;
    case JOB_LOAD:
        LoadSave(demo);
        break;
    case JOB_CONNECT:
        if (demo->failWifi) {
            demo->failWifi = 0;
            model->net = CP_UI_NET_ERROR;
            model->error = DEMO_WIFI_CODE;
            model->wifiError = 1;
        } else if (demo->failConnect) {
            demo->failConnect = 0;
            model->net = CP_UI_NET_ERROR;
            model->error = DEMO_ERROR_CODE;
        } else {
            model->net = CP_UI_NET_READY;
        }
        break;
    case JOB_RESUME:
        ClearOutcome(model);
        model->net = CP_UI_NET_RESPONSE;
        break;
    case JOB_CLOSE:
        model->net = CP_UI_NET_CLOSED;
        model->canLeave = 1;
        break;
    default:
        Answer(demo, job);
        break;
    }
}

static const CpUiOps kOps = { Scan, Load, Unload, Connect, Trade, Collect, Close, Update,
    WifiScan, WifiJoin, WifiForget, WifiLeave };

void CpUiDemo_Init(CpUiDemo *demo, CompanionLanguage systemLanguage)
{
    memset(demo, 0, sizeof(*demo));
    demo->model.ops = &kOps;
    demo->model.context = demo;
    demo->model.systemLanguage = systemLanguage;
    demo->model.loadMessage = -1;
    demo->model.canLeave = 1;
    demo->model.tradePhase = WT_JOURNAL_IDLE;
    /* The demo pretends DSi mode, so WPA2 networks can be joined. */
    demo->model.wifi.wpa2 = 1;
}

void CpUiDemo_SetOutcome(CpUiDemo *demo, enum CpUiDemoOutcome outcome)
{
    demo->outcome = outcome;
}

/* The walk: press a key (or nothing), optionally pick the next outcome, then
 * wait `hold` frames for the screens to settle and be seen. */
enum { NONE = -1, FAIL_CONNECT = -2, FAIL_WIFI = -3, FAIL_JOIN = -4 };
/* A tap on a key of the password keyboard, in place of buttons. */
#define TAP_FLAG 0x10000u
#define TAP(index) (TAP_FLAG | (unsigned)(index))
enum { KEY_P = 19, KEY_O = 18, KEY_K = 27, KEY_E = 12, KEY_M = 36, KEY_N = 35, KEY_1 = 0, KEY_2 = 1 };
static const struct {
    unsigned keys;
    int outcome;
    unsigned hold;
} kTour[] = {
    { 0, NONE, 240 },                   /* BOOT, then the empty save list */
    /* Wi-Fi settings (LUNE-875). */
    { CP_UI_KEY_DOWN, NONE, 60 },       /* the Wi-Fi settings button */
    { CP_UI_KEY_A, NONE, 150 },         /* searching, then the networks */
    { CP_UI_KEY_DOWN, NONE, 60 },       /* an open network */
    { CP_UI_KEY_DOWN, NONE, 150 },      /* WPA (TKIP): grey, with its reason */
    { CP_UI_KEY_A, NONE, 150 },         /* the reason, in full */
    { CP_UI_KEY_B, NONE, 30 },
    { CP_UI_KEY_DOWN, NONE, 150 },      /* 802.1X */
    { CP_UI_KEY_DOWN, NONE, 30 },
    { CP_UI_KEY_DOWN, NONE, 30 },
    { CP_UI_KEY_DOWN, NONE, 150 },      /* the last rows scroll into view */
    { CP_UI_KEY_DOWN, NONE, 60 },       /* back to the first network */
    { CP_UI_KEY_A, NONE, 150 },         /* the password keyboard */
    { TAP(KEY_P), NONE, 8 }, { TAP(KEY_O), NONE, 8 }, { TAP(KEY_K), NONE, 8 },
    { TAP(KEY_E), NONE, 8 }, { TAP(KEY_M), NONE, 8 }, { TAP(KEY_O), NONE, 8 },
    { TAP(KEY_N), NONE, 150 },       /* seven characters: Connect is grey */
    { TAP(CP_KEY_INDEX_JOIN), NONE, 150 }, /* "WPA2 passwords have 8 to 63 characters" */
    { TAP(KEY_1), NONE, 8 },
    { TAP(CP_KEY_INDEX_SHOW), NONE, 150 }, /* the password shown */
    { TAP(CP_KEY_INDEX_SHIFT), NONE, 60 },
    { TAP(CP_KEY_INDEX_SYMBOLS), NONE, 150 }, /* the symbols */
    { TAP(CP_KEY_INDEX_SYMBOLS), NONE, 30 },
    { CP_UI_KEY_UP, NONE, 60 },         /* the D-pad moves the highlight */
    { TAP(CP_KEY_INDEX_JOIN), FAIL_JOIN, 150 }, /* JOINING */
    { 0, NONE, 150 },                   /* WIFI_FAILED: a wrong password, with its code */
    { CP_UI_KEY_A, NONE, 30 },          /* Retry: the keyboard keeps the text */
    { TAP(KEY_2), NONE, 8 },
    { TAP(CP_KEY_INDEX_JOIN), NONE, 240 }, /* JOINING, then JOINED */
    { CP_UI_KEY_A, NONE, 60 },          /* back to the save list */
    { CP_UI_KEY_DOWN, NONE, 30 },
    { CP_UI_KEY_A, NONE, 150 },         /* the list, with the saved network */
    { CP_UI_KEY_RIGHT, NONE, 60 },      /* Forget network */
    { CP_UI_KEY_A, NONE, 150 },         /* "Forget Lune Home?" */
    { CP_UI_KEY_A, NONE, 60 },
    { CP_UI_KEY_B, NONE, 60 },          /* back to the save list */
    { CP_UI_KEY_A, NONE, 240 },         /* Retry: BOOT, then three saves */
    { CP_UI_KEY_DOWN, NONE, 120 },      /* a save without its ROM */
    { CP_UI_KEY_A, NONE, 240 },         /* "Matching ROM not found" */
    { CP_UI_KEY_B, NONE, 60 },
    { CP_UI_KEY_UP, NONE, 60 },
    { CP_UI_KEY_A, NONE, 210 },         /* checking, then "Platinum, English. Correct?" */
    { CP_UI_KEY_A, NONE, 330 },         /* CONNECTING, PINGING, then MENU */
    { CP_UI_KEY_DOWN, NONE, 60 },
    { CP_UI_KEY_A, NONE, 180 },         /* Info */
    { CP_UI_KEY_B, NONE, 60 },
    { CP_UI_KEY_A, NONE, 150 },         /* the party */
    { CP_UI_KEY_RIGHT, NONE, 30 },
    { CP_UI_KEY_RIGHT, NONE, 150 },     /* an Egg, greyed out */
    { CP_UI_KEY_A, NONE, 150 },         /* "Cannot offer this Pokémon." */
    { CP_UI_KEY_B, NONE, 60 },
    { CP_UI_KEY_DOWN, NONE, 120 },      /* a duplicate */
    { CP_UI_KEY_RIGHT, NONE, 150 },     /* a shiny holding an item */
    { CP_UI_KEY_UP, NONE, 30 },
    { CP_UI_KEY_UP, NONE, 30 },
    { CP_UI_KEY_LEFT, NONE, 60 },
    { CP_UI_KEY_A, NONE, 150 },         /* CONFIRM */
    { CP_UI_KEY_A, CP_UI_DEMO_WAITING, 240 },   /* PINGING, then "No trade partner yet" */
    { CP_UI_KEY_B, NONE, 150 },         /* MENU with a pending offer */
    { CP_UI_KEY_A, NONE, 150 },         /* COLLECT_CONFIRM */
    { CP_UI_KEY_A, CP_UI_DEMO_COMPLETED, 240 }, /* the received Pokémon's card */
    { CP_UI_KEY_B, NONE, 60 },
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, CP_UI_DEMO_REJECTED, 240 },  /* "It failed the legality check." */
    { CP_UI_KEY_B, NONE, 30 },
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, CP_UI_DEMO_WAITING, 150 },
    { CP_UI_KEY_B, NONE, 30 },
    { CP_UI_KEY_DOWN, NONE, 30 },
    { CP_UI_KEY_A, NONE, 150 },         /* CANCEL_CONFIRM */
    { CP_UI_KEY_A, CP_UI_DEMO_RETURNED, 240 },  /* "Offer cancelled." */
    { CP_UI_KEY_B, NONE, 30 },
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, CP_UI_DEMO_CANNOT_HOLD, 240 }, /* "This game cannot hold..." */
    { CP_UI_KEY_B, NONE, 30 },
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, CP_UI_DEMO_NO_ROOM, 240 },   /* MENU: "Your result is kept pending." */
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, CP_UI_DEMO_SUPERSEDED, 240 }, /* "Another copy of this save traded first." */
    { CP_UI_KEY_B, NONE, 30 },
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, CP_UI_DEMO_KEEP_PLAYING, 240 }, /* "A little more play is needed" */
    { CP_UI_KEY_B, NONE, 180 },         /* closing, back to the saves */
    { CP_UI_KEY_A, FAIL_CONNECT, 90 },
    { CP_UI_KEY_A, NONE, 240 },         /* ERROR with Retry and Exit */
    { CP_UI_KEY_A, NONE, 330 },         /* Retry: RESTART, CONNECTING, PINGING, MENU */
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, NONE, 30 },
    { CP_UI_KEY_A, CP_UI_DEMO_ERROR, 240 },     /* ERROR with Retry only */
    { CP_UI_KEY_A, NONE, 330 },
    { CP_UI_KEY_DOWN, NONE, 30 },
    { CP_UI_KEY_DOWN, NONE, 30 },
    { CP_UI_KEY_A, NONE, 150 },         /* Exit: back to the saves */
    { CP_UI_KEY_DOWN, NONE, 30 },
    { CP_UI_KEY_DOWN, NONE, 30 },
    { CP_UI_KEY_A, NONE, 210 },         /* a Japanese HeartGold save */
    { CP_UI_KEY_A, NONE, 240 },         /* RECOVERY, in Japanese */
    { CP_UI_KEY_A, FAIL_WIFI, 330 },    /* no saved network: the Wi-Fi settings, in Japanese */
    { CP_UI_KEY_B, NONE, 240 },         /* ERROR with Retry and Wi-Fi settings */
};

int CpUiDemo_Tour(CpUiDemo *demo, CpUiInput *input)
{
    memset(input, 0, sizeof(*input));
    if (demo->step >= sizeof(kTour) / sizeof(*kTour)) return 0;
    if (demo->stepFrame == 0) {
        if (demo->step == 0) demo->noSaves = 1;
        if (kTour[demo->step].outcome == FAIL_CONNECT) demo->failConnect = 1;
        else if (kTour[demo->step].outcome == FAIL_WIFI) demo->failWifi = 1;
        else if (kTour[demo->step].outcome == FAIL_JOIN) demo->failJoin = 1;
        else if (kTour[demo->step].outcome >= 0) demo->outcome = kTour[demo->step].outcome;
        input->pressed = kTour[demo->step].keys;
        if (input->pressed & TAP_FLAG) {
            CpKey key;
            CpKeyboard_Key(NULL, (int)(input->pressed & 0xFF), &key);
            input->pressed = 0;
            input->touchPressed = input->touchHeld = 1;
            input->touchX = key.x * 8 + key.w * 4;
            input->touchY = key.y * 8 + key.h * 4;
        }
    }
    if (++demo->stepFrame >= kTour[demo->step].hold) {
        demo->step++;
        demo->stepFrame = 0;
    }
    return 1;
}
