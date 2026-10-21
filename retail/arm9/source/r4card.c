// r4card: see r4card.h (the same file is in bootloaderi/source/arm7)
#include <nds.h>
#include <string.h>
#include "r4card.h"
#include "ntrinit.h"

#define DSTT_RC      0xA7180000u	// 4-byte transfer, as YSMenu
#define DSTT_RC_READ 0xA1180000u	// 200h-byte transfer
#define POLL_LIMIT   100000		// status polls before giving up
#define ACMD41_LIMIT 4000

static u32 s_gap;	// extra first gap (1000h) for 51h commands while setting up, as ttreset

static u32 card(u8 b0, u32 arg, u8 b5, u8 b6, bool* ok)
{
	u32 v = 0;
	bool r = ntrCardCmd(((u32)b0 << 24) | (arg >> 8), ((arg & 0xFF) << 24) | ((u32)b5 << 16) | ((u32)b6 << 8),
		DSTT_RC | (b0 == 0x51 ? s_gap : 0), &v, 1);
	if (ok && !r) *ok = false;
	return v;
}

// 5Fh: card mode
static void mode(u8 n, bool* ok)
{
	card(0x5F, (u32)(n | 0x30) << 24, 0, 0, ok);
}

static u32 s_lastStatus;

// 50h until idle
static bool waitReady(int limit)
{
	bool ok = true;
	for (int i = 0; i < limit && ok; i++) {
		s_lastStatus = card(0x50, 0, 0, 0, &ok);
		if (s_lastStatus == 0) return ok;
	}
	return false;
}

bool dsttStatus(u32* status)
{
	bool ok = true;
	*status = card(0x50, 0, 0, 0, &ok);
	return ok;
}

bool dsttEnterSd(bool key2, u32* resp)
{
	*resp = 0;
	// ttreset: command 66h with ROMCTRL A7586000h (KEY2 command and data, 4 bytes)
	return ntrCardCmd(0x66000000, 0, key2 ? 0xA7586000u : DSTT_RC, resp, 1);
}

// 51h: an SD command (op 0: no response, 1: 48-bit response, 2/3/4: the 136-bit response in parts)
static bool sdSend(u8 op, u8 cmd, u32 arg)
{
	bool ok = true;
	card(0x51, arg, cmd, op, &ok);
	return ok && waitReady(POLL_LIMIT);
}

static bool sdCmdR1(u8 cmd, u32 arg, u32* resp)
{
	if (!sdSend(1, cmd, arg)) return false;
	bool ok = true;
	u32 r = card(0x52, 0, 0, 0, &ok);
	if (resp) *resp = r;
	return ok;
}

static bool sdCmdR2(u8 cmd, u32 arg)
{
	if (!sdSend(2, cmd, arg)) return false;
	for (int i = 0; i < 4; i++) {
		if (!sdSend(3, cmd, arg)) return false;
	}
	return sdSend(4, cmd, arg);
}

bool dsttSdInit(DsttInfo* info)
{
	memset(info, 0, sizeof(*info));
	bool ok = true;
	s_gap = 0;
	info->step = 1;
	mode(7, &ok);
	s_gap = 0x1000;
	card(0x51, 0, 0, 4, &ok);		// op 4 with no command: the power-up clocks
	if (!ok) goto fail;

	info->step = 2;				// CMD0
	if (!sdSend(0, 0, 0)) goto fail;

	info->step = 3;				// CMD8: SD 2.0 check
	card(0x51, 0x1AA, 8, 1, &ok);
	if (!ok) goto fail;
	if (waitReady(9999)) {
		info->cmd8 = card(0x52, 0, 0, 0, &ok);
	} else {
		mode(0, &ok);
		mode(7, &ok);
		goto fail;
	}

	info->step = 4;				// CMD55 + ACMD41 until ready
	for (;;) {
		if (++info->acmd41Tries > ACMD41_LIMIT) goto fail;
		card(0x51, 0, 55, 1, &ok);
		if (!ok) goto fail;
		if (!waitReady(9999)) {
			mode(0, &ok);
			mode(7, &ok);
			info->cmd8 = 0;		// as ttreset: carry on as an SD 1.x card
			continue;
		}
		card(0x52, 0, 0, 0, &ok);
		if (!sdCmdR1(41, info->cmd8 ? 0x40FC0000 : 0x00FC0000, &info->ocr)) goto fail;
		if (info->ocr & 0x80) break;
	}
	info->sdhc = (info->ocr & 0x40) != 0;

	info->step = 5;				// CMD2, CMD3
	if (!sdCmdR2(2, 0)) goto fail;
	if (!sdCmdR1(3, 0, &info->rca)) goto fail;
	u32 r = info->rca & 0xFFFF;
	u32 rcaArg = ((r >> 8) | (r << 8)) << 16;

	info->step = 6;				// CMD7 select, ACMD6 4-bit bus
	if (!sdCmdR1(7, rcaArg, NULL)) goto fail;
	if (!sdCmdR1(55, rcaArg, NULL)) goto fail;
	if (!sdCmdR1(6, 2, NULL)) goto fail;

	info->step = 7;
	mode(0, &ok);
	mode(5, &ok);
	if (info->sdhc) mode(13, &ok);
	s_gap = 0;
	if (!ok) return false;
	info->step = 8;
	info->lastStatus = s_lastStatus;
	return true;

fail:
	s_gap = 0;
	info->lastStatus = s_lastStatus;
	return false;
}

bool dsttReadSector(u32 sector, bool sdhc, void* buf)
{
	bool ok = true;
	card(0x54, sdhc ? sector : sector << 9, 0, 0, &ok);
	if (!ok) return false;
	int i;
	for (i = 0; i < POLL_LIMIT && ok; i++) {
		if (card(0x80, 0, 0, 0, &ok) == 0) break;
	}
	if (!ok || i == POLL_LIMIT) return false;
	if (!ntrCardCmd(0x81000000, 0, DSTT_RC_READ, (u32*)buf, 0x200 / 4)) return false;
	return sdCmdR1(12, 0, NULL);	// stop
}
