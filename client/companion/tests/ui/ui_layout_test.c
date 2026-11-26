/* Messages with values filled in stay inside their window in every language:
 * the longest game and language names on the game check, and long save
 * names in "Matching ROM not found". */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/rom.h"
#include "ui/render.h"

static uint8_t sTopText[CP_CANVAS_BYTES], sBottomText[CP_CANVAS_BYTES];
static CpScreen sTop = { .text = { .tiles = sTopText } }, sBottom = { .text = { .tiles = sBottomText } };

/* The message window when no card is shown: tiles (2, 9), 27 x 6. */
#define WINDOW_X 16
#define WINDOW_Y 72
#define WINDOW_W 216
#define WINDOW_H 48

static const uint8_t kLanguages[] = { CP_LANGUAGE_JAPANESE, CP_LANGUAGE_ENGLISH, CP_LANGUAGE_FRENCH,
    CP_LANGUAGE_ITALIAN, CP_LANGUAGE_GERMAN, CP_LANGUAGE_SPANISH, CP_LANGUAGE_KOREAN };
static const char *const kSaveNames[] = {
    "Pokemon Diamond.sav",
    "Pokemon Platinum Version (Europe) (En,Fr,De,Es,It) (Rev 1).sav",
    "PokemonPlatinumVersionEuropeEnFrDeEsItRev1WithoutAnySpaces.sav",
};

static int SupportedVersion(unsigned version)
{
    unsigned family;
    for (family = 1; family <= UINT8_MAX; family++)
        if (CpBuildCode_Family(CpBuildCode((CpFamily)family, version, CP_LANGUAGE_ENGLISH, 0)) != CP_FAMILY_NONE)
            return 1;
    return 0;
}

/* The full text window a message takes when it needs more than three lines. */
#define BODY_X 16
#define BODY_Y 40
#define BODY_W 224
#define BODY_H 128

/* The message wraps to the box's width in at most the lines of the window it
 * goes to; nothing of the text layer below the title lies outside that window,
 * and the window holds some text. */
static void ExpectInside(const CpUi *ui, const CpUiModel *model, const char *what, unsigned language)
{
    char message[CP_RENDER_MESSAGE_MAX];
    int x, y, inked = 0, width, lines, wx = WINDOW_X, wy = WINDOW_Y, ww = WINDOW_W, wh = WINDOW_H;
    CpRender_Message(ui, model, message, sizeof(message));
    lines = CpRender_MessageLines(message, &width);
    if (width > WINDOW_W || lines > BODY_H / 16) {
        fprintf(stderr, "%s (language %u): %d lines, %d pixels wide\n", what, language, lines, width);
        assert(0);
    }
    if (lines > WINDOW_H / 16) { wx = BODY_X; wy = BODY_Y; ww = BODY_W; wh = BODY_H; }
    for (y = 24; y < CP_CANVAS_HEIGHT; y++)
        for (x = 0; x < CP_CANVAS_WIDTH; x++) {
            uint8_t colour = CpCanvas_Get(&sTop.text, x, y);
            int inside = x >= wx && x < wx + ww && y >= wy && y < wy + wh;
            if (colour && !inside) {
                fprintf(stderr, "%s (language %u): text at (%d, %d) outside its window\n", what, language, x, y);
                assert(0);
            }
            if (inside && colour == CP_INK_DARK) inked = 1;
        }
    assert(inked);
}

static void TestWrap(void)
{
    char out[256];
    unsigned id, language;
    /* Breaks at spaces, keeps existing breaks and drops the space it breaks at. */
    assert(CpText_Wrap(out, sizeof(out), "aaa bbb\nccc", CpText_Width("aaa b")) == 3);
    assert(!strcmp(out, "aaa\nbbb\nccc"));
    /* A word wider than the line breaks between characters. */
    assert(CpText_Wrap(out, sizeof(out), "abcdefgh", CpText_Width("abc")) == 3);
    assert(!strcmp(out, "abc\ndef\ngh"));
    /* Japanese breaks at ideographic spaces too. */
    assert(CpText_Wrap(out, sizeof(out), "\xe3\x81\x82\xe3\x80\x80\xe3\x81\x84", CpText_Width("\xe3\x81\x82")) == 2);
    assert(!strcmp(out, "\xe3\x81\x82\n\xe3\x81\x84"));
    /* Every catalogue string wraps to the box's width, and one that already
     * fits comes out unchanged. */
    for (id = 0; id < Companion_Text_Count; id++)
        for (language = 0; language < Companion_Language_Count; language++) {
            const char *text = Companion_Text[id][language];
            char wrapped[CP_RENDER_MESSAGE_MAX];
            CpText_Wrap(wrapped, sizeof(wrapped), text, WINDOW_W);
            assert(CpText_Width(wrapped) <= WINDOW_W);
            if (CpText_Width(text) <= WINDOW_W) assert(!strcmp(wrapped, text));
        }
}

/* LUNE-875: the Wi-Fi settings' messages, with the longest network names,
 * fit where they are drawn in every language: under the network list and the
 * password field (the three-line box), alone (box or full window) and on the
 * failure page above its support code. */
static void TestWifiMessages(void)
{
    static const char *const names[] = {
        "Home", "12345678901234567890123456789012", "A network name with many words",
        "\xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xb3\xe3\x82\xbf\xe3\x83\xbc 5G",
    };
    static const int listed[] = { Companion_Text_WifiScanning, Companion_Text_WifiChoose, Companion_Text_WifiChooseSaved,
        Companion_Text_WifiSavedFailed, Companion_Text_WifiReasonTkip, Companion_Text_WifiReasonEnterprise,
        Companion_Text_WifiReasonDsi };
    static const int typed[] = { Companion_Text_WifiPassword, Companion_Text_WifiKeyRuleWpa, Companion_Text_WifiKeyRuleWep };
    static const int alone[] = { Companion_Text_WifiJoinedSaved, Companion_Text_WifiJoinedOpen,
        Companion_Text_WifiJoinedUnsaved, Companion_Text_WifiForgetConfirm, Companion_Text_WifiConnecting,
        Companion_Text_WifiReasonTkip, Companion_Text_WifiReasonEnterprise, Companion_Text_WifiReasonDsi };
    static const int failures[] = { Companion_Text_WifiFailed, Companion_Text_WifiNotFound,
        Companion_Text_WifiNoAddress, Companion_Text_WifiTimedOut, Companion_Text_WifiForgetFailed };
    static CpUiModel model;
    CpUi ui;
    char message[CP_RENDER_MESSAGE_MAX], wrapped[CP_RENDER_MESSAGE_MAX];
    unsigned l, n, i;
    for (l = 0; l < sizeof(kLanguages); l++)
        for (n = 0; n < sizeof(names) / sizeof(*names); n++) {
            memset(&model, 0, sizeof(model));
            memset(&ui, 0, sizeof(ui));
            model.systemLanguage = CpUi_GameLanguage(kLanguages[l], Companion_Language_en_US);
            model.loadMessage = -1;
            model.wifi.saved = 1;
            snprintf(model.wifi.savedSsid, sizeof(model.wifi.savedSsid), "%s", names[n]);
            snprintf(ui.wifiSsid, sizeof(ui.wifiSsid), "%s", names[n]);
            ui.wifiReturn = CP_UI_SAVE_PICKER;
            ui.state = CP_UI_WIFI_LIST;
            for (i = 0; i < sizeof(listed) / sizeof(*listed); i++) {
                ui.message = listed[i];
                CpRender_Message(&ui, &model, message, sizeof(message));
                assert(CpRender_MessageLines(message, NULL) <= WINDOW_H / 16);
            }
            ui.state = CP_UI_WIFI_KEYBOARD;
            for (i = 0; i < sizeof(typed) / sizeof(*typed); i++) {
                ui.message = typed[i];
                CpRender_Message(&ui, &model, message, sizeof(message));
                assert(CpRender_MessageLines(message, NULL) <= WINDOW_H / 16);
            }
            /* The plain-text notice: two lines under the password field. */
            assert(CpText_Wrap(wrapped, sizeof(wrapped), Companion_Text[Companion_Text_WifiPasswordWarning]
                [model.systemLanguage], 216) <= 2);
            for (i = 0; i < sizeof(alone) / sizeof(*alone); i++) {
                ui.state = alone[i] == Companion_Text_WifiForgetConfirm ? CP_UI_WIFI_FORGET
                    : alone[i] == Companion_Text_WifiConnecting ? CP_UI_WIFI_JOINING : CP_UI_WIFI_JOINED;
                if (alone[i] >= Companion_Text_WifiReasonTkip && alone[i] <= Companion_Text_WifiReasonDsi) {
                    ui.state = CP_UI_MESSAGE;
                    ui.messageReturn = CP_UI_WIFI_LIST;
                }
                ui.message = alone[i];
                CpRender_Draw(&ui, &model, &sTop, &sBottom);
                ExpectInside(&ui, &model, names[n], kLanguages[l]);
            }
            /* The failure page keeps its explanation above the code (line 7). */
            ui.state = CP_UI_WIFI_FAILED;
            for (i = 0; i < sizeof(failures) / sizeof(*failures); i++) {
                ui.message = failures[i];
                CpRender_Message(&ui, &model, message, sizeof(message));
                assert(CpText_Wrap(wrapped, sizeof(wrapped), message, BODY_W - 8) <= 6);
            }
        }
}

int main(void)
{
    static CpUiModel model;
    CpUi ui;
    unsigned v, l, n;
    TestWrap();
    TestWifiMessages();
    for (l = 0; l < sizeof(kLanguages); l++) {
        for (v = 0; v < CP_VERSION_COUNT; v++) {
            if (!SupportedVersion(v)) continue;
            memset(&model, 0, sizeof(model));
            memset(&ui, 0, sizeof(ui));
            model.gameVersion = v;
            model.gameLanguage = kLanguages[l];
            model.loadMessage = -1;
            ui.state = CP_UI_GAME_CONFIRM;
            ui.message = Companion_Text_GameConfirm;
            CpRender_Draw(&ui, &model, &sTop, &sBottom);
            ExpectInside(&ui, &model, "game check", kLanguages[l]);
            /* The game check keeps its box: three lines at most. */
            {
                char message[CP_RENDER_MESSAGE_MAX];
                CpRender_Message(&ui, &model, message, sizeof(message));
                /* Every supported version must resolve to a catalogue name. */
                if (!message[0] || message[0] == '?' || message[0] == ',') {
                    fprintf(stderr, "missing game name: version %u, language %u: %s\n", v, kLanguages[l], message);
                    assert(0);
                }
                assert(CpRender_MessageLines(message, NULL) <= WINDOW_H / 16);
            }
        }
        for (n = 0; n < sizeof(kSaveNames) / sizeof(*kSaveNames); n++) {
            memset(&model, 0, sizeof(model));
            memset(&ui, 0, sizeof(ui));
            model.systemLanguage = CpUi_GameLanguage(kLanguages[l], Companion_Language_en_US);
            model.saveCount = 1;
            snprintf(model.saves[0].name, CP_UI_SAVE_NAME_MAX, "%s", kSaveNames[n]);
            ui.state = CP_UI_MESSAGE;
            ui.messageReturn = CP_UI_SAVE_PICKER;
            ui.message = Companion_Text_MatchingRomMissing;
            CpRender_Draw(&ui, &model, &sTop, &sBottom);
            ExpectInside(&ui, &model, kSaveNames[n], kLanguages[l]);
        }
    }
    puts("ui_layout_test: ok");
    return 0;
}
