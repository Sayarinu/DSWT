#ifndef COMPANION_ADAPTER_SAME_POKEMON_H
#define COMPANION_ADAPTER_SAME_POKEMON_H

/* LUNE-902: whether a Pokémon in a save is the offered one as play in the
 * unmodified game may have left it, for both generations' canonical images.
 * The identity (personality and original trainer ID) alone is not enough: an
 * older deposit check compared exact images, so a save may hold another
 * Pokémon with the offer's identity. Play changes experience, moves, EVs,
 * items, friendship, ribbons, contest stats, Pokérus, the nickname, form and
 * (through evolution) species, ability and gender, never what is compared
 * here: the language, the IVs, the fateful-encounter bit, the Generation V
 * nature and hidden-ability / N's Pokémon flags (0x41, 0x42), the words at
 * 0x44 (Platinum and HeartGold/SoulSilver egg and met locations; unused, and
 * never written, in Generation V), the origin game, the original trainer's
 * name, the egg and met dates and locations, the ball, the met level and
 * trainer gender, the encounter type and the HeartGold/SoulSilver ball.
 * This is defence in depth: the engine keeps an escrow only when its deposit
 * proved the identity unique (wonder_trade/engine.c UniqueMark). The species must be the offer's or one it can
 * evolve into (two steps at most), read from the player's ROM; without ROM
 * data only the same species counts. Anything else is not proof. */
#include <stdint.h>
#include <string.h>

#include "adapter/platinum_receive.h"

static inline int CpSamePokemon_Fields(const unsigned char *a, const unsigned char *b, unsigned at, unsigned size)
{
    return memcmp(a + at, b + at, size) == 0;
}

/* Nonzero when `to` is `from` or an evolution of it within `depth` steps. */
static inline int CpSamePokemon_Evolves(const CpAdapterData *data, unsigned from, unsigned to, unsigned depth)
{
    CpEvolution evolutions[CP_EVOLUTIONS];
    unsigned i;
    if (from == to) return 1;
    if (!depth || !data || !data->evolutions || data->evolutions(data->arg, (uint16_t)from, evolutions) != CP_OK)
        return 0;
    for (i = 0; i < CP_EVOLUTIONS; ++i)
        if (evolutions[i].target && CpSamePokemon_Evolves(data, evolutions[i].target, to, depth - 1)) return 1;
    return 0;
}

/* Nonzero when `found` (canonical) can be `offer` (canonical) after play.
 * 0x41 and 0x42 are the nature and flags in Generation V; in Generation IV
 * 0x41 holds HeartGold/SoulSilver's shiny leaves, which play changes. */
static inline int CpSamePokemon(const CpAdapterData *data, int gen5, const unsigned char offer[136],
                                const unsigned char found[136])
{
    const unsigned ivs = (offer[0x38] | offer[0x39] << 8 | offer[0x3A] << 16 | (offer[0x3B] & 0x3Fu) << 24)
        ^ (found[0x38] | found[0x39] << 8 | found[0x3A] << 16 | (found[0x3B] & 0x3Fu) << 24);
    return CpSamePokemon_Fields(offer, found, 0x00, 4) && CpSamePokemon_Fields(offer, found, 0x0C, 4)
        && CpSamePokemon_Fields(offer, found, 0x17, 1) && ivs == 0 && ((offer[0x40] ^ found[0x40]) & 1) == 0
        && (!gen5 || CpSamePokemon_Fields(offer, found, 0x41, 2)) && CpSamePokemon_Fields(offer, found, 0x44, 4)
        && CpSamePokemon_Fields(offer, found, 0x5F, 1) && CpSamePokemon_Fields(offer, found, 0x68, 16)
        && CpSamePokemon_Fields(offer, found, 0x78, 10) && CpSamePokemon_Fields(offer, found, 0x83, 4)
        && CpSamePokemon_Evolves(data, (unsigned)(offer[0x08] | offer[0x09] << 8),
                                 (unsigned)(found[0x08] | found[0x09] << 8), 2);
}

#endif
