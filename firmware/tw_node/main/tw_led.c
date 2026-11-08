/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0
 *
 * tw_led: the color a board's RGB LED shows (tw_led.h).
 */
#include "tw_led.h"

#include <math.h>
#include <string.h>

/* replay.py, light theme (as it is usually seen): --s1 .. --s5, pale blue for a small change to
   deep navy for a loud one */
static const uint8_t RAMP[5][3] = {
    {0xcd, 0xe2, 0xfb}, {0x86, 0xb6, 0xef}, {0x39, 0x87, 0xe5}, {0x1c, 0x5c, 0xab}, {0x0d, 0x36, 0x6b},
};

/* how far along the scale a score is: 0 at quiet, 1 at 6 x quiet and above (log, as the replay) */
static float scale_of(uint8_t score)
{
    float f = logf(score / 10.0f) / logf(6.0f);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

uint8_t tw_led_pick(const uint8_t score[TW_LED_LINKS])
{
    uint8_t best = 0;
    for (int i = 0; i < TW_LED_LINKS; i++) {
        if (score[i] >= TW_LED_QUIET && score[i] > best) {
            best = score[i];
        }
    }
    return best;
}

void tw_led_srgb(uint8_t score, uint8_t out[3])
{
    if (score < TW_LED_QUIET) {
        memset(out, 0, 3);
        return;
    }
    /* as replay.py's colorFor: log scale, quiet .. 6 x quiet over the ramp */
    float f = scale_of(score) * 4.0f;
    int a = (int)f, b = a < 4 ? a + 1 : 4;
    float w = f - (float)a;
    for (int k = 0; k < 3; k++) {
        out[k] = (uint8_t)lrintf(RAMP[a][k] + (RAMP[b][k] - RAMP[a][k]) * w);
    }
}

void tw_led_color(uint8_t score, uint8_t bright, uint8_t out[3])
{
    uint8_t c[3];
    tw_led_srgb(score, c);
    /* the color's hue, at a strength that grows with the change (a quarter of the brightness at
       quiet, all of it at 6 x): on screen the deep navy is dark, on an LED dark is dim, so the hue
       alone would make the loudest the faintest */
    uint8_t mx = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    float level = 0.25f + 0.75f * scale_of(score);
    for (int k = 0; k < 3; k++) {
        float v = mx ? (float)c[k] / mx : 0.0f;                  /* the hue, brightest part 1 */
        out[k] = (uint8_t)lrintf(powf(v, 2.2f) * level * bright);  /* linear for the LED */
    }
}
