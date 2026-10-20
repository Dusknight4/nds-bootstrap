@ nbwifi: the few ARM-mode helpers (BIOS delay, interrupt lock)
	.arm
	.section .text.nbwWaitLoop, "ax", %progbits
	.global nbwWaitLoop
	.type nbwWaitLoop, %function
nbwWaitLoop:			@ r0 = count (BIOS WaitByLoop: 4 ARM7 cycles each)
	swi	0x030000
	bx	lr

	.section .text.armIrqLockByPsr, "ax", %progbits
	.global armIrqLockByPsr
	.type armIrqLockByPsr, %function
armIrqLockByPsr:		@ returns the old CPSR, with IRQs now off
	mrs	r0, cpsr
	orr	r1, r0, #0x80
	msr	cpsr_c, r1
	bx	lr

	.section .text.armIrqUnlockByPsr, "ax", %progbits
	.global armIrqUnlockByPsr
	.type armIrqUnlockByPsr, %function
armIrqUnlockByPsr:		@ r0 = what armIrqLockByPsr returned
	msr	cpsr_c, r0
	bx	lr
