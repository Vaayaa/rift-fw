/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FM-1 interrupt / exception HAL.
 *
 *   fm1_irq_init()        first thing in cstart: all ICFG off, pendings and
 *                         exception causes cleared, all 128 vectors -> fatal
 *                         stubs (fm1_vec.S), vector 1 (CPU
 *                         exception) enabled at prio 7, ETM on, div0 trap
 *                         off (see below).
 *   fm1_irq_attach(n, h, prio)   h = asm wrapper (fm1_isr.S), prio 0..7
 *   fm1_irq_enable_all()  icfg bit 8 + sti, after every source is set up
 *
 * The application supplies fm1_fault(const fm1_crash_t *) which reports the
 * crash (LCD) and resets; the record survives in .noinit for the next boot.
 */
#pragma once
#include <stdint.h>

#define FM1_VEC ((volatile uint32_t *)0x01C7FE00u)
#define FM1_ICFG(n) (*(volatile uint32_t *)(0x1EEF100u + 4u * ((n) >> 3)))
#define FM1_IPND(i) (*(volatile uint32_t *)(0x1EEF180u + 4u * (i)))
#define FM1_ILAT_SET (*(volatile uint32_t *)0x1EEF1A0u)
#define FM1_ILAT_CLR (*(volatile uint32_t *)0x1EEF1A4u)
#define FM1_EMU_CON (*(volatile uint32_t *)0x1EEF0D0u)
#define FM1_EMU_MSG (*(volatile uint32_t *)0x1EEF0D4u)
#define FM1_ETM_CON (*(volatile uint32_t *)0x1EEF1C0u)
#define FM1_ETM_PC(i) (*(volatile uint32_t *)(0x1EEF1C4u + 4u * (i)))
#define FM1_C1_EMU_MSG (*(volatile uint32_t *)0x1EEF2D4u)   /* q32DSP(1): + 0x200 */
#define FM1_C1_PCRS (*(volatile uint32_t *)0x1EEF27Cu)
#define FM1_C1_ETM_PC(i) (*(volatile uint32_t *)(0x1EEF3C4u + 4u * (i)))
#define FM1_DBG_WR_EN (*(volatile uint32_t *)0x1EEE240u)
#define FM1_DBG_MSG (*(volatile uint32_t *)0x1EEE244u)
#define FM1_DBG_MSG_CLR (*(volatile uint32_t *)0x1EEE248u)
#define FM1_DBG_EN (*(volatile uint32_t *)0x1EEE340u)
/* DBG_MSG's error bits by core (SDK cpu/wl82/debug.c, as Melodee found them): MMU, PC / write limits,
 * bus-invalid fetch / read / write (JIANT 0.6.71) */
#define FM1_DBG_C1 ((1u << 6) | (1u << 8) | (1u << 10) | (1u << 11) | (7u << 16))
#define FM1_DBG_C0 ((1u << 7) | (1u << 9) | (1u << 12) | (1u << 13) | (7u << 19))
#ifndef FM1_DUAL_CORE
#define FM1_DUAL_CORE 1
#endif

enum { FM1_IRQ_EXCEPTION = 1, FM1_IRQ_ALNK0 = 11, FM1_IRQ_SPI1 = 16, FM1_IRQ_UART1 = 20,
       FM1_IRQ_SARADC = 24, FM1_IRQ_SPI2 = 37, FM1_IRQ_TIMER5 = 63, FM1_IRQ_SOFT0 = 120 };

static inline uint32_t fm1_icfg(void) { uint32_t v; __asm__ volatile("%0 = icfg" : "=r"(v)); return v; }
static inline void fm1_icfg_set(uint32_t v) { __asm__ volatile("icfg = %0" ::"r"(v) : "memory"); }
static inline void fm1_irq_off(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void fm1_irq_on(void) { __asm__ volatile("csync\n\tsti" ::: "memory"); }
static inline uint32_t fm1_cnum(void) { uint32_t v; __asm__ volatile("%0 = cnum" : "=r"(v)); return v; }   /* this core: 0 or 1 */

#define FM1_CRASH_MAGIC 0x43525348u          /* "CRSH" */
typedef struct {
    uint32_t magic, count, vec, pc, rets, emu, dbg, sp, psr, icfg, uptime_ms;
    uint32_t etm[4];
    uint32_t early;                          /* consecutive crashes < 3 s after boot */
    uint32_t r[16], rete, retx, usp, ssp;   /* the frame (RIFT 0.3.2: a crash read over SysEx) */
    uint32_t c1_emu, c1_pcrs, c1_etm[4];     /* core 1 at that moment: its errors, where it ran, its branches */
} fm1_crash_t;
/* Not static: .noinit holds whatever the last run (or power-on) left, and a
 * file-local object would let the compiler assume its zero initial value and
 * fold the magic check away. */
fm1_crash_t fm1_crash __attribute__((section(".noinit")));

extern const char fm1_fatal_stubs[];
static void fm1_fault(const fm1_crash_t *c);   /* application: report, then reset */

static void fm1_irq_init(void)
{
    uint32_t i;
    for (i = 0; i < 32u; i++)
        *(volatile uint32_t *)(0x1EEF100u + 4u * i) = 0;   /* every source off (CPU0 bank) */
    FM1_ILAT_CLR = 0xFFu;
    FM1_EMU_MSG = 0xFFFFFFFFu;
    if (!(FM1_DBG_WR_EN & 1u))
        FM1_DBG_WR_EN = 0xE7u;                              /* unlock */
    FM1_DBG_MSG_CLR = 0xFFFFFFFFu;
    for (i = 0; i < 128u; i++)
        FM1_VEC[i] = (uint32_t)(uintptr_t)(fm1_fatal_stubs + 6u * i);
    FM1_ICFG(1) = (FM1_ICFG(1) & ~0xF0u) | 0xF0u;           /* exception: enable, prio 7 */
    /* div0 trap off (cleared, in case a reset kept it from a build that armed it), as SLOOP 2.4 / Felucca 1.0.3.1 (#61,
     * after PR #111 by spinkham): this compiler treats a divide as harmless and may run a loop-invariant one ahead of the
     * test that guards it. Off, such a divide gives a value nobody uses; the cost: a real divide by 0 gives a wrong value
     * instead of a crash report (the host tests run with -fsanitize=integer-divide-by-zero to catch one) */
    FM1_EMU_CON &= ~(1u << 2);
    FM1_ETM_CON |= 1u;                                      /* branch trace for the report */
}

static void fm1_irq_attach(uint32_t n, void (*h)(void), uint32_t prio)   /* IRQs off */
{
    uint32_t sh = (n & 7u) * 4u;
    FM1_VEC[n] = (uint32_t)(uintptr_t)h;
    FM1_ICFG(n) = (FM1_ICFG(n) & ~(0xFu << sh)) | ((((prio & 7u) << 1) | 1u) << sh);
}
static void fm1_irq_mask(uint32_t n) { FM1_ICFG(n) &= ~(1u << ((n & 7u) * 4u)); }

static void fm1_irq_enable_all(void)
{
    fm1_icfg_set(fm1_icfg() | 0x100u);
    fm1_irq_on();
}

/* The second core's faults (hal/fm1_multicore.h; JIANT 0.6.71 after Melodee, keremimo/melodee#17). They are not a crash
 * of the unit: core 1 is held, the fault counted, and core 0 renders its share again. Core 1 enables no interrupt, so
 * the debug unit's errors about it raise the exception on core 0, whose own state is intact: core 0 holds core 1,
 * clears those bits and resumes (fm1_vec.S). Core 1 may also take an exception itself: it records it and stops there */
static struct { volatile uint32_t count; uint32_t cpu, dbg, pc; } fm1_core1_fault;
static int fm1_core1_fault_c(const uint32_t *f)   /* 1: core 1's, core 0 resumes */
{
#if FM1_DUAL_CORE
    uint32_t cpu = fm1_cnum(), dbg = FM1_DBG_MSG;
    if (!cpu && ((dbg & FM1_DBG_C0) || !(dbg & FM1_DBG_C1) || fm1_core1_fault.count >= 16u))
        return 0;                                   /* core 0's own (or a hold that did not take): the crash report */
    fm1_core1_stop();
    fm1_core1_fault.cpu = cpu;
    fm1_core1_fault.dbg = dbg;
    fm1_core1_fault.pc = f[16];
    fm1_core1_fault.count++;
    if (!(FM1_DBG_WR_EN & 1u))
        FM1_DBG_WR_EN = 0xE7u;                      /* (unlock) */
    FM1_DBG_MSG_CLR = dbg & FM1_DBG_C1;
    __asm__ volatile("csync" ::: "memory");
    if (cpu)
        for (;;)
            ;                                       /* (held: it goes no further) */
    return 1;
#else
    (void)f;
    return 0;
#endif
}

/* called from fm1_fatal_common (fm1_vec.S) with the saved frame:
 * f[0..15] r0..r15, f[16] reti, f[17] rete, f[18] retx, f[19] stub+6,
 * f[20] psr, f[21] icfg, f[22] usp, f[23] ssp, f[24] sp, f[25] interrupted rets.
 * Returns only after a fault of the second core, to resume core 0 */
void fm1_fault_c(uint32_t *f)
{
    static volatile uint32_t in_fault;
    uint32_t i, count, early;
    if (fm1_core1_fault_c(f))
        return;                                   /* (fm1_vec.S resumes core 0) */
    count = fm1_crash.magic == FM1_CRASH_MAGIC ? fm1_crash.count : 0;
    early = fm1_crash.magic == FM1_CRASH_MAGIC ? fm1_crash.early : 0;
    fm1_irq_off();
    if (in_fault++)                           /* fault while reporting: reset at once */
        *(volatile uint32_t *)0x10000u |= 1u << 4;
    fm1_crash.magic = FM1_CRASH_MAGIC;
    fm1_crash.count = count + 1u;
    fm1_crash.vec = (f[19] - (uint32_t)(uintptr_t)fm1_fatal_stubs) / 6u - 1u;
    fm1_crash.pc = f[16];
    fm1_crash.rets = f[25];
    fm1_crash.emu = FM1_EMU_MSG;
    fm1_crash.dbg = FM1_DBG_MSG;
    fm1_crash.sp = f[24];
    fm1_crash.psr = f[20];
    fm1_crash.icfg = f[21];
    fm1_crash.uptime_ms = *(volatile uint32_t *)0x10804u / 24000u;   /* TIMER4 since boot */
    for (i = 0; i < 4u; i++)
        fm1_crash.etm[i] = FM1_ETM_PC(i);
    fm1_crash.early = fm1_crash.uptime_ms < 3000u ? early + 1u : 0u;
    for (i = 0; i < 16u; i++)
        fm1_crash.r[i] = f[i];
    fm1_crash.rete = f[17];
    fm1_crash.retx = f[18];
    fm1_crash.usp = f[22];
    fm1_crash.ssp = f[23];
    fm1_crash.c1_emu = FM1_C1_EMU_MSG;
    fm1_crash.c1_pcrs = FM1_C1_PCRS;
    for (i = 0; i < 4u; i++)
        fm1_crash.c1_etm[i] = FM1_C1_ETM_PC(i);
    fm1_fault(&fm1_crash);
    for (;;)
        ;
}
