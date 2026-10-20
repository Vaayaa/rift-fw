/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
/* profiling (0.5): where a block's time goes, part by part (one core only: the parts run in one line). prof_req asks
 * (SysEx F0 7D 'R' 'P' 'R' 'O' F7 / host HOST_PROF), prof_on runs it from the next block */
enum { PF_PRE, PF_SEQ, PF_CTL, PF_OSC, PF_FLT, PF_FOLD, PF_SPLIT, PF_BURST, PF_POST,
       PF_S2DEC, PF_S2LOW, PF_S2OTT, PF_S2FRZ, PF_S2PH, PF_S2DLY, PF_S2DRM, PF_S2OUT, PF_N };
static uint32_t prof_acc[PF_N], prof_blocks, prof_t, prof_t0, prof_max;
static uint8_t prof_on;
static volatile uint8_t prof_req;
#if defined(DINGHY_HOST) && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#define PROF_NOW() ((uint32_t)__rdtsc())
#elif defined(DINGHY_HOST)
#include <time.h>
static uint32_t prof_now_host(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec); }
#define PROF_NOW() prof_now_host()                    /* (an ARM Mac: ns) */
#else
#define PROF_NOW() fm1_ticks()                        /* TIMER4, 24 MHz */
#endif
#define PROF(k) do { if (prof_on) { uint32_t t_ = PROF_NOW(); prof_acc[k] += t_ - prof_t; prof_t = t_; } } while (0)
