/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0 */
/*
 * tw_module.h - a module, as the firmware's core sees it (docs/MODULES.md).
 *
 * Each module is an ESP-IDF component, modules/<name>/tw_mod_<name>/, built when its Kconfig
 * option CONFIG_TW_MOD_<NAME> is on; it defines one tw_module_t named tw_mod_<name>. The build
 * writes the table of the enabled ones (tw_modules.c, in the build folder: main/CMakeLists.txt),
 * and the core calls each through it: the core has no #ifdef for any module.
 *
 * Every hook is optional (NULL), and runs in the work task, the one that owns the protocol state
 * (tw_node_t): a module needs no locks for it.
 */
#pragma once
#include <stdint.h>
#include "tw_proto.h"

typedef struct tw_module {
    uint8_t     id, version;            /* module.yml */
    const char *name;                   /* "csi" */
    uint8_t     senses_people;          /* module.yml: shown wherever the board is */
    /* the CSI sensing module only: its portable part, given to the core (tw_node_t.sense) */
    const tw_sense_t *sense;
    /* at start, after the core loaded its own settings: the module's, from flash */
    void (*init)(tw_node_t *n);
    /* settings and actions taken (tw_node_t.set_changed bits, of every key): keep the module's */
    void (*keys_changed)(tw_node_t *n, uint32_t changed);
    /* a line from the computer (TWSET ...): the reply's length if it is the module's (the reply in
       reply, ending in a newline), 0 if not. after: set to 1 if its settings changed (the core
       then acts on them, as for its own) */
    int  (*command)(tw_node_t *n, char *cmd, char *reply, int max, int *after);
    /* the LED, once a second, when nothing of the core's shows: its color in c (0 0 0: none);
       returns 1 to show a short white flash first */
    int  (*led)(const tw_node_t *n, uint8_t bright, uint8_t c[3]);
} tw_module_t;

/* the enabled modules (tw_modules.c) */
extern const tw_module_t *const tw_modules[];
extern const int tw_modules_n;
