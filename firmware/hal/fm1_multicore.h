/* SPDX-License-Identifier: GPL-3.0-only */
/* (JIANT 0.6.71) The AC79's second core, for the audio render (fx.c mix_block: some synth tracks on it).
 * What starts it and what keeps it safe comes from Melodee (github.com/keremimo/melodee, GPL-3.0) and fm1-x0x
 * (github.com/charlesvestal/fm1-x0x, GPL-3.0), who worked it out on the device from the AC79 SDK (its EnableOtherCpu
 * and startup): this is JIANT's own version of it.
 *
 *   fm1_mc_start()   at boot, before fm1_guard_lock_top: core 1 out of hold into fm1_c1_entry (crt0.S); 1 = it answered
 *   fm1_mc_run(f,a)  core 0 (the audio ISR): hand it f(a); 1 = taken, fm1_mc_wait before touching what f works on
 *   fm1_mc_wait()    0 = done; -1 = it read a wrong job, faulted or did not finish: held for good, core 0 does the work
 *   fm1_mc_hold()    stop it for good (MENU > CORES 1, a fault)
 *
 * Start: its interrupt bank all off, its entry address in the word the chip starts it from (0x01C7FFF8, in the
 * protected top of RAM: before fm1_guard_lock_top), bit 3 of 0x10008 held while it starts, C1_CON (0x1EEE004) bit 3
 * set and bit 1 cleared. It boots through the chip's ROM, which the PC limits refuse: open until it reports in.
 *
 * Units that cannot run it (Melodee #17 / #21, fm1-x0x #10): on some FM-1s core 1 now and then reads a word of RAM
 * wrong (0x00200000 where it holds 0), more often under load. So it runs a job only when the job's check word (the
 * function, the argument and the job's number) matches what it read, and keeps the count of the jobs it finished in a
 * register; anything else it counts (mb.bad) and core 0 holds it at the first one. A fault of core 1 is caught in
 * fm1_irq.h (fm1_core1_fault). Once it was given up, or core 0 crashed while it worked (fm1_mc_bar), every boot plays
 * on one core until the unit is switched off.
 *
 * Its waiting loop is in RAM (.c1_text, app.ld): idle it never fetches from the flash; it works only for the audio ISR, which
 * waits for it before it returns, and core 0 turns the flash off only with interrupts off. Both cores share the
 * caches: csync orders what each publishes. */
#pragma once
#include <stdint.h>
#include "fm1_irq.h"
#include "fm1_guard.h"

#define FM1_C1_CON (*(volatile uint32_t *)0x1EEE004u)
#define FM1_C1_SYNC() __asm__ volatile("csync" ::: "memory")
#define FM1_C1_T4() (*(volatile uint32_t *)0x10804u)            /* TIMER4, 24 MHz */
#define FM1_C1_CHECK(fn, arg, job) ((uint32_t)(uintptr_t)(fn) ^ (arg) ^ (job) * 0x9E3779B1u)

extern void fm1_c1_entry(void);
extern uint8_t _c1_stack_lo[], _c1_stack_hi[];
typedef struct {
    volatile uint32_t job, arg;                 /* core 0's: job + 1 hands fn(arg) over */
    void (*volatile fn)(uint32_t);
    volatile uint32_t check;
    uint32_t pad0[12];                          /* (each core's words on cache lines of their own) */
    volatile uint32_t alive, done, bad;         /* core 1's: started; the last job finished; jobs refused */
    uint32_t pad1[13];
} fm1_mc_box_t;
static fm1_mc_box_t fm1_mc_box __attribute__((aligned(64)));
static inline uint32_t fm1_mc_ticks(void) { return FM1_C1_T4(); }   /* (0.6.73) TIMER4 from either core: fx.c times the parts */
static uint8_t fm1_mc_on;                       /* answering: core 0 may hand it work */
static uint8_t fm1_mc_lost;                     /* why it stopped: 1 a wrong job, 2 a job not back, 3 a fault */
static uint32_t fm1_mc_max_ticks;               /* the longest wait for a job (TIMER4 ticks): MENU > ABOUT */

/* carried across soft resets (a crash, the watchdog, an update), gone at power-off */
#define FM1_MC_BAR_MAGIC 0x3152414Au            /* "JAR1" */
typedef struct { uint32_t magic, barred; } fm1_mc_bar_t;
fm1_mc_bar_t fm1_mc_bar __attribute__((section(".noinit")));   /* (not static: no assumed zero start) */
static inline int fm1_mc_barred(void) { return fm1_mc_bar.magic == FM1_MC_BAR_MAGIC && fm1_mc_bar.barred; }
static inline void fm1_mc_bar_set(void) { fm1_mc_bar.magic = FM1_MC_BAR_MAGIC; fm1_mc_bar.barred = 1; }

void fm1_c1_main(void);
void __attribute__((section(".c1_text"), noinline, noreturn, used)) fm1_c1_main(void)
{
    /* its stack limit in its own EMU bank (core 0's is at 0x1EEF0D0), the lowest 256 bytes left out, as Melodee sets it */
    uint32_t lo = (uint32_t)(uintptr_t)_c1_stack_lo + 256u, hi = (uint32_t)(uintptr_t)_c1_stack_hi - 1u, done;
    *(volatile uint32_t *)0x1EEF2D8u = hi;
    *(volatile uint32_t *)0x1EEF2DCu = lo;
    *(volatile uint32_t *)0x1EEF2E0u = hi;
    *(volatile uint32_t *)0x1EEF2E4u = lo;
    *(volatile uint32_t *)0x1EEF2D0u = (*(volatile uint32_t *)0x1EEF2D0u & ~((1u << 2) | (0x1Fu << 16))) | (1u << 3);
    done = fm1_mc_box.done;
    fm1_mc_box.alive = 1;
    FM1_C1_SYNC();
    for (;;) {
        uint32_t j = fm1_mc_box.job;
        FM1_C1_SYNC();
        if (j == done)
            continue;
        if (j == done + 1u) {
            void (*fn)(uint32_t) = fm1_mc_box.fn;
            uint32_t arg = fm1_mc_box.arg;
            if (fn && fm1_mc_box.check == FM1_C1_CHECK(fn, arg, j)) {
                fn(arg);                        /* (the work itself runs from the flash) */
                done = j;
                FM1_C1_SYNC();
                fm1_mc_box.done = j;
                FM1_C1_SYNC();
                continue;
            }
        }
        fm1_mc_box.bad++;                       /* a wrong read: refused, core 0 holds it */
        FM1_C1_SYNC();
        for (;;)
            ;
    }
}

static void fm1_mc_hold(void)
{
    fm1_core1_stop();
    fm1_mc_on = 0;
}

static int fm1_mc_start(void)
{
    uint32_t saved, t0, i;
    fm1_mc_hold();
    fm1_mc_box.alive = fm1_mc_box.bad = 0;
    fm1_mc_box.done = fm1_mc_box.job;
    fm1_mc_box.fn = 0;
    FM1_C1_SYNC();
    for (i = 0; i < 32u; i++)                   /* its interrupt bank: all off */
        *(volatile uint32_t *)(0x1EEF300u + 4u * i) = 0;
    fm1_guard_pc_open();
    *(volatile uint32_t *)0x01C7FFF8u = (uint32_t)(uintptr_t)&fm1_c1_entry;
    saved = *(volatile uint32_t *)0x10008u;
    *(volatile uint32_t *)0x10008u = saved | 8u;
    FM1_C1_SYNC();
    FM1_C1_CON |= 8u;
    FM1_C1_CON &= ~2u;
    t0 = FM1_C1_T4();
    while (!fm1_mc_box.alive && FM1_C1_T4() - t0 < 24000u * 20u)   /* 20 ms */
        FM1_C1_SYNC();
    *(volatile uint32_t *)0x10008u = saved;
    fm1_guard_enable(FM1_GUARD_PC);
    if (!fm1_mc_box.alive) {
        fm1_mc_hold();
        return 0;
    }
    fm1_mc_on = 1;
    return 1;
}

static inline int fm1_mc_run(void (*fn)(uint32_t), uint32_t arg)
{
    uint32_t job;
    if (!fm1_mc_on)
        return 0;
    if (fm1_core1_fault.count) {                /* (faulted between blocks) */
        fm1_mc_hold();
        fm1_mc_lost = 3;
        return 0;
    }
    job = fm1_mc_box.job + 1u;
    fm1_mc_box.fn = fn;
    fm1_mc_box.arg = arg;
    fm1_mc_box.check = FM1_C1_CHECK(fn, arg, job);
    FM1_C1_SYNC();
    fm1_mc_box.job = job;
    FM1_C1_SYNC();
    return 1;
}

static inline int fm1_mc_wait(void)
{
    uint32_t t0 = FM1_C1_T4(), w;
    for (;;) {
        FM1_C1_SYNC();
        if (fm1_mc_box.done == fm1_mc_box.job)
            break;
        if (fm1_mc_box.bad || fm1_core1_fault.count || FM1_C1_T4() - t0 > 24000u * 8u) {   /* 8 ms: far past a block */
            fm1_mc_lost = fm1_mc_box.bad ? 1u : fm1_core1_fault.count ? 3u : 2u;
            fm1_mc_hold();
            return -1;
        }
    }
    w = FM1_C1_T4() - t0;
    if (w > fm1_mc_max_ticks)
        fm1_mc_max_ticks = w;
    return 0;
}
