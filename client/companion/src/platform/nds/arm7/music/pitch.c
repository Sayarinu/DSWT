/* SPDX-License-Identifier: WTFPL
 * Adapted from FeOS Sound System, copyright 2012 fincs. */
#include "fss7.h"


fss_channel_t FSS_Channels[16];
u16 FSS_ChnVol[16];
int FSS_NoteLengths[16];
fss_player_t FSS_Players[FSS_PLAYERCOUNT];
fss_track_t FSS_Tracks[FSS_MAXTRACKS];
byte_t FSS_TrackUpdateFlags[FSS_MAXTRACKS];
u16 FSS_ChnLockMask;


/* Equal temperament in 64ths of a semitone. Interpolate between semitones
 * in Q15 so ARM7 needs neither floating point nor a console BIOS table. */
u16 Timer_Adjust(u16 base, int pitch)
{
static const u16 ratio[] = {
    32768, 30929, 29193, 27554, 26008, 24548, 23170, 21870, 20643, 19484, 18390, 17358,
    16384,
};

    int octave = pitch / 768, within = pitch % 768;
    if (within < 0) { within += 768; --octave; }
    if (octave < -12) return 65535;
    if (octave > 12) return 16;
    unsigned note = (unsigned)within / 64, fraction = within % 64;
    unsigned factor = ratio[note] - ((ratio[note] - ratio[note+1]) * fraction / 64);
    u32 period = ((u32)base * factor) >> (15 + octave);
    return period < 16 ? 16 : period > 65535 ? 65535 : period;
}

/* Attenuation in tenths of a decibel. The channel's hardware divider
 * covers the quiet end of the range. */
int MusicVolume(int index)
{
static const u8 gain[] = {
    32, 32, 33, 33, 33, 34, 34, 35, 35, 35, 36, 36,
    37, 37, 37, 38, 38, 39, 39, 40, 40, 41, 41, 42,
    42, 43, 43, 44, 44, 45, 45, 46, 46, 47, 47, 48,
    48, 49, 49, 50, 51, 51, 52, 52, 53, 54, 54, 55,
    55, 56, 57, 57, 58, 59, 59, 60, 61, 61, 62, 63,
    64, 64, 65, 66, 67, 67, 68, 69, 70, 71, 71, 72,
    73, 74, 75, 76, 77, 77, 78, 79, 80, 81, 82, 83,
    84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
    96, 97, 99, 100, 101, 102, 103, 104, 106, 107, 108, 109,
    111, 112, 113, 114, 116, 117, 119, 120, 121, 123, 124, 126,
    127,
};

    int db = index - AMPL_K;
    if (db < -240) db += 240;
    else if (db < -120) db += 120;
    else if (db < -60) db += 60;
    /* Below -12 dB, each further 6 dB halves the amplitude. */
    unsigned shift = 0;
    while (db < -120) { db += 60; ++shift; }
    return gain[120 + db] >> shift;
}
int Cnv_Attack(int x)
{
    if (x >= 127) return 0;
    if (x < 109) return 255 - x;
    return (127 - x) * 8;
}
int Cnv_Fall(int x)
{
    if (x >= 127) return 65535;
    if (x < 50) return 2 * x + 1;
    return x == 126 ? 15360 : 7680 / (126 - x);
}
/* Convert linear amplitudes to tenths of a decibel using 200*log10(x/127). */
static const s16 amplitude[] = {
    -723, -421, -361, -325, -300, -281, -265, -252, -240, -230, -221, -212,
    -205, -198, -192, -186, -180, -175, -170, -165, -161, -156, -152, -148,
    -145, -141, -138, -134, -131, -128, -125, -122, -120, -117, -114, -112,
    -110, -107, -105, -103, -100, -98, -96, -94, -92, -90, -88, -86,
    -85, -83, -81, -79, -78, -76, -74, -73, -71, -70, -68, -67,
    -65, -64, -62, -61, -60, -58, -57, -56, -54, -53, -52, -51,
    -49, -48, -47, -46, -45, -43, -42, -41, -40, -39, -38, -37,
    -36, -35, -34, -33, -32, -31, -30, -29, -28, -27, -26, -25,
    -24, -23, -23, -22, -21, -20, -19, -18, -17, -17, -16, -15,
    -14, -13, -12, -12, -11, -10, -9, -9, -8, -7, -6, -6,
    -5, -4, -3, -3, -2, -1, -1, 0,
};

int Cnv_Vol(int x) { return amplitude[x & 127]; }
int Cnv_Sust(int x) { return x ? 2 * Cnv_Vol(x) : -723; }
int Cnv_Sine(int x)
{
static const u8 quarter[] = {
    0, 6, 12, 19, 25, 31, 37, 43, 49, 54, 60, 65,
    71, 76, 81, 85, 90, 94, 98, 102, 106, 109, 112, 115,
    117, 120, 122, 123, 125, 126, 126, 127, 127,
};

    unsigned phase = (unsigned)x & 127;
    unsigned offset = phase & 31;
    int value = quarter[(phase & 32) ? 32 - offset : offset];
    return (phase & 64) ? -value : value;
}
int Cnv_Random(void)
{
	static u32 seed = 0x12345678;
	seed *= 1664525;
	seed += 1013904223;
	return seed >> 16;
}
