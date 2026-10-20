// nbwifi blob: the radio only while the game uses its wireless (blob_radio.c). Shared by
// the blob and its PC test (nbwtest, not included here), which runs the real driver against a
// simulated WiFi device.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nbwifi.h"

#define RADIO_IDLE_FRAMES   (20 * 60)  // the game's wireless off this long: leave the network
#define RADIO_RETRY_FRAMES  (5 * 60)   // wanted but joining failed: try again after this, then
#define RADIO_RETRY_MAX_FRAMES (30 * 60) //   twice that, and so on up to this

// once a frame, in game: frames (counted by the caller), whether the game has its wireless
// on (wmhle between WM Initialize and End), whether the lid is closed, the saved networks
void radioTick(uint32_t frames, bool gameWireless, bool lidClosed, const NbwNetwork* nets, unsigned count);

// right after the game's wireless requests are handled: the lid closed and its wireless just
// ended, leave the network now (it may sleep before radioTick's next turn)
void radioLidCheck(bool gameWireless, bool lidClosed);

// worth polling the device (joining, joined or leaving); resting or failed: don't
bool blobRadioActive(void);

// (tests) back to how it starts
void radioReset(void);
