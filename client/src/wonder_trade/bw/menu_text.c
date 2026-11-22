/* The X menu's Wonder Trade button and page labels (client/aletheia.patch, ov021/field_menu.c),
 * in the game's character encoding, each ending with its terminator. */
const unsigned short gWtFieldMenuLabel[] = {
    'W', 'O', 'N', 'D', 'E', 'R', ' ', 'T', 'R', 'A', 'D', 'E', 0xFFFF,
};

const unsigned short gWtFieldMenuPage[2][8] = {
    { '<', ' ', '1', '/', '2', ' ', '>', 0xFFFF },
    { '<', ' ', '2', '/', '2', ' ', '>', 0xFFFF },
};
