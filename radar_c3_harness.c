/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0 */
/* Minimal bare-metal host for Espressif's esp_radar_motion_dec (ESP32-C3 build),
 * so the closed motion/presence math can run under an emulator on recorded CSI.
 *
 * radar_c3.elf = this file + libesp_radar_motion_dec.a 0.1.1 (esp32c3, IDF 5.5),
 * (c) Espressif Systems, Apache License 2.0 (LICENSE-esp-radar.txt), downloaded
 * from components.espressif.com, + zig's compiler_rt soft-float routines (MIT).
 * Built with:
 *   python3 -m ziglang cc -target riscv32-freestanding-none -mcpu=generic_rv32+m+c -O2  *     -ffreestanding -fno-builtin -Wl,--no-gc-sections -Wl,-e,_start -Wl,--image-base=0x10000  *     -o radar_c3.elf radar_c3_harness.c libesp_radar_motion_dec.a
 */
#include <stdint.h>
#include <stddef.h>

typedef int esp_err_t;
esp_err_t esp_radar_motion_dec_compute(uint32_t cols, uint32_t row_0, const float *data_0,
                                       uint32_t row_1, const float *data_1, float *output);
float esp_radar_motion_dec_wander_compute(const float *a, const float *b, size_t len);
float esp_radar_motion_dec_compute_jitter(float *current, float **history, uint16_t cols,
                                          uint8_t window_size, uint32_t buff_num);

/* ---- heap: bump allocator, reset by the host before each call ---- */
static uint8_t heap[256 * 1024] __attribute__((aligned(16)));
volatile uint32_t heap_top = 0;
volatile uint32_t n_malloc = 0, n_powf = 0, n_sqrtf = 0, n_log = 0;
volatile uint32_t last_pow_exp_bits = 0;

void *malloc(size_t n) {
    n_malloc++;
    n = (n + 15) & ~(size_t)15;
    if (heap_top + n > sizeof(heap)) return 0;
    void *p = &heap[heap_top];
    heap_top += n;
    return p;
}
void free(void *p) { (void)p; }
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void esp_log(int cfg, const char *tag, const char *fmt, ...) { (void)cfg; (void)tag; (void)fmt; n_log++; }
uint32_t esp_log_timestamp(void) { return 0; }

/* correctly rounded sqrtf (round to nearest even), integer method */
static uint64_t isqrt64(uint64_t n) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}
float sqrtf(float x) {
    n_sqrtf++;
    union { float f; uint32_t u; } v = { x };
    uint32_t u = v.u;
    if (u == 0 || u == 0x80000000u) return x;
    if (u & 0x80000000u) { v.u = 0x7fc00000u; return v.f; }
    if ((u >> 23) == 0xff) return x;
    int32_t e = (int32_t)(u >> 23);
    uint64_t m = u & 0x7fffff;
    if (e == 0) { e = 1; while (!(m & 0x800000)) { m <<= 1; e--; } } else m |= 0x800000;
    e -= 127;                       /* x = m * 2^(e-23) */
    if (e & 1) { m <<= 1; e -= 1; }
    /* sqrt(x) = sqrt(m * 2^(e-23)); take m * 2^(e-23) = (m << 25) * 2^(e-48) -> sqrt = isqrt(m<<25) * 2^((e-48)/2) */
    uint64_t big = m << 25;
    uint64_t q = isqrt64(big);      /* 25 bits */
    uint64_t rem = big - q * q;
    /* q has 25 significant bits; we need 24: round using the extra bit and remainder */
    uint64_t q24 = q >> 1, lastbit = q & 1;
    int exp_out = (e - 48) / 2 + 1;
    if (lastbit && (rem != 0 || (q24 & 1))) q24++;
    if (q24 & 0x1000000) { q24 >>= 1; exp_out++; }
    int32_t E = exp_out + 23 + 127;
    v.u = ((uint32_t)E << 23) | ((uint32_t)q24 & 0x7fffff);
    return v.f;
}
float powf(float x, float y) {
    n_powf++;
    union { float f; uint32_t u; } v = { y };
    last_pow_exp_bits = v.u;
    if (y == 2.0f) return x * x;
    if (y == 1.0f) return x;
    if (y == 0.5f) return sqrtf(x);
    if (y == 0.0f) return 1.0f;
    union { float f; uint32_t u; } nan = { .u = 0x7fc00000u };
    return nan.f;                   /* any other exponent shows up as NaN: then we will know */
}

/* ---- entry points the emulator host calls ---- */
volatile int32_t ret_code;
volatile float ret_float;
void call_compute(uint32_t cols, uint32_t r0, const float *d0, uint32_t r1, const float *d1, float *out) {
    heap_top = 0;
    ret_code = esp_radar_motion_dec_compute(cols, r0, d0, r1, d1, out);
}
void call_wander(const float *a, const float *b, uint32_t len) {
    heap_top = 0;
    ret_float = esp_radar_motion_dec_wander_compute(a, b, len);
}
void call_jitter(float *cur, float **hist, uint32_t cols, uint32_t win, uint32_t num) {
    heap_top = 0;
    ret_float = esp_radar_motion_dec_compute_jitter(cur, hist, (uint16_t)cols, (uint8_t)win, num);
}
void _start(void) { for (;;) {} }
