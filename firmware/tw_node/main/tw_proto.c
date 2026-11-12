/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0 */
/*
 * tw_proto.c - the two-witnesses many-board protocol, portable core.
 * See tw_proto.h for the design. No ESP-IDF in this file.
 */
#include "tw_proto.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


/* ---------------- small helpers ---------------- */
static int popcount16(uint16_t v)
{
    int c = 0;
    while (v) {
        v &= (uint16_t)(v - 1);
        c++;
    }
    return c;
}

static uint16_t bit_of(uint8_t id) { return (uint16_t)(1u << (id - 1)); }

static int valid_id(uint8_t id) { return id >= 1 && id <= TW_MAX_NODES; }
static tw_rec_t *up_status_record(tw_node_t *n, uint32_t now);    /* 4.0 */
static int up_need_count(const tw_node_t *n);
static uint32_t rd32be(const uint8_t *p);
static void up_report(tw_node_t *n, uint8_t s, const tw_rec_t *r);

uint32_t tw_rand(tw_node_t *n)
{
    n->rng = n->rng * 1664525u + 1013904223u;
    return n->rng >> 8;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------------- state ---------------- */
void tw_init(tw_node_t *n, uint8_t id, uint8_t is_exit, uint32_t seed)
{
    memset(n, 0, sizeof(*n));
    n->id = id;
    n->is_exit = is_exit;
    n->leader = id;
    n->synced = 1;              /* alone, a board is its own leader */
    n->rng = seed ? seed : 12345u + id;
    n->boot = (uint8_t)(tw_rand(n) & 0xff);
    n->access_us = TW_ACCESS_US;
}

uint16_t tw_active(const tw_node_t *n, uint32_t now)
{
    uint16_t a = bit_of(n->id);
    for (int i = 1; i <= TW_MAX_NODES; i++) {
        if (i != n->id && n->heard[i] && (uint32_t)(now - n->heard_us[i]) < TW_ACTIVE_US) {
            a |= bit_of((uint8_t)i);
        }
    }
    return a;
}

static uint8_t lowest(uint16_t active)
{
    for (int i = 1; i <= TW_MAX_NODES; i++) {
        if (active & bit_of((uint8_t)i)) {
            return (uint8_t)i;
        }
    }
    return 0;
}

static uint8_t highest(uint16_t active)
{
    for (int i = TW_MAX_NODES; i >= 1; i--) {
        if (active & bit_of((uint8_t)i)) {
            return (uint8_t)i;
        }
    }
    return 0;
}

/* the period covers every listed board's slot: 20 ms up to board 6, then 3 ms per board */
static uint8_t own_period_ms(uint16_t active)
{
    int p = (int)(highest(active) * TW_SLOT_US / 1000u);
    if (p < TW_MIN_PERIOD_MS) {
        p = TW_MIN_PERIOD_MS;
    }
    return (uint8_t)(p > 250 ? 250 : p);
}

static int lead_fresh(const tw_node_t *n, uint32_t now)
{
    return n->leader != n->id && n->n_cand > 0 && (uint32_t)(now - n->lead_heard_us) < TW_ACTIVE_US;
}

uint8_t tw_period_ms(const tw_node_t *n, uint32_t now)
{
    if (lead_fresh(n, now) && n->lead_period_ms) {
        return n->lead_period_ms;
    }
    return own_period_ms(tw_active(n, now));
}

void tw_update_roles(tw_node_t *n, uint32_t now)
{
    uint8_t new_leader = lowest(tw_active(n, now));
    if (new_leader != n->leader) {
        n->leader_changes++;
        n->n_cand = 0;
        n->cand_pos = 0;
        if (new_leader == n->id) {
            n->synced = 1;              /* keep our current clock: followers stay close */
        } else {
            n->synced = 0;              /* wait for the new leader's frames */
        }
        n->leader = new_leader;
    }
}

/* the network settings as the transport's keys (TW_K_POWER..TW_K_CAP) */
static void net_values(uint8_t power, uint8_t f, uint8_t chan, uint8_t v[TW_K_NET_N])
{
    v[5] = chan;                                    /* 3.2 */
    v[0] = power;
    v[1] = (f & TW_SET_RELAY_OFF) ? 0 : 1;
    v[2] = (f & TW_SET_SECOND_FORCE) ? 2 : (f & TW_SET_SECOND_OFF) ? 1 : 0;
    v[3] = (f & TW_SET_MEASURE_EXTRA) ? 1 : 0;
    v[4] = (uint8_t)(f >> TW_SET_CAP_SHIFT);
}

/* ... and back: the setting flags from the keys' values */
static uint8_t net_flags(const uint8_t v[TW_K_NET_N])
{
    return (uint8_t)((v[1] ? 0 : TW_SET_RELAY_OFF) |
                     (v[2] == 2 ? TW_SET_SECOND_FORCE : v[2] == 1 ? TW_SET_SECOND_OFF : 0) |
                     (v[3] ? TW_SET_MEASURE_EXTRA : 0) | ((v[4] & 15) << TW_SET_CAP_SHIFT));
}

static uint8_t net_seq(const uint8_t gen[TW_K_NET_N])
{
    unsigned sum = 0;
    for (int i = 0; i < TW_K_NET_N; i++) {
        sum += gen[i];
    }
    return (uint8_t)sum;
}

/* 3.0: the settings this board uses, from its network keys (after one was taken) */
static void net_apply(tw_node_t *n)
{
    uint8_t v[TW_K_NET_N], g[TW_K_NET_N];
    for (int i = 0; i < TW_K_NET_N; i++) {
        v[i] = n->set_val[TW_K_NET_FIRST + i][0];
        g[i] = n->set_gen[TW_K_NET_FIRST + i];
    }
    uint8_t power = (v[0] >= TW_POWER_MIN_Q && v[0] <= TW_POWER_MAX_Q) ? v[0] : n->set_power;
    uint8_t flags = net_flags(v);
    n->set_seq = net_seq(g);
    if (tw_chan_valid(v[5])) {
        n->set_channel = v[5];                      /* 3.2: tw_chan_step moves the radio, later */
    }
    if (power != n->set_power || flags != n->set_flags) {
        n->set_power = power;
        n->set_flags = flags;
        n->set_pending = 1;
        n->set_changes++;
    }
}

static void tw_setting_give_(tw_node_t *n, uint8_t target, uint8_t key, const uint8_t *val, int len, uint8_t gen);

static uint8_t gen_next(uint8_t g)
{
    return (uint8_t)(g + 1) ? (uint8_t)(g + 1) : 1u;     /* 0 is "never given" */
}

/* a setting given here: each key that changes, its next generation, for every board */
static void net_give(tw_node_t *n, uint8_t power, uint8_t flags, uint8_t chan)
{
    uint8_t v[TW_K_NET_N];
    net_values(power, flags, chan, v);
    for (int i = 0; i < TW_K_NET_N; i++) {
        uint8_t k = (uint8_t)(TW_K_NET_FIRST + i);
        if (!n->set_len[k] || n->set_val[k][0] != v[i]) {
            tw_setting_give_(n, TW_SET_ALL, k, &v[i], 1, gen_next(n->set_gen[k]));
        }
    }
}

void tw_net_init(tw_node_t *n, const uint8_t gen[TW_K_NET_N])
{
    uint8_t v[TW_K_NET_N];
    net_values(n->set_power, n->set_flags, n->set_channel, v);
    for (int i = 0; i < TW_K_NET_N; i++) {
        tw_setting_init(n, (uint8_t)(TW_K_NET_FIRST + i), &v[i], 1, gen ? gen[i] : 0);
    }
    n->set_seq = gen ? net_seq(gen) : 0;
}

int tw_set_power(tw_node_t *n, uint8_t power_q)
{
    if (power_q < TW_POWER_MIN_Q || power_q > TW_POWER_MAX_Q) {
        return -1;
    }
    net_give(n, power_q, n->set_flags, n->set_channel);
    return 0;
}

void tw_set_flags(tw_node_t *n, uint8_t mask, uint8_t value)
{
    /* a setting always carries a power: full, as before, if none was ever set */
    net_give(n, n->set_power ? n->set_power : TW_POWER_MAX_Q, (uint8_t)((n->set_flags & ~mask) | (value & mask)),
             n->set_channel);
}

/* ---- 3.2: the channel ---- */
int tw_chan_valid(uint8_t ch)
{
    return ch >= TW_CHAN_FIRST && ch <= TW_CHAN_LAST;
}

int tw_chan_partner(uint8_t ch)
{
    return ch <= 4 ? 1 : -1;                        /* 1-4: 4 above; 5-11: 4 below (11's: 7, as before) */
}

void tw_chan_init(tw_node_t *n, uint32_t now)
{
    n->chan_home = n->chan_target = n->set_channel;
    n->chan_wait = n->scan_on = 0;
    n->last_frame_us = now;                         /* alone for TW_CHAN_LOST_US before it looks */
    n->scan_next_us = now;
}

int tw_set_channel(tw_node_t *n, uint8_t ch)
{
    if (!tw_chan_valid(ch)) {
        return -1;
    }
    net_give(n, n->set_power ? n->set_power : TW_POWER_MAX_Q, n->set_flags, ch);
    return 0;
}

uint8_t tw_chan_step(tw_node_t *n, uint32_t now, int *quiet)
{
    *quiet = 0;
    if (!tw_chan_valid(n->chan_home)) {             /* not started (tw_chan_init): stay where it is */
        return n->set_channel;
    }
    if (n->scan_on) {
        if ((int32_t)(n->last_frame_us - n->scan_step_us) >= 0) {
            /* heard a board here: this is where the network is. Stay; its settings come within 5 s
               (and if this board's are newer, the network takes them and moves) */
            n->scan_on = 0;
            n->scan_found++;
            n->chan_home = n->chan_target = n->scan_ch;
            n->chan_wait = 0;
            n->chan_changed = 1;
            tw_score_relearn(n);                    /* 3.4: another channel: learn quiet again */
            if (n->set_gen[TW_K_CHANNEL] == 0) {
                /* 3.3: its own channel was never given (a new board, or one moved to this group): the
                   one it found is its channel. Else the network's never-given channel would not look
                   newer than its own, and it would go back to that 20 s later, and again */
                n->set_val[TW_K_CHANNEL][0] = n->scan_ch;
                n->set_len[TW_K_CHANNEL] = 1;
                n->set_channel = n->scan_ch;
                n->set_changed |= 1u << TW_K_CHANNEL;   /* kept in flash */
            }
            return n->chan_home;
        }
        if ((int32_t)(now - n->scan_step_us) >= (int32_t)(TW_CHAN_SCAN_MS * 1000u)) {
            uint8_t c = n->scan_ch;
            do {
                c++;
            } while (c == n->chan_home);
            if (c > TW_CHAN_LAST) {                 /* all tried: home a while, then again */
                n->scan_on = 0;
                n->scan_next_us = now + TW_CHAN_HOME_US;
                return n->chan_home;
            }
            n->scan_ch = c;
            n->scan_step_us = now;
        }
        *quiet = 1;
        return n->scan_ch;
    }
    /* a new channel taken: move to it TW_CHAN_SWITCH_US later (a newer one starts the wait again) */
    if (n->set_channel != n->chan_home) {
        if (!n->chan_wait || n->chan_target != n->set_channel) {
            n->chan_wait = 1;
            n->chan_target = n->set_channel;
            n->chan_at = now + TW_CHAN_SWITCH_US;
        } else if ((int32_t)(now - n->chan_at) >= 0) {
            n->chan_home = n->chan_target;
            n->chan_wait = 0;
            n->chan_changed = 1;
            n->chan_switches++;
            n->last_frame_us = now;                 /* the others come within a second; if not, look */
            tw_score_relearn(n);                    /* 3.4: another channel sees the room anew */
        }
    } else {
        n->chan_wait = 0;
    }
    /* nobody heard for a while: look for them */
    /* signed: a frame noted this turn can be stamped a moment after now (not "71 minutes ago") */
    if ((int32_t)(now - n->last_frame_us) >= (int32_t)TW_CHAN_LOST_US && (int32_t)(now - n->scan_next_us) >= 0 &&
        !n->chan_wait) {
        n->scan_on = 1;
        n->chan_scans++;
        n->scan_ch = TW_CHAN_FIRST == n->chan_home ? TW_CHAN_FIRST + 1 : TW_CHAN_FIRST;
        n->scan_step_us = now;
        *quiet = 1;
        return n->scan_ch;
    }
    return n->chan_home;
}

void tw_set_relay(tw_node_t *n, int on)
{
    tw_set_flags(n, TW_SET_RELAY_OFF, on ? 0 : TW_SET_RELAY_OFF);
}

void tw_note_frame(tw_node_t *n, const tw_hdr_t *h, uint32_t rx_local, uint32_t start_local)
{
    if (!valid_id(h->src) || h->src == n->id) {
        return;
    }
    n->n_rx_frames++;
    n->last_frame_us = rx_local;                    /* 3.2: someone is here (tw_chan_step) */
    uint8_t s = h->src;
    if (h->flags & TW_F_EXTRA) {
        /* an extra frame: its records are used (by the caller), nothing else here. Counted when it
           follows its own normal frame (same number), with the time between them: no clock
           comes into it */
        if (n->heard[s] && n->hear_seq[s] == h->seq) {
            uint32_t gap = rx_local - n->heard_us[s];
            int16_t g = (int16_t)(gap > 32767u ? 32767 : gap);
            if (g > n->c_xgap[s]) {
                n->c_xgap[s] = g;
            }
            if (n->c_xheard[s] < 0xffff) {
                n->c_xheard[s]++;
            }
        }
        return;
    }
    if (n->c_frames[s] < 0xffff) {
        n->c_frames[s]++;
    }
    if ((h->flags & TW_F_EXTRA_NEXT) && n->c_xnext[s] < 0xffff) {
        n->c_xnext[s]++;
    }
    /* hearing: a smoothed share of src's frames (its frame numbers tell how many were missed) */
    const float a = 1.0f / 32.0f;
    if (!n->heard[s] || (uint32_t)(rx_local - n->heard_us[s]) >= TW_ACTIVE_US) {
        n->hear_r[s] = 1.0f;                        /* (re)appeared: start from "all heard" */
    } else {
        uint32_t gap = h->seq - n->hear_seq[s];
        if (gap >= 1 && gap <= 1000) {
            n->hear_r[s] *= powf(1.0f - a, (float)(gap - 1));
            n->hear_r[s] = n->hear_r[s] * (1.0f - a) + a;
        }                                           /* else: src restarted; keep the estimate */
    }
    n->hear_seq[s] = h->seq;
    memcpy(n->peer_hear[s], h->hear, TW_MAX_NODES);
    n->peer_us[s] = rx_local;
    n->peer_flags[s] = h->flags;
    n->heard[h->src] = 1;
    n->heard_us[h->src] = rx_local;
    tw_update_roles(n, rx_local);
    if (h->src != n->leader) {
        return;
    }
    /* the leader's clock: its frame says "net_us" when it was built, and rx_local is when it had
       been received, i.e. its end: add its own time on the air (2.2: from its length; before, a
       fixed 150 us, so long frames left the followers up to ~700 us behind). It went on the air a
       little later than it was built (queueing, channel access), so each candidate is still an
       underestimate: keep the max */
    uint32_t cand = h->net_us - rx_local + tw_air_us(h->len);
    if (start_local && h->delay_us) {
        /* 2.3: the frame started on the air (the radio's own receive time) its sender's delay
           after its stamp: the delay it says (byte 18), to its send-done less the air, less the
           send-done's own lateness. Its least delay, so still an underestimate: keeping the max
           still works */
        uint32_t pre = h->delay_us > TW_POST_EST_US ? h->delay_us - TW_POST_EST_US : 0u;
        cand = h->net_us + pre - start_local;
    }
    n->cand[n->cand_pos] = cand;
    n->cand_pos = (uint8_t)((n->cand_pos + 1) % 16);
    if (n->n_cand < 16) {
        n->n_cand++;
    }
    int32_t best = 0;
    for (int i = 0; i < n->n_cand; i++) {
        int32_t d = (int32_t)(n->cand[i] - cand);
        if (d > best) {
            best = d;
        }
    }
    n->offset = cand + (uint32_t)best;
    n->synced = 1;
    n->lead_active = (uint16_t)(h->active | bit_of(h->src));
    n->lead_period_ms = h->period_ms;
    n->lead_heard_us = rx_local;
}

/* slot: fixed by the board number, so two boards can never pick the same one, whatever
   each has heard. Returns 0 if the slot does not fit in the period (a board numbered
   beyond what the leader lists): it must send at random until the leader hears it. */
static int slot_of(const tw_node_t *n, uint32_t now, uint32_t *slot_start, uint32_t *period_us)
{
    uint32_t p = (uint32_t)tw_period_ms(n, now) * 1000u;
    uint32_t start = (uint32_t)(n->id - 1) * TW_SLOT_US + TW_GUARD_US;
    *slot_start = start;
    *period_us = p;
    return start + TW_SLOT_US - TW_GUARD_US <= p;
}

uint32_t tw_next_tx_local(tw_node_t *n, uint32_t now)
{
    tw_update_roles(n, now);
    uint32_t start, p;
    int fits = slot_of(n, now, &start, &p);
    if (!n->synced || !fits) {
        /* not yet in step (or numbered beyond the leader's list): send now and then, at random,
           so the leader hears us and makes room; 60-140 ms apart */
        return now + 60000u + tw_rand(n) % 80000u;
    }
    uint32_t net = now + n->offset;
    uint32_t phase = net % p;
    uint32_t wait = (start + p - phase) % p;
    if (wait < 600) {
        wait += p;
    }
    return now + wait;
}

int32_t tw_slot_pos(const tw_node_t *n, uint32_t now, uint8_t id)
{
    uint32_t p = (uint32_t)tw_period_ms(n, now) * 1000u;
    uint32_t raw = ((uint32_t)(id - 1) * TW_SLOT_US) % p;
    uint32_t d = ((now + n->offset) % p + p - raw) % p;
    return d >= p / 2 ? (int32_t)d - (int32_t)p : (int32_t)d;
}

void tw_note_sent_normal(tw_node_t *n, uint32_t stamp, uint32_t done, int len)
{
    int32_t d = (int32_t)(done - stamp) - (int32_t)tw_air_us(len);
    if (d < 0 || d > 5100) {
        return;                         /* nonsense, or a stall: keep the last ones */
    }
    n->own_delay[n->delay_pos] = (uint16_t)d;
    n->delay_pos = (uint8_t)((n->delay_pos + 1) % 16);
    if (n->n_delay < 16) {
        n->n_delay++;
    }
}

void tw_note_sent(tw_node_t *n, uint32_t now, int extra)
{
    if (!extra) {
        return;
    }
    int32_t pos = tw_slot_pos(n, now, n->id);
    int q = (pos + 10) / 20;
    n->xend_q = (uint8_t)(q < 1 ? 1 : q > 255 ? 255 : q);
    n->xend_us = now;
}

uint32_t tw_air_us(int len)
{
    return TW_PREAMBLE_US + (uint32_t)(len + TW_MAC_BYTES) * 16u / 27u;    /* 13.5 Mbit/s */
}

void tw_clear_counts(tw_node_t *n)
{
    memset(n->c_frames, 0, sizeof(n->c_frames));
    memset(n->c_xnext, 0, sizeof(n->c_xnext));
    memset(n->c_xheard, 0, sizeof(n->c_xheard));
    memset(n->c_xcsi, 0, sizeof(n->c_xcsi));
    memset(n->c_xgap, 0, sizeof(n->c_xgap));
    memset(n->c_rxerr, 0, sizeof(n->c_rxerr));
}

/* ---------------- hearing and relaying ---------------- */
static int fresh(const tw_node_t *n, uint32_t now, uint8_t id)
{
    return n->heard[id] && (uint32_t)(now - n->heard_us[id]) < TW_ACTIVE_US;
}

uint8_t tw_hear_pct(const tw_node_t *n, uint32_t now, uint8_t id)
{
    if (id == n->id) {
        return 100;
    }
    if (!valid_id(id) || !fresh(n, now, id)) {
        return 0;
    }
    int p = (int)lrintf(n->hear_r[id] * 100.0f);
    return (uint8_t)(p < 0 ? 0 : p > 100 ? 100 : p);
}

uint8_t tw_primary_exit(const tw_node_t *n, uint32_t now)
{
    if (n->is_exit) {
        return 0;
    }
    for (int i = 1; i <= TW_MAX_NODES; i++) {
        if (i != n->id && fresh(n, now, (uint8_t)i) && (n->peer_flags[i] & TW_F_EXIT)) {
            return (uint8_t)i;
        }
    }
    return 0;
}

/* how well board a says it hears board b (this board's own view when a is this board) */
static int says_hears(const tw_node_t *n, uint32_t now, uint8_t a, uint8_t b)
{
    if (a == n->id) {
        return tw_hear_pct(n, now, b);
    }
    if (!fresh(n, now, a)) {
        return -1;
    }
    return n->peer_hear[a][b - 1];
}

int tw_relay_share(const tw_node_t *n, uint32_t now, uint8_t src, int *idx, int *of)
{
    if ((n->set_flags & TW_SET_RELAY_OFF) || !valid_id(src) || src == n->id) {
        return 0;
    }
    uint8_t ex = tw_primary_exit(n, now);
    if (!ex || src == ex) {
        return 0;
    }
    int e_src = says_hears(n, now, ex, src);
    if (e_src < 0 || e_src >= TW_RELAY_STRANDED) {
        return 0;                                   /* the exit hears it well enough */
    }
    /* candidates: not src, not an exit; hear src well and are heard well by the exit. The
       lowest-numbered TW_RELAY_MAX of them share the work: ranking by board number (not by how
       well) keeps every board's list the same while the percentages wobble */
    uint8_t cand[TW_RELAY_MAX];
    int nc = 0;
    for (int c = 1; c <= TW_MAX_NODES && nc < TW_RELAY_MAX; c++) {
        if (c == src || c == ex) {
            continue;
        }
        if (c != n->id && (!fresh(n, now, (uint8_t)c) || (n->peer_flags[c] & TW_F_EXIT))) {
            continue;
        }
        int c_src = says_hears(n, now, (uint8_t)c, src), e_c = says_hears(n, now, ex, (uint8_t)c);
        if (c_src < TW_RELAY_GOOD || e_c < TW_RELAY_GOOD) {
            continue;
        }
        cand[nc++] = (uint8_t)c;
    }
    for (int k = 0; k < nc; k++) {
        if (cand[k] == n->id) {
            *idx = k;
            *of = nc;
            return 1;
        }
    }
    return 0;
}

int tw_relay_record(tw_node_t *n, uint32_t now, uint8_t src, const tw_rec_t *r)
{
    int idx, of;
    if (n->is_exit || r->hop != 0 || r->rx != src) {
        return 0;
    }
    if (!tw_relay_share(n, now, src, &idx, &of)) {
        n->relay_for &= (uint16_t)~bit_of(src);
        return 0;
    }
    n->relay_for |= bit_of(src);
    if ((int)(r->recseq % (uint32_t)of) != idx || tw_seen(n, r->rx, r->boot, r->recseq)) {
        return 0;                                   /* another relay's share, or the second copy */
    }
    /* a free slot, else the oldest own record already sent once (its second copy ranks below
       relayed records anyway); never an own record that has not gone out yet */
    tw_qent_t *e = NULL, *old = NULL;
    for (int i = 0; i < TW_QUEUE_LEN; i++) {
        tw_qent_t *c = &n->q[i];
        if (!c->used) {
            e = c;
            break;
        }
        if (!c->relay && c->sent >= 1 && (!old || (int32_t)(c->made_us - old->made_us) < 0)) {
            old = c;
        }
    }
    if (!e) {
        e = old;
    }
    if (!e) {
        n->n_relay_full++;
        return 0;
    }
    e->rec = *r;
    e->rec.hop = 1;
    e->used = 1;
    e->sent = 0;
    e->relay = 1;
    e->made_us = now;
    return 1;
}

/* ---------------- frames ---------------- */
static int rec_len(const tw_rec_t *r) { return TW_REC_HDR_LEN + r->n; }

static void put_rec(uint8_t *p, const tw_rec_t *r)
{
    p[0] = r->rx; p[1] = r->tx; p[2] = r->hop; p[3] = r->kind;
    put32(p + 4, r->recseq);
    put32(p + 8, r->txseq);
    put32(p + 12, r->rx_us);
    put32(p + 16, r->net_us);
    p[20] = (uint8_t)r->rssi; p[21] = (uint8_t)r->noise; p[22] = r->agc; p[23] = (uint8_t)r->fft;
    p[24] = r->n; p[25] = r->step; p[26] = r->phy; p[27] = r->boot;
    memcpy(p + TW_REC_HDR_LEN, r->q, r->n);
}

static int cap_of(const tw_node_t *n)
{
    int c = n->set_flags >> TW_SET_CAP_SHIFT;
    return c ? c : 255;
}

/* the records a frame takes, in this order (pass): own never-sent records, relayed ones (sent
   once), own second copies. An extra frame does not take a copy of a record that went out in
   this slot's normal frame (a copy 1 ms later protects less than one a frame later), unless the
   test setting forces extra frames (then anything goes, to fill them) */
static int wants(const tw_node_t *n, const tw_qent_t *e, int pass, int extra, int force)
{
    static const uint8_t pass_relay[3] = {0, 1, 0}, pass_sent[3] = {0, 0, 1};
    if (!e->used || e->relay != pass_relay[pass] || e->sent != pass_sent[pass]) {
        return 0;
    }
    return !(extra && !force && e->sent && e->slot == n->slot_no);
}

/* put records from the queue into buf from pos, up to max bytes; returns the new end */
static int put_records(tw_node_t *n, uint8_t *buf, int pos, int max, int copies, int extra, int force,
                       int *count)
{
    uint8_t in_frame[TW_QUEUE_LEN];
    memset(in_frame, 0, sizeof(in_frame));
    int cap = cap_of(n);
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 2 && copies < 2) {
            break;
        }
        for (;;) {
            int pick = -1;
            for (int i = 0; i < TW_QUEUE_LEN; i++) {
                if (!in_frame[i] && wants(n, &n->q[i], pass, extra, force) &&
                        (pick < 0 || (int32_t)(n->q[i].made_us - n->q[pick].made_us) < 0)) {
                    pick = i;
                }
            }
            if (pick < 0 || pos + rec_len(&n->q[pick].rec) > max || *count >= cap || *count >= 255) {
                break;
            }
            put_rec(buf + pos, &n->q[pick].rec);
            pos += rec_len(&n->q[pick].rec);
            (*count)++;
            n->q[pick].sent++;
            n->q[pick].slot = n->slot_no;
            in_frame[pick] = 1;
            if (extra && n->x_npick < TW_MAX_RECS_X) {
                n->x_pick[n->x_npick] = (uint8_t)pick;
                n->x_pick_rx[n->x_npick] = n->q[pick].rec.rx;
                n->x_pick_seq[n->x_npick] = n->q[pick].rec.recseq;
                n->x_npick++;
            }
            if (n->q[pick].relay) {
                n->n_relayed_out++;
            }
        }
    }
    return pos;
}

/* records still waiting that an extra frame would take */
static int waiting(const tw_node_t *n, int copies)
{
    for (int i = 0; i < TW_QUEUE_LEN; i++) {
        for (int pass = 0; pass < (copies < 2 ? 2 : 3); pass++) {
            if (wants(n, &n->q[i], pass, 1, 0)) {
                return 1;
            }
        }
    }
    return 0;
}

static void put_header(tw_node_t *n, uint32_t now, uint8_t *buf, uint32_t seq, uint8_t more_flags, int count,
                       uint16_t act)
{
    buf[0] = TW_MAGIC0; buf[1] = TW_MAGIC1; buf[2] = TW_VERSION; buf[3] = n->id;
    put32(buf + 4, seq);
    put32(buf + 8, now + n->offset);
    buf[12] = n->leader;
    buf[13] = (uint8_t)((n->synced ? TW_F_SYNCED : 0) | (n->is_exit ? TW_F_EXIT : 0) |
                        (n->leader == n->id ? TW_F_LEADER : 0) | more_flags);
    put16(buf + 14, act);
    buf[16] = tw_period_ms(n, now);
    buf[17] = (uint8_t)count;
    buf[18] = 0;                        /* 2.3: our delay (0 until known), the least of our last 16 */
    if (n->n_delay) {
        uint16_t d = n->own_delay[0];
        for (int i = 1; i < n->n_delay; i++) {
            if (n->own_delay[i] < d) {
                d = n->own_delay[i];
            }
        }
        buf[18] = (uint8_t)(d / 20u > 255u ? 255u : d / 20u < 1u ? 1u : d / 20u);
    }
    buf[19] = n->group;                 /* 3.3: our group */
    memset(buf + 20, 0, 4);             /* 3.0: reserved, sent as 0, ignored */
    for (int i = 1; i <= TW_MAX_NODES; i++) {
        buf[23 + i] = tw_hear_pct(n, now, (uint8_t)i);
    }
}

int tw_build_frame(tw_node_t *n, uint32_t now, uint8_t *buf, int max)
{
    if (max > TW_MAX_PAYLOAD) {
        max = TW_MAX_PAYLOAD;
    }
    tw_update_roles(n, now);
    uint16_t act = tw_active(n, now);
    int copies = popcount16(act) <= TW_FULL_MAX_ACTIVE ? TW_COPIES : 1;     /* airtime budget */
    /* expire old records */
    for (int i = 0; i < TW_QUEUE_LEN; i++) {
        tw_qent_t *e = &n->q[i];
        int done = e->relay ? e->sent >= 1 : e->sent >= copies;
        if (e->used && (done || (uint32_t)(now - e->made_us) > TW_RECORD_TTL_US)) {
            if (e->sent == 0 && !e->relay) {
                n->n_dropped++;
            }
            e->used = 0;
        }
    }
    /* own never-sent records first (oldest first), then relayed ones (sent once), then own
       second copies; a record goes into a given frame at most once */
    n->slot_no++;
    if (n->xend_q && (uint32_t)(now - n->xend_us) > 1000000u) {
        n->xend_q = 0;                  /* no extra frame for a second: stop saying where the last ended */
    }
    int force = (n->set_flags & TW_SET_SECOND_FORCE) != 0, count = 0;
    int pos = put_records(n, buf, TW_HDR_LEN, max, copies, 0, force, &count);
    /* an extra frame after this one? Only in step, in our own slot, and not from an exit (it
       prints its records itself) unless forced */
    uint32_t start, p;
    int fits = slot_of(n, now, &start, &p);
    int allowed = n->synced && fits &&
                  (force || (!(n->set_flags & TW_SET_SECOND_OFF) && !n->is_exit));
    n->extra_planned = (uint8_t)(allowed && (force || waiting(n, copies)));
    if (force) {
        memset(buf + pos, 0, (size_t)(max - pos));      /* full length: test airtime */
        pos = max;
    }
    put_header(n, now, buf, n->seq++, n->extra_planned ? TW_F_EXTRA_NEXT : 0, count, act);
    n->n_tx++;
    return pos;
}

int tw_build_extra(tw_node_t *n, uint32_t now, uint8_t *buf, int max)
{
    if (!n->extra_planned) {
        return 0;
    }
    n->extra_planned = 0;
    if (max > TW_MAX_PAYLOAD) {
        max = TW_MAX_PAYLOAD;
    }
    /* it must end by our slot's end + TW_SLOT_SLACK_US (the next board's guard is 400 us) */
    int32_t at = tw_slot_pos(n, now, n->id);
    int32_t left = (int32_t)(TW_SLOT_US + TW_SLOT_SLACK_US) - at - (int32_t)(n->access_us + TW_PREAMBLE_US);
    int room = (at < 0 || at > (int32_t)TW_SLOT_US || left <= 0) ? 0 : (int)(left * 27 / 16) - TW_MAC_BYTES;
    if (room < max) {
        max = room;
    }
    if (max < TW_HDR_LEN + TW_REC_HDR_LEN) {
        n->n_extra_skipped++;
        return 0;
    }
    uint16_t act = tw_active(n, now);
    int copies = popcount16(act) <= TW_FULL_MAX_ACTIVE ? TW_COPIES : 1;
    int force = (n->set_flags & TW_SET_SECOND_FORCE) != 0, count = 0;
    n->x_npick = 0;
    int pos = put_records(n, buf, TW_HDR_LEN, max, copies, 1, force, &count);
    if (count == 0 && !force) {
        n->n_extra_skipped++;                           /* nothing waiting fitted in the time left */
        return 0;
    }
    if (force) {
        memset(buf + pos, 0, (size_t)(max - pos));
        pos = max;
    }
    put_header(n, now, buf, n->seq - 1, TW_F_EXTRA, count, act);   /* the same number as the frame before */
    n->n_extra_sent++;
    return pos;
}

void tw_unsend_extra(tw_node_t *n)
{
    for (int k = 0; k < n->x_npick; k++) {
        tw_qent_t *e = &n->q[n->x_pick[k]];
        /* only if the entry still holds that record (a relayed one may have taken its place) */
        if (e->used && e->sent > 0 && e->rec.rx == n->x_pick_rx[k] && e->rec.recseq == n->x_pick_seq[k]) {
            e->sent--;
            if (e->relay && n->n_relayed_out) {
                n->n_relayed_out--;
            }
        }
    }
    n->x_npick = 0;
    if (n->n_extra_sent) {
        n->n_extra_sent--;
    }
    n->n_extra_late++;
}

int tw_peek_header(const uint8_t *buf, int len, tw_hdr_t *h)
{
    if (len < TW_HDR_LEN || buf[0] != TW_MAGIC0 || buf[1] != TW_MAGIC1 || buf[2] != TW_VERSION) {
        return -1;
    }
    h->version = buf[2];
    h->src = buf[3];
    h->seq = get32(buf + 4);
    h->net_us = get32(buf + 8);
    h->leader = buf[12];
    h->flags = buf[13];
    h->active = get16(buf + 14);
    h->period_ms = buf[16];
    h->n_records = buf[17];
    h->delay_us = (uint16_t)(buf[18] * 20u);
    h->group = buf[19];                         /* 3.3 */
    memcpy(h->hear, buf + 24, TW_MAX_NODES);   /* bytes 19-23: reserved, ignored */
    h->len = (uint16_t)len;
    return valid_id(h->src) ? 0 : -1;
}

int tw_parse_frame(const uint8_t *buf, int len, tw_hdr_t *h, tw_rec_cb cb, void *ctx)
{
    if (tw_peek_header(buf, len, h) < 0) {
        return -1;
    }
    int pos = TW_HDR_LEN;
    tw_rec_t r;
    for (int i = 0; i < h->n_records; i++) {
        if (pos + TW_REC_HDR_LEN > len) {
            return -1;
        }
        const uint8_t *p = buf + pos;
        r.rx = p[0]; r.tx = p[1]; r.hop = p[2]; r.kind = p[3];
        r.recseq = get32(p + 4);
        r.txseq = get32(p + 8);
        r.rx_us = get32(p + 12);
        r.net_us = get32(p + 16);
        r.rssi = (int8_t)p[20]; r.noise = (int8_t)p[21]; r.agc = p[22]; r.fft = (int8_t)p[23];
        r.n = p[24]; r.step = p[25]; r.phy = p[26]; r.boot = p[27];
        if (r.n > TW_MAX_AMPS || pos + TW_REC_HDR_LEN + r.n > len || !valid_id(r.rx) || !valid_id(r.tx)) {
            return -1;
        }
        memcpy(r.q, p + TW_REC_HDR_LEN, r.n);
        pos += TW_REC_HDR_LEN + r.n;
        if (cb) {
            cb(ctx, &r);
        }
    }
    return h->n_records;
}

/* ---------------- measurements ---------------- */
int tw_lltf_ranges(uint8_t secondary, uint8_t sig_mode, uint8_t cwb, uint8_t stbc, int len, int ranges[4])
{
    (void)sig_mode; (void)cwb; (void)stbc;
    /* esp-radar csi_sub_carrier_table.c (ESP32/S2/S3/C3): with no secondary channel the L-LTF
       is bytes 76..128 then 2..54; with a secondary channel (above or below) 12..64 then 66..118 */
    if (len < 118) {
        return 0;
    }
    if (secondary == 0) {
        ranges[0] = 76; ranges[1] = 128; ranges[2] = 2; ranges[3] = 54;
    } else {
        ranges[0] = 12; ranges[1] = 64; ranges[2] = 66; ranges[3] = 118;
    }
    return 1;
}

/* a queue slot for a new own record: a free one, else the oldest (counted as dropped if never sent) */
static tw_qent_t *new_entry(tw_node_t *n)
{
    tw_qent_t *e = NULL, *oldest = NULL;
    for (int i = 0; i < TW_QUEUE_LEN; i++) {
        tw_qent_t *c = &n->q[(n->q_next + i) % TW_QUEUE_LEN];
        if (!c->used) {
            e = c;
            break;
        }
        if (!oldest || (int32_t)(c->made_us - oldest->made_us) < 0) {
            oldest = c;
        }
    }
    if (!e) {
        e = oldest;
        if (e->sent == 0) {
            n->n_dropped++;
        }
    }
    n->q_next = (uint16_t)(((e - n->q) + 1) % TW_QUEUE_LEN);
    return e;
}

void tw_set_info(tw_node_t *n, const char *text)
{
    int k = 0;
    for (; text[k] && k < TW_INFO_MAX; k++) {
        char c = text[k];
        n->info[k] = (c == ',' || c < 32 || c > 126) ? ' ' : c;
    }
    n->info[k] = 0;
    n->info_sent = 0;
}

/* queue a record that is not a measurement: its kind, and len bytes of content */
static tw_rec_t *queue_aux(tw_node_t *n, uint32_t now, uint8_t kind, const uint8_t *data, int len)
{
    tw_qent_t *e = new_entry(n);
    tw_rec_t *r = &e->rec;
    memset(r, 0, sizeof(*r));
    r->rx = n->id; r->tx = n->id; r->hop = 0; r->boot = n->boot;
    r->kind = kind;
    r->recseq = ++n->recseq;
    r->rx_us = now;
    r->net_us = now + n->offset;
    r->n = (uint8_t)len;
    memcpy(r->q, data, (size_t)len);
    e->used = 1;
    e->sent = 0;
    e->relay = 0;
    e->made_us = now;
    tw_seen(n, n->id, n->boot, r->recseq);
    return r;
}

/* ---------------- 2.5+: settings and commands by record ---------------- */
int tw_key_class(uint8_t key)
{
    switch (key) {
    case TW_K_LED_PIN:
    case TW_K_LED_BRIGHT:
    case TW_K_DETECT:
    case TW_K_DETECT_LOW:
        return TW_KC_BOARD;
    case TW_K_LED_TEST:
    case TW_K_LED_TRY:
    case TW_K_SURVEY:
    case TW_K_RELEARN:
        return TW_KC_ACTION;
    case TW_K_POWER:
    case TW_K_RELAY:
    case TW_K_SECOND:
    case TW_K_MEASURE:
    case TW_K_CAP:
    case TW_K_CHANNEL:
        return TW_KC_NETWORK;
    default:
        return 0;
    }
}

static int gen_newer(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) {                     /* 3.0: 0 is "never given" (tw_proto.h) */
        return a != 0 && b == 0;
    }
    return (int8_t)(uint8_t)(a - b) > 0;
}

static void setting_take(tw_node_t *n, uint8_t target, uint8_t key, const uint8_t *val, int len, uint8_t gen)
{
    if (!tw_key_class(key) || key >= TW_K_N || len < 0 || len > TW_CMD_VAL ||
        !(target == TW_SET_ALL || target == n->id)) {
        return;
    }
    int net = tw_key_class(key) == TW_KC_NETWORK;
    /* 3.0: a network key of the same generation and another value (given at two boards at once):
       the larger value wins, the same everywhere, so every board ends with the same */
    int tie = net && gen && gen == n->set_gen[key] && len >= 1 && n->set_len[key] >= 1 && val[0] > n->set_val[key][0];
    if (gen_newer(gen, n->set_gen[key]) || tie) {
        n->set_gen[key] = gen;
        memset(n->set_val[key], 0, TW_CMD_VAL);
        if (len) {
            memcpy(n->set_val[key], val, (size_t)len);
        }
        n->set_len[key] = (uint8_t)len;
        n->set_changed |= 1u << key;        /* the caller keeps it, acts on it */
        n->set_rep_sent = 0;                /* and it is reported within a second */
        if (net) {
            net_apply(n);                   /* 3.0: the settings it uses (set_pending if they changed) */
        }
        if (key == TW_K_RELEARN) {
            tw_score_relearn(n);            /* 3.4: here, so the firmware and the simulator do the same */
        }
    }
}

/* into the entries passed on: a newer generation replaces, the same keeps the longer time left
   (each board counts it down, so it ends) */
static void setting_keep(tw_node_t *n, uint8_t target, uint8_t key, const uint8_t *val, int len, uint8_t gen,
                         uint8_t ttl)
{
    int free_i = -1, low_i = 0;
    for (int i = 0; i < TW_SET_TABLE; i++) {
        if (n->set_tab[i].ttl && n->set_tab[i].target == target && n->set_tab[i].key == key) {
            if (gen_newer(gen, n->set_tab[i].gen)) {
                n->set_tab[i].gen = gen;
                n->set_tab[i].len = (uint8_t)len;
                if (len) {
                    memcpy(n->set_tab[i].val, val, (size_t)len);
                }
                n->set_tab[i].ttl = ttl;
            } else if (gen == n->set_tab[i].gen && ttl > n->set_tab[i].ttl) {
                n->set_tab[i].ttl = ttl;
            }
            return;
        }
        if (!n->set_tab[i].ttl && free_i < 0) {
            free_i = i;
        }
        if (n->set_tab[i].ttl < n->set_tab[low_i].ttl) {
            low_i = i;
        }
    }
    int i = free_i >= 0 ? free_i : low_i;       /* full: the one nearest its end makes room */
    n->set_tab[i].target = target;
    n->set_tab[i].key = key;
    n->set_tab[i].gen = gen;
    n->set_tab[i].len = (uint8_t)len;
    if (len) {
        memcpy(n->set_tab[i].val, val, (size_t)len);
    }
    n->set_tab[i].ttl = ttl;
}

void tw_setting_init(tw_node_t *n, uint8_t key, const uint8_t *val, int len, uint8_t gen)
{
    if (key && key < TW_K_N && len >= 0 && len <= TW_CMD_VAL) {
        memset(n->set_val[key], 0, TW_CMD_VAL);
        if (len) {
            memcpy(n->set_val[key], val, (size_t)len);
        }
        n->set_len[key] = (uint8_t)len;
        n->set_gen[key] = gen;
    }
}

void tw_setting_give(tw_node_t *n, uint8_t target, uint8_t key, const uint8_t *val, int len, uint8_t gen)
{
    tw_setting_give_(n, target, key, val, len, gen);
}

static void tw_setting_give_(tw_node_t *n, uint8_t target, uint8_t key, const uint8_t *val, int len, uint8_t gen)
{
    if (!tw_key_class(key) || target > TW_MAX_NODES || len < 0 || len > TW_CMD_VAL) {
        return;
    }
    setting_take(n, target, key, val, len, gen);
    if (target != n->id) {
        setting_keep(n, target, key, val, len, gen, TW_SET_TTL_S);
    }
    n->set_rep_sent = 0;                        /* report soon: the computer is waiting */
}

void tw_setting_local(tw_node_t *n, uint8_t key, const uint8_t *val, int len)
{
    if (key && key < TW_K_N) {
        setting_take(n, n->id, key, val, len, gen_next(n->set_gen[key]));
        n->set_rep_sent = 0;
    }
}

uint16_t tw_setting_u16(const tw_node_t *n, uint8_t key)
{
    return key < TW_K_N ? (uint16_t)(n->set_val[key][0] | (n->set_val[key][1] << 8)) : 0;
}

static void setting_entry(tw_node_t *n, uint8_t target, uint8_t key, uint8_t flags, uint8_t gen, uint8_t ttl,
                          const uint8_t *val, int len)
{
    if ((flags & TW_CF_REPORT) || ttl == 0 || !tw_key_class(key) || target > TW_MAX_NODES) {
        return;                                 /* a report, spent, or a key this firmware does not know */
    }
    setting_take(n, target, key, val, len, gen);
    if (target != n->id) {
        setting_keep(n, target, key, val, len, gen, ttl);
    }
}

void tw_setting_heard(tw_node_t *n, const tw_rec_t *r)
{
    uint8_t s = r->rx;
    if (s == n->id || !valid_id(s)) {
        return;
    }
    int kind = r->kind & TW_KIND_MASK;
    if (kind == TW_KIND_SCORE) {                /* 3.0: its uptime, and where its second frame ended */
        n->peer_up[s] = (uint16_t)r->txseq;
        if (r->n > TW_MAX_NODES) {
            n->c_xend_q[s] = r->q[TW_MAX_NODES];
        }
    } else if (kind == TW_KIND_UPDATE) {        /* 4.0: at the exit, while it sends */
        up_report(n, s, r);
    } else if (kind == TW_KIND_CMD) {
        for (int i = 0; i + TW_CMD_HDR <= r->n;) {
            const uint8_t *e = r->q + i;
            int len = e[5];
            if (len > TW_CMD_VAL || i + TW_CMD_HDR + len > r->n) {
                break;                          /* not a whole entry: stop here */
            }
            int k = e[1] - TW_K_NET_FIRST;
            if ((e[2] & TW_CF_STANDING) && k >= 0 && k < TW_K_NET_N && len >= 1) {
                n->peer_net[s][k] = e[TW_CMD_HDR];  /* 3.0: its own network key (for TWN) */
                n->peer_gen[s][k] = e[3];
                n->peer_has[s] |= (uint8_t)(1u << k);
            }
            setting_entry(n, e[0], e[1], e[2], e[3], e[4], e + TW_CMD_HDR, len);
            i += TW_CMD_HDR + len;
        }
    }
}

static int put_entry(uint8_t *q, int len, uint8_t target, uint8_t key, uint8_t flags, uint8_t gen, uint8_t ttl,
                     const uint8_t *val, int vlen)
{
    if (len + TW_CMD_HDR + vlen > TW_MAX_AMPS) {
        return len;                             /* no room: the rest next second */
    }
    uint8_t *e = q + len;
    e[0] = target; e[1] = key; e[2] = flags; e[3] = gen; e[4] = ttl; e[5] = (uint8_t)vlen;
    if (vlen) {
        memcpy(e + TW_CMD_HDR, val, (size_t)vlen);
    }
    return len + TW_CMD_HDR + vlen;
}

/* once a second: what is being passed on (each entry's time left counted down), and every
   TW_REPORT_EVERY_US this board's own keys, as one record */
static tw_rec_t *setting_record(tw_node_t *n, uint32_t now)
{
    uint8_t q[TW_MAX_AMPS];
    int len = 0;
    for (int i = 0; i < TW_SET_TABLE; i++) {
        if (!n->set_tab[i].ttl || !--n->set_tab[i].ttl) {
            continue;
        }
        len = put_entry(q, len, n->set_tab[i].target, n->set_tab[i].key, 0, n->set_tab[i].gen, n->set_tab[i].ttl,
                        n->set_tab[i].val, n->set_tab[i].len);
    }
    if (!n->set_rep_sent || (uint32_t)(now - n->set_rep_us) >= TW_REPORT_EVERY_US) {
        n->set_rep_sent = 1;
        n->set_rep_us = now;
        for (uint8_t k = 1; k < TW_K_N; k++) {
            int c = tw_key_class(k);
            if (c == TW_KC_BOARD) {
                len = put_entry(q, len, n->id, k, TW_CF_REPORT, n->set_gen[k], 0, n->set_val[k], n->set_len[k]);
            } else if (c == TW_KC_ACTION) {     /* an action: which generation it last ran */
                len = put_entry(q, len, n->id, k, TW_CF_REPORT, n->set_gen[k], 0, NULL, 0);
            } else if (c == TW_KC_NETWORK && n->set_len[k]) {
                /* 2.6: standing: said to every board, newest generation wins (and, from the
                   sender, its own value: the computer reads it as its report; 3.0: so marked) */
                len = put_entry(q, len, TW_SET_ALL, k, TW_CF_STANDING, n->set_gen[k], 1, n->set_val[k],
                                n->set_len[k]);
            }
        }
    }
    return len ? queue_aux(n, now, TW_KIND_CMD, q, len) : NULL;
}

/* one record into the score of board tx (a normal frame's measurement) */
static void score_add(tw_node_t *n, uint8_t tx, const tw_rec_t *r)
{
    tw_score_t *s = &n->score[tx];
    if (s->n != r->n) {                         /* first record, or its length changed: start over */
        memset(s, 0, sizeof(*s));
        s->n = r->n;
    }
    if (s->nb == 0) {                           /* learning which bins are live */
        for (int i = 0; i < r->n; i++) {
            s->bin[i] = (uint8_t)(s->bin[i] + (r->q[i] > 0));       /* counts, while learning */
        }
        if (++s->learn < TW_SCORE_LEARN) {
            return;
        }
        int k = 0;
        for (int i = 0; i < r->n; i++) {        /* the counts become the list of live bins (k <= i) */
            if (2 * s->bin[i] > TW_SCORE_LEARN) {
                s->bin[k++] = (uint8_t)i;
            }
        }
        s->nb = (uint8_t)(k / TW_SCORE_BAND);
        if (s->nb == 0) {
            s->n = 0;                           /* nothing live: try again */
        }
        return;
    }
    float bm[TW_SCORE_BANDS], level = 0;
    float step = r->step / 4.0f;
    for (int b = 0; b < s->nb; b++) {
        float sum = 0;
        for (int j = 0; j < TW_SCORE_BAND; j++) {
            sum += 20.0f * log10f(r->q[s->bin[b * TW_SCORE_BAND + j]] * step + 0.5f);
        }
        bm[b] = sum / TW_SCORE_BAND;
        level += bm[b];
    }
    level /= s->nb;
    for (int b = 0; b < s->nb; b++) {
        float v = bm[b] - level;                /* the record's own level out */
        s->s1[b] += v;
        s->s2[b] += v * v;
    }
    s->recs++;
}

/* close a second of board tx's score: x10 as a byte (0: none this second) */
static uint8_t score_close(tw_score_t *s, int hold_down)
{
    uint8_t out = 0;
    if (s->nb && s->recs >= 5) {
        float tot = 0;
        int started = s->start >= TW_SCORE_START;
        for (int b = 0; b < s->nb; b++) {
            float m = s->s1[b] / s->recs;
            float sd = sqrtf(fmaxf(s->s2[b] / s->recs - m * m, 0.0f));
            if (!started) {
                /* 3.4: the quiet level starts as a stepped median of 60 seconds: the first second's,
                   then a step toward each next one, large at first and smaller each second (30% down
                   to 4%). Only above or below counts, not by how much, so a few loud seconds (someone
                   about) move it little: with 10 of 60 seconds busy at 4x, within 7% (the mean of
                   30 seconds was 54% high, and took minutes to come down) */
                if (s->start == 0) {
                    s->ref[b] = sd;
                } else {
                    float a = 0.3f * 8.0f / (8.0f + (float)s->start);
                    s->ref[b] *= sd > s->ref[b] ? 1.0f + a : 1.0f / (1.0f + a);
                }
            } else {
                tot += sd / fmaxf(s->ref[b], 1e-3f);
                /* then a running median: a small step towards each new second (0.2%: a lasting
                   change becomes the usual in about 10-15 minutes, as the replay's settle) */
                /* 3.5: but not down while the link is marked steadier than quiet (hold_down) */
                if (sd > s->ref[b]) {
                    s->ref[b] *= 1.002f;
                } else if (!hold_down) {
                    s->ref[b] *= 1.0f / 1.002f;
                }
            }
        }
        if (!started) {
            s->start++;
        } else {
            int v = (int)lrintf(tot / s->nb * 10.0f);
            out = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
        }
    }
    memset(s->s1, 0, sizeof(s->s1));
    memset(s->s2, 0, sizeof(s->s2));
    s->recs = 0;
    return out;
}

void tw_score_relearn(tw_node_t *n)
{
    for (int i = 1; i <= TW_MAX_NODES; i++) {
        n->score[i].start = 0;              /* the live bins stay: the quiet level starts again */
        n->score[i].above = 0;
        n->score[i].below = 0;
        n->score[i].low_held = 0;
        n->score[i].none = 0;
        n->score[i].armed = 0;
        n->score[i].busy = 0;
        /* 4.2: and this second's sums so far: after a channel change they mix two channels, and
           would be the new quiet level's first value */
        memset(n->score[i].s1, 0, sizeof(n->score[i].s1));
        memset(n->score[i].s2, 0, sizeof(n->score[i].s2));
        n->score[i].recs = 0;
    }
}

uint8_t tw_detect_level(const tw_node_t *n)
{
    uint8_t v = n->set_len[TW_K_DETECT] ? n->set_val[TW_K_DETECT][0] : 0;
    return v >= TW_DETECT_MIN && v <= TW_DETECT_MAX ? v : TW_DETECT_DEFAULT;
}

uint8_t tw_detect_low_level(const tw_node_t *n)
{
    if (!n->set_len[TW_K_DETECT_LOW]) {
        return TW_LOW_DEFAULT;
    }
    uint8_t v = n->set_val[TW_K_DETECT_LOW][0];
    return v <= TW_LOW_MAX ? v : TW_LOW_DEFAULT;
}

const tw_rec_t *tw_aux_record(tw_node_t *n, uint32_t now)
{
    uint32_t sec = now / 1000000u;
    if (sec != n->score_sec) {                  /* once a second: close it, queue the scores */
        n->score_sec = sec;
        uint8_t sc[TW_MAX_NODES + 5];
        int any = 0;
        uint8_t level = tw_detect_level(n), low = tw_detect_low_level(n);
        uint16_t bits = 0, lbits = 0;
        for (int i = 1; i <= TW_MAX_NODES; i++) {
            tw_score_t *t = &n->score[i];
            int held = low && t->below >= TW_LOW_HOLD && t->low_held < TW_LOW_KEEP;
            sc[i - 1] = i == n->id ? 0 : score_close(t, held);
            if (sc[i - 1]) {
                t->none = 0;
            } else if (t->none < 255) {
                t->none++;                          /* 4.2: seconds in a row without a score */
            }
            if (sc[i - 1] >= TW_LOW_ARM_MIN && sc[i - 1] <= TW_LOW_ARM_MAX && t->armed < TW_LOW_ARM_S) {
                t->armed++;                         /* 4.2: seen near quiet */
            }
            /* 4.2: a busy start: never seen at quiet, and low TW_LOW_HOLD seconds in a row: this link
               learns its quiet level again. 4.3: by its own level, TW_BUSY_LOW, always on */
            if (t->armed >= TW_LOW_ARM_S || t->none >= TW_LOW_GAP || (sc[i - 1] && sc[i - 1] > TW_BUSY_LOW)) {
                t->busy = 0;
            } else if (sc[i - 1] && ++t->busy >= TW_LOW_HOLD) {
                t->start = 0;
                t->busy = 0;
                t->below = 0;
                t->armed = 0;                       /* (the new quiet level earns it afresh) */
                n->low_relearned++;
            }
            /* 3.5: steadier than quiet: at or below the low threshold TW_LOW_HOLD seconds in a row;
               a second without a score leaves it as it was. 4.2: armed links only */
            if (!low || t->armed < TW_LOW_ARM_S || t->none >= TW_LOW_GAP || (sc[i - 1] && sc[i - 1] > low)) {
                t->below = 0;
                t->low_held = 0;
            } else if (sc[i - 1]) {
                if (t->below < 255) {
                    t->below++;
                }
                if (t->below >= TW_LOW_HOLD && t->low_held < TW_LOW_KEEP) {
                    if (t->below == TW_LOW_HOLD) {
                        n->low_count++;             /* one more began */
                    }
                    if (++t->low_held == TW_LOW_KEEP) {
                        n->low_accepted++;          /* held long enough: the new quiet */
                    }
                }
            }
            if (t->below >= TW_LOW_HOLD && t->low_held > 0 && t->low_held < TW_LOW_KEEP) {
                lbits |= (uint16_t)(1u << (i - 1));
            }
            any |= sc[i - 1];
            /* 3.4: a detection: at or above the threshold TW_DETECT_HOLD seconds in a row */
            tw_score_t *s = &n->score[i];
            if (sc[i - 1] && sc[i - 1] >= level) {
                if (s->above < 255) {
                    s->above++;
                }
                if (s->above >= TW_DETECT_HOLD) {
                    bits |= (uint16_t)(1u << (i - 1));
                    if (s->above == TW_DETECT_HOLD) {
                        n->det_count++;             /* one more began */
                    }
                }
            } else if (sc[i - 1] || s->none >= TW_DETECT_GAP) {
                s->above = 0;                       /* 4.2: not for one second without a score */
            }
        }
        memcpy(n->score_last, sc, TW_MAX_NODES);
        n->det_bits = bits;
        /* 2.7: and where its last second frame ended (as header byte 23, which goes in 3.0) */
        sc[TW_MAX_NODES] = n->xend_q;
        sc[TW_MAX_NODES + 1] = (uint8_t)bits;   /* 3.4: this second's detections */
        sc[TW_MAX_NODES + 2] = (uint8_t)(bits >> 8);
        n->low_bits = lbits;
        sc[TW_MAX_NODES + 3] = (uint8_t)lbits;  /* 3.5: this second's steadier-than-quiet marks */
        sc[TW_MAX_NODES + 4] = (uint8_t)(lbits >> 8);
        any |= n->xend_q;
        if (any) {
            tw_rec_t *r = queue_aux(n, now, TW_KIND_SCORE, sc, TW_MAX_NODES + 5);
            r->txseq = sec;                     /* this board's uptime, s */
            return r;
        }
    }
    if (sec != n->set_sec) {                    /* 2.5: once a second, the settings passed on */
        n->set_sec = sec;
        tw_rec_t *r = setting_record(n, now);
        if (r) {
            return r;
        }
    }
    if (n->up_state && sec != n->up_rep_sec && !tw_up_hush(n, now) &&
        (n->up_id || n->up_state >= TW_UP_ST_FAIL)) {  /* 4.0: its part in an update, once a second */
        n->up_rep_sec = sec;
        return up_status_record(n, now);
    }
    if (n->survey_len && (uint32_t)(now - n->survey_us) < TW_SURVEY_SEND_US &&
        (uint32_t)(now - n->survey_sent_us) >= TW_INFO_EVERY_US) {    /* 3.1: the survey, every 5 s */
        n->survey_sent_us = now;
        tw_rec_t *r = queue_aux(n, now, TW_KIND_SURVEY, n->survey, n->survey_len);
        r->txseq = n->survey_gen;
        return r;
    }
    if (n->info[0] && (!n->info_sent || (uint32_t)(now - n->info_us) >= TW_INFO_EVERY_US)) {
        n->info_sent = 1;
        n->info_us = now;
        return queue_aux(n, now, TW_KIND_INFO, (const uint8_t *)n->info, (int)strlen(n->info));
    }
    return NULL;
}

/* ---------------- 4.0: updates over the air ---------------- */
static int up_bit(const uint8_t *bits, unsigned i) { return (bits[i >> 3] >> (i & 7)) & 1; }
static void up_set(uint8_t *bits, unsigned i) { bits[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static void up_clr(uint8_t *bits, unsigned i) { bits[i >> 3] &= (uint8_t)~(1u << (i & 7)); }
static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int up_chunk_len(const tw_node_t *n, unsigned idx)
{
    uint32_t off = (uint32_t)idx * TW_UP_CHUNK;
    return off >= n->up_size ? 0 : (int)(n->up_size - off < TW_UP_CHUNK ? n->up_size - off : TW_UP_CHUNK);
}

static void up_drop(tw_node_t *n)
{
    if (n->up_buf) {
        tw_up_free(n->up_buf);
        n->up_buf = NULL;
    }
    n->up_state = TW_UP_ST_NONE;
    n->up_tx_on = 0;
    n->up_burst = 0;
    n->up_have = 0;
    n->up_man_ok = 0;
}

/* a new update to receive (by radio, or from the computer on the exit): memory, nothing yet */
static int up_start(tw_node_t *n, uint32_t id, uint32_t size, uint8_t target, uint32_t now)
{
    up_drop(n);
    n->up_id = id;
    n->up_size = size;
    n->up_total = (uint16_t)((size + TW_UP_CHUNK - 1) / TW_UP_CHUNK);
    n->up_target = target;
    memset(n->up_bits, 0, sizeof(n->up_bits));
    n->up_last_us = now;
    n->up_state_us = now;
    n->up_buf = size && size <= TW_UP_MAX ? tw_up_alloc(size) : NULL;
    n->up_state = n->up_buf ? TW_UP_ST_RECEIVING : TW_UP_ST_NOMEM;
    if (!n->up_buf) {
        n->up_failed_id = id;
    }
    return n->up_buf ? 0 : -1;
}

static void up_take(tw_node_t *n, unsigned idx, const uint8_t *data, int len, uint32_t now)
{
    if (idx == TW_UP_MANIFEST) {
        if (len >= TW_UP_MAN_LEN && !n->up_man_ok) {
            memcpy(n->up_man, data, TW_UP_MAN_LEN);
            n->up_man_ok = 1;
        }
    } else if (idx < n->up_total && !up_bit(n->up_bits, idx)) {
        int want = up_chunk_len(n, idx);
        if (len >= want) {
            memcpy(n->up_buf + (uint32_t)idx * TW_UP_CHUNK, data, (size_t)want);
            up_set(n->up_bits, idx);
            n->up_have++;
        }
    }
    if (n->up_id && n->up_have == n->up_total && n->up_man_ok && n->up_state == TW_UP_ST_RECEIVING) {
        n->up_state = TW_UP_ST_COMPLETE;        /* the firmware takes it from here */
        n->up_state_us = now;
    }
}

int tw_up_rx(tw_node_t *n, const uint8_t *d, int len, uint32_t now)
{
    if (len < TW_UP_HDR || d[0] != TW_MAGIC0 || d[1] != TW_UP_MAGIC1) {
        return 0;
    }
    if (d[2] != TW_VERSION || d[3] != n->group || d[4] == n->id || n->up_tx_on) {
        return 1;                               /* another group's (or our own echo): nothing to do */
    }
    n->up_hush_on = 1;                          /* the group's air is the update's for now */
    n->up_hush_until = now + TW_UP_HUSH_US;
    uint8_t target = d[5];
    if (target && target != n->id) {
        return 1;
    }
    uint32_t id = rd32be(d + 6), size = (uint32_t)d[10] | (uint32_t)d[11] << 8 | (uint32_t)d[12] << 16;
    unsigned idx = (unsigned)d[13] | (unsigned)d[14] << 8;
    if (id && id == n->up_done_id) {            /* we run it already (restarted into it) */
        if (n->up_state == TW_UP_ST_NONE) {
            n->up_id = id;
            n->up_size = size;
            n->up_total = n->up_have = (uint16_t)((size + TW_UP_CHUNK - 1) / TW_UP_CHUNK);
            n->up_man_ok = 1;
            memset(n->up_bits, 0xff, sizeof(n->up_bits));
            n->up_state = TW_UP_ST_READY;
            n->up_state_us = now;
        }
        return 1;
    }
    if (id != n->up_id || n->up_state == TW_UP_ST_NONE) {
        if (n->up_state == TW_UP_ST_COMPLETE || n->up_state == TW_UP_ST_INSTALLING ||
            n->up_state == TW_UP_ST_READY || n->up_state == TW_UP_ST_STAGED || id == n->up_failed_id) {
            return 1;                           /* busy with one, or this one failed here already */
        }
        up_start(n, id, size, target, now);
    }
    if (n->up_state != TW_UP_ST_RECEIVING || size != n->up_size) {
        return 1;
    }
    n->up_last_us = now;
    up_take(n, idx, d + TW_UP_HDR, len - TW_UP_HDR, now);
    return 1;
}

int tw_up_hush(const tw_node_t *n, uint32_t now)
{
    return n->up_burst || (n->up_hush_on && (int32_t)(n->up_hush_until - now) > 0);
}

int tw_up_begin(tw_node_t *n, uint32_t size, uint32_t now)
{
    if (n->up_tx_on || n->up_state == TW_UP_ST_COMPLETE || n->up_state == TW_UP_ST_INSTALLING ||
        n->up_state == TW_UP_ST_READY || !size || size > TW_UP_MAX) {
        return -1;
    }
    n->up_failed_id = 0;
    return up_start(n, 0, size, 0, now);   /* (4.0 gave it a stale time: the update was dropped as idle
                                               at the first tick after its manifest) */
}

int tw_up_chunk(tw_node_t *n, uint16_t idx, const uint8_t *data, int len)
{
    if (n->up_state != TW_UP_ST_RECEIVING || n->up_id || idx >= n->up_total || len != up_chunk_len(n, idx)) {
        return -1;
    }
    up_take(n, idx, data, len, n->up_last_us);
    return 0;
}

int tw_up_manifest(tw_node_t *n, const uint8_t *man, uint32_t now)
{
    if (n->up_state != TW_UP_ST_RECEIVING || n->up_id) {
        return -1;
    }
    memcpy(n->up_man, man, TW_UP_MAN_LEN);
    n->up_man_ok = 1;
    n->up_id = rd32be(man);
    n->up_last_us = now;                        /* from now it counts as idle (TW_UP_IDLE_US) */
    return n->up_have == n->up_total;
}

int tw_up_send(tw_node_t *n, uint32_t now, uint8_t target)
{
    if (n->up_state != TW_UP_ST_STAGED || (target && !valid_id(target))) {
        return -1;
    }
    n->up_target = target;
    n->up_self = !target || target == n->id;
    n->up_start_us = now;
    n->up_state_us = now;
    if (target == n->id) {                      /* only this board: no radio */
        n->up_state = TW_UP_ST_COMPLETE;
        return 0;
    }
    memset(n->up_need, 0, sizeof(n->up_need));
    for (unsigned i = 0; i < n->up_total; i++) {
        up_set(n->up_need, i);
    }
    memset(n->up_rep, 0, sizeof(n->up_rep));
    memset(n->up_rep_have, 0, sizeof(n->up_rep_have));
    n->up_cursor = 0;
    n->up_burst = 0;
    n->up_first_pass = 1;
    n->up_next_us = now;
    n->up_frames = n->up_bursts = 0;
    n->up_tx_on = 1;
    n->up_state = TW_UP_ST_SENDING;
    return 0;
}

void tw_up_cancel(tw_node_t *n)
{
    up_drop(n);
}

static int up_need_count(const tw_node_t *n)
{
    int c = 0;
    for (unsigned i = 0; n->up_tx_on && i < n->up_total; i++) {
        c += up_bit(n->up_need, i);
    }
    return c;
}

static int up_need_next(const tw_node_t *n, unsigned from)
{
    for (unsigned i = from; i < n->up_total; i++) {
        if (up_bit(n->up_need, i)) {
            return (int)i;
        }
    }
    return -1;
}

static int up_build(const tw_node_t *n, unsigned idx, uint8_t *b, int max)
{
    int dl = idx == TW_UP_MANIFEST ? TW_UP_MAN_LEN : up_chunk_len(n, idx);
    if (TW_UP_HDR + dl > max) {
        return 0;
    }
    b[0] = TW_MAGIC0;
    b[1] = TW_UP_MAGIC1;
    b[2] = TW_VERSION;
    b[3] = n->group;
    b[4] = n->id;
    b[5] = n->up_target;
    wr32be(b + 6, n->up_id);
    b[10] = (uint8_t)n->up_size;
    b[11] = (uint8_t)(n->up_size >> 8);
    b[12] = (uint8_t)(n->up_size >> 16);
    b[13] = (uint8_t)idx;
    b[14] = (uint8_t)(idx >> 8);
    b[15] = 0;
    memcpy(b + TW_UP_HDR, idx == TW_UP_MANIFEST ? n->up_man : n->up_buf + (uint32_t)idx * TW_UP_CHUNK, (size_t)dl);
    return TW_UP_HDR + dl;
}

int tw_up_frame(tw_node_t *n, uint32_t now, uint8_t *buf, int max, uint32_t *next)
{
    if (!n->up_tx_on) {
        *next = now + 1000000u;
        return 0;
    }
    if ((int32_t)(now - n->up_next_us) < 0) {
        *next = n->up_next_us;
        return 0;
    }
    if (!n->up_burst) {
        if (up_need_next(n, 0) < 0) {           /* nothing lacking that we know of: wait for reports */
            n->up_next_us = now + 200000u;
            *next = n->up_next_us;
            return 0;
        }
        n->up_burst = 1;                        /* a burst: the manifest first, then what is lacking */
        n->up_burst_end = now + TW_UP_BURST_US;
        n->up_cursor = 0;
        n->up_bursts++;
        n->up_frames++;
        n->up_next_us = now + TW_UP_PACE_US;
        *next = n->up_next_us;
        return up_build(n, TW_UP_MANIFEST, buf, max);
    }
    int idx = up_need_next(n, n->up_cursor);
    if (idx < 0 || (int32_t)(now - n->up_burst_end) >= 0) {
        n->up_burst = 0;                        /* the burst ends: the network's turn */
        if (idx < 0 && n->up_first_pass && up_need_next(n, 0) < 0) {
            n->up_first_pass = 0;
            n->up_pass_us = now;
        }
        n->up_next_us = now + TW_UP_GAP_US;
        *next = n->up_next_us;
        return 0;
    }
    up_clr(n->up_need, (unsigned)idx);
    n->up_cursor = (uint16_t)(idx + 1);
    n->up_frames++;
    n->up_next_us = now + TW_UP_PACE_US;
    *next = n->up_next_us;
    return up_build(n, (unsigned)idx, buf, max);
}

/* a board's report (TW_KIND_UPDATE), at the exit while it sends: what it lacks is sent again */
static void up_report(tw_node_t *n, uint8_t s, const tw_rec_t *r)
{
    if (!n->up_tx_on || r->n < 10 || rd32be(r->q) != n->up_id || (n->up_target && s != n->up_target)) {
        return;
    }
    n->up_rep[s] = r->q[4];
    n->up_rep_have[s] = (uint16_t)(r->q[5] | r->q[6] << 8);
    n->up_rep_us[s] = n->up_last_us;
    if (r->q[4] != TW_UP_ST_RECEIVING) {
        return;
    }
    for (unsigned i = 0; i < n->up_total; i++) {
        unsigned byte = 10 + (i >> 3);
        if (byte >= r->n || !((r->q[byte] >> (i & 7)) & 1)) {
            up_set(n->up_need, i);
        }
    }
}

void tw_up_tick(tw_node_t *n, uint32_t now)
{
    if (n->up_hush_on && (int32_t)(n->up_hush_until - now) <= 0) {
        n->up_hush_on = 0;
    }
    if (n->up_state == TW_UP_ST_RECEIVING && n->up_id && !n->up_tx_on &&
        (uint32_t)(now - n->up_last_us) > TW_UP_IDLE_US) {
        up_drop(n);                             /* the sender went quiet: dropped */
    }
    if ((n->up_state >= TW_UP_ST_FAIL || (n->up_state == TW_UP_ST_READY && !n->up_buf)) &&
        (uint32_t)(now - n->up_state_us) > TW_UP_FAIL_SEND_US) {
        up_drop(n);                             /* said long enough (up_failed_id, up_done_id keep it) */
    }
    if (!n->up_tx_on) {
        return;
    }
    n->up_last_us = now;                        /* (the exit's clock for the reports' times) */
    int done = !n->up_first_pass;
    for (int b = 1; b <= TW_MAX_NODES; b++) {
        if (b == n->id || (n->up_target && b != n->up_target)) {
            continue;
        }
        uint8_t st = n->up_rep[b];
        int active = n->heard[b] && (uint32_t)(now - n->heard_us[b]) < TW_ACTIVE_US;
        if ((st == TW_UP_ST_RECEIVING || st == TW_UP_ST_COMPLETE || st == TW_UP_ST_INSTALLING) &&
            (uint32_t)(now - n->up_rep_us[b]) > TW_UP_STALE_US) {
            if (active) {                       /* restarted: everything again */
                st = n->up_rep[b] = TW_UP_ST_NONE;
                n->up_rep_us[b] = now;          /* (its wait starts again) */
                for (unsigned i = 0; i < n->up_total; i++) {
                    up_set(n->up_need, i);
                }
            } else {
                st = n->up_rep[b] = TW_UP_ST_SILENT;    /* gone (off, out of reach) */
            }
        }
        if (!done) {
            continue;
        }
        if (st == TW_UP_ST_NONE) {
            uint32_t since = now - n->up_pass_us;       /* waiting for its first report since */
            if (n->up_rep_us[b] && (uint32_t)(now - n->up_rep_us[b]) < since) {
                since = now - n->up_rep_us[b];
            }
            if (n->up_target) {
                done = since > TW_UP_WAIT_US;           /* the one asked for: off */
                if (done) {
                    n->up_rep[b] = TW_UP_ST_SILENT;
                }
            } else if (active) {
                if (since > TW_UP_WAIT_US) {
                    n->up_rep[b] = TW_UP_ST_SILENT;     /* active, but takes no part */
                } else {
                    done = 0;
                }
            }
        } else if (st != TW_UP_ST_READY && st < TW_UP_ST_FAIL) {
            done = 0;
        }
    }
    if (done || (uint32_t)(now - n->up_start_us) > TW_UP_GIVE_UP_US) {
        n->up_tx_on = 0;
        n->up_burst = 0;
        if (n->up_self) {
            n->up_state = TW_UP_ST_COMPLETE;    /* its own turn: the firmware checks and writes it */
        } else {
            up_drop(n);
        }
        n->up_state_us = now;
    }
}

void tw_up_set_state(tw_node_t *n, uint8_t state, uint32_t now)
{
    n->up_state = state;
    n->up_state_us = now;
    if (state >= TW_UP_ST_FAIL) {
        n->up_failed_id = n->up_id;
        if (n->up_buf) {
            tw_up_free(n->up_buf);
            n->up_buf = NULL;
        }
    }
}

static tw_rec_t *up_status_record(tw_node_t *n, uint32_t now)
{
    uint8_t q[10 + TW_UP_BITS];
    int nb = (n->up_total + 7) / 8;
    if (10 + nb > TW_MAX_AMPS) {
        nb = TW_MAX_AMPS - 10;
    }
    wr32be(q, n->up_id);
    q[4] = n->up_state;
    q[5] = (uint8_t)n->up_have;
    q[6] = (uint8_t)(n->up_have >> 8);
    q[7] = (uint8_t)n->up_total;
    q[8] = (uint8_t)(n->up_total >> 8);
    q[9] = n->up_man_ok;
    memcpy(q + 10, n->up_bits, (size_t)nb);
    return queue_aux(n, now, TW_KIND_UPDATE, q, 10 + nb);
}

int tw_frame_ours(tw_node_t *n, const tw_hdr_t *h)
{
    if (h->group == n->group) {
        return 1;
    }
    n->n_other_group++;
    n->other_group_last = h->group;
    return 0;
}

void tw_survey_done(tw_node_t *n, uint32_t now, uint8_t gen, const uint8_t *entries, int count)
{
    if (count < 0 || count > TW_SURVEY_N) {
        count = count < 0 ? 0 : TW_SURVEY_N;
    }
    n->survey[0] = TW_SURVEY_FIRST;
    n->survey[1] = (uint8_t)count;
    memcpy(n->survey + 2, entries, (size_t)count * TW_SURVEY_ENTRY);
    n->survey_len = (uint8_t)(2 + count * TW_SURVEY_ENTRY);
    n->survey_gen = gen;
    n->survey_us = now;
    n->survey_sent_us = now - TW_INFO_EVERY_US;     /* the first goes with the next record */
}

const tw_rec_t *tw_add_measurement(tw_node_t *n, uint32_t now, uint8_t tx, uint32_t txseq,
                                   uint32_t rx_us, int8_t rssi, int8_t noise, uint8_t agc, int8_t fft,
                                   uint8_t phy, uint8_t kflags, const int8_t *csi, int csi_len, float gain,
                                   int compact)
{
    if (kflags & TW_KIND_EXTRA) {               /* measured from an extra frame */
        /* counted, like c_xheard, when it follows its own normal frame */
        if (valid_id(tx) && n->heard[tx] && n->hear_seq[tx] == txseq && n->c_xcsi[tx] < 0xffff) {
            n->c_xcsi[tx]++;
        }
        if (!(n->set_flags & TW_SET_MEASURE_EXTRA)) {
            return NULL;                            /* counted only: one measurement per period */
        }
    }
    if ((kflags & TW_KIND_RXERR) && valid_id(tx) && n->c_rxerr[tx] < 0xffff) {
        n->c_rxerr[tx]++;
    }
    int fwi = (kflags & TW_KIND_FWI) != 0;
    float amp[TW_MAX_AMPS];
    int cnt = 0;
    int ranges[4];
    if (compact && tw_lltf_ranges((uint8_t)((phy >> 5) & 3), (uint8_t)((phy >> 3) & 3), (uint8_t)(phy & 3),
                                  (uint8_t)((phy >> 2) & 1), csi_len, ranges)) {
        for (int k = 0; k < 2; k++) {
            for (int b = ranges[2 * k]; b + 1 < ranges[2 * k + 1] && cnt < TW_COMPACT_AMPS; b += 2) {
                float i = csi[b], q = csi[b + 1];
                amp[cnt++] = sqrtf(i * i + q * q) * gain;
            }
        }
    } else {
        compact = 0;
        int bins = csi_len / 2;
        if (bins > TW_MAX_AMPS) {
            bins = TW_MAX_AMPS;
        }
        for (int b = 0; b < bins; b++) {
            float i = csi[2 * b], q = csi[2 * b + 1];
            amp[cnt++] = sqrtf(i * i + q * q) * gain;
        }
        if (fwi && cnt >= 2) {
            amp[0] = amp[1] = 0;            /* first 4 bytes invalid */
        }
    }
    tw_qent_t *e = new_entry(n);
    tw_rec_t *r = &e->rec;
    float mx = 0;
    for (int i = 0; i < cnt; i++) {
        if (amp[i] > mx) {
            mx = amp[i];
        }
    }
    int step = (int)ceilf(mx * 4.0f / 255.0f);
    if (step < 1) {
        step = 1;
    }
    if (step > 255) {
        step = 255;
    }
    r->rx = n->id; r->tx = tx; r->hop = 0; r->boot = n->boot;
    r->kind = (uint8_t)((compact ? TW_KIND_LLTF : TW_KIND_FULL) | (kflags & (TW_KIND_FWI | TW_KIND_RXERR | TW_KIND_EXTRA)));
    r->recseq = ++n->recseq;
    r->txseq = txseq;
    r->rx_us = rx_us;
    r->net_us = now + n->offset;
    r->rssi = rssi; r->noise = noise; r->agc = agc; r->fft = fft;
    r->n = (uint8_t)cnt;
    r->step = (uint8_t)step;
    r->phy = phy;
    for (int i = 0; i < cnt; i++) {
        int v = (int)lrintf(amp[i] * 4.0f / (float)step);
        r->q[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    e->used = 1;
    e->sent = 0;
    e->relay = 0;                       /* the slot may have held a relayed record before */
    e->made_us = now;
    n->n_meas++;
    if (!(kflags & TW_KIND_EXTRA) && valid_id(tx) && tx != n->id) {
        score_add(n, tx, r);            /* 2.2: the movement score (normal frames only) */
    }
    tw_seen(n, n->id, n->boot, r->recseq);
    return r;
}

/* ---------------- de-duplication: a 256-wide window per measuring board and boot ---------------- */
int tw_seen(tw_node_t *n, uint8_t rx, uint8_t boot, uint32_t recseq)
{
    if (!valid_id(rx)) {
        return 1;
    }
    tw_seen_t *s = &n->seen[rx];
    if (!s->used || s->boot != boot) {          /* first record from this board, or it rebooted */
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->boot = boot;
        s->hi = recseq;
        s->bits[0] = 1;
        return 0;
    }
    int32_t d = (int32_t)(recseq - s->hi);
    if (d > 0) {                                /* newer: slide the window up by d */
        if (d >= TW_SEEN_LEN) {
            memset(s->bits, 0, sizeof(s->bits));
        } else {
            int words = d / 32, sh = d % 32;
            for (int w = TW_SEEN_LEN / 32 - 1; w >= 0; w--) {
                uint32_t v = 0;
                int src = w - words;
                if (src >= 0) {
                    v = s->bits[src] << sh;
                    if (sh && src - 1 >= 0) {
                        v |= s->bits[src - 1] >> (32 - sh);
                    }
                }
                s->bits[w] = v;
            }
        }
        s->hi = recseq;
        s->bits[0] |= 1;
        return 0;
    }
    uint32_t back = (uint32_t)(-d);
    if (back >= TW_SEEN_LEN) {
        n->n_dup++;
        return 1;                               /* too old to tell: treat as seen */
    }
    uint32_t m = 1u << (back % 32);
    if (s->bits[back / 32] & m) {
        n->n_dup++;
        return 1;
    }
    s->bits[back / 32] |= m;
    return 0;
}

/* ---------------- text for the computer ---------------- */
static const char HEX[] = "0123456789abcdef";

int tw_format_record(const tw_rec_t *r, char *out, int max)
{
    if ((r->kind & TW_KIND_MASK) == TW_KIND_INFO) {
        /* TWV,board,boot,record number,firmware,build ("firmware|build" in the record) */
        char text[TW_INFO_MAX + 1];
        int n = r->n > TW_INFO_MAX ? TW_INFO_MAX : r->n;
        for (int i = 0; i < n; i++) {
            char c = (char)r->q[i];
            text[i] = c == '|' ? ',' : (c == ',' || c < 32 || c > 126) ? ' ' : c;
        }
        text[n] = 0;
        return snprintf(out, (size_t)max, "TWV,%u,%u,%lu,%s\n", r->rx, r->boot, (unsigned long)r->recseq, text);
    }
    if ((r->kind & TW_KIND_MASK) == TW_KIND_CMD) {
        /* TWX,board,boot,record number,target:key:flags:generation:seconds left:value (hex),... */
        int k = snprintf(out, (size_t)max, "TWX,%u,%u,%lu", r->rx, r->boot, (unsigned long)r->recseq);
        for (int i = 0; i + TW_CMD_HDR <= r->n && k > 0 && k < max - 40;) {
            const uint8_t *e = r->q + i;
            int len = e[5];
            if (len > TW_CMD_VAL || i + TW_CMD_HDR + len > r->n) {
                break;
            }
            k += snprintf(out + k, (size_t)(max - k), ",%u:%u:%u:%u:%u:", e[0], e[1], e[2], e[3], e[4]);
            for (int b = 0; b < len; b++) {
                out[k++] = HEX[e[TW_CMD_HDR + b] >> 4];
                out[k++] = HEX[e[TW_CMD_HDR + b] & 15];
            }
            out[k] = 0;
            i += TW_CMD_HDR + len;
        }
        if (k > 0 && k < max - 1) {
            out[k++] = '\n';
            out[k] = 0;
        }
        return k;
    }
    if ((r->kind & TW_KIND_MASK) == TW_KIND_UPDATE) {
        /* 4.0: TWP,board,boot,record number,the update's id (hex),its state (TW_UP_ST_*),chunks it has,
           of how many,1 if it has the manifest */
        if (r->n < 10) {
            return 0;
        }
        return snprintf(out, (size_t)max, "TWP,%u,%u,%lu,%08lx,%u,%u,%u,%u\n", r->rx, r->boot,
                        (unsigned long)r->recseq, (unsigned long)rd32be(r->q), r->q[4],
                        (unsigned)(r->q[5] | r->q[6] << 8), (unsigned)(r->q[7] | r->q[8] << 8), r->q[9]);
    }
    if ((r->kind & TW_KIND_MASK) == TW_KIND_SURVEY) {
        /* 3.1: TWU,board,boot,record number,generation,then per channel
           channel:busy %:foreign frames:loudest RSSI:mean RSSI:noise floor (the last three empty
           with no frames) */
        int k = snprintf(out, (size_t)max, "TWU,%u,%u,%lu,%lu", r->rx, r->boot, (unsigned long)r->recseq,
                         (unsigned long)r->txseq);
        int cnt = r->n >= 2 ? r->q[1] : 0;
        for (int i = 0; i < cnt && 2 + (i + 1) * TW_SURVEY_ENTRY <= r->n && k > 0 && k < max - 40; i++) {
            const uint8_t *e = r->q + 2 + i * TW_SURVEY_ENTRY;
            unsigned fr = (unsigned)e[1] | ((unsigned)e[2] << 8);
            if (fr) {
                k += snprintf(out + k, (size_t)(max - k), ",%u:%u.%u:%u:%d:%d:%d", r->q[0] + i, e[0] / 2,
                              (e[0] & 1) * 5, fr, (int8_t)e[3], (int8_t)e[4], (int8_t)e[5]);
            } else {
                k += snprintf(out + k, (size_t)(max - k), ",%u:%u.%u:0:::", r->q[0] + i, e[0] / 2, (e[0] & 1) * 5);
            }
        }
        if (k > 0 && k < max - 1) {
            out[k++] = '\n';
            out[k] = 0;
        }
        return k;
    }
    if ((r->kind & TW_KIND_MASK) == TW_KIND_SCORE) {
        /* TWM,board,boot,record number,its uptime s,the timekeeper's clock,score of board 1..16 (x10),
           2.7: where its last second frame ended, us after its slot start by its clock (0: none),
           3.4: its detections that second, hex (bit i: board i+1); 3.5: its steadier-than-quiet marks,
           hex (the same bits) */
        int k = snprintf(out, (size_t)max, "TWM,%u,%u,%lu,%lu,%lu", r->rx, r->boot, (unsigned long)r->recseq,
                         (unsigned long)r->txseq, (unsigned long)r->net_us);
        for (int i = 0; i < TW_MAX_NODES && k > 0 && k < max - 6; i++) {
            k += snprintf(out + k, (size_t)(max - k), ",%u", i < r->n ? r->q[i] : 0);
        }
        if (r->n > TW_MAX_NODES && k > 0 && k < max - 8) {
            k += snprintf(out + k, (size_t)(max - k), ",%u", 20u * r->q[TW_MAX_NODES]);
        }
        if (r->n > TW_MAX_NODES + 2 && k > 0 && k < max - 8) {   /* 3.4: its detections, bit i: board i+1 */
            k += snprintf(out + k, (size_t)(max - k), ",%04x",
                          (unsigned)r->q[TW_MAX_NODES + 1] | ((unsigned)r->q[TW_MAX_NODES + 2] << 8));
        }
        if (r->n > TW_MAX_NODES + 4 && k > 0 && k < max - 8) {   /* 3.5: steadier than quiet, the same bits */
            k += snprintf(out + k, (size_t)(max - k), ",%04x",
                          (unsigned)r->q[TW_MAX_NODES + 3] | ((unsigned)r->q[TW_MAX_NODES + 4] << 8));
        }
        if (k > 0 && k < max - 1) {
            out[k++] = '\n';
            out[k] = 0;
        }
        return k;
    }
    int k = snprintf(out, (size_t)max, "TWR,%u,%u,%lu,%u,%lu,%u,%lu,%lu,%d,%d,%u,%d,%u,%u,%u,%u,",
                     r->rx, r->tx, (unsigned long)r->txseq, r->hop, (unsigned long)r->recseq, r->boot,
                     (unsigned long)r->rx_us, (unsigned long)r->net_us, r->rssi, r->noise, r->agc, r->fft,
                     r->phy, r->kind, r->n, r->step);
    if (k < 0 || k + 2 * r->n + 2 > max) {
        return -1;
    }
    for (int i = 0; i < r->n; i++) {
        out[k++] = HEX[r->q[i] >> 4];
        out[k++] = HEX[r->q[i] & 15];
    }
    out[k++] = '\n';
    out[k] = 0;
    return k;
}

int tw_format_node(const tw_node_t *n, const tw_hdr_t *h, int8_t rssi, uint8_t heard_pct, char *out, int max)
{
    /* ...,rssi,setting number,its transmit power (quarter dBm; 0 = never set: full power),
       relaying on (1) / off (0), how well this (the printing) board hears it, %,
       2.1+: its setting flags; since the last TWN: its normal frames heard, of them announcing an
       extra frame, its extra frames heard, CSI on them, the longest time from its normal frame
       to its extra frame arriving here (us), when it says its last extra frame ended (us after
       its slot start, by its own clock; 0 = none), this board's records of it flagged by the
       radio (rx_state) */
    /* 3.0: its uptime, settings and second frame's end as its records said them (TWM, its standing
       network keys), not its header; the same fields as before (0 until said) */
    uint8_t s = h->src <= TW_MAX_NODES ? h->src : 0;
    uint8_t all = (uint8_t)((1u << TW_K_NET_FLAGS_N) - 1), has = (n->peer_has[s] & all) == all;
    uint8_t power = has ? n->peer_net[s][0] : 0, flags = has ? net_flags(n->peer_net[s]) : 0;
    uint8_t chan = (n->peer_has[s] & (1u << (TW_K_CHANNEL - TW_K_NET_FIRST))) ? n->peer_net[s][TW_K_CHANNEL - TW_K_NET_FIRST] : 0;
    return snprintf(out, (size_t)max, "TWN,%u,%lu,%u,%u,%04x,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%u,%u\n", h->src,
                    (unsigned long)h->seq, h->leader, h->flags, h->active, h->period_ms, n->peer_up[s], rssi,
                    has ? net_seq(n->peer_gen[s]) : 0u, power, (flags & TW_SET_RELAY_OFF) ? 0u : 1u, heard_pct,
                    flags, n->c_frames[s], n->c_xnext[s], n->c_xheard[s], n->c_xcsi[s],
                    n->c_xgap[s], 20u * n->c_xend_q[s], n->c_rxerr[s], chan);     /* 3.2: its channel key */
}

int tw_format_status(const tw_node_t *n, uint32_t now, char *out, int max)
{
    uint32_t start, p;
    int fits = slot_of(n, now, &start, &p);
    int used = 0;
    for (int i = 0; i < TW_QUEUE_LEN; i++) {
        used += n->q[i].used;
    }
    /* ...,queue_used,setting number,setting power,power the radio reports (quarter dBm),
       relaying on/off, records this board relayed, relayed records it had no room for,
       2.1+: setting flags, extra frames sent, extra frames skipped (nothing fitted in time) */
    /* 3.2, at the end: the channel it works on, the network's (its key), 1 while looking for the
       others, switches and scans since start */
    /* 3.3, after those: its group, frames of other groups heard, and the last such group; 3.4: its
       detection threshold (x10) and the detections begun since start; 3.5: its steadier-than-quiet
       threshold (x10, 0: off), the marks begun, and those that became the new quiet; 4.0: its update's
       state, 1 while sending one, update frames and bursts sent, chunks still lacking somewhere; 4.2:
       links that learned their quiet level again after a busy start */
    return snprintf(out, (size_t)max, "TWS,%u,%u,%u,%u,%04x,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%d,%u,%u,%u,%u,%lu,%lu,%u,%lu,%lu,%u,%u,%u,%u,%u,%u,%lu,%u,%u,%lu,%u,%lu,%lu,%u,%u,%lu,%lu,%d,%lu\n",
                    n->id, TW_VERSION, n->leader, (unsigned)((n->synced ? TW_F_SYNCED : 0) |
                    (n->is_exit ? TW_F_EXIT : 0) | (n->leader == n->id ? TW_F_LEADER : 0)),
                    tw_active(n, now), (unsigned long)(p / 1000u), (unsigned long)(fits ? start : 0),
                    (unsigned long)n->offset, (unsigned long)n->n_tx, (unsigned long)n->n_rx_frames,
                    (unsigned long)n->n_bad_frames, (unsigned long)n->n_meas, (unsigned long)n->n_dup,
                    (unsigned long)n->n_dropped, n->leader_changes, used, n->set_seq, n->set_power,
                    n->tx_power_q, (n->set_flags & TW_SET_RELAY_OFF) ? 0u : 1u, (unsigned long)n->n_relayed_out,
                    (unsigned long)n->n_relay_full, n->set_flags, (unsigned long)n->n_extra_sent,
                    (unsigned long)n->n_extra_skipped, n->chan_home, n->set_channel, n->scan_on, n->chan_switches,
                    n->chan_scans, n->group, (unsigned long)n->n_other_group, n->other_group_last,
                    tw_detect_level(n), (unsigned long)n->det_count, tw_detect_low_level(n),
                    (unsigned long)n->low_count, (unsigned long)n->low_accepted, n->up_state, n->up_tx_on,
                    (unsigned long)n->up_frames, (unsigned long)n->up_bursts, up_need_count(n),
                    (unsigned long)n->low_relearned);
}
