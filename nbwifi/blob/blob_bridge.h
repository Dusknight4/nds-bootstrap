// nbwifi blob: the link to the others on the network (DSes and MultiMelons playing local
// wireless over WiFi), and its hello and log messages (blob_bridge.c). Shared by the blob and
// its PC test (nbwtest, not included here).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BRIDGE_PORT 7071

// once a frame while online (frames: the caller's frame count)
void bridgeTick(uint32_t frames);

// a UDP packet that came to the bridge port
void bridgeRx(uint32_t src_ip, uint16_t src_port, const uint8_t* data, unsigned len);

// a message of wmhle's: to the console it names, or to everyone we know
void bridgeSend(const uint8_t* msg, unsigned len);

// a MultiMelon we know (0: none), and how many others in all (for the log and the test)
uint32_t bridgeWideMelonIp(void);
int bridgePeerCount(void);

// (tests) back to how it starts
void bridgeReset(void);
