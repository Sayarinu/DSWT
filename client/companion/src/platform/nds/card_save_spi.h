#ifndef COMPANION_NDS_CARD_SAVE_SPI_H
#define COMPANION_NDS_CARD_SAVE_SPI_H

#include "card_save.h"

/* One chip-select interval. Header includes the optional IR prefix and the
 * big-endian address. Input is clocked with zeroes when tx is NULL. */
CpChipResult CpSaveSpi_Transfer(const uint8_t *header, size_t headerSize,
                                const uint8_t *tx, uint8_t *rx, size_t size);
void CpSaveSpi_PollDelay(void);

#endif
