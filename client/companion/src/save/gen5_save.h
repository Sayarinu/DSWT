#ifndef CP_GEN5_SAVE_H
#define CP_GEN5_SAVE_H

#include "save_image.h"

/* The Generation V half of save_image.c: Black/White and Black 2/White 2's
 * main save, as their backup system (aletheia arm9/backup_system.c, pokebw2
 * src/gfl/backup_system.c, the same code) and save control load and write it.
 *
 * Load (BACKUP_SYSTEM_LoadDoubleCopy): both copies are read; a copy's footer
 * is good when its size and magic match and its CRC is the CRC table's. With
 * one good footer the other copy takes its CRC table and footer; with none the
 * save is none (size or magic wrong in both) or broken. The newer good copy's
 * CRC table is tried first, then the other's: every block must have a footer
 * CRC equal to the table's and a body with that CRC in at least one copy, and
 * if both are good their counts must equal the chosen copy's. Each block comes
 * from the main copy unless only the backup holds it. Each block, the CRC
 * table and the footer remember the copy that must be written next.
 *
 * Then the save control (SAVE_CONTROL_Create and _Reload at Continue): an
 * intro save (the new game's first save) loads as no save; the musical block's
 * "distributed program" flag is set only when the musical distribution marker
 * is set and that extra save loads with the count the trainer block records,
 * and a marker whose extra save does not load is cleared. The other extra
 * saves' markers are kept as they are: the game would clear one whose extra
 * save (a battle video, say) is broken; such a save hashes differently in the
 * companion and the game, which only makes a step that compares them fail.
 *
 * Save (BACKUP_SYSTEM_SaveSeqMain): the save count goes up; each block not
 * marked is compared with the main copy on the card and marked for the main
 * copy when it differs; every marked block's count goes up. Then, for the
 * main copy and then the backup (the marks swapped between them): each marked
 * block with its footer, the CRC table, the footer but its last byte, and the
 * last byte. */

CpSaveResult CpGen5Save_Load(CpSaveImage *, const CpFile *, const CpSaveLayout *);
CpSaveResult CpGen5Save_Hash(const CpSaveImage *, unsigned char digest[CP_SAVE_HASH_SIZE]);
CpSaveResult CpGen5Save_Verify(const CpSaveImage *, const CpFile *, const unsigned char expected[CP_SAVE_HASH_SIZE]);
CpSaveResult CpGen5Save_Commit(CpSaveImage *, const CpFile *, const unsigned char expected[CP_SAVE_HASH_SIZE]);

/* The CRC-16/CCITT (initial 0xFFFF) every footer uses. */
uint16_t CpGen5Save_Crc(uint16_t crc, const unsigned char *data, size_t size);

/* Whether the musical distribution extra save on the card would load, with
 * the save count `count` (BACKUP_SYSTEM_LoadLinked on one copy): 1 loads,
 * 0 does not, -1 when the file cannot be read. */
int CpGen5Save_MusicalLoads(const CpSaveLayout *, const CpFile *, uint32_t count);

enum { CP_GEN5_MUSICAL_MARKER = 6, CP_GEN5_MARKER_SET = 0xC21E };

#endif
