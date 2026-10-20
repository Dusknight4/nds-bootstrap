/* nbwifi blob: one binary at a fixed place in main RAM (blob.h) */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(g_nbwBlob)

MEMORY {
	blob : ORIGIN = 0x02F40000, LENGTH = 0x20000
}

SECTIONS
{
	.header : { KEEP(*(.header)) } >blob
	.text : {
		*(.text*)
		*(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
		. = ALIGN(4);
	} >blob
	.rodata : { *(.rodata*) . = ALIGN(4); } >blob
	.data : { *(.data*) . = ALIGN(4); } >blob
	.bss (NOLOAD) : {
		__bss_start = .;
		*(.bss*) *(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} >blob
	__blob_end = .;
	/DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
