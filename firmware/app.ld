/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* RIFT memory map (Felucca's, with JIANT 0.6.71 / 0.6.78's RAM code layout).
 * Code + constants execute in place from XIP (app.bin offset 0 = 0x02000120).
 * RAMTEXT at the top of the pool: .ram_text (code that must run while the flash is busy: the flash driver),
 * .c1_text (the second core's waiting loop), .hot_text (the audio render, tools/build.py HOT_FNS). A call reaches
 * +-4 MiB: from here it reaches the XIP image (build.py checks the margin).
 * LOWBUF 0x01C00000..0x01C07000: zeroed buffers; 0x01C07000..0x01C08000 stays for RAM-run debug.
 * RAM  : .data/.bss (96 KiB)
 * POOL : big zero-initialised buffers (FX lines, canvases), section .pool; the second core's stacks after it
 * STACKS with 256-byte guard bands; NOINIT survives resets and UBOOT entry.
 * 0x01C7FD50.. is reserved: boot_info, boot-state mailbox, vector table. */
MEMORY
{
    XIP    (rx)  : ORIGIN = 0x02000120, LENGTH = 0x8DFBC
    LOWBUF (rw)  : ORIGIN = 0x01C00000, LENGTH = 0x7000
    RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 96K
    POOL   (rw)  : ORIGIN = 0x01C20000, LENGTH = 0x4C000
    RAMTEXT (rwx): ORIGIN = 0x01C6C000, LENGTH = 0x8000
    NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D50
}
SECTIONS
{
    .text : { KEEP(*(.text.crt0)) *(.text*) *(.rodata*) . = ALIGN(4); _etext = .; } > XIP
    .ram_text : { _rt_start = .; KEEP(*(.ram_text*)) . = ALIGN(4); _rt_end = .; } > RAMTEXT AT > XIP
    _rt_load = LOADADDR(.ram_text);
    .c1_text : { _c1t_start = .; KEEP(*(.c1_text*)) . = ALIGN(4); _c1t_end = .; } > RAMTEXT AT > XIP
    _c1t_load = LOADADDR(.c1_text);
    .hot_text : { _hot_start = .; KEEP(*(.hot_text*)) . = ALIGN(4); _hot_end = .; } > RAMTEXT AT > XIP
    _hot_load = LOADADDR(.hot_text);
    .data : { . = ALIGN(4); _data_start = .; *(.data*) *(.sdata*) . = ALIGN(4); _data_end = .; } > RAM AT > XIP
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : { _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    .pool (NOLOAD) : { _pool_start = .; *(.pool*) . = ALIGN(4); _pool_end = .; } > POOL
    .c1_stacks (NOLOAD) : { . = ALIGN(32); _c1_stack_lo = .; . += 7168; _c1_stack_hi = .; } > POOL
    _c1_ustack_top = _c1_stack_lo + 6144;
    _c1_sstack_top = _c1_stack_hi;
    .lowbuf (NOLOAD) : { _low_start = .; *(.lowbuf*) . = ALIGN(4); _low_end = .; } > LOWBUF
    .noinit (NOLOAD) : { *(.noinit*) } > NOINIT
    _guard0 = 0x01C74000; _ustack_lo = 0x01C74100; _ustack_top = 0x01C7A000;
    _guard1 = 0x01C7A000; _sstack_lo = 0x01C7A100; _sstack_top = 0x01C7C000;
    /DISCARD/ : { *(.comment) *(.note*) }
}
