// nbwifi internals: what the calico-derived sources need in place of calico's
// threads and system services. Everything here runs on the ARM7, from one
// place at a time, by polling.
#pragma once
#include <calico/types.h>
#include <calico/dev/netbuf.h>

// waits about this many microseconds (BIOS WaitByLoop, so it doesn't depend
// on timers the game might be using)
void nbwSleepUs(u32 us);

// calico's threadSleep(us)
#define threadSleep(_us) nbwSleepUs(_us)

// adds a line to the log (see nbwifiLog)
void nbwLogf(const char* fmt, ...);

// set while nbwifi is handling received packets: sending then must not wait
// for the device (see _ar6kHtcCheckCredits)
extern bool g_nbwInPoll;
extern unsigned g_nbwRxBatches;

// static packet buffer pools (nbw_netbuf.c)
void nbwNetbufInit(void* mem);
