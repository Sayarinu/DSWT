#ifndef COMPANION_UI_MODEL_H
#define COMPANION_UI_MODEL_H

#include <stdint.h>

#include "companion_text.h"
#include "game/text.h"
#include "game/portrait.h"
#include "platform/wifi_status.h"

/* What the screens show and what they can ask for. The UI never touches the
 * save, the ROM or the network itself: a model owns them, publishes their
 * state in a CpUiModel and starts work when the UI calls one of its ops.
 *
 * The fields mirror the game's WonderTradeNetwork (client/core/include/
 * wonder_trade/network.h) so a session driver can fill them the way the
 * patched game's application.c reads them. Ops never block: they start work
 * and return, and the model reports progress through the fields, refreshed by
 * ops->update once per frame before the UI reads them. */

#define CP_UI_SAVES_MAX 16
#define CP_UI_SAVE_NAME_MAX 64
#define CP_UI_PARTY_MAX 6
/* Game-encoded text, CP_CHAR_EOS-terminated: 10 characters for a Pokémon's
 * name and 7 for a trainer's, as Generation IV stores them. */
#define CP_UI_NAME_MAX 11
#define CP_UI_TRAINER_MAX 8
#define CP_UI_ITEM_NAME_MAX 16

/* One .sav file the model found, and the ROM it pairs with. */
enum CpUiSaveStatus {
    CP_UI_SAVE_READY,      /* a supported ROM matches it */
    CP_UI_SAVE_NO_ROM,     /* no matching ROM next to it */
    CP_UI_SAVE_UNSUPPORTED /* the ROM is not a game the companion trades for */
};

typedef struct CpUiSave {
    char name[CP_UI_SAVE_NAME_MAX]; /* the file name, UTF-8 */
    uint8_t status;                 /* enum CpUiSaveStatus */
    uint8_t version, language;      /* CP_VERSION_* and CP_LANGUAGE_* of the ROM */
    uint8_t gameCard;               /* physical Slot-1 save */
    uint8_t writableCard;           /* set after the verified card copy opens */
} CpUiSave;

/* Why a party Pokémon cannot be offered, as platinum/game_save.c decides. */
enum CpUiIneligible {
    CP_UI_ELIGIBLE,
    CP_UI_INELIGIBLE_EGG,
    CP_UI_INELIGIBLE_MAIL,
    CP_UI_INELIGIBLE_CAPSULE,
    CP_UI_INELIGIBLE_LAST_USABLE, /* the only other usable Pokémon would be gone */
    CP_UI_INELIGIBLE_DUPLICATE,   /* an identical or damaged copy in the party or PC */
    CP_UI_INELIGIBLE_OTHER        /* any other refusal (an unknown species, a bad record) */
};

enum CpUiGender { CP_UI_MALE, CP_UI_FEMALE, CP_UI_GENDERLESS };

typedef struct CpUiPokemon {
    uint16_t species; /* national dex number; 0 for an empty slot */
    uint8_t level, gender, shiny, isEgg, form;
    uint8_t ineligible; /* enum CpUiIneligible */
    uint16_t heldItem;  /* 0 for none */
    /* The species name in the game's language (CpGame_SpeciesName) and the
     * Pokémon's own name. */
    uint16_t speciesName[CP_UI_NAME_MAX];
    uint16_t nickname[CP_UI_NAME_MAX];
    /* The held item's name from the ROM, or empty (CP_CHAR_EOS first) to show
     * only the held-item mark. */
    uint16_t itemName[CP_UI_ITEM_NAME_MAX];
    CpPortrait portrait;
} CpUiPokemon;

/* The Pokémon a completed trade delivered, for its card. */
typedef struct CpUiReceived {
    uint16_t species;
    uint8_t gender, shiny, hasNickname, originalTrainerGender;
    uint16_t speciesName[CP_UI_NAME_MAX];
    uint16_t nickname[CP_UI_NAME_MAX];
    uint16_t originalTrainer[CP_UI_TRAINER_MAX];
    CpPortrait portrait;
} CpUiReceived;

/* The session's state: WonderTradeNetwork.state, reduced to what the UI reads. */
enum CpUiNet {
    CP_UI_NET_CLOSED,   /* nothing running; after close, the session ended */
    CP_UI_NET_BUSY,     /* connecting, or an operation is in flight */
    CP_UI_NET_READY,    /* connected; the UI resumes the save (trade -1) */
    CP_UI_NET_RESPONSE, /* the last operation finished: read the outcome */
    CP_UI_NET_ERROR     /* the last operation failed: read error */
};

typedef struct CpUiOps {
    /* Look for saves; the result arrives in saves[] once busy clears. */
    void (*scan)(void *context);
    /* Open saves[index] and its ROM and check its journal (the game's
     * WtBoot_Check). Once busy clears, either loadMessage names why it cannot
     * be used, or game*, party and recovery describe it. */
    void (*load)(void *context, unsigned index);
    /* Drop the loaded save, back to the list. */
    void (*unload)(void *context);
    /* Join Wi-Fi and start a session (WonderTradeNetwork_Start). */
    void (*connect)(void *context);
    /* A negative slot resumes and reconciles; 0..5 offers that party slot
     * (WonderTradeNetwork_Trade). */
    void (*trade)(void *context, int slot);
    /* Collect a result, or cancel the pending offer (WonderTradeNetwork_Collect). */
    void (*collect)(void *context, int cancel);
    /* End the session (WonderTradeNetwork_Close); net reaches CLOSED. */
    void (*close)(void *context);
    /* Once per frame, before the UI reads the fields. */
    void (*update)(void *context);

    /* Wi-Fi settings (LUNE-875). A model without them leaves these NULL and
     * the screens offer no Wi-Fi settings. */
    /* List the networks in range (wifi.networks), stopping any attempt. */
    void (*wifiScan)(void *context);
    /* Join a listed network with the key the player typed; on success the
     * model stores it for the next launch (wifi.state JOINED). */
    void (*wifiJoin)(void *context, const char *ssid, unsigned security, const char *key);
    /* Delete the stored network. */
    void (*wifiForget)(void *context);
    /* The settings closed: join automatically again, in the background. */
    void (*wifiLeave)(void *context);
} CpUiOps;

/* The Wi-Fi settings' progress (CpUiModel.wifi.state). */
enum CpUiWifi {
    CP_UI_NETWORK_IDLE,
    CP_UI_NETWORK_SCANNING, /* listing networks; the list grows as they are found */
    CP_UI_NETWORK_JOINING,
    CP_UI_NETWORK_JOINED,   /* connected; saved says whether it was stored */
    CP_UI_NETWORK_FAILED    /* failure and code say why */
};

#define CP_UI_NETWORKS_MAX 24

typedef struct CpUiModel {
    const CpUiOps *ops;
    void *context;

    /* The console's language, for the screens before a save is chosen. */
    CompanionLanguage systemLanguage;
    /* A scan or load is running. */
    int busy;

    unsigned saveCount;
    CpUiSave saves[CP_UI_SAVES_MAX];

    /* The loaded save. loadMessage is a CompanionTextId naming why it cannot
     * be used (such as Companion_Text_SaveInGameFirst), or -1. */
    int loadMessage;
    uint8_t gameVersion, gameLanguage;
    /* The opened ROM's system font (CpGame_Font), to draw game-encoded names
     * as the game does; NULL draws them with the companion font. */
    const CpFont *font;
    int recovery; /* the journal needs recovery before anything else */
    unsigned partyCount;
    CpUiPokemon party[CP_UI_PARTY_MAX];

    /* The session, as WonderTradeNetwork publishes it. */
    enum CpUiNet net;
    int canLeave;
    int error;           /* the support code shown with an error */
    unsigned tradePhase; /* enum WtJournalPhase of the save's journal */
    int completed, returned, rejected, waiting, superseded, needsEntropy, refused;
    unsigned char refusal; /* enum WtWireRefusal */
    int hasReceived;
    CpUiReceived received;

    /* The last connect failed because no Wi-Fi network could be joined: the
     * screens offer the Wi-Fi settings. */
    int wifiError;
    struct {
        unsigned state; /* enum CpUiWifi */
        /* Networks in range, in the order they were found; one per name. */
        unsigned count;
        CpWifiNetwork networks[CP_UI_NETWORKS_MAX];
        unsigned revision; /* changes whenever networks[] does */
        int wpa2;       /* WPA2 networks can be joined (DSi mode) */
        unsigned failure; /* enum CpWifiFailure of a FAILED join */
        int code;       /* its support code */
        /* The network stored for the next launch, if any. */
        int saved;
        char savedSsid[CP_WIFI_SSID_MAX + 1];
        /* JOINED, but the network could not be written to the SD card. */
        int saveFailed;
        /* The last forget could not delete wifi.cfg (code says so); the
         * network stays stored and saved stays set. */
        int forgetFailed;
    } wifi;
} CpUiModel;

#endif
