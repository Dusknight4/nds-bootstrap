// ntrinit: starts a Slot-1 card (a game card or a flashcard such as an R4) from DSi mode the way
// the console's own boot does: reset, the unencrypted commands, KEY1 (Blowfish), KEY2, and main
// data mode. A flashcard's SD driver (its DLDI) expects the card in main data mode with KEY2
// running, as after a normal boot. Polled CPU transfers. Before calling: the slot freshly powered
// on (SCFG_MC, so the card is still in reset) and the ARM9 owning the card bus.
// Follows calico's ntrcard.c (fincs, devkitPro, ZPL-2.1) and GBATEK's DS cartridge protocol.
#pragma once
#include <nds/ndstypes.h>

enum {
	NTRINIT_RESET = 1,	// releasing reset
	NTRINIT_DUMMY,		// 9Fh
	NTRINIT_HEADER,		// 00h
	NTRINIT_CHIPID,		// 90h
	NTRINIT_KEY1,		// 3Ch
	NTRINIT_KEY2,		// KEY1 4xxx, then the KEY2 seeds
	NTRINIT_CHIPID2,	// KEY1 1xxx
	NTRINIT_SECURE,		// KEY1 2xxx (optional)
	NTRINIT_MAIN,		// KEY1 Axxx
	NTRINIT_CHIPID3,	// KEY2 B8h
	NTRINIT_DONE,
};

typedef struct NtrInitInfo {
	u32 header[0x200/4];
	u32 chipId1;		// 90h, unencrypted
	u32 chipId2;		// KEY1 1xxx, KEY2 reply
	u32 chipId3;		// KEY2 B8h, in main data mode
	u32 romcntMain;		// ROMCTRL for main data mode commands (KEY2 on), from the header's 60h
	int step;			// how far it got (NTRINIT_*)
} NtrInitInfo;

#define NTRINIT_F_SECURE 1	// also read the 4 secure area blocks, as the console's boot does
#define NTRINIT_F_RAW    2	// stop after the 1st chip ID (the card stays in unencrypted mode)

// keyTable: the 1048h-byte DS Blowfish table. random: any value (picks the KEY1/KEY2 parameters).
// True when the card got as far as asked: with NTRINIT_F_RAW a plausible 1st chip ID, otherwise
// all three chip IDs agreeing (the card is in main data mode).
bool ntrInit(const u32* keyTable, u32 random, u32 flags, NtrInitInfo* info);

// Waits about 'ms' milliseconds on timer 3.
void ntrDelayMs(u32 ms);

// One raw card command: the 8 command bytes as two words (the first byte is the top byte of hi),
// sent with the given ROMCTRL; up to 'words' words received into buf (buf may be NULL).
// False on a timeout.
bool ntrCardCmd(u32 hi, u32 lo, u32 romcnt, u32* buf, u32 words);
