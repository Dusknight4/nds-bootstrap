// nbwifi blob: finding WideMelon on the network, and the bridge's hello and log messages
// (blob_bridge.c). Shared by the blob and its PC test (nbwtest, not included here).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BRIDGE_PORT 7071
#define BRIDGE_QUIET_FRAMES (15 * 60)  // online, and no hello from WideMelon this long: look for it again

// once a frame while online (frames: the caller's frame count)
void bridgeTick(uint32_t frames);

// a UDP packet that came to the bridge port
void bridgeRx(uint32_t src_ip, const uint8_t* data, unsigned len);

// where WideMelon is (0: not known, saying hello by broadcast)
uint32_t bridgeWideMelonIp(void);

// (tests) back to how it starts
void bridgeReset(void);
