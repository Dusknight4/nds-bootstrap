// ntrinit: see ntrinit.h
#include <nds.h>
#include <string.h>
#include "ntrinit.h"
#include "fugu.h"

#define REG_NC_CNT     (*(vu16*)0x040001A0)
#define REG_NC_ROMCNT  (*(vu32*)0x040001A4)
#define REG_NC_CMD_HI  (*(vu32*)0x040001A8)
#define REG_NC_CMD_LO  (*(vu32*)0x040001AC)
#define REG_NC_SEED0LO (*(vu32*)0x040001B0)
#define REG_NC_SEED1LO (*(vu32*)0x040001B4)
#define REG_NC_SEED0HI (*(vu16*)0x040001B8)
#define REG_NC_SEED1HI (*(vu16*)0x040001BA)
#define REG_NC_FIFO    (*(vu32*)0x04100010)

#define NC_CNT_ROM     (0x8000 | 0x4000)	// enable, ROM mode, transfer IRQ (not enabled in IE)

#define RC_GAP1(n)       ((n) & 0x1fff)
#define RC_ENCR_DATA     (1u << 13)
#define RC_ENCR_ENABLE   (1u << 14)
#define RC_SEED_APPLY    (1u << 15)
#define RC_GAP2(n)       (((n) & 0x3f) << 16)
#define RC_ENCR_CMD      (1u << 22)
#define RC_DATA_READY    (1u << 23)
#define RC_BLK(n)        (((n) & 7) << 24)
#define RC_CLK_DIV_8     (1u << 27)
#define RC_CLK_GAP_ON    (1u << 28)
#define RC_NO_RESET      (1u << 29)
#define RC_START         (1u << 31)
#define RC_PARAMS        (RC_GAP1(0x1fff) | RC_GAP2(0x3f) | RC_CLK_DIV_8)

enum { BLK_0 = 0, BLK_200 = 1, BLK_1000 = 4, BLK_2000 = 5, BLK_4 = 7 };

#define SPIN_LIMIT 4000000	// polls without progress before giving up (no card, or it stopped answering)

static const u8 s_seedBytes[8] = { 0xE8, 0x4D, 0x5A, 0xB1, 0x17, 0x8F, 0x99, 0xD5 };

static FuguNtr s_key1;
static u32 s_secRomcnt;	// ROMCTRL for KEY1 commands
static u32 s_secCounter;	// kkkkk, +1 after each KEY1 command
static u32 s_secFixed;		// iiijjj
static u16 s_secParam;		// llll
static u16 s_secDelay;		// header 6Eh, 1T-ROM cards only (131 kHz units)
static bool s_is1trom;

static void delay131k(u32 n)
{
	while (n) {
		u32 part = n > 60000 ? 60000 : n;
		TIMER_CR(3) = 0;
		TIMER_DATA(3) = 0;
		TIMER_CR(3) = TIMER_ENABLE | TIMER_DIV_256;
		while (TIMER_DATA(3) < part);
		TIMER_CR(3) = 0;
		n -= part;
	}
}

void ntrDelayMs(u32 ms)
{
	delay131k(ms * 131);
}

static bool waitIdle(void)
{
	for (int i = 0; i < SPIN_LIMIT; i++) {
		if (!(REG_NC_ROMCNT & RC_START)) return true;
	}
	return false;
}

static void sendCmd(u32 hi, u32 lo)
{
	REG_NC_CNT = NC_CNT_ROM;
	REG_NC_CMD_HI = __builtin_bswap32(hi);
	REG_NC_CMD_LO = __builtin_bswap32(lo);
}

// Starts a transfer and takes its words: the first 'words' into buf (if any), the rest dropped
static bool recv(u32 romcnt, u32* buf, u32 words)
{
	REG_NC_ROMCNT = romcnt;
	u32 n = 0;
	for (int i = 0; i < SPIN_LIMIT; i++) {
		u32 c = REG_NC_ROMCNT;
		if (c & RC_DATA_READY) {
			u32 v = REG_NC_FIFO;
			if (buf && n < words) buf[n] = v;
			n++;
			i = 0;
		}
		if (!(c & RC_START)) return true;
	}
	return false;
}

bool ntrCardCmd(u32 hi, u32 lo, u32 romcnt, u32* buf, u32 words)
{
	if (!waitIdle()) return false;
	sendCmd(hi, lo);
	return recv(romcnt, buf, words);
}

// One KEY1 (Blowfish-encrypted) command: cmd nibble, the 24-bit middle field (iiijjj, or
// mmmnnn for 4xxx), then a transfer of 'blk'. 0- and 4-byte transfers get the sector gap
// added to the first gap (the hardware skips it for those), as calico does.
static bool key1Cmd(u32 cmd, u32 field24, u32 blk, u32* buf, u32 words)
{
	u64 v = (u64)(s_secCounter & 0xfffff)
	      | ((u64)(field24 & 0xffffff) << 20)
	      | ((u64)s_secParam << 44)
	      | ((u64)(cmd & 0xf) << 60);
	u32 raw[2] = { (u32)v, (u32)(v >> 32) };
	fuguEncrypt(&s_key1.state, raw);

	if (!waitIdle()) return false;
	sendCmd(raw[1], raw[0]);

	u32 romcnt = s_secRomcnt;
	if (blk == BLK_0 || blk == BLK_4) {
		u32 gap2 = (romcnt >> 16) & 0x3f;
		romcnt = (romcnt & ~RC_GAP2(0x3f)) + RC_GAP1(gap2);
	}

	bool ok = true;
	if (s_is1trom) {
		// 1T-ROM cards need the command once more after the secure area delay
		REG_NC_ROMCNT = romcnt;
		delay131k(s_secDelay);
		ok = waitIdle();
	}
	if (ok) {
		if (blk == BLK_0) {
			REG_NC_ROMCNT = romcnt;
			ok = waitIdle();
		} else if (blk == BLK_1000 && s_is1trom) {
			// 1T-ROM: the 1000h block as 8 transfers of 200h
			for (int i = 0; i < 8 && ok; i++) {
				ok = recv(romcnt | RC_BLK(BLK_200), buf ? buf + i * 0x80 : NULL, buf ? 0x80 : 0);
			}
		} else {
			ok = recv(romcnt | RC_BLK(blk), buf, words);
		}
	}
	s_secCounter++;
	return ok;
}

bool ntrInit(const u32* keyTable, u32 random, u32 flags, NtrInitInfo* info)
{
	memset(info, 0, sizeof(*info));
	const u32 romcntInit = RC_START | RC_NO_RESET | RC_PARAMS;	// the slowest timings

	// Release reset and give the card time to wake up
	info->step = NTRINIT_RESET;
	REG_NC_CNT = NC_CNT_ROM;
	REG_NC_ROMCNT = romcntInit & ~RC_START;
	ntrDelayMs(120);

	// 9Fh: dummy, 2000h bytes
	info->step = NTRINIT_DUMMY;
	if (!waitIdle()) return false;
	sendCmd(0x9F000000, 0);
	if (!recv(RC_START | RC_NO_RESET | RC_CLK_DIV_8 | RC_BLK(BLK_2000) | RC_GAP2(0x18), NULL, 0)) return false;

	// 00h: header
	info->step = NTRINIT_HEADER;
	if (!waitIdle()) return false;
	sendCmd(0x00000000, 0);
	if (!recv(romcntInit | RC_BLK(BLK_200), info->header, 0x200 / 4)) return false;

	// 90h: 1st chip ID
	info->step = NTRINIT_CHIPID;
	if (!waitIdle()) return false;
	sendCmd(0x90000000, 0);
	if (!recv(romcntInit | RC_BLK(BLK_4), &info->chipId1, 1)) return false;
	if (info->chipId1 == 0 || info->chipId1 == 0xFFFFFFFF) return false;
	if (flags & NTRINIT_F_RAW) {
		info->step = NTRINIT_DONE;
		return true;
	}

	const u8* h = (const u8*)info->header;
	const u32 gamecode = h[0x0C] | (h[0x0D] << 8) | (h[0x0E] << 16) | (h[0x0F] << 24);
	const u32 cntNormal = h[0x60] | (h[0x61] << 8) | (h[0x62] << 16) | (h[0x63] << 24);
	const u32 cntKey1 = h[0x64] | (h[0x65] << 8) | (h[0x66] << 16) | (h[0x67] << 24);
	s_secDelay = h[0x6E] | (h[0x6F] << 8);
	s_is1trom = (info->chipId1 >> 31) & 1;

	// 3Ch: into KEY1 mode. Blowfish keyed by the gamecode (level 2, 8-byte modulo)
	info->step = NTRINIT_KEY1;
	fuguNtrInit(&s_key1, keyTable, gamecode);
	fuguNtrKeySchedule(&s_key1, false, false);
	fuguNtrKeySchedule(&s_key1, false, false);
	s_secFixed = random & 0xffffff;
	s_secCounter = (random * 2654435761u) >> 12;
	s_secParam = (u16)((random >> 16) ^ (random * 40503u));
	if (!waitIdle()) return false;
	sendCmd((0x3Cu << 24) | s_secFixed, (s_secCounter & 0xfffff) << 8);
	REG_NC_ROMCNT = romcntInit;
	if (!waitIdle()) return false;
	s_secRomcnt = (cntKey1 & RC_PARAMS) | RC_START | RC_NO_RESET | (s_is1trom ? 0 : RC_CLK_GAP_ON);

	// 4xxx: KEY2 on, seeded with mmmnnn; then the same seeds on our side
	info->step = NTRINIT_KEY2;
	const u32 mmmnnn = ((random ^ 0x5A5A5A5A) * 2246822519u) >> 8;
	if (!key1Cmd(0x4, mmmnnn, BLK_0, NULL, 0)) return false;
	const u64 seed0 = ((u64)mmmnnn << 15) + 0x6000 + s_seedBytes[h[0x13] & 7];
	const u64 seed1 = 0x5C879B9B05ull;
	REG_NC_SEED0LO = (u32)seed0;
	REG_NC_SEED1LO = (u32)seed1;
	REG_NC_SEED0HI = (seed0 >> 32) & 0x7f;
	REG_NC_SEED1HI = (seed1 >> 32) & 0x7f;
	REG_NC_ROMCNT = RC_ENCR_DATA | RC_ENCR_ENABLE | RC_SEED_APPLY | RC_NO_RESET;
	s_secRomcnt |= RC_ENCR_DATA | RC_ENCR_ENABLE;

	// 1xxx: 2nd chip ID (KEY2-encrypted reply)
	info->step = NTRINIT_CHIPID2;
	if (!key1Cmd(0x1, s_secFixed, BLK_4, &info->chipId2, 1)) return false;

	// 2bbbb: the secure area blocks 4..7 (data not kept)
	if (flags & NTRINIT_F_SECURE) {
		info->step = NTRINIT_SECURE;
		const u16 llll = s_secParam;
		for (u16 b = 4; b < 8; b++) {
			s_secParam = b;
			bool ok = key1Cmd(0x2, s_secFixed, BLK_1000, NULL, 0);
			s_secParam = llll;
			if (!ok) return false;
		}
	}

	// Axxx: main data mode
	info->step = NTRINIT_MAIN;
	if (!key1Cmd(0xA, s_secFixed, BLK_0, NULL, 0)) return false;
	info->romcntMain = (cntNormal & RC_PARAMS) | RC_START | RC_NO_RESET | RC_ENCR_DATA | RC_ENCR_ENABLE | RC_ENCR_CMD;

	// B8h: 3rd chip ID, KEY2 both ways
	info->step = NTRINIT_CHIPID3;
	if (!waitIdle()) return false;
	sendCmd(0xB8000000, 0);
	if (!recv(info->romcntMain | RC_BLK(BLK_4), &info->chipId3, 1)) return false;

	info->step = NTRINIT_DONE;
	return info->chipId2 == info->chipId1 && info->chipId3 == info->chipId1;
}
