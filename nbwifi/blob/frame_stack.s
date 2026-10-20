@ nbwifi blob: frame() runs from the game's VBlank interrupt, on the game's
@ (small) ARM7 interrupt stack. The driver can go about a kilobyte deep, so
@ frame() runs it on a stack of its own (s_frameStack, in blob.c).
	.arm
	.section .text.blobFrame,"ax",%progbits
	.global blobFrame
	.type blobFrame, %function
blobFrame:
	push	{r4, lr}
	mov	r4, sp
	ldr	sp, =s_frameStack + 0x1000
	ldr	r0, =blobFrameImpl
	mov	lr, pc
	bx	r0
	mov	sp, r4
	pop	{r4, lr}
	bx	lr
	.pool

@ blobVCount: our mid-frame (VCount) IRQ handler, called by the game's IRQ
@ dispatcher; blobVCountImpl on a second stack of its own (s_vcountStack in
@ blob_wm.c), in case an IRQ ever nests in the VBlank one
	.section .text.blobVCount,"ax",%progbits
	.global blobVCount
	.type blobVCount, %function
blobVCount:
	push	{r4, lr}
	mov	r4, sp
	ldr	sp, =s_vcountStack + 0x1000
	ldr	r0, =blobVCountImpl
	mov	lr, pc
	bx	r0
	mov	sp, r4
	pop	{r4, lr}
	bx	lr
	.pool
