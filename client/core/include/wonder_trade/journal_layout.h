#ifndef WONDER_TRADE_JOURNAL_LAYOUT_H
#define WONDER_TRADE_JOURNAL_LAYOUT_H

/* Byte offsets in the on-flash record. The contract tests compare the section
 * boundaries with every family in client/core/save-layouts/. */
#define WT_JOURNAL_RECORD_SIZE 4096
#define WT_JOURNAL_COMMIT_SIZE 4
#define WT_FLASH_PAGE_SIZE 256

enum {
    WT_REC_FORMAT = 0,                    /* "DSWJ" and format version 1 */
    WT_REC_PHASE = 6,
    WT_REC_SIZE = 8,
    WT_REC_RESERVED_SIZE_TAIL = 12,
    WT_REC_GENERATION = 16,
    WT_REC_RESERVED_HEADER_TAIL = 24,
    WT_REC_SAVE_ID = 32,                  /* identity */
    WT_REC_CREDENTIAL = 48,
    WT_REC_ENROLLMENT_ID = 80,
    WT_REC_NEXT_COUNTER = 96,             /* progress */
    WT_REC_EVENT_CURSOR = 104,
    WT_REC_LAST_RECEIPT = 112,
    WT_REC_LAST_OPERATION = 128,
    WT_REC_LAST_OPERATION_COUNTER = 144,
    WT_REC_RESERVED_PROGRESS_TAIL = 152,
    WT_REC_OPERATION = 160,               /* transaction */
    WT_REC_OPERATION_COUNTER = 176,
    WT_REC_TRADE = 184,
    WT_REC_RECEIPT = 200,
    WT_REC_SOURCE = 216,
    WT_REC_DESTINATION = 220,
    WT_REC_DELIVERY_KIND = 224,
    WT_REC_GLOBAL_COUNTER = 225,
    WT_REC_GENERAL_COUNTER = 229,
    WT_REC_BOX_COUNTER = 233,
    WT_REC_INVENTORY_BEFORE = 237,
    WT_REC_INVENTORY_AFTER = 269,
    WT_REC_ORIGINAL_DIGEST = 301,
    WT_REC_INCOMING_DIGEST = 333,
    WT_REC_APPLIED_DIGEST = 365,
    WT_REC_RESERVED_TRANSACTION_TAIL = 397,
    WT_REC_ORIGINAL = 416,                /* pokemon_images */
    WT_REC_DESTINATION_BEFORE = 652,
    WT_REC_DESTINATION_AFTER = 888,
    WT_REC_OUTGOING = 1124,
    WT_REC_INCOMING = 1260,
    WT_REC_RESERVED_IMAGES_TAIL = 1396,
    WT_REC_ENTROPY_FORMAT = 1440,         /* transport_private_state */
    WT_REC_SEED = 1444,
    WT_REC_RESERVED_ENTROPY_TAIL = 1476,
    WT_REC_PENDING_REQUEST = 1952,        /* pending_request: length, then the frame */
    WT_REC_RESERVED_REQUEST_HEADER = 1954,
    WT_REC_REQUEST = 1960,
    WT_REC_RESERVED = 2216,               /* reserved_zero */
    WT_REC_DIGEST = 4060,                 /* sha256 of the bytes before it */
    WT_REC_COMMIT = 4092                  /* "CMIT" */
};

/* Each pair is checked against the game's journal_banks array. */
#define WT_GEN4_BANK_A 0x30000
#define WT_GEN4_BANK_B 0x70000
#define WT_BW_BANK_A 0x74000
#define WT_BW_BANK_B 0x75000
#define WT_B2W2_BANK_A 0x6B000
#define WT_B2W2_BANK_B 0x6C000

#endif
