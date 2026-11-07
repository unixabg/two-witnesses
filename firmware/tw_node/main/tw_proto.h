/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0 */
/*
 * tw_proto.h - the two-witnesses many-board protocol, portable core.
 *
 * Every board runs the same code. Each board broadcasts one ESP-NOW frame per
 * superframe, in its own time slot; that frame is the sounding packet the
 * other boards measure (CSI), and its payload carries the board's own recent
 * measurements of the others. Any board with a computer on its USB port
 * prints everything it has (its own measurements + those relayed to it): it
 * is an "exit". The lowest-numbered board heard in the last 2 s keeps time
 * (the "leader"); its frames set the superframe for everyone. Both roles move
 * by themselves when a board goes quiet.
 *
 * Network settings (transmit power, for now) are set at one board, the one with
 * the computer, and spread by themselves: every frame carries the sender's
 * current setting and its setting number; a board that hears a newer setting
 * adopts it, keeps it in flash and carries it on. A board that was off when it
 * changed picks it up from the first frame it hears. (The setting rides in two
 * header bytes that older firmware sent as zero and ignores, so old and new
 * boards still work together; an old board just keeps full power.)
 *
 * Relaying (version 2): every frame also says how well its sender hears each
 * board (percent of that board's frames, over about the last second). When the
 * exit hears a board badly (under TW_RELAY_STRANDED %), the boards that hear
 * that board well and are heard well by the exit (TW_RELAY_GOOD %) carry its
 * records too, with hop = 1: up to TW_RELAY_MAX of them, lowest numbers first, each
 * taking the records whose number falls to it (record number mod how many).
 * Every board works out the same list from the same hearing reports, so
 * nothing is needed but the frames already sent. Relayed records only fill
 * room left in the relay's own frames (after its own new records, before
 * its second copies), never another slot. Records are
 * de-duplicated by (measuring board, boot, record number) at the exit and on
 * the computer. A network setting turns relaying off (tw_set_relay).
 *
 * A second frame in the slot (tw_node 2.1, docs/SECOND-FRAME.md): when records
 * are still waiting after a board's frame has gone out (its own new ones,
 * relayed ones, or second copies of records sent in an earlier slot), it sends
 * an "extra frame" right after, in the same slot, cut to end 200 us into the
 * next board's 400 us guard (tw_build_extra). It has the same frame number and
 * the flag TW_F_EXTRA; the frame before it says TW_F_EXTRA_NEXT. Receivers use
 * its records as usual, but it counts for nothing else: not for hearing, the
 * clock or settings, and by default no measurement is made from it, so every
 * link is still measured once per period. Exits send none (they print their
 * records themselves). Network settings in set_flags turn it off, force it
 * (with full-length frames: test airtime), let receivers measure it, or cap
 * the records per frame (to crowd 3 boards like 6).
 *
 * No ESP-IDF here: the same file is compiled into the firmware and into the
 * desk simulator (sim.c). All times are 32-bit microseconds, modular.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define TW_VERSION          2           /* 2: hearing reports and relaying (1.x boards cannot mix) */
#define TW_MAGIC0           'T'
#define TW_MAGIC1           'W'
#define TW_MAX_NODES        16          /* board numbers 1..16 */
#define TW_MAX_AMPS         192         /* amplitudes in a full record (HT40: 64 L-LTF + 128 HT-LTF bins) */
#define TW_COMPACT_AMPS     52          /* L-LTF only (as esp-radar uses) */
#define TW_HDR_LEN          40          /* 24 bytes, then how well the sender hears boards 1..16 */
#define TW_REC_HDR_LEN      28
#define TW_MAX_PAYLOAD      1400        /* ESP-NOW v2 allows 1470 */
#define TW_QUEUE_LEN        96          /* own records waiting to be carried */
#define TW_SEEN_LEN         256         /* per measuring board, for de-duplication */
#define TW_ACTIVE_US        2000000u    /* a board is "active" if heard in the last 2 s */
#define TW_RECORD_TTL_US    1500000u    /* records older than this are not sent any more */
#define TW_COPIES           2           /* each record goes out in up to 2 frames (redundancy), while */
                                        /* there are no more than TW_FULL_MAX_ACTIVE boards */
#define TW_FULL_MAX_ACTIVE  6           /* above this many boards, records switch to compact */
#define TW_GUARD_US         400u        /* gap at the start of each slot */
#define TW_SLOT_US          3000u       /* each board owns a 3 ms slot: board n starts at (n-1) x 3 ms */
#define TW_MIN_PERIOD_MS    20          /* never faster than 50 frames/s per board */
#define TW_RELAY_STRANDED   70          /* the exit hears a board under this %: it is stranded */
#define TW_RELAY_GOOD       80          /* a relay must hear it, and be heard by the exit, at least this % */
#define TW_RELAY_MAX        8           /* at most this many relays share one stranded board */
#define TW_SET_RELAY_OFF    0x01        /* setting flag: relaying off (default on) */
#define TW_SET_SECOND_OFF   0x02        /* setting flag: no extra frames (default: when records wait) */
#define TW_SET_SECOND_FORCE 0x04        /* test: every board sends an extra frame in every slot, and both
                                           its frames are padded to the full length */
#define TW_SET_MEASURE_EXTRA 0x08       /* test: receivers measure extra frames too (TW_KIND_EXTRA) */
#define TW_SET_CAP_SHIFT    4           /* test: bits 4-7 = at most this many records per frame (0: no cap) */
#define TW_SLOT_SLACK_US    200u        /* an extra frame may run this far into the next slot's guard */
#define TW_ACCESS_US        150u        /* allowed for getting on the air (channel access, driver) */
#define TW_PREAMBLE_US      40u         /* HT preamble and PHY header */
#define TW_MAC_BYTES        30          /* ESP-NOW's MAC header, vendor header and FCS around our payload */
#define TW_POWER_MIN_Q      8           /* transmit power, in quarter dBm: 2 dBm .. */
#define TW_POWER_MAX_Q      80          /* .. 20 dBm (the chip's limit; the default) */

/* record kinds */
#define TW_KIND_FULL        0           /* amplitude of every bin of the CSI buffer */
#define TW_KIND_LLTF        1           /* amplitude of the 52 L-LTF subcarriers */
#define TW_KIND_FWI         0x80        /* flag: first_word_invalid (first 4 bytes bad) */
#define TW_KIND_MASK        0x0F        /* the kind itself (FULL, LLTF, INFO); the bits above are flags */
#define TW_KIND_INFO        2           /* 2.2+: not a measurement: the board's firmware and build, as
                                           text in q[] (n bytes); printed as TWV, never as TWR */
#define TW_INFO_MAX         64
#define TW_INFO_EVERY_US    5000000u    /* how often each board sends it */
#define TW_KIND_EXTRA       0x20        /* flag: measured from an extra frame (TW_SET_MEASURE_EXTRA only) */
#define TW_KIND_RXERR       0x40        /* flag: the radio reported this frame received with an error
                                           (rx_state != 0; the numbers are not documented). Kept, only
                                           marked: the computer decides what to do with it */

/* frame header flags */
#define TW_F_SYNCED         0x01        /* sender follows a leader's clock (or is the leader) */
#define TW_F_EXIT           0x02        /* sender has a computer on its USB port */
#define TW_F_LEADER         0x04        /* sender thinks it is the leader */
#define TW_F_EXTRA          0x08        /* an extra frame: the second in the sender's slot, same number */
#define TW_F_EXTRA_NEXT     0x10        /* an extra frame should follow this one */

typedef struct {
    uint8_t  version, src, leader, flags, period_ms, n_records;
    uint32_t seq;           /* sender's frame counter */
    uint32_t net_us;        /* sender's estimate of the leader's clock when the frame was built */
    uint16_t active;        /* bit (id-1): boards the sender counts as active */
    uint16_t uptime_s;
    uint8_t  set_seq;       /* network setting number (0 and set_power 0: none yet) */
    uint8_t  set_power;     /* transmit power in quarter dBm, 0 = never set (full power) */
    uint8_t  set_flags;     /* TW_SET_RELAY_OFF */
    uint8_t  xend_q;        /* 2.1+: when the sender's last extra frame ended, by its own clock, in
                               20 us steps after its slot start (0: none yet; byte 23, zero before) */
    uint8_t  hear[TW_MAX_NODES];   /* how well the sender hears board i+1, % (0 = not heard) */
    uint16_t len;           /* the whole frame's length, bytes (from the receiver, not on the air) */
} tw_hdr_t;

typedef struct {
    uint8_t  rx, tx, hop, kind;     /* measuring board, measured board, hops, kind|flags */
    uint8_t  boot;                  /* measuring board's boot number (random each start) */
    uint32_t recseq;                /* measuring board's record counter */
    uint32_t txseq;                 /* frame counter of the measured frame */
    uint32_t rx_us;                 /* measuring board's radio timestamp (rx_ctrl.timestamp) */
    uint32_t net_us;                /* reception time on the leader's clock */
    int8_t   rssi, noise;
    uint8_t  agc;
    int8_t   fft;
    uint8_t  n, step;               /* amplitude count; amplitude = q * step / 4 */
    uint8_t  phy;                   /* bits 0-1 cwb, 2 stbc, 3-4 sig_mode, 5-6 secondary channel */
    uint8_t  q[TW_MAX_AMPS];
} tw_rec_t;

typedef struct {
    tw_rec_t rec;
    uint8_t  used, sent;
    uint8_t  relay;                 /* 1: another board's record, carried for it (sent once) */
    uint32_t made_us;               /* local time it was made (or taken on, for a relay) */
    uint32_t slot;                  /* slot number (tw_node_t.slot_no) it last went out in */
} tw_qent_t;

typedef struct {
    uint32_t hi;                    /* highest record number seen */
    uint32_t bits[TW_SEEN_LEN / 32];/* which of the TW_SEEN_LEN below it were seen */
    uint8_t  boot, used;
} tw_seen_t;

typedef struct {
    uint8_t  id, is_exit, boot;
    uint32_t seq, recseq;
    uint32_t heard_us[TW_MAX_NODES + 1];    /* local time last heard, per board */
    uint8_t  heard[TW_MAX_NODES + 1];
    /* time keeping */
    uint8_t  leader;                        /* current leader (lowest active id) */
    uint8_t  synced;
    uint32_t offset;                        /* net = local + offset (mod 2^32) */
    uint32_t cand[16];                      /* recent offset candidates from the leader's frames */
    uint8_t  n_cand, cand_pos;
    uint16_t lead_active;                   /* the leader's active list (sets the slots) */
    uint8_t  lead_period_ms;
    uint32_t lead_heard_us;
    uint8_t  leader_changes;
    /* network settings (see the top of this file) */
    uint8_t  set_seq, set_power;            /* the setting this board uses and carries on */
    uint8_t  set_flags;                     /* TW_SET_RELAY_OFF */
    uint8_t  set_pending;                   /* 1: a new setting to apply (the firmware applies it) */
    uint8_t  set_changes;                   /* how many times it changed since start */
    uint8_t  tx_power_q;                    /* the power the radio reports it uses (filled in by the firmware) */
    /* hearing: how well this board hears each board (smoothed share of its frames) */
    float    hear_r[TW_MAX_NODES + 1];
    uint32_t hear_seq[TW_MAX_NODES + 1];
    /* what each board last said it hears, and its flags (for the exit and the relays) */
    uint8_t  peer_hear[TW_MAX_NODES + 1][TW_MAX_NODES];
    uint32_t peer_us[TW_MAX_NODES + 1];
    uint8_t  peer_flags[TW_MAX_NODES + 1];
    uint16_t relay_for;                     /* boards this board relayed for, in the last frame parsed of each */
    /* outgoing records */
    tw_qent_t q[TW_QUEUE_LEN];
    uint16_t q_next;
    /* de-duplication of everything printed or relayed, per measuring board */
    tw_seen_t seen[TW_MAX_NODES + 1];
    /* counters */
    uint32_t n_tx, n_rx_frames, n_bad_frames, n_meas, n_dup, n_dropped, n_relayed_out, n_relay_full;
    uint32_t rng;
    /* extra frames (tw_node 2.1) */
    uint32_t slot_no;                       /* one number per normal frame this board built */
    uint8_t  extra_planned;                 /* the frame just built announced an extra frame */
    uint32_t n_extra_sent, n_extra_skipped; /* extra frames built; announced but nothing fitted in time */
    /* 2.2: what this board says about itself (tw_set_info), and when it last did */
    char     info[TW_INFO_MAX + 1];
    uint32_t info_us;
    uint8_t  info_sent;
    uint8_t  xend_q;                        /* when our last extra frame ended (tw_note_sent), 20 us steps */
    uint32_t xend_us;                       /* ... and when that was (2.2: said only for 1 s after) */
    /* about each board heard, since tw_clear_counts (the firmware: once a second, after TWN) */
    uint16_t c_frames[TW_MAX_NODES + 1];    /* its normal frames heard */
    uint16_t c_xnext[TW_MAX_NODES + 1];     /* ... of them announcing an extra frame */
    uint16_t c_xheard[TW_MAX_NODES + 1];    /* its extra frames heard right after their normal frame */
    uint16_t c_xcsi[TW_MAX_NODES + 1];      /* CSI the radio gave on them (after their normal frame) */
    int16_t  c_xgap[TW_MAX_NODES + 1];      /* longest time from its normal frame to its extra frame, here */
    uint8_t  c_xend_q[TW_MAX_NODES + 1];    /* what it last said about its own extra frame's end */
    uint16_t c_rxerr[TW_MAX_NODES + 1];     /* this board's records of it flagged TW_KIND_RXERR */
} tw_node_t;

void     tw_init(tw_node_t *n, uint8_t id, uint8_t is_exit, uint32_t seed);
/* call when a valid frame arrives (rx_local_us: our local clock at reception) */
void     tw_note_frame(tw_node_t *n, const tw_hdr_t *h, uint32_t rx_local_us);
/* refresh the active list, leader and period; call before building or scheduling */
void     tw_update_roles(tw_node_t *n, uint32_t now_local_us);
uint16_t tw_active(const tw_node_t *n, uint32_t now_local_us);
uint8_t  tw_period_ms(const tw_node_t *n, uint32_t now_local_us);
/* local time at which this board should send its next frame */
uint32_t tw_next_tx_local(tw_node_t *n, uint32_t now_local_us);
/* build this board's next frame; returns its length. Sets n->extra_planned if an extra frame
   should follow (call tw_build_extra once this one has gone out) */
int      tw_build_frame(tw_node_t *n, uint32_t now_local_us, uint8_t *buf, int max);
/* the extra frame, built when the normal one has gone out: its length, cut to what still fits
   in the slot, or 0 if none is to be sent */
int      tw_build_extra(tw_node_t *n, uint32_t now_local_us, uint8_t *buf, int max);
/* the firmware's word that a frame this board sent has gone out (ESP-NOW's send-done): for an
   extra frame, notes when it ended, by our clock (carried in our next headers) */
void     tw_note_sent(tw_node_t *n, uint32_t now_local_us, int extra);
/* microseconds since board id's slot started (on the leader's clock), now; negative = before */
int32_t  tw_slot_pos(const tw_node_t *n, uint32_t now_local_us, uint8_t id);
/* how long a frame with a payload of len bytes is on the air, us (HT40 MCS0, 13.5 Mbit/s) */
uint32_t tw_air_us(int len);
/* clear the per-board counts (c_*) */
void     tw_clear_counts(tw_node_t *n);
/* parse a frame: fills *h; calls cb for each record; returns record count or -1 */
typedef void (*tw_rec_cb)(void *ctx, const tw_rec_t *r);
int      tw_parse_frame(const uint8_t *buf, int len, tw_hdr_t *h, tw_rec_cb cb, void *ctx);
/* peek at the header only (for the CSI callback: who sent it, which frame) */
int      tw_peek_header(const uint8_t *buf, int len, tw_hdr_t *h);
/* what this board says about itself in its TW_KIND_INFO records: "firmware|build" (commas and
   line ends become spaces; at most TW_INFO_MAX bytes) */
void     tw_set_info(tw_node_t *n, const char *text);
/* records that are not measurements, queued when due (2.2: the info record, every
   TW_INFO_EVERY_US): call about once a second; returns one record queued now (for an exit to
   print), or NULL. Call again until NULL. They take the board's ordinary record numbers, so the
   computer counts them for delivery like any record */
const tw_rec_t *tw_aux_record(tw_node_t *n, uint32_t now_local_us);
/* make a record from one CSI measurement and queue it (returns it, for printing). kflags: the
   record flags the radio gave (TW_KIND_FWI, TW_KIND_RXERR), and TW_KIND_EXTRA if the frame was an
   extra frame: then it is only counted, and NULL returned, unless TW_SET_MEASURE_EXTRA is set */
const tw_rec_t *tw_add_measurement(tw_node_t *n, uint32_t now_local_us, uint8_t tx, uint32_t txseq,
                                   uint32_t rx_us, int8_t rssi, int8_t noise, uint8_t agc, int8_t fft,
                                   uint8_t phy, uint8_t kflags, const int8_t *csi, int csi_len, float gain,
                                   int compact);
/* 1 if (rx, boot, recseq) was already seen (and marks it seen otherwise) */
int      tw_seen(tw_node_t *n, uint8_t rx, uint8_t boot, uint32_t recseq);
/* text lines for the computer */
int      tw_format_record(const tw_rec_t *r, char *out, int max);
int      tw_format_node(const tw_node_t *n, const tw_hdr_t *h, int8_t rssi, uint8_t heard_pct, char *out,
                        int max);
int      tw_format_status(const tw_node_t *n, uint32_t now_local_us, char *out, int max);
/* the L-LTF byte ranges for an ESP32/S3 CSI buffer (esp-radar's table); 0 if unknown */
int      tw_lltf_ranges(uint8_t secondary, uint8_t sig_mode, uint8_t cwb, uint8_t stbc, int len,
                        int ranges[4]);
uint32_t tw_rand(tw_node_t *n);
/* a new setting given at this board (the computer asked for it): power in quarter dBm,
   TW_POWER_MIN_Q..TW_POWER_MAX_Q. Returns 0, or -1 if out of range */
int      tw_set_power(tw_node_t *n, uint8_t power_q);
/* relaying on (1) or off (0) for every board, the same way (a new setting number) */
void     tw_set_relay(tw_node_t *n, int on);
/* any of the setting flags: the bits in mask become those of value (a new setting number) */
void     tw_set_flags(tw_node_t *n, uint8_t mask, uint8_t value);
/* adopt a setting from another board's frame if it is newer (called by tw_note_frame) */
void     tw_note_settings(tw_node_t *n, const tw_hdr_t *h);
/* how well this board hears board id now, % (100 for itself, 0 if not heard in 2 s) */
uint8_t  tw_hear_pct(const tw_node_t *n, uint32_t now_local_us, uint8_t id);
/* the exit this board's relays serve: the lowest-numbered board with a computer, heard in the
   last 2 s (0 if none, or if this board is an exit itself) */
uint8_t  tw_primary_exit(const tw_node_t *n, uint32_t now_local_us);
/* should this board carry board src's records, and which share: returns 1 and sets *idx, *of */
int      tw_relay_share(const tw_node_t *n, uint32_t now_local_us, uint8_t src, int *idx, int *of);
/* call for each record parsed from another board's frame (not on an exit): queues a relayed
   copy if this board is one of src's relays and the record falls to it. Returns 1 if queued */
int      tw_relay_record(tw_node_t *n, uint32_t now_local_us, uint8_t src, const tw_rec_t *r);
