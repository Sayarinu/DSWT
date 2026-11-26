#ifndef WONDER_TRADE_HGSS_MENU_ICON_H
#define WONDER_TRADE_HGSS_MENU_ICON_H

#include "field_system.h"
#include "sprite.h"

/* The X menu's touch-screen icon grid (overlay 27) has seven icons in a
 * two-column, four-row grid; the Wonder Trade icon takes the free eighth cell.
 * Overlay 27 tracks its cursor as an icon index (0-6, with 7 and 8 for the
 * registered-item buttons) and hands the start menu a selection index. */
#define WT_MENU_ICON_INDEX 9
#define WT_MENU_ICON_SELECTION 15
#define WT_MENU_ICON_HITBOX 10

/* The menu's own availability rule: saving allowed and the Pokémon icon on. */
BOOL WtMenuIcon_Available(FieldSystem *fieldSystem);

/* Hooks called from overlay 27 with its menu work area. */
void WtMenuIcon_Create(void *menu);
void WtMenuIcon_Destroy(void *menu);
void WtMenuIcon_Update(void *menu);
Sprite *WtMenuIcon_GetSprite(void *menu, int index);
void WtMenuIcon_Highlight(void *menu, int index);
int WtMenuIcon_Navigate(int index, int direction, const u8 *available);
int WtMenuIcon_SelectionForIndex(void *menu, int index);
BOOL WtMenuIcon_Touch(void *menu);

#endif
