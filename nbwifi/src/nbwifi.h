// nbwifi: the DSi's own WiFi (Atheros, WPA/WPA2) for ARM7 code that can't use
// threads or interrupts, such as nds-bootstrap's resident code inside a game.
// Everything happens in nbwifiPoll(), which must be called often (every frame
// or more). Built on calico's driver (ZPL-2.1, Copyright fincs, devkitPro).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NbwState {
	NbwState_Off = 0,       // not started, or the device didn't come up
	NbwState_Ready,         // device up, not connected
	NbwState_Scanning,      // looking for the network
	NbwState_Connecting,    // associating (and the WPA handshake)
	NbwState_GettingIp,     // connected, asking DHCP for an address
	NbwState_Online,        // ready to send and receive
	NbwState_Failed,        // couldn't connect (see the log)
	NbwState_Disconnecting, // leaving the network (nbwifiDisconnect); then Ready
} NbwState;

// memory nbwifi needs for its packet buffers (about 20 KB; any RAM the ARM7 can reach)
unsigned nbwNetbufMemSize(void);

// brings the WiFi device up (selecting the Atheros chip, which turns off the
// DS-compatible one); mem: nbwNetbufMemSize() bytes. Blocks for up to a few seconds.
bool nbwifiInit(void* mem);

// starts connecting to the network named ssid. pmk: the 32-byte WPA key
// (nbwifiDerivePmk), or NULL for an open network. Progress: nbwifiState().
bool nbwifiConnect(const char* ssid, unsigned ssid_len, const void* pmk);

// a network it may join, as saved in the console's settings
#define NBW_MAX_NETWORKS 6
typedef struct {
	char ssid[32];
	uint8_t ssidLen;
	bool hasPmk;            // false: an open network
	uint8_t pmk[32];        // the WPA key (the DSi/3DS settings keep it worked out)
} NbwNetwork;

// starts connecting to whichever of these networks is in range (the strongest;
// if joining it fails, the next one). Progress: nbwifiState().
bool nbwifiConnectAny(const NbwNetwork* list, unsigned count);

// leaves the network (or stops trying to join it) and puts the device in its
// power-saving mode, as calico's twlwifiDisassociate does: the radio goes quiet.
// The device stays up (its firmware is never reloaded): nbwifiConnectAny joins
// again later. State: Disconnecting (keep polling) until the network confirms,
// at most a second, then Ready (no polling needed)
void nbwifiDisconnect(void);

// since the last nbwifiConnectAny: a scan heard one of the saved networks (a join that then
// failed had bad luck, and is worth trying again soon; none heard: out of range)
bool nbwifiHeardSaved(void);

// for a caller that polls once a frame at the same point (a VBlank handler):
// call this each frame, before nbwifiPoll, as the driver's timeouts count frames
void nbwifiFrameTick(void);

// works out the WPA key from the network name and password (PBKDF2, slow:
// a few seconds on the ARM7, so keep the result)
void nbwifiDerivePmk(void* pmk, const char* ssid, unsigned ssid_len, const char* password, unsigned password_len);

// does all pending work: received packets, connection steps, DHCP
void nbwifiPoll(void);
// most rounds of the device's receive queue one nbwifiPoll handles (default 16)
extern int g_nbwPollRounds;

NbwState nbwifiState(void);
const uint8_t* nbwifiMac(void);
uint32_t nbwifiIp(void);        // network byte order, as in a sockaddr
uint32_t nbwifiNetmask(void);
uint32_t nbwifiGateway(void);

// UDP. Addresses in network byte order; ip may be the broadcast address.
// The handler is called from within nbwifiPoll().
typedef void (*NbwUdpHandler)(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const void* data, unsigned len);
void nbwifiSetUdpHandler(NbwUdpHandler fn);
bool nbwifiUdpSend(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, const void* data, unsigned len);

// copies log text written since *cursor into out (NUL-terminated) and advances
// *cursor; returns the number of characters copied
unsigned nbwifiLog(char* out, unsigned size, unsigned* cursor);

// formats into out like snprintf (a small subset: %d %u %x %X %c %s, 0, width,
// .precision), without pulling in the C library's printf
unsigned nbwSnprintf(char* out, unsigned size, const char* fmt, ...);

// time spent (in scanlines, for the blob's log): SDIO transactions and UDP sends
typedef struct { unsigned sdioN, sdioSum, sdioMax, sendN, sendSum, sendMax; } NbwProf;
extern NbwProf g_nbwProf;
static inline unsigned nbwLinesSince(unsigned short v0) { const unsigned short v1 = *(volatile unsigned short*)0x04000006; return (unsigned)((v1 + 263 - v0) % 263); }

#ifdef __cplusplus
}
#endif
