/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0
 *
 * tw_led: the color a board's RGB LED shows (tw_led.h).
 */
#include "tw_led.h"

#include <math.h>
#include <string.h>

/* replay.py, dark theme: --quiet, --s1 .. --s5, and --shape (the lasting change) */
static const uint8_t RAMP[6][3] = {
    {0x3a, 0x3a, 0x37}, {0x18, 0x4f, 0x95}, {0x2a, 0x78, 0xd6},
    {0x55, 0x98, 0xe7}, {0x9e, 0xc5, 0xf4}, {0xe3, 0xee, 0xfc},
};
static const uint8_t ORANGE[3] = {0xd9, 0x59, 0x26};

void tw_led_step(tw_led_state_t *st, const uint8_t score[TW_LED_LINKS], uint8_t *show, int *orange)
{
    uint8_t best = 0;
    int any_lasting = 0;
    for (int i = 0; i < TW_LED_LINKS; i++) {
        uint8_t v = score[i];
        if (v == 0) {
            continue;                       /* nothing measured this second: no change either way */
        }
        if (v >= TW_LED_QUIET) {
            if (st->above[i] < 255) {
                st->above[i]++;
            }
            st->calm[i] = 0;
            if (st->above[i] >= TW_LED_LASTING_S) {
                st->lasting[i] = 1;
            }
        } else if (++st->calm[i] >= TW_LED_CALM_S) {
            st->calm[i] = TW_LED_CALM_S;
            st->above[i] = 0;
            st->lasting[i] = 0;             /* back to quiet: relearned, or over */
        }
        if (st->lasting[i]) {
            any_lasting = 1;
        } else if (v >= TW_LED_QUIET && v > best) {
            best = v;
        }
    }
    *show = best;
    *orange = !best && any_lasting;
}

void tw_led_srgb(uint8_t score, int orange, uint8_t out[3])
{
    if (orange) {
        memcpy(out, ORANGE, 3);
        return;
    }
    if (score < TW_LED_QUIET) {
        memset(out, 0, 3);
        return;
    }
    /* as replay.py's colorFor: log scale, quiet .. 6 x quiet over the ramp */
    float f = logf(score / 10.0f) / logf(6.0f);
    f = (f < 0 ? 0 : f > 1 ? 1 : f) * 5.0f;
    int a = (int)f, b = a < 5 ? a + 1 : 5;
    float w = f - (float)a;
    for (int k = 0; k < 3; k++) {
        out[k] = (uint8_t)lrintf(RAMP[a][k] + (RAMP[b][k] - RAMP[a][k]) * w);
    }
}

void tw_led_color(uint8_t score, int orange, uint8_t bright, uint8_t out[3])
{
    uint8_t c[3];
    tw_led_srgb(score, orange, c);
    for (int k = 0; k < 3; k++) {
        float lin = powf(c[k] / 255.0f, 2.2f) * bright;
        out[k] = (uint8_t)(c[k] && lin < 1.0f ? 1 : lrintf(lin));  /* a color part never goes out */
    }
}
