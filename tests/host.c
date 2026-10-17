/* SPDX-License-Identifier: GPL-3.0-only */
/* A fake FM-1 on a computer: the app (firmware/src/dinghy.c) runs unchanged on it. platform.c includes
 * this file instead of the hardware when DINGHY_HOST is set. A script presses keys over time; out come
 * the screen (PPM, at the times SHOT asks) and the sound (WAV, as the FM-1 sends it over USB).
 *
 *   mkdir -p build/host
 *   cc -O2 -Wall -DDINGHY_HOST -Ifirmware/src -Ifirmware/gen -o build/host/dinghy firmware/src/dinghy.c
 *   build/host/dinghy build/host/dinghy        (-> build/host/dinghy.wav, build/host/dinghy-<ms>.ppm)
 *   build/host/dinghy OUT script.txt [FPS]    (your own script; with FPS: OUT-NNNNN.ppm, a frame every 1/FPS s:
 *                                             a film of your firmware, e.g. ffmpeg -r 30 -i OUT-%05d.ppm ...)
 *
 * Change SCRIPT below to try your own app. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef APP_NAME
#define APP_NAME "DINGHY"
#endif
#ifndef APP_VER
#define APP_VER "0.0"
#endif
#define FS 44100u
#define HALF_FRAMES 256u

static volatile uint32_t fm1_ms;
static struct { uint32_t notes, buttons; } fm1_in;
static uint32_t host_edges, host_rel;
static int32_t host_enc[7];
static uint32_t fm1_input_edges(uint32_t *released) { uint32_t b = host_edges; host_edges = 0; *released = host_rel; host_rel = 0; return b; }
static uint32_t fm1_input_note_edges(void) { return 0; }   /* (the script sets fm1_in.notes) */
static int32_t fm1_enc_take(uint32_t e) { int32_t d = host_enc[e]; host_enc[e] = 0; return d; }
static uint8_t host_lit[41], host_glow[41];
static void fm1_led_key(uint32_t id, int on) { if (id < 41u) host_lit[id] = (uint8_t)(on != 0); }
static void fm1_led_bg_all(int on) { (void)on; }
static void fm1_led_glow_key(uint32_t id, int on) { if (id < 41u) host_glow[id] = (uint8_t)(on != 0); }
static volatile uint16_t fm1_led_bg_ns;

/* the second core (hal/fm1_multicore.h): HOST_MC=0 none (stage 2 on core 0), 1 the job at once (before stage 1),
 * 2 the job at the wait (after stage 1): the same sound all three ways, or stage 2 reads what stage 1 writes */
static uint8_t fm1_mc_on;
static int host_mc = -1;
static void (*host_mc_fn)(uint32_t);
static uint32_t host_mc_arg;
static int fm1_mc_run(void (*fn)(uint32_t), uint32_t a)
{
    if (host_mc < 0) { const char *e = getenv("HOST_MC"); host_mc = e ? atoi(e) : 0; fm1_mc_on = host_mc > 0; }
    if (!host_mc) return 0;
    if (host_mc == 1) fn(a); else { host_mc_fn = fn; host_mc_arg = a; }
    return 1;
}
static int fm1_mc_wait(void) { if (host_mc_fn) { host_mc_fn(host_mc_arg); host_mc_fn = 0; } return 0; }
static void crumb(uint32_t c, uint32_t a, uint32_t b) { (void)c; (void)a; (void)b; }
static void mc_start_dual(void) {}
static const char *mc_status(void) { return host_mc > 0 ? "2 CORES" : "1 CORE"; }
static volatile uint8_t usb_stems;
static int16_t stem_q[HALF_FRAMES * 6u];
static int32_t stem_pk[6]; static uint32_t stem_rail[6];
static void usb_stems_set(uint32_t on) { usb_stems = (uint8_t)(on != 0); printf("%6u ms  USB re-enumerates: %s\n", fm1_ms, on ? "8 ch stems" : "stereo"); }

/* the screen: 240 x 240 RGB565, as the ST7789 gets it (gfx.c sends the bytes swapped) */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) {
                uint16_t c = px[j * w + i];
                fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);
            }
}
#define CV_MAX (240u * 124u)
#include "gfx.c"

static uint8_t host_store[3072 + 8];   /* (like the flash: the size is part of the record; a different size does not load) */
static int settings_load(void *p, uint32_t n)
{
    uint32_t len;
    memcpy(&len, host_store + 4, 4);
    if (host_store[0] != 1 || n > 3072u || len != n) return -1;
    memcpy(p, host_store + 8, n);
    return 0;
}
static int settings_save(const void *p, uint32_t n)
{
    host_store[0] = 1;
    memcpy(host_store + 4, &n, 4);
    memcpy(host_store + 8, p, n);
    printf("%6u ms  settings saved (%u B)\n", fm1_ms, n);
    return 0;
}

static uint8_t mq[64][3];
static uint32_t mq_w, mq_r;
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    if (mq_r == mq_w) return 0;
    *st = mq[mq_r % 64u][0]; *d1 = mq[mq_r % 64u][1]; *d2 = mq[mq_r % 64u][2];
    mq_r++;
    return 1;
}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2) { printf("%6u ms  MIDI out %02X %u %u\n", fm1_ms, st, d1, d2); }
/* MIDI clock in, stamped (platform.c's clk_q): "T clock BPM [JITTER_US]" sends 24 ppqn from T, "T clock 0" stops */
#define CKN 64u
static struct { uint32_t t; uint8_t st, src; } clk_q[CKN];
static volatile uint32_t ck_w, ck_r;
static uint64_t host_us;                       /* the host's time, us */
static void clk_push_at(uint32_t st, uint64_t us) { if (ck_w - ck_r < CKN) { clk_q[ck_w % CKN].t = (uint32_t)(us * 24u); clk_q[ck_w % CKN].st = (uint8_t)st; clk_q[ck_w % CKN].src = 0; ck_w++; } }
static uint32_t clk_now(void) { return (uint32_t)(host_us * 24u); }
static uint8_t dna_rx[1024];
static volatile uint32_t dna_rx_len;
static void dna_rx_frame(const uint8_t *f, uint32_t n)
{
    if (dna_rx_len || n < 5u || n > sizeof dna_rx || f[0] != 0x7D || f[1] != 'R') return;
    memcpy(dna_rx, f, n); dna_rx_len = n;
}
static int ota_wire_send(const uint8_t *p, uint32_t n)   /* SysEx out: printed; HOST_DNA_LOOP=1 hears itself */
{
    printf("%6u ms  SysEx out %u B\n", fm1_ms, n);
    if (getenv("HOST_DNA_LOOP") && n > 2u) dna_rx_frame(p + 1, n - 2u);
    return 0;
}

static void app_audio(int32_t *out, uint32_t n);
static void app_main(void);

/* the script: at t ms, keys down (bit k = key k, 0 = F3), a button edge, a MIDI message. A script file (the second
 * argument) replaces it, a line each: "T keys MASK" (hex or decimal), "T btn ID", "T midi ST D1 D2", "T end" */
typedef struct { uint32_t t, keys; int btn; uint8_t st, d1, d2; int up, enc, det, clk, jit; } host_ev_t;
static const host_ev_t SCRIPT0[] = {
    {200, 1u << 7, -1, 0, 0, 0, 0, -1, 0},                                /* C4 */
    {700, 1u << 7 | 1u << 11 | 1u << 14, -1, 0, 0, 0, 0, -1, 0},          /* + E4 G4: a chord */
    {1500, 0, -1, 0, 0, 0, 0, -1, 0},
    {1700, 0, -1, 0x90, 81, 120, 0, -1, 0},                               /* MIDI in: A5 */
    {2300, 0, -1, 0x80, 81, 0, 0, -1, 0},
    {2500, 0, BTN_OCT_UP, 0, 0, 0, 0, -1, 0},
    {2700, 1u << 2 | 1u << 9, -1, 0, 0, 0, 0, -1, 0},                     /* G4 + D5, an octave up */
    {3400, 0, -1, 0, 0, 0, 0, -1, 0},
};
static const host_ev_t *script = SCRIPT0;
static uint32_t script_n = sizeof SCRIPT0 / sizeof SCRIPT0[0], end_ms = 6500, frame_fps;
#ifndef HOST_SHOTS
#define HOST_SHOTS 1000, 3000
#endif
static const uint32_t SHOT[] = {HOST_SHOTS};

static void read_script(const char *path)
{
    static host_ev_t ev[4096];
    char line[256], what[16];
    FILE *f = fopen(path, "r");
    uint32_t n = 0;
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f) && n < 4096u) {
        host_ev_t e = {0, 0, -1, 0, 0, 0, 0, -1, 0};
        unsigned a = 0, b = 0, c = 0;
        if (line[0] == '#' || sscanf(line, "%u %15s", &e.t, what) != 2) continue;
        if (!strcmp(what, "end")) { end_ms = e.t; continue; }
        if (!strcmp(what, "keys")) { sscanf(line, "%*u %*s %i", (int *)&e.keys); }
        else if (!strcmp(what, "btn")) { sscanf(line, "%*u %*s %d", &e.btn); }
        else if (!strcmp(what, "btnup")) { sscanf(line, "%*u %*s %d", &e.btn); e.up = 1; }
        else if (!strcmp(what, "enc")) { sscanf(line, "%*u %*s %d %d", &e.enc, &e.det); }
        else if (!strcmp(what, "clock")) { e.jit = 0; sscanf(line, "%*u %*s %d %d", &e.clk, &e.jit); e.clk = e.clk ? e.clk : -1; }
        else if (!strcmp(what, "midi")) { sscanf(line, "%*u %*s %i %u %u", (int *)&a, &b, &c); e.st = (uint8_t)a; e.d1 = (uint8_t)b; e.d2 = (uint8_t)c; }
        else continue;
        ev[n++] = e;
    }
    fclose(f);
    script = ev;
    script_n = n;
}

static const char *host_out = "build/host/dinghy";
static int16_t *wav;
static uint32_t wav_n;

static void write_ppm_as(const char *name)
{
    FILE *f;
    uint32_t i;
    if (!(f = fopen(name, "wb"))) { perror(name); exit(1); }
    fprintf(f, "P6 240 240 255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = fb[i];
        fputc((c >> 11) * 255 / 31, f);
        fputc((c >> 5 & 63) * 255 / 63, f);
        fputc((c & 31) * 255 / 31, f);
    }
    fclose(f);
}
static void write_ppm(uint32_t t)
{
    char name[512];
    snprintf(name, sizeof name, "%s-%u.ppm", host_out, t);
    write_ppm_as(name);
    printf("%6u ms  screen -> %s\n", t, name);
}

static void write_wav(void)
{
    char name[512];
    FILE *f;
    uint32_t bytes = wav_n * 4u, v[] = {16, 0x00020001u, FS, FS * 4u, 0x00100004u};
    snprintf(name, sizeof name, "%s.wav", host_out);
    if (!(f = fopen(name, "wb"))) { perror(name); exit(1); }
    fwrite("RIFF", 1, 4, f); bytes += 36; fwrite(&bytes, 4, 1, f); bytes -= 36;
    fwrite("WAVEfmt ", 1, 8, f); fwrite(v, 4, 5, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    fwrite(wav, 4, wav_n, f);
    fclose(f);
    printf("sound  -> %s (%u frames)\n", name, wav_n);
}

static void platform_poll(void)                 /* one call = one millisecond */
{
    static uint32_t step, shot, owed, frame, cbpm, cjit, rnd = 12345;
    static uint64_t cnext;
    static int32_t blk[2u * HALF_FRAMES];
    fm1_ms++;
    host_us = (uint64_t)fm1_ms * 1000u;
    while (step < script_n && script[step].t <= fm1_ms) {
        if (script[step].clk) {
            cbpm = script[step].clk > 0 ? (uint32_t)script[step].clk : 0u; cjit = (uint32_t)script[step].jit;
            if (cbpm) { cnext = host_us; clk_push_at(0xFA, host_us); }
            else clk_push_at(0xFC, host_us);
        } else if (script[step].st >= 0xF8u)
            clk_push_at(script[step].st, host_us);
        else if (script[step].st) {
            mq[mq_w % 64u][0] = script[step].st; mq[mq_w % 64u][1] = script[step].d1; mq[mq_w % 64u][2] = script[step].d2;
            mq_w++;
        } else if (script[step].enc >= 0)
            host_enc[script[step].enc] += script[step].det;
        else if (script[step].btn >= 0 && script[step].up) {
            host_rel |= 1u << script[step].btn; fm1_in.buttons &= ~(1u << script[step].btn);
        } else if (script[step].btn >= 0) {
            host_edges |= 1u << script[step].btn; fm1_in.buttons |= 1u << script[step].btn;
        } else
            fm1_in.notes = script[step].keys;
        step++;
    }
    while (cbpm && cnext <= host_us) {          /* the clock: 24 ppqn, the stamp late by up to JITTER us */
        rnd = rnd * 1103515245u + 12345u;
        clk_push_at(0xF8, cnext + (cjit ? (rnd >> 8) % cjit : 0u));
        cnext += 60000000u / (cbpm * 24u);
    }
    owed += FS;                                 /* the frames this millisecond is worth, in blocks as the ISR renders */
    while (owed >= 1000u * HALF_FRAMES) {
        uint32_t i;
        owed -= 1000u * HALF_FRAMES;
        memset(blk, 0, sizeof blk);
        app_audio(blk, HALF_FRAMES);
        if (getenv("HOST_STEMS")) usb_stems = 1;
        if (getenv("HOST_PROF")) prof_req = 1;
        if (usb_stems) {                        /* the stems' level: peaks and samples at the rail (HOST_STEMS report at the end) */
            uint32_t k;
            for (k = 0; k < HALF_FRAMES * 6u; k++) {
                int32_t a = stem_q[k] < 0 ? -stem_q[k] : stem_q[k];
                if (a > stem_pk[k % 6u]) stem_pk[k % 6u] = a;
                if (a >= 32700) stem_rail[k % 6u]++;
            }
        }
        wav = realloc(wav, (wav_n + HALF_FRAMES) * 4u);
        for (i = 0; i < 2u * HALF_FRAMES; i++) {
            int32_t v = blk[i];                 /* (USB audio at the MASTER knob all the way up) */
            wav[2u * wav_n + i] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
        }
        wav_n += HALF_FRAMES;
    }
    if (frame_fps) {                            /* a film: a frame every 1/fps s, as the screen is then */
        if ((uint64_t)frame * 1000u <= (uint64_t)fm1_ms * frame_fps) {
            char name[512];
            snprintf(name, sizeof name, "%s-%05u.ppm", host_out, frame++);
            if (getenv("HOST_LEDS")) {   /* the panel's LEDs at this frame: buttons by id, keys by number (lit *, glow .) */
                uint32_t id;
                printf("%6u ms  LEDS ", fm1_ms);
                for (id = 0; id < 41u; id++) if (host_lit[id] || host_glow[id]) printf("%u%c ", id, host_lit[id] ? '*' : '.');
                printf("\n");
            }
            write_ppm_as(name);
        }
    } else if (shot < sizeof SHOT / sizeof SHOT[0] && fm1_ms == SHOT[shot])
        write_ppm(SHOT[shot++]);
    if (fm1_ms >= end_ms) {
        if (prof_blocks) {   /* where a block's time goes (x86 cycles: the shares, not the device's numbers) */
            static const char *const PN[PF_N] = {"pre", "seq", "ctl", "osc", "filter", "fold", "split", "burst", "post",
                                                 "s2 decim", "s2 low", "s2 crush+ott", "s2 freeze", "s2 phaser", "s2 delay", "s2 drums", "s2 out"};
            uint64_t tot = 0; int k;
            for (k = 0; k < PF_N; k++) tot += prof_acc[k];
            printf("profile over %u blocks (per block avg, cycles):\n", prof_blocks);
            for (k = 0; k < PF_N; k++) printf("  %-14s %8u  %5.1f %%\n", PN[k], prof_acc[k] / prof_blocks, 100.0 * prof_acc[k] / (double)tot);
            printf("  total avg %llu  max block %u\n", (unsigned long long)(tot / prof_blocks), prof_max);
        }
        if (usb_stems) { int c; printf("stems peak/rail:"); for (c = 0; c < 6; c++) printf(" %d/%u", stem_pk[c], stem_rail[c]); printf("\n"); }
        write_wav();
        exit(0);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) host_out = argv[1];
    if (argc > 2) read_script(argv[2]);
    if (argc > 3) frame_fps = (uint32_t)atoi(argv[3]);
    app_main();
    return 0;
}
