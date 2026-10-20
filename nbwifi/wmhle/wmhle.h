// wmhle: the ARM7 half of the DS SDK's wireless library (WM), reimplemented so
// a DS game's local wireless can run over another link: the game's ARM9 WM
// library stays as it is and sends its requests (PXI FIFO tag 10) here instead
// of to the ARM7 wireless component, which is never used. What would go on the
// air goes to WideMelon as DsBridge messages ("DSB1") instead.
//
// Portable C: the same code runs in the PC test harness (on emulated memory)
// and in nds-bootstrap's WiFi blob on the DS (on real memory). The host gives
// it memory, the FIFO back to the ARM9, the bridge and a frame tick.
#ifndef WMHLE_H
#define WMHLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WmHleHost {
	// main RAM as the DS sees it (0x02000000...): a pointer to that byte
	uint8_t* (*ram)(uint32_t addr);
	// a word to the ARM9 through the IPC FIFO, as the ARM7 would send it
	// (tag in bits 0-4, data in bits 6-31)
	void (*sendToArm9)(uint32_t word);
	// a DsBridge message to the other end (already serialized)
	void (*bridgeSend)(const uint8_t* msg, unsigned len);
	// handle what has come in from the other end (calls wmhleBridgeRx for each
	// message); used while a parent waits for its children's replies
	void (*pollNet)(void);
	// after a callback went to the ARM9: wait (briefly) until the ARM9 has taken
	// it, so the next can follow. Returns false if it can't wait (the PC harness,
	// where the ARM9 only runs between calls): the rest goes on the next pump
	bool (*waitArm9)(void);
	// a clock in microseconds (for the parent's reply wait; may wrap)
	uint32_t (*nowUs)(void);
	// this console's MAC address
	const uint8_t* mac;
	// log a line (may be NULL)
	void (*log)(const char* text);
	// the IPC FIFO to the ARM9 is full right now (may be NULL): no callback is handed
	// over then (marking it busy and then losing its word stalled every callback after)
	bool (*arm9FifoFull)(void);
} WmHleHost;

void wmhleInit(const WmHleHost* host);

// testing: log every port packet received (set by the host)
extern int g_wmhleTracePorts;
// testing: log every request and callback (the DS logs only notable ones: each line costs ARM7 time)
extern int g_wmhleVerbose;
// testing: chance per frame (in 1000) of a 10-40 frame stretch where the ARM9 looks too busy
extern int g_wmhleBusyTest;

// a tag-10 word from the ARM9 (its data: a pointer to the request). On the DS
// this comes in the FIFO interrupt, so it only queues; wmhlePump does the work
void wmhleRequest(uint32_t data);

// as often as the host can (at least once a frame): runs queued requests and
// hands queued callbacks to the ARM9
void wmhlePump(void);

// once a frame (VBlank): timers, beacons, the parent's MP round
void wmhleFrame(void);
// hands queued callbacks to the ARM9 (as many as it takes); call when it may have taken one
void wmhleDeliver(void);

// a DsBridge message from the other end
void wmhleBridgeRx(const uint8_t* msg, unsigned len);

// whether the game has the wireless on (for the host: nothing to do otherwise)
bool wmhleActive(void);

#ifdef __cplusplus
}
#endif

#endif
