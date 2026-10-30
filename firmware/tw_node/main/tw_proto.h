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
 * Records carry a hop count (0 = carried by the board that measured it) and
 * are de-duplicated by (measuring board, record sequence number), so that
 * multi-hop forwarding can be switched on later without changing the format.
 *
 * No ESP-IDF here: the same file is compiled into the firmware and into the
 * desk simulator (sim.c). All times are 32-bit microseconds, modular.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define TW_VERSION          1
#define TW_MAGIC0           'T'
#define TW_MAGIC1           'W'
#define TW_MAX_NODES        16          /* board numbers 1..16 */
#define TW_MAX_AMPS         192         /* amplitudes in a full record (HT40: 64 L-LTF + 128 HT-LTF bins) */
#define TW_COMPACT_AMPS     52          /* L-LTF only (as esp-radar uses) */
#define TW_HDR_LEN          22
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
#define TW_FORWARD          0           /* multi-hop forwarding: off (format ready for it) */

/* record kinds */
#define TW_KIND_FULL        0           /* amplitude of every bin of the CSI buffer */
#define TW_KIND_LLTF        1           /* amplitude of the 52 L-LTF subcarriers */
#define TW_KIND_FWI         0x80        /* flag: first_word_invalid (first 4 bytes bad) */

/* frame header flags */
#define TW_F_SYNCED         0x01        /* sender follows a leader's clock (or is the leader) */
#define TW_F_EXIT           0x02        /* sender has a computer on its USB port */
#define TW_F_LEADER         0x04        /* sender thinks it is the leader */

typedef struct {
    uint8_t  version, src, leader, flags, period_ms, n_records;
    uint32_t seq;           /* sender's frame counter */
    uint32_t net_us;        /* sender's estimate of the leader's clock when the frame was built */
    uint16_t active;        /* bit (id-1): boards the sender counts as active */
    uint16_t uptime_s;
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
    uint32_t made_us;               /* local time it was made */
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
    /* outgoing records */
    tw_qent_t q[TW_QUEUE_LEN];
    uint16_t q_next;
    /* de-duplication of everything printed or relayed, per measuring board */
    tw_seen_t seen[TW_MAX_NODES + 1];
    /* counters */
    uint32_t n_tx, n_rx_frames, n_bad_frames, n_meas, n_dup, n_dropped, n_relayed_out;
    uint32_t rng;
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
/* build this board's next frame; returns its length */
int      tw_build_frame(tw_node_t *n, uint32_t now_local_us, uint8_t *buf, int max);
/* parse a frame: fills *h; calls cb for each record; returns record count or -1 */
typedef void (*tw_rec_cb)(void *ctx, const tw_rec_t *r);
int      tw_parse_frame(const uint8_t *buf, int len, tw_hdr_t *h, tw_rec_cb cb, void *ctx);
/* peek at the header only (for the CSI callback: who sent it, which frame) */
int      tw_peek_header(const uint8_t *buf, int len, tw_hdr_t *h);
/* make a record from one CSI measurement and queue it (returns it, for printing) */
const tw_rec_t *tw_add_measurement(tw_node_t *n, uint32_t now_local_us, uint8_t tx, uint32_t txseq,
                                   uint32_t rx_us, int8_t rssi, int8_t noise, uint8_t agc, int8_t fft,
                                   uint8_t phy, uint8_t fwi, const int8_t *csi, int csi_len, float gain,
                                   int compact);
/* 1 if (rx, boot, recseq) was already seen (and marks it seen otherwise) */
int      tw_seen(tw_node_t *n, uint8_t rx, uint8_t boot, uint32_t recseq);
/* text lines for the computer */
int      tw_format_record(const tw_rec_t *r, char *out, int max);
int      tw_format_node(const tw_hdr_t *h, int8_t rssi, char *out, int max);
int      tw_format_status(const tw_node_t *n, uint32_t now_local_us, char *out, int max);
/* the L-LTF byte ranges for an ESP32/S3 CSI buffer (esp-radar's table); 0 if unknown */
int      tw_lltf_ranges(uint8_t secondary, uint8_t sig_mode, uint8_t cwb, uint8_t stbc, int len,
                        int ranges[4]);
uint32_t tw_rand(tw_node_t *n);
