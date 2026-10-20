// nbwifi blob: the game's local wireless over the bridge. wmhle (the ARM7 half
// of the SDK's wireless library, reimplemented) takes the game's WM requests
// in place of the ARM7 wireless component, and talks to WideMelon over UDP.
//
// The requests come through the ARM7's PXI FIFO dispatcher: its handler table
// gets our handler for the WM tag (10) once the game has put its own there.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nbwifi.h"
#include "wmhle.h"
#include "blob.h"
#include "blob_radio.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;

void nbwLogf(const char* fmt, ...);

#define REG_VCOUNT       (*(vu16*)0x04000006)
#define REG_IPC_FIFO_CR  (*(vu16*)0x04000184)
#define REG_IPC_FIFO_TX  (*(vu32*)0x04000188)
#define CALLBACK_CONTROL (*(vu16*)0x027FFF96)

#define BRIDGE_PORT 7071

// The game's ARM7 PXI dispatch table (the WM entry is table[10]) is found by the code of the
// SDK's FIFO receive handler (pxi_find.h), a slice of ARM7 RAM a frame. (It was Platinum's fixed
// address, 03806EEC: on HeartGold, whose table is at 03806DFC, the WM requests then went to the
// game's own wireless driver, which couldn't start beside ours: a blue screen at the title menu.)
#include "pxi_find.h"

static u32 s_wmIp;          // WideMelon, once it has answered
static u32 s_lastV, s_lines;    // a running scanline count: our clock

static u32 linesNow(void)
{
	const u16 v = REG_VCOUNT;
	s_lines += v >= s_lastV ? v - s_lastV : v + 263 - s_lastV;
	s_lastV = v;
	return s_lines;
}

// ---- what wmhle needs from us ----

static uint8_t* hostRam(uint32_t addr) { return (uint8_t*)addr; }

bool blobTimerActive(void);

// "the ARM9 can't take a callback now": its FIFO is full, or it's VBlank (lines 190-262 and the first
// few). On the ARM9 a WM callback runs in its FIFO interrupt; one handed over as VBlank began
// held up the game's VBlank work (its sprite textures for the 3D field), which then ran into
// the picture: lines on the top screen in the Union Room (every frame's wmhleFrame used to
// deliver right at VBlank). The real ARM7 delivers when its radio events happen. While our
// timer runs, callbacks wait out VBlank (a few ms at most); without it, VBlank is the only time.
static bool hostArm9FifoFull(void)
{
	if (REG_IPC_FIFO_CR & 2) return true;
	const u16 v = REG_VCOUNT;
	return (v >= 190 || v < 4) && blobTimerActive();
}

static void hostToArm9(uint32_t word)
{
	// as the SDK's PXI_SendWordByFifo: wait while the FIFO is full (bit 1)
	for (int i = 0; i < 100000 && (REG_IPC_FIFO_CR & 2); i++);
	REG_IPC_FIFO_TX = word;
}

static void hostBridgeSend(const uint8_t* msg, unsigned len)
{
	if (s_wmIp) nbwifiUdpSend(s_wmIp, BRIDGE_PORT, BRIDGE_PORT, msg, len);
}

static void hostPollNet(void) { nbwifiPoll(); }

// The ARM9 takes callbacks one at a time (bit 0 of CALLBACK_CONTROL is busy until
// it has). We never wait for it here: at VBlank the ARM9 is busy with its own
// VBlank work and doesn't take one for milliseconds (on hardware every 2 ms wait
// ran out, and the backlog broke the game). Instead the next callback goes at the
// next chance: VBlank, and our VCount handler a few times a frame.
static u32 s_handOvers, s_armBusy;      // for the log
static bool hostWaitArm9(void)
{
	s_armBusy++;
	return false;
}

static uint32_t hostNowUs(void) { return linesNow() * 6356 / 100; }

static void hostLog(const char* text) { nbwLogf("%s\n", text); }

static WmHleHost s_host;

// (for the log: callbacks handed over by kind, and the line each went at: lines kept 32 to a bin)
static u16 s_cbKinds[8];    // scan, parent, beacon sent, MP ind, MP ack, port send, port recv, other
static u16 s_cbLines[9];    // by line: 0-31, 32-63 ... 224-262
static void hostToArm9Counted(uint32_t word)
{
	s_handOvers++;
	const u32 a = word >> 6;
	if (a >= 0x02000000 && a < 0x03000000) {
		const u16 api = *(vu16*)a, st8 = *(vu16*)(a + 8), st4 = *(vu16*)(a + 4);
		const int k = api == 10 ? 0 : api == 8 ? (st8 == 2 ? 2 : 1) : api == 14 ? (st4 == 12 ? 3 : st4 == 13 ? 4 : 7)
		            : api == 129 ? 5 : api == 130 ? 6 : 7;
		s_cbKinds[k]++;
	}
	const u16 v = REG_VCOUNT;
	s_cbLines[v / 32 < 8 ? v / 32 : 8]++;
	hostToArm9(word);
}

// the PXI handler for the WM tag: just queues (it runs in the FIFO interrupt)
static void wmPxiHandler(u32 tag, u32 data, int err)
{
	(void)tag;
	if (!err) wmhleRequest(data);
}

static vu32* s_pxiTable;        // the game's PXI table, once found
static u32 s_pxiLiteral;        // where the game's handler keeps its address (checked each frame)
static u32 s_pxiScan = PXI_FIND_START;

static u32 arm7Read32(u32 a) { return *(vu32*)a; }

// a slice of the search a frame (0x800 bytes: all of ARM7 RAM in 48 frames), never a long stop
static void pxiSearchStep(void)
{
	if (s_pxiTable) {
		if (*(vu32*)s_pxiLiteral == (u32)s_pxiTable) return;
		nbwLogf("[WM] the game's PXI table moved: looking again\n");
		s_pxiTable = NULL;
		s_pxiScan = PXI_FIND_START;
	}
	u32 end = s_pxiScan + 0x800;
	if (end > PXI_FIND_END - 32) end = PXI_FIND_END - 32;
	for (u32 a = s_pxiScan; a < end; a += 4) {
		if (*(vu32*)a != s_pxiDispatch[0]) continue;
		u32 literal = 0;
		const u32 t = pxiTableAt(a, arm7Read32, &literal);
		if (!t) continue;
		s_pxiTable = (vu32*)t;
		s_pxiLiteral = literal;
		nbwLogf("[WM] the game's PXI table: %X (its handler at %X)\n", (unsigned)t, (unsigned)a);
		return;
	}
	s_pxiScan = end >= PXI_FIND_END - 32 ? PXI_FIND_START : end;
}

// our handler in the game's PXI table, once the game has put its own there (the
// game's ARM7 wireless component puts it there each time it starts)
static void claimPxi(void)
{
	if (!s_pxiTable) return;
	vu32* entry = s_pxiTable + 10;
	if (*entry && *entry != (u32)wmPxiHandler) {
		nbwLogf("[WM] the game's wireless requests come to us now (was %X)\n", (unsigned)*entry);
		*entry = (u32)wmPxiHandler;
	}
}

// ---- the mid-frame handler (a hardware timer the game doesn't use) ----
//
// About every 2 ms (8 times a frame) it polls the network and hands over the next
// callback. (It used the VCount IRQ before, but that is the game's: its V-alarms,
// which the game's own ARM7 code puts back whenever it starts or ends its wireless,
// and other things such as touch sampling may need them. A timer nobody uses is ours alone.)

#define REG_IE       (*(vu32*)0x04000210)
#define REG_TMCNT_L(n) (*(vu16*)(0x04000100 + 4 * (n)))
#define REG_TMCNT_H(n) (*(vu16*)(0x04000102 + 4 * (n)))
#define TIMER_ON     0x00C3      // running, IRQ on overflow, F/1024 (32.7 kHz)
#define TIMER_TICKS  65          // ~2 ms

u8 s_vcountStack[0x1000] __attribute__((aligned(8)));     // frame_stack.s runs blobVCountImpl on it
void blobVCount(void);                                      // frame_stack.s (the handler's entry, whichever IRQ)
volatile bool g_nbwBusy;     // set while blob.c's frame runs: we don't step into it

static int s_timer = -1;     // the timer we use (0-3), -1 none
static bool s_timerTried;
static u32 s_vcountCalls;    // (named for the old VCount handler: calls of the mid-frame handler)

// How long we hold the ARM7 (in scanlines), for the log: the longest VBlank run and
// timer call, and how often the VBlank run went on into the next picture (VBlank is
// lines 192-262; past it the ARM9's per-frame graphics work may be held up: glitches)
static u16 s_maxFrameLines, s_maxTimerLines, s_maxPollLines;
static u32 s_frameOverruns;

// time by section (scanlines, for the log): longest and total per 5 s
// 0 VBlank: frame tick, 1 VBlank: wireless glue, 2 VBlank: bridge log/hello,
// 3 timer: network poll, 4 timer: callback hand-over, 5 VBlank: wmhle frame (in 1)
#define PROF_SECTIONS 6
static u16 s_profMax[PROF_SECTIONS];
static u32 s_profSum[PROF_SECTIONS];
void blobProf(int i, u16 v0)
{
	const u16 l = (u16)((REG_VCOUNT + 263 - v0) % 263);
	if (l > s_profMax[i]) s_profMax[i] = l;
	s_profSum[i] += l;
}

void blobTimeSpent(u16 v0, u16 v1, int vblank)
{
	const u16 lines = (u16)((v1 + 263 - v0) % 263);
	if (vblank == 2) { if (lines > s_maxPollLines) s_maxPollLines = lines; return; }   // (the network poll in it)
	if (vblank) {
		if (lines > s_maxFrameLines) s_maxFrameLines = lines;
		if (v0 >= 192 && v1 < 192) s_frameOverruns++;
	} else if (lines > s_maxTimerLines) s_maxTimerLines = lines;
}

extern NbwBlob g_nbwBlob;
static u32 s_wmFrames, s_lastTimerFrame;   // frames (VBlank), and the frame of our last timer call

// our timer still ours: running as we set it (the game may start a timer of its own with
// the same settings: its counter then runs from its own reload value, not ours)
static bool timerStillOurs(int n)
{
	return (REG_TMCNT_H(n) & 0xFF) == TIMER_ON && REG_TMCNT_L(n) >= (u16)(0x10000 - TIMER_TICKS);
}

static u32 s_savedEntry;     // (vcountClaim) the game's entry for our timer, from before we took it

void blobFrameLateFromTimer(u16 vcount);                    // blob.c: the frame's late half (wireless glue, bridge)
bool blobRadioActive(void);                                 // blob.c: joining, joined or leaving (worth polling)

void blobVCountImpl(void)
{
	// the game started our timer for itself (its handler was in the table before we took
	// it): its interrupt, so its handler, at once (vcountClaim puts it back in the table)
	if (s_timer >= 0 && !timerStillOurs(s_timer)) {
		if (s_savedEntry) ((void (*)(void))s_savedEntry)();
		return;
	}
	s_vcountCalls++;
	s_lastTimerFrame = s_wmFrames;
	if (g_nbwBusy) return;
	const u16 v0 = REG_VCOUNT;
	g_nbwBusy = true;
	claimPxi();
	{
		u16 p = REG_VCOUNT;
		// the network (whether or not the game's wireless is on: polled in VBlank instead,
		// on a busy home network it took up to 69 lines there, with lines on the screen):
		// replies go out sooner than at the next VBlank. Not in VBlank and the lines just
		// after it (188-262, 0-15): the game's ARM9 copies its sprites and palettes from
		// main RAM then, and our time on the bus made them late (lines at the top of the
		// screen); what came in waits for the next call. A few receive rounds a call, so
		// no one call holds the ARM7 long (calls up to 250 lines were seen)
		g_nbwPollRounds = 4;
		if (v0 >= 16 && v0 < 188 && blobRadioActive()) nbwifiPoll();     // (resting radio: not at all)
		blobProf(3, p);
		p = REG_VCOUNT;
		// the game's wireless requests (run here, not just once a frame: the real ARM7 takes
		// each at once, and the game's ARM9 waited for us, up to a frame, when it sent several:
		// lines on the 3D field in the Union Room, none with regular local wireless), then
		// the next callback if the ARM9 has taken the last one; in VBlank too (it's quick)
		wmhlePump();
		// the lid shut and that was the game ending its wireless: off the network before the
		// game can sleep (its sleep runs on this ARM7, after this interrupt; blob_radio.c)
		radioLidCheck(wmhleActive(), (*(vu16*)0x04000136 & 0x80) != 0);    // (REG_KEYXY bit 7: the hinge)
		blobProf(4, p);
		blobFrameLateFromTimer(REG_VCOUNT);   // (blob.c: the frame's work, once VBlank's over)
	}
	blobTimeSpent(v0, REG_VCOUNT, 0);
	g_nbwBusy = false;
}

// the game's live ARM7 IRQ table: in ARM7 RAM, where the game copied it, found
// by the two entries the loader put in it (nds-bootstrap's VBlank and IPC sync handlers)
static vu32* findIrqTable(void)
{
	extern NbwBlob g_nbwBlob;
	const u32 vblank = g_nbwBlob.irqVblank, ipcSync = g_nbwBlob.irqIpcSync;
	if (!vblank) return NULL;
	for (u32 a = 0x037F8000; a < 0x03810000 - 0x48; a += 4) {
		vu32* t = (vu32*)a;
		if (t[0] == vblank && (!ipcSync || t[16] == ipcSync)) return t;
	}
	return NULL;
}

static vu32* s_irqTable;
static u32 s_irqSearches;
static u8 s_gameTimers;      // timers the game uses (seen running, IRQ on, or taken back from us): never ours

static void vcountClaim(void)
{
	// the game's IRQ table where it was? (after a soft reset the game sets it up anew:
	// everything from before is void, timers included)
	if (s_irqTable && s_wmFrames % 60 == 0 && s_irqTable[0] != g_nbwBlob.irqVblank) {
		nbwLogf("[WM] the game's IRQ table changed: starting over\n");
		s_irqTable = NULL;
		s_irqSearches = 0;
		s_timer = -1;
		s_gameTimers = 0;
		s_timerTried = false;
	}
	// No timer left: every 2 s, any the game marked as its own that's stopped with its IRQ off
	// now is free again (after a trade Platinum's ARM7 takes timers 2 and 3 for a while, and
	// we stayed on VBlank alone from then on: up to 71 lines of work in VBlank, lines on the
	// screen in the Union Room after the first trade). If the game starts it again, we step
	// back as before (it changes the table entry or the timer's setup).
	if (s_timerTried && s_timer < 0 && s_gameTimers == 0x0F) {
		if (s_wmFrames % 120 != 0 || !s_irqTable) return;
		for (int n = 0; n <= 3; n++)
			if (!(REG_TMCNT_H(n) & 0x80) && !(REG_IE & (1u << (3 + n)))) s_gameTimers &= (u8)~(1 << n);
		if (s_gameTimers == 0x0F) return;
		nbwLogf("[WM] a timer is free again (game's timers %X)\n", (unsigned)s_gameTimers);
		s_timerTried = false;
	}
	if (!s_irqTable) {
		static u32 frames;
		if (frames++ % 60 != 0) return;     // once a second until the game has it in place
		s_irqTable = findIrqTable();
		if (!s_irqTable) {
			if (++s_irqSearches >= 10) {
				nbwLogf("[WM] the game's IRQ table not found: callbacks only at VBlank\n");
				s_timerTried = true;
				s_gameTimers = 0x0F;
			}
			return;
		}
	}
	vu32* table = s_irqTable;
	if (s_timer >= 0) {
		// if the game starts using it after all, it's the game's: we step back (its
		// handler back in the table: it may have put it there before we took the timer,
		// and start the timer only now) and look for another
		const u32 bit = 1u << (3 + s_timer);
		if (table[3 + s_timer] != (u32)blobVCount || !timerStillOurs(s_timer)) {
			if (table[3 + s_timer] == (u32)blobVCount) table[3 + s_timer] = s_savedEntry;
			s_gameTimers |= (u8)(1 << s_timer);
			nbwLogf("[WM] the game took timer %d: looking for another\n", s_timer);
			s_timer = -1;
			s_timerTried = false;
			return;
		}
		if (!(REG_IE & bit)) REG_IE |= bit;     // the game rewrote IE without it
		return;
	}
	if (s_timerTried) return;
	s_timerTried = true;
	// timers the game uses now are the game's, for good
	for (int n = 0; n <= 3; n++)
		if ((REG_TMCNT_H(n) & 0x80) || (REG_IE & (1u << (3 + n)))) s_gameTimers |= (u8)(1 << n);
	// one it leaves alone: 2 first (the SDK's ARM7 OS takes 0 and 1, Platinum's ARM7 takes 3)
	static const int order[4] = { 2, 1, 0, 3 };
	for (int i = 0; i < 4; i++) {
		const int n = order[i];
		if (s_gameTimers & (1 << n)) continue;
		s_savedEntry = table[3 + n];
		table[3 + n] = (u32)blobVCount;
		REG_TMCNT_H(n) = 0;
		REG_TMCNT_L(n) = (u16)(0x10000 - TIMER_TICKS);
		REG_TMCNT_H(n) = TIMER_ON;
		REG_IE |= 1u << (3 + n);
		s_timer = n;
		nbwLogf("[WM] timer %d IRQ ours (table %X, game's timers %X): a poll every ~2 ms\n", n, (unsigned)(u32)table, (unsigned)s_gameTimers);
		return;
	}
	nbwLogf("[WM] no free timer (game's timers %X): callbacks only at VBlank\n", (unsigned)s_gameTimers);
}

// ---- for blob.c ----

void wmGlueInit(void)
{
	s_host.ram = hostRam;
	s_host.sendToArm9 = hostToArm9Counted;
	s_host.bridgeSend = hostBridgeSend;
	s_host.pollNet = hostPollNet;
	s_host.waitArm9 = hostWaitArm9;
	s_host.nowUs = hostNowUs;
	s_host.mac = nbwifiMac();
	s_host.log = hostLog;
	s_host.arm9FifoFull = hostArm9FifoFull;
	s_lastV = REG_VCOUNT;
	wmhleInit(&s_host);
}

// a DsBridge message from WideMelon (the first one tells us where it is)
void wmGlueBridgeRx(u32 srcIp, const u8* data, unsigned len)
{
	if (!s_wmIp) s_wmIp = srcIp;
	wmhleBridgeRx(data, len);
}

void wmGlueSetPeer(u32 ip) { s_wmIp = ip; }

u32 wmGlueWideMelonIp(void) { return s_wmIp; }

void wmGlueFrame(void)
{
	s_wmFrames++;
	pxiSearchStep();
	claimPxi();
	vcountClaim();
	const u16 pw = REG_VCOUNT;
	wmhleFrame();
	blobProf(5, pw);
	static u32 frames;
	if (++frames % 300 == 0 && wmhleActive()) {
		nbwLogf("[WM] last 5 s: %u callbacks handed over, ARM9 busy %u times, %u timer calls\n",
		        (unsigned)s_handOvers, (unsigned)s_armBusy, (unsigned)s_vcountCalls);
		nbwLogf("[WM] longest VBlank run %u lines (%u ran into the picture, network poll in it %u), longest timer call %u lines, EXMEMSTAT %04X\n",
		        (unsigned)s_maxFrameLines, (unsigned)s_frameOverruns, (unsigned)s_maxPollLines, (unsigned)s_maxTimerLines, (unsigned)*(vu16*)0x04000204);
		// (by section: longest/total lines in these 5 s)
		nbwLogf("[WM] max/sum lines: tick %u/%u glue %u/%u (wmhle %u/%u) bridge %u/%u, timer poll %u/%u deliver %u/%u\n",
		        s_profMax[0], (unsigned)s_profSum[0], s_profMax[1], (unsigned)s_profSum[1], s_profMax[5], (unsigned)s_profSum[5],
		        s_profMax[2], (unsigned)s_profSum[2], s_profMax[3], (unsigned)s_profSum[3], s_profMax[4], (unsigned)s_profSum[4]);
		nbwLogf("[WM] SDIO %u transactions, max %u lines, sum %u; UDP sends %u, max %u lines, sum %u\n",
		        g_nbwProf.sdioN, g_nbwProf.sdioMax, g_nbwProf.sdioSum, g_nbwProf.sendN, g_nbwProf.sendMax, g_nbwProf.sendSum);
		nbwLogf("[WM] callbacks: scan %u parent %u beacon-sent %u MP-ind %u MP-ack %u port-send %u port-recv %u other %u\n",
		        s_cbKinds[0], s_cbKinds[1], s_cbKinds[2], s_cbKinds[3], s_cbKinds[4], s_cbKinds[5], s_cbKinds[6], s_cbKinds[7]);
		nbwLogf("[WM] callbacks by line (32s): %u %u %u %u %u %u %u %u %u\n", s_cbLines[0], s_cbLines[1], s_cbLines[2], s_cbLines[3],
		        s_cbLines[4], s_cbLines[5], s_cbLines[6], s_cbLines[7], s_cbLines[8]);
		for (int i = 0; i < 8; i++) s_cbKinds[i] = 0;
		for (int i = 0; i < 9; i++) s_cbLines[i] = 0;
		s_handOvers = s_armBusy = s_vcountCalls = 0;
		s_maxFrameLines = s_maxTimerLines = s_maxPollLines = 0;
		s_frameOverruns = 0;
		for (int i = 0; i < PROF_SECTIONS; i++) { s_profMax[i] = 0; s_profSum[i] = 0; }
		memset(&g_nbwProf, 0, sizeof(g_nbwProf));
	}
}

// the mid-frame timer is ours: it polls the network every ~2 ms, so VBlank needn't
// (the VBlank run held the ARM7 for longer than VBlank itself: graphics glitches)
bool blobTimerActive(void) { return s_timer >= 0 && s_wmFrames - s_lastTimerFrame <= 2; }   // (and its calls arriving)
