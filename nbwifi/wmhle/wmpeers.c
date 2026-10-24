// wmpeers: the other consoles on the network (see wmpeers.h)
#include <string.h>
#include "wmpeers.h"

// (the DSB1 header, as DsBridge::Msg serializes it: see wmhle.c)
#define HEAD 40
#define TYPE_HELLO 8
#define TYPE_LOG 9

static const char* kindWord(uint8_t kind) { return kind == 'D' ? "DSHW" : "WMPC"; }

// (a tiny formatter: no printf on the DS)
static char* putStr(char* o, const char* s) { while (*s) *o++ = *s++; return o; }
static char* putNum(char* o, unsigned v)
{
	char t[10];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
	while (n) *o++ = t[--n];
	return o;
}

void wmpeersDescribe(const WmPeer* p, char* out)
{
	const uint8_t* ip = (const uint8_t*)&p->ip;
	char* o = out;
	for (int i = 0; i < 4; i++) { o = putNum(o, ip[i]); *o++ = i < 3 ? '.' : ':'; }
	o = putNum(o, p->port);
	o = putStr(o, p->kind == 'D' ? " (a DS)" : p->kind == 'M' ? " (a MultiMelon)" : "");
	*o = 0;
}

static void logPeer(WmPeers* w, const char* what, const WmPeer* p)
{
	if (!w->log) return;
	char t[96], d[40];
	wmpeersDescribe(p, d);
	char* o = putStr(t, what);
	o = putStr(o, d);
	*o = 0;
	w->log(w->logCtx, t);
}

void wmpeersInit(WmPeers* w, const uint8_t mac[6], uint8_t kind, const uint8_t tag[4])
{
	void (*log)(void*, const char*) = w->log;
	void* ctx = w->logCtx;
	memset(w, 0, sizeof(*w));
	w->log = log;
	w->logCtx = ctx;
	memcpy(w->mac, mac, 6);
	w->kind = kind;
	memcpy(w->tag, tag, 4);
}

bool wmpeersIsHello(const uint8_t* msg, unsigned len) { return len >= HEAD && msg[4] == TYPE_HELLO; }
bool wmpeersIsLog(const uint8_t* msg, unsigned len) { return len >= HEAD && msg[4] == TYPE_LOG; }

static int findAddr(const WmPeers* w, uint32_t ip, uint16_t port)
{
	for (int i = 0; i < WMPEERS_MAX; i++) if (w->p[i].used && w->p[i].ip == ip && w->p[i].port == port) return i;
	return -1;
}

static int findMac(const WmPeers* w, const uint8_t* mac)
{
	for (int i = 0; i < WMPEERS_MAX; i++) if (w->p[i].used && w->p[i].macKnown && memcmp(w->p[i].mac, mac, 6) == 0) return i;
	return -1;
}

int wmpeersRx(WmPeers* w, uint32_t ip, uint16_t port, const uint8_t* msg, unsigned len, uint32_t nowMs)
{
	if (len < HEAD || memcmp(msg, "DSB1", 4) != 0) return -1;
	const unsigned bodyLen = msg[38] | (unsigned)msg[39] << 8;
	if (HEAD + bodyLen > len) return -1;
	const uint8_t* mac = msg + 5;
	const uint8_t* body = msg + HEAD;
	const bool hello = msg[4] == TYPE_HELLO;
	// our own (a broadcast comes back to the console that sent it)
	if (memcmp(mac, w->mac, 6) == 0) return -1;
	if (hello && bodyLen >= 8 && memcmp(body + 4, w->tag, 4) == 0) return -1;
	const bool macOk = !(mac[0] & 1) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
	int i = findAddr(w, ip, port);
	if (i < 0 && macOk && (i = findMac(w, mac)) >= 0) {
		// one we know, from another address: it moved (a DS back with another IP)
		w->p[i].ip = ip;
		w->p[i].port = port;
		logPeer(w, "link: moved to ", &w->p[i]);
	}
	if (i < 0) {
		// someone new: a free slot, else the one heard from longest ago
		i = 0;
		for (int k = 0; k < WMPEERS_MAX; k++) {
			if (!w->p[k].used) { i = k; break; }
			if ((int32_t)(w->p[k].heardMs - w->p[i].heardMs) < 0) i = k;
		}
		if (w->p[i].used) logPeer(w, "link: no room, forgetting ", &w->p[i]);
		memset(&w->p[i], 0, sizeof(w->p[i]));
		w->p[i].used = true;
		w->p[i].ip = ip;
		w->p[i].port = port;
		if (hello && bodyLen >= 4) w->p[i].kind = memcmp(body, "DSHW", 4) == 0 ? 'D' : memcmp(body, "WMPC", 4) == 0 ? 'M' : 0;
		logPeer(w, "link: found ", &w->p[i]);
	}
	WmPeer* p = &w->p[i];
	p->heardMs = nowMs;
	p->msgsIn++;
	// (a MultiMelon started again has another MAC at the same address)
	if (macOk && (!p->macKnown || memcmp(p->mac, mac, 6) != 0)) { memcpy(p->mac, mac, 6); p->macKnown = true; }
	if (hello && bodyLen >= 4 && !p->kind) {
		p->kind = memcmp(body, "DSHW", 4) == 0 ? 'D' : memcmp(body, "WMPC", 4) == 0 ? 'M' : 0;
		if (p->kind) logPeer(w, "link: it says it is ", p);
	}
	return i;
}

unsigned wmpeersHello(const WmPeers* w, uint8_t* out)
{
	memset(out, 0, HEAD);
	memcpy(out, "DSB1", 4);
	out[4] = TYPE_HELLO;
	memcpy(out + 5, w->mac, 6);
	out[16] = out[17] = 0xFF;      // RefSeq: none
	out[38] = 8;
	memcpy(out + HEAD, kindWord(w->kind), 4);
	memcpy(out + HEAD + 4, w->tag, 4);
	return WMPEERS_HELLO_LEN;
}

int wmpeersRoute(WmPeers* w, const uint8_t* msg, unsigned len, int* out, int max)
{
	// (wmhle.c: the destination's MAC at 30, then 0xD5 0x4D; a broadcast MAC is everyone)
	if (len >= HEAD && msg[36] == 0xD5 && msg[37] == 0x4D && !(msg[30] & 1)) {
		const int i = findMac(w, msg + 30);
		if (i >= 0 && max > 0) { out[0] = i; w->p[i].msgsOut++; return 1; }
	}
	int n = 0;
	for (int i = 0; i < WMPEERS_MAX && n < max; i++)
		if (w->p[i].used) { out[n++] = i; w->p[i].msgsOut++; }
	return n;
}

void wmpeersExpire(WmPeers* w, uint32_t nowMs)
{
	for (int i = 0; i < WMPEERS_MAX; i++) {
		if (!w->p[i].used || nowMs - w->p[i].heardMs <= WMPEERS_GONE_MS) continue;
		logPeer(w, "link: nothing for 10 s, forgetting ", &w->p[i]);
		w->p[i].used = false;
	}
}

int wmpeersFind(const WmPeers* w, uint32_t ip, uint16_t port) { return findAddr(w, ip, port); }

int wmpeersCount(const WmPeers* w)
{
	int n = 0;
	for (int i = 0; i < WMPEERS_MAX; i++) n += w->p[i].used;
	return n;
}
