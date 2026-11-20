/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0 */
/* csi_sense.h - the CSI sensing module's portable part (csi_sense.c, docs/MODULES.md) */
#pragma once
#include "tw_proto.h"

/* given to the core: tw_node_t.sense = &tw_csi_sense (the firmware's module table, the simulator) */
extern const tw_sense_t tw_csi_sense;

/* 3.4: learn every link's quiet level again (TW_K_RELEARN, and after a channel change) */
void     tw_score_relearn(tw_node_t *n);
/* 3.4: this board's detection threshold, x10 (its TW_K_DETECT, or TW_DETECT_DEFAULT) */
uint8_t  tw_detect_level(const tw_node_t *n);
/* 4.7: this board's detection hold, in seconds (its TW_K_DETECT_HOLD, minutes, or TW_DHOLD_DEFAULT;
   0: none) */
uint16_t tw_detect_hold_s(const tw_node_t *n);
/* 3.5: this board's steadier-than-quiet threshold, x10 (its TW_K_DETECT_LOW, or TW_LOW_DEFAULT;
   0: off) */
uint8_t  tw_detect_low_level(const tw_node_t *n);
/* the L-LTF byte ranges for an ESP32/S3 CSI buffer (esp-radar's table); 0 if unknown */
int      tw_lltf_ranges(uint8_t secondary, uint8_t sig_mode, uint8_t cwb, uint8_t stbc, int len,
                       int ranges[4]);
