#ifndef WONDER_TRADE_SAVE_CRC_H
#define WONDER_TRADE_SAVE_CRC_H

/* CRC-16/CCITT over retail save blocks, most significant bit first. */
#define WT_SAVE_CRC_POLYNOMIAL 0x1021
#define WT_SAVE_CRC_HIGH_BIT 0x8000
#define WT_SAVE_CRC_INITIAL 0xFFFF

#endif
