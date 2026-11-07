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

int tw_set_power(tw_node_t *n, uint8_t power_q)
{
    if (power_q < TW_POWER_MIN_Q || power_q > TW_POWER_MAX_Q) {
        return -1;
    }
    n->set_seq = (uint8_t)(n->set_seq + 1);
    n->set_power = power_q;
    n->set_pending = 1;
    n->set_changes++;
    return 0;
}

void tw_set_flags(tw_node_t *n, uint8_t mask, uint8_t value)
{
    n->set_seq = (uint8_t)(n->set_seq + 1);
    if (n->set_power == 0) {
        n->set_power = TW_POWER_MAX_Q;      /* a setting always carries a power: full, as before */
    }
    n->set_flags = (uint8_t)((n->set_flags & ~mask) | (value & mask));
    n->set_pending = 1;
    n->set_changes++;
}

void tw_set_relay(tw_node_t *n, int on)
{
    tw_set_flags(n, TW_SET_RELAY_OFF, on ? 0 : TW_SET_RELAY_OFF);
}

void tw_note_settings(tw_node_t *n, const tw_hdr_t *h)
{
    if (h->set_power < TW_POWER_MIN_Q || h->set_power > TW_POWER_MAX_Q) {
        return;                     /* none (older firmware sends 0), or nonsense */
    }
    int8_t newer = (int8_t)(uint8_t)(h->set_seq - n->set_seq);
    /* newer number wins; the same number with a different value (two computers set it at
       the same moment) is settled the same way everywhere: the lower board number wins */
    if (n->set_power == 0 || newer > 0 ||
        (newer == 0 && (h->set_power != n->set_power || h->set_flags != n->set_flags) && h->src < n->id)) {
        n->set_seq = h->set_seq;
        n->set_power = h->set_power;
        n->set_flags = h->set_flags;
        n->set_pending = 1;
        n->set_changes++;
    }
}

void tw_note_frame(tw_node_t *n, const tw_hdr_t *h, uint32_t rx_local)
{
    if (!valid_id(h->src) || h->src == n->id) {
        return;
    }
    n->n_rx_frames++;
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
    if (h->xend_q) {
        n->c_xend_q[s] = h->xend_q;
    }
    if ((h->flags & TW_F_EXTRA_NEXT) && n->c_xnext[s] < 0xffff) {
        n->c_xnext[s]++;
    }
    tw_note_settings(n, h);
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
    memset(n->c_xend_q, 0, sizeof(n->c_xend_q));
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
    put16(buf + 18, (uint16_t)(now / 1000000u));
    buf[20] = n->set_seq;
    buf[21] = n->set_power;
    buf[22] = n->set_flags;
    buf[23] = n->xend_q;
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
    h->uptime_s = get16(buf + 18);
    h->set_seq = buf[20];
    h->set_power = buf[21];
    h->set_flags = buf[22];
    h->xend_q = buf[23];
    memcpy(h->hear, buf + 24, TW_MAX_NODES);
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
static uint8_t score_close(tw_score_t *s)
{
    uint8_t out = 0;
    if (s->nb && s->recs >= 5) {
        float tot = 0;
        int started = s->start >= TW_SCORE_START;
        for (int b = 0; b < s->nb; b++) {
            float m = s->s1[b] / s->recs;
            float sd = sqrtf(fmaxf(s->s2[b] / s->recs - m * m, 0.0f));
            if (!started) {                     /* the quiet level starts as the mean of 30 seconds */
                s->ref[b] += (sd - s->ref[b]) / (float)(s->start + 1);
            } else {
                tot += sd / fmaxf(s->ref[b], 1e-3f);
                /* then a running median: a small step towards each new second (0.2%: a lasting
                   change becomes the usual in about 10-15 minutes, as the replay's settle) */
                s->ref[b] *= sd > s->ref[b] ? 1.002f : 1.0f / 1.002f;
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

const tw_rec_t *tw_aux_record(tw_node_t *n, uint32_t now)
{
    uint32_t sec = now / 1000000u;
    if (sec != n->score_sec) {                  /* once a second: close it, queue the scores */
        n->score_sec = sec;
        uint8_t sc[TW_MAX_NODES];
        int any = 0;
        for (int i = 1; i <= TW_MAX_NODES; i++) {
            sc[i - 1] = i == n->id ? 0 : score_close(&n->score[i]);
            any |= sc[i - 1];
        }
        if (any) {
            tw_rec_t *r = queue_aux(n, now, TW_KIND_SCORE, sc, TW_MAX_NODES);
            r->txseq = sec;                     /* this board's uptime, s */
            return r;
        }
    }
    if (n->info[0] && (!n->info_sent || (uint32_t)(now - n->info_us) >= TW_INFO_EVERY_US)) {
        n->info_sent = 1;
        n->info_us = now;
        return queue_aux(n, now, TW_KIND_INFO, (const uint8_t *)n->info, (int)strlen(n->info));
    }
    return NULL;
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
    if ((r->kind & TW_KIND_MASK) == TW_KIND_SCORE) {
        /* TWM,board,boot,record number,its uptime s,the timekeeper's clock,score of board 1..16 (x10) */
        int k = snprintf(out, (size_t)max, "TWM,%u,%u,%lu,%lu,%lu", r->rx, r->boot, (unsigned long)r->recseq,
                         (unsigned long)r->txseq, (unsigned long)r->net_us);
        for (int i = 0; i < TW_MAX_NODES && k > 0 && k < max - 6; i++) {
            k += snprintf(out + k, (size_t)(max - k), ",%u", i < r->n ? r->q[i] : 0);
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
    uint8_t s = h->src <= TW_MAX_NODES ? h->src : 0;
    return snprintf(out, (size_t)max, "TWN,%u,%lu,%u,%u,%04x,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%u\n", h->src,
                    (unsigned long)h->seq, h->leader, h->flags, h->active, h->period_ms, h->uptime_s, rssi,
                    h->set_seq, h->set_power, (h->set_flags & TW_SET_RELAY_OFF) ? 0u : 1u, heard_pct,
                    h->set_flags, n->c_frames[s], n->c_xnext[s], n->c_xheard[s], n->c_xcsi[s],
                    n->c_xgap[s], 20u * n->c_xend_q[s], n->c_rxerr[s]);
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
    return snprintf(out, (size_t)max, "TWS,%u,%u,%u,%u,%04x,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%d,%u,%u,%u,%u,%lu,%lu,%u,%lu,%lu\n",
                    n->id, TW_VERSION, n->leader, (unsigned)((n->synced ? TW_F_SYNCED : 0) |
                    (n->is_exit ? TW_F_EXIT : 0) | (n->leader == n->id ? TW_F_LEADER : 0)),
                    tw_active(n, now), (unsigned long)(p / 1000u), (unsigned long)(fits ? start : 0),
                    (unsigned long)n->offset, (unsigned long)n->n_tx, (unsigned long)n->n_rx_frames,
                    (unsigned long)n->n_bad_frames, (unsigned long)n->n_meas, (unsigned long)n->n_dup,
                    (unsigned long)n->n_dropped, n->leader_changes, used, n->set_seq, n->set_power,
                    n->tx_power_q, (n->set_flags & TW_SET_RELAY_OFF) ? 0u : 1u, (unsigned long)n->n_relayed_out,
                    (unsigned long)n->n_relay_full, n->set_flags, (unsigned long)n->n_extra_sent,
                    (unsigned long)n->n_extra_skipped);
}
