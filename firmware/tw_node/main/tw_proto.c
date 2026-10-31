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

#define AIRTIME_US   150u          /* rough preamble + header time, added to offset candidates */

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

void tw_note_settings(tw_node_t *n, const tw_hdr_t *h)
{
    if (h->set_power < TW_POWER_MIN_Q || h->set_power > TW_POWER_MAX_Q) {
        return;                     /* none (older firmware sends 0), or nonsense */
    }
    int8_t newer = (int8_t)(uint8_t)(h->set_seq - n->set_seq);
    /* newer number wins; the same number with a different value (two computers set it at
       the same moment) is settled the same way everywhere: the lower board number wins */
    if (n->set_power == 0 || newer > 0 ||
        (newer == 0 && h->set_power != n->set_power && h->src < n->id)) {
        n->set_seq = h->set_seq;
        n->set_power = h->set_power;
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
    tw_note_settings(n, h);
    n->heard[h->src] = 1;
    n->heard_us[h->src] = rx_local;
    tw_update_roles(n, rx_local);
    if (h->src != n->leader) {
        return;
    }
    /* the leader's clock: its frame says "net_us" when it was built; it went on the
       air a little later (queueing), so each candidate is an underestimate: keep the max */
    uint32_t cand = h->net_us - rx_local + AIRTIME_US;
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

int tw_build_frame(tw_node_t *n, uint32_t now, uint8_t *buf, int max)
{
    if (max > TW_MAX_PAYLOAD) {
        max = TW_MAX_PAYLOAD;
    }
    tw_update_roles(n, now);
    uint16_t act = tw_active(n, now);
    int pos = TW_HDR_LEN, count = 0;
    int copies = popcount16(act) <= TW_FULL_MAX_ACTIVE ? TW_COPIES : 1;     /* airtime budget */
    /* expire old records */
    for (int i = 0; i < TW_QUEUE_LEN; i++) {
        tw_qent_t *e = &n->q[i];
        if (e->used && (e->sent >= copies || (uint32_t)(now - e->made_us) > TW_RECORD_TTL_US)) {
            if (e->sent == 0) {
                n->n_dropped++;
            }
            e->used = 0;
        }
    }
    /* never-sent records first (oldest first), then second copies; a record goes into a
       given frame at most once */
    uint8_t in_frame[TW_QUEUE_LEN];
    memset(in_frame, 0, sizeof(in_frame));
    for (int pass = 0; pass < copies; pass++) {
        for (;;) {
            int pick = -1;
            for (int i = 0; i < TW_QUEUE_LEN; i++) {
                tw_qent_t *e = &n->q[i];
                if (e->used && !in_frame[i] && e->sent == pass &&
                        (pick < 0 || (int32_t)(e->made_us - n->q[pick].made_us) < 0)) {
                    pick = i;
                }
            }
            if (pick < 0 || pos + rec_len(&n->q[pick].rec) > max || count >= 255) {
                break;
            }
            put_rec(buf + pos, &n->q[pick].rec);
            pos += rec_len(&n->q[pick].rec);
            count++;
            n->q[pick].sent++;
            in_frame[pick] = 1;
        }
    }
    buf[0] = TW_MAGIC0; buf[1] = TW_MAGIC1; buf[2] = TW_VERSION; buf[3] = n->id;
    put32(buf + 4, n->seq++);
    put32(buf + 8, now + n->offset);
    buf[12] = n->leader;
    buf[13] = (uint8_t)((n->synced ? TW_F_SYNCED : 0) | (n->is_exit ? TW_F_EXIT : 0) |
                        (n->leader == n->id ? TW_F_LEADER : 0));
    put16(buf + 14, act);
    buf[16] = tw_period_ms(n, now);
    buf[17] = (uint8_t)count;
    put16(buf + 18, (uint16_t)(now / 1000000u));
    buf[20] = n->set_seq;
    buf[21] = n->set_power;
    n->n_tx++;
    return pos;
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

const tw_rec_t *tw_add_measurement(tw_node_t *n, uint32_t now, uint8_t tx, uint32_t txseq,
                                   uint32_t rx_us, int8_t rssi, int8_t noise, uint8_t agc, int8_t fft,
                                   uint8_t phy, uint8_t fwi, const int8_t *csi, int csi_len, float gain,
                                   int compact)
{
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
    /* pick the slot: a free one, else the oldest (counted as dropped if never sent) */
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
    r->kind = (uint8_t)((compact ? TW_KIND_LLTF : TW_KIND_FULL) | (fwi ? TW_KIND_FWI : 0));
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
    e->made_us = now;
    n->n_meas++;
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

int tw_format_node(const tw_hdr_t *h, int8_t rssi, char *out, int max)
{
    /* ...,rssi,setting number,its transmit power (quarter dBm; 0 = never set: full power) */
    return snprintf(out, (size_t)max, "TWN,%u,%lu,%u,%u,%04x,%u,%u,%d,%u,%u\n", h->src, (unsigned long)h->seq,
                    h->leader, h->flags, h->active, h->period_ms, h->uptime_s, rssi, h->set_seq, h->set_power);
}

int tw_format_status(const tw_node_t *n, uint32_t now, char *out, int max)
{
    uint32_t start, p;
    int fits = slot_of(n, now, &start, &p);
    int used = 0;
    for (int i = 0; i < TW_QUEUE_LEN; i++) {
        used += n->q[i].used;
    }
    /* ...,queue_used,setting number,setting power,power the radio reports (quarter dBm) */
    return snprintf(out, (size_t)max, "TWS,%u,%u,%u,%u,%04x,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%d,%u,%u,%u\n",
                    n->id, TW_VERSION, n->leader, (unsigned)((n->synced ? TW_F_SYNCED : 0) |
                    (n->is_exit ? TW_F_EXIT : 0) | (n->leader == n->id ? TW_F_LEADER : 0)),
                    tw_active(n, now), (unsigned long)(p / 1000u), (unsigned long)(fits ? start : 0),
                    (unsigned long)n->offset, (unsigned long)n->n_tx, (unsigned long)n->n_rx_frames,
                    (unsigned long)n->n_bad_frames, (unsigned long)n->n_meas, (unsigned long)n->n_dup,
                    (unsigned long)n->n_dropped, n->leader_changes, used, n->set_seq, n->set_power,
                    n->tx_power_q);
}
