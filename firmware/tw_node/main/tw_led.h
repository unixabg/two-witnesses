/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0
 *
 * tw_led: what a board's RGB LED shows (tw_node 2.4): its own movement score (tw_proto.c,
 * 2.2) on the loudest link it measures, in the replay's colors (replay.py, light theme):
 *   quiet (score under 1.15 x the quiet level, as the replay)      off
 *   change                                                          the blue ramp: faint pale blue
 *                                                                   to full deep blue at 6 x quiet
 * Movement only (2.4.3): the replay's orange (a link's shape standing apart from its usual) is
 * a different measurement, not made on the board. Plain C, no ESP-IDF: main.c drives the LED;
 * this decides the color (and a test can run it on a computer).
 */
#ifndef TW_LED_H
#define TW_LED_H

#include <stdint.h>

#define TW_LED_LINKS      16    /* boards a board can hear (TW_MAX_NODES) */
#define TW_LED_QUIET      12    /* score x10 from which a link counts as changed (1.15 x quiet) */

/* that second's scores (x10; 0 = none, as in TWM): the loudest change, 0 when all are quiet */
uint8_t tw_led_pick(const uint8_t score[TW_LED_LINKS]);

/* the color in the replay's terms (sRGB, 0..255): off, or the ramp */
void tw_led_srgb(uint8_t score, uint8_t out[3]);

/* what goes to the LED: that color's hue made linear for the LED (gamma 2.2), at a strength that
   grows with the change (a quarter of `bright`, 1..255, at quiet; all of it at 6 x quiet) */
void tw_led_color(uint8_t score, uint8_t bright, uint8_t out[3]);

#endif
