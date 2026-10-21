// r4card: the microSD of a DSTT-type flashcard (the R4 SDHC running YSMenu is one) when the card
// was not booted by the DS, so nothing has set up its microSD. (The same file is in
// bootloaderi/include, for the loader.) dsttSdInit does what YSMenu's
// ttreset.dat does (its ARM9 at 02000D78): CMD0, CMD8, CMD55+ACMD41 until ready, CMD2, CMD3,
// CMD7, ACMD6, through the card's own commands:
//   50h status (0 = idle)   51h SD command (arg, index, op)   52h response
//   54h read setup (address) 5Fh mode (30h|n)   80h data poll (0 = ready)   81h read 200h bytes
// Every wait has a time limit, unlike the card's DLDI driver.
#pragma once
#include <nds/ndstypes.h>

typedef struct DsttInfo {
	u32 lastStatus;		// last 50h answer (for the log)
	u32 cmd8;			// CMD8 response (0: SD 1.x)
	u32 ocr;			// last ACMD41 response
	u32 rca;			// CMD3 response
	int acmd41Tries;
	bool sdhc;			// block addressing
	int step;			// how far it got (for the log)
} DsttInfo;

// 66h: switches the card from game-card mode to its SD commands (ttreset sends it just before
// the SD set-up). In main data mode the card expects it KEY2-encrypted (key2 true).
bool dsttEnterSd(bool key2, u32* resp);

// 50h: the card's status (0 = idle, in its SD mode); false if it didn't answer
bool dsttStatus(u32* status);

bool dsttSdInit(DsttInfo* info);
bool dsttReadSector(u32 sector, bool sdhc, void* buf);
