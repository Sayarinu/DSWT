/* SPDX-License-Identifier: WTFPL
 * Adapted from FeOS Sound System, copyright 2012 fincs. */
#pragma once
#include <nds.h>
#include <string.h>
typedef u8 byte_t;
typedef u16 hword_t;
typedef s8 char_t;
#define FSS_PLAYERCOUNT 1
#define FSS_TRACKCOUNT 16
#define FSS_MAXTRACKS 16
#define FSS_TRACKSTACKSIZE 3
#include "fssplayer.h"
int MusicVolume(int index);

void Snd_Init(void);

int Cnv_Attack(int x);
int Cnv_Fall(int x);
int Cnv_Sust(int x); // quadratic volume curve
int Cnv_Vol(int x); // linear volume curve
int Cnv_Random(void);

enum { TYPE_PCM, TYPE_PSG, TYPE_NOISE };
int Chn_Alloc(int type, int prio);

void Chn_Kill(fss_channel_t* pCh, int nCh);
void Chn_Release(fss_channel_t* pCh, int nCh);

u16 Timer_Adjust(u16 basetmr, int pitch);

int Cnv_Sine(int arg);

int Player_Alloc(int prio);
void Player_Free(int handle);
void Player_Run(int handle);

bool Player_Setup(int handle, const byte_t* pSeq, const byte_t* pBnk, const byte_t* const pWar[4]);
void Player_Play(int handle);
void Player_Stop(int handle, bool bKillSound);
void Player_SetPause(int handle, bool bPause);

int Track_Alloc();
void Track_Free(int handle);
void Track_ReleaseAllNotes(int handle);

void Chn_UpdateTracks();
