// dstt_sd: DLDI driver for the microSD of DSTT-type slot-1 flashcards (an R4 SDHC with YSMenu
// is one), for nds-bootstrap's R4 save copy. Written from the card's command protocol as
// YSMenu's ttreset.dat and DLDI use it:
//   50h status (0 = idle)       51h [arg][index][op] SD command   52h response
//   54h [addr] read setup       56h end of a written block        80h data poll (0 = ready)
//   81h read 200h bytes         82h write 200h bytes
// nds-bootstrap starts the card and sets up its microSD (arm9 r4save.c) before using this
// driver, so startup only checks that the card answers. Unlike YSMenu's driver, every wait
// has a limit: a card that stops answering makes a read or write fail instead of freezing.
#include <stddef.h>
#include <nds/ndstypes.h>

extern u32 dsttSdhc;	// dldi_header.s, offset 80h
// dldi_header.s, offset 84h: what the first failure was, for nds-bootstrap's log:
// [0] code (10h+command: bus stuck, 20h+command: no answer, 30h: never idle, 40h: data never
// ready, 50h: short read, 60h: short write, 70h: answers all ones = out of its SD mode),
// [1] detail, [2] reads so far, [3] ROMCTRL, [4] SCFG_MC << 16 | EXMEMSTAT, [5] the card's
// first 50h answer
extern u32 dsttDiag[6];
static u32 s_reads;
// After the first failure every call fails at once: a card that has left its SD mode (it was
// reset) can't come back without nds-bootstrap starting it again, and waiting out the limits on
// every sector would freeze a game's save for minutes.
static bool s_dead;

#define REG_AUXSPICNT_HI (*(vu8*)0x040001A1)
#define REG_ROMCTRL      (*(vu32*)0x040001A4)
#define REG_CARD_CMD     ((vu8*)0x040001A8)
#define REG_CARD_DATA    (*(vu32*)0x04100010)

#define RC_BUSY   (1u << 31)
#define RC_READY  (1u << 23)
#define RC_CMD4   0xA7180000u	// 4-byte transfer
#define RC_READ   0xA1180000u	// 200h-byte read
#define RC_WRITE  0xE1180000u	// 200h-byte write

#define SPIN  400000	// register polls without progress
#define POLLS 200000	// 50h/80h polls

static void fail(u32 code, u32 detail)
{
	s_dead = true;
	if (dsttDiag[0]) return;	// the first one
	dsttDiag[0] = code;
	dsttDiag[1] = detail;
	dsttDiag[2] = s_reads;
	dsttDiag[3] = REG_ROMCTRL;
	dsttDiag[4] = ((u32)*(vu16*)0x04004010 << 16) | *(vu16*)0x04000204;
}

static bool waitBus(void)
{
	for (int i = 0; i < SPIN; i++) {
		if (!(REG_ROMCTRL & RC_BUSY)) return true;
	}
	return false;
}

static void setCmd(u8 b0, u32 arg, u8 b5, u8 b6)
{
	REG_CARD_CMD[0] = b0;
	REG_CARD_CMD[1] = arg >> 24;
	REG_CARD_CMD[2] = arg >> 16;
	REG_CARD_CMD[3] = arg >> 8;
	REG_CARD_CMD[4] = arg;
	REG_CARD_CMD[5] = b5;
	REG_CARD_CMD[6] = b6;
	REG_CARD_CMD[7] = 0;
}

// a 4-byte command; false when the card never delivered its word
static bool cmd4(u8 b0, u32 arg, u8 b5, u8 b6, u32* out)
{
	if (!waitBus()) {
		fail(0x10 + b0, REG_ROMCTRL);
		return false;
	}
	setCmd(b0, arg, b5, b6);
	REG_ROMCTRL = RC_CMD4;
	bool got = false;
	for (int i = 0; i < SPIN; i++) {
		u32 c = REG_ROMCTRL;
		if (c & RC_READY) {
			u32 v = REG_CARD_DATA;
			if (out) *out = v;
			got = true;
		}
		if (!(c & RC_BUSY)) {
			if (!got) fail(0x20 + b0, c);
			return got;
		}
	}
	fail(0x20 + b0, REG_ROMCTRL);
	return false;
}

// a poll answer of all ones, 64 times running: the card is out of its SD mode
static bool allOnes(u32 s, int* run)
{
	*run = (s == 0xFFFFFFFF) ? *run + 1 : 0;
	if (*run < 64) return false;
	fail(0x70, s);
	return true;
}

static bool waitIdle(void)
{
	u32 s = 0;
	int ones = 0;
	for (int i = 0; i < POLLS; i++) {
		if (!cmd4(0x50, 0, 0, 0, &s)) return false;
		if (s == 0) return true;
		if (allOnes(s, &ones)) return false;
	}
	fail(0x30, s);
	return false;
}

static bool sdCmd(u8 index, u32 arg)
{
	return cmd4(0x51, arg, index, 1, NULL) && waitIdle() && cmd4(0x52, 0, 0, 0, NULL);
}

static bool dataReady(void)
{
	u32 s = 0;
	int ones = 0;
	for (int i = 0; i < POLLS; i++) {
		if (!cmd4(0x80, 0, 0, 0, &s)) return false;
		if (s == 0) return true;
		if (allOnes(s, &ones)) return false;
	}
	fail(0x40, s);
	return false;
}

static bool readBlock(u8* buf)
{
	if (!waitBus()) return false;
	setCmd(0x81, 0, 0, 0);
	REG_ROMCTRL = RC_READ;
	u32 n = 0;
	for (int i = 0; i < SPIN; i++) {
		u32 c = REG_ROMCTRL;
		if (c & RC_READY) {
			u32 v = REG_CARD_DATA;
			if (n < 0x200) {
				if (((u32)buf & 3) == 0) {
					*(u32*)(buf + n) = v;
				} else {
					buf[n] = v; buf[n + 1] = v >> 8; buf[n + 2] = v >> 16; buf[n + 3] = v >> 24;
				}
			}
			n += 4;
			i = 0;
		}
		if (!(c & RC_BUSY)) {
			if (n != 0x200) fail(0x50, n);
			return n == 0x200;
		}
	}
	fail(0x50, n);
	return false;
}

static bool writeBlock(const u8* buf)
{
	if (!waitBus()) return false;
	setCmd(0x82, 0, 0, 0);
	REG_ROMCTRL = RC_WRITE;
	u32 n = 0;
	for (int i = 0; i < SPIN; i++) {
		u32 c = REG_ROMCTRL;
		if (c & RC_READY) {
			u32 v = 0;
			if (n < 0x200) {
				if (((u32)buf & 3) == 0) {
					v = *(const u32*)(buf + n);
				} else {
					v = buf[n] | (buf[n + 1] << 8) | (buf[n + 2] << 16) | ((u32)buf[n + 3] << 24);
				}
			}
			REG_CARD_DATA = v;
			n += 4;
			i = 0;
		}
		if (!(c & RC_BUSY)) {
			if (n != 0x200) fail(0x60, n);
			return n == 0x200;
		}
	}
	fail(0x60, n);
	return false;
}

bool startup(void)
{
	REG_AUXSPICNT_HI = 0xC0;
	u32 s;
	return cmd4(0x50, 0, 0, 0, &s);
}

bool isInserted(void)
{
	return true;
}

bool readSectors(u32 sector, u32 numSectors, void* buffer)
{
	if (numSectors == 0) return true;
	if (s_dead) return false;
	REG_AUXSPICNT_HI = 0xC0;
	if (s_reads++ == 0) cmd4(0x50, 0, 0, 0, &dsttDiag[5]);	// the card's first answer, for the log
	u8* buf = (u8*)buffer;
	bool ok = cmd4(0x54, dsttSdhc ? sector : sector << 9, 0, 0, NULL);
	for (u32 i = 0; ok && i < numSectors; i++) {
		ok = dataReady() && readBlock(buf);
		buf += 0x200;
		if (ok && i + 1 < numSectors) {
			ok = cmd4(0x51, 0, 0, 7, NULL);	// on to the next block
		}
	}
	return sdCmd(12, 0) && ok;	// stop
}

bool writeSectors(u32 sector, u32 numSectors, const void* buffer)
{
	if (numSectors == 0) return true;
	if (s_dead) return false;
	REG_AUXSPICNT_HI = 0xC0;
	const u8* buf = (const u8*)buffer;
	const bool multi = numSectors > 1;
	bool ok = sdCmd(multi ? 25 : 24, dsttSdhc ? sector : sector << 9);
	for (u32 i = 0; ok && i < numSectors; i++) {
		ok = writeBlock(buf) && waitIdle();
		buf += 0x200;
		if (ok && multi && i + 1 == numSectors) {
			ok = sdCmd(12, 0);	// stop
		}
		ok = ok && cmd4(0x56, 0, 0, 0, NULL) && waitIdle();
	}
	return ok;
}

bool clearStatus(void)
{
	return true;
}

bool shutdown(void)
{
	return true;
}
