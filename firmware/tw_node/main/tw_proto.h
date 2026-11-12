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
 * Network settings (transmit power, relaying, the second frame) are set at one
 * board, the one with the computer, and spread by themselves (protocol 3,
 * docs/PROTOCOL-3.md): each is a network key on the settings transport
 * (tw_setting_*), which every board says every 5 s with its generation; a
 * board that hears a newer generation takes it, keeps it in flash and says it
 * from then on. A board that was off when it changed catches up within about
 * 5 s of coming back. (Before 3.0 the frame header carried them; 2.x and 3.x
 * boards ignore each other's frames.)
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

#define TW_VERSION          3           /* 3: the lean header (docs/PROTOCOL-3.md); 2: hearing reports and
                                           relaying. Boards of different versions ignore each other */
#define TW_MAGIC0           'T'
#define TW_MAGIC1           'W'
#define TW_MAX_NODES        16          /* board numbers 1..16 */
#define TW_MAX_AMPS         192         /* amplitudes in a full record (HT40: 64 L-LTF + 128 HT-LTF bins) */
#define TW_COMPACT_AMPS     52          /* L-LTF only (as esp-radar uses) */
#define TW_HDR_LEN          40          /* 24 bytes (19: the group, 3.3; 20-23 reserved: sent 0, ignored),
                                           then how well the sender hears boards 1..16 */
/* 3.3: groups. Arrays of boards near each other keep to themselves: each frame says its sender's
   group (header byte 19); a board takes only its own group's frames, and counts the others'. Each
   board's group is in its flash, set over its cable (like its number); a board with none is in
   TW_GROUP_DEFAULT, the group of every board before 3.3 (which send 0 there). Two groups on one
   channel share its air (their slots collide): give each group its own channel (tw_setid.py
   survey, channel). The group is also in the board's radio address (byte 4), so the radio's
   measurements are of its own group's boards only */
#define TW_GROUP_DEFAULT    0
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
/* the setting flags: the network keys TW_K_RELAY..TW_K_CAP, as one byte (n->set_flags) */
#define TW_SET_RELAY_OFF    0x01        /* setting flag: relaying off (default on) */
#define TW_SET_SECOND_OFF   0x02        /* setting flag: no extra frames (default: when records wait) */
#define TW_SET_SECOND_FORCE 0x04        /* test: every board sends an extra frame in every slot, and both
                                           its frames are padded to the full length */
#define TW_SET_MEASURE_EXTRA 0x08       /* test: receivers measure extra frames too (TW_KIND_EXTRA) */
#define TW_SET_CAP_SHIFT    4           /* test: bits 4-7 = at most this many records per frame (0: no cap) */
#define TW_SLOT_SLACK_US    200u        /* an extra frame may run this far into the next slot's guard */
#define TW_ACCESS_US        150u        /* allowed for getting on the air (channel access, driver) */
#define TW_MAX_RECS_X       16          /* records an extra frame can carry (compact ones are small) */
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
#define TW_KIND_SCORE       3           /* 2.2+: not a measurement: once a second, this board's movement
                                           score for each board it heard (q[i]: board i+1, x10; 0 =
                                           none); 2.7: q[16] where its last second frame ended (20 us
                                           steps); txseq its uptime, s; printed as TWM. 3.0: the only
                                           place of both (the header carried them before). 3.4:
                                           q[17..18] its detections that second (bit i: board i+1).
                                           3.5: q[19..20] its steadier-than-quiet marks (the same) */
                                        /* 4: 2.5's settings record (TW_KIND_SET), gone in 3.0 */
#define TW_KIND_SURVEY      6           /* 3.1: not a measurement: this board's last channel survey;
                                           txseq its generation (TW_K_SURVEY), q: the first channel, the
                                           count, then TW_SURVEY_ENTRY bytes a channel: busy (0.5% steps,
                                           0..200), foreign frames (2 bytes), their loudest and mean
                                           RSSI, the noise floor (dBm; 0 with no frames). Printed as TWU */
#define TW_SURVEY_FIRST     1           /* channels surveyed: 1..11 (2.4 GHz, US). A future band adds a
                                           range here; a channel number names its band by itself */
#define TW_SURVEY_N         11
#define TW_SURVEY_ENTRY     6
#define TW_SURVEY_DELAY_US  5000000u    /* from taking the action to leaving: time for it to reach all */
#define TW_SURVEY_DWELL_MS  1000        /* listening on each channel */
#define TW_SURVEY_SEND_US   60000000u   /* the result is sent every 5 s for this long after */
#define TW_KIND_UPDATE      7           /* 4.0: not a measurement: this board's part in an update over
                                           the air, once a second while it has one: q[0..3] the
                                           update's id, q[4] its state (TW_UP_ST_*), q[5..6] chunks it
                                           has, q[7..8] of how many, q[9] 1 if it has the manifest, then
                                           a bit per chunk it has (chunk i: byte 10 + i/8, bit i%8).
                                           Printed as TWP */
#define TW_KIND_CMD         5           /* 2.5.1+: settings and commands for boards (docs/PROTOCOL-3.md):
                                           entries of TW_CMD_HDR bytes and a value: target (0 = all),
                                           key, flags (TW_CF_REPORT), generation, seconds left to pass
                                           it on, value length L (0..TW_CMD_VAL), value (L bytes, low
                                           first); printed as TWX */
#define TW_CMD_HDR          6
#define TW_CMD_VAL          8
#define TW_CF_REPORT        0x01        /* a board's own state (target = itself), not an instruction */
#define TW_CF_STANDING      0x02        /* 3.0: a network key as its sender has it (standing, every 5 s):
                                           an instruction to all like any, and its sender's own value */
#define TW_SET_ALL          0           /* an entry's target: every board */
/* keys (docs/PROTOCOL-3.md, "Keys"); a board skips keys it does not know, by their length */
#define TW_K_LED_PIN        1           /* board: the LED's GPIO, 1 byte (0 = no LED) */
#define TW_K_LED_BRIGHT     2           /* board: its brightness, 1 byte (1..255) */
#define TW_K_LED_TEST       3           /* action: the LED's test sequence, no value */
#define TW_K_LED_TRY        4           /* action: show R, G, B (3 bytes) on the LED for 10 s */
#define TW_K_SURVEY         5           /* action (3.1): the channel survey, no value; for all boards at
                                           once (TW_SURVEY_*) */
#define TW_K_DETECT         6           /* board (3.4): its detection threshold, x10 of quiet, 1 byte
                                           (TW_DETECT_MIN..MAX; never given: TW_DETECT_DEFAULT) */
#define TW_K_RELEARN        7           /* action (3.4): learn every link's quiet level again, no value */
#define TW_K_DETECT_LOW     8           /* board (3.5): its steadier-than-quiet threshold, x10 of quiet,
                                           1 byte (0: off; 1..TW_LOW_MAX; never given: TW_LOW_DEFAULT) */
#define TW_K_POWER          16          /* network (2.6): transmit power, 1 byte, quarter dBm (0: never set) */
#define TW_K_RELAY          17          /* network: relaying, 1 byte (1 on, 0 off) */
#define TW_K_SECOND         18          /* network: the second frame, 1 byte (0 on, 1 off, 2 force) */
#define TW_K_MEASURE        19          /* network: measure-second, 1 byte (0 off, 1 on) */
#define TW_K_CAP            20          /* network: at most N records a frame, 1 byte (0: no cap) */
#define TW_K_CHANNEL        21          /* network (3.2): the radio channel, 1 byte (TW_CHAN_*) */
#define TW_K_NET_FIRST      TW_K_POWER
#define TW_K_NET_N          6           /* 3.2: 6 (the channel); 3.0-3.1 boards know the first 5 */
#define TW_K_NET_FLAGS_N    5           /* power and the setting flags: what TWN's settings need */

/* 3.2: the channel. Every board of a network shares one; the channel key spreads like any network
   key, and a board that takes a new one switches TW_CHAN_SWITCH_US later (all boards have it by
   then, so they move together). A board that hears no other for TW_CHAN_LOST_US looks for them:
   TW_CHAN_SCAN_MS on each other channel, then TW_CHAN_HOME_US back on its own, and again; where it
   hears one it stays, and takes the network's settings (a board that was off during a change, or a
   new one). Valid channels: 2.4 GHz, US, 1-11, 40 MHz wide with a partner 4 above (1-4) or below
   (5-11). A future band adds channels to tw_chan_valid (a channel number names its band) */
#define TW_CHAN_FIRST       1
#define TW_CHAN_LAST        11
#define TW_CHAN_SWITCH_US   20000000u
#define TW_CHAN_LOST_US     30000000u
#define TW_CHAN_SCAN_MS     300
#define TW_CHAN_HOME_US     10000000u
#define TW_K_N              32          /* keys 1..31 */
#define TW_KC_BOARD         1           /* key classes: a setting of one board, kept in its flash */
#define TW_KC_NETWORK       2           /*   one value for every board, said by every board (2.6+) */
#define TW_KC_ACTION        3           /*   do it once per new generation; not kept */
/* generations: 8 bits, wrapping (newer: (int8_t)(new - old) > 0); 3.0: 0 is "never given" (a
   board's flash had none): any other is newer than it, it is newer than none, and counting on
   skips it */
#define TW_SET_TABLE        8           /* entries a board passes on at once */
#define TW_SET_TTL_S        60          /* seconds an entry is passed on after it was given */
#define TW_REPORT_EVERY_US  5000000u    /* each board's report entries: its keys' values, generations */
#define TW_SCORE_BAND       4           /* subcarriers per band (as replay.py, dash.py) */
#define TW_SCORE_BANDS      (TW_MAX_AMPS / TW_SCORE_BAND)
#define TW_SCORE_LEARN      50          /* records that decide which bins are live */
#define TW_SCORE_START      60          /* seconds whose spread starts the quiet level (3.4: 60, a stepped
                                           median; 30 and their mean before, which a few loud seconds
                                           pulled up, and the board under-reported for minutes) */
/* 3.4: detections. A link's score at or above its board's threshold (TW_K_DETECT, x10; never given:
   TW_DETECT_DEFAULT) for TW_DETECT_HOLD seconds in a row; marked in the TWM record, bit tx-1 of
   q[17..18], every second it holds */
#define TW_DETECT_DEFAULT   30          /* 3.0 x quiet */
#define TW_DETECT_MIN       15
#define TW_DETECT_MAX       100
#define TW_DETECT_HOLD      2
#define TW_DETECT_GAP       2           /* 4.2: a second without a score (too few frames: someone in
                                           the way costs some) neither counts nor breaks a detection;
                                           this many in a row do break it */
/* 3.5: steadier than quiet. A link's score at or below its board's low threshold (TW_K_DETECT_LOW,
   x10; never given: TW_LOW_DEFAULT; 0: off) for TW_LOW_HOLD seconds in a row (seconds without a
   score neither count nor break it); marked in the TWM record, bit tx-1 of q[19..20], every second
   it holds. While it holds, the link's quiet level does not step down (it would follow the
   stillness and end the mark within minutes), for up to TW_LOW_KEEP seconds: then the stillness
   is the new quiet (counted), the mark ends and the quiet level follows again */
#define TW_LOW_DEFAULT      5           /* 0.5 x quiet */
#define TW_LOW_MAX          9
#define TW_LOW_HOLD         10
#define TW_LOW_KEEP         1800
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
                                        /* 0x20: 2.3-2.7's TW_F_DELAY (the delay in the sender's own
                                           place of the hearing list); 3.0: header byte 18. Free */
#define TW_POST_EST_US      100u        /* 2.3: a send-done comes about this long after the frame's end */

typedef struct {
    uint8_t  version, src, leader, flags, period_ms, n_records;
    uint32_t seq;           /* sender's frame counter */
    uint32_t net_us;        /* sender's estimate of the leader's clock when the frame was built */
    uint16_t active;        /* bit (id-1): boards the sender counts as active */
    uint8_t  hear[TW_MAX_NODES];   /* how well the sender hears board i+1, % (0 = not heard) */
    uint16_t len;           /* the whole frame's length, bytes (from the receiver, not on the air) */
    uint8_t  group;         /* byte 19 (3.3): the sender's group (0 from boards before 3.3) */
    uint16_t delay_us;      /* byte 18 (2.3's, its own byte since 3.0): the sender's delay, the least
                               over its last 16 frames of time stamp to send-done less the air, in
                               20 us steps; 0 if it does not know yet */
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

/* 4.0: updates over the air (docs/PROTOCOL-3.md, "Updates over the air"). The computer gives a
   signed program to the board on its cable (the exit), which sends it to its group in chunks, in
   short bursts: its own frames, marked 'T','U' (older firmware and other groups drop them). While
   they come, every board of the group keeps quiet (TW_UP_HUSH_US after the last one). Between
   bursts the network runs as usual, and each board taking part says which chunks it has
   (TW_KIND_UPDATE); the next bursts send what any of them still lacks. A board with every chunk
   and the manifest (the program's SHA-256 and the signature over it) leaves the rest to its
   firmware: check, write to the other program slot, restart. The update's id is the first 4
   bytes of the SHA-256. The frame: 'T','U', TW_VERSION, group, sender, target (0: every board of
   the group), id (4 bytes), size (3 bytes), chunk number (2 bytes; TW_UP_MANIFEST: the
   manifest), 0; then the chunk (TW_UP_CHUNK bytes, the last one shorter) or the manifest */
#define TW_UP_MAGIC1        'U'
#define TW_UP_HDR           16
#define TW_UP_CHUNK         1280
#define TW_UP_MAX           0x180000u   /* a program slot (partitions.csv) */
#define TW_UP_CHUNKS_MAX    ((TW_UP_MAX + TW_UP_CHUNK - 1) / TW_UP_CHUNK)
#define TW_UP_BITS          ((TW_UP_CHUNKS_MAX + 7) / 8)
#define TW_UP_MANIFEST      0xFFFFu
#define TW_UP_MAN_LEN       96          /* SHA-256 (32), then the signature: ECDSA P-256, r and s (64) */
#define TW_UP_PACE_US       2200u       /* between the exit's update frames in a burst */
#define TW_UP_BURST_US      600000u     /* a burst: at most this long (under TW_ACTIVE_US: no board is
                                           taken for gone) */
#define TW_UP_GAP_US        1600000u    /* between bursts: the network as usual, the reports come in */
#define TW_UP_HUSH_US       60000u      /* a board sends nothing this long after an update frame */
#define TW_UP_IDLE_US       120000000u  /* a board drops an update with no frame of it for this long */
#define TW_UP_GIVE_UP_US    300000000u  /* the exit stops sending after this long */
#define TW_UP_WAIT_US       15000000u   /* after the first pass: an active board not reporting by then
                                           is not taking part (older firmware, no memory) */
#define TW_UP_FAIL_SEND_US  60000000u   /* a failed board says so this long, then drops it */
#define TW_UP_STALE_US      6000000u    /* a board taking part that has not reported for this long: if
                                           still heard, it restarted (it gets every chunk again); if
                                           not, it is gone */
/* states (TWP; the exit's view in up_rep[]) */
#define TW_UP_ST_NONE       0
#define TW_UP_ST_RECEIVING  1
#define TW_UP_ST_COMPLETE   2           /* every chunk: the firmware checks it */
#define TW_UP_ST_INSTALLING 3           /* checked: being written to the other slot */
#define TW_UP_ST_READY      4           /* written: restarting into it */
#define TW_UP_ST_SENDING    5           /* the exit, sending */
#define TW_UP_ST_STAGED     6           /* the exit: given by the computer, checked, not sent yet */
#define TW_UP_ST_FAIL       10          /* 10 and up: failed, and why */
#define TW_UP_ST_NOMEM      10          /* no PSRAM, or too big */
#define TW_UP_ST_NOKEY      11          /* no update key set over its cable: takes no update */
#define TW_UP_ST_BADHASH    12          /* the chunks do not make the program the manifest names */
#define TW_UP_ST_BADSIG     13          /* not signed with the key it was given */
#define TW_UP_ST_BADIMAGE   14          /* not a tw_node program for this chip */
#define TW_UP_ST_WRITE      15          /* writing it to flash failed */
#define TW_UP_ST_SILENT     16          /* the exit's view only: active, never reported */

/* the movement score for one board heard (tw_node 2.2): per record the amplitudes in dB, in
   bands of 4 live bins, the record's own level out; per second each band's spread, against the
   band's quiet level (its running median of those spreads); averaged over the bands */
typedef struct {
    uint8_t  n;                             /* record length it learned on (0: not started) */
    uint8_t  learn;                         /* records seen while learning the live bins */
    uint8_t  nb;                            /* bands (0 until learned) */
    uint8_t  start;                         /* seconds into the quiet level's start */
    uint8_t  above;                         /* 3.4: seconds in a row at or above the threshold */
    uint8_t  below;                         /* 3.5: seconds in a row at or below the low threshold */
    uint16_t low_held;                      /* 3.5: seconds its quiet level was held (up to TW_LOW_KEEP) */
    uint8_t  none;                          /* 4.2: seconds in a row without a score */
    uint8_t  bin[TW_MAX_AMPS];              /* while learning: records with that bin above 0; then
                                               the live bins, TW_SCORE_BAND per band */
    uint16_t recs;                          /* records this second */
    float    s1[TW_SCORE_BANDS], s2[TW_SCORE_BANDS];   /* this second's sums of the band shape */
    float    ref[TW_SCORE_BANDS];           /* each band's quiet level */
} tw_score_t;

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
    uint16_t own_delay[16];                 /* 2.3: our last frames' stamp to send-done less the air, us */
    uint8_t  n_delay, delay_pos;
    uint8_t  n_cand, cand_pos;
    uint16_t lead_active;                   /* the leader's active list (sets the slots) */
    uint8_t  lead_period_ms;
    uint32_t lead_heard_us;
    uint8_t  leader_changes;
    /* network settings (see the top of this file): what this board uses, from its network keys */
    uint8_t  set_seq;                       /* 3.0: the setting number: the sum of the network keys'
                                               generations (one more with every change, the same on
                                               every board that has the same settings) */
    uint8_t  set_power;                     /* quarter dBm, 0 = never set (full power) */
    uint8_t  set_flags;                     /* TW_SET_* */
    uint8_t  set_channel;                   /* 3.2: the channel key's value (the network's channel) */
    /* 3.2: the radio's channel (tw_chan_step): home, the one it works on; a switch waiting; a scan */
    uint8_t  chan_home, chan_target, chan_wait, chan_changed;
    uint8_t  scan_on, scan_ch, scan_found;
    uint32_t chan_at, last_frame_us, scan_step_us, scan_next_us;
    uint16_t chan_switches, chan_scans;
    /* 3.4: this second's detections (bit tx-1), and how many began since start (for TWS) */
    uint16_t det_bits;
    uint32_t det_count;
    /* 3.5: this second's steadier-than-quiet marks (bit tx-1); how many began, and how many
       became the new quiet after TW_LOW_KEEP, since start */
    uint16_t low_bits;
    uint32_t low_count, low_accepted;
    /* 3.3: this board's group; frames of other groups: how many, and the last group heard */
    uint8_t  group, other_group_last;
    uint32_t n_other_group;
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
    /* 2.3: an extra frame may be built ahead (while the normal one is on the air) and dropped if it
       would end too late: the queue entries it took, to put them back (tw_unsend_extra) */
    uint16_t access_us;                     /* allowed for an extra frame to get on the air (TW_ACCESS_US) */
    uint8_t  x_npick;
    uint8_t  x_pick[TW_MAX_RECS_X];
    uint8_t  x_pick_rx[TW_MAX_RECS_X];
    uint32_t x_pick_seq[TW_MAX_RECS_X];
    uint32_t n_extra_late;                  /* extra frames built ahead, then dropped: too late */
    /* 2.2: what this board says about itself (tw_set_info), and when it last did */
    char     info[TW_INFO_MAX + 1];
    uint32_t info_us;
    uint8_t  info_sent;
    /* 2.2: the movement score, per board heard, and when the last second was closed */
    tw_score_t score[TW_MAX_NODES + 1];
    uint32_t score_sec;
    uint8_t  score_last[TW_MAX_NODES];      /* 2.4: the last second's scores (as TWM; 0 = none) */
    /* 2.5+: settings and commands by record (tw_setting_*): the entries being passed on, and this
       board's own value and generation for each key (a newer generation wins; 8 bits, wrapping) */
    struct {
        uint8_t  target, key, gen, ttl, len;   /* ttl 0: free */
        uint8_t  val[TW_CMD_VAL];
    } set_tab[TW_SET_TABLE];
    uint8_t  set_val[TW_K_N][TW_CMD_VAL];
    uint8_t  set_len[TW_K_N];
    uint8_t  set_gen[TW_K_N];
    uint32_t set_changed;                   /* keys of this board's taken since (bit = key): the
                                               caller acts on them and clears it */
    uint32_t set_sec, set_rep_us;
    uint8_t  set_rep_sent;
    uint8_t  xend_q;                        /* when our last extra frame ended (tw_note_sent), 20 us steps */
    uint32_t xend_us;                       /* ... and when that was (2.2: said only for 1 s after) */
    /* about each board heard, since tw_clear_counts (the firmware: once a second, after TWN) */
    uint16_t c_frames[TW_MAX_NODES + 1];    /* its normal frames heard */
    uint16_t c_xnext[TW_MAX_NODES + 1];     /* ... of them announcing an extra frame */
    uint16_t c_xheard[TW_MAX_NODES + 1];    /* its extra frames heard right after their normal frame */
    uint16_t c_xcsi[TW_MAX_NODES + 1];      /* CSI the radio gave on them (after their normal frame) */
    int16_t  c_xgap[TW_MAX_NODES + 1];      /* longest time from its normal frame to its extra frame, here */
    uint8_t  c_xend_q[TW_MAX_NODES + 1];    /* what it last said about its own extra frame's end (TWM) */
    uint16_t c_rxerr[TW_MAX_NODES + 1];     /* this board's records of it flagged TW_KIND_RXERR */
    /* 3.0: what each board says of itself in its records (the header did before): its uptime (TWM),
       and its network keys (its standing entries); for TWN */
    uint16_t peer_up[TW_MAX_NODES + 1];
    uint8_t  peer_net[TW_MAX_NODES + 1][TW_K_NET_N];
    uint8_t  peer_gen[TW_MAX_NODES + 1][TW_K_NET_N];
    uint8_t  peer_has[TW_MAX_NODES + 1];    /* bit i: peer_net[.][i] heard */
    /* 3.1: the last channel survey's result (a TW_KIND_SURVEY payload), and when it was made / sent */
    uint8_t  survey[2 + TW_SURVEY_N * TW_SURVEY_ENTRY];
    uint8_t  survey_len, survey_gen;
    uint32_t survey_us, survey_sent_us;
    /* 4.0: an update over the air. Its program (up_buf, TW_UP_MAX at most: the firmware's PSRAM, by
       tw_up_alloc), the chunks we have (a bit each), the manifest */
    uint8_t  up_state, up_target, up_man_ok, up_hush_on;
    uint32_t up_id, up_size, up_last_us, up_hush_until, up_rep_sec, up_state_us, up_failed_id;
    uint32_t up_done_id;                    /* the firmware's word: the update this board now runs (it
                                               says READY to it, and takes it no more) */
    uint16_t up_total, up_have;
    uint8_t *up_buf;
    uint8_t  up_bits[TW_UP_BITS];
    uint8_t  up_man[TW_UP_MAN_LEN];
    /* the exit, sending: chunks still to send (any board lacks them), the cursor, the burst, each
       board's last report (TW_UP_ST_*), counts for TWS */
    uint8_t  up_tx_on, up_burst, up_first_pass, up_self;
    uint8_t  up_need[TW_UP_BITS];
    uint16_t up_cursor;
    uint32_t up_start_us, up_burst_end, up_next_us, up_pass_us;
    uint8_t  up_rep[TW_MAX_NODES + 1];
    uint16_t up_rep_have[TW_MAX_NODES + 1];
    uint32_t up_rep_us[TW_MAX_NODES + 1];   /* when its last report came */
    uint32_t up_frames, up_bursts;
} tw_node_t;

void     tw_init(tw_node_t *n, uint8_t id, uint8_t is_exit, uint32_t seed);
/* call when a valid frame arrives (rx_local_us: our local clock at reception) */
/* start_local_us (2.3): when the frame started on the air by our clock, from the radio's own
   receive time; 0 = not known (then the receive callback's time and the frame's length are used) */
void     tw_note_frame(tw_node_t *n, const tw_hdr_t *h, uint32_t rx_local_us, uint32_t start_local_us);
/* 2.3: our normal frame, stamped for stamp_local_us and len bytes long, had its send-done at
   done_local_us: its delay goes into our next headers (byte 18) */
void     tw_note_sent_normal(tw_node_t *n, uint32_t stamp_local_us, uint32_t done_local_us, int len);
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
/* 2.3: the extra frame last built did not go out (built ahead, then too late): its records go back
   in the queue as they were, and it counts as late, not sent */
void     tw_unsend_extra(tw_node_t *n);
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

/* 2.5+: settings and commands by record (docs/PROTOCOL-3.md). An entry is (target board or
   TW_SET_ALL, key, generation, value): a board takes it when it is for it and its generation is
   newer than the one it has (8 bits, wrapping), and every board passes it on once a second for
   TW_SET_TTL_S seconds, so it reaches boards the giver does not hear. Every TW_REPORT_EVERY_US
   each board also says its own keys' values and generations (report entries), so the computer
   can choose the next generation and see it taken. */
int      tw_key_class(uint8_t key);                 /* TW_KC_*, 0 for a key this firmware does not know */
/* what this board starts with (from its flash) */
void     tw_setting_init(tw_node_t *n, uint8_t key, const uint8_t *val, int len, uint8_t gen);
/* given here (the board on the computer): taken here too if it is for this board, and passed on */
void     tw_setting_give(tw_node_t *n, uint8_t target, uint8_t key, const uint8_t *val, int len, uint8_t gen);
/* this board's own key, changed here (over its cable): the next generation, not passed on */
void     tw_setting_local(tw_node_t *n, uint8_t key, const uint8_t *val, int len);
/* this board's value of a key (its first two bytes, low first) */
uint16_t tw_setting_u16(const tw_node_t *n, uint8_t key);
/* call for every record heard from another board (3.0: also what it says of itself, for TWN): a
   TW_KIND_CMD record's entries for this board are taken, the rest passed on */
void     tw_setting_heard(tw_node_t *n, const tw_rec_t *r);
/* the network settings (3.0: the transport is their only way). set_power and set_flags are what
   the network keys say; a newer key taken sets set_pending (the firmware applies and keeps it).
   Start (at boot) with the settings and the keys' generations kept in flash: set_power and
   set_flags filled in first, then this */
void     tw_net_init(tw_node_t *n, const uint8_t gen[TW_K_NET_N]);
/* 3.2: the channel. Valid for setting (1..11, US); its 40 MHz partner: +1 above, -1 below */
int      tw_chan_valid(uint8_t ch);
int      tw_chan_partner(uint8_t ch);
/* at boot, after set_channel is loaded (and before tw_net_init): the radio starts there */
void     tw_chan_init(tw_node_t *n, uint32_t now);
/* given at this board (the computer asked): every board, its next generation. 0, or -1 if not valid */
int      tw_set_channel(tw_node_t *n, uint8_t ch);
/* call often (every frame or so): the channel the radio should be on now; *quiet set while it is
   looking for the others (do not send then). A switch to home sets chan_changed (keep it in flash) */
uint8_t  tw_chan_step(tw_node_t *n, uint32_t now, int *quiet);

/* 3.4: learn every link's quiet level again (TW_K_RELEARN, and after a channel change) */
void     tw_score_relearn(tw_node_t *n);
/* 3.4: this board's detection threshold, x10 (its TW_K_DETECT, or TW_DETECT_DEFAULT) */
uint8_t  tw_detect_level(const tw_node_t *n);
/* 3.5: this board's steadier-than-quiet threshold, x10 (its TW_K_DETECT_LOW, or TW_LOW_DEFAULT;
   0: off) */
uint8_t  tw_detect_low_level(const tw_node_t *n);

/* 3.3: is this frame (its header peeked) of this board's group? If not, it is counted (TWS) and
   the caller drops it */
int      tw_frame_ours(tw_node_t *n, const tw_hdr_t *h);

/* 4.0: updates over the air (TW_UP_*). The firmware (and the simulator) gives the memory: */
void    *tw_up_alloc(uint32_t size);
void     tw_up_free(void *p);
/* an update frame ('T','U') arrived: keeps quiet for a while, and takes the chunk if it is for
   this board. Returns 1 if it was one (of any group), 0 if not an update frame */
int      tw_up_rx(tw_node_t *n, const uint8_t *data, int len, uint32_t now_local_us);
/* 1 while this board must send nothing of its own (update frames on the air, or its own burst) */
int      tw_up_hush(const tw_node_t *n, uint32_t now_local_us);
/* the computer gives this board (the exit) a program of size bytes: memory for it, every chunk
   missing. 0, or -1 (too big, no memory, busy) */
int      tw_up_begin(tw_node_t *n, uint32_t size, uint32_t now_local_us);
/* chunk idx of it (len bytes: TW_UP_CHUNK, the last one shorter); 0 or -1 */
int      tw_up_chunk(tw_node_t *n, uint16_t idx, const uint8_t *data, int len);
/* its manifest (TW_UP_MAN_LEN bytes): sets the update's id. 1 if every chunk is there too */
int      tw_up_manifest(tw_node_t *n, const uint8_t *man, uint32_t now_local_us);
/* send the staged program to target (0: every board of the group, this one last); 0 or -1 */
int      tw_up_send(tw_node_t *n, uint32_t now_local_us, uint8_t target);
/* drop any update (sending or receiving) */
void     tw_up_cancel(tw_node_t *n);
/* the exit: its next update frame, if one is due now (its length; 0: none). *next_local_us: when to
   ask again */
int      tw_up_frame(tw_node_t *n, uint32_t now_local_us, uint8_t *buf, int max, uint32_t *next_local_us);
/* once a second: drops a stale update, ends the sending when every target is done (or after
   TW_UP_GIVE_UP_US; then the exit's own turn, if it is one) */
void     tw_up_tick(tw_node_t *n, uint32_t now_local_us);
/* the firmware's word on this board's update (TW_UP_ST_INSTALLING, READY, a failure) */
void     tw_up_set_state(tw_node_t *n, uint8_t state, uint32_t now_local_us);

/* 3.1: this board finished the channel survey (TW_K_SURVEY, generation gen): TW_SURVEY_N entries
   of TW_SURVEY_ENTRY bytes, from channel TW_SURVEY_FIRST up; sent as a TW_KIND_SURVEY record
   every 5 s for TW_SURVEY_SEND_US */
void     tw_survey_done(tw_node_t *n, uint32_t now, uint8_t gen, const uint8_t *entries, int count);
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
/* a new setting given at this board (the computer asked for it), for every board: the network
   key's next generation. Power in quarter dBm, TW_POWER_MIN_Q..TW_POWER_MAX_Q. Returns 0, or -1 if
   out of range */
int      tw_set_power(tw_node_t *n, uint8_t power_q);
/* relaying on (1) or off (0) for every board, the same way (a new setting number) */
void     tw_set_relay(tw_node_t *n, int on);
/* any of the setting flags: the bits in mask become those of value (a new setting number) */
void     tw_set_flags(tw_node_t *n, uint8_t mask, uint8_t value);
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
