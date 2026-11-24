/* Walk the demo model's tour through the renderer: every state is reached
 * and drawn, text lands inside its windows, and nothing draws past the
 * buffers (the sanitizers watch). With a directory argument, each settled
 * screen pair is also written there as raw layers for ui_composite.py. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/demo.h"
#include "ui/render.h"

static uint8_t sTopText[CP_CANVAS_BYTES], sBottomText[CP_CANVAS_BYTES];
static CpScreen sTop = { .text = { .tiles = sTopText } }, sBottom = { .text = { .tiles = sBottomText } };

static const char *kNames[CP_UI_STATE_COUNT] = {
    "BOOT", "SAVE_PICKER", "GAME_CONFIRM", "RECOVERY", "CONNECTING", "MENU", "PICKER", "CONFIRM",
    "COLLECT_CONFIRM", "CANCEL_CONFIRM", "PINGING", "RECEIVED", "MESSAGE", "ERROR", "RESTART", "CLOSING", "EXIT"
};

static void Dump(const char *directory, unsigned index, const CpUi *ui)
{
    char path[512];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%03u-%s-%d.bin", directory, index, kNames[ui->state], ui->message);
    file = fopen(path, "wb");
    assert(file);
    fwrite(sTopText, 1, sizeof(sTopText), file);
    fwrite(sTop.map, 1, sizeof(sTop.map), file);
    fwrite(sBottomText, 1, sizeof(sBottomText), file);
    fwrite(sBottom.map, 1, sizeof(sBottom.map), file);
    fwrite(CpRender_Palettes, 1, sizeof(CpRender_Palettes), file);
    fclose(file);
}

/* Some text pixel inside the rectangle. */
static int Inked(const CpCanvas *canvas, int x, int y, int w, int h, uint8_t colour)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            if (CpCanvas_Get(canvas, i, j) == colour) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    static CpUiDemo demo;
    CpUi ui;
    CpUiInput input;
    int seen[CP_UI_STATE_COUNT] = { 0 }, state, drawn = 0;
    unsigned frame = 0, dumps = 0, lastStep = 0;
    CpUiDemo_Init(&demo, Companion_Language_en_US);
    CpUi_Start(&ui, &demo.model);
    while (CpUiDemo_Tour(&demo, &input)) {
        CpUi_Frame(&ui, &demo.model, &input);
        seen[ui.state] = 1;
        if (ui.dirty) {
            CpRender_Draw(&ui, &demo.model, &sTop, &sBottom);
            ui.dirty = 0;
            drawn++;
            /* Every screen has its title and some message text. */
            assert(Inked(&sTop.text, 16, 8, 224, 16, CP_INK_WHITE));
            assert(Inked(&sBottom.text, 16, 8, 224, 16, CP_INK_WHITE));
            assert(Inked(&sTop.text, 0, 24, 256, 168, CP_INK_DARK));
            if (ui.state == CP_UI_MENU) assert(Inked(&sBottom.text, 56, 40, 144, 112, CP_INK_DARK));
        }
        /* The last frame of each tour step: the screens have settled. */
        if (argc > 1 && demo.step != lastStep) Dump(argv[1], dumps++, &ui);
        lastStep = demo.step;
        frame++;
        assert(frame < 100000);
    }
    for (state = 0; state < CP_UI_STATE_COUNT; state++) {
        if (state == CP_UI_EXIT) continue;
        if (!seen[state]) fprintf(stderr, "never reached %s\n", kNames[state]);
        assert(seen[state]);
    }
    printf("ui_render_test: %d draws over %u frames, every state reached\n", drawn, frame);
    return 0;
}
