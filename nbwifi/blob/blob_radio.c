// nbwifi blob: the radio only while the game uses its wireless.
//
// Joined to the network only while the game has its local wireless on (between its WM
// Initialize and End: the Union Room, a Pokémon Center's upstairs...) and for a while after
// (it turns it off and on again between steps); otherwise the device leaves the network and
// rests (nbwifiDisconnect) and isn't polled at all: no radio traffic, no work every 2 ms.
// Closing the lid with the game's wireless off leaves the network at once (the game then
// sleeps, and nothing would run to do it later). Joining again takes about 2-3 seconds.
// The loader still joins once before the game (it proves the network and key work, and
// without a network the game gets its own local wireless instead).
#include "blob_radio.h"

void nbwLogf(const char* fmt, ...);   // nbwifi: adds a line to its log

static uint32_t s_wirelessOnAt;     // the last frame the game's wireless was on
static uint32_t s_lastJoinAt;       // the last frame we started joining
static bool s_joinedOnce;           // we started a join in game (s_lastJoinAt is meaningful)
static bool s_radioUsed;            // the game turned its wireless on since boot
static bool s_wirelessWas;          // the game's wireless on at the last tick
static unsigned s_failStreak;       // tries that failed in a row while wanted: retries spaced out

void radioReset(void)
{
	s_wirelessOnAt = s_lastJoinAt = 0;
	s_joinedOnce = s_radioUsed = s_wirelessWas = false;
	s_failStreak = 0;
}

// wanted, but the last try failed (the network out of range, say): the next one this long after
// it started: 5 s, 10 s, 20 s, then every 30 s (a try takes 1.5-5 s; a scan is the radio's most
// costly work, so not back to back for as long as the game's wireless stays on)
static uint32_t retryFrames(void)
{
	if (nbwifiHeardSaved()) return RADIO_RETRY_FRAMES;    // (the last try heard it: bad luck, not out of range)
	return s_failStreak >= 3 ? RADIO_RETRY_MAX_FRAMES : (uint32_t)RADIO_RETRY_FRAMES << s_failStreak;
}

bool blobRadioActive(void)
{
	const NbwState st = nbwifiState();
	return st == NbwState_Scanning || st == NbwState_Connecting || st == NbwState_GettingIp
	    || st == NbwState_Online || st == NbwState_Disconnecting;
}

void radioTick(uint32_t frames, bool gameWireless, bool lidClosed, const NbwNetwork* nets, unsigned count)
{
	const NbwState st = nbwifiState();
	if (st == NbwState_Off) return;
	if (gameWireless) {
		s_wirelessOnAt = frames;
		if (!s_radioUsed) { s_radioUsed = true; nbwLogf("[NBW] the game's wireless is on\n"); }
		if (!s_wirelessWas || st == NbwState_Online) s_failStreak = 0;   // (just turned on: a try at once)
		s_wirelessWas = true;
		// not on the network (rested, or joining failed a while ago): join
		if (st == NbwState_Ready || (st == NbwState_Failed && (!s_joinedOnce || frames - s_lastJoinAt >= retryFrames()))) {
			// (spaced out only while none of the networks is heard: in range, a failed join was bad
			// luck, so the next try is 5 s on)
			if (st == NbwState_Failed && s_joinedOnce) s_failStreak = nbwifiHeardSaved() ? 0 : s_failStreak + 1;
			nbwLogf("[NBW] the game wants its wireless: joining\n");
			s_lastJoinAt = frames;
			s_joinedOnce = true;
			if (!nbwifiConnectAny(nets, count)) nbwLogf("[NBW] couldn't start joining\n");
		}
		return;
	}
	s_wirelessWas = false;
	// the game's wireless off: leave the network after a while, or now if the lid is closed
	if (st == NbwState_Ready || st == NbwState_Disconnecting) return;
	if (lidClosed || frames - s_wirelessOnAt >= RADIO_IDLE_FRAMES) {
		nbwLogf(lidClosed ? "[NBW] lid closed, the game's wireless off: radio resting\n"
		                  : "[NBW] the game's wireless off for 20 s: radio resting\n");
		nbwifiDisconnect();
	}
}

void radioLidCheck(bool gameWireless, bool lidClosed)
{
	// (the game may sleep as soon as it hears its wireless has ended: radioTick, once a frame,
	// could come too late; this runs right after the game's requests are handled, before
	// the ARM9 can have its answer)
	if (gameWireless || !lidClosed) return;
	const NbwState st = nbwifiState();
	if (st == NbwState_Off || st == NbwState_Ready || st == NbwState_Disconnecting) return;
	nbwLogf("[NBW] lid closed, the game's wireless ended: radio resting\n");
	nbwifiDisconnect();
}
