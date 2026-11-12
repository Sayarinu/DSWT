#define _POSIX_C_SOURCE 200809L
#include "probe.h"
#include "wonder_trade/seed.h"
#include "wonder_trade/tls.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mbedtls/memory_buffer_alloc.h>
#include <mbedtls/platform.h>
#include <psa/crypto.h>

static Probe probe;
static unsigned char arena[WT_TLS_HEAP_SIZE];

static int ReadSeed(unsigned char *seed, size_t size) { return WtSeed_Read(&probe.engine.journal, seed, size); }
static int WriteSeed(unsigned char *seed, size_t size) { return WtSeed_Write(&probe.engine.journal, seed, size); }
static int Random(void *arg, unsigned char *bytes, size_t size)
{
    (void)arg;
    return psa_generate_random(bytes, size) == PSA_SUCCESS ? 0 : -1;
}

/* The synthetic inventory can hold anything the server delivers. */
static int CanHold(void *arg, const unsigned char pk4[136])
{
    (void)arg; (void)pk4;
    return 1;
}

static int ParseUnsigned(const char *text, unsigned maximum, unsigned *value)
{
    char *end;
    unsigned long number = strtoul(text, &end, 10);
    if (!*text || *end || number == 0 || number > maximum) return -1;
    *value = (unsigned)number; return 0;
}

static int Pin(const char *text, unsigned char bytes[32])
{
    unsigned i;
    if (strlen(text) != 64) return -1;
    for (i = 0; i < 32; ++i) {
        char pair[3] = {text[i * 2], text[i * 2 + 1], 0}, *end;
        if (strspn(pair, "0123456789abcdef") != 2) return -1;
        bytes[i] = (unsigned char)strtoul(pair, &end, 16);
    }
    return 0;
}

static int Run(unsigned port, const char *hostname, const unsigned char pins[2][32], const char *action)
{
    WtEngine *engine = &probe.engine;
    WtJournalIO journal = {&probe, ReadBank, WriteBank, Hash};
    WtEngineIO inventory = {&probe, Random, Inventory, CanHold};
    unsigned char entropy[32];
    unsigned count, operation, requested = 0, processing = 0;
    int slot = -1, result = -1;
    if (strcmp(action, "submit") == 0) slot = 0;
    else if (strcmp(action, "collect") == 0) requested = WT_OP_COLLECT;
    else if (strcmp(action, "cancel") == 0) requested = WT_OP_CANCEL;
    else if (strcmp(action, "reconcile") != 0) return -1;
    if (WtEngine_Open(engine, journal, inventory) != 0) return -1;
    /* A copied journal must never reuse its previous process's TLS random stream. */
    if (HostRandom(entropy, sizeof(entropy)) != 0 || WtSeed_Write(&engine->journal, entropy, sizeof(entropy)) != 32) return -1;
    for (count = 0; count < 24; ++count) {
        if (psa_crypto_init() != PSA_SUCCESS || WtJournal_Read(&engine->journal, &engine->entry) != 0) return -1;
        if (engine->entry.phase == WT_JOURNAL_EMPTY && WtEngine_Enroll(engine, 1) != 0) return -1;
        if (engine->entry.phase == WT_JOURNAL_IDLE && engine->synchronized) {
            if (slot < 0) return 0;
            if (WtEngine_Deposit(engine, (unsigned)slot) != 0) return -1;
            slot = -1;
        }
        operation = requested;
        if (engine->entry.deliveryKind == WT_DELIVERY_RETURN) operation = WT_OP_COLLECT;
        if (engine->lastResult == WT_RESULT_ILLEGAL_POKEMON
            || (engine->entry.requestLength && engine->entry.request[5] == WT_OP_CANCEL
                && (engine->lastResult == WT_RESULT_CONFLICT || engine->lastResult == WT_RESULT_NOT_FOUND))) operation = WT_OP_RECONCILE;
        if (operation == WT_OP_CANCEL && engine->entry.deliveryKind == WT_DELIVERY_MATCH) operation = WT_OP_COLLECT;
        result = WtEngine_Request(engine, operation);
        if (result != 0) return result < 0 ? -1 : 0;
        operation = engine->frame[5];
        result = Exchange(&probe, port, hostname, pins);
        if (result == -100 - WT_RESULT_PROCESSING && processing < WT_ENGINE_PROCESSING_RETRIES) {
            /* Mirrors the game's bounded wait for the server's backup to cover an outcome. */
            ++processing; --count;
            nanosleep(&(struct timespec){WT_ENGINE_PROCESSING_WAIT_MS / 1000, 0}, NULL);
            continue;
        }
        if (result < 0) return result;
        if (operation == WT_OP_STATUS) return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    unsigned char pins[2][32];
    unsigned port;
    size_t used, blocks;
    int result = -1;
    signal(SIGPIPE, SIG_IGN);
    mbedtls_memory_buffer_alloc_init(arena, sizeof(arena));
    if (argc == 4 && strcmp(argv[1], "init") == 0) result = Initialize(argv[2], argv[3]);
    else if ((argc == 3 && strcmp(argv[1], "inspect") == 0)
        || ((argc == 8 || argc == 11) && strcmp(argv[1], "run") == 0)) {
        if (OpenFiles(&probe, argv[2]) != 0) goto done;
        if (argc == 3) result = 0;
        else {
            if (ParseUnsigned(argv[3], 65535, &port) != 0 || strcmp(argv[4], "localhost") != 0
                || Pin(argv[5], pins[0]) != 0 || Pin(argv[6], pins[1]) != 0) goto done;
            if (argc == 11) {
                if ((strcmp(argv[8], "kill") != 0 && strcmp(argv[8], "error") != 0)
                    || ParseUnsigned(argv[10], 100000, &probe.occurrence) != 0) goto done;
                probe.fault = argv[8]; probe.checkpoint = argv[9];
            }
            mbedtls_platform_set_nv_seed(ReadSeed, WriteSeed);
            result = Run(port, argv[4], pins, argv[7]);
        }
        if (Report(&probe, result) != 0) result = -1;
        CloseFiles(&probe);
    }
done:
    mbedtls_psa_crypto_free();
    mbedtls_memory_buffer_alloc_cur_get(&used, &blocks);
    mbedtls_memory_buffer_alloc_free();
    if (used || blocks) return 2;
    return result == 0 ? 0 : 1;
}
