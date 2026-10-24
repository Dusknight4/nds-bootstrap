// wmpeers: the other consoles on the network playing local wireless over WiFi (DSes running
// the modified nds-bootstrap, and MultiMelons), for the link under wmhle. Shared by the DS's
// blob and MultiMelon's DS link, so both find each other the same way.
//
// As on the air, where every console in range hears every other, everyone on the network is
// a peer: no pairing, the game itself picks whom it talks to (wmhle's messages name the
// console they're for, see DSB_DEST_AT in wmhle.c). Each console says hello (a DSB1 Hello)
// once a second while the game has its wireless on: by broadcast, which finds the ones not
// yet known, and to each one it knows, so a lost broadcast doesn't lose a peer. A peer
// heard nothing from for WMPEERS_GONE_MS of our own time on the network is forgotten (one
// whose wireless is off says nothing, and comes back with its next hello).
//
// A message wmhle addressed to one console goes to that peer alone; the rest (beacons, and
// a message for a console we don't know yet) to everyone, who drop what isn't theirs.
//
// Portable C, no globals (the PC test harness runs several consoles in one program).
#ifndef WMPEERS_H
#define WMPEERS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WMPEERS_MAX     8
#define WMPEERS_GONE_MS 10000
#define WMPEERS_HELLO_LEN (40 + 8)

typedef struct {
	bool used;
	bool macKnown;
	uint32_t ip;            // (network order, as it comes)
	uint16_t port;
	uint8_t mac[6];
	uint8_t kind;           // 'D' a real DS (its hello says "DSHW"), 'M' a MultiMelon ("WMPC"), 0 not said
	uint32_t heardMs;       // on our clock (see wmpeersRx)
	uint32_t msgsIn, msgsOut;
} WmPeer;

typedef struct {
	WmPeer p[WMPEERS_MAX];
	uint8_t mac[6];         // ours
	uint8_t kind;           // what we say we are ('D', 'M')
	uint8_t tag[4];         // this session's, in our hellos: our own broadcast heard back is skipped
	// (may be NULL) a line for the log: a peer found, moved or forgotten
	void (*log)(void* ctx, const char* text);
	void* logCtx;
} WmPeers;

void wmpeersInit(WmPeers* w, const uint8_t mac[6], uint8_t kind, const uint8_t tag[4]);

// A DSB1 message from ip:port. Returns the peer's index (a new one is added; one whose MAC we
// know from another address has moved there), or -1: not a DSB1 message, or our own hello.
// nowMs: a clock that runs only while we're on the network (the DS's radio rests otherwise),
// so a peer's silence counts only while we could have heard it.
int wmpeersRx(WmPeers* w, uint32_t ip, uint16_t port, const uint8_t* msg, unsigned len, uint32_t nowMs);

// whether a message is a Hello / a Log (the link's own: not for wmhle)
bool wmpeersIsHello(const uint8_t* msg, unsigned len);
bool wmpeersIsLog(const uint8_t* msg, unsigned len);

// our hello (WMPEERS_HELLO_LEN bytes)
unsigned wmpeersHello(const WmPeers* w, uint8_t* out);

// where a message of wmhle's goes: the indices of the peers to send it to. Its destination's,
// if it names one we know; else everyone's
int wmpeersRoute(WmPeers* w, const uint8_t* msg, unsigned len, int* out, int max);

// peers silent for WMPEERS_GONE_MS (on the same clock as wmpeersRx) are forgotten
void wmpeersExpire(WmPeers* w, uint32_t nowMs);

int wmpeersCount(const WmPeers* w);

// the peer at ip:port, -1 none (nothing added)
int wmpeersFind(const WmPeers* w, uint32_t ip, uint16_t port);

// "10.0.0.65:7071 (a DS)" and the like, for the log (out: 40 bytes)
void wmpeersDescribe(const WmPeer* p, char* out);

#ifdef __cplusplus
}
#endif

#endif
