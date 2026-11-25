#ifndef COMPANION_CORE_JOURNAL_FAMILY_H
#define COMPANION_CORE_JOURNAL_FAMILY_H

/* The companion links one build of the client core for every game. Of the
 * core sources it compiles, only journal.c depends on the family
 * (wonder_trade/game_family.h): its locators carry the family's party image
 * size and PC box count. journal.c is compiled twice, once per family
 * (Generation IV, and Generation V, whose Black/White and Black 2/White 2
 * builds are the same), with its functions renamed by journal_rename.h; this
 * module's WtJournal_* call the build of the family selected here. Every other
 * core source must compile to the same code for each family: the ROM build and
 * the host tests check that their preprocessed text is identical.
 *
 * Select the family before opening or decoding a journal and keep it while
 * any WtJournal or WtEngine of the save is open. */
typedef enum CpCoreFamily {
    CP_CORE_GEN4 = 0,
    CP_CORE_GEN5 = 1
} CpCoreFamily;

void CpCore_SelectFamily(CpCoreFamily);
CpCoreFamily CpCore_Family(void);

#endif
