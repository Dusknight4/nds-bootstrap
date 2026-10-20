// nbwifi: stand-in for calico's arm/common.h: the few CPU helpers its WiFi
// driver uses. nbwifi never runs from more than one place at a time, so
// locking only has to keep interrupts out.
// SPDX-License-Identifier: ZPL-2.1
#pragma once
#include "../types.h"

MK_EXTERN_C_START

typedef u32 ArmIrqState;

MK_EXTERN32 ArmIrqState armIrqLockByPsr(void);
MK_EXTERN32 void armIrqUnlockByPsr(ArmIrqState st);

MK_INLINE u32 armSwapWord(u32 value, u32* addr)
{
	ArmIrqState st = armIrqLockByPsr();
	u32 old = *addr;
	*addr = value;
	armIrqUnlockByPsr(st);
	return old;
}

MK_EXTERN_C_END
