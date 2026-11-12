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
 *       [--update T@TARGET:SIZE] [--still A-B] [--walk A-B[@X-Y]]
 *
 * --update T@TARGET:SIZE (4.0): at T seconds the computer has given the first exit a program of
 * SIZE bytes (made up), and it sends it to TARGET (0: every board of its group) over the air. A
 * board with every chunk checks it against the program (the firmware: SHA-256 and signature),
 * "writes" it (READY) and restarts 3 s later (off for 4 s: a reboot). The JSON summary says when
 * each board had it, how many update frames and bursts it took, and when the sending ended.
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
 * --fix ahead,hwrx,cb|queue: what tw_node 2.3 changes (implies --late). ahead: the frame built
 * ahead and handed over at its time (0-40 us late); hwrx: tw_proto.c's 2.3 clock (each frame
 * says its sender's least stamp-to-send-done delay, header byte 18 in 3.0, and receivers time it by the
 * radio's own receive time, its start, as the board learns it from the CSI callbacks); cb: as
 * tw_node 2.3's firmware, the extra frame built while the normal one is on the air, cut for a
 * send-done at the running mean + 2 spreads of the board's own and xbudget=310 us of access,
 * sent from the send-done if it still fits there, else dropped and its records put back; queue: the extra frame built at once and handed over behind the normal one, cut for
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
    double away_from, away_to;  /* 3.1: away surveying the channels (silent and deaf, keeps its state) */
    uint8_t radio;              /* 3.2: the channel its radio is on (boards on others do not hear it) */
    int away_done;
    double last_status;
    double node_last[MAXN + 1];
    int8_t node_rssi[MAXN + 1];
    tw_hdr_t node_hdr[MAXN + 1];
    long made[MAXN + 1];        /* ground truth: measurements made of each sender */
    /* --late: a built frame waits for the air */
    double air_t;               /* true time it asks for the air (< 0: none) */
    int pend_len, pend_extra;
    uint8_t pend[TW_MAX_PAYLOAD];
    int q_len;                  /* --fix queue: the extra frame, built at once, behind the normal one */
    uint8_t qbuf[TW_MAX_PAYLOAD];
    double stamp_t, hand_t, hand2_t;
    double up_at;               /* 4.0: true time the exit asks for its next update frame (< 0: none) */
    lstat_t s_start, s_xend, s_defer, f_at, f_radio, f_wake, f_xradio, f_xend, f_len, f_xlen;
    long x_over, x_into;
    double ovh, ovh_dev;        /* --fix cb: hand-over to send-done less the air, running mean / spread */
    uint32_t x_latest;          /* --fix cb: the extra frame built ahead may start by then (own clock) */
} simnode_t;

static simnode_t N_[MAXN + 1];
static int n_nodes = 3, realtime = 0, exits[MAXN + 1];
static double seconds = 60, p_loss = 0.05, p_rxerr = 0, t0_unix;
static double walk_a = 30e6, walk_b = 40e6;    /* planted walk across the 1-2 link (--walk A-B seconds) */
static int walk_x = 1, walk_y = 2;              /* 4.3: --walk A-B@X-Y: across link X-Y instead */
static double up_t = -1;                        /* 4.0: --update T@TARGET:SIZE */
static int up_target = 0, up_fired = 0;
static uint32_t up_size = 0;
static uint8_t *up_src;                         /* the program the computer gave */
static double up_done_t[MAXN + 1], up_end_t = -1;
static int up_state_end[MAXN + 1];
static long up_frames_air = 0;
void *tw_up_alloc(uint32_t size) { return malloc(size); }
void tw_up_free(void *p) { free(p); }
static double still_a = -1, still_b = -1;       /* 3.5: --still A-B: link 1-3 has a fan going (part of its
                                                   quiet) except between A and B s: steadier than quiet */
static long collisions = 0, frames = 0, lost_random = 0, lost_blocked = 0, rx_err_made = 0;
static double air_used_us = 0;
static long extra_frames = 0, collisions_extra = 0, extra_measured = 0;
/* --late: the delays the boards measured (tw_node 2.2, TWF), us; --fix: what 2.3 would change */
static int late = 0, fix_ahead = 0, fix_x = 0, fix_hwrx = 0;     /* fix_x: 1 cb, 2 queue */
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
        if (t > walk_a && t < walk_b && ((rx == walk_x && tx == walk_y) || (rx == walk_y && tx == walk_x))) {
            motion = 0.25 * sin(0.9 * b + t / 1e5);
        }
        if (still_a >= 0 && !(t > still_a && t < still_b) && ((rx == 1 && tx == 3) || (rx == 3 && tx == 1))) {
            motion = 0.2 * (urand() - 0.5);   /* the fan: restless all the time, but nothing walking */
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
    tw_setting_heard(&c->self->st, r);              /* 2.5+: as the firmware's relay_cb */
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

/* 2.5+: --give T@TARGET:KEY:VAL:GEN,...: the exit gives a setting or action by record at T
   seconds (keys: tw_proto.h TW_K_*; try's VAL = R*65536 + G*256 + B); the summary says when each
   board took it */
#define MAXGIVE 8
static struct { double t; int target, key, val, gen, done; } give[MAXGIVE];
static int n_give;
static double took_t[MAXN + 1][TW_K_N];

static char group_spec[128];                /* 3.3: "FROM-TO:GROUP[:CHANNEL],...": boards in a group (on a channel) */

static int parse_args(int argc, char **argv, char *kill_spec, char *exit_spec, unsigned *seed, char **outpath)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nodes") && i + 1 < argc) n_nodes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc) p_loss = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rxerr") && i + 1 < argc) p_rxerr = atof(argv[++i]);
        else if (!strcmp(argv[i], "--exit") && i + 1 < argc) strncpy(exit_spec, argv[++i], 63);
        else if (!strcmp(argv[i], "--group") && i + 1 < argc) strncpy(group_spec, argv[++i], 127);
        else if (!strcmp(argv[i], "--kill") && i + 1 < argc) strncpy(kill_spec, argv[++i], 255);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) *seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--realtime")) realtime = 1;
        else if (!strcmp(argv[i], "--no-hdr-set")) ;      /* 2.6's test; 3.0: always so */
        else if (!strcmp(argv[i], "--give") && i + 1 < argc) {
            for (char *p = argv[++i]; *p && n_give < MAXGIVE;) {
                if (sscanf(p, "%lf@%d:%d:%d:%d", &give[n_give].t, &give[n_give].target, &give[n_give].key,
                           &give[n_give].val, &give[n_give].gen) == 5) n_give++;
                else { fprintf(stderr, "--give T@TARGET:KEY:VAL:GEN\n"); return -1; }
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
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
                else if (!strncmp(p, "hwrx", 4)) fix_hwrx = 1;
                else if (!strncmp(p, "cb", 2)) fix_x = 1;
                else if (!strncmp(p, "queue", 5)) fix_x = 2;
                else { fprintf(stderr, "--fix: unknown %s\n", p); return -1; }
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
        else if (!strcmp(argv[i], "--walk") && i + 1 < argc) {
            char *e; walk_a = strtod(argv[++i], &e) * 1e6; walk_b = (*e == '-' ? strtod(e + 1, &e) : walk_a / 1e6 + 10) * 1e6;
            if (*e == '@') sscanf(e + 1, "%d-%d", &walk_x, &walk_y);
        }
        else if (!strcmp(argv[i], "--update") && i + 1 < argc) {
            int tg; unsigned sz;
            if (sscanf(argv[++i], "%lf@%d:%u", &up_t, &tg, &sz) != 3 || sz == 0 || sz > TW_UP_MAX) {
                fprintf(stderr, "--update T@TARGET:SIZE (SIZE up to %u)\n", TW_UP_MAX); return -1;
            }
            up_target = tg; up_size = sz;
        }
        else if (!strcmp(argv[i], "--still") && i + 1 < argc) {
            char *e; still_a = strtod(argv[++i], &e) * 1e6; still_b = (*e == '-' ? strtod(e + 1, NULL) : still_a / 1e6 + 60) * 1e6;
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
        s->away_from = s->away_to = -1;
        s->next_tx = urand() * 20000;
        s->extra_at = -1;
        s->air_t = -1;
        s->ovh = 650;
        s->ovh_dev = 60;
        s->last_status = -1e9;
        s->up_at = -1;
        tw_init(&s->st, (uint8_t)id, (uint8_t)s->exit, seed * 31u + (unsigned)id);
        tw_set_info(&s->st, "sim|board");
        s->st.set_channel = 11;                          /* 3.2: the firmware's default */
        for (char *p = group_spec; *p;) {                /* 3.3: --group FROM-TO:GROUP[:CHANNEL] */
            int a, b, g, c = 0;
            int got = sscanf(p, "%d-%d:%d:%d", &a, &b, &g, &c);
            if (got >= 3 && id >= a && id <= b) {
                s->st.group = (uint8_t)g;
                if (got == 4 && c >= 1 && c <= 11) s->st.set_channel = (uint8_t)c;
            }
            char *q = strchr(p, ',');
            if (!q) break;
            p = q + 1;
        }
        tw_net_init(&s->st, NULL);                       /* the network keys (3.0: their only way) */
        tw_chan_init(&s->st, local_us(s, 0));
        s->radio = s->st.chan_home;
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
        /* 4.0: the computer gives the first exit a program, and has it send it */
        if (!up_fired && up_t >= 0 && t >= up_t * 1e6) {
            up_fired = 1;
            simnode_t *x = &N_[first_exit()];
            up_src = malloc(up_size);
            for (uint32_t i = 0; i < up_size; i++) up_src[i] = (uint8_t)(urand() * 256);
            uint8_t man[TW_UP_MAN_LEN];
            for (int i = 0; i < TW_UP_MAN_LEN; i++) man[i] = (uint8_t)(urand() * 256);
            if (tw_up_begin(&x->st, up_size, local_us(x, t)) == 0) {
                for (uint32_t i = 0; i * TW_UP_CHUNK < up_size; i++) {
                    uint32_t l = up_size - i * TW_UP_CHUNK < TW_UP_CHUNK ? up_size - i * TW_UP_CHUNK : TW_UP_CHUNK;
                    tw_up_chunk(&x->st, (uint16_t)i, up_src + i * TW_UP_CHUNK, (int)l);
                }
                tw_up_manifest(&x->st, man, local_us(x, t));
                tw_up_tick(&x->st, local_us(x, t) + 1500000);   /* a tick before the check (4.0 dropped it here) */
                if (x->st.up_state != TW_UP_ST_RECEIVING) { fprintf(stderr, "the exit lost the update before its check\n"); return 1; }
                tw_up_set_state(&x->st, TW_UP_ST_STAGED, local_us(x, t));    /* the firmware checked it */
                if (tw_up_send(&x->st, local_us(x, t), (uint8_t)up_target) == 0) x->up_at = t;
            }
        }
        for (int id = 1; up_fired && id <= n_nodes; id++) {   /* the firmware's part: check, write, restart */
            simnode_t *r = &N_[id];
            if (r->st.up_state != TW_UP_ST_COMPLETE) continue;
            int ok = r->st.up_buf && r->st.up_size == up_size && !memcmp(r->st.up_buf, up_src, up_size);
            tw_up_set_state(&r->st, ok ? TW_UP_ST_READY : TW_UP_ST_BADHASH, local_us(r, t));
            up_done_t[id] = t / 1e6;
            up_state_end[id] = ok ? TW_UP_ST_READY : TW_UP_ST_BADHASH;
            if (ok && r->dead_from < 0) { r->dead_from = t + 3e6; r->dead_to = t + 7e6; }   /* restarts */
        }
        if (up_fired && up_end_t < 0 && !N_[first_exit()].st.up_tx_on && t > up_t * 1e6) up_end_t = t / 1e6;
        /* 2.5: settings given at the exit, and which boards took them when */
        for (int g = 0; g < n_give; g++) {
            if (!give[g].done && t >= give[g].t * 1e6) {
                give[g].done = 1;
                int ex = first_exit(), k = give[g].key, v = give[g].val;
                /* the value as the key takes it: an LED key 1 byte, try 3 (VAL = R*65536 + G*256 + B),
                   test none */
                uint8_t vb[3] = {(uint8_t)v, 0, 0};
                int vl = tw_key_class((uint8_t)k) == TW_KC_BOARD || tw_key_class((uint8_t)k) == TW_KC_NETWORK ? 1 : 0;
                if (k == TW_K_LED_TRY) { vb[0] = (uint8_t)(v >> 16); vb[1] = (uint8_t)(v >> 8); vb[2] = (uint8_t)v; vl = 3; }
                tw_setting_give(&N_[ex].st, (uint8_t)give[g].target, (uint8_t)k, vb, vl, (uint8_t)give[g].gen);
            }
        }
        for (int id = 1; id <= n_nodes; id++) {          /* when each board took a key (newest) */
            uint32_t ch = N_[id].st.set_changed;
            N_[id].st.set_changed = 0;
            for (int k = 1; k < TW_K_N; k++)
                if (ch & (1u << k)) took_t[id][k] = t / 1e6;
            if (ch & (1u << TW_K_SURVEY)) {              /* 3.1: away in 5 s, for the survey's ~12 s */
                N_[id].away_from = t + TW_SURVEY_DELAY_US;
                N_[id].away_to = N_[id].away_from + TW_SURVEY_N * (TW_SURVEY_DWELL_MS + 25) * 1000.0 + 200000;
                N_[id].away_done = 0;
            }
            simnode_t *a = &N_[id];
            if (a->away_to >= 0 && !a->away_done && t >= a->away_to) {   /* back: its (made-up) result */
                uint8_t res[TW_SURVEY_N * TW_SURVEY_ENTRY];
                memset(res, 0, sizeof(res));
                for (int c = 0; c < TW_SURVEY_N; c++) {
                    int busy = c == 5 ? 80 : (c + id) % 4 * 3;          /* channel 6 busy, others a little */
                    res[c * TW_SURVEY_ENTRY] = (uint8_t)busy;
                    if (busy) {
                        res[c * TW_SURVEY_ENTRY + 1] = (uint8_t)(busy * 3);
                        res[c * TW_SURVEY_ENTRY + 3] = (uint8_t)(int8_t)(-60 - c);
                        res[c * TW_SURVEY_ENTRY + 4] = (uint8_t)(int8_t)(-75 - c);
                        res[c * TW_SURVEY_ENTRY + 5] = (uint8_t)(int8_t)-95;
                    }
                }
                tw_survey_done(&a->st, local_us(a, t), a->st.set_gen[TW_K_SURVEY], res, TW_SURVEY_N);
                a->away_done = 1;
            }
        }
        /* next sender: a board's slot, or the extra frame it has pending */
        /* ev: 0 build the normal frame, 1 build the extra one, 2 (--late) a built frame asks for the air */
        int who = 0, is_extra = 0, ev = 0;
        double when = 0;
        for (int id = 1; id <= n_nodes; id++) {
            double tt = N_[id].next_tx;
            int e = 0;
            if (N_[id].extra_at >= 0 && N_[id].extra_at <= tt) { tt = N_[id].extra_at; e = 1; }
            if (N_[id].air_t >= 0 && N_[id].air_t <= tt) { tt = N_[id].air_t; e = 2; }
            if (N_[id].up_at >= 0 && N_[id].up_at <= tt) { tt = N_[id].up_at; e = 3; }
            if (!who || tt < when) { who = id; when = tt; ev = e; }
        }
        simnode_t *s = &N_[who];
        is_extra = ev == 1;
        double tx_t = late || ev == 3 ? when
                    : is_extra ? when + urand() * TW_ACCESS_US      /* channel access after send-done */
                               : s->next_tx + urand() * 300;        /* queueing delay before the air */
        /* deliver the previous frame if it ended before this one starts */
        if (last_len && busy_until <= tx_t) {
            /* receivers */
            for (int id = 1; id <= n_nodes; id++) {
                simnode_t *r = &N_[id];
                int dead = r->dead_from >= 0 && busy_until >= r->dead_from && (r->dead_to < 0 || busy_until < r->dead_to);
                dead |= r->away_from >= 0 && busy_until >= r->away_from && busy_until < r->away_to;
                if (id == busy_src || dead || last_collided) continue;
                if (r->radio != N_[busy_src].radio) continue;    /* 3.2: another channel */
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
                if (tw_up_rx(&r->st, last_frame, last_len, now_l)) continue;     /* 4.0: an update frame */
                tw_hdr_t h;
                if (tw_peek_header(last_frame, last_len, &h) < 0) { r->st.n_bad_frames++; continue; }
                if (!tw_frame_ours(&r->st, &h)) continue;        /* 3.3: another group's */
                /* --fix hwrx (tw_node 2.3): the clock from the frame's start by the radio's own receive
                   time, as the board knows it: the offset to its clock is learned from the CSI
                   callbacks, so it comes out late by their least delay */
                tw_note_frame(&r->st, &h, cb_l, fix_hwrx ? hw_l + (uint32_t)csi_lo : 0u);
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
        int dead_real = dead;                        /* switched off: reboots when back */
        dead |= s->away_from >= 0 && tx_t >= s->away_from && tx_t < s->away_to;   /* 3.1: surveying */
        uint32_t lnow = local_us(s, tx_t);
        if (!dead_real) {                            /* 3.2: its channel; silent while looking */
            int quiet;
            s->radio = tw_chan_step(&s->st, lnow, &quiet);
            dead |= quiet;
        }
        int hushed = !dead && tw_up_hush(&s->st, lnow);   /* 4.0: update frames on the air: quiet */
        /* the computer's power commands, and the firmware applying settings */
        for (int k = 0; k < n_sets; k++) {
            if (!sets[k].fired && tx_t >= sets[k].t) {
                sets[k].fired = 1;
                tw_set_power(&N_[sets[k].at].st, sets[k].q);
                sets[k].seq = N_[sets[k].at].st.set_gen[TW_K_POWER];   /* 3.0: the power key's generation */
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
                if ((int8_t)(uint8_t)(r->st.set_gen[TW_K_POWER] - sets[k].seq) < 0 || r->st.set_power == 0) { all = 0; break; }
            }
            if (all) sets[k].done_t = tx_t;
        }
        int len = 0;
        if (ev == 3) {                                       /* 4.0: the exit's next update frame */
            if (dead) { s->up_at = tx_t + 20000; t = tx_t; continue; }
            uint32_t nx;
            len = tw_up_frame(&s->st, lnow, frame, TW_MAX_PAYLOAD, &nx);
            s->up_at = s->st.up_tx_on ? true_after(s, tx_t, lnow, nx) : -1;
            if (len > 0) {
                double air = tw_air_us(len);
                air_used_us += air;
                frames++;
                up_frames_air++;
                if (last_len && tx_t < busy_until) {
                    collisions++;
                    last_collided = 1;
                } else {
                    memcpy(last_frame, frame, (size_t)len);
                    last_len = len;
                    last_collided = 0;
                    last_extra = 0;
                    last_start = tx_t;
                    busy_until = tx_t + air;
                    busy_src = who;
                }
            }
            t = tx_t;
            continue;
        }
        dead |= hushed;
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
            memcpy(frame, s->pend, (size_t)len);
        } else if (is_extra) {
            s->extra_at = -1;
            if (!dead) {
                len = tw_build_extra(&s->st, local_us(s, when), frame, TW_MAX_PAYLOAD);
            }
            if (late) {
                if (len > 0) {
                    memcpy(s->pend, frame, (size_t)len);
                    s->pend_len = len;
                    s->pend_extra = 1;
                    s->air_t = s->hand2_t + pre_draw(over2);
                }
                t = tx_t;
                continue;
            }
        } else if (!dead) {
            tw_up_tick(&s->st, lnow);                    /* 4.0 (the firmware: once a second) */
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
                if (fix_x == 1 && s->st.extra_planned) {
                    /* 2.3 "cb" (the firmware's way): built now, cut for a send-done late in the
                       board's usual spread and its own access budget */
                    s->st.access_us = (uint16_t)xbudget;
                    double pred = s->hand_t + tw_air_us(len) + s->ovh + 2 * s->ovh_dev;
                    uint32_t pl = local_us(s, pred);
                    s->q_len = tw_build_extra(&s->st, pl, s->qbuf, TW_MAX_PAYLOAD);
                    if (s->q_len > 0)
                        s->x_latest = pl + (uint32_t)((int32_t)(TW_SLOT_US + TW_SLOT_SLACK_US) -
                                                      (int32_t)(s->st.access_us + tw_air_us(s->q_len)) -
                                                      tw_slot_pos(&s->st, pl, (uint8_t)who));
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
                last_start = tx_t;
                busy_until = tx_t + air;
                busy_src = who;
            }
            if (!late && !is_extra && s->st.extra_planned) {
                s->extra_at = tx_t + air + 50;               /* ESP-NOW's send-done, then the build */
            }
            if (late && !is_extra) {
                if (fix_hwrx)                                /* tw_node 2.3: its delay, for its headers */
                    tw_note_sent_normal(&s->st, local_us(s, s->stamp_t), local_us(s, sd), len);
                if (fix_x == 2) {
                    if (s->q_len > 0) {                      /* queued behind it: goes right after */
                        memcpy(s->pend, s->qbuf, (size_t)s->q_len);
                        s->pend_len = s->q_len;
                        s->pend_extra = 1;
                        s->air_t = tx_t + air + 50 + csma() + (urand() < stall_p ? ur(stall_lo, stall_hi) : 0);
                        s->q_len = 0;
                    }
                } else if (fix_x == 1) {
                    double o = sd - s->hand_t - air;
                    if (o > 0 && o < 3000) {
                        double d = o - s->ovh;
                        s->ovh += d / 16;
                        s->ovh_dev += (fabs(d) - s->ovh_dev) / 16;
                    }
                    if (s->q_len > 0) {
                        if ((int32_t)(local_us(s, sd) - s->x_latest) <= 0) {   /* the callback sends it */
                            memcpy(s->pend, s->qbuf, (size_t)s->q_len);
                            s->pend_len = s->q_len;
                            s->pend_extra = 1;
                            s->hand2_t = sd + ur(0, 20);
                            s->air_t = s->hand2_t + pre_draw(over2);
                            if (tx_t > 10e6 && s->st.synced) lst(&s->f_wake, s->hand2_t - sd);
                        } else {
                            tw_unsend_extra(&s->st);         /* too late: its records go back */
                        }
                        s->q_len = 0;
                    }
                } else if (s->st.extra_planned) {
                    {                                        /* 2.2: our task wakes, builds, hands over */
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
        if (dead_real && (s->st.n_rx_frames || s->st.n_tx)) {
            /* a dead board forgets everything: it reboots when it comes back */
            int was_exit = s->exit;
            uint8_t grp = s->st.group;                   /* 3.3: its group is in its flash: kept */
            uint32_t done_id = s->st.up_state == TW_UP_ST_READY ? s->st.up_id : s->st.up_done_id;   /* 4.0 */
            tw_init(&s->st, (uint8_t)who, (uint8_t)was_exit, (unsigned)(t + who));
            s->st.group = grp;
            s->st.up_done_id = done_id;                  /* 4.0: the firmware's word, from its flash */
            tw_set_info(&s->st, "sim|board");
            s->st.set_channel = 11;                      /* a board with an empty flash: channel 11 */
            tw_net_init(&s->st, NULL);
            tw_chan_init(&s->st, lnow);
            s->radio = s->st.chan_home;
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
            rdead |= r->away_from >= 0 && t >= r->away_from && t < r->away_to;
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
                            "\"x_end_own\": [%.0f, %.0f], \"tx_len\": %.0f, \"x_len\": %.0f, \"x_dropped\": %lu}",
                    id > 1 ? ", " : "", id, LS(q->s_start), LS(q->s_xend), q->x_over, q->x_into, q->s_xend.n,
                    q->s_start.n, q->s_defer.n, LS(q->s_defer), LS(q->f_at), LS(q->f_radio), LS(q->f_wake),
                    LS(q->f_xradio), LS(q->f_xend), q->f_len.n ? q->f_len.sum / q->f_len.n : 0.0,
                    q->f_xlen.n ? q->f_xlen.sum / q->f_xlen.n : 0.0, (unsigned long)q->st.n_extra_late);
#undef LS
        }
    }
    fprintf(stderr, "}, \"net\": {");              /* each board's settings, and its network keys */
    for (int id = 1; id <= n_nodes; id++) {
        simnode_t *q = &N_[id];
        fprintf(stderr, "%s\"%d\": {\"hdr\": [%u, %u, %u], \"diff_s\": 0, \"chan\": [%u, %u, %u, %u, %u], "
                "\"group\": [%u, %lu, %u], \"leader\": %u, \"keys\": [",
                id > 1 ? ", " : "", id, q->st.set_seq, q->st.set_power, q->st.set_flags, q->st.chan_home,
                q->st.set_channel, q->st.chan_switches, q->st.chan_scans, q->st.scan_found, q->st.group,
                (unsigned long)q->st.n_other_group, q->st.other_group_last, q->st.leader);
        for (int i = 0; i < TW_K_NET_N; i++) {
            int k = TW_K_NET_FIRST + i;
            fprintf(stderr, "%s[%u, %u, %.2f]", i ? ", " : "", q->st.set_val[k][0], q->st.set_gen[k], took_t[id][k]);
        }
        fprintf(stderr, "]}");
    }
    if (n_give) {
        fprintf(stderr, "}, \"given\": {");
        for (int id = 1; id <= n_nodes; id++) {
            fprintf(stderr, "%s\"%d\": [", id > 1 ? ", " : "", id);
            for (int k = 1; k <= TW_K_LED_TRY; k++)
                fprintf(stderr, "%s[%u, %u, %.2f]", k > 1 ? ", " : "", tw_setting_u16(&N_[id].st, (uint8_t)k),
                        N_[id].st.set_gen[k], took_t[id][k]);
            fprintf(stderr, "]");
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
    fprintf(stderr, "}, \"update\": {\"size\": %u, \"target\": %d, \"start\": %.1f, \"end\": %.1f, "
                    "\"frames\": %ld, \"bursts\": %lu, \"boards\": {", up_size, up_target, up_t, up_end_t,
            up_frames_air, (unsigned long)N_[first_exit()].st.up_bursts);
    for (int id = 1; id <= n_nodes; id++)
        fprintf(stderr, "%s\"%d\": [%d, %.1f]", id > 1 ? ", " : "", id, up_state_end[id], up_done_t[id]);
    fprintf(stderr, "}}}\n");
    if (out != stdout) fclose(out);
    return 0;
}
