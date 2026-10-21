// r4save: see r4save.h
#include <nds.h>
#include <nds/arm9/dldi.h>
#include <fat.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "r4save.h"
#include "myDSiMode.h"
extern "C" {
#include "ntrinit.h"
#include "r4card.h"
}

#define R4SAVE_DIR       "sd:/_nds/nds-bootstrap/r4save"
#define R4SAVE_LOG       "sd:/_nds/nds-bootstrap/r4save.log"
#define R4_SAVES_DIR     "r4:/roms/nds/saves"
#define REG_SCFG_MC_ARM9 (*(vu16*)0x04004010)
#define CHUNK            0x8000

static FILE* s_log;

static void note(const char* fmt, ...)
{
	if (!s_log) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(s_log, fmt, ap);
	va_end(ap);
}

static u32 s_crcTable[256];

static u32 crc32Update(u32 c, const u8* p, u32 n)
{
	if (!s_crcTable[1]) {
		for (u32 i = 0; i < 256; i++) {
			u32 x = i;
			for (int k = 0; k < 8; k++) x = (x >> 1) ^ (0xEDB88320u & -(x & 1));
			s_crcTable[i] = x;
		}
	}
	while (n--) c = s_crcTable[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return c;
}

// CRC32 of the first 'size' bytes of a file
static bool fileCrc(const char* path, u32 size, u8* buf, u32* crc)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	u32 c = 0xFFFFFFFF;
	bool ok = true;
	for (u32 done = 0; ok && done < size; ) {
		u32 n = size - done < CHUNK ? size - done : CHUNK;
		ok = fread(buf, 1, n, f) == n;
		c = crc32Update(c, buf, n);
		done += n;
	}
	fclose(f);
	*crc = ~c;
	return ok;
}

// Copies 'size' bytes; inPlace writes into the existing file, so it keeps its clusters
static bool copyFile(const char* from, const char* to, u32 size, bool inPlace, u8* buf)
{
	FILE* in = fopen(from, "rb");
	if (!in) return false;
	FILE* out = fopen(to, inPlace ? "r+b" : "wb");
	if (!out) {
		fclose(in);
		return false;
	}
	bool ok = true;
	for (u32 done = 0; ok && done < size; ) {
		u32 n = size - done < CHUNK ? size - done : CHUNK;
		ok = fread(buf, 1, n, in) == n && fwrite(buf, 1, n, out) == n;
		done += n;
	}
	fclose(in);
	return (fclose(out) == 0) && ok;
}

static bool readRecord(const char* path, u32* crc)
{
	FILE* f = fopen(path, "r");
	if (!f) return false;
	unsigned long v;
	bool ok = fscanf(f, "%lx", &v) == 1;
	fclose(f);
	*crc = v;
	return ok;
}

static void writeRecord(const char* path, u32 crc)
{
	FILE* f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "%08lX\n", crc);
	fclose(f);
}

// The card's reset line follows the ROMCTRL of whichever CPU owns the bus (each has its own): the
// ARM7 takes the bus for the loader and the card engine, so its side is released too, right after
// power-on, before the card is started (later, the hand-over itself would reset the card).
// Returns the ARM7's ROMCTRL (arm7 main.c answers on FIFO_USER_08).
static u32 releaseResetOnArm7(void)
{
	while (fifoCheckValue32(FIFO_USER_08)) fifoGetValue32(FIFO_USER_08);
	sysSetCardOwner(BUS_OWNER_ARM7);
	fifoSendValue32(FIFO_USER_08, 1);
	u32 v = 0xDEADDEAD;
	for (int i = 0; i < 200; i++) {
		if (fifoCheckValue32(FIFO_USER_08)) {
			v = fifoGetValue32(FIFO_USER_08);
			break;
		}
		ntrDelayMs(1);
	}
	sysSetCardOwner(BUS_OWNER_ARM9);
	return v;
}

static void slotOff(void)
{
	disableSlot1();
	ntrDelayMs(50);
}

// The two saves made level; false if they can't be (then the R4 copy stays off)
static bool bringLevel(configuration* conf, const char* r4Path, const char* base, u8* buf, u32* result)
{
	const u32 size = conf->saveSize;
	char recPath[256], bakPath[256];
	snprintf(recPath, sizeof(recPath), R4SAVE_DIR "/%s.crc", base);

	u32 crcSd;
	if (!fileCrc(conf->savPath, size, buf, &crcSd)) {
		note("could not read %s\n", conf->savPath);
		return false;
	}

	struct stat stR4;
	if (stat(r4Path, &stR4) != 0) {
		note("no save on the R4 yet: the 3DS save copied there\n");
		if (!copyFile(conf->savPath, r4Path, size, false, buf)) {
			note("copy FAILED\n");
			return false;
		}
		*result = crcSd;
	} else if ((u32)stR4.st_size != size) {
		note("sizes differ (3DS %lu, R4 %lu): left alone, no R4 copy\n", size, (u32)stR4.st_size);
		return false;
	} else {
		u32 crcR4;
		if (!fileCrc(r4Path, size, buf, &crcR4)) {
			note("could not read %s\n", r4Path);
			return false;
		}
		*result = crcSd;
		if (crcR4 == crcSd) {
			note("saves already match (%08lX)\n", crcSd);
		} else {
			u32 rec;
			bool hasRec = readRecord(recPath, &rec);
			bool r4Wins;
			if (hasRec && rec == crcSd) {
				r4Wins = true;
				note("changed on the R4 since last time\n");
			} else if (hasRec && rec == crcR4) {
				r4Wins = false;
				note("changed on the 3DS since last time\n");
			} else {
				// nds-bootstrap dates the SD save at every launch (conf_sd.cpp), so its date from before
				// that: when the game was last played on the 3DS. The R4's: last played there (or copied).
				r4Wins = (long)stR4.st_mtime > conf->saveMtimeBefore;
				char dSd[24], dR4[24];
				time_t tSd = conf->saveMtimeBefore, tR4 = stR4.st_mtime;
				strftime(dSd, sizeof(dSd), "%Y-%m-%d %H:%M", gmtime(&tSd));
				strftime(dR4, sizeof(dR4), "%Y-%m-%d %H:%M", gmtime(&tR4));
				note("%s: the newer one wins: 3DS %s, R4 %s -> %s\n", hasRec ? "changed on both" : "first time",
					dSd, dR4, r4Wins ? "R4" : "3DS");
			}
			const char* from = r4Wins ? r4Path : conf->savPath;
			const char* to = r4Wins ? conf->savPath : r4Path;
			snprintf(bakPath, sizeof(bakPath), R4SAVE_DIR "/backup/%s (%s).sav", base, r4Wins ? "3DS" : "R4");
			if (!copyFile(to, bakPath, size, false, buf)) {
				note("backup to %s FAILED: nothing copied\n", bakPath);
				return false;
			}
			if (!copyFile(from, to, size, true, buf)) {
				note("copy to %s FAILED\n", to);
				return false;
			}
			u32 check;
			if (!fileCrc(to, size, buf, &check) || check != (r4Wins ? crcR4 : crcSd)) {
				note("copy to %s does not read back right\n", to);
				return false;
			}
			*result = check;
			note("%s copied over the %s save (old one in %s)\n", r4Wins ? "R4" : "3DS", r4Wins ? "3DS" : "R4", bakPath);
		}
	}
	writeRecord(recPath, *result);
	return true;
}

void r4SavePrepare(configuration* conf)
{
	conf->saveMirrorCluster = 0;
	conf->saveMirrorSdhc = 0;
	conf->saveMirrorLogCluster = 0;
	if (!dsiFeatures() || conf->b4dsMode || !conf->sdFound || conf->bootstrapOnFlashcard
	 || conf->gameOnFlashcard || conf->saveOnFlashcard || conf->isDSiWare) return;	// (the DSiWare card engine has no R4 copy)
	if (conf->saveSize == 0) return;	// no save
	if (REG_SCFG_MC_ARM9 & BIT(0)) return;	// no card in slot-1

	mkdir("sd:/_nds/nds-bootstrap", 0777);
	mkdir(R4SAVE_DIR, 0777);
	mkdir(R4SAVE_DIR "/backup", 0777);
	s_log = fopen(R4SAVE_LOG, "w");
	note("R4 save copy for %s\n", conf->savPath);

	char gameCode[4] = {0};
	FILE* rom = fopen(conf->ndsPath, "rb");
	if (rom) {
		fseek(rom, 0x0C, SEEK_SET);
		fread(gameCode, 1, 4, rom);
		fclose(rom);
	}

	u32* keyTable = (u32*)malloc(0x1048);
	NtrInitInfo* info = (NtrInitInfo*)malloc(sizeof(NtrInitInfo));
	u8* buf = (u8*)malloc(CHUNK);
	DLDI_INTERFACE* dl = NULL;
	bool mounted = false;
	FILE* key = fopen("nitro:/encr_data.bin", "rb");
	bool ok = keyTable && info && buf && key && fread(keyTable, 1, 0x1048, key) == 0x1048;
	if (key) fclose(key);

	// The card in slot-1: freshly powered, then into main data mode
	bool started = false;
	u32 arm7Romctrl = 0;
	for (int attempt = 0; ok && attempt < 2 && !started; attempt++) {
		disableSlot1();
		ntrDelayMs(100);
		enableSlot1();
		ntrDelayMs(100);
		arm7Romctrl = releaseResetOnArm7();
		sysSetCardOwner(BUS_OWNER_ARM9);
		started = ntrInit(keyTable, (u32)time(NULL) ^ ((u32)REG_VCOUNT << 16), attempt ? NTRINIT_F_SECURE : 0, info);
	}
	note("ARM7 side released: ROMCTRL %08lX\n", arm7Romctrl);
	if (ok) {
		const char* h = (const char*)info->header;
		note("slot-1: %s, chip ID %08lX, header \"%.12s\" %.4s\n", started ? "started" : "no answer", info->chipId1, h, h + 0x0C);
	}
	ok = ok && started;
	if (ok && memcmp((const char*)info->header + 0x0C, gameCode, 4) == 0) {
		note("the game's own card: left to it\n");
		ok = false;
	}

	// Its microSD
	DsttInfo di;
	if (ok) {
		u32 resp;
		ok = dsttEnterSd(true, &resp) && dsttSdInit(&di);
		note("microSD: %s (step %d), %s\n", ok ? "set up" : "FAILED", di.step, di.sdhc ? "SDHC" : "SDSC");
	}
	if (ok) {
		dl = dldiLoadFromFile("nitro:/dstt_sd.dldi");
		ok = (dl != NULL);
		if (ok) {
			*(u32*)((u8*)dl + 0x80) = di.sdhc;
			DC_FlushAll();
			IC_InvalidateAll();
			sysSetCardOwner(BUS_OWNER_ARM9);
			ok = mounted = fatMountSimple("r4", &dl->ioInterface);
		}
		note("R4 card: %s\n", ok ? "mounted" : "could not mount");
	}

	// The two saves
	if (ok) {
		const char* name = strrchr(conf->ndsPath, '/');
		name = name ? name + 1 : conf->ndsPath;
		char base[160];
		snprintf(base, sizeof(base), "%s", name);
		char* dot = strrchr(base, '.');
		if (dot) *dot = 0;
		char r4Path[256];
		snprintf(r4Path, sizeof(r4Path), R4_SAVES_DIR "/%s.sav", base);
		mkdir("r4:/roms", 0777);
		mkdir("r4:/roms/nds", 0777);
		mkdir(R4_SAVES_DIR, 0777);
		u32 crc = 0;
		ok = bringLevel(conf, r4Path, base, buf, &crc);
		struct stat st;
		if (ok && stat(r4Path, &st) == 0 && st.st_ino >= 2) {
			conf->saveMirrorCluster = st.st_ino;
			conf->saveMirrorSdhc = di.sdhc;
			note("R4 copy on: %s, cluster %lu, CRC32 %08lX\n", r4Path, (u32)st.st_ino, crc);
		} else {
			ok = false;
		}
	}

	if (mounted) fatUnmount("r4");
	if (dl) dldiFree(dl);
	if (ok) {
		u32 status = 0xDEADDEAD;
		dsttStatus(&status);
		note("R4 status (50h) now: %08lX (0 = in its SD mode)\n", status);
	}
	if (!ok) {
		slotOff();
		note("R4 copy off\n");
	}
	free(keyTable);
	free(info);
	free(buf);
	if (s_log) {
		// 4 KB in all, the last KB left for the loader's notes (it can't make the file longer)
		long pos = ftell(s_log);
		while (pos < 0xBFF) {
			fputc(' ', s_log);
			pos++;
		}
		fputc('\n', s_log);
		for (int i = 0; i < 0x3FF; i++) fputc(' ', s_log);
		fputc('\n', s_log);
		fclose(s_log);
		struct stat st;
		if (conf->saveMirrorCluster && stat(R4SAVE_LOG, &st) == 0) {
			conf->saveMirrorLogCluster = st.st_ino;
		}
	}
	s_log = NULL;
}
