/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0 */
/*
 * sim.c - a pretend garage for tw_proto.c: N boards with their own drifting
 * clocks, one shared radio channel (frames that overlap in time collide and
 * are lost; others are lost at random), boards that die and come back, and
 * any board(s) cabled to a "computer" (exit). The exit boards print exactly
 * the lines the firmware prints; the summary of what really happened goes to
 * stderr as JSON, for check_sim.py.
 *
 *   sim --nodes 3 --seconds 60 [--loss 0.05] [--exit 1[,2]] [--kill 1@20-40]
 *       [--seed 7] [--realtime] [--out FILE] [--setpower 20@11[@1],40@20]
 *       [--block 2-1@10-50@0.75,...] [--setrelay 30@off,60@on] [--rxerr 0.01]
 *       [--set 0@cap=3,0@second=off,...]
 *
 * --block S-R@A-B@P: from A to B seconds, board R misses each of board S's frames with
 * probability P (on top of --loss): something in the way of that one direction.
 * --setrelay T@on|off: at T seconds the computer turns relaying on or off (first exit).
 * --rxerr P: each measurement is flagged by the radio (rx_state != 0) with probability P.
 * --set T@KEY=VALUE: at T seconds the computer changes a setting (first exit): second=on|off|force,
 *   measure=on|off (receivers measure extra frames), cap=N|off (records per frame).
 * Extra frames (a second frame in a board's slot, tw_node 2.1) go on the air 50 us after the
 * board's frame ends (ESP-NOW's send-done) plus up to TW_ACCESS_US of channel access, and
 * collide like any other frame; the summary counts them and their latest end in the slot.
 *
 * --setpower T@DBM[@BOARD]: at T seconds, the computer sets the network's transmit
 * power at BOARD (default: the first exit). The JSON summary says how long every
 * live board took to adopt each setting, and what each board ended with.
 *
 * In --realtime mode the exit's lines are written as they happen (for a pty
 * feeding listen.py and the dashboard); otherwise simulated time runs flat out
 * and each line is prefixed with the simulated host time in ns, as listen.py
 * writes its files.
 */
#define _DEFAULT_SOURCE
#include "../tw_node/main/tw_proto.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXN TW_MAX_NODES
#define CSI_LEN 384

#define MAXSET_F 16
typedef struct {
    tw_node_t st;
    double extra_at;            /* true time its pending extra frame is built (< 0: none) */
    int32_t extra_end_max;      /* latest end of its extra frames, us after its slot start (own clock) */
    long extra_late;            /* extra frames that ended after the slot + TW_SLOT_SLACK_US */
    double drift;               /* clock rate error */
    double off0;                /* clock offset at t = 0 (us) */
    double next_tx;             /* true time of the next send */
    int alive, exit;
    double dead_from, dead_to;
    double last_status;
    double node_last[MAXN + 1];
    int8_t node_rssi[MAXN + 1];
    tw_hdr_t node_hdr[MAXN + 1];
    long made[MAXN + 1];        /* ground truth: measurements made of each sender */
} simnode_t;

static simnode_t N_[MAXN + 1];
static int n_nodes = 3, realtime = 0, exits[MAXN + 1];
static double seconds = 60, p_loss = 0.05, p_rxerr = 0, t0_unix;
static double walk_a = 30e6, walk_b = 40e6;    /* planted walk across the 1-2 link (--walk A-B seconds) */
static long collisions = 0, frames = 0, lost_random = 0, lost_blocked = 0, rx_err_made = 0;
static double air_used_us = 0;
static long extra_frames = 0, collisions_extra = 0, extra_measured = 0;
static double clk_sum = 0, clk_max = 0, clk_last = 0;   /* followers' clock against the timekeeper's */
static long clk_n = 0;
static struct { double t; uint8_t mask, value; int fired; } fsets[MAXSET_F];
static FILE *out;
static unsigned rng_state = 7;
#define MAXSET 16
static struct { double t; uint8_t q; int at; uint8_t seq; double done_t; int fired; } sets[MAXSET];
static int n_sets = 0;
#define MAXBLK 32
static struct { int s, r; double a, b, p; } blks[MAXBLK];
static int n_blks = 0;
static struct { double t; int on, fired; } rsets[MAXSET];
static int n_rsets = 0, n_fsets = 0;

static double urand(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return ((rng_state >> 8) & 0xffffff) / (double)0x1000000;
}

static uint32_t local_us(simnode_t *s, double t) { return (uint32_t)(int64_t)llround(t * (1.0 + s->drift) + s->off0); }

static double true_after(simnode_t *s, double t, uint32_t now_local, uint32_t target_local)
{
    return t + (double)(uint32_t)(target_local - now_local) / (1.0 + s->drift);
}

static void emit(double t, const char *line)
{
    if (realtime) {
        fputs(line, out);
        fflush(out);
    } else {
        fprintf(out, "%lld,%s", (long long)llround((t0_unix + t / 1e6) * 1e9), line);
    }
}

/* the CSI a board measures from a sender: a fixed pattern per link, a slow drift,
   noise, and a "walk" across the 1-2 link between 30 and 40 s of simulated time */
static void make_csi(int rx, int tx, double t, int8_t *buf)
{
    for (int b = 0; b < CSI_LEN / 2; b++) {
        int live = (b >= 6 && b < 59 && b != 32) || (b >= 66 && b < 186);
        double base = live ? 20 + 12 * sin(0.37 * b + rx * 1.3 + tx * 0.7) : 0;
        double motion = 0;
        if (t > walk_a && t < walk_b && ((rx == 1 && tx == 2) || (rx == 2 && tx == 1))) {
            motion = 0.25 * sin(0.9 * b + t / 1e5);
        }
        double a = base * (1 + 0.03 * (urand() - 0.5) + motion);
        double ph = urand() * 6.283;
        int i = (int)lround(a * cos(ph)), q = (int)lround(a * sin(ph));
        buf[2 * b] = (int8_t)(i > 127 ? 127 : i < -128 ? -128 : i);
        buf[2 * b + 1] = (int8_t)(q > 127 ? 127 : q < -128 ? -128 : q);
    }
}

typedef struct {
    simnode_t *self;
    double t;
} rctx_t;

static uint8_t cur_src;

static void on_record(void *ctx, const tw_rec_t *r)
{
    rctx_t *c = ctx;
    if (!c->self->exit) {
        tw_relay_record(&c->self->st, local_us(c->self, c->t), cur_src, r);
        return;
    }
    if (!tw_seen(&c->self->st, r->rx, r->boot, r->recseq)) {
        char line[600];
        if (tw_format_record(r, line, sizeof(line)) > 0) {
            emit(c->t, line);
        }
    }
}

static int parse_args(int argc, char **argv, char *kill_spec, char *exit_spec, unsigned *seed, char **outpath)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nodes") && i + 1 < argc) n_nodes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc) p_loss = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rxerr") && i + 1 < argc) p_rxerr = atof(argv[++i]);
        else if (!strcmp(argv[i], "--exit") && i + 1 < argc) strncpy(exit_spec, argv[++i], 63);
        else if (!strcmp(argv[i], "--kill") && i + 1 < argc) strncpy(kill_spec, argv[++i], 255);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) *seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--realtime")) realtime = 1;
        else if (!strcmp(argv[i], "--walk") && i + 1 < argc) {
            char *e; walk_a = strtod(argv[++i], &e) * 1e6; walk_b = (*e == '-' ? strtod(e + 1, NULL) : walk_a / 1e6 + 10) * 1e6;
        }
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) *outpath = argv[++i];
        else if (!strcmp(argv[i], "--block") && i + 1 < argc) {
            for (char *p = argv[++i]; *p && n_blks < MAXBLK;) {
                blks[n_blks].s = (int)strtol(p, &p, 10);
                if (*p == '-') blks[n_blks].r = (int)strtol(p + 1, &p, 10);
                if (*p == '@') blks[n_blks].a = strtod(p + 1, &p) * 1e6;
                if (*p == '-') blks[n_blks].b = strtod(p + 1, &p) * 1e6;
                if (*p == '@') blks[n_blks].p = strtod(p + 1, &p);
                n_blks++;
                if (*p == ',') p++; else break;
            }
        }
        else if (!strcmp(argv[i], "--setrelay") && i + 1 < argc) {
            for (char *p = argv[++i]; *p && n_rsets < MAXSET;) {
                rsets[n_rsets].t = strtod(p, &p) * 1e6;
                if (*p == '@') { p++; rsets[n_rsets].on = !strncmp(p, "on", 2); while (*p && *p != ',') p++; }
                n_rsets++;
                if (*p == ',') p++; else break;
            }
        }
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            for (char *p = argv[++i]; *p && n_fsets < MAXSET_F;) {
                fsets[n_fsets].t = strtod(p, &p) * 1e6;
                if (*p == '@') p++;
                uint8_t m = 0, v = 0;
                if (!strncmp(p, "second=off", 10)) { m = TW_SET_SECOND_OFF | TW_SET_SECOND_FORCE; v = TW_SET_SECOND_OFF; }
                else if (!strncmp(p, "second=on", 9)) { m = TW_SET_SECOND_OFF | TW_SET_SECOND_FORCE; v = 0; }
                else if (!strncmp(p, "second=force", 12)) { m = TW_SET_SECOND_OFF | TW_SET_SECOND_FORCE; v = TW_SET_SECOND_FORCE; }
                else if (!strncmp(p, "measure=on", 10)) { m = TW_SET_MEASURE_EXTRA; v = TW_SET_MEASURE_EXTRA; }
                else if (!strncmp(p, "measure=off", 11)) { m = TW_SET_MEASURE_EXTRA; v = 0; }
                else if (!strncmp(p, "cap=", 4)) { m = 0xF0; v = (uint8_t)((atoi(p + 4) & 15) << TW_SET_CAP_SHIFT); }
                else { fprintf(stderr, "--set: unknown %s\n", p); return -1; }
                fsets[n_fsets].mask = m; fsets[n_fsets].value = v; n_fsets++;
                while (*p && *p != ',') p++;
                if (*p == ',') p++; else break;
            }
        }
        else if (!strcmp(argv[i], "--setpower") && i + 1 < argc) {
            for (char *p = argv[++i]; *p && n_sets < MAXSET;) {
                double t = strtod(p, &p), dbm = 0; int at = 0;
                if (*p == '@') dbm = strtod(p + 1, &p);
                if (*p == '@') at = (int)strtol(p + 1, &p, 10);
                sets[n_sets].t = t * 1e6; sets[n_sets].q = (uint8_t)lround(dbm * 4); sets[n_sets].at = at;
                sets[n_sets].done_t = -1; n_sets++;
                if (*p == ',') p++; else break;
            }
        }
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return -1; }
    }
    if (n_nodes < 1 || n_nodes > MAXN) { fprintf(stderr, "--nodes 1..%d\n", MAXN); return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    char kill_spec[256] = "", exit_spec[64] = "1";
    unsigned seed = 7;
    char *outpath = NULL;
    if (parse_args(argc, argv, kill_spec, exit_spec, &seed, &outpath) < 0) return 2;
    rng_state = seed;
    out = outpath ? fopen(outpath, "w") : stdout;
    if (!out) { perror(outpath); return 2; }
    t0_unix = (double)time(NULL);
    for (char *p = exit_spec; *p;) {
        int id = (int)strtol(p, &p, 10);
        if (id >= 1 && id <= MAXN) exits[id] = 1;
        if (*p == ',') p++; else break;
    }
    for (int id = 1; id <= n_nodes; id++) {
        simnode_t *s = &N_[id];
        s->drift = (urand() - 0.5) * 40e-6;                 /* +-20 ppm */
        s->off0 = urand() * 4e9;                            /* booted at different times */
        s->alive = 1;
        s->exit = exits[id];
        s->dead_from = s->dead_to = -1;
        s->next_tx = urand() * 20000;
        s->extra_at = -1;
        s->last_status = -1e9;
        tw_init(&s->st, (uint8_t)id, (uint8_t)s->exit, seed * 31u + (unsigned)id);
        tw_set_info(&s->st, "sim|board");
    }
    /* kills: "id@from-to,id@from-to" in seconds */
    for (char *p = kill_spec; *p;) {
        int id = (int)strtol(p, &p, 10);
        double a = -1, b = -1;
        if (*p == '@') { a = strtod(p + 1, &p); if (*p == '-') b = strtod(p + 1, &p); }
        if (id >= 1 && id <= n_nodes) { N_[id].dead_from = a * 1e6; N_[id].dead_to = b * 1e6; }
        if (*p == ',') p++; else break;
    }

    for (int k = 0; k < n_sets; k++) {
        if (!sets[k].at) for (int id = 1; id <= n_nodes; id++) if (exits[id]) { sets[k].at = id; break; }
        if (!sets[k].at) sets[k].at = 1;
    }
    double t = 0, end = seconds * 1e6;
    double busy_until = 0;      /* the channel: end of the frame on the air */
    int busy_src = 0;
    uint8_t frame[TW_MAX_PAYLOAD], last_frame[TW_MAX_PAYLOAD];
    int last_len = 0, last_collided = 0, last_extra = 0;
    double last_start = 0;
    long leader_changes_total = 0;
    struct timespec wall0;
    clock_gettime(CLOCK_MONOTONIC, &wall0);

    while (t < end) {
        /* next sender: a board's slot, or the extra frame it has pending */
        int who = 0, is_extra = 0;
        double when = 0;
        for (int id = 1; id <= n_nodes; id++) {
            double tt = N_[id].next_tx;
            int ex = 0;
            if (N_[id].extra_at >= 0 && N_[id].extra_at <= tt) { tt = N_[id].extra_at; ex = 1; }
            if (!who || tt < when) { who = id; when = tt; is_extra = ex; }
        }
        simnode_t *s = &N_[who];
        double tx_t = is_extra ? when + urand() * TW_ACCESS_US      /* channel access after send-done */
                               : s->next_tx + urand() * 300;        /* queueing delay before the air */
        /* deliver the previous frame if it ended before this one starts */
        if (last_len && busy_until <= tx_t) {
            /* receivers */
            for (int id = 1; id <= n_nodes; id++) {
                simnode_t *r = &N_[id];
                int dead = r->dead_from >= 0 && busy_until >= r->dead_from && (r->dead_to < 0 || busy_until < r->dead_to);
                if (id == busy_src || dead || last_collided) continue;
                if (urand() < p_loss) { lost_random++; continue; }
                int blocked = 0;
                for (int k = 0; k < n_blks; k++)
                    if (blks[k].s == busy_src && blks[k].r == id && busy_until >= blks[k].a && busy_until < blks[k].b &&
                        urand() < blks[k].p) blocked = 1;
                if (blocked) { lost_blocked++; continue; }
                uint32_t now_l = local_us(r, busy_until);
                tw_hdr_t h;
                if (tw_peek_header(last_frame, last_len, &h) < 0) { r->st.n_bad_frames++; continue; }
                tw_note_frame(&r->st, &h, now_l);
                int8_t csi[CSI_LEN];
                make_csi(id, h.src, busy_until, csi);
                int compact = __builtin_popcount(tw_active(&r->st, now_l)) > TW_FULL_MAX_ACTIVE;
                uint8_t phy = (uint8_t)(1 | (1 << 3) | (2 << 5));   /* HT40, HT, secondary below */
                int8_t rssi = (int8_t)(-50 - 3 * abs(id - h.src));
                uint8_t kflags = (p_rxerr > 0 && urand() < p_rxerr) ? TW_KIND_RXERR : 0;
                if (h.flags & TW_F_EXTRA) kflags |= TW_KIND_EXTRA;
                const tw_rec_t *rec = tw_add_measurement(&r->st, now_l, h.src, h.seq, now_l, rssi, -92, 30, 0,
                                                         phy, kflags, csi, CSI_LEN, 1.0f, compact);
                if (rec) {
                    rx_err_made += (kflags & TW_KIND_RXERR) != 0;
                    extra_measured += (kflags & TW_KIND_EXTRA) != 0;
                    r->made[h.src]++;
                }
                rctx_t c = { r, busy_until };
                cur_src = h.src;
                if (r->exit) {
                    char line[600];
                    if (rec && tw_format_record(rec, line, sizeof(line)) > 0) emit(busy_until, line);
                    if (!(h.flags & TW_F_EXTRA)) {
                        r->node_hdr[h.src] = h;
                        r->node_rssi[h.src] = rssi;
                    }
                }
                if (tw_parse_frame(last_frame, last_len, &h, on_record, &c) < 0) r->st.n_bad_frames++;
            }
            last_len = 0;
        }
        if (tx_t >= end) break;
        /* realtime pacing */
        if (realtime) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double wall = (now.tv_sec - wall0.tv_sec) * 1e6 + (now.tv_nsec - wall0.tv_nsec) / 1e3;
            if (tx_t > wall) usleep((useconds_t)(tx_t - wall));
        }
        int dead = s->dead_from >= 0 && tx_t >= s->dead_from && (s->dead_to < 0 || tx_t < s->dead_to);
        uint32_t lnow = local_us(s, tx_t);
        /* the computer's power commands, and the firmware applying settings */
        for (int k = 0; k < n_sets; k++) {
            if (!sets[k].fired && tx_t >= sets[k].t) {
                sets[k].fired = 1;
                tw_set_power(&N_[sets[k].at].st, sets[k].q);
                sets[k].seq = N_[sets[k].at].st.set_seq;
            }
        }
        for (int k = 0; k < n_fsets; k++) {
            if (!fsets[k].fired && tx_t >= fsets[k].t) {
                fsets[k].fired = 1;
                int at = 1;
                for (int id = 1; id <= n_nodes; id++) if (exits[id]) { at = id; break; }
                tw_set_flags(&N_[at].st, fsets[k].mask, fsets[k].value);
            }
        }
        for (int k = 0; k < n_rsets; k++) {
            if (!rsets[k].fired && tx_t >= rsets[k].t) {
                rsets[k].fired = 1;
                int at = 1;
                for (int id = 1; id <= n_nodes; id++) if (exits[id]) { at = id; break; }
                tw_set_relay(&N_[at].st, rsets[k].on);
            }
        }
        for (int id = 1; id <= n_nodes; id++) {
            tw_node_t *st = &N_[id].st;
            if (st->set_pending) { st->set_pending = 0; }
            st->tx_power_q = st->set_power ? st->set_power : TW_POWER_MAX_Q;
        }
        for (int k = 0; k < n_sets; k++) {         /* has every live board got setting k (or newer)? */
            if (!sets[k].fired || sets[k].done_t >= 0) continue;
            int all = 1;
            for (int id = 1; id <= n_nodes; id++) {
                simnode_t *r = &N_[id];
                int rd = r->dead_from >= 0 && tx_t >= r->dead_from && (r->dead_to < 0 || tx_t < r->dead_to);
                if (rd) continue;
                if ((int8_t)(uint8_t)(r->st.set_seq - sets[k].seq) < 0 || r->st.set_power == 0) { all = 0; break; }
            }
            if (all) sets[k].done_t = tx_t;
        }
        int len = 0;
        if (is_extra) {
            s->extra_at = -1;
            if (!dead) len = tw_build_extra(&s->st, local_us(s, when), frame, TW_MAX_PAYLOAD);
        } else if (!dead) {
            for (const tw_rec_t *a; (a = tw_aux_record(&s->st, lnow)) != NULL;) {    /* firmware: once a second */
                char line[600];
                if (s->exit && tw_format_record(a, line, sizeof(line)) > 0) emit(tx_t, line);
            }
            len = tw_build_frame(&s->st, lnow, frame, TW_MAX_PAYLOAD);
        }
        if (len > 0) {
            double air = tw_air_us(len);
            air_used_us += air;
            frames++;
            if (is_extra) {
                extra_frames++;
                tw_note_sent(&s->st, local_us(s, tx_t + air), 1);        /* its send-done */
                int32_t end_pos = tw_slot_pos(&s->st, local_us(s, tx_t + air), (uint8_t)who);
                if (end_pos > s->extra_end_max) s->extra_end_max = end_pos;
                if (end_pos > (int32_t)(TW_SLOT_US + TW_SLOT_SLACK_US)) s->extra_late++;
            }
            if (last_len && tx_t < busy_until) {             /* overlaps the frame still on the air */
                collisions++;
                if (is_extra || last_extra) collisions_extra++;
                last_collided = 1;                           /* both lost */
            } else {
                memcpy(last_frame, frame, (size_t)len);
                last_len = len;
                last_collided = 0;
                last_extra = is_extra;
                last_start = tx_t;
                busy_until = tx_t + air;
                busy_src = who;
            }
            if (!is_extra && s->st.extra_planned) {
                s->extra_at = tx_t + air + 50;               /* ESP-NOW's send-done, then the build */
            }
        }
        if (is_extra) {
            t = tx_t;
            continue;
        }
        if (dead && (s->st.n_rx_frames || s->st.n_tx)) {
            /* a dead board forgets everything: it reboots when it comes back */
            int was_exit = s->exit;
            tw_init(&s->st, (uint8_t)who, (uint8_t)was_exit, (unsigned)(t + who));
            tw_set_info(&s->st, "sim|board");
        }
        (void)last_start;
        if (dead) s->extra_at = -1;
        s->next_tx = true_after(s, tx_t, lnow, tw_next_tx_local(&s->st, lnow));
        if (dead) s->next_tx = tx_t + 20000;
        t = tx_t;
        /* once a second, each exit prints its status and the boards it hears */
        for (int id = 1; id <= n_nodes; id++) {
            simnode_t *r = &N_[id];
            int rdead = r->dead_from >= 0 && t >= r->dead_from && (r->dead_to < 0 || t < r->dead_to);
            if (!r->exit || rdead || t - r->last_status < 1e6) continue;
            r->last_status = t;
            char line[300];
            uint32_t lr = local_us(r, t);
            tw_update_roles(&r->st, lr);
            tw_format_status(&r->st, lr, line, sizeof(line));
            emit(t, line);
            for (int k = 1; k <= n_nodes; k++) {
                if (k != id && r->st.heard[k] && (uint32_t)(lr - r->st.heard_us[k]) < TW_ACTIVE_US) {
                    tw_format_node(&r->st, &r->node_hdr[k], r->node_rssi[k], tw_hear_pct(&r->st, lr, (uint8_t)k),
                                   line, sizeof(line));
                    emit(t, line);
                }
            }
            tw_clear_counts(&r->st);
        }
        /* once a second (after the first 10): how far each follower's clock is from its
           timekeeper's, in truth (us; negative = behind) */
        if (t >= 10e6 && t - clk_last >= 1e6) {
            clk_last = t;
            for (int id = 1; id <= n_nodes; id++) {
                simnode_t *f = &N_[id];
                uint8_t ld = f->st.leader;
                if (!f->st.synced || ld == id || ld < 1 || ld > n_nodes) continue;
                simnode_t *L = &N_[ld];
                int32_t err = (int32_t)((local_us(f, t) + f->st.offset) - (local_us(L, t) + L->st.offset));
                if (err > 100000 || err < -100000) continue;        /* a timekeeper change under way */
                clk_sum += err;
                clk_n++;
                if (fabs((double)err) > clk_max) clk_max = fabs((double)err);
            }
        }
    }
    for (int id = 1; id <= n_nodes; id++) leader_changes_total += N_[id].st.leader_changes;
    /* ground truth for check_sim.py */
    fprintf(stderr, "{\"nodes\": %d, \"seconds\": %.1f, \"loss\": %.3f, \"frames\": %ld, \"collisions\": %ld, "
                    "\"lost_random\": %ld, \"lost_blocked\": %ld, \"airtime_pct\": %.2f, \"leader_changes\": %ld, "
                    "\"rx_err_made\": %ld, \"extra_frames\": %ld, \"collisions_extra\": %ld, \"extra_measured\": %ld, "
                    "\"slot_end_us\": %u, \"clock_err_us\": [%.0f, %.0f], \"made\": {",
            n_nodes, seconds, p_loss, frames, collisions, lost_random, lost_blocked, 100 * air_used_us / end,
            leader_changes_total, rx_err_made, extra_frames, collisions_extra, extra_measured,
            TW_SLOT_US + TW_SLOT_SLACK_US, clk_n ? clk_sum / clk_n : 0.0, clk_max);
    int first = 1;
    for (int rx = 1; rx <= n_nodes; rx++)
        for (int tx = 1; tx <= n_nodes; tx++)
            if (N_[rx].made[tx]) {
                fprintf(stderr, "%s\"%d-%d\": %ld", first ? "" : ", ", tx, rx, N_[rx].made[tx]);
                first = 0;
            }
    fprintf(stderr, "}, \"dropped\": {");
    for (int id = 1; id <= n_nodes; id++)
        fprintf(stderr, "%s\"%d\": %lu", id > 1 ? ", " : "", id, (unsigned long)N_[id].st.n_dropped);
    fprintf(stderr, "}, \"relayed_out\": {");
    for (int id = 1; id <= n_nodes; id++)
        fprintf(stderr, "%s\"%d\": [%lu, %lu]", id > 1 ? ", " : "", id, (unsigned long)N_[id].st.n_relayed_out,
                (unsigned long)N_[id].st.n_relay_full);
    fprintf(stderr, "}, \"extra\": {");
    for (int id = 1; id <= n_nodes; id++)
        fprintf(stderr, "%s\"%d\": [%lu, %lu, %d, %ld]", id > 1 ? ", " : "", id, (unsigned long)N_[id].st.n_extra_sent,
                (unsigned long)N_[id].st.n_extra_skipped, N_[id].st.n_extra_sent ? N_[id].extra_end_max : 0,
                N_[id].extra_late);
    fprintf(stderr, "}, \"settings\": [");
    for (int k = 0; k < n_sets; k++)
        fprintf(stderr, "%s{\"t\": %.3f, \"dbm\": %.2f, \"at\": %d, \"seq\": %u, \"all_after_s\": %.3f}",
                k ? ", " : "", sets[k].t / 1e6, sets[k].q / 4.0, sets[k].at, sets[k].seq,
                sets[k].done_t >= 0 ? (sets[k].done_t - sets[k].t) / 1e6 : -1.0);
    fprintf(stderr, "], \"final_power\": {");
    for (int id = 1; id <= n_nodes; id++)
        fprintf(stderr, "%s\"%d\": [%u, %u]", id > 1 ? ", " : "", id, N_[id].st.set_seq, N_[id].st.set_power);
    fprintf(stderr, "}}\n");
    if (out != stdout) fclose(out);
    return 0;
}
