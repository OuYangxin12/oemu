/*
 * Guest link script: the smallest layout that is still honest.
 *
 * Text is placed at the fixed, conventional address 0x400000 so the image
 * oemu's future loader will map is predictable, and the output keeps to two
 * PT_LOAD segments (read-only text+rodata, read-write data+bss) -- segment
 * count is exactly what a loader has to handle, so guest binaries stay at
 * the minimum. Grammar is the common subset GNU ld, gold, mold and lld all
 * accept.
 */
ENTRY(_start)

SECTIONS
{
  . = 0x400000;

  .text : { *(.text*) }

  . = ALIGN(0x1000);
  .rodata : { *(.rodata*) }

  . = ALIGN(0x1000);
  .data : { *(.data*) }

  .bss (NOLOAD) : { *(.bss*) *(COMMON) }

  /DISCARD/ : { *(.comment) *(.note .note.*) }
}
