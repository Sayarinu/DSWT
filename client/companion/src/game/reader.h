#ifndef COMPANION_GAME_READER_H
#define COMPANION_GAME_READER_H

#include <stddef.h>
#include <stdint.h>

/* Results of the game data reader. Every function here returns CP_OK or one
 * of the negative codes. */
enum {
    CP_OK = 0,
    CP_ERR_IO = -1,          /* the reader failed or the read ran past the end */
    CP_ERR_FORMAT = -2,      /* the bytes are not the structure they should be */
    CP_ERR_NOT_FOUND = -3,   /* no such path, member, entry or record */
    CP_ERR_UNSUPPORTED = -4, /* a cartridge or language this code has no table for */
    CP_ERR_RANGE = -5        /* an argument or a caller's buffer is out of range */
};

/* Random access to a ROM image on SD or the game card in Slot-1. The
 * platform layer implements it; readAt fills all `size` bytes or fails. */
typedef struct CpReader {
    void *context;
    int (*readAt)(void *context, uint32_t offset, void *destination, uint32_t size);
    uint32_t (*size)(void *context);
} CpReader;

/* A byte range of a reader: a NitroFS file or a NARC member. */
typedef struct CpRomFile {
    const CpReader *reader;
    uint32_t offset, size;
} CpRomFile;

/* Read `size` bytes at `offset` within the range, refusing reads past its end. */
int CpRomFile_Read(const CpRomFile *, uint32_t offset, void *destination, uint32_t size);

/* The whole reader as one range. */
int CpRomFile_Whole(const CpReader *, CpRomFile *out);

static inline uint16_t CpLe16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t CpLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#endif
