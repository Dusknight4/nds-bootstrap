// wmhle: the ARM7 half of the DS SDK's wireless library (WM), over a bridge.
// See wmhle.h. Follows the real ARM7 code (TwlSDK 5.5's wmsp_*.c, as
// decompiled in NathaanTFM/arm7decomp; HGSS's matches) for what the ARM9 sees:
// the request/callback protocol, the WMStatus block it reads directly, the
// callback sequences of each API and the MP receive buffers. What the real code
// does on the air (beacons, association, MP command/reply/ack frames) becomes
// DsBridge messages to WideMelon. Structure layouts: "WM protocol research.md".
#include <string.h>
#include "wmhle.h"
#include "wmhle_air.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;

// ---- the ARM9 <-> ARM7 contract ----

#define PXI_TAG_WM            10
#define REQUEST_ACCEPTED      0x8000      // set in a request's apiid when done: the ARM9 may reuse it
#define CALLBACK_CONTROL      0x027FFF96  // bit 0: fifo7to9 holds a message the ARM9 hasn't taken
#define PXI_HANDLE_CHECKER_7  0x027FFF8C  // bit 10: "the ARM7 WM is there"

enum {
	API_INITIALIZE = 0, API_RESET, API_END, API_ENABLE, API_DISABLE, API_POWER_ON, API_POWER_OFF,
	API_SET_P_PARAM, API_START_PARENT, API_END_PARENT, API_START_SCAN, API_END_SCAN,
	API_START_CONNECT, API_DISCONNECT, API_START_MP, API_SET_MP_DATA, API_END_MP,
	API_START_DCF, API_SET_DCF_DATA, API_END_DCF, API_SET_WEPKEY, API_START_KS, API_END_KS,
	API_GET_KEYSET, API_SET_GAMEINFO, API_SET_BEACON_IND, API_START_TESTMODE, API_STOP_TESTMODE,
	API_VALARM_MP, API_SET_LIFETIME, API_MEASURE_CHANNEL, API_INIT_W_COUNTER, API_GET_W_COUNTER,
	API_SET_ENTRY, API_AUTO_DEAUTH, API_SET_MP_PARAMETER, API_SET_BEACON_PERIOD, API_AUTO_DISCONNECT,
	API_START_SCAN_EX, API_SET_WEPKEY_EX, API_SET_PS_MODE, API_START_TESTRXMODE, API_STOP_TESTRXMODE,
	API_KICK_MP_PARENT, API_KICK_MP_CHILD, API_KICK_MP_RESUME, API_ASYNC_KIND_MAX,
	API_INDICATION = 128, API_PORT_SEND = 129, API_PORT_RECV = 130,
};

enum { ERR_SUCCESS = 0, ERR_FAILED = 1, ERR_OPERATING = 2, ERR_ILLEGAL_STATE = 3, ERR_INVALID_PARAM = 6,
       ERR_NO_CHILD = 7, ERR_TIMEOUT = 9, ERR_SEND_QUEUE_FULL = 10, ERR_NO_DATA = 14, ERR_SEND_FAILED = 15 };

enum { SC_PARENT_START = 0, SC_BEACON_SENT = 2, SC_SCAN_START = 3, SC_PARENT_NOT_FOUND = 4,
       SC_PARENT_FOUND = 5, SC_CONNECT_START = 6, SC_CONNECTED = 7, SC_BEACON_LOST = 8,
       SC_DISCONNECTED = 9, SC_MP_START = 10, SC_MPEND_IND = 11, SC_MP_IND = 12, SC_MPACK_IND = 13,
       SC_PORT_SEND = 20, SC_PORT_RECV = 21, SC_DISCONNECTED_FROM_MYSELF = 26 };

enum { ST_READY = 0, ST_STOP = 1, ST_IDLE = 2, ST_CLASS1 = 3, ST_SCAN = 5, ST_CONNECT = 6,
       ST_PARENT = 7, ST_CHILD = 8, ST_MP_PARENT = 9, ST_MP_CHILD = 10 };

// WMStatus field offsets (the ARM9 reads the block directly; 0x7D0 bytes)
#define S_state              0x000
#define S_BusyApiid          0x002
#define S_apiBusy            0x004
#define S_mp_flag            0x00C
#define S_VSyncFlag          0x01C
#define S_mp_parentSize      0x030
#define S_mp_childSize       0x032
#define S_mp_parentMaxSize   0x034
#define S_mp_childMaxSize    0x036
#define S_mp_sendSize        0x038
#define S_mp_recvSize        0x03A
#define S_mp_maxSendSize     0x03C
#define S_mp_maxRecvSize     0x03E
#define S_mp_parentVCount    0x040
#define S_mp_childVCount     0x042
#define S_mp_parentInterval  0x044
#define S_mp_childInterval   0x048
#define S_mp_minFreq         0x058
#define S_mp_freq            0x05A
#define S_mp_maxFreq         0x05C
#define S_mp_recvBufSel      0x070
#define S_mp_recvBufSize     0x072
#define S_mp_recvBuf0        0x074
#define S_mp_recvBuf1        0x078
#define S_mp_sendBuf         0x07C
#define S_mp_sendBufSize     0x080
#define S_mp_readyBitmap     0x086
#define S_mp_current_minPollBmpMode 0x7C6
#define S_linkLevel          0x0BC
#define S_beaconIndicateFlag 0x0C2
#define S_pwrMgtMode         0x0C6
#define S_miscFlags          0x0C8
#define S_MacAddress         0x0E0
#define S_mode               0x0E6
#define S_pparam             0x0E8
#define S_childMacAddress    0x128
#define S_child_bitmap       0x182
#define S_pInfoBuf           0x184
#define S_aid                0x188
#define S_parentMacAddress   0x18A
#define S_scan_channel       0x190
#define S_preamble           0x1EE
#define S_enableChannel      0x1F4
#define S_allowedChannel     0x1F6
#define S_portSeqNo          0x1F8
#define S_wlVersion          0x020      // (the wireless firmware's version and the chip's ids, as the real ARM7 fills them)
#define S_SIZE               0x7D0

// WMParentParam offsets (inside S_pparam)
#define PP_userGameInfo       0x00
#define PP_userGameInfoLength 0x04
#define PP_ggid               0x08
#define PP_tgid               0x0C
#define PP_entryFlag          0x0E
#define PP_maxEntry           0x10
#define PP_multiBootFlag      0x12
#define PP_KS_Flag            0x14
#define PP_CS_Flag            0x16
#define PP_beaconPeriod       0x18
#define PP_channel            0x32
#define PP_parentMaxSize      0x34
#define PP_childMaxSize       0x36

// WMArm7Buf offsets
#define A7_connectPInfo 0x10

// ---- state ----

#define REQ_QUEUE  32
#define CB_QUEUE   48
#define CB_MAX     0xC0                 // largest callback we send (bytes; fifo7to9 is 0x100)
#define SEND_QUEUE 32
// children a parent takes (WM allows 15; Pokémon's most is the Underground's 7). The DS blob
// builds with 7: its memory is tight, and the reply buffers are one per child
#ifndef WMHLE_MAX_CHILD
#define WMHLE_MAX_CHILD 15
#endif
#define MAX_CHILD WMHLE_MAX_CHILD
// the parents we've heard (their beacons, for scans): as a real DS, every parent in range (the
// DS blob builds with 4: three others hosting at once is the most a four-player game has)
#ifndef WMHLE_MAX_PARENTS
#define WMHLE_MAX_PARENTS 6
#endif
#define MAX_PARENTS WMHLE_MAX_PARENTS
#define PARENT_HEARD_FRAMES 60      // a scan finds a parent heard this lately
#define PARENT_KEEP_FRAMES 300      // and forgets one not heard this long
// A link partner we've had nothing from for the game's MP lifetime (WM_SetLifeTime, in 100 ms;
// Pokemon sets 10 s for its battles and the like) is gone (out of range, switched off): out of the
// link, as the real ARM7's AUTO_DISCONNECT. Until then the link holds, as on the air, where a console
// a little out of range (a crowded bus) misses its turns and comes back. A game that sets none (the
// Union Room, the Underground): the real ARM7 then never ends a link for silence (its MP lifetime
// is off and the radio's own station lifetime is 0xFFFF), leaving it to the game, whose wireless
// layer only reports what WM tells it; this, the same 10 s, so a console gone for good (switched
// off, a PC asleep) doesn't leave the other stuck on "Communicating".
#define LIFETIME_DEFAULT_FRAMES 600
// (the child) the parent unheard this long: the "beacon lost" notice (the game takes no action
// on it), the link still up
#define BEACON_LOST_FRAMES 120

typedef struct {
	u16 len;
	u32 buf;            // the receive buffer it points into (MP_IND, PORT_RECV), 0 none
	u8 data[CB_MAX];
} QueuedCallback;

typedef struct {
	bool used;
	u32 data;           // ARM9 address of the payload
	u16 size;           // bytes
	u16 destBitmap;
	u16 restBitmap;     // still to be delivered
	u16 port;
	u16 priority;
	u32 callback, arg;  // the ARM9 function and argument from SET_MP_DATA
	u16 seqNo;
	bool sentOnce;
	bool inCmd;         // (parent) sent in a CMD; firstCmd: the first one's sequence number:
	u16 firstCmd;       // a reply to that CMD or a later one proves the child has it
	u32 uid;            // (child) which packet this is, for the record of what each reply carried
} SendPacket;

static struct {
	const WmHleHost* h;
	volatile u32 req[REQ_QUEUE];
	volatile unsigned reqIn, reqOut;
	QueuedCallback cb[CB_QUEUE];
	unsigned cbIn, cbOut;

	bool initialized;   // INITIALIZE done: the addresses below are good
	u32 arm7buf, status, fifo7to9;
	u32 frames;

	// parent
	u32 nextBeacon;
	u16 beaconSeq;
	// scan
	bool scanning;
	u32 scanReq;        // its request buffer (the scan runs for maxChannelTime)
	u32 scanBuf;
	u16 scanChannel;
	u32 scanEnd;
	// the parent we're connecting to or connected to (as a child)
	u8 remoteMac[6];
	u16 scanBssidAny;           // (the scan) any parent, or only scanBssid
	u8 scanBssid[6];
	// connecting / connected as a child
	bool connecting;
	u32 connectStart;
	u32 lastFromParent;
	u32 lastCmdFromParent;      // (child) the parent's last CMD to us, or the join (its MP lifetime counts from it)
	u32 lastCmdFrame;           // (log) the frame we last took a CMD (child)
	// MP
	SendPacket sendq[SEND_QUEUE];
	u16 nextSeqNo[16];          // per port, for the ports with sequence numbers
	u16 lastRecvSeq[MAX_CHILD + 1][16];    // per sender aid, per port: duplicates are dropped
	u32 childHeardAt[MAX_CHILD + 1];       // (parent) the frame we last had a reply from each child
	u16 childPolls[MAX_CHILD + 1];         // (parent) CMDs sent it since (a child never polled isn't silent)
	u8 childMissed[MAX_CHILD + 1];         // (parent) rounds in a row that closed without its reply
	u32 lifeFrames;             // the MP lifetime in frames (0: none, the game turned it off)
	bool beaconLostSent;        // (child) the notice for this silence of the parent's sent
	u16 cmdSeq;                 // 802.11 sequence of our frames
	u16 lastCmdSeq;             // child: the parent's last CMD (for our reply's RefSeq)
	u16 ackCounter;
	bool childReplied[MAX_CHILD + 1];
	bool waitingAck;            // child: our reply's packets wait for the parent's ack
	u32 waitingAckFrame;        // (child) when we replied: no ack 2 frames on, MPACK_IND says timeout (as the SDK's alarm)
	u16 rxCount[9], txCount[9];   // DsBridge messages by type, for the log
	u32 staleReplies;
	u32 nMpData, nPortRecv, nPortSendOk, nPortSendFail, nDupDropped, nReplyCopies, nCmdsHeldBack;   // for the log
	u32 lastProgress, maxGap;   // MP rounds: the frame of the last one, the longest wait between two (for the log)
	u32 nextUid;                // SendPacket uids
	u32 inFlightBuf;            // the receive buffer of the callback the ARM9 has now
	u32 nBufClobbered;          // (log) CMDs let go: their receive buffer still pointed into by unread callbacks
	u32 nNotForUs;              // (log) messages for another console, dropped
	// INITIALIZE's answer, held back as long as the real ARM7 takes (it powers the radio up)
	bool initReplyPending;
	u16 initReplyApi;
	u32 initReplyAt;
} W;
// The real ARM7 answers INITIALIZE some frames after it's asked (powering the radio up), and
// games count on that: Platinum's Underground asks it, sets "alone" (which only takes once its
// comm system exists) and starts its client, which needs the answer; on a DS that fails the
// first time, and the second time round "alone" takes. Answered at once, the client started
// first time, "alone" never took, and the game waited for good for a link it didn't have: the
// player frozen, Roark's welcome never coming, no one ever found.
#define INIT_REPLY_FRAMES 6

// the parents in range (their latest beacon): a scan reports one each time, in turn, as a
// real one reports whichever it hears first (with several hosting, the game scans again
// and again and so finds them all)
typedef struct {
	bool used;
	u8 mac[6];
	u8 channel;
	u32 heardAt, reportedAt;    // frames
	u16 len;
	u8 beacon[256];
} RemoteParent;
static RemoteParent s_parents[MAX_PARENTS];

static RemoteParent* parentFind(const u8* mac)
{
	for (int i = 0; i < MAX_PARENTS; i++) if (s_parents[i].used && memcmp(s_parents[i].mac, mac, 6) == 0) return &s_parents[i];
	return NULL;
}

// ---- little helpers on DS memory ----

static u8* M(u32 a) { return W.h->ram(a); }
static u16 rd16(u32 a) { const u8* p = M(a); return (u16)(p[0] | p[1] << 8); }
static u32 rd32(u32 a) { const u8* p = M(a); return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void wr16(u32 a, u16 v) { u8* p = M(a); p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u32 a, u32 v) { u8* p = M(a); p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
// (the game's buffers never cross the end of the RAM the host maps: one lookup a block.
// On the DS, byte by byte through the host made a lot of main RAM traffic from the ARM7.)
static void copyIn(void* dst, u32 src, unsigned n) { if (n) memcpy(dst, M(src), n); }
static void copyOut(u32 dst, const void* src, unsigned n) { if (n) memcpy(M(dst), src, n); }
static void fill(u32 dst, u8 v, unsigned n) { if (n) memset(M(dst), v, n); }

static u16 sget16(unsigned off) { return rd16(W.status + off); }
static void sset16(unsigned off, u16 v) { wr16(W.status + off, v); }
static u32 sget32(unsigned off) { return rd32(W.status + off); }
static void sset32(unsigned off, u32 v) { wr32(W.status + off, v); }
static u16 pp16(unsigned off) { return sget16(S_pparam + off); }
static u32 pp32(unsigned off) { return sget32(S_pparam + off); }

static void put16(u8* p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8* p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }
static u16 get16(const u8* p) { return (u16)(p[0] | p[1] << 8); }

static void logf_(const char* text) { if (W.h->log) W.h->log(text); }

static void logNum(const char* what, u32 a, u32 b)
{
	// a tiny formatter: no printf on the DS side
	char t[96];
	unsigned n = 0;
	while (*what && n < 60) t[n++] = *what++;
	const u32 v[2] = {a, b};
	for (int k = 0; k < 2; k++) {
		t[n++] = ' ';
		for (int s = 28; s >= 0; s -= 4) t[n++] = "0123456789ABCDEF"[(v[k] >> s) & 15];
	}
	t[n] = 0;
	logf_(t);
}

// ---- callbacks to the ARM9 ----

// a callback (any of the WM callback structs) for the ARM9, in order
static u8* newCallback(u16 apiid, u16 errcode, unsigned len)
{
	if (W.cbIn - W.cbOut >= CB_QUEUE) {
		// (shouldn't happen: the MP rounds hold back while the queue is nearly full, see
		// cbRoom; dropping the oldest lost callbacks the game was waiting for, and it
		// crashed. Now this one goes, into a spare, and the order of the rest holds)
		static u32 dropped, lastLog;
		static QueuedCallback spare;
		dropped++;
		if (W.frames - lastLog >= 60) { logNum("wmhle: callback queue full, dropped", dropped, 0); lastLog = W.frames; dropped = 0; }
		return spare.data;
	}
	QueuedCallback* c = &W.cb[W.cbIn++ % CB_QUEUE];
	memset(c->data, 0, sizeof(c->data));
	c->buf = 0;
	c->len = (u16)(len < CB_MAX ? len : CB_MAX);
	put16(c->data + 0, apiid);
	put16(c->data + 2, errcode);
	return c->data;
}

// the receive buffer the callback just queued points into
static void cbSetBuf(u32 buf) { W.cb[(W.cbIn - 1) % CB_QUEUE].buf = buf; }

// a receive buffer callbacks still point into: queued ones, or the one the ARM9 has
// now (CALLBACK_CONTROL busy). The game has two, used in turn: overwriting one before
// the ARM9 has read what it points to gives the game wrong data
static bool bufBusy(u32 buf)
{
	if (!buf) return false;
	if ((rd16(CALLBACK_CONTROL) & 1) && W.inFlightBuf == buf) return true;
	for (u32 i = W.cbOut; i != W.cbIn; i++) if (W.cb[i % CB_QUEUE].buf == buf) return true;
	return false;
}

// room in the queue for an MP round's callbacks (MPEND, the packets it brings, the sends
// it completes): while there isn't, rounds wait (on real hardware the ARM9 can be slow
// to take them; the round and its data wait instead of callbacks being lost)
#define CB_RESERVE 12
static bool cbRoom(void) { return CB_QUEUE - (W.cbIn - W.cbOut) >= CB_RESERVE; }

// (testing: g_wmhleBusyTest = chance per frame in 1000 of the ARM9 "falling behind" for 10-40
// frames, as a real game does while it saves: the child holds CMDs then, see holdCmd)
int g_wmhleBusyTest;
static bool busyTest(void)
{
	static u32 until, lastFrame, rng = 12345;
	if (!g_wmhleBusyTest) return false;
	if (W.frames != lastFrame) {
		lastFrame = W.frames;
		rng = rng * 1103515245u + 12345u;
		{
			// (negative: short blips of 1-2 frames, the held CMD let through before the ack comes)
			const u32 chance = (u32)(g_wmhleBusyTest < 0 ? -g_wmhleBusyTest : g_wmhleBusyTest);
			if (W.frames >= until && (rng >> 16) % 1000 < chance) until = W.frames + (g_wmhleBusyTest < 0 ? 1 + (rng >> 8) % 2 : 10 + (rng >> 8) % 31);
		}
	}
	return W.frames < until;
}

static void rqReset(void);   // (the MP reply queue: emptied for a new session)

// the simple form: {apiid, errcode, wlCmdID 0, wlResult 0}
static void reply(u16 apiid, u16 errcode)
{
	newCallback(apiid, errcode, 8);
}

// hands queued callbacks to the ARM9 while it takes them
static void deliverCallbacks(void)
{
	while (W.initialized && W.cbOut != W.cbIn) {
		if (rd16(CALLBACK_CONTROL) & 1) {
			if (!W.h->waitArm9 || !W.h->waitArm9()) return;
			if (rd16(CALLBACK_CONTROL) & 1) return;
		}
		if (W.h->arm9FifoFull && W.h->arm9FifoFull()) return;     // (next chance)
		QueuedCallback* c = &W.cb[W.cbOut++ % CB_QUEUE];
		{
			const u16 api = get16(c->data);
			// (every routine one only with g_wmhleVerbose: on the DS each line costs ARM7 time the
			// game's graphics feel; the Union Room's parent/scan cycle makes many)
			const u16 cst = get16(c->data + 8);
			const bool notable = get16(c->data + 2) != ERR_SUCCESS
			                     || ((api == API_START_CONNECT || api == API_START_PARENT) && (cst == SC_CONNECTED || cst == SC_DISCONNECTED))
			                     || api == API_INITIALIZE || api == API_END;
			if ((g_wmhleVerbose || notable) && (
			     (api != API_START_SCAN && api != API_END_SCAN && api != API_SET_MP_DATA && api < 128 && api != API_START_MP
			     && !(api == API_START_PARENT && get16(c->data + 8) == SC_BEACON_SENT))
			    || (g_wmhleTracePorts && get16(c->data + 2) != ERR_SUCCESS && api != API_PORT_SEND)))   // (testing: every error)
				logNum("wmhle: callback api/err, state/aid", (u32)api << 16 | get16(c->data + 2), (u32)get16(c->data + 8) << 16 | get16(c->data + 0xA));
		}
		W.inFlightBuf = c->buf;
		wr16(CALLBACK_CONTROL, (u16)(rd16(CALLBACK_CONTROL) | 1));
		copyOut(W.fifo7to9, c->data, c->len);
		W.h->sendToArm9(PXI_TAG_WM | (W.fifo7to9 << 6));
		if (get16(c->data) == API_END) {
			// the ARM9 frees its WM memory (status, fifo) now: hands off it until
			// the next INITIALIZE
			W.initialized = false;
			W.cbOut = W.cbIn;
			W.scanning = W.connecting = W.waitingAck = false;
			memset(s_parents, 0, sizeof(s_parents));
		}
	}
}

// ---- bridge messages ("DSB1", as DsBridge::Msg serializes them) ----

enum { DSB_HostInfo = 1, DSB_Join, DSB_JoinOk, DSB_Cmd, DSB_Reply, DSB_Ack, DSB_Leave, DSB_Hello };
#define DSB_HEAD 40
// The message's destination, as the 802.11 frame's receiver address on the air: in the 8
// header bytes after the timestamp (the melonDS header for an emulated console's frames;
// zero from an older wmhle), its MAC and then the two mark bytes. With several consoles
// on one network everyone hears everything, as on the air: a message for another console
// (another parent's child joining, another group's replies) is dropped. The host's link
// sends one with a destination to that console only, the rest (beacons) to all.
#define DSB_DEST_AT    30
#define DSB_DEST_MARK0 0xD5
#define DSB_DEST_MARK1 0x4D

typedef struct {
	u8 type;
	u8 mac[6];
	u8 channel;
	u16 aid, seq, refSeq, frameCtl, duration;
	const u8* body;
	unsigned bodyLen;
} BridgeMsg;

static const u8 kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// dest: the console it's for (NULL: everyone, as a beacon)
static void bridgeSend(const u8* dest, u8 type, u16 aid, u16 seq, u16 refSeq, u16 frameCtl, const u8* body, unsigned bodyLen)
{
	static u8 out[DSB_HEAD + 600];
	if (bodyLen > 600) bodyLen = 600;
	memset(out, 0, DSB_HEAD);
	memcpy(out, "DSB1", 4);
	memcpy(out + DSB_DEST_AT, dest ? dest : kBroadcastMac, 6);
	out[DSB_DEST_AT + 6] = DSB_DEST_MARK0;
	out[DSB_DEST_AT + 7] = DSB_DEST_MARK1;
	out[4] = type;
	if (type < 9) W.txCount[type]++;
	memcpy(out + 5, W.h->mac, 6);
	out[11] = (u8)pp16(PP_channel);
	put16(out + 12, aid);
	put16(out + 14, seq);
	put16(out + 16, refSeq);
	put16(out + 18, frameCtl);
	put16(out + 20, 0);                         // duration
	put32(out + 22, W.frames * 16715);          // timestamp (µs, about): only for the log on the far side
	put16(out + 38, (u16)bodyLen);
	if (bodyLen) memcpy(out + DSB_HEAD, body, bodyLen);
	W.h->bridgeSend(out, DSB_HEAD + bodyLen);
}

static bool bridgeParse(const u8* in, unsigned len, BridgeMsg* m)
{
	if (len < DSB_HEAD || memcmp(in, "DSB1", 4) != 0) return false;
	m->type = in[4];
	memcpy(m->mac, in + 5, 6);
	m->channel = in[11];
	m->aid = get16(in + 12);
	m->seq = get16(in + 14);
	m->refSeq = get16(in + 16);
	m->frameCtl = get16(in + 18);
	m->duration = get16(in + 20);
	m->bodyLen = get16(in + 38);
	if (DSB_HEAD + m->bodyLen > len) return false;
	m->body = in + DSB_HEAD;
	return true;
}

// for another console (a destination that's neither us nor everyone)?
static bool bridgeNotForUs(const u8* in)
{
	if (in[DSB_DEST_AT + 6] != DSB_DEST_MARK0 || in[DSB_DEST_AT + 7] != DSB_DEST_MARK1) return false;
	const u8* d = in + DSB_DEST_AT;
	return !(d[0] & 1) && memcmp(d, W.h->mac, 6) != 0;
}

// a deauthentication to one console: the reason, then whom it's for (as WideMelon's: a
// child takes its parent's as its own only if it names it, so a parent letting one child
// go doesn't end every child's link)
static void sendLeave(const u8* to)
{
	u8 body[8];
	put16(body, 3);         // reason: leaving
	memcpy(body + 2, to, 6);
	bridgeSend(to, DSB_Leave, sget16(S_aid), 0, 0xFFFF, 0x00C0, body, 8);
}
static void sendLeaveTo(u16 aid) { u8 mac[6]; memcpy(mac, M(W.status + S_childMacAddress + 6 * (aid - 1)), 6); sendLeave(mac); }

// ---- requests ----

static u32 arg(u32 req, int n) { return rd32(req + 4 * n); }

static void setState(u16 s) { sset16(S_state, s); }
static u16 state(void) { return sget16(S_state); }

static void commonInit(u32 miscFlags)
{
	// as WMSPi_CommonInit: defaults, all ports without a sequence yet
	fill(W.status, 0, S_SIZE);
	sset16(S_mp_minFreq, 1);
	sset16(S_mp_freq, 1);
	sset16(S_mp_maxFreq, 6);
	sset16(S_mp_parentVCount, 260);
	sset16(S_mp_childVCount, 240);
	sset16(S_mp_parentInterval, 1000);
	sset16(S_preamble, 1);
	sset32(S_miscFlags, miscFlags);
	for (int i = 0; i < 16 * 8; i++) sset16(S_portSeqNo + 2 * i, 1);
	copyOut(W.status + S_MacAddress, W.h->mac, 6);
	// as a DS's ARM7 leaves the block (melonDS's emulated firmware, compared field by field): the
	// channels local wireless may use (1, 7 and 13), no link yet, the power save the child
	// wants, the firmware's version and ids, the MP rates in force. (Platinum's Underground stood
	// with its own player frozen on the block wmhle had left: channels 1-13, link level 3)
	sset16(S_enableChannel, 0x3FFE);
	sset16(S_allowedChannel, 0x1041);
	sset16(S_linkLevel, 0);
	sset16(S_pwrMgtMode, 1);
	sset32(S_mp_childInterval, 0x20B);
	{
		static const u16 wl[8] = { 0x2E32, 0x3338, 0x302E, 0x0030, 0xC340, 0x0003, 0x006D, 0x5B1A };   // "2.83.00", ids
		for (int i = 0; i < 8; i++) sset16(S_wlVersion + 2 * i, wl[i]);
	}
	sset16(0x1EC, 2);
	sset16(0x7C0, 1); sset16(0x7C2, 1); sset16(0x7C4, 6);
	// and its own buffer: where the status block and the callback FIFO are
	wr32(W.arm7buf + 0, W.status);
	wr32(W.arm7buf + 8, W.fifo7to9);
	for (int i = 0; i < 32; i++) wr32(W.arm7buf + 0xD0 + 16 * i, 0x8000);
	memset(W.sendq, 0, sizeof(W.sendq));
	for (int p = 0; p < 16; p++) W.nextSeqNo[p] = 1;
	memset(W.lastRecvSeq, 0xFF, sizeof(W.lastRecvSeq));
	W.scanning = W.connecting = false;
}

static void setSizes(u16 parentSize, u16 childSize, bool parent)
{
	// as WMSP_SetParentMaxSize / WMSP_SetChildMaxSize
	if (parentSize > 0x200) parentSize = 0x200;
	if (childSize > 0x200) childSize = 0x200;
	sset16(S_mp_parentMaxSize, parentSize);
	sset16(S_mp_childMaxSize, childSize);
	sset16(S_mp_parentSize, parentSize);
	sset16(S_mp_childSize, childSize);
	if (parent) {
		sset16(S_mp_sendSize, (u16)(parentSize + 4)); sset16(S_mp_maxSendSize, (u16)(parentSize + 4));
		sset16(S_mp_recvSize, (u16)(childSize + 2)); sset16(S_mp_maxRecvSize, (u16)(childSize + 2));
	} else {
		sset16(S_mp_sendSize, (u16)(childSize + 2)); sset16(S_mp_maxSendSize, (u16)(childSize + 2));
		sset16(S_mp_recvSize, (u16)(parentSize + 4)); sset16(S_mp_maxRecvSize, (u16)(parentSize + 4));
	}
}

static struct {      // the parent's MP round in progress (see parentMpRound)
	bool pending;
	u16 children;
	u16 cmdSeq;         // the CMD's sequence number: replies name it (refSeq)
	u32 startFrame;
	SendPacket* sent[16];
	int nsent;
} s_round;

static void stopMp(void)
{
	s_round.pending = false;
	sset32(S_mp_flag, 0);
	memset(W.sendq, 0, sizeof(W.sendq));
	W.waitingAck = false;
}

static void sendParentInfo(void);

static void reqStartParent(u32 req)
{
	u8* c;
	if (state() != ST_IDLE) { c = newCallback(API_START_PARENT, ERR_ILLEGAL_STATE, 0x30); return; }
	rqReset();
	setState(ST_PARENT);
	sset16(S_mode, 1);
	sset16(S_aid, 0);
	sset16(S_child_bitmap, 0);
	setSizes(pp16(PP_parentMaxSize), pp16(PP_childMaxSize), true);
	c = newCallback(API_START_PARENT, ERR_SUCCESS, 0x30);
	put16(c + 8, SC_PARENT_START);
	sset16(S_beaconIndicateFlag, 1);    // as WMSP_StartParent: "beacon sent" callbacks on
	W.nextBeacon = W.frames;
	sendParentInfo();
	logNum("wmhle: hosting, channel/maxEntry", pp16(PP_channel), pp16(PP_maxEntry));
	(void)req;
}

static void endParent(void)
{
	sset16(S_beaconIndicateFlag, 0);
	for (u16 a = 1; a <= MAX_CHILD; a++) if (sget16(S_child_bitmap) & (1 << a)) sendLeaveTo(a);
	sset16(S_child_bitmap, 0);
	sset16(S_linkLevel, 0);
	stopMp();
	setState(ST_IDLE);
}

static void reqStartScan(u32 req)
{
	// WMStartScanReq: channel at 2, scanBuf at 4, maxChannelTime at 8 (ms)
	const u16 st = state();
	if (st != ST_IDLE && st != ST_SCAN) { reply(API_START_SCAN, ERR_ILLEGAL_STATE); return; }
	setState(ST_SCAN);
	W.scanning = true;
	W.scanChannel = rd16(req + 2);
	W.scanBuf = rd32(req + 4);
	sset16(S_mode, 2);
	sset32(S_pInfoBuf, W.scanBuf);
	u16 ms = rd16(req + 8);
	if (ms < 10) ms = 10;
	W.scanEnd = W.frames + (ms * 60 + 999) / 1000;
	sset16(S_scan_channel, W.scanChannel);
	// bssid at 10: a parent to look for (a game finding its parent again), or broadcast for any
	copyIn(W.scanBssid, req + 10, 6);
	W.scanBssidAny = (W.scanBssid[0] & 1) || memcmp(W.scanBssid, "\0\0\0\0\0\0", 6) == 0;
	// (testing: a scan's start, at most one a second)
	static u32 lastLog;
	if (g_wmhleVerbose && W.frames - lastLog >= 60) { lastLog = W.frames; logNum("wmhle: scan, channel / ms", W.scanChannel, ms); }
}

// a parent this scan would hear now: heard lately, on its channel, the one it looks for. Of
// several, the one reported longest ago (each scan reports one, as the real one stops at the
// first beacon it hears)
static RemoteParent* scanPick(void)
{
	RemoteParent* best = NULL;
	for (int i = 0; i < MAX_PARENTS; i++) {
		RemoteParent* p = &s_parents[i];
		if (!p->used || W.frames - p->heardAt >= PARENT_HEARD_FRAMES) continue;
		if (W.scanChannel != 0 && p->channel != W.scanChannel) continue;
		if (!W.scanBssidAny && memcmp(p->mac, W.scanBssid, 6) != 0) continue;
		if (!best || (s32)(p->reportedAt - best->reportedAt) < 0) best = p;
	}
	return best;
}

// a scan's result: a parent if we've heard one on this channel lately
static void scanTick(void)
{
	if (!W.scanning) return;
	RemoteParent* p = scanPick();
	if (!p && (s32)(W.frames - W.scanEnd) < 0) return;
	W.scanning = false;
	u8* c = newCallback(API_START_SCAN, ERR_SUCCESS, 0x38 + 0x80);
	put16(c + 0x10, W.scanChannel);
	if (!p) {
		put16(c + 8, SC_PARENT_NOT_FOUND);
		static u32 lastNone;
		if (g_wmhleVerbose && W.frames - lastNone >= 60) { lastNone = W.frames; logNum("wmhle: scan found no parent, channel", W.scanChannel, 0); }
		return;
	}
	// the full WMBssDesc goes to the ARM9's scan buffer, the essentials into the callback
	u8 desc[192];
	AirBeaconInfo info;
	if (!airParseBeacon(p->beacon, p->len, p->mac, p->channel, desc, &info)) {
		put16(c + 8, SC_PARENT_NOT_FOUND);
		return;
	}
	p->reportedAt = W.frames;
	if (W.scanBuf) copyOut(W.scanBuf, desc, sizeof(desc));
	static u32 lastFound;
	if (W.frames - lastFound >= 300) { logNum("wmhle: scan found a parent (MAC end), channel", (u32)p->mac[4] << 8 | p->mac[5], p->channel); lastFound = W.frames; }
	put16(c + 8, SC_PARENT_FOUND);
	airFillScanCallback(c, desc);               // MAC, channel, SSID, game info: as the real scan fills them
	put16(c + 0x12, 3);                         // link level
}

static void reqStartConnect(u32 req)
{
	// WMStartConnectReq: pInfo at 4 (WMBssDesc*), ssid[24] at 8, powerSave at 0x20
	const u16 st = state();
	if (st != ST_IDLE && st != ST_SCAN) { u8* c = newCallback(API_START_CONNECT, ERR_ILLEGAL_STATE, 0x1A); (void)c; return; }
	W.scanning = false;
	const u32 pInfo = rd32(req + 4);
	u8 desc[192], ssid24[24];
	copyIn(desc, pInfo, sizeof(desc));
	copyIn(ssid24, req + 8, 24);
	// the child's power save mode (the ARM9's WM_StartMP wants 1 on a child)
	sset16(S_pwrMgtMode, rd32(req + 0x20) ? 1 : 0);
	copyOut(W.arm7buf + A7_connectPInfo, desc, sizeof(desc));
	setState(ST_CLASS1);
	u8* c = newCallback(API_START_CONNECT, ERR_SUCCESS, 0x1A);
	put16(c + 8, SC_CONNECT_START);
	// the association request, as a DS child sends it
	u8 body[128];
	const unsigned n = airBuildAssocReq(body, desc, ssid24);
	memcpy(W.remoteMac, desc + 4, 6);
	copyOut(W.status + S_parentMacAddress, desc + 4, 6);
	bridgeSend(W.remoteMac, DSB_Join, 0, W.cmdSeq += 0x10, 0xFFFF, 0x0000, body, n);
	W.connecting = true;
	W.connectStart = W.frames;
}

static void connectTick(void)
{
	if (!W.connecting || W.frames - W.connectStart < 120) return;
	// no answer in 2 seconds
	W.connecting = false;
	setState(ST_IDLE);
	u8* c = newCallback(API_START_CONNECT, ERR_TIMEOUT, 0x1A);
	put16(c + 8, SC_DISCONNECTED);
}

static void reqStartMp(u32 req)
{
	// WMStartMPReq: recvBuf 4, recvBufSize 8 (halfwords), sendBuf 0xC, sendBufSize 0x10, param 0x14, tmpParam 0x30
	const u16 st = state();
	if (st != ST_PARENT && st != ST_CHILD) { u8* c = newCallback(API_START_MP, ERR_ILLEGAL_STATE, 0x24); (void)c; return; }
	const u32 recvBuf = rd32(req + 4), recvHalf = rd32(req + 8);
	sset32(S_mp_recvBuf0, recvBuf);
	sset32(S_mp_recvBuf1, recvBuf + recvHalf);
	sset16(S_mp_recvBufSize, (u16)recvHalf);
	sset16(S_mp_recvBufSel, 0);
	sset32(S_mp_sendBuf, rd32(req + 0xC));
	sset16(S_mp_sendBufSize, (u16)rd32(req + 0x10));
	const u32 tmask = rd32(req + 0x30);
	if (tmask & 1) sset16(S_mp_minFreq, rd16(req + 0x34));
	if (tmask & 2) sset16(S_mp_freq, rd16(req + 0x36));
	sset32(S_mp_flag, 1);
	setState(st == ST_PARENT ? ST_MP_PARENT : ST_MP_CHILD);
	memset(W.childReplied, 0, sizeof(W.childReplied));
	u8* c = newCallback(API_START_MP, ERR_SUCCESS, 0x24);
	put16(c + 4, SC_MP_START);
}

static void reqSetMpData(u32 req)
{
	// buf[1] data, [2] size, [3] destBitmap, [4] port, [5] priority, [6] callback, [7] arg
	const u16 st = state();
	u8* c;
	if (st != ST_MP_PARENT && st != ST_MP_CHILD) {
		if (g_wmhleTracePorts) logNum("wmhle: setmp REJECTED (state), port", (u16)arg(req, 4), st);
		c = newCallback(API_PORT_SEND, ERR_ILLEGAL_STATE, 0x28);
		put16(c + 8, SC_PORT_SEND);
		put16(c + 0x0A, (u16)arg(req, 4));
		put32(c + 0x14, arg(req, 1));
		put16(c + 0x18, (u16)arg(req, 2));
		put32(c + 0x1C, arg(req, 6));
		put32(c + 0x20, arg(req, 7));
		return;
	}
	W.nMpData++;
	SendPacket* p = NULL;
	for (int i = 0; i < SEND_QUEUE; i++) if (!W.sendq[i].used) { p = &W.sendq[i]; break; }
	if (!p) {
		if (g_wmhleTracePorts) logNum("wmhle: setmp REJECTED (queue full), port", (u16)arg(req, 4), 0);
		c = newCallback(API_PORT_SEND, ERR_SEND_QUEUE_FULL, 0x28);
		put16(c + 8, SC_PORT_SEND);
		put32(c + 0x1C, arg(req, 6));
		put32(c + 0x20, arg(req, 7));
		return;
	}
	memset(p, 0, sizeof(*p));
	p->used = true;
	p->uid = ++W.nextUid;
	p->data = arg(req, 1);
	p->size = (u16)arg(req, 2);
	p->destBitmap = (u16)arg(req, 3);
	p->port = (u16)(arg(req, 4) & 15);
	p->priority = (u16)(arg(req, 5) & 3);
	p->callback = arg(req, 6);
	p->arg = arg(req, 7);
	if (st == ST_MP_PARENT) {
		p->destBitmap &= (u16)(sget16(S_child_bitmap) | 1);
		p->restBitmap = (u16)(p->destBitmap & ~1);
	} else {
		p->restBitmap = 1;      // to the parent
	}
	if (p->port & 8) p->seqNo = W.nextSeqNo[p->port]++ & 0x7FFF;
	if (g_wmhleTracePorts) logNum("wmhle: setmp port/seq, size", (u32)p->port << 16 | p->seqNo, p->size);
}

static void sendPortSendCallback(SendPacket* p, u16 errcode)
{
	if (errcode == ERR_SUCCESS) W.nPortSendOk++; else W.nPortSendFail++;
	if (g_wmhleTracePorts) logNum("wmhle: sendcb port/seq, err", (u32)p->port << 16 | p->seqNo, errcode);
	u8* c = newCallback(API_PORT_SEND, errcode, 0x28);
	put16(c + 8, SC_PORT_SEND);
	put16(c + 0x0A, p->port);
	put16(c + 0x0C, p->destBitmap);
	put16(c + 0x0E, p->restBitmap);
	put16(c + 0x10, (u16)(p->destBitmap & ~p->restBitmap));
	put32(c + 0x14, p->data);
	put16(c + 0x18, p->size);
	put16(c + 0x1A, p->seqNo);
	put32(c + 0x1C, p->callback);
	put32(c + 0x20, p->arg);
	put16(c + 0x24, sget16(S_mp_maxSendSize));
	put16(c + 0x26, sget16(S_mp_maxRecvSize));
	p->used = false;
}

static void dropPacketsFor(u16 aid);

static void reqDisconnect(u32 req)
{
	const u16 bitmap = (u16)arg(req, 1);
	u8* c = newCallback(API_DISCONNECT, ERR_SUCCESS, 0x0C);
	put16(c + 8, bitmap);
	put16(c + 0x0A, bitmap);
	if (state() == ST_CHILD || state() == ST_MP_CHILD) {
		sendLeave(W.remoteMac);
		stopMp();
		setState(ST_IDLE);
	} else {
		// (each child let go is told, by name: the others stay)
		for (u16 a = 1; a <= MAX_CHILD; a++) if ((bitmap & sget16(S_child_bitmap)) & (1 << a)) sendLeaveTo(a);
		sset16(S_child_bitmap, (u16)(sget16(S_child_bitmap) & ~bitmap));
		for (u16 a = 1; a <= MAX_CHILD; a++) if (bitmap & (1 << a)) dropPacketsFor(a);
	}
}

static void runRequest(u32 req)
{
	u16 apiid = rd16(req) & 0x7FFF;
	if (apiid >= API_ASYNC_KIND_MAX) return;
	if (apiid != API_INITIALIZE && apiid != API_ENABLE && !W.initialized) return;
	if (W.initialized) { sset16(S_apiBusy, 1); sset16(S_BusyApiid, apiid); }
	if (apiid != API_SET_MP_DATA && apiid != API_START_SCAN && apiid != API_END_SCAN
	    && (g_wmhleVerbose || apiid == API_INITIALIZE || apiid == API_END || apiid == API_START_CONNECT || apiid == API_DISCONNECT))   // (routine ones: verbose only)
		logNum("wmhle: request", apiid, state());

	switch (apiid) {
		case API_INITIALIZE:
		case API_ENABLE:
			W.arm7buf = arg(req, 1);
			W.status = arg(req, 2);
			W.fifo7to9 = arg(req, 3);
			W.initialized = true;
			// (a fresh start, e.g. after a soft reset: nothing left over from before, and the
			// callback hand-over free; a stale "busy" bit would hold every callback back)
			W.cbOut = W.cbIn;
			wr16(CALLBACK_CONTROL, (u16)(rd16(CALLBACK_CONTROL) & ~1));
			rqReset();
			W.lifeFrames = LIFETIME_DEFAULT_FRAMES;
			commonInit(arg(req, 4));
			setState(apiid == API_INITIALIZE ? ST_IDLE : ST_STOP);
			logNum("wmhle: initialize, status/fifo", W.status, W.fifo7to9);
			W.initReplyPending = true;          // (answered in INIT_REPLY_FRAMES: see initTick)
			W.initReplyApi = apiid;
			W.initReplyAt = W.frames + INIT_REPLY_FRAMES;
			break;
		case API_POWER_ON:   setState(ST_IDLE); reply(apiid, ERR_SUCCESS); break;
		case API_POWER_OFF:  setState(ST_STOP); reply(apiid, ERR_SUCCESS); break;
		case API_DISABLE:    setState(ST_READY); reply(apiid, ERR_SUCCESS); break;
		case API_RESET:
			if (state() == ST_PARENT || state() == ST_MP_PARENT) endParent();
			stopMp();
			W.scanning = W.connecting = false;
			setState(ST_IDLE);
			reply(apiid, ERR_SUCCESS);
			break;
		case API_END:
			stopMp();
			setState(ST_READY);
			reply(apiid, ERR_SUCCESS);
			logf_("wmhle: end");
			break;
		case API_SET_P_PARAM:
			copyOut(W.status + S_pparam, M(arg(req, 1)), 0x40);
			reply(apiid, ERR_SUCCESS);
			break;
		case API_SET_GAMEINFO: {
			const u32 attr = arg(req, 5);
			sset32(S_pparam + PP_userGameInfo, arg(req, 1));
			sset16(S_pparam + PP_userGameInfoLength, (u16)arg(req, 2));
			sset32(S_pparam + PP_ggid, arg(req, 3));
			sset16(S_pparam + PP_tgid, (u16)arg(req, 4));
			sset16(S_pparam + PP_entryFlag, (u16)(attr & 1));
			sset16(S_pparam + PP_multiBootFlag, (u16)((attr >> 1) & 1));
			sset16(S_pparam + PP_KS_Flag, (u16)((attr >> 2) & 1));
			sset16(S_pparam + PP_CS_Flag, (u16)((attr >> 3) & 1));
			reply(apiid, ERR_SUCCESS);
			break;
		}
		case API_SET_BEACON_IND: sset16(S_beaconIndicateFlag, (u16)arg(req, 1)); reply(apiid, ERR_SUCCESS); break;
		case API_SET_ENTRY:      sset16(S_pparam + PP_entryFlag, (u16)arg(req, 1)); reply(apiid, ERR_SUCCESS); break;
		case API_SET_BEACON_PERIOD: sset16(S_pparam + PP_beaconPeriod, (u16)arg(req, 1)); reply(apiid, ERR_SUCCESS); break;
		case API_SET_PS_MODE:    sset16(S_pwrMgtMode, (u16)arg(req, 1)); reply(apiid, ERR_SUCCESS); break;
		case API_SET_LIFETIME: {
			// tableNumber, camLifeTime, frameLifeTime, mpLifeTime (100 ms; 0xFFFF: none)
			const u16 mp = (u16)arg(req, 4);
			W.lifeFrames = mp == 0xFFFF ? 0 : mp ? (u32)mp * 6 : LIFETIME_DEFAULT_FRAMES;
			logNum("wmhle: lifetime: MP (100 ms), frames", mp, W.lifeFrames);
			reply(apiid, ERR_SUCCESS);
			break;
		}
		case API_SET_WEPKEY:
		case API_SET_WEPKEY_EX:
		case API_INIT_W_COUNTER:
			reply(apiid, ERR_SUCCESS);
			break;
		case API_SET_MP_PARAMETER: {
			u8* c = newCallback(apiid, ERR_SUCCESS, 0x24);
			put32(c + 4, rd32(req + 4));
			break;
		}
		case API_MEASURE_CHANNEL: {
			// WMMeasureChannelReq: channel at 6. The game hosts on the quietest one: with
			// them all alike it picked a different one each time it hosted, and a child
			// that had heard its beacon on one channel asked to join after it had moved
			// (the join and what followed went unheard, the talk kept failing). So
			// channel 1 is always the quiet one: it stays put, as a real DS's does.
			u8* c = newCallback(apiid, ERR_SUCCESS, 0x0C);
			const u16 ch = rd16(req + 6);
			put16(c + 8, ch);
			put16(c + 0x0A, ch == 1 ? 0 : 30);
			break;
		}
		case API_START_PARENT:  reqStartParent(req); break;
		case API_END_PARENT:    endParent(); reply(apiid, ERR_SUCCESS); break;
		case API_START_SCAN:    reqStartScan(req); break;
		case API_END_SCAN:      W.scanning = false; setState(ST_IDLE); reply(apiid, ERR_SUCCESS); if (g_wmhleVerbose) logf_("wmhle: scan ended"); break;
		case API_START_CONNECT: reqStartConnect(req); break;
		case API_DISCONNECT:    reqDisconnect(req); break;
		case API_START_MP:      reqStartMp(req); break;
		case API_SET_MP_DATA:   reqSetMpData(req); break;
		case API_END_MP: {
			stopMp();
			if (state() == ST_MP_PARENT) setState(ST_PARENT);
			else if (state() == ST_MP_CHILD) setState(ST_CHILD);
			reply(apiid, ERR_SUCCESS);
			break;
		}
		default:
			logNum("wmhle: unhandled request", apiid, 0);
			reply(apiid, ERR_SUCCESS);
			break;
	}
	if (W.initialized) { sset16(S_apiBusy, 0); sset16(S_BusyApiid, 0); }
}

// ---- the parent: beacons, children joining, the MP round ----

// a message (the round's CMD, its ack) to each of these children, by name: as on the air, where
// one frame reaches them all, every one gets it (and the other consoles around don't)
static void bridgeSendChildren(u16 children, u8 type, u16 seq, u16 frameCtl, const u8* body, unsigned n)
{
	for (u16 a = 1; a <= MAX_CHILD; a++) {
		if (!((children >> a) & 1)) continue;
		u8 mac[6];
		memcpy(mac, M(W.status + S_childMacAddress + 6 * (a - 1)), 6);
		bridgeSend(mac, type, 0, seq, 0xFFFF, frameCtl, body, n);
	}
}

static void sendParentInfo(void)
{
	// the beacon body, built from the parent parameters and the game info
	AirParentInfo pi;
	memset(&pi, 0, sizeof(pi));
	memcpy(pi.mac, W.h->mac, 6);
	pi.ggid = pp32(PP_ggid);
	pi.tgid = pp16(PP_tgid);
	pi.entryFlag = (u8)pp16(PP_entryFlag);
	pi.multiBootFlag = (u8)pp16(PP_multiBootFlag);
	pi.ksFlag = (u8)pp16(PP_KS_Flag);
	pi.csFlag = (u8)pp16(PP_CS_Flag);
	pi.maxEntry = pp16(PP_maxEntry);
	pi.parentMaxSize = pp16(PP_parentMaxSize);
	pi.childMaxSize = pp16(PP_childMaxSize);
	pi.beaconPeriod = pp16(PP_beaconPeriod) ? pp16(PP_beaconPeriod) : 50;
	pi.channel = (u8)pp16(PP_channel);
	pi.tsf = (u64)W.frames * 16715;
	pi.vtsf = airVtsf((u32)pi.tsf, 0);         // the frame-start sync value, as the SDK puts it in each beacon
	pi.userGameInfoLength = pp16(PP_userGameInfoLength);
	if (pi.userGameInfoLength > sizeof(pi.userGameInfo)) pi.userGameInfoLength = sizeof(pi.userGameInfo);
	copyIn(pi.userGameInfo, pp32(PP_userGameInfo), pi.userGameInfoLength);
	u8 body[256];
	const unsigned n = airBuildBeacon(body, &pi);
	bridgeSend(NULL, DSB_HostInfo, 0, W.beaconSeq += 0x10, 0xFFFF, 0x0080, body, n);   // (to everyone in range)
	W.nextBeacon = W.frames + (pi.beaconPeriod * 60 + 999) / 1000;
	// "beacon sent", if the game asked (WM_SetBeaconIndication): the Union Room counts these
	// to know when to stop hosting a while and look for others
	if (sget16(S_beaconIndicateFlag)) {
		u8* c = newCallback(API_START_PARENT, ERR_SUCCESS, 0x30);
		put16(c + 8, SC_BEACON_SENT);
	}
}

// a remote child asks to join
static void onJoin(const BridgeMsg* m)
{
	const u16 st = state();
	if (st != ST_PARENT && st != ST_MP_PARENT) return;
	u8 ssid32[32];
	if (!airParseAssocReq(m->body, m->bodyLen, ssid32)) return;
	// a free association id (the same child again keeps its own)
	u16 aid = 0;
	for (u16 a = 1; a <= MAX_CHILD; a++)
		if ((sget16(S_child_bitmap) & (1 << a)) && memcmp(M(W.status + S_childMacAddress + 6 * (a - 1)), m->mac, 6) == 0) aid = a;
	if (!pp16(PP_entryFlag) && !aid) return;
	for (u16 a = 1; a <= pp16(PP_maxEntry) && a <= MAX_CHILD && !aid; a++)
		if (!(sget16(S_child_bitmap) & (1 << a))) aid = a;
	if (!aid) return;
	copyOut(W.status + S_childMacAddress + 6 * (aid - 1), m->mac, 6);
	sset16(S_child_bitmap, (u16)(sget16(S_child_bitmap) | 1 << aid));
	W.childReplied[aid] = false;
	W.childHeardAt[aid] = W.frames;
	W.childPolls[aid] = 0;
	W.childMissed[aid] = 0;
	sset16(S_linkLevel, 3);     // (the signal icon: full, while linked)
	for (int p = 0; p < 16; p++) W.lastRecvSeq[aid][p] = 0xFFFF;
	u8 body[64];
	const unsigned n = airBuildAssocResp(body, aid);
	bridgeSend(m->mac, DSB_JoinOk, aid, W.cmdSeq += 0x10, 0xFFFF, 0x0010, body, n);
	u8* c = newCallback(API_START_PARENT, ERR_SUCCESS, 0x30);
	put16(c + 8, SC_CONNECTED);
	memcpy(c + 0x0A, m->mac, 6);
	put16(c + 0x10, aid);
	memcpy(c + 0x14, ssid32 + 8, 24);
	put16(c + 0x2C, pp16(PP_parentMaxSize));
	put16(c + 0x2E, pp16(PP_childMaxSize));
	logNum("wmhle: child joined, aid / MAC end", aid, (u32)m->mac[4] << 8 | m->mac[5]);
}

static void dropPacketsFor(u16 aid)
{
	// as the real ARM7 when a child goes: what was still to go to it won't, and a
	// packet left for no one is reported failed (else the next child given this aid
	// would get messages from the session before, and the game would lose its way)
	for (int i = 0; i < SEND_QUEUE; i++) {
		SendPacket* p = &W.sendq[i];
		if (!p->used || !(p->restBitmap & (1 << aid))) continue;
		p->restBitmap &= (u16)~(1 << aid);
		if (!p->restBitmap) sendPortSendCallback(p, ERR_SEND_FAILED);
	}
}

// a child's leaving, waiting for its last reply to reach the game (see onChildLeave)
static u16 s_leavePending;
static u32 s_leaveAt[MAX_CHILD + 1];
static bool childHasQueuedReply(u16 aid);    // (below, with the reply queue)

static void childLeaveNow(u16 a)
{
	logNum("wmhle: a child left: aid, its leave waited (frames)", a, (s_leavePending & (1 << a)) ? W.frames - s_leaveAt[a] : 0);
	u8 mac[6];
	memcpy(mac, M(W.status + S_childMacAddress + 6 * (a - 1)), 6);
	s_leavePending &= (u16)~(1 << a);
	sset16(S_child_bitmap, (u16)(sget16(S_child_bitmap) & ~(1 << a)));
	dropPacketsFor(a);
	u8* c = newCallback(API_START_PARENT, ERR_SUCCESS, 0x30);
	put16(c + 8, SC_DISCONNECTED);
	memcpy(c + 0x0A, mac, 6);
	put16(c + 0x10, a);
	put16(c + 0x12, 1);
}

static void onChildLeave(const BridgeMsg* m)
{
	for (u16 a = 1; a <= MAX_CHILD; a++) {
		if (!(sget16(S_child_bitmap) & (1 << a))) continue;
		if (memcmp(M(W.status + S_childMacAddress + 6 * (a - 1)), m->mac, 6) != 0) continue;
		// A child's last reply often comes just before it leaves (a game ending a trade
		// sends its "done" and disconnects at once). Taking the child out now threw that
		// reply away, still queued for the next round: the game never got the "done" and
		// ended the link in a blue screen (hardware: cancel in the trade room, a DS host).
		// So it leaves once that reply has reached the game (at most 30 frames later).
		if (childHasQueuedReply(a)) {
			if (!(s_leavePending & (1 << a))) s_leaveAt[a] = W.frames;
			s_leavePending |= (u16)(1 << a);
			continue;
		}
		childLeaveNow(a);
	}
}

// the packets that go in this frame: in priority order, at most one per port,
// as much as fits. Returns the body length.
static unsigned buildPackets(u8* out, unsigned room, bool parent, SendPacket** sent, int* nsent)
{
	unsigned n = 0;
	bool first = true, portUsed[16] = {false};
	*nsent = 0;
	for (int prio = 0; prio < 4; prio++) {
		for (int i = 0; i < SEND_QUEUE; i++) {
			SendPacket* p = &W.sendq[i];
			if (!p->used || p->priority != prio || portUsed[p->port] || !p->restBitmap) continue;
			const bool seq = (p->port & 8) != 0;
			// as WMSP_GetSendData: the first header is the frame's wmHeader, outside mp_sendSize
			const bool dest = parent && (p->restBitmap | 1) != 0xFFFF;
			const unsigned size = (p->size + 1u) & ~1u;
			const unsigned need = (first ? 0 : 2) + size + (seq ? 2 : 0) + (dest ? 2 : 0);
			if (n + need > room) continue;
			u16 header = (u16)((p->port << 8) | ((size / 2) & 0xFF));
			if (first && parent) header |= 0x8000;      // VSync, on the parent's first header (a child's has none)
			if (dest) header |= 0x1000;
			put16(out + n, header); n += 2;
			copyIn(out + n, p->data, p->size);
			if (size != p->size) out[n + p->size] = 0;
			n += size;
			if (seq) { put16(out + n, (u16)(p->seqNo & 0x7FFF)); n += 2; }
			if (dest) { put16(out + n, p->restBitmap); n += 2; }
			p->sentOnce = true;
			portUsed[p->port] = true;
			first = false;
			sent[(*nsent)++] = p;
		}
	}
	return n;
}

int g_wmhleTracePorts;      // (testing, set by the host) log every port packet received
int g_wmhleVerbose;         // (testing, set by the host) log every request and callback, stats every 2 s (else 10 s)
// port packets in a received body: one PORT_RECV callback each
static void parsePackets(u16 fromAid, const u8* mac, const u8* body, unsigned len, u32 recvBuf, u32 bodyAddr)
{
	unsigned o = 0;
	const u16 myAid = sget16(S_aid);
	while (o + 2 <= len) {
		const u16 header = get16(body + o);
		const u16 port = (header >> 8) & 15;
		unsigned size = (header & 0xFF) * 2;
		if (size == 0 && (header & 0x7FFF) == 0) break;     // padding / empty
		if (size == 0) size = 512;
		const bool seq = (port & 8) != 0, dest = (header & 0x1000) != 0;
		const unsigned dataAt = o + 2;
		unsigned next = dataAt + size + (seq ? 2 : 0) + (dest ? 2 : 0);
		if (next > len) break;
		bool forMe = true;
		u16 seqNo = 0xFFFF;
		if (seq) seqNo = get16(body + dataAt + size) & 0x7FFF;
		if (dest) forMe = (get16(body + dataAt + size + (seq ? 2 : 0)) >> myAid) & 1;
		bool dup = false;
		if (forMe && seq) {
			// a packet seen already (sent again for want of an ack) goes no further: as the
			// SDK's ARM7 (WMSP port receive), one the same as the last or up to 127 behind
			// it; anything else is new (a window of 16384 dropped far more than the SDK)
			u16* last = &W.lastRecvSeq[fromAid <= MAX_CHILD ? fromAid : 0][port];
			if (*last != 0xFFFF && (u16)((*last - seqNo) & 0x7FFF) < 0x80) { forMe = false; dup = true; W.nDupDropped++; }
			else *last = seqNo;
		}
		// (testing: every packet, delivered or not)
		if (g_wmhleTracePorts && size != 512)
			logNum(forMe ? "wmhle: rx port/seq, size/from" : dup ? "wmhle: dup port/seq, size/from" : "wmhle: notme port/seq, size/from",
			       (u32)port << 16 | seqNo, (u32)size << 16 | fromAid);
		if (forMe) {
			W.nPortRecv++;
			u8* c = newCallback(API_PORT_RECV, ERR_SUCCESS, 0x44);
			put16(c + 4, SC_PORT_RECV);
			cbSetBuf(recvBuf);
			put16(c + 6, port);
			put32(c + 8, recvBuf);
			put32(c + 0x0C, bodyAddr + dataAt);
			put16(c + 0x10, (u16)size);
			put16(c + 0x12, fromAid);
			memcpy(c + 0x14, mac, 6);
			put16(c + 0x1A, seqNo);
			put16(c + 0x20, myAid);
			put16(c + 0x40, sget16(S_mp_maxSendSize));
			put16(c + 0x42, sget16(S_mp_maxRecvSize));
		}
		o = next;
	}
}

// one MP round as the parent: CMD to the children, their replies (waiting for
// them: the host-waits rule), the result to the ARM9, then the ACK
static u8 s_replyBody[MAX_CHILD + 1][0x220];
static unsigned s_replyLen[MAX_CHILD + 1];
static bool s_replyIn[MAX_CHILD + 1];

// Replies as they come, each taken by the next round that wants that child's reply.
// None is dropped for being late: a WideMelon client takes our ack as its own (it
// acks its reply itself, at once, so as never to wait on the network), so a reply
// we dropped would never be sent again and the game's data in it would be lost
// (the talk or trade then stood waiting for good). A late reply just answers the
// next round instead.
#define REPLY_QUEUE 6
static struct { u16 aid; u16 len; u16 refSeq; u8 body[0x220]; } s_rq[REPLY_QUEUE];
static u16 s_replyRef[MAX_CHILD + 1];     // the CMD the round's reply from each child answers (0xFFFF unknown)
static int s_rqCount;

static void rqDrop(int i)
{
	for (; i + 1 < s_rqCount; i++) s_rq[i] = s_rq[i + 1];
	s_rqCount--;
}

// WideMelon sends each reply twice (a lost one would never be sent again): the copy
// (same sequence number, CMD answered and contents as the child's reply before it) goes
static struct { u16 seq, refSeq, len; u32 sum; bool any; } s_rqLast[MAX_CHILD + 1];

static void rqPush(u16 aid, u16 seq, u16 refSeq, const u8* body, unsigned len)
{
	u32 sum = 2166136261u;
	for (unsigned i = 0; i < len; i++) sum = (sum ^ body[i]) * 16777619u;
	if (s_rqLast[aid].any && s_rqLast[aid].seq == seq && s_rqLast[aid].refSeq == refSeq && s_rqLast[aid].len == len && s_rqLast[aid].sum == sum) {
		W.nReplyCopies++;
		return;
	}
	s_rqLast[aid].seq = seq; s_rqLast[aid].refSeq = refSeq; s_rqLast[aid].len = (u16)len; s_rqLast[aid].sum = sum; s_rqLast[aid].any = true;
	if (s_rqCount == REPLY_QUEUE) {
		// full: an empty ("not ready") reply goes first, else the oldest (counted)
		int i = 0;
		while (i < s_rqCount && s_rq[i].len >= 2) i++;
		if (i == s_rqCount) { i = 0; W.staleReplies++; }
		rqDrop(i);
	}
	s_rq[s_rqCount].aid = aid;
	s_rq[s_rqCount].refSeq = refSeq;
	s_rq[s_rqCount].len = (u16)len;
	memcpy(s_rq[s_rqCount].body, body, len);
	s_rqCount++;
}

// a new session (initialize, hosting again, joining): no replies from the one before
static void rqReset(void)
{
	s_rqCount = 0;
	memset(s_rqLast, 0, sizeof(s_rqLast));
	s_leavePending = 0;     // (a new session: nobody waiting to leave)
	s_round.pending = false;
}

// the open round's missing replies, from the queue (each child's oldest)
static void rqTake(void)
{
	for (int i = 0; i < s_rqCount; ) {
		const u16 a = s_rq[i].aid;
		if (!((s_round.children >> a) & 1)) { rqDrop(i); continue; }   // not in the link any more
		if (s_replyIn[a]) { i++; continue; }
		memcpy(s_replyBody[a], s_rq[i].body, s_rq[i].len);
		s_replyLen[a] = s_rq[i].len;
		s_replyRef[a] = s_rq[i].refSeq;
		s_replyIn[a] = true;
		rqDrop(i);
	}
}

// One MP round: the CMD goes out, then the replies are waited for briefly (this
// runs in the DS's VBlank interrupt, so not long); a round still missing replies
// is finished on the next frame with what has come in by then.
#define ROUND_WAIT_US 0      // no waiting in the VBlank IRQ (the ARM7 held the main RAM bus: graphics glitches);
                             // the reply closes the round from the next poll (VBlank or the mid-frame timer)
#define ROUND_MAX_FRAMES 3     // a round waits this many frames at most for its replies
#define LAGGING_ROUNDS 3       // a child whose replies missed this many rounds in a row isn't waited for

static bool childHasQueuedReply(u16 aid)
{
	if (s_round.pending && ((s_round.children >> aid) & 1) && s_replyIn[aid]) return true;
	for (int i = 0; i < s_rqCount; i++) if (s_rq[i].aid == aid) return true;
	return false;
}

// children whose leaving waited for their last reply: they go now that it's delivered
// (or after 30 frames, whatever happened)
static void leavePendingTick(void)
{
	if (!s_leavePending) return;
	for (u16 a = 1; a <= MAX_CHILD; a++)
		if ((s_leavePending & (1 << a)) && (!childHasQueuedReply(a) || W.frames - s_leaveAt[a] > 30)) childLeaveNow(a);
}

// the children the open round waits for: all of them, except, while another keeps up, any whose
// replies missed the last LAGGING_ROUNDS rounds. As on the air, where the parent's round never
// waits and a child in a bad spot only misses its turn, one child lagging (a weak WiFi signal, a
// busy PC) doesn't slow the others' rounds to its pace; its replies, when they come, go in the next
// round (rqTake), and the first one in time has it waited for again. A lone child, or all of them
// lagging, is still waited for: closing the rounds without it had the child send again round after
// round (see parentMpRound).
static u16 roundWaitsFor(void)
{
	u16 keeping = 0;
	for (u16 a = 1; a <= MAX_CHILD; a++)
		if (((s_round.children >> a) & 1) && W.childMissed[a] < LAGGING_ROUNDS) keeping |= (u16)(1 << a);
	return keeping ? keeping : s_round.children;
}

static bool roundRepliesIn(void)
{
	const u16 waits = roundWaitsFor();
	for (u16 a = 1; a <= MAX_CHILD; a++) if ((waits >> a) & 1 && !s_replyIn[a]) return false;
	return true;
}

static void parentMpFinish(void);

// the receive buffer the open round's result goes in is free: no callback the ARM9 hasn't
// read points into it (writing it then gave the game wrong data: seen on hardware with a
// heavy trade screen, the game ending the link in a blue screen); the round waits instead
static bool parentBufFree(void)
{
	return !bufBusy(sget32(sget16(S_mp_recvBufSel) ? S_mp_recvBuf1 : S_mp_recvBuf0));
}

static void parentMpRound(void)
{
	// a round still waiting for its replies stays open a little (up to ROUND_MAX_FRAMES):
	// over a network a child's reply can take longer than a frame, and closing the round
	// without it told the child "missed" and made it send again, round after round (the
	// Union Room's talk then never got through). The rounds go at the link's pace instead.
	if (s_round.pending) {
		if (!roundRepliesIn() && W.frames - s_round.startFrame < ROUND_MAX_FRAMES) return;
		if (!cbRoom() || !parentBufFree()) return;   // (the ARM9 is behind with its callbacks: the round waits)
		parentMpFinish();
	}
	if (!cbRoom()) return;               // (likewise no new round)
	const u16 children = (u16)(sget16(S_child_bitmap) & ~1);
	if (!children) return;
	u8 body[0x230];
	put16(body, 0x017E);                // txop: reply time per child, as the real ones send
	put16(body + 2, children);          // poll bitmap
	s_round.children = children;
	s_round.nsent = 0;
	unsigned n = 4 + buildPackets(body + 4, sget16(S_mp_sendSize), true, s_round.sent, &s_round.nsent);
	if (n == 4) { put16(body + 4, 0x8000); n = 6; }     // nothing to send: still a wmHeader (VSync), as real parents send
	memset(s_replyIn, 0, sizeof(s_replyIn));
	rqTake();                           // replies that came after their own round closed
	W.cmdSeq += 0x10;
	s_round.cmdSeq = W.cmdSeq;
	for (int i = 0; i < s_round.nsent; i++)
		if (!s_round.sent[i]->inCmd) { s_round.sent[i]->inCmd = true; s_round.sent[i]->firstCmd = s_round.cmdSeq; }
	s_round.startFrame = W.frames;
	bridgeSendChildren(children, DSB_Cmd, W.cmdSeq, 0x0228, body, n);
	for (u16 a = 1; a <= MAX_CHILD; a++) if (((children >> a) & 1) && W.childPolls[a] < 0xFFFF) W.childPolls[a]++;
	s_round.pending = true;

	const u32 start = W.h->nowUs();
	for (;;) {
		W.h->pollNet();                 // (a reply that completes the round closes it, in wmhleBridgeRx)
		if (!s_round.pending) return;
		if (W.h->nowUs() - start > ROUND_WAIT_US) return;
	}
}

// an MP round that got through: the longest wait between two is logged (a pause on screen)
static void mpProgress(void)
{
	if (W.lastProgress && W.frames - W.lastProgress > W.maxGap) W.maxGap = W.frames - W.lastProgress;
	W.lastProgress = W.frames;
}

static void parentMpFinish(void)
{
	s_round.pending = false;
	const u16 children = s_round.children;
	SendPacket** sent = s_round.sent;
	const int nsent = s_round.nsent;
	if (state() != ST_MP_PARENT) return;

	// the result into the receive buffer: a WMMpRecvHeader and one WMMpRecvData per child
	const u16 sel = sget16(S_mp_recvBufSel);
	const u32 rb = sget32(sel ? S_mp_recvBuf1 : S_mp_recvBuf0);
	const u16 stride = (u16)(sget16(S_mp_maxRecvSize) + 12);   // as the real ARM7 sizes the buffer
	u16 count = 0, missing = 0;
	for (u16 a = 1; a <= MAX_CHILD; a++) {
		if (!((children >> a) & 1)) continue;
		const u32 d = rb + 10 + count * stride;
		if (s_replyIn[a] && s_replyLen[a] >= 2) {
			wr16(d + 0, (u16)(s_replyLen[a] - 2));
			wr16(d + 6, 0);
			copyOut(d + 8, s_replyBody[a], s_replyLen[a] > (unsigned)stride - 8 ? (unsigned)stride - 8 : s_replyLen[a]);
		} else {
			wr16(d + 0, s_replyIn[a] ? 0 : 0xFFFF);
			wr16(d + 6, s_replyIn[a] ? 0 : 1);
			if (!s_replyIn[a]) missing |= (u16)(1 << a);
		}
		if (!s_replyIn[a]) { if (W.childMissed[a] < 0xFF) W.childMissed[a]++; }
		else W.childMissed[a] = 0;
		wr16(d + 2, 0);
		wr16(d + 4, a);
		count++;
	}
	wr16(rb + 0, missing);
	wr16(rb + 2, missing);
	wr16(rb + 4, count);
	wr16(rb + 6, stride);
	wr16(rb + 8, 1);
	sset16(S_mp_readyBitmap, (u16)(children & ~missing));
	if (!missing) mpProgress();

	// the packets each child got, and what came back
	for (u16 a = 1; a <= MAX_CHILD; a++) {
		// delivered once the child answers, with a header (an empty reply is "not ready"),
		// the CMD the packet first went in or a later one (all carry it until then): the
		// reply names the CMD it answers. A late reply (to an earlier CMD) proves nothing
		// about this round's CMD, which may have been lost on the way (counting it as
		// delivered lost the packet for good: the game then stood "Communicating").
		// (A reply naming no CMD: as before, this round's packets.)
		if (!((children >> a) & 1) || !s_replyIn[a] || s_replyLen[a] < 2) continue;
		const u16 r = s_replyRef[a];
		if (r == 0xFFFF) {
			for (int i = 0; i < nsent; i++) sent[i]->restBitmap &= (u16)~(1 << a);
			continue;
		}
		for (int i = 0; i < SEND_QUEUE; i++) {
			SendPacket* p = &W.sendq[i];
			if (p->used && p->inCmd && (u16)(r - p->firstCmd) < 0x8000) p->restBitmap &= (u16)~(1 << a);
		}
	}
	count = 0;
	for (u16 a = 1; a <= MAX_CHILD; a++) {
		if (!((children >> a) & 1)) continue;
		const u32 d = rb + 10 + count * stride;
		count++;
		if (!s_replyIn[a] || s_replyLen[a] < 2) continue;
		parsePackets(a, M(W.status + S_childMacAddress + 6 * (a - 1)), s_replyBody[a], s_replyLen[a], rb, d + 8);
	}
	// (any packet now delivered: a late reply can complete one sent rounds ago)
	for (int i = 0; i < SEND_QUEUE; i++)
		if (W.sendq[i].used && W.sendq[i].inCmd && !W.sendq[i].restBitmap) sendPortSendCallback(&W.sendq[i], ERR_SUCCESS);

	u8* c = newCallback(API_START_MP, ERR_SUCCESS, 0x24);
	put16(c + 4, SC_MPEND_IND);
	put32(c + 8, rb);
	put16(c + 0x20, children);
	sset16(S_mp_recvBufSel, (u16)(sel ^ 1));

	// the ack closes the round. After the missing bitmap, for each aid up to the highest
	// polled, the CMD the reply we took from that child answers (0xFFFF none): the child
	// completes exactly the packets that reply carried (as WideMelon's host does for aid 1;
	// without it, over a lossy link one wmhle child took "got it" for its latest reply when
	// we had taken an older one, and its lost packets left both games "Communicating")
	u8 ack[4 + 2 * MAX_CHILD];
	put16(ack, ++W.ackCounter);
	put16(ack + 2, missing);
	unsigned an = 4;
	for (u16 a = 1; a <= MAX_CHILD; a++) {
		if (!((children >> a) & 1)) continue;
		while (an < 4 + 2u * a) { put16(ack + an, 0xFFFF); an += 2; }
		put16(ack + 4 + 2 * (a - 1), s_replyIn[a] && s_replyLen[a] >= 2 ? s_replyRef[a] : 0xFFFF);
	}
	bridgeSendChildren(children, DSB_Ack, W.cmdSeq, 0x0218, ack, an);
	leavePendingTick();     // (a child that left once its last reply was in: now)
}

// (each frame, MP parent) a child that hasn't answered for the MP lifetime while we kept polling
// it (at least 30 CMDs) is gone: it's told (it may still hear us) and leaves the link. (Its
// silence meanwhile doesn't slow the others: see roundWaitsFor.)
static void childSilenceTick(void)
{
	if (!W.lifeFrames) return;
	for (u16 a = 1; a <= MAX_CHILD; a++) {
		if (!(sget16(S_child_bitmap) & (1 << a))) continue;
		if (W.frames - W.childHeardAt[a] <= W.lifeFrames || W.childPolls[a] < 30) continue;
		logNum("wmhle: a child stopped answering: aid, CMDs unanswered", a, W.childPolls[a]);
		sendLeaveTo(a);
		s_leavePending &= (u16)~(1 << a);
		childLeaveNow(a);
	}
}

// ---- the child: the parent's MP rounds ----

// our last replies as a child: the CMD each answered and the packets it carried (by uid)
#define CHILD_REPLIES 16
typedef struct { u16 ref; u8 n; u32 uid[16]; } ChildReply;
static ChildReply s_childReplies[CHILD_REPLIES];
static unsigned s_childReplyNext;
// our last reply as a child, for a copy of the CMD it answered
static struct { bool valid; u16 cmdSeq, seq, len; u8 body[0x220]; } s_lastReply;

static SendPacket* s_childSent[16];
static int s_childNsent;

// our reply's ack never came (lost on the way): the round's MPACK_IND anyway, with a
// timeout, as the SDK's ARM7 gives it from its alarm (the game always gets one per
// round it was polled in; missing ones left it waiting after a trade, over a lossy link)
static void childAckTimeout(void)
{
	if (!W.waitingAck) return;
	W.waitingAck = false;
	s_childNsent = 0;
	u8* c = newCallback(API_START_MP, ERR_TIMEOUT, 0x24);
	put16(c + 4, SC_MPACK_IND);
}

// The newest CMD that had to wait (the ARM9 behind): handed to it as soon as it can take it.
// Letting it go relied on the parent sending its data again, but its last CMD before it
// leaves (a game ending a trade: its "done", then the disconnect) never comes again: the
// game missed the "done" and ended the link in a blue screen (hardware, a cancel after a
// trade). The parent's leaving waits for it too (s_lostPending).
static struct { bool valid; bool answered; BridgeMsg m; u8 body[0x240]; } s_heldCmd;   // (answered: "not ready" sent for it)
static bool s_releaseAnswered;     // (onCmd, for a held CMD let through: it was answered "not ready" already)
static bool s_lostPending;
static u32 s_lostAt;
static void onCmd(const BridgeMsg* m);
static void childLost(u16 reasonCode);

static void holdCmd(const BridgeMsg* m)
{
	if (m->bodyLen > sizeof(s_heldCmd.body)) return;
	const bool same = s_heldCmd.valid && s_heldCmd.m.seq == m->seq;
	s_heldCmd.answered = same && s_heldCmd.answered;
	if (m->body != s_heldCmd.body) memcpy(s_heldCmd.body, m->body, m->bodyLen);
	s_heldCmd.m = *m;
	s_heldCmd.m.body = s_heldCmd.body;
	s_heldCmd.valid = true;
}

// a held CMD still gets an answer, if it polls us: the hardware's "not ready" one (empty,
// frame control 0x0158), as a real child's chip gives when its game has no reply ready.
// The parent knows we're there and sends its data again; nothing counts as delivered.
// Silence instead (no reply for half a second while the game saved after a trade) made a
// HeartGold host end the link: blue screens on both on leaving the trade room.
static void replyNotReady(const BridgeMsg* m)
{
	const u16 myAid = sget16(S_aid);
	if (m->bodyLen < 4 || !((get16(m->body + 2) >> myAid) & 1)) return;
	bridgeSend(W.remoteMac, DSB_Reply, myAid, W.cmdSeq += 0x10, m->seq, 0x0158, NULL, 0);
	s_heldCmd.answered = true;
}

// (each pump and frame) the held CMD, if the ARM9 can take it now; then a parent's leaving
// that waited for it
static void childHeldTick(void)
{
	if (s_heldCmd.valid && (state() == ST_MP_CHILD) && cbRoom() && !busyTest()) {
		const u16 nextSel = (u16)(sget16(S_mp_recvBufSel) ^ 1);
		if (!bufBusy(sget32(nextSel ? S_mp_recvBuf1 : S_mp_recvBuf0))) {
			BridgeMsg m = s_heldCmd.m;
			s_heldCmd.valid = false;
			s_releaseAnswered = s_heldCmd.answered;
			onCmd(&m);      // (may hold it again; the copy's body stays valid: holdCmd copies first)
			s_releaseAnswered = false;
		}
	}
	if (s_lostPending && (!s_heldCmd.valid || W.frames - s_lostAt > 30)) {
		s_lostPending = false;
		s_heldCmd.valid = false;
		if (state() == ST_CHILD || state() == ST_MP_CHILD) childLost(SC_DISCONNECTED);
	}
}

static void onCmd(const BridgeMsg* m)
{
	if (state() != ST_MP_CHILD && state() != ST_CHILD) return;
	if (memcmp(m->mac, W.remoteMac, 6) != 0) return;
	W.lastFromParent = W.lastCmdFromParent = W.frames;
	if (state() != ST_MP_CHILD || m->bodyLen < 4) return;
	// the CMD we just answered, again (WideMelon sends each twice, a lost one being its
	// data lost for good; a host also resends one it got no reply to): the same reply
	// again, and nothing more to the ARM9 (it has had this one)
	if (s_lastReply.valid && m->seq == s_lastReply.cmdSeq) {
		bridgeSend(W.remoteMac, DSB_Reply, sget16(S_aid), s_lastReply.seq, m->seq, s_lastReply.len ? 0x0118 : 0x0158, s_lastReply.body, s_lastReply.len);   // (no body: it was "not ready")
		return;
	}
	// the ARM9 behind with its callbacks: this CMD waits (held, see holdCmd), unanswered as
	// one lost on the air (the parent sends its data again), rather than callbacks being lost
	if (!cbRoom() || busyTest()) { W.nCmdsHeldBack++; holdCmd(m); replyNotReady(m); return; }
	// likewise while the buffer this CMD would go in is still being read by the ARM9 (the
	// game has two, used in turn): writing it gave the game wrong data (on hardware, a
	// heavy trade screen; both games then ended the link in a blue screen)
	{
		const u16 nextSel = (u16)(sget16(S_mp_recvBufSel) ^ 1);
		if (bufBusy(sget32(nextSel ? S_mp_recvBuf1 : S_mp_recvBuf0))) { W.nBufClobbered++; holdCmd(m); replyNotReady(m); return; }
	}
	s_heldCmd.valid = false;    // (this one is newer than any held)
	childAckTimeout();      // (the last round's ack never came: its MPACK_IND first)
	W.lastCmdSeq = m->seq;
	W.lastCmdFrame = W.frames;
	mpProgress();
	const u16 myAid = sget16(S_aid);
	const u16 pollBitmap = get16(m->body + 2);

	// a WMMpRecvBuf: header fields as the hardware gives them, then the frame body
	sset16(S_mp_recvBufSel, (u16)(sget16(S_mp_recvBufSel) ^ 1));
	const u32 rb = sget32(sget16(S_mp_recvBufSel) ? S_mp_recvBuf1 : S_mp_recvBuf0);
	fill(rb, 0, 0x2C);
	wr16(rb + 0x06, (u16)(m->bodyLen - 4 >= 2 ? m->bodyLen - 4 - 2 : 0));   // body length minus the wmHeader
	wr16(rb + 0x14, m->frameCtl);
	copyOut(rb + 0x18, (const u8[]){0x03, 0x09, 0xBF, 0x00, 0x00, 0x00}, 6);
	copyOut(rb + 0x1E, m->mac, 6);
	copyOut(rb + 0x24, m->mac, 6);
	wr16(rb + 0x2A, m->seq);
	const unsigned room = sget16(S_mp_recvBufSize) > 0x2C ? sget16(S_mp_recvBufSize) - 0x2C : 0;
	copyOut(rb + 0x2C, m->body, m->bodyLen < room ? m->bodyLen : room);

	const bool polled = (pollBitmap >> myAid) & 1;
	if (polled) {
		u8* c = newCallback(API_START_MP, m->bodyLen > 4 ? ERR_SUCCESS : ERR_NO_DATA, 0x24);
		put16(c + 4, SC_MP_IND);
		cbSetBuf(rb);
		put32(c + 8, rb);
		memcpy(c + 0x10, (const u8[]){0x03, 0x09, 0xBF, 0x00, 0x00, 0x00}, 6);
		memcpy(c + 0x16, m->mac, 6);
		put16(c + 0x1C, m->seq);
		put16(c + 0x20, pollBitmap);
	}
	if (m->bodyLen > 4) parsePackets(0, m->mac, m->body + 4, m->bodyLen - 4, rb, rb + 0x30);

	if (!polled) return;
	// A held CMD we already answered "not ready": its data has now gone to the game, but no
	// second reply. The parent took the "not ready" as our reply to it, and its ack names this
	// CMD: a real reply under the same CMD would have its packets counted delivered by that
	// ack, though the parent threw the late reply away (both games then stood "Communicating"
	// after a trade, on hardware). Our packets go in the next reply instead; a copy of this
	// CMD gets "not ready" again (s_lastReply with no body).
	if (s_releaseAnswered) {
		s_lastReply.valid = true;
		s_lastReply.cmdSeq = m->seq;
		s_lastReply.seq = W.cmdSeq;
		s_lastReply.len = 0;
		return;
	}
	// our reply: what's queued for the parent (resent until its ack says it got it)
	u8 body[0x220];
	unsigned n = buildPackets(body, sget16(S_mp_sendSize), false, s_childSent, &s_childNsent);
	// nothing to send: just a zero wmHeader, which tells the parent we got its packet
	// (an empty 0x0158 reply is the hardware's "not ready" one: the parent would resend)
	if (!n) { put16(body, 0); n = 2; }
	// what this reply carries, under the CMD it answers (see onAck)
	ChildReply* r = &s_childReplies[s_childReplyNext++ % CHILD_REPLIES];
	r->ref = m->seq;
	r->n = 0;
	for (int i = 0; i < s_childNsent && r->n < 16; i++) r->uid[r->n++] = s_childSent[i]->uid;
	bridgeSend(W.remoteMac, DSB_Reply, myAid, W.cmdSeq += 0x10, m->seq, 0x0118, body, n);
	// (kept, for a copy of this CMD: see above)
	s_lastReply.valid = true;
	s_lastReply.cmdSeq = m->seq;
	s_lastReply.seq = W.cmdSeq;
	s_lastReply.len = (u16)n;
	memcpy(s_lastReply.body, body, n);
	W.waitingAck = true;
	W.waitingAckFrame = W.frames;
}

// a packet in our replies, delivered at last: its callback
static void childDelivered(u32 uid)
{
	for (int i = 0; i < SEND_QUEUE; i++) {
		SendPacket* p = &W.sendq[i];
		if (p->used && p->uid == uid) {
			if (g_wmhleTracePorts) logNum("wmhle: delivered port/seq, size", (u32)p->port << 16 | p->seqNo, p->size);
			p->restBitmap = 0; sendPortSendCallback(p, ERR_SUCCESS); return;
		}
	}
}

static void onAck(const BridgeMsg* m)
{
	if (state() != ST_MP_CHILD || memcmp(m->mac, W.remoteMac, 6) != 0) return;
	const u16 err = m->bodyLen >= 4 ? get16(m->body + 2) : 0;
	const bool gotIt = !((err >> sget16(S_aid)) & 1);
	// WideMelon as host (newer ones) says which of our replies it took this round (the
	// CMD it answered): the packets that one carried are delivered. Over a network our
	// reply often comes a round late, and the host takes it then; "got it" alone was
	// taken for our latest reply, so when that one was lost its packets were lost for
	// good (the game stood "Communicating" or gave up the link).
	if (m->bodyLen >= 6) {
		// (a wmhle host: one per aid; WideMelon's: one, for its only remote)
		const u16 aid = sget16(S_aid);
		const u16 used = aid >= 1 && m->bodyLen >= 4u + 2 * aid ? get16(m->body + 4 + 2 * (aid - 1)) : get16(m->body + 4);
		if (gotIt && used != 0xFFFF)
			for (int k = 0; k < CHILD_REPLIES; k++) {
				ChildReply* r = &s_childReplies[k];
				if (r->n == 0 || r->ref != used) continue;
				for (int i = 0; i < r->n; i++) childDelivered(r->uid[i]);
				r->n = 0;
			}
		if (!W.waitingAck) return;
		W.waitingAck = false;
		s_childNsent = 0;
		u8* c = newCallback(API_START_MP, gotIt ? ERR_SUCCESS : ERR_SEND_FAILED, 0x24);   // (as the SDK: an ack that says we were missed)
		put16(c + 4, SC_MPACK_IND);
		memcpy(c + 0x16, m->mac, 6);
		return;
	}
	if (!W.waitingAck) return;
	W.waitingAck = false;
	if (gotIt)
		for (int i = 0; i < s_childNsent; i++) if (s_childSent[i]->used) { s_childSent[i]->restBitmap = 0; sendPortSendCallback(s_childSent[i], ERR_SUCCESS); }
	s_childNsent = 0;
	u8* c = newCallback(API_START_MP, gotIt ? ERR_SUCCESS : ERR_SEND_FAILED, 0x24);   // (as the SDK: an ack that says we were missed)
	put16(c + 4, SC_MPACK_IND);
	memcpy(c + 0x16, m->mac, 6);
}

static void onJoinOk(const BridgeMsg* m)
{
	if (!W.connecting || memcmp(m->mac, W.remoteMac, 6) != 0) return;
	rqReset();
	memset(s_childReplies, 0, sizeof(s_childReplies));
	s_lastReply.valid = false;
	s_heldCmd.valid = false;
	s_lostPending = false;
	u16 aid = 0;
	if (!airParseAssocResp(m->body, m->bodyLen, &aid) || !aid) return;
	W.connecting = false;
	W.lastFromParent = W.lastCmdFromParent = W.frames;
	sset16(S_aid, aid);
	sset16(S_linkLevel, 3);
	setState(ST_CHILD);
	u8 desc[192];
	copyIn(desc, W.arm7buf + A7_connectPInfo, sizeof(desc));
	const u16 parentSize = get16(desc + 0x40 + 0x0C), childSize = get16(desc + 0x40 + 0x0E);
	setSizes(parentSize, childSize, false);
	for (int p = 0; p < 16; p++) W.lastRecvSeq[0][p] = 0xFFFF;
	u8* c = newCallback(API_START_CONNECT, ERR_SUCCESS, 0x1A);
	put16(c + 8, SC_CONNECTED);
	put16(c + 0x0A, aid);
	memcpy(c + 0x10, m->mac, 6);
	put16(c + 0x16, parentSize);
	put16(c + 0x18, childSize);
	logNum("wmhle: joined, aid / parent,child size", aid, (u32)parentSize << 16 | childSize);
}

// (each frame, a child) the parent unheard: after a little the "beacon lost" notice, as the real
// ARM7 gives (the game takes no action on it), the link still up, so it carries on when the parent
// is heard again; with no CMD from it for the MP lifetime (its beacons alone don't keep the link:
// a parent that let us go sends none) the link ends. (Ending it after two seconds, with the game
// still taking itself to be linked, as before, turned every short dropout into a lost link.)
static void parentSilenceTick(void)
{
	const u32 noCmd = W.frames - W.lastCmdFromParent;
	if (W.lifeFrames && noCmd > (W.lifeFrames > BEACON_LOST_FRAMES ? W.lifeFrames : BEACON_LOST_FRAMES)) { childLost(SC_DISCONNECTED); return; }
	const u32 quiet = W.frames - W.lastFromParent;
	if (quiet <= BEACON_LOST_FRAMES) { W.beaconLostSent = false; return; }
	if (W.beaconLostSent) return;
	W.beaconLostSent = true;
	logNum("wmhle: the parent unheard (frames), link kept for (frames)", quiet, W.lifeFrames);
	u8* c = newCallback(API_START_CONNECT, ERR_SUCCESS, 0x1A);
	put16(c + 8, SC_BEACON_LOST);
	put16(c + 0x0A, sget16(S_aid));
	memcpy(c + 0x10, W.remoteMac, 6);
}

static void childLost(u16 reasonCode)
{
	// (for the log, when a link ends: why, whether a CMD was still held, frames since the last
	// CMD we took and since we last heard the parent, ARM9 callbacks still queued)
	logNum("wmhle: link to the parent ended: reason, held CMD", reasonCode, s_heldCmd.valid);
	logNum("wmhle:   frames since last CMD taken / heard, callbacks queued", (u32)((W.frames - W.lastCmdFrame) & 0xFFFF) << 16 | ((W.frames - W.lastFromParent) & 0xFFFF), W.cbIn - W.cbOut);
	s_heldCmd.valid = false;
	s_lostPending = false;
	stopMp();
	setState(ST_IDLE);
	sset16(S_linkLevel, 0);
	u8* c = newCallback(API_START_CONNECT, ERR_SUCCESS, 0x1A);
	put16(c + 8, reasonCode);
	put16(c + 0x0A, sget16(S_aid));
	memcpy(c + 0x10, W.remoteMac, 6);
	sset16(S_aid, 0);
}

// ---- the entry points ----

void wmhleInit(const WmHleHost* host)
{
	memset(&W, 0, sizeof(W));
	W.h = host;
}

bool wmhleActive(void)
{
	return W.initialized && state() >= ST_IDLE;
}

void wmhleRequest(uint32_t data)
{
	if (W.reqIn - W.reqOut >= REQ_QUEUE) return;    // the ARM9 can't have more than 10 out anyway
	W.req[W.reqIn % REQ_QUEUE] = data;
	W.reqIn++;
}

// INITIALIZE's answer, once its time has come
static void initTick(void)
{
	if (!W.initReplyPending || (s32)(W.frames - W.initReplyAt) < 0) return;
	W.initReplyPending = false;
	reply(W.initReplyApi, ERR_SUCCESS);
}

void wmhlePump(void)
{
	while (W.reqOut != W.reqIn) {
		const u32 req = W.req[W.reqOut % REQ_QUEUE];
		W.reqOut++;
		runRequest(req);
		wr16(req, (u16)(rd16(req) | REQUEST_ACCEPTED));    // the ARM9 may reuse it
	}
	initTick();
	scanTick();
	connectTick();
	childHeldTick();
	deliverCallbacks();
}

void wmhleDeliver(void) { childHeldTick(); deliverCallbacks(); }

void wmhleFrame(void)
{
	W.frames++;
	if (W.initialized) {
		const u16 st = state();
		if ((st == ST_PARENT || st == ST_MP_PARENT) && (s32)(W.frames - W.nextBeacon) >= 0) sendParentInfo();
		if (st == ST_MP_PARENT) parentMpRound();
		if (st == ST_PARENT || st == ST_MP_PARENT) leavePendingTick();
		if (st == ST_CHILD || st == ST_MP_CHILD) childHeldTick();
		if (st == ST_CHILD || st == ST_MP_CHILD) parentSilenceTick();
		if (st == ST_MP_CHILD && W.waitingAck && W.frames - W.waitingAckFrame >= 2) childAckTimeout();
		for (int i = 0; i < MAX_PARENTS; i++)
			if (s_parents[i].used && W.frames - s_parents[i].heardAt > PARENT_KEEP_FRAMES) s_parents[i].used = false;
		if (st == ST_MP_PARENT) childSilenceTick();
		if (st >= ST_CLASS1 && st != ST_SCAN && W.frames % (g_wmhleVerbose ? 120 : 600) == 0) {
			logNum("wmhle: state, rx join/joinok/cmd/ack", st, (u32)W.rxCount[2] << 24 | (u32)W.rxCount[3] << 16 | (u32)(W.rxCount[4] & 0xFF) << 8 | (W.rxCount[6] & 0xFF));
			logNum("wmhle: tx join/joinok/cmd/reply/ack", (u32)W.txCount[2] << 24 | (u32)W.txCount[3] << 16 | (u32)W.txCount[4], (u32)W.txCount[5] << 16 | W.txCount[6]);
			if (st == ST_MP_PARENT) logNum("wmhle: replies lost (queue full)", W.staleReplies, 0);
			logNum("wmhle: mpdata/portrecv, sendok/sendfail", W.nMpData << 16 | (W.nPortRecv & 0xFFFF), W.nPortSendOk << 16 | (W.nPortSendFail & 0xFFFF));
			logNum("wmhle: dups dropped, reply copies", W.nDupDropped, W.nReplyCopies);
			logNum("wmhle: callbacks queued / hand-over busy, CMDs held back", (W.cbIn - W.cbOut) << 16 | (rd16(CALLBACK_CONTROL) & 1), W.nCmdsHeldBack);
			logNum("wmhle: CMDs let go (receive buffer still being read)", W.nBufClobbered, 0);
			if ((st == ST_MP_PARENT && (sget16(S_child_bitmap) & ~1)) || st == ST_MP_CHILD) {   // (a parent with no one linked has no rounds)
				if (W.lastProgress && W.frames - W.lastProgress > W.maxGap) W.maxGap = W.frames - W.lastProgress;
				logNum("wmhle: longest gap between MP rounds (frames)", W.maxGap, 0);
			} else W.lastProgress = 0;
			W.maxGap = 0;
		}
	}
	// requests even before INITIALIZE (it's one of them)
	wmhlePump();
}

// a parent's beacon: kept (with the others in range) for scans
static void onBeacon(const BridgeMsg* m)
{
	if (m->bodyLen > sizeof(s_parents[0].beacon)) return;
	RemoteParent* p = parentFind(m->mac);
	if (!p) {
		// a new one: a free slot, else the one heard longest ago
		p = &s_parents[0];
		for (int i = 0; i < MAX_PARENTS; i++) {
			if (!s_parents[i].used) { p = &s_parents[i]; break; }
			if ((s32)(s_parents[i].heardAt - p->heardAt) < 0) p = &s_parents[i];
		}
		memset(p, 0, sizeof(*p));
		p->used = true;
		memcpy(p->mac, m->mac, 6);
		p->reportedAt = W.frames - 0x10000;     // (never reported: the next scan's first pick)
		logNum("wmhle: hearing a parent (MAC end), channel", (u32)m->mac[4] << 8 | m->mac[5], m->channel);
	} else if (p->channel != m->channel) logNum("wmhle: a parent changed channel (MAC end), channel", (u32)m->mac[4] << 8 | m->mac[5], m->channel);
	memcpy(p->beacon, m->body, m->bodyLen);
	p->len = (u16)m->bodyLen;
	p->channel = m->channel;
	p->heardAt = W.frames;
}

// (parent) the aid of the child with this MAC, 0 none
static u16 childAid(const u8* mac)
{
	for (u16 a = 1; a <= MAX_CHILD; a++)
		if ((sget16(S_child_bitmap) & (1 << a)) && memcmp(M(W.status + S_childMacAddress + 6 * (a - 1)), mac, 6) == 0) return a;
	return 0;
}

void wmhleBridgeRx(const uint8_t* msg, unsigned len)
{
	BridgeMsg m;
	if (!bridgeParse(msg, len, &m) || !W.initialized) return;
	if (bridgeNotForUs(msg)) { W.nNotForUs++; return; }      // (for another console)
	if (m.type < 9) W.rxCount[m.type]++;
	switch (m.type) {
		case DSB_HostInfo:
			// a parent's beacon: kept for scans, and our parent's a sign of life
			onBeacon(&m);
			if ((state() == ST_CHILD || state() == ST_MP_CHILD) && memcmp(m.mac, W.remoteMac, 6) == 0) W.lastFromParent = W.frames;
			break;
		case DSB_Join:   onJoin(&m); break;
		case DSB_JoinOk: onJoinOk(&m); break;
		case DSB_Cmd:    onCmd(&m); break;
		case DSB_Reply:
			// every reply is kept, in order (see s_rq): a late one answers the next round. Only
			// from the child that has that aid (another parent's child has its own aid 1)
			if (state() != ST_MP_PARENT && state() != ST_PARENT) break;
			if (m.aid >= 1 && m.aid <= MAX_CHILD && childAid(m.mac) == m.aid && m.bodyLen <= sizeof(s_replyBody[0])) {
				W.childHeardAt[m.aid] = W.frames;
				W.childPolls[m.aid] = 0;
				rqPush(m.aid, m.seq, m.refSeq, m.body, m.bodyLen);
				if (!s_round.pending) break;
				rqTake();
				// all in: the round closes now (its ack goes out at once, not next frame)
				if (state() == ST_MP_PARENT && roundRepliesIn() && cbRoom() && parentBufFree()) parentMpFinish();
			}
			break;
		case DSB_Ack:    onAck(&m); break;
		case DSB_Leave:
			if (state() == ST_PARENT || state() == ST_MP_PARENT) onChildLeave(&m);
			// (WideMelon puts the deauth's target after the reason: one for an earlier
			// attempt of ours, or for someone else, isn't this connection ending)
			else if ((state() == ST_CHILD || state() == ST_MP_CHILD) && memcmp(m.mac, W.remoteMac, 6) == 0
			         && (m.bodyLen < 8 || memcmp(m.body + 2, W.h->mac, 6) == 0 || (m.body[2] & 1))) {
				// (a held CMD, its last data, reaches the game first: see s_heldCmd)
				if (s_heldCmd.valid) { if (!s_lostPending) s_lostAt = W.frames; s_lostPending = true; }
				else childLost(SC_DISCONNECTED);
			}
			break;
		default: break;
	}
}
