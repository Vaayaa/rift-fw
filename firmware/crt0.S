/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* App entry. The SPL maps app.bin at XIP 0x02000120 and jumps to its first
 * byte with r0 = boot-parameter block (0x01C7FE08, inside the vector table).
 * The SPL expects `goto +4; rts` as the first instruction. The core state
 * the SPL left is saved first; then interrupts off and our stacks. */
	.section .text.crt0,"ax",@progbits
	.globl _start
_start:
	goto 1f
	rts
1:	r1 = icfg
	r2 = psr
	cli
	r3 = 0x1C07F00		/* STATUS (debug area) */
	[r3 + 0x20] = r1	/* icfg as handed over */
	[r3 + 0x24] = r2	/* psr */
	[r3 + 0x28] = r0	/* boot-parameter pointer */
	r1 = sp
	[r3 + 0x2C] = r1
	r1 = ssp
	[r3 + 0x30] = r1
	sp = _ustack_top
	ssp = _sstack_top
	call fm1_cstart
	goto .

/* The second core's entry (hal/fm1_multicore.h puts its address in the word the chip starts core 1 from):
 * interrupts off and none configured, its own stacks (app.ld: _c1_*), then out of the start-up state into
 * fm1_c1_main (RAM), which never returns. As the AC79 SDK's startup, Melodee, fm1-x0x and JIANT 0.6.71 have it */
	.globl fm1_c1_entry
fm1_c1_entry:
	cli
	icfg = 0
	usp = _c1_ustack_top
	sp = _c1_sstack_top
	ssp = _c1_sstack_top
	reti = fm1_c1_main
	rti
