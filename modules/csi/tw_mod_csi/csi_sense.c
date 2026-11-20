/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0 */
/*
 * csi_sense.c - the CSI sensing module's portable part (docs/MODULES.md): measurement records
 * (kinds 0 and 1, TWR), the movement score, detections (3.4), steadier than quiet (3.5) and
 * learning the quiet level again. Moved here unchanged from tw_proto.c; the core calls it
 * through tw_csi_sense (tw_node_t.sense). No ESP-IDF: built into the firmware (CONFIG_TW_MOD_CSI)
 * and into the desk simulator.
 *
 * Its state (tw_node_t.score[], det_*, low_*, score_last) and constants (TW_SCORE_*, TW_DETECT_*,
 * TW_LOW_*) are still in tw_proto.h; they move here in a later step.
 */
#include "csi_sense.h"

#include <math.h>
#include <string.h>

static int valid_id(uint8_t id) { return id >= 1 && id <= TW_MAX_NODES; }

/* ---------------- the L-LTF of a CSI buffer ---------------- */
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

/* ---------------- the movement score, detections, steadier than quiet ---------------- */
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
static uint8_t score_close(tw_score_t *s, int hold_down, int hold_up)
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
                /* 3.5: but not down while the link is marked steadier than quiet (hold_down);
                   4.7: nor up while it is detecting, within the board's detection hold (hold_up) */
                if (sd > s->ref[b]) {
                    if (!hold_up) {
                        s->ref[b] *= 1.002f;
                    }
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
        n->score[i].det_held = 0;
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

uint16_t tw_detect_hold_s(const tw_node_t *n)
{
    if (!n->set_len[TW_K_DETECT_HOLD]) {
        return TW_DHOLD_DEFAULT * 60;
    }
    uint8_t v = n->set_val[TW_K_DETECT_HOLD][0];
    return (uint16_t)((v <= TW_DHOLD_MAX ? v : TW_DHOLD_DEFAULT) * 60);
}

uint8_t tw_detect_low_level(const tw_node_t *n)
{
    if (!n->set_len[TW_K_DETECT_LOW]) {
        return TW_LOW_DEFAULT;
    }
    uint8_t v = n->set_val[TW_K_DETECT_LOW][0];
    return v <= TW_LOW_MAX ? v : TW_LOW_DEFAULT;
}

/* once a second (the core's TWM record): close each link's second, and fill TWM's fields of this
   module: sc[0..15] the scores, sc[17..18] the detections, sc[19..20] the steadier-than-quiet
   marks. Returns nonzero if any score */
static int csi_second(tw_node_t *n, uint32_t now, uint8_t *sc)
{
    (void)now;
    int any = 0;
    uint8_t level = tw_detect_level(n), low = tw_detect_low_level(n);
    uint16_t hold_s = tw_detect_hold_s(n);
    uint16_t bits = 0, lbits = 0;
    for (int i = 1; i <= TW_MAX_NODES; i++) {
        tw_score_t *t = &n->score[i];
        int held = low && t->below >= TW_LOW_HOLD && t->low_held < t->low_cap;
        /* 4.7: detecting (as of the last second), and within its detection hold */
        int held_up = hold_s && t->above >= TW_DETECT_HOLD && t->det_held < hold_s;
        sc[i - 1] = i == n->id ? 0 : score_close(t, held, held_up);
        if (sc[i - 1]) {
            t->none = 0;
        } else if (t->none < 255) {
            t->none++;                          /* 4.2: seconds in a row without a score */
        }
        if (sc[i - 1] >= TW_LOW_ARM_MIN && sc[i - 1] <= TW_LOW_ARM_MAX && t->armed < TW_LOW_KEEP) {
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
            if (t->below == TW_LOW_HOLD) {
                n->low_count++;                 /* one more began: held as long as the quiet was seen */
                t->low_cap = t->armed < TW_LOW_KEEP ? t->armed : TW_LOW_KEEP;
            }
            if (t->below >= TW_LOW_HOLD && t->low_held < t->low_cap && ++t->low_held == t->low_cap) {
                n->low_accepted++;              /* held as long as it may: */
                t->start = 0;                   /* 4.3: the quiet level again, from the stillness */
                t->below = 0;
                t->low_held = 0;
                t->armed = 0;
                t->busy = 0;
            }
        }
        if (t->below >= TW_LOW_HOLD && t->low_held > 0 && t->low_held < t->low_cap) {
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
                if (s->det_held < 65535) {
                    s->det_held++;              /* 4.7: a second of it, toward its detection hold */
                }
            }
        } else if (sc[i - 1] || s->none >= TW_DETECT_GAP) {
            s->above = 0;                       /* 4.2: not for one second without a score */
            s->det_held = 0;                    /* 4.7: the next detection has its hold afresh */
        }
    }
    memcpy(n->score_last, sc, TW_MAX_NODES);
    n->det_bits = bits;
    sc[TW_MAX_NODES + 1] = (uint8_t)bits;       /* 3.4: this second's detections */
    sc[TW_MAX_NODES + 2] = (uint8_t)(bits >> 8);
    n->low_bits = lbits;
    sc[TW_MAX_NODES + 3] = (uint8_t)lbits;      /* 3.5: this second's steadier-than-quiet marks */
    sc[TW_MAX_NODES + 4] = (uint8_t)(lbits >> 8);
    return any;
}

/* ---------------- one CSI measurement into a record ---------------- */
/* after the core's counts (tw_add_measurement): the record, queued (returned, for printing), and
   the movement score */
static const tw_rec_t *csi_measure(tw_node_t *n, uint32_t now, uint8_t tx, uint32_t txseq,
                                   uint32_t rx_us, int8_t rssi, int8_t noise, uint8_t agc, int8_t fft,
                                   uint8_t phy, uint8_t kflags, const int8_t *csi, int csi_len, float gain,
                                   int compact)
{
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
    tw_qent_t *e = tw_new_entry(n);
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

const tw_sense_t tw_csi_sense = {
    .measure = csi_measure,
    .second = csi_second,
    .relearn = tw_score_relearn,
    .detect_level = tw_detect_level,
    .detect_low_level = tw_detect_low_level,
};
