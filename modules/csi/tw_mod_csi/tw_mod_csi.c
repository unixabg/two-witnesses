/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0 */
/*
 * tw_mod_csi.c - the CSI sensing module, board side (docs/MODULES.md, module 1): its settings in
 * flash (detect 3.4, detect low 3.5), their cable commands (TWSET detect, detect low, relearn) and
 * the LED's movement colors, white detection flash and green steadier mark. Moved here unchanged
 * from main.c. The measurements, scores and detections themselves: csi_sense.c (portable).
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "tw_module.h"
#include "tw_led.h"
#include "csi_sense.h"

static const char *TAG = "tw_node";

static void csi_init(tw_node_t *n)
{
    nvs_handle_t h;
    /* 3.4: its detection threshold (x10), never given: the default (reported as such, generation 0) */
    uint8_t det = 0, gd = 0;
    if (nvs_open("tw", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "det", &det);
        nvs_get_u8(h, "lgd", &gd);
        nvs_close(h);
    }
    if (det < TW_DETECT_MIN || det > TW_DETECT_MAX) {
        det = TW_DETECT_DEFAULT;
        gd = 0;
    }
    tw_setting_init(n, TW_K_DETECT, &det, 1, gd);
    /* 3.5: its steadier-than-quiet threshold (x10, 0: off), never given: the default */
    uint8_t low = 255, gl = 0;
    if (nvs_open("tw", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "dlo", &low);
        nvs_get_u8(h, "lgl", &gl);
        nvs_close(h);
    }
    if (low > TW_LOW_MAX) {
        low = TW_LOW_DEFAULT;
        gl = 0;
    }
    tw_setting_init(n, TW_K_DETECT_LOW, &low, 1, gl);
}

static void csi_keys_changed(tw_node_t *n, uint32_t ch)
{
    if (ch & (1u << TW_K_DETECT)) {     /* 3.4: its detection threshold: kept */
        nvs_handle_t h;
        if (nvs_open("tw", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "det", tw_detect_level(n));
            nvs_set_u8(h, "lgd", n->set_gen[TW_K_DETECT]);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGW(TAG, "detection threshold %u.%u x quiet", tw_detect_level(n) / 10, tw_detect_level(n) % 10);
    }
    if (ch & (1u << TW_K_DETECT_LOW)) { /* 3.5: its steadier-than-quiet threshold: kept */
        nvs_handle_t h;
        uint8_t v = tw_detect_low_level(n);
        if (nvs_open("tw", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "dlo", v);
            nvs_set_u8(h, "lgl", n->set_gen[TW_K_DETECT_LOW]);
            nvs_commit(h);
            nvs_close(h);
        }
        if (v) {
            ESP_LOGW(TAG, "steadier-than-quiet threshold 0.%u x quiet", v);
        } else {
            ESP_LOGW(TAG, "steadier-than-quiet detection off");
        }
    }
    if (ch & (1u << TW_K_RELEARN)) {    /* 3.4: tw_proto.c started it again when it took the action */
        ESP_LOGW(TAG, "learning the quiet level again (60 s)");
    }
}

/* 3.4: "TWSET detect V" (x10 of quiet, TW_DETECT_MIN..MAX) and "TWSET relearn" (3.5: and
   "TWSET detect low V", 0..TW_LOW_MAX, 0: off), for this board; with " board N gen G", given to
   board N (0: all) by record, as led */
static int csi_command(tw_node_t *n, char *cmd, char *msg, int max, int *after)
{
    if (strncmp(cmd, "TWSET detect ", 13) && strncmp(cmd, "TWSET relearn", 13)) {
        return 0;
    }
    char *bd = strstr(cmd, " board ");
    int k, v = 0, target = -1, gen = -1, key = 0, vlen = 0;
    uint8_t val[1] = {0};
    if (bd) {
        if (sscanf(bd, " board %d gen %d", &target, &gen) != 2 || target < 0 ||
            target > TW_MAX_NODES || gen < 1 || gen > 255) {
            target = -2;
        }
        *bd = 0;
    }
    if (sscanf(cmd, "TWSET detect low %d", &v) == 1 && v >= 0 && v <= TW_LOW_MAX) {
        key = TW_K_DETECT_LOW;          /* 3.5: steadier than quiet, x10 (0: off) */
        val[0] = (uint8_t)v;
        vlen = 1;
    } else if (sscanf(cmd, "TWSET detect %d", &v) == 1 && v >= TW_DETECT_MIN && v <= TW_DETECT_MAX) {
        key = TW_K_DETECT;
        val[0] = (uint8_t)v;
        vlen = 1;
    } else if (!strcmp(cmd, "TWSET relearn")) {
        key = TW_K_RELEARN;
    }
    if (key && target == -1) {
        tw_setting_local(n, (uint8_t)key, val, vlen);
        *after = 1;
        k = key == TW_K_DETECT ? snprintf(msg, (size_t)max, "TWOK detect %d.%d x quiet\n", v / 10, v % 10)
            : key == TW_K_DETECT_LOW ? (v ? snprintf(msg, (size_t)max, "TWOK detect low 0.%d x quiet\n", v)
                                          : snprintf(msg, (size_t)max, "TWOK detect low off\n"))
            : snprintf(msg, (size_t)max, "TWOK relearn: the quiet level again (60 s)\n");
    } else if (key && target >= 0) {
        tw_setting_give(n, (uint8_t)target, (uint8_t)key, val, vlen, (uint8_t)gen);
        *after = 1;
        char who[12];
        snprintf(who, sizeof(who), target ? "board %d" : "all boards", target);
        k = snprintf(msg, (size_t)max, "TWOK %s given to %s, generation %d\n",
                     key == TW_K_DETECT ? "detect" : key == TW_K_DETECT_LOW ? "detect low" : "relearn",
                     who, gen);
    } else {
        k = snprintf(msg, (size_t)max, "TWERR detect %d..%d, detect low 0..%d (x10 of quiet; 0: off), "
                     "relearn [board N gen G]\n", TW_DETECT_MIN, TW_DETECT_MAX, TW_LOW_MAX);
    }
    return k > 0 && k < max ? k : 0;
}

/* once a second: the color of the most moving link (tw_led.c), green for a link steadier than
   quiet (held, 3.5) when nothing moves, and a white flash first for a detection (3.4) */
static int csi_led(const tw_node_t *n, uint8_t bright, uint8_t c[3])
{
    tw_led_color(tw_led_pick(n->score_last), bright, c);
    if (!c[0] && !c[1] && !c[2] && n->low_bits) {
        c[1] = bright;
    }
    return n->det_bits != 0;
}

const tw_module_t tw_mod_csi = {
    .id = 1,
    .version = 1,
    .name = "csi",
    .senses_people = 1,
    .sense = &tw_csi_sense,
    .init = csi_init,
    .keys_changed = csi_keys_changed,
    .command = csi_command,
    .led = csi_led,
};
