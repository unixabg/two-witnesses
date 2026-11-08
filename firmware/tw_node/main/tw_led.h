/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0
 *
 * tw_led: what a board's RGB LED shows (tw_node 2.4): its own movement score (tw_proto.c,
 * 2.2) on the loudest link it measures, in the replay's colors (replay.py, light theme):
 *   quiet (score under 1.15 x the quiet level, as the replay)      off
 *   change                                                          the blue ramp: faint pale blue
 *                                                                   to full deep blue at 6 x quiet
 *                                                                   (2.4.1; 2.4 ran the dark theme's
 *                                                                   ramp, loud showing near-white)
 *   a lasting change (a link above quiet over 60 s, until the board's quiet level has
 *   relearned it: as the replay's orange mark)                      orange
 * A fresh change on another link shows over orange. Plain C, no ESP-IDF: main.c drives the LED;
 * this decides the color (and the simulator or a test can run it on a computer).
 */
#ifndef TW_LED_H
#define TW_LED_H

#include <stdint.h>

#define TW_LED_LINKS      16    /* boards a board can hear (TW_MAX_NODES) */
#define TW_LED_QUIET      12    /* score x10 from which a link counts as changed (1.15 x quiet) */
#define TW_LED_LASTING_S  60    /* seconds above quiet that make a change lasting */
#define TW_LED_CALM_S     5     /* seconds below quiet that end a change (shorter dips do not) */

typedef struct {
    uint8_t above[TW_LED_LINKS];    /* seconds above quiet, per board heard */
    uint8_t calm[TW_LED_LINKS];     /* seconds below quiet since */
    uint8_t lasting[TW_LED_LINKS];
} tw_led_state_t;

/* once a second, with that second's scores (x10; 0 = none, as in TWM): what to show.
   *score = the loudest fresh change's score (0: none); *orange = 1 for a lasting change only */
void tw_led_step(tw_led_state_t *st, const uint8_t score[TW_LED_LINKS], uint8_t *show, int *orange);

/* the color in the replay's terms (sRGB, 0..255): off, the ramp, or orange */
void tw_led_srgb(uint8_t score, int orange, uint8_t out[3]);

/* what goes to the LED: that color's hue made linear for the LED (gamma 2.2), at a strength that
   grows with the change (a quarter of `bright`, 1..255, at quiet; all of it at 6 x quiet) */
void tw_led_color(uint8_t score, int orange, uint8_t bright, uint8_t out[3]);

#endif
