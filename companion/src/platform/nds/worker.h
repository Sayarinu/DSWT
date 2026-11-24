#ifndef COMPANION_PLATFORM_NDS_WORKER_H
#define COMPANION_PLATFORM_NDS_WORKER_H

#include <stddef.h>

#include "wonder_trade/seed.h"

/* One worker cothread for engine, journal and network operations, so the UI
 * thread keeps running while they wait. Only one job runs at a time.
 *
 * Cothreads are cooperative: the worker gives the CPU back whenever it waits
 * for the network (and the UI's VBlank wait gives it the CPU back), but a
 * long computation does not yield. A TLS handshake's elliptic-curve steps keep
 * the UI thread from running for up to about a second each at DS speed, so
 * animation that must not stall belongs in the VBlank interrupt.
 *
 * Each job runs with Mbed TLS's allocator on a static 64 KiB arena
 * (WT_TLS_HEAP_SIZE) and an NV seed source, as client/tests/native/probe.c
 * and the game's network worker set them up; both are torn down and cleared
 * afterwards. */

typedef int (*CpWorkerJob)(void *arg);

/* Where Mbed TLS keeps its NV seed. */
enum CpSeedKind {
    /* The save's journal (WtSeed_Read/WtSeed_Write on an open journal). Reads
     * are mixed with the entropy pool, as in the game. Use this for real
     * trades. */
    CP_SEED_JOURNAL,
    /* RAM only, mixed with the entropy pool: no durable seed. For diagnostics
     * that never touch a save; never for enrollment or trades. */
    CP_SEED_VOLATILE
};

typedef struct CpSeed {
    enum CpSeedKind kind;
    WtSeed *journal; /* CP_SEED_JOURNAL: open for the whole job */
} CpSeed;

/* Zero when the job started; -1 when one is still running. */
int CpWorker_Start(CpWorkerJob, void *arg, CpSeed);
/* Nonzero while a job runs or its result has not been collected. */
int CpWorker_Busy(void);
/* Ask the job to stop at its next wait (see CpWorker_Cancelled). */
void CpWorker_Cancel(void);
const volatile int *CpWorker_Cancelled(void);
/* When the job has finished: collect its result and the TLS arena's peak use
 * and leftover bytes, free the thread and return 1. Otherwise return 0. */
int CpWorker_Collect(int *result, size_t *peak, size_t *leaked);

#endif
