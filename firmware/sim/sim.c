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
 *       [--set 0@cap=3,0@second=off,...] [--late measured] [--fix ahead,hwrx,cb]
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
 * --late measured|KEY=VALUE,...: the delays the real boards measured (tw_node 2.2, TWF,
 * 2026-11-07), instead of the ideal radio above. A board builds its frame (and
 * stamps its time) at its slot time; hands it to ESP-NOW hand=340 us later; the radio puts it
 * on the air later still and calls send-done post=100 us after the air; a receiver's CSI
 * callback (its record's time) comes csicb=100 us after the air and its ESP-NOW receive
 * callback (its clock) rxcb=500 us after (fitted to lag_check.py on the real boards; a
 * record's rx_us: the frame's start); handed-over to send-done, less the air, is over1=650 us for a normal
 * frame and over2=290 us for an extra one; from send-done to handing the extra frame over,
 * wake=250 us (its length is decided at the send-done). Now and then (stall=0.002 of frames)
 * the radio is 1-5 ms late (stallmax=5000). A radio that finds the channel busy waits for it
 * (DIFS and a random backoff) instead of colliding; two frames starting within 9 us collide.
 * The summary adds, per board, where its frames were on the air in its slot by the
 * timekeeper's clock (the truth), and what its TWF would say.
 * --fix ahead,clock|hwrx,cb|queue: what tw_node 2.3 would change (implies --late). ahead: the frame
 * built ahead and handed over at its time (0-40 us late); clock: each frame says the least
 * stamp-to-send-done delay of its sender's last 16 (20 us steps) and receivers add it to the
 * clock; hwrx: as clock, and receivers take the radio's receive time (the frame's start) plus
 * the air instead of the callback's time, less 100 us for the sender's send-done delay; cb: the extra frame sent from the send-done itself, cut for xbudget=310 us of
 * access; queue: the extra frame built at once and handed over behind the normal one, cut for
 * the normal frame's worst start. timing_cases.py runs the cases.
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
typedef struct { long n; double sum, max; } lstat_t;
static void lst(lstat_t *a, double v) { a->n++; a->sum += v; if (a->n == 1 || v > a->max) a->max = v; }
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
    /* --late: a built frame waits for the air */
    double air_t;               /* true time it asks for the air (< 0: none) */
    int pend_len, pend_extra;
    uint32_t pend_corr;         /* --fix clock: the delay it says it had, us */
    uint8_t pend[TW_MAX_PAYLOAD];
    int q_len;                  /* --fix queue: the extra frame, built at once, behind the normal one */
    uint8_t qbuf[TW_MAX_PAYLOAD];
    double stamp_t, hand_t, hand2_t;
    uint32_t m_ring[16];        /* --fix clock: its own stamp to send-done, less the air, us */
    int m_n, m_pos;
    lstat_t s_start, s_xend, s_defer, f_at, f_radio, f_wake, f_xradio, f_xend, f_len, f_xlen;
    long x_over, x_into;
} simnode_t;

static simnode_t N_[MAXN + 1];
static int n_nodes = 3, realtime = 0, exits[MAXN + 1];
static double seconds = 60, p_loss = 0.05, p_rxerr = 0, t0_unix;
static double walk_a = 30e6, walk_b = 40e6;    /* planted walk across the 1-2 link (--walk A-B seconds) */
static long collisions = 0, frames = 0, lost_random = 0, lost_blocked = 0, rx_err_made = 0;
static double air_used_us = 0;
static long extra_frames = 0, collisions_extra = 0, extra_measured = 0;
/* --late: the delays the boards measured (tw_node 2.2, TWF), us; --fix: what 2.3 would change */
static int late = 0, fix_ahead = 0, fix_clock = 0, fix_x = 0, fix_hwrx = 0;     /* fix_x: 1 cb, 2 queue */
static double hand_lo = 270, hand_hi = 430;     /* slot time -> handed to ESP-NOW (the build and more) */
static double post_lo = 50, post_hi = 150;      /* end of the air -> send-done */
static double csi_lo = 50, csi_hi = 150;        /* end of the air -> a receiver's CSI callback (its records) */
static double rxcb_lo = 450, rxcb_hi = 550;     /* end of the air -> its ESP-NOW receive callback (the clock): fitted
                                                   to lag_check.py on the boards, 1.1-1.4 ms lag */
static double over1 = 650, over2 = 290;         /* handed -> send-done, less the air: normal / extra frame */
static double wake_lo = 170, wake_hi = 330;     /* send-done -> extra frame handed over (our task) */
static double stall_p = 0.002, stall_lo = 1000, stall_hi = 5000;   /* now and then the radio is late */
static double xbudget = 310;                    /* --fix: allowed for an extra frame's access */
static long deferrals = 0;
static double urand(void);
static double ur(double lo, double hi);
static double csma(void);
static double pre_draw(double over)
{
    /* the part of the overhead before the air: overhead less the send-done delay and the mean
       channel access (drawn on its own), +-100 us */
    double fixed = over - (post_lo + post_hi) / 2 - (34 + 7.5 * 9);
    return (fixed < 0 ? 0 : ur(fixed - 100 < 0 ? 0 : fixed - 100, fixed + 100)) + csma() +
           (urand() < stall_p ? ur(stall_lo, stall_hi) : 0);
}
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

static double ur(double lo, double hi) { return lo + urand() * (hi - lo); }
static double csma(void) { return 34 + (int)(urand() * 16) * 9; }   /* DIFS + backoff, 9 us slots */
static int first_exit(void) { for (int id = 1; id <= n_nodes; id++) if (exits[id]) return id; return 1; }

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
        else if (!strcmp(argv[i], "--late") && i + 1 < argc) {
            late = 1;                   /* "measured", or KEY=VALUE,... over the measured values */
            for (char *p = argv[++i]; *p;) {
                double v = 0;
                char *eq = strchr(p, '=');
                if (eq) v = strtod(eq + 1, NULL);
                if (!strncmp(p, "hand=", 5)) { hand_lo = v - 80; hand_hi = v + 80; }
                else if (!strncmp(p, "post=", 5)) { post_lo = v * 0.5; post_hi = v * 1.5; }
                else if (!strncmp(p, "rxcb=", 5)) { rxcb_lo = v < 50 ? 0 : v - 50; rxcb_hi = v + 50; }
                else if (!strncmp(p, "csicb=", 6)) { csi_lo = v < 50 ? 0 : v - 50; csi_hi = v + 50; }
                else if (!strncmp(p, "over1=", 6)) over1 = v;
                else if (!strncmp(p, "over2=", 6)) over2 = v;
                else if (!strncmp(p, "wake=", 5)) { wake_lo = v * 0.7; wake_hi = v * 1.3; }
                else if (!strncmp(p, "stall=", 6)) stall_p = v;
                else if (!strncmp(p, "stallmax=", 9)) stall_hi = v;
                else if (!strncmp(p, "xbudget=", 8)) xbudget = v;
                else if (strncmp(p, "measured", 8)) { fprintf(stderr, "--late: unknown %s\n", p); return -1; }
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
        else if (!strcmp(argv[i], "--fix") && i + 1 < argc) {
            late = 1;
            for (char *p = argv[++i]; *p;) {
                if (!strncmp(p, "ahead", 5)) fix_ahead = 1;
                else if (!strncmp(p, "clock", 5)) fix_clock = 1;
                else if (!strncmp(p, "hwrx", 4)) fix_hwrx = fix_clock = 1;
                else if (!strncmp(p, "cb", 2)) fix_x = 1;
                else if (!strncmp(p, "queue", 5)) fix_x = 2;
                else { fprintf(stderr, "--fix: unknown %s\n", p); return -1; }
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
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
        s->air_t = -1;
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
    uint32_t last_corr = 0;
    double last_start = 0;
    long leader_changes_total = 0;
    struct timespec wall0;
    clock_gettime(CLOCK_MONOTONIC, &wall0);

    while (t < end) {
        /* next sender: a board's slot, or the extra frame it has pending */
        /* ev: 0 build the normal frame, 1 build the extra one, 2 (--late) a built frame asks for the air */
        int who = 0, is_extra = 0, ev = 0;
        double when = 0;
        for (int id = 1; id <= n_nodes; id++) {
            double tt = N_[id].next_tx;
            int e = 0;
            if (N_[id].extra_at >= 0 && N_[id].extra_at <= tt) { tt = N_[id].extra_at; e = 1; }
            if (N_[id].air_t >= 0 && N_[id].air_t <= tt) { tt = N_[id].air_t; e = 2; }
            if (!who || tt < when) { who = id; when = tt; ev = e; }
        }
        simnode_t *s = &N_[who];
        is_extra = ev == 1;
        double tx_t = late ? when
                    : is_extra ? when + urand() * TW_ACCESS_US      /* channel access after send-done */
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
                /* --late: its CSI callback (the record's time) and, later, its ESP-NOW receive callback
                   (the clock, hearing, relaying) */
                double rx_t = late ? busy_until + ur(csi_lo, csi_hi) : busy_until;
                double cb_t = late ? rx_t + ur(rxcb_lo, rxcb_hi) - (csi_lo + csi_hi) / 2 : busy_until;
                if (cb_t < rx_t) cb_t = rx_t;
                uint32_t now_l = local_us(r, rx_t), cb_l = local_us(r, cb_t);
                /* --late: the radio's own receive time (rx_ctrl timestamp) taken as the frame's start */
                uint32_t hw_l = late ? local_us(r, last_start) : now_l;
                tw_hdr_t h;
                if (tw_peek_header(last_frame, last_len, &h) < 0) { r->st.n_bad_frames++; continue; }
                tw_hdr_t hc = h;
                hc.net_us += last_corr;                      /* --fix clock: the delay it says it had */
                if (fix_hwrx)   /* the clock from the radio's receive time (the frame's start) + the air,
                                   not the callback; the sender's delay to its send-done, less 100 us */
                    hc.net_us += (uint32_t)(cb_l - hw_l) - tw_air_us(last_len) - 100u;
                tw_note_frame(&r->st, &hc, cb_l);
                int8_t csi[CSI_LEN];
                make_csi(id, h.src, busy_until, csi);
                int compact = __builtin_popcount(tw_active(&r->st, cb_l)) > TW_FULL_MAX_ACTIVE;
                uint8_t phy = (uint8_t)(1 | (1 << 3) | (2 << 5));   /* HT40, HT, secondary below */
                int8_t rssi = (int8_t)(-50 - 3 * abs(id - h.src));
                uint8_t kflags = (p_rxerr > 0 && urand() < p_rxerr) ? TW_KIND_RXERR : 0;
                if (h.flags & TW_F_EXTRA) kflags |= TW_KIND_EXTRA;
                const tw_rec_t *rec = tw_add_measurement(&r->st, now_l, h.src, h.seq, hw_l, rssi, -92, 30, 0,
                                                         phy, kflags, csi, CSI_LEN, 1.0f, compact);
                if (rec) {
                    rx_err_made += (kflags & TW_KIND_RXERR) != 0;
                    extra_measured += (kflags & TW_KIND_EXTRA) != 0;
                    r->made[h.src]++;
                }
                rctx_t c = { r, cb_t };
                cur_src = h.src;
                if (r->exit) {
                    char line[600];
                    if (rec && tw_format_record(rec, line, sizeof(line)) > 0) emit(rx_t, line);
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
        uint32_t corr = 0;
        if (ev == 2) {                                       /* --late: a built frame asks for the air */
            if (dead) { s->air_t = -1; s->q_len = 0; t = tx_t; continue; }
            if (last_len && tx_t < busy_until && tx_t - last_start >= 9) {
                double na = busy_until + csma();             /* the radio hears the channel busy: waits */
                if (tx_t > 10e6) lst(&s->s_defer, na - tx_t);
                deferrals++;
                s->air_t = na;
                t = tx_t;
                continue;
            }
            s->air_t = -1;
            len = s->pend_len;
            is_extra = s->pend_extra;
            corr = s->pend_corr;
            memcpy(frame, s->pend, (size_t)len);
        } else if (is_extra) {
            s->extra_at = -1;
            if (!dead) {
                uint32_t bl = local_us(s, when);
                if (fix_x == 1) bl += (uint32_t)(xbudget - TW_ACCESS_US);   /* 2.3: its own budget */
                len = tw_build_extra(&s->st, bl, frame, TW_MAX_PAYLOAD);
            }
            if (late) {
                if (len > 0) {
                    memcpy(s->pend, frame, (size_t)len);
                    s->pend_len = len;
                    s->pend_extra = 1;
                    s->pend_corr = 0;
                    s->air_t = s->hand2_t + pre_draw(over2);
                }
                t = tx_t;
                continue;
            }
        } else if (!dead) {
            for (const tw_rec_t *a; (a = tw_aux_record(&s->st, lnow)) != NULL;) {    /* firmware: once a second */
                char line[600];
                if (s->exit && tw_format_record(a, line, sizeof(line)) > 0) emit(tx_t, line);
            }
            len = tw_build_frame(&s->st, lnow, frame, TW_MAX_PAYLOAD);
            if (late && len > 0) {
                /* built now (its time stamped now), handed to ESP-NOW later, on the air later still */
                s->stamp_t = tx_t;
                s->hand_t = tx_t + (fix_ahead ? ur(0, 40) : ur(hand_lo, hand_hi));
                memcpy(s->pend, frame, (size_t)len);
                s->pend_len = len;
                s->pend_extra = 0;
                s->pend_corr = 0;
                if (fix_clock && s->m_n) {                   /* the least of its last 16, 20 us steps */
                    uint32_t m = s->m_ring[0];
                    for (int k = 1; k < s->m_n; k++) if (s->m_ring[k] < m) m = s->m_ring[k];
                    s->pend_corr = m > 5100 ? 5100 : m / 20 * 20;
                }
                s->air_t = s->hand_t + pre_draw(over1);
                if (s->st.synced && tx_t > 10e6) {
                    lst(&s->f_at, tw_slot_pos(&s->st, local_us(s, s->hand_t), (uint8_t)who));
                    lst(&s->f_len, len);
                }
                s->q_len = 0;
                if (fix_x == 2 && s->st.extra_planned) {
                    /* 2.3 "queue": the extra frame built at once and handed over behind the normal
                       one; cut for the normal frame's worst start and a 219 us gap after it */
                    double fixed = over1 - (post_lo + post_hi) / 2 - (34 + 7.5 * 9);
                    double pre_max = (fixed < 0 ? 0 : fixed) + 100 + 34 + 135;
                    uint32_t bl = local_us(s, s->hand_t + pre_max + tw_air_us(len)) + (219 - TW_ACCESS_US);
                    s->q_len = tw_build_extra(&s->st, bl, s->qbuf, TW_MAX_PAYLOAD);
                    s->hand2_t = s->hand_t + 20;
                }
                len = 0;                                     /* nothing on the air yet */
            }
        }
        if (len > 0) {
            double air = tw_air_us(len);
            double sd = tx_t + air + (late ? ur(post_lo, post_hi) : 0);   /* its send-done */
            air_used_us += air;
            frames++;
            if (is_extra) {
                extra_frames++;
                tw_note_sent(&s->st, local_us(s, sd), 1);
                int32_t end_pos = tw_slot_pos(&s->st, local_us(s, sd), (uint8_t)who);
                if (end_pos > s->extra_end_max) s->extra_end_max = end_pos;
                if (end_pos > (int32_t)(TW_SLOT_US + TW_SLOT_SLACK_US)) s->extra_late++;
            }
            if (late && tx_t > 10e6 && s->st.synced) {
                /* the truth: where in its slot it was on the air, by the timekeeper's clock; and
                   what the board itself would report (TWF) */
                int tk = N_[first_exit()].st.leader;
                if (tk >= 1 && tk <= n_nodes) {
                    simnode_t *L = &N_[tk];
                    if (is_extra) {
                        double e = tw_slot_pos(&L->st, local_us(L, tx_t + air), (uint8_t)who);
                        lst(&s->s_xend, e);
                        s->x_over += e > TW_SLOT_US + TW_SLOT_SLACK_US;
                        s->x_into += e > TW_SLOT_US + TW_GUARD_US;
                        lst(&s->f_xradio, sd - s->hand2_t);
                        lst(&s->f_xend, tw_slot_pos(&s->st, local_us(s, sd), (uint8_t)who));
                        lst(&s->f_xlen, len);
                    } else {
                        lst(&s->s_start, tw_slot_pos(&L->st, local_us(L, tx_t), (uint8_t)who));
                        lst(&s->f_radio, sd - s->hand_t);
                    }
                }
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
                last_corr = corr;
                last_start = tx_t;
                busy_until = tx_t + air;
                busy_src = who;
            }
            if (!late && !is_extra && s->st.extra_planned) {
                s->extra_at = tx_t + air + 50;               /* ESP-NOW's send-done, then the build */
            }
            if (late && !is_extra) {
                uint32_t m = (uint32_t)(local_us(s, sd) - local_us(s, s->stamp_t)) - (uint32_t)air;
                s->m_ring[s->m_pos] = m;
                s->m_pos = (s->m_pos + 1) % 16;
                if (s->m_n < 16) s->m_n++;
                if (fix_x == 2) {
                    if (s->q_len > 0) {                      /* queued behind it: goes right after */
                        memcpy(s->pend, s->qbuf, (size_t)s->q_len);
                        s->pend_len = s->q_len;
                        s->pend_extra = 1;
                        s->pend_corr = 0;
                        s->air_t = tx_t + air + 50 + csma() + (urand() < stall_p ? ur(stall_lo, stall_hi) : 0);
                        s->q_len = 0;
                    }
                } else if (s->st.extra_planned) {
                    if (fix_x == 1) {                        /* 2.3 "cb": sent from the send-done itself */
                        s->extra_at = sd + ur(0, 20);
                        s->hand2_t = s->extra_at;
                    } else {                                 /* 2.2: our task wakes, builds, hands over */
                        /* the length is decided as the build starts, right after the send-done
                           (the real boards' second frames are as long as that gives): most of
                           the wake is the build */
                        s->extra_at = sd + ur(0, 20);
                        s->hand2_t = sd + ur(wake_lo, wake_hi);
                    }
                    if (tx_t > 10e6 && s->st.synced) lst(&s->f_wake, s->hand2_t - sd);
                }
            }
        }
        if (is_extra || ev == 2) {
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
    if (late) {
        fprintf(stderr, "}, \"deferrals\": %ld, \"exit_board\": %d, \"late\": {", deferrals, first_exit());
        for (int id = 1; id <= n_nodes; id++) {
            simnode_t *q = &N_[id];
#define LS(a) (a).n ? (a).sum / (a).n : 0.0, (a).max
            fprintf(stderr, "%s\"%d\": {\"start\": [%.0f, %.0f], \"xend\": [%.0f, %.0f], \"x_over\": %ld, \"x_into\": %ld, "
                            "\"n_x\": %ld, \"n\": %ld, \"defer\": [%ld, %.0f, %.0f], \"tx_at\": [%.0f, %.0f], "
                            "\"tx_radio\": [%.0f, %.0f], \"x_wake\": [%.0f, %.0f], \"x_radio\": [%.0f, %.0f], "
                            "\"x_end_own\": [%.0f, %.0f], \"tx_len\": %.0f, \"x_len\": %.0f}",
                    id > 1 ? ", " : "", id, LS(q->s_start), LS(q->s_xend), q->x_over, q->x_into, q->s_xend.n,
                    q->s_start.n, q->s_defer.n, LS(q->s_defer), LS(q->f_at), LS(q->f_radio), LS(q->f_wake),
                    LS(q->f_xradio), LS(q->f_xend), q->f_len.n ? q->f_len.sum / q->f_len.n : 0.0,
                    q->f_xlen.n ? q->f_xlen.sum / q->f_xlen.n : 0.0);
#undef LS
        }
    }
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
