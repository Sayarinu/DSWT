/* SPDX-License-Identifier: WTFPL
 * Adapted from FeOS Sound System, copyright 2012 fincs. */
#include "fss7.h"

static void Snd_Timer();

void Snd_Init(void)
{
    for (int i = 0; i < 16; ++i) {
        FSS_Channels[i].trackId = -1;
        FSS_NoteLengths[i] = -1;
    }
    FSS_ChnLockMask = BIT(13); /* UI confirmation tone */
    timerStart(LIBNDS_DEFAULT_TIMER_MUSIC, ClockDivider_64, (u16)-2728, Snd_Timer);
}

void Chn_Kill(fss_channel_t* pCh, int nCh)
{
	pCh->state = CS_NONE;
	pCh->trackId = -1;
	pCh->prio = 0;
	SCHANNEL_CR(nCh) = 0;
	FSS_ChnVol[nCh] = 0;
	FSS_NoteLengths[nCh] = -1;
}

void Chn_Release(fss_channel_t* pCh, int nCh)
{
	FSS_NoteLengths[nCh] = -1;
	pCh->prio = 1;
	pCh->state = CS_RELEASE;
}

static void Snd_UpdChannel(fss_channel_t* pCh, int nCh);


void Snd_Timer()
{
	register int i;
	//int cS = enterCriticalSection();

	Chn_UpdateTracks();

	for (i = 0; i < 16; i ++)
	{
		if (FSS_ChnLockMask & BIT(i))
			continue;
		Snd_UpdChannel(FSS_Channels + i, i);
	}

	for (i = 0; i < FSS_PLAYERCOUNT; i ++)
		Player_Run(i);



	Cnv_Random(); // Run the PRNG

	//leaveCriticalSection(cS);
}

static int calcVolDivShift(int x)
{
	// VOLDIV(0) /1  >>0
	// VOLDIV(1) /2  >>1
	// VOLDIV(2) /4  >>2
	// VOLDIV(3) /16 >>4
	if (x < 3) return x;
	return 4;
}

void Snd_UpdChannel(fss_channel_t* pCh, int nCh)
{
	// Kill active channels that aren't physically active
	if (pCh->state > CS_START && !SCHANNEL_ACTIVE(nCh))
	{
		Chn_Kill(pCh, nCh);
		return;
	}

	u8* pFlag = &pCh->flags;
	const bool bNotInSustain = pCh->state != CS_SUSTAIN;
	const bool bInStart = pCh->state == CS_START;
	const bool bPitchSweep = pCh->sweepPitch != 0 && pCh->sweepLen && pCh->sweepCnt <= pCh->sweepLen;
	bool bModulation = pCh->modDepth != 0;
	bool bVolNeedUpdate = bNotInSustain || *pFlag & F_UPDVOL;
	bool bPanNeedUpdate = *pFlag & F_UPDPAN || bInStart;
	bool bTmrNeedUpdate = *pFlag & F_UPDTMR || bInStart || bPitchSweep;
	const int modType = pCh->modType;
	int modParam = 0;

	switch (pCh->state)
	{
		case CS_NONE: return;
		case CS_START:
		{
			SCHANNEL_CR(nCh) = 0;
			SCHANNEL_SOURCE(nCh) = pCh->reg.SOURCE;
			SCHANNEL_REPEAT_POINT(nCh) = pCh->reg.REPEAT_POINT;
			SCHANNEL_LENGTH(nCh) = pCh->reg.LENGTH;
			pCh->ampl = AMPL_THRESHOLD;
			pCh->state = CS_ATTACK;
			__attribute__((fallthrough));
		}
		case CS_ATTACK:
		{
			pCh->ampl = ((int)pCh->ampl * (int)pCh->attackLvl) / 255;
			if (pCh->ampl != 0) break;
			pCh->state = CS_DECAY;
			break;
		}
		case CS_DECAY:
		{
			pCh->ampl -= (int)pCh->decayRate;
			int sustLvl = Cnv_Sust(pCh->sustainLvl) * 128;
			if (pCh->ampl > sustLvl) break;
			pCh->ampl = sustLvl;
			pCh->state = CS_SUSTAIN;
			break;
		}
		case CS_RELEASE:
		{
			pCh->ampl -= (int)pCh->releaseRate;
			if (pCh->ampl > AMPL_THRESHOLD) break;
			Chn_Kill(pCh, nCh);
			return;
		}
	}

	if (bModulation) do
	{
		if (pCh->modDelayCnt < pCh->modDelay)
		{
			pCh->modDelayCnt ++;
			bModulation = false;
			break;
		}

		switch (modType)
		{
			case 0: bTmrNeedUpdate = true; break;
			case 1: bVolNeedUpdate = true; break;
			case 2: bPanNeedUpdate = true; break;
		}

		// Get the current modulation parameter
		modParam = Cnv_Sine(pCh->modCounter >> 8) * pCh->modRange * pCh->modDepth;

		if (modType == 0)
			modParam = (s64)(modParam * 60) >> 14;
		else
			// Volume and pan modulation use 8 fractional bits.
			modParam >>= 8;

		// Update the modulation variables

		u16 speed = (u16)pCh->modSpeed << 6;
		u16 counter = (pCh->modCounter + speed) >> 8;

		while (counter >= 0x80)
			counter -= 0x80;

		pCh->modCounter += speed;
		pCh->modCounter &= 0xFF;
		pCh->modCounter |= counter << 8;
	} while(0);

	if (bTmrNeedUpdate)
	{
		int totalAdj = pCh->extTune;
		if (bModulation && modType == 0) totalAdj += modParam;
		if (bPitchSweep)
		{
			int len = pCh->sweepLen;
			int cnt = pCh->sweepCnt;
			totalAdj += ((s64)pCh->sweepPitch*(len-cnt)) / len;
			if (!pCh->manualSweep)
				pCh->sweepCnt ++;
		}
		u16 tmr = pCh->reg.TIMER;

		if (totalAdj) tmr = Timer_Adjust(tmr, totalAdj);
		SCHANNEL_TIMER(nCh) = -tmr;
		*pFlag &= ~F_UPDTMR;
	}

	if (bVolNeedUpdate || bPanNeedUpdate)
	{
		u32 cr = pCh->reg.CR;
		if (bVolNeedUpdate)
		{
			int totalVol = pCh->ampl >> 7;
			totalVol += pCh->extAmpl;
			totalVol += pCh->velocity;
			if (bModulation && modType == 1) totalVol += modParam;
			totalVol += AMPL_K;
			if (totalVol < 0) totalVol = 0;
			if (totalVol > AMPL_K) totalVol = AMPL_K;

			cr &= ~(SOUND_VOL(0x7F) | SOUND_VOLDIV(3));
			cr |= SOUND_VOL((int)MusicVolume(totalVol));

			if      (totalVol < (-240 + AMPL_K)) cr |= SOUND_VOLDIV(3);
			else if (totalVol < (-120 + AMPL_K)) cr |= SOUND_VOLDIV(2);
			else if (totalVol < (-60 + AMPL_K))  cr |= SOUND_VOLDIV(1);

			FSS_ChnVol[nCh] = ((cr & SOUND_VOL(0x7F)) << 4) >> calcVolDivShift((cr & SOUND_VOLDIV(3)) >> 8);

			*pFlag &= ~F_UPDVOL;
		}

		if (bPanNeedUpdate)
		{
			int pan = pCh->pan;
			pan += pCh->extPan;
			if (bModulation && modType == 2) pan += modParam;
			pan += 64;
			if      (pan < 0) pan = 0;
			else if (pan > 127) pan = 127;

			cr &= ~SOUND_PAN(0x7F);
			cr |= SOUND_PAN(pan);
			*pFlag &= ~F_UPDPAN;
		}

		pCh->reg.CR = cr;
		SCHANNEL_CR(nCh) = cr;
	}
}

int Chn_Alloc(int type, int prio)
{
	static const byte_t pcmChnArray[] = { 4, 5, 6, 7, 2, 0, 3, 1, 8, 9, 10, 11, 14, 12, 15, 13 };
	static const byte_t psgChnArray[] = { 8, 9, 10, 11, 12, 13 };
	static const byte_t noiseChnArray[] = { 14, 15 };
	static const byte_t arraySizes[] = { sizeof(pcmChnArray), sizeof(psgChnArray), sizeof(noiseChnArray) };
	static const byte_t* const arrayArray[] = { pcmChnArray, psgChnArray, noiseChnArray };

	const byte_t* chnArray = arrayArray[type];
	const int arraySize = arraySizes[type];

	register int i;
	register int curChnNo = -1;
	for (i = 0; i < arraySize; i ++)
	{
		int thisChnNo = chnArray[i];
		if (FSS_ChnLockMask & BIT(thisChnNo))
			continue;
		fss_channel_t* thisChn = FSS_Channels + thisChnNo;
		fss_channel_t* curChn = FSS_Channels + curChnNo;
		if (curChnNo != -1 && thisChn->prio >= curChn->prio)
		{
			if (thisChn->prio != curChn->prio)
				continue;
			if (FSS_ChnVol[curChnNo] <= FSS_ChnVol[thisChnNo])
				continue;
		}
		curChnNo = thisChnNo;
	}

	if (curChnNo == -1 || prio < FSS_Channels[curChnNo].prio) return -1;
	FSS_NoteLengths[curChnNo] = -1;
	FSS_ChnVol[curChnNo] = 0x7FF; // Special value that indicates 'unprocessed'
	return curChnNo;
}
