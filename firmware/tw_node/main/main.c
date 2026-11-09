/* SPDX-FileCopyrightText: 2026 Richard Nelson
 * SPDX-License-Identifier: Apache-2.0 */
/*
 * tw_node - two-witnesses many-board firmware, one image for every board.
 *
 * Built on Espressif's esp-csi examples (examples/get-started/csi_send and
 * csi_recv at esp-csi commit 8633d67, ESP-IDF v5.5.6): the same channel (11,
 * 40 MHz, secondary below), the same ESP-NOW rate (HT40 MCS0), the same CSI
 * settings and gain compensation, so its measurements stay comparable with
 * the recordings made so far. What is new: every board both sends and listens,
 * each board has its own address (1a:00:00:00:00:NN, NN = board number), and
 * each frame carries the sender's own measurements of the others (tw_proto.c).
 *
 * Roles move by themselves:
 *   exit        any board with a computer reading its native USB port prints
 *               everything it has (its own measurements and those relayed to it)
 *   timekeeper  the lowest-numbered board heard in the last 2 s
 *
 * Board number: from the table below (by factory MAC), or set once with
 *   tw_setid.py /dev/ttyACM0 N     (stored in flash; survives reflashing the
 *   same image; erase-flash clears it)
 *
 * Transmit power: set at the board on the computer, for every board at once:
 *   tw_setid.py /dev/ttyACM0 power 11      (dBm, 2..20; "power default" = 20, full)
 * The setting spreads in every frame (tw_proto.h), is kept in each board's flash,
 * and is reported per board (TWN, TWS lines; the dashboard shows it).
 *
 * Relaying (on by default): when the exit hears a board badly, the boards that
 * hear it well carry its records too (tw_proto.h). For a comparison run:
 *   tw_setid.py /dev/ttyACM0 relay off      (relay on: back to the default)
 * a network setting like the power.
 *
 * A second frame in the slot (2.1, on by default; docs/SECOND-FRAME.md): when
 * records are still waiting after a board's frame has gone out (ESP-NOW's
 * send-done), it sends an extra frame in the same slot. Test settings, network
 * settings like the power: tw_setid.py PORT second on|off|force,
 * measure-second on|off, cap N|off.
 *
 * An RGB LED (2.4, off by default; per board, kept in its flash): shows the board's own
 * movement score in the replay's colors (tw_led.h). tw_setid.py PORT led on [GPIO] (48 if not
 * given), led off, led bright N (1..255, 32 by default), led test, led try R G B (2.4.2: that
 * color for 10 s, to choose colors by eye).
 *
 * Output on the native USB port (text lines; listen.py adds the computer's time):
 *   TWR,...  one measurement record   TWN,...  a board heard (once a second)
 *   TWS,...  this board's status      TWH,...  hello: board number, MAC, version
 * Debug messages (warnings only) go to the COM/UART port.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "driver/rmt_tx.h"
#include "tw_led.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "esp_csi_gain_ctrl.h"

#include "tw_proto.h"

#define FW_VERSION              "tw_node 3.1"
#define TW_CHANNEL              11      /* 3.2: the channel of a board with nothing in its flash */
#define TW_PHYMODE              WIFI_PHY_MODE_HT40
#define TW_RATE                 WIFI_PHY_RATE_MCS0_LGI
#define CSI_MAX                 384         /* HT40 non-STBC buffer; longer ones are cut */
#define ESPNOW_HDR_SKIP         15          /* user data starts here in info->payload (as csi_recv) */

static const char *TAG = "tw_node";

/* known boards: factory MAC -> board number. Usually left empty: give each board its
   number once with tw_setid.py (kept in the board's own flash, and it wins over this
   table). To have numbers without tw_setid.py, list your boards here and rebuild.
   Any board can be the exit; this only sets the numbers. */
static const struct { uint8_t mac[6]; uint8_t id; } KNOWN[] = {
    {{0, 0, 0, 0, 0, 0}, 0},                       /* placeholder: matches no board */
    /* {{0x30, 0x30, 0xf9, 0x12, 0x34, 0x56}, 1},     example: board 1 */
};

typedef enum { EV_CSI, EV_FRAME, EV_SENT } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    uint32_t  local_us;
    union {
        struct {
            uint8_t  src, kflags, agc, phy;     /* kflags: TW_KIND_FWI, TW_KIND_RXERR */
            int8_t   rssi, noise, fft;
            uint32_t seq, rx_us;
            uint16_t len, flen;                 /* CSI bytes; 2.3: the frame's length (our payload) */
            float    gain;
            int8_t   csi[CSI_MAX];
        } c;
        struct {
            uint16_t len;
            uint32_t rx_ts;                     /* 2.3: the radio's receive time (rx_ctrl), 0: none */
            uint8_t  data[TW_MAX_PAYLOAD];
        } f;
        struct {
            uint8_t  ok;                /* ESP-NOW's send status */
            uint8_t  which;             /* 2.3: 1 our normal frame, 2 our extra frame (0: unknown) */
            uint8_t  x;                 /* 2.3, after a normal frame: the extra frame built ahead was
                                           1 sent from the callback, 2 refused there, 3 too late (0: not
                                           taken by the callback) */
            uint32_t x_hand;            /* when the callback handed it over */
        } s;
    };
} ev_t;

static tw_node_t s_node;
static QueueHandle_t s_q;
static uint8_t s_id;
static uint8_t s_fake_mac[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x00};
static int s_max_payload = TW_MAX_PAYLOAD;
static uint32_t s_usb_drops, s_q_full, s_send_err, s_csi_count, s_rx_err;
/* the frames of our slot: 0 none going out, 1 the normal frame, 2 the extra frame (work task only) */
static uint8_t s_tx_state;
static uint8_t s_frame2[TW_MAX_PAYLOAD];
static const uint8_t BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
/* 2.3: the extra frame is built while the normal one is on the air, and ESP-NOW's send-done
   callback (Wi-Fi task, the other core) sends it if it still fits. The callback touches only
   these: s_cb_state, which of our frames is out (set before each send: 1 normal, 2 extra);
   s_x_ready, an extra frame built and waiting (whoever takes it, callback or work task, clears
   it); s_x_len and s_x_latest (our clock: the latest it may go), written before s_x_ready. */
static volatile uint8_t s_cb_state;
static uint8_t s_x_ready;
static int s_x_len;
static uint32_t s_x_latest;
/* hand-over to send-done, less the air, us: running mean and spread (2.2 boards: 650 us); the
   extra frame is cut for a send-done at mean + 2 spreads */
static int32_t s_ovh = 650, s_ovh_dev = 60;
static uint32_t s_x_cb, s_x_task, s_x_refused;     /* extra frames sent from the callback; by the task */
static volatile uint8_t s_cb_send = 1;          /* cleared when ESP-NOW refuses a send from the callback:
                                                   from then on the work task sends them */
static uint32_t s_extra_err;
static int32_t s_extra_end_max = INT32_MIN;     /* latest end of our extra frames since the last TWF */
/* 2.2: where a frame's time goes (mean/max over each TWF's 10 s): where in our slot we handed the
   normal frame over (tx_at), from then to its send-done (tx_radio), from that to handing the extra
   frame over (x_wake: our task), from then to its send-done (x_radio); and their lengths */
typedef struct { uint32_t n; uint64_t sum; int32_t max; } tstat_t;
static tstat_t s_t_at, s_t_radio, s_t_wake, s_t_xradio, s_t_len, s_t_xlen;
/* 2.3: how long building the normal frame takes (build), and how late the ESP-NOW receive callback
   comes after a frame's end, as the radio's own receive time says (rxcb) */
static tstat_t s_t_build, s_t_rxcb;
#define BUILD_AHEAD_US          800u        /* 2.3: build the normal frame this long before its slot time */
#define X_ACCESS_US             310u        /* 2.3: allowed for an extra frame to get on the air (2.2: 150;
                                               the boards measured 290 us from hand-over to send-done,
                                               less the air) */

/* 2.3: the radio stamps each received frame (rx_ctrl timestamp, us) on a clock that runs with ours
   but starts elsewhere (about 984 ms apart on the 2.2 boards; it marks the frame's start). The
   CSI callback comes soon after a frame's end: the smallest (callback - stamp - air) over the last
   8 seconds is that offset, plus the callback's least delay. Work task only. */
#define RXK_WIN 8
static int32_t s_rxk_win[RXK_WIN];
static uint8_t s_rxk_n, s_rxk_pos;
static int32_t s_rxk_cur = INT32_MAX;

static void rxk_note(uint32_t cb_local, uint32_t rx_ts, int frame_len)
{
    int32_t d = (int32_t)(cb_local - rx_ts - tw_air_us(frame_len));
    if (d < s_rxk_cur) {
        s_rxk_cur = d;
    }
}

static void rxk_roll(void)                  /* once a second */
{
    if (s_rxk_cur == INT32_MAX) {
        return;
    }
    s_rxk_win[s_rxk_pos] = s_rxk_cur;
    s_rxk_pos = (uint8_t)((s_rxk_pos + 1) % RXK_WIN);
    if (s_rxk_n < RXK_WIN) {
        s_rxk_n++;
    }
    s_rxk_cur = INT32_MAX;
}

/* the offset (radio's receive time -> our clock), or 0 with *ok = 0 when not learned yet */
static int32_t rxk_get(int *ok)
{
    int32_t k = s_rxk_cur;
    for (int i = 0; i < s_rxk_n; i++) {
        if (s_rxk_win[i] < k) {
            k = s_rxk_win[i];
        }
    }
    *ok = k != INT32_MAX;
    return *ok ? k : 0;
}
static uint32_t s_hand1_us, s_cb1_us, s_hand2_us;
static int s_len1;
static uint32_t s_stamp1;                       /* 2.3: the time our normal frame is stamped with */

/* 2.2: how busy each core was since the last call, % (FreeRTOS counts each core's idle time on
   esp_timer, 1 MHz; 32 bits wrap after 71 min, so only differences over 10 s are used) */
static uint32_t s_idle_prev[2], s_wall_prev;

static void core_load(unsigned busy[2])
{
    uint32_t wall = (uint32_t)esp_timer_get_time(), dw = wall - s_wall_prev;
    for (int c = 0; c < 2; c++) {
        uint32_t idle = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(c), di = idle - s_idle_prev[c];
        busy[c] = (s_wall_prev == 0 || dw == 0 || di >= dw) ? 0u : (unsigned)(100u - (uint32_t)((uint64_t)di * 100u / dw));
        s_idle_prev[c] = idle;
    }
    s_wall_prev = wall;
}

static void tstat_add(tstat_t *t, int32_t v)
{
    if (t->n == 0 || v > t->max) {
        t->max = v;
    }
    t->n++;
    t->sum += (uint64_t)(v < 0 ? 0 : v);
}

/* "mean/max" for the TWF line (0/0 if none), then cleared */
static const char *tstat_text(tstat_t *t, char *buf, size_t len)
{
    snprintf(buf, len, "%lu/%ld", (unsigned long)(t->n ? t->sum / t->n : 0), (long)(t->n ? t->max : 0));
    memset(t, 0, sizeof(*t));
    return buf;
}
static tw_hdr_t s_last_hdr[TW_MAX_NODES + 1];
static int8_t s_last_rssi[TW_MAX_NODES + 1];

/* ---------------- board number ---------------- */
static uint8_t board_id(void)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    nvs_handle_t h;
    uint8_t id = 0;
    if (nvs_open("tw", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "id", &id);
        nvs_close(h);
    }
    if (id >= 1 && id <= TW_MAX_NODES) {
        return id;
    }
    for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
        if (!memcmp(mac, KNOWN[i].mac, 6)) {
            return KNOWN[i].id;
        }
    }
    ESP_LOGW(TAG, "board " MACSTR " has no number: set one with tw_setid.py (using 16 for now)", MAC2STR(mac));
    return TW_MAX_NODES;
}

/* the network setting, kept across restarts */
static void load_settings(void)
{
    nvs_handle_t h;
    uint8_t seq = 0, pwr = 0, flg = 0, chan = 0;
    if (nvs_open("tw", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "pseq", &seq);
        nvs_get_u8(h, "pwr", &pwr);
        nvs_get_u8(h, "rflg", &flg);
        nvs_get_u8(h, "chan", &chan);   /* 3.2 */
        nvs_close(h);
    }
    s_node.set_channel = tw_chan_valid(chan) ? chan : TW_CHANNEL;
    if (pwr >= TW_POWER_MIN_Q && pwr <= TW_POWER_MAX_Q) {
        s_node.set_seq = seq;
        s_node.set_power = pwr;
        s_node.set_flags = flg;
        s_node.set_pending = 1;
    }
}

/* apply a new setting to the radio and keep it (called once a second; 3.0: set_pending comes from
   a network key taken, tw_proto.c) */
static void apply_settings(void)
{
    if (s_node.set_pending) {
        s_node.set_pending = 0;
        if (s_node.set_power && esp_wifi_set_max_tx_power((int8_t)s_node.set_power) != ESP_OK) {
            ESP_LOGW(TAG, "could not set transmit power %u/4 dBm", s_node.set_power);
        }
        {                                   /* kept as 2.x kept it (pseq: the setting number) */
            nvs_handle_t h;
            if (nvs_open("tw", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, "pseq", s_node.set_seq);
                nvs_set_u8(h, "pwr", s_node.set_power);
                nvs_set_u8(h, "rflg", s_node.set_flags);
                nvs_commit(h);
                nvs_close(h);
            }
        }
    }
    int8_t q = 0;
    if (esp_wifi_get_max_tx_power(&q) == ESP_OK) {
        s_node.tx_power_q = (uint8_t)q;
    }
}

static void save_id(uint8_t id)
{
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "id", id);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------------- output to the computer ---------------- */
static int usb_up(void) { return usb_serial_jtag_is_connected(); }

static void out(const char *s, int len)
{
    if (len <= 0 || !usb_up()) {
        return;
    }
    if (usb_serial_jtag_write_bytes(s, (size_t)len, 0) < len) {
        s_usb_drops++;                  /* never wait: the radio work comes first */
    }
}

/* answers to the computer's commands: wait (briefly) for room rather than lose them */
static void out_wait(const char *s, int len)
{
    if (len > 0 && usb_up() && usb_serial_jtag_write_bytes(s, (size_t)len, pdMS_TO_TICKS(200)) < len) {
        s_usb_drops++;
    }
}

/* ---------------- Wi-Fi and ESP-NOW, as Espressif's csi_send/csi_recv ---------------- */
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_node.set_channel, tw_chan_partner(s_node.set_channel) > 0 ?
                                         WIFI_SECOND_CHAN_ABOVE : WIFI_SECOND_CHAN_BELOW));    /* 3.2 */
    s_fake_mac[5] = s_id;
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, s_fake_mac));
}

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < TW_HDR_LEN || len > TW_MAX_PAYLOAD || data[0] != TW_MAGIC0 || data[1] != TW_MAGIC1) {
        return;
    }
    static ev_t ev;                     /* the Wi-Fi task calls us one at a time */
    ev.kind = EV_FRAME;
    ev.local_us = (uint32_t)esp_timer_get_time();
    ev.f.len = (uint16_t)len;
    ev.f.rx_ts = (info && info->rx_ctrl) ? info->rx_ctrl->timestamp : 0;
    memcpy(ev.f.data, data, (size_t)len);
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
        s_q_full++;
    }
}

/* a frame we sent has gone out (or failed): tell the work task first of all, ahead of anything
   queued, so an extra frame can follow at once */
static void espnow_send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    static ev_t ev;                     /* the Wi-Fi task calls us one at a time */
    uint32_t now = (uint32_t)esp_timer_get_time();
    ev.kind = EV_SENT;
    ev.local_us = now;
    ev.s.ok = status == ESP_NOW_SEND_SUCCESS;
    ev.s.which = s_cb_state;
    ev.s.x = 0;
    if (s_cb_state == 1) {
        s_cb_state = 0;
        /* 2.3: our normal frame is out: the extra frame at once, if it is built and still fits */
        if (s_cb_send && __atomic_exchange_n(&s_x_ready, 0, __ATOMIC_ACQ_REL)) {
            if ((int32_t)(now - s_x_latest) > 0) {
                ev.s.x = 3;
            } else {
                s_cb_state = 2;
                ev.s.x_hand = (uint32_t)esp_timer_get_time();
                if (esp_now_send(BCAST, s_frame2, (size_t)s_x_len) == ESP_OK) {
                    ev.s.x = 1;
                } else {
                    s_cb_state = 0;
                    s_cb_send = 0;
                    ev.s.x = 2;
                }
            }
        }
    } else if (s_cb_state == 2) {
        s_cb_state = 0;
    }
    if (xQueueSendToFront(s_q, &ev, 0) != pdTRUE) {
        s_q_full++;
    }
}

static void espnow_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)"pmk1234567890123"));
    esp_now_peer_info_t peer = {
        .channel = s_node.set_channel,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
        .peer_addr = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    };
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    esp_now_rate_config_t rate = {.phymode = TW_PHYMODE, .rate = TW_RATE, .ersu = false, .dcm = false};
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer.peer_addr, &rate));
    uint32_t ver = 0;
    if (esp_now_get_version(&ver) == ESP_OK && ver < 2) {
        s_max_payload = 250;            /* ESP-NOW v1: small frames only */
        ESP_LOGW(TAG, "ESP-NOW v%lu: frames limited to 250 bytes", (unsigned long)ver);
    }
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
}

/* ---------------- CSI ---------------- */
static void csi_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    if (!info || !info->buf || memcmp(info->mac, s_fake_mac, 5) || info->mac[5] < 1 ||
            info->mac[5] > TW_MAX_NODES || info->mac[5] == s_id) {
        return;                         /* not one of our boards */
    }
    tw_hdr_t h;
    if (!info->payload || info->payload_len < ESPNOW_HDR_SKIP + TW_HDR_LEN ||
            tw_peek_header(info->payload + ESPNOW_HDR_SKIP, info->payload_len - ESPNOW_HDR_SKIP, &h) < 0 ||
            h.src != info->mac[5]) {
        return;
    }
    const wifi_pkt_rx_ctrl_t *rx = &info->rx_ctrl;
    static ev_t ev;                     /* the Wi-Fi task calls us one at a time */
    ev.kind = EV_CSI;
    ev.local_us = (uint32_t)esp_timer_get_time();
    ev.c.src = h.src;
    ev.c.seq = h.seq;
    ev.c.kflags = (uint8_t)((info->first_word_invalid ? TW_KIND_FWI : 0) | (rx->rx_state ? TW_KIND_RXERR : 0) |
                            ((h.flags & TW_F_EXTRA) ? TW_KIND_EXTRA : 0));
    ev.c.rssi = rx->rssi;
    ev.c.noise = rx->noise_floor;
    ev.c.rx_us = rx->timestamp;
    ev.c.phy = (uint8_t)((rx->cwb & 3) | ((rx->stbc & 1) << 2) | ((rx->sig_mode & 3) << 3) |
                         ((rx->secondary_channel & 3) << 5));
    /* gain compensation, as Espressif's csi_recv: baseline from the first 100 packets */
    uint8_t agc = 0;
    int8_t fft = 0;
    float gain = 1.0f;
    esp_csi_gain_ctrl_get_rx_gain(rx, &agc, &fft);
    if (s_csi_count < 100) {
        esp_csi_gain_ctrl_record_rx_gain(agc, fft);
    }
    esp_csi_gain_ctrl_get_gain_compensation(&gain, agc, fft);
    s_csi_count++;
    ev.c.agc = agc;
    ev.c.fft = fft;
    ev.c.gain = gain;
    ev.c.len = info->len > CSI_MAX ? CSI_MAX : info->len;
    ev.c.flen = (uint16_t)(info->payload_len - ESPNOW_HDR_SKIP);
    memcpy(ev.c.csi, info->buf, ev.c.len);
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
        s_q_full++;
    }
}

static void csi_init(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    wifi_csi_config_t csi_config = {    /* exactly csi_recv's settings */
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

/* ---------------- the work loop: one task owns the protocol state ---------------- */
typedef struct {
    uint8_t  src;
    uint32_t now;
} rctx_t;

static void relay_cb(void *ctx, const tw_rec_t *r)
{
    const rctx_t *c = ctx;
    tw_setting_heard(&s_node, r);       /* 2.5+: settings and commands (it picks the kinds it knows) */
    if (!s_node.is_exit) {
        tw_relay_record(&s_node, c->now, c->src, r);    /* carry it on, if this board is a relay for src */
        return;
    }
    if (tw_seen(&s_node, r->rx, r->boot, r->recseq)) {
        return;
    }
    static char line[600];
    out(line, tw_format_record(r, line, sizeof(line)));
}

/* hand the extra frame over from the work task (2.2's way: the fallback), or put it back */
static void extra_from_task(uint32_t now)
{
    if ((int32_t)(now - s_x_latest) > 0) {
        tw_unsend_extra(&s_node);
        return;
    }
    s_hand2_us = now;
    s_cb_state = 2;
    if (esp_now_send(BCAST, s_frame2, (size_t)s_x_len) == ESP_OK) {
        s_tx_state = 2;
        s_x_task++;
    } else {
        s_cb_state = 0;
        s_extra_err++;
        tw_unsend_extra(&s_node);
    }
}

/* ESP-NOW says a frame of ours has gone out. After the normal frame: the callback has sent the
   extra frame, or left it to us (not ready yet when it came, or refused), or found it too late */
static void sent(const ev_t *ev)
{
    if (ev->s.which == 1 && s_tx_state == 1) {
        s_tx_state = 0;
        s_cb1_us = ev->local_us;
        int32_t radio = (int32_t)(s_cb1_us - s_hand1_us);
        tstat_add(&s_t_radio, radio);
        tw_note_sent_normal(&s_node, s_stamp1, s_cb1_us, s_len1);     /* 2.3: our delay, for our headers */
        int32_t o = radio - (int32_t)tw_air_us(s_len1);
        if (o > 0 && o < 3000) {        /* a running mean and spread, 1/16 each frame */
            int32_t d = o - s_ovh;
            s_ovh += d / 16;
            s_ovh_dev += ((d < 0 ? -d : d) - s_ovh_dev) / 16;
        }
        if (ev->s.x == 1) {
            s_hand2_us = ev->s.x_hand;
            s_tx_state = 2;
            s_x_cb++;
        } else if (ev->s.x == 3) {
            tw_unsend_extra(&s_node);
        } else if (ev->s.x == 2 || __atomic_exchange_n(&s_x_ready, 0, __ATOMIC_ACQ_REL)) {
            s_x_refused += ev->s.x == 2;
            extra_from_task((uint32_t)esp_timer_get_time());
        }
        if (s_tx_state == 2) {
            tstat_add(&s_t_wake, (int32_t)(s_hand2_us - s_cb1_us));
        }
    } else if (ev->s.which == 2 && s_tx_state == 2) {
        s_tx_state = 0;
        tstat_add(&s_t_xradio, (int32_t)(ev->local_us - s_hand2_us));
        tw_note_sent(&s_node, ev->local_us, 1);
        int32_t pos = tw_slot_pos(&s_node, ev->local_us, s_id);
        if (pos > s_extra_end_max) {
            s_extra_end_max = pos;
        }
        if (!ev->s.ok) {
            s_extra_err++;
        }
    }
}

static void handle(ev_t *ev)
{
    static char line[600];
    if (ev->kind == EV_SENT) {
        sent(ev);
        return;
    }
    if (ev->kind == EV_FRAME) {
        tw_hdr_t h;
        if (tw_peek_header(ev->f.data, ev->f.len, &h) < 0) {
            s_node.n_bad_frames++;
            return;
        }
        int kok;
        int32_t k = rxk_get(&kok);
        if (kok && ev->f.rx_ts && !(h.flags & TW_F_EXTRA)) {
            /* how late this callback is after the frame's end (by the radio's stamp) */
            tstat_add(&s_t_rxcb, (int32_t)(ev->local_us - (ev->f.rx_ts + (uint32_t)k + tw_air_us(ev->f.len))));
        }
        /* 2.3: the clock from when the frame started on the air (the radio's own receive time),
           not from this callback, which comes about 0.4 ms after the CSI callback */
        tw_note_frame(&s_node, &h, ev->local_us, (kok && ev->f.rx_ts) ? ev->f.rx_ts + (uint32_t)k : 0u);
        if (h.src >= 1 && h.src <= TW_MAX_NODES && !(h.flags & TW_F_EXTRA)) {
            s_last_hdr[h.src] = h;
        }
        rctx_t c = {h.src, ev->local_us};
        if (tw_parse_frame(ev->f.data, ev->f.len, &h, relay_cb, &c) < 0) {
            s_node.n_bad_frames++;
        }
        return;
    }
    rxk_note(ev->local_us, ev->c.rx_us, ev->c.flen);
    int compact = __builtin_popcount(tw_active(&s_node, ev->local_us)) > TW_FULL_MAX_ACTIVE;
    const tw_rec_t *r = tw_add_measurement(&s_node, ev->local_us, ev->c.src, ev->c.seq, ev->c.rx_us,
                                           ev->c.rssi, ev->c.noise, ev->c.agc, ev->c.fft, ev->c.phy,
                                           ev->c.kflags, ev->c.csi, ev->c.len, ev->c.gain, compact);
    if (!r) {
        return;                         /* an extra frame: counted only (tw_add_measurement) */
    }
    if (ev->c.kflags & TW_KIND_RXERR) {
        s_rx_err++;
    }
    s_last_rssi[ev->c.src] = ev->c.rssi;
    if (s_node.is_exit) {
        out(line, tw_format_record(r, line, sizeof(line)));
    }
}

/* 2.4: the RGB LED (a WS2812-type: one data pin, 24 bits per color, green first), driven by the
   RMT peripheral: the hardware times the bits, the work task only queues 3 bytes a second */
#define LED_PIN_DEFAULT  48
#define LED_BRIGHT_DEFAULT 32
static uint8_t s_led_pin, s_led_bright = LED_BRIGHT_DEFAULT;     /* pin 0: no LED */
static rmt_channel_handle_t s_led_ch;
static rmt_encoder_handle_t s_led_enc;
static uint8_t s_led_test;                      /* seconds of the test sequence left */
static uint8_t s_led_try, s_led_try_rgb[3];     /* 2.4.2: seconds of a color to try left, and it */
static uint8_t s_led_shown[3];

static int led_pin_ok(int pin)
{
    /* not the USB pins (19, 20) nor the flash and PSRAM's (26-37 on the S3) */
    return pin >= 0 && pin <= 48 && pin != 19 && pin != 20 && !(pin >= 26 && pin <= 37);
}

static void led_show(const uint8_t rgb[3])
{
    static uint8_t grb[3];              /* stays put while the RMT sends it */
    if (!s_led_ch) {
        return;
    }
    grb[0] = rgb[1];
    grb[1] = rgb[0];
    grb[2] = rgb[2];
    memcpy(s_led_shown, rgb, 3);
    rmt_transmit_config_t t = {.loop_count = 0};
    rmt_transmit(s_led_ch, s_led_enc, grb, sizeof(grb), &t);  /* queued; a full queue just skips */
}

static void led_stop(void)
{
    if (s_led_ch) {
        static const uint8_t off[3] = {0, 0, 0};
        led_show(off);
        rmt_tx_wait_all_done(s_led_ch, 10);
        rmt_disable(s_led_ch);
        rmt_del_channel(s_led_ch);
        rmt_del_encoder(s_led_enc);
        s_led_ch = NULL;
        s_led_enc = NULL;
    }
}

static void led_start(void)
{
    led_stop();
    if (!s_led_pin || !led_pin_ok(s_led_pin)) {
        return;
    }
    rmt_tx_channel_config_t c = {
        .gpio_num = s_led_pin, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000,
        .mem_block_symbols = 48, .trans_queue_depth = 2,
    };
    rmt_bytes_encoder_config_t e = {    /* 0.1 us ticks: a 0 is 0.3 us high, 0.9 low; a 1 the reverse */
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&c, &s_led_ch) != ESP_OK) {
        s_led_ch = NULL;
        ESP_LOGW(TAG, "LED: no RMT channel for GPIO %u", s_led_pin);
        return;
    }
    if (rmt_new_bytes_encoder(&e, &s_led_enc) != ESP_OK || rmt_enable(s_led_ch) != ESP_OK) {
        rmt_del_channel(s_led_ch);
        s_led_ch = NULL;
        ESP_LOGW(TAG, "LED: could not start on GPIO %u", s_led_pin);
        return;
    }
    static const uint8_t off[3] = {0, 0, 0};
    led_show(off);
}

static void led_load(void)
{
    nvs_handle_t h;
    uint8_t gp = 0, gb = 0;
    if (nvs_open("tw", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "led", &s_led_pin);
        nvs_get_u8(h, "ledb", &s_led_bright);
        nvs_get_u8(h, "lgp", &gp);          /* 2.5: the settings' generations */
        nvs_get_u8(h, "lgb", &gb);
        nvs_close(h);
    }
    if (!s_led_bright) {
        s_led_bright = LED_BRIGHT_DEFAULT;
    }
    tw_setting_init(&s_node, TW_K_LED_PIN, &s_led_pin, 1, gp);
    tw_setting_init(&s_node, TW_K_LED_BRIGHT, &s_led_bright, 1, gb);
}

/* what the board says about itself every 5 s (TWV): firmware and build (2.5-2.7 added its LED
   setting; 3.0: that is in its reports, TWX) */
static void info_update(void)
{
    tw_set_info(&s_node, FW_VERSION "|" __DATE__ " " __TIME__);
}

static void led_save(void);
static void led_start(void);

/* 2.6: the network keys' generations, kept in flash (their values as the settings, "pwr" and "rflg") */
static const char *NET_NVS[TW_K_NET_N] = {"ngp", "ngr", "ngs", "ngm", "ngc", "ngh"};   /* 3.2: "ngh" */

static void net_load(void)
{
    uint8_t g[TW_K_NET_N] = {0};
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < TW_K_NET_N; i++) {
            nvs_get_u8(h, NET_NVS[i], &g[i]);
        }
        nvs_close(h);
    }
    tw_net_init(&s_node, g);
}

static void net_save(void)
{
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < TW_K_NET_N; i++) {
            nvs_set_u8(h, NET_NVS[i], s_node.set_gen[TW_K_NET_FIRST + i]);
        }
        nvs_set_u8(h, "chan", s_node.set_channel);     /* 3.2: the channel key's value (where it starts) */
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------------- 3.2: the radio's channel ---------------- */
static uint8_t s_radio_ch;                  /* where the radio is now */

static void radio_channel(uint8_t ch)
{
    if (esp_wifi_set_channel(ch, tw_chan_partner(ch) > 0 ? WIFI_SECOND_CHAN_ABOVE : WIFI_SECOND_CHAN_BELOW) != ESP_OK) {
        ESP_LOGW(TAG, "could not go to channel %u", ch);
        return;
    }
    esp_now_peer_info_t p;
    if (esp_now_get_peer(BCAST, &p) == ESP_OK) {
        p.channel = ch;
        esp_now_mod_peer(&p);
    }
    s_radio_ch = ch;
}

/* ---------------- 3.1: the channel survey ----------------
   Every board at once (TW_K_SURVEY, an action for all): TW_SURVEY_DELAY_US after taking it, a board
   leaves its channel and listens TW_SURVEY_DWELL_MS on each of channels TW_SURVEY_FIRST.. (20 MHz,
   every frame the radio can decode), then comes back. Our own boards are silent then (and any that
   is not is left out by its address), so it counts the others' traffic: how many frames, how loud,
   and how much of the time they took (busy: their air time, from each frame's length and rate). Not
   seen: what the radio cannot decode (Bluetooth, a microwave oven, a weak distant network): only as
   the noise floor. The network pauses for about 12 s and starts again as after any gap. */
typedef struct {
    uint32_t n;                         /* foreign frames */
    uint32_t air_us;                    /* their air time */
    int32_t  rssi_sum, noise_sum;
    int8_t   rssi_max;
} svy_t;
static svy_t s_svy;                     /* written by the Wi-Fi task while s_svy_on, read after */
static volatile uint8_t s_svy_on;
static uint32_t s_survey_at;            /* local time to start (when s_survey_due) */
static uint8_t s_survey_due, s_survey_gen;

/* a frame's air time, us: legacy (11b DSSS/CCK, 11g OFDM) or 11n, from its length and rate */
static uint32_t frame_air_us(const wifi_pkt_rx_ctrl_t *rx)
{
    uint32_t len = rx->sig_len, bits = 16u + 8u * len + 6u;
    if (rx->sig_mode == 0) {
        /* wifi_phy_rate_t codes 0..15: Mbps x 10 (code 4 is not used) */
        static const uint16_t r10[16] = {10, 20, 55, 110, 10, 20, 55, 110, 480, 240, 120, 60, 540, 360, 180, 90};
        unsigned code = rx->rate & 15u, r = r10[code];
        if (code < 8) {                 /* DSSS/CCK: long (192 us) or short (96 us) preamble */
            return (code < 4 ? 192u : 96u) + (80u * len + r - 1u) / r;
        }
        uint32_t bps = 4u * r / 10u;    /* bits per 4-us OFDM symbol */
        return 20u + 4u * ((bits + bps - 1u) / bps);
    }
    static const uint16_t ht20[8] = {26, 52, 78, 104, 156, 208, 234, 260};
    static const uint16_t ht40[8] = {54, 108, 162, 216, 324, 432, 486, 540};
    uint32_t bps = rx->cwb ? ht40[rx->mcs & 7u] : ht20[rx->mcs & 7u];
    return 36u + 4u * ((bits + bps - 1u) / bps);
}

static void survey_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_svy_on || !buf) {
        return;
    }
    const wifi_promiscuous_pkt_t *p = buf;
    const wifi_pkt_rx_ctrl_t *rx = &p->rx_ctrl;
    if (type != WIFI_PKT_MISC && rx->sig_len >= 16 && !memcmp(p->payload + 10, s_fake_mac, 5)) {
        return;                         /* one of our boards (its sender address): not counted */
    }
    s_svy.n++;
    s_svy.air_us += frame_air_us(rx);
    s_svy.rssi_sum += rx->rssi;
    s_svy.noise_sum += rx->noise_floor;
    if (rx->rssi > s_svy.rssi_max) {
        s_svy.rssi_max = (int8_t)rx->rssi;
    }
}

static int8_t clamp8(int32_t v)
{
    return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
}

/* the survey itself (the work task, about 12 s): the result goes to the protocol state, which sends
   it; the board then starts its slot again as after a restart */
static void survey_run(uint32_t *next_tx)
{
    uint8_t res[TW_SURVEY_N * TW_SURVEY_ENTRY];
    wifi_promiscuous_filter_t keep = {0}, all = {.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL};
    static const uint8_t off[3] = {0, 0, 0};
    if (s_led_ch) {
        led_show(off);
    }
    ESP_LOGW(TAG, "channel survey %u: channels %d..%d, %d ms each", s_survey_gen, TW_SURVEY_FIRST,
             TW_SURVEY_FIRST + TW_SURVEY_N - 1, TW_SURVEY_DWELL_MS);
    esp_wifi_get_promiscuous_filter(&keep);
    esp_wifi_set_promiscuous_filter(&all);
    esp_wifi_set_promiscuous_rx_cb(survey_cb);
    for (int i = 0; i < TW_SURVEY_N; i++) {
        uint8_t *e = res + i * TW_SURVEY_ENTRY;
        memset(e, 0, TW_SURVEY_ENTRY);
        if (esp_wifi_set_channel((uint8_t)(TW_SURVEY_FIRST + i), WIFI_SECOND_CHAN_NONE) != ESP_OK) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(20));  /* settle on the channel */
        memset(&s_svy, 0, sizeof(s_svy));
        s_svy.rssi_max = -128;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        uint32_t t0 = (uint32_t)esp_timer_get_time();
        s_svy_on = 1;
        vTaskDelay(pdMS_TO_TICKS(TW_SURVEY_DWELL_MS));
        s_svy_on = 0;
        uint32_t dt = (uint32_t)esp_timer_get_time() - t0;
        vTaskDelay(pdMS_TO_TICKS(5));   /* a callback under way finishes */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        uint32_t busy = dt ? (uint32_t)((uint64_t)s_svy.air_us * 200u / dt) : 0;
        e[0] = (uint8_t)(busy > 200u ? 200u : busy);
        uint32_t n = s_svy.n > 0xffffu ? 0xffffu : s_svy.n;
        e[1] = (uint8_t)n;
        e[2] = (uint8_t)(n >> 8);
        if (s_svy.n) {
            e[3] = (uint8_t)s_svy.rssi_max;
            e[4] = (uint8_t)clamp8(s_svy.rssi_sum / (int32_t)s_svy.n);
            e[5] = (uint8_t)clamp8(s_svy.noise_sum / (int32_t)s_svy.n);
        }
    }
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_set_promiscuous_filter(&keep);
    uint8_t home = s_node.chan_home;    /* 3.2: its channel (3.1: always 11) */
    if (esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40) != ESP_OK ||
        esp_wifi_set_channel(home, tw_chan_partner(home) > 0 ? WIFI_SECOND_CHAN_ABOVE : WIFI_SECOND_CHAN_BELOW) != ESP_OK) {
        ESP_LOGE(TAG, "could not come back to channel %u: restarting", home);
        esp_restart();                  /* it starts on its own channel */
    }
    s_radio_ch = home;
    uint32_t now = (uint32_t)esp_timer_get_time();
    tw_survey_done(&s_node, now, s_survey_gen, res, TW_SURVEY_N);
    tw_update_roles(&s_node, now);      /* who is still there: nobody, for a moment */
    *next_tx = now + 50000u + esp_random() % 50000u;    /* as at the start */
    s_led_shown[0] = s_led_shown[1] = s_led_shown[2] = 255;     /* redraw */
}

/* the board's keys changed (over its cable, or by record): take the settings, run the actions */
static void led_apply(void)
{
    uint32_t ch = s_node.set_changed;
    s_node.set_changed = 0;
    if (ch & (((1u << TW_K_NET_N) - 1) << TW_K_NET_FIRST)) {
        net_save();                     /* 2.6: a network key's generation moved on */
    }
    if ((ch & (1u << TW_K_LED_TEST)) && s_led_ch) {        /* 2.5.1: actions, once per generation */
        s_led_test = 7;
        s_led_try = 0;
    }
    if (ch & (1u << TW_K_SURVEY)) {     /* 3.1: the channel survey, in TW_SURVEY_DELAY_US */
        s_survey_gen = s_node.set_gen[TW_K_SURVEY];
        s_survey_at = (uint32_t)esp_timer_get_time() + TW_SURVEY_DELAY_US;
        s_survey_due = 1;
    }
    if ((ch & (1u << TW_K_LED_TRY)) && s_led_ch) {
        memcpy(s_led_try_rgb, s_node.set_val[TW_K_LED_TRY], 3);
        s_led_try = 10;
        s_led_test = 0;
    }
    if (!(ch & ((1u << TW_K_LED_PIN) | (1u << TW_K_LED_BRIGHT)))) {
        return;
    }
    uint16_t pin = tw_setting_u16(&s_node, TW_K_LED_PIN), br = tw_setting_u16(&s_node, TW_K_LED_BRIGHT);
    uint8_t new_pin = (uint8_t)(pin <= 48 && led_pin_ok(pin) ? pin : 0);   /* a pin it must not drive: none */
    uint8_t new_br = (uint8_t)(br >= 1 && br <= 255 ? br : s_led_bright);
    int restart = new_pin != s_led_pin;
    s_led_pin = new_pin;
    s_led_bright = new_br;
    led_save();
    if (restart) {
        led_start();
    } else {
        s_led_shown[0] = s_led_shown[1] = s_led_shown[2] = 255;   /* redraw */
    }
    info_update();
}

static void led_save(void)
{
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "led", s_led_pin);
        nvs_set_u8(h, "ledb", s_led_bright);
        nvs_set_u8(h, "lgp", s_node.set_gen[TW_K_LED_PIN]);
        nvs_set_u8(h, "lgb", s_node.set_gen[TW_K_LED_BRIGHT]);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* once a second: the test sequence, or what the board's scores say */
static void led_second(void)
{
    if (!s_led_ch) {
        return;
    }
    uint8_t c[3];
    if (s_led_try) {
        s_led_try--;                    /* 2.4.2: exactly the numbers given, for 10 seconds */
        memcpy(c, s_led_try_rgb, 3);
    } else if (s_led_test) {
        /* red, green, blue, then the ramp (1.5, 3, 6 x quiet), then off */
        static const uint8_t prim[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
        static const uint8_t ramp[3] = {15, 30, 60};
        int step = 7 - s_led_test--;
        if (step < 3) {
            for (int k = 0; k < 3; k++) {
                c[k] = (uint8_t)((prim[step][k] * s_led_bright) / 255);
            }
        } else {
            tw_led_color(step < 6 ? ramp[step - 3] : 0, s_led_bright, c);
        }
    } else {
        tw_led_color(tw_led_pick(s_node.score_last), s_led_bright, c);
    }
    if (memcmp(c, s_led_shown, 3)) {
        led_show(c);
    }
}

static void hello(void);

static void poll_commands(void)
{
    static char cmd[64];
    static int n = 0;
    uint8_t ch;
    while (usb_serial_jtag_read_bytes(&ch, 1, 0) == 1) {
        if (ch == '\n' || ch == '\r') {
            cmd[n] = 0;
            int id;
            float dbm;
            if (n && (!strcmp(cmd, "TWSET relay on") || !strcmp(cmd, "TWSET relay off"))) {
                tw_set_relay(&s_node, !strcmp(cmd, "TWSET relay on"));
                apply_settings();
                char msg[64];
                int k = snprintf(msg, sizeof(msg), "TWOK relay %s, setting %u\n",
                                 (s_node.set_flags & TW_SET_RELAY_OFF) ? "off" : "on", s_node.set_seq);
                out_wait(msg, k);
            } else if (n && (!strncmp(cmd, "TWSET second ", 13) || !strncmp(cmd, "TWSET measure-second ", 21) ||
                             !strncmp(cmd, "TWSET cap ", 10))) {
                const uint8_t sec = TW_SET_SECOND_OFF | TW_SET_SECOND_FORCE;
                uint8_t mask = 0, val = 0;
                int cap = 0;
                if (!strcmp(cmd, "TWSET second on")) {
                    mask = sec;
                } else if (!strcmp(cmd, "TWSET second off")) {
                    mask = sec;
                    val = TW_SET_SECOND_OFF;
                } else if (!strcmp(cmd, "TWSET second force")) {
                    mask = sec;
                    val = TW_SET_SECOND_FORCE;
                } else if (!strcmp(cmd, "TWSET measure-second on") || !strcmp(cmd, "TWSET measure-second off")) {
                    mask = TW_SET_MEASURE_EXTRA;
                    val = strcmp(cmd, "TWSET measure-second on") ? 0 : TW_SET_MEASURE_EXTRA;
                } else if (!strcmp(cmd, "TWSET cap off")) {
                    mask = 0xF0;
                } else if (sscanf(cmd, "TWSET cap %d", &cap) == 1 && cap >= 1 && cap <= 15) {
                    mask = 0xF0;
                    val = (uint8_t)(cap << TW_SET_CAP_SHIFT);
                }
                char msg[128];
                int k;
                if (mask) {
                    tw_set_flags(&s_node, mask, val);
                    apply_settings();
                    uint8_t f = s_node.set_flags;
                    k = snprintf(msg, sizeof(msg), "TWOK flags %u: second %s, measure-second %s, cap %u, setting %u\n", f,
                                 (f & TW_SET_SECOND_FORCE) ? "force" : (f & TW_SET_SECOND_OFF) ? "off" : "on",
                                 (f & TW_SET_MEASURE_EXTRA) ? "on" : "off", f >> TW_SET_CAP_SHIFT, s_node.set_seq);
                } else {
                    k = snprintf(msg, sizeof(msg), "TWERR second on|off|force, measure-second on|off, cap 1..15|off\n");
                }
                out_wait(msg, k);
            } else if (n && (!strcmp(cmd, "TWSET power default") ||
                      (sscanf(cmd, "TWSET power %f", &dbm) == 1))) {
                if (!strcmp(cmd, "TWSET power default")) {
                    dbm = TW_POWER_MAX_Q / 4.0f;
                }
                char msg[96];
                int q = (int)(dbm * 4.0f + 0.5f), k;
                if (tw_set_power(&s_node, (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q)) == 0) {
                    apply_settings();
                    k = snprintf(msg, sizeof(msg), "TWOK power %.2f dBm, setting %u (radio reports %.2f dBm)\n",
                                 q / 4.0, s_node.set_seq, s_node.tx_power_q / 4.0);
                } else {
                    k = snprintf(msg, sizeof(msg), "TWERR power must be %d..%d dBm\n",
                                 TW_POWER_MIN_Q / 4, TW_POWER_MAX_Q / 4);
                }
                out_wait(msg, k);
            } else if (n && !strncmp(cmd, "TWSET led ", 10)) {
                /* 2.4: this board's LED (kept in its flash). 2.5+: "... board N gen G" gives it to
                   board N (0 = all) by record instead, generation G (the computer chooses it: one
                   more than the boards report). 2.5.1: test and try are actions, the same way */
                char msg[128], *bd = strstr(cmd, " board ");
                int k = 0, pin, br, tr, tg, tb, target = -1, gen = -1, key = 0, vlen = 0;
                uint8_t val[3] = {0, 0, 0};
                if (bd) {
                    if (sscanf(bd, " board %d gen %d", &target, &gen) != 2 || target < 0 ||
                        target > TW_MAX_NODES || gen < 0 || gen > 255) {
                        target = -2;
                    }
                    *bd = 0;                    /* the rest is the setting itself */
                }
                if (!strcmp(cmd, "TWSET led off")) {
                    key = TW_K_LED_PIN;
                    vlen = 1;
                } else if (!strcmp(cmd, "TWSET led on")) {
                    key = TW_K_LED_PIN;
                    val[0] = LED_PIN_DEFAULT;
                    vlen = 1;
                } else if (sscanf(cmd, "TWSET led on %d", &pin) == 1 && led_pin_ok(pin) && pin > 0) {
                    key = TW_K_LED_PIN;
                    val[0] = (uint8_t)pin;
                    vlen = 1;
                } else if (sscanf(cmd, "TWSET led bright %d", &br) == 1 && br >= 1 && br <= 255) {
                    key = TW_K_LED_BRIGHT;
                    val[0] = (uint8_t)br;
                    vlen = 1;
                } else if (sscanf(cmd, "TWSET led try %d %d %d", &tr, &tg, &tb) == 3 &&
                           tr >= 0 && tr <= 255 && tg >= 0 && tg <= 255 && tb >= 0 && tb <= 255) {
                    /* 2.4.2: a color as given (red, green, blue; not scaled by the brightness) for
                       10 seconds, then back to what the LED shows: to choose colors by eye */
                    key = TW_K_LED_TRY;
                    val[0] = (uint8_t)tr;
                    val[1] = (uint8_t)tg;
                    val[2] = (uint8_t)tb;
                    vlen = 3;
                } else if (!strcmp(cmd, "TWSET led test")) {
                    key = TW_K_LED_TEST;
                }
                const char *what = key == TW_K_LED_PIN ? "pin" : key == TW_K_LED_BRIGHT ? "brightness" :
                                   key == TW_K_LED_TEST ? "test" : "try";
                if (key && target == -1) {      /* this board, over its cable */
                    int action = tw_key_class((uint8_t)key) == TW_KC_ACTION;
                    tw_setting_local(&s_node, (uint8_t)key, val, vlen);
                    led_apply();
                    if (action && !s_led_ch) {
                        k = snprintf(msg, sizeof(msg), "TWERR led is off: led on first\n");
                    } else if (key == TW_K_LED_TEST) {
                        k = snprintf(msg, sizeof(msg), "TWOK led test: red, green, blue, three blues, off (7 s)\n");
                    } else if (key == TW_K_LED_TRY) {
                        k = snprintf(msg, sizeof(msg), "TWOK led try %u %u %u (10 s)\n", val[0], val[1], val[2]);
                    } else if (s_led_pin) {
                        k = snprintf(msg, sizeof(msg), "TWOK led on GPIO %u, brightness %u%s\n", s_led_pin,
                                     s_led_bright, s_led_ch ? "" : " (could not start it)");
                    } else {
                        k = snprintf(msg, sizeof(msg), "TWOK led off (brightness %u)\n", s_led_bright);
                    }
                } else if (key && target >= 0) {   /* given to board N (0: all) by record */
                    tw_setting_give(&s_node, (uint8_t)target, (uint8_t)key, val, vlen, (uint8_t)gen);
                    led_apply();                /* if it was for this board too */
                    char who[12];
                    snprintf(who, sizeof(who), target ? "board %d" : "all boards", target);
                    k = snprintf(msg, sizeof(msg), "TWOK led %s given to %s, generation %d\n", what, who, gen);
                } else {
                    k = snprintf(msg, sizeof(msg), "TWERR led on [GPIO], led off, led bright 1..255, led test, "
                                 "led try R G B [board N gen G] (not GPIO 19, 20, 26-37)\n");
                }
                out_wait(msg, k);
                hello();
            } else if (n && sscanf(cmd, "TWSET channel %d", &id) == 1) {
                /* 3.2: the channel, for every board: each moves TW_CHAN_SWITCH_US after taking it */
                char msg[128];
                int k;
                if (id >= 0 && id <= 255 && tw_set_channel(&s_node, (uint8_t)id) == 0) {
                    led_apply();
                    k = snprintf(msg, sizeof(msg), "TWOK channel %d given to every board, setting %u: they move "
                                 "in %u s\n", id, s_node.set_seq, TW_CHAN_SWITCH_US / 1000000u);
                } else {
                    k = snprintf(msg, sizeof(msg), "TWERR channel must be %d..%d\n", TW_CHAN_FIRST, TW_CHAN_LAST);
                }
                out_wait(msg, k);
            } else if (n && !strncmp(cmd, "TWSET survey", 12)) {
                /* 3.1: the channel survey, for every board at once (generation G: the computer chooses
                   it, one more than the boards report) */
                char msg[160];
                int gen = -1, k;
                if (sscanf(cmd, "TWSET survey gen %d", &gen) == 1 && gen >= 1 && gen <= 255) {
                    tw_setting_give(&s_node, TW_SET_ALL, TW_K_SURVEY, NULL, 0, (uint8_t)gen);
                    led_apply();
                    k = snprintf(msg, sizeof(msg), "TWOK survey given to all boards, generation %d: in %u s, "
                                 "channels %d..%d, about %d s away\n", gen, TW_SURVEY_DELAY_US / 1000000u,
                                 TW_SURVEY_FIRST, TW_SURVEY_FIRST + TW_SURVEY_N - 1,
                                 TW_SURVEY_N * (TW_SURVEY_DWELL_MS + 30) / 1000 + 1);
                } else {
                    k = snprintf(msg, sizeof(msg), "TWERR survey gen G (1..255)\n");
                }
                out_wait(msg, k);
            } else if (n && sscanf(cmd, "TWSET id %d", &id) == 1 && id >= 1 && id <= TW_MAX_NODES) {
                save_id((uint8_t)id);
                char msg[64];
                int k = snprintf(msg, sizeof(msg), "TWOK id %d, restarting\n", id);
                out_wait(msg, k);
                usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
            n = 0;
        } else if (n < (int)sizeof(cmd) - 1) {
            cmd[n++] = (char)ch;
        }
    }
}

static void hello(void)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char line[180];
    /* ...,max payload, 2.2: PSRAM found (bytes; 0 = none), 2.4: the LED's pin (0 = none), brightness */
    int k = snprintf(line, sizeof(line), "TWH,%u,%02x:%02x:%02x:%02x:%02x:%02x,%s,%s %s,%d,%lu,%u,%u\n", s_id,
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], FW_VERSION, __DATE__, __TIME__,
                     s_max_payload, (unsigned long)(esp_psram_is_initialized() ? esp_psram_get_size() : 0),
                     s_led_pin, s_led_bright);
    out(line, k);
}

static void work_task(void *arg)
{
    (void)arg;
    static ev_t ev;
    static uint8_t frame[TW_MAX_PAYLOAD];
    static char line[800];               /* the TWF line is near 550 bytes (2.3) */
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t next_tx = now + 50000u + esp_random() % 50000u;
    uint32_t next_status = now + 1000000u;
    uint32_t next_hello = now;
    int len = 0, built = 0;
    for (;;) {
        now = (uint32_t)esp_timer_get_time();
        int quiet;                          /* 3.2: where the radio should be; silent while looking */
        uint8_t want = tw_chan_step(&s_node, now, &quiet);
        if (want != s_radio_ch) {
            radio_channel(want);
        }
        if (s_node.chan_changed) {
            s_node.chan_changed = 0;
            ESP_LOGW(TAG, "now on channel %u (the network's: %u)", s_node.chan_home, s_node.set_channel);
        }
        if (quiet && (int32_t)(next_tx - now) <= (int32_t)BUILD_AHEAD_US) {
            built = 0;
            next_tx = now + 20000u;         /* no frames while on another channel */
        }
        int32_t wait = (int32_t)(next_tx - now);
        if (!built && wait <= (int32_t)BUILD_AHEAD_US) {
            /* 2.3: build the frame ahead, for its slot time (its time stamp and its place in the
               slot are the slot time's), then hand it over at that time: building took the
               first 300-400 us of the slot in 2.2 */
            s_node.is_exit = (uint8_t)usb_up();
            s_stamp1 = wait > 0 ? next_tx : now;
            len = tw_build_frame(&s_node, s_stamp1, frame, s_max_payload);
            tstat_add(&s_t_build, (int32_t)((uint32_t)esp_timer_get_time() - now));
            built = 1;
            continue;
        }
        if (wait <= 0) {
            built = 0;
            s_hand1_us = (uint32_t)esp_timer_get_time();
            if (s_node.synced) {
                tstat_add(&s_t_at, tw_slot_pos(&s_node, s_hand1_us, s_id));
            }
            tstat_add(&s_t_len, len);
            s_len1 = len;
            s_cb_state = 1;
            if (esp_now_send(BCAST, frame, (size_t)len) != ESP_OK) {
                s_send_err++;
                s_cb_state = 0;
                s_tx_state = 0;
            } else {
                s_tx_state = 1;
                if (s_node.extra_planned) {
                    /* 2.3: build the extra frame now, while this one is on the air, cut for a
                       send-done late in the usual spread; its send-done callback sends it */
                    uint32_t pred = s_hand1_us + tw_air_us(len) + (uint32_t)(s_ovh + 2 * s_ovh_dev);
                    int xl = tw_build_extra(&s_node, pred, s_frame2, s_max_payload);
                    if (xl > 0) {
                        int32_t pos = tw_slot_pos(&s_node, pred, s_id);
                        s_x_latest = pred + (uint32_t)((int32_t)(TW_SLOT_US + TW_SLOT_SLACK_US) -
                                                       (int32_t)(s_node.access_us + tw_air_us(xl)) - pos);
                        s_x_len = xl;
                        tstat_add(&s_t_xlen, xl);
                        __atomic_store_n(&s_x_ready, 1, __ATOMIC_RELEASE);
                    }
                }
            }
            next_tx = tw_next_tx_local(&s_node, (uint32_t)esp_timer_get_time());
            continue;
        }
        if (wait < 1500) {              /* close to our slot: wait precisely (to the build, then the slot) */
            esp_rom_delay_us((uint32_t)(built ? wait : wait - (int32_t)BUILD_AHEAD_US));
            continue;
        }
        TickType_t ticks = pdMS_TO_TICKS((wait - 1000) / 1000);
        if (xQueueReceive(s_q, &ev, ticks) == pdTRUE) {
            handle(&ev);
        }
        now = (uint32_t)esp_timer_get_time();
        if ((int32_t)(now - next_status) >= 0) {
            next_status += 1000000u;
            rxk_roll();
            s_node.is_exit = (uint8_t)usb_up();
            tw_update_roles(&s_node, now);
            apply_settings();
            out(line, tw_format_status(&s_node, now, line, sizeof(line)));
            for (int k = 1; k <= TW_MAX_NODES; k++) {
                if (k != s_id && s_node.heard[k] && (uint32_t)(now - s_node.heard_us[k]) < TW_ACTIVE_US) {
                    out(line, tw_format_node(&s_node, &s_last_hdr[k], s_last_rssi[k],
                                             tw_hear_pct(&s_node, now, (uint8_t)k), line, sizeof(line)));
                }
            }
            tw_clear_counts(&s_node);
            /* records that are not measurements (2.2: what firmware this board runs): queued like
               any record; an exit prints its own here, the others' come in their frames */
            for (const tw_rec_t *a; (a = tw_aux_record(&s_node, now)) != NULL;) {
                if (s_node.is_exit) {
                    out(line, tw_format_record(a, line, sizeof(line)));
                }
            }
            led_apply();                        /* 2.5: settings that came by record */
            led_second();                       /* 2.4: the LED, from the scores just closed */
            poll_commands();
            if (s_survey_due && (int32_t)(now - s_survey_at) >= 0) {
                s_survey_due = 0;
                built = 0;
                survey_run(&next_tx);           /* 3.1: about 12 s away from the channel */
                continue;
            }
        }
        if ((int32_t)(now - next_hello) >= 0) {
            next_hello = now + 10000000u;
            hello();
            /* TWF: health. csi = CSI measurements made, frames = ESP-NOW frames received: if csi
               grows and frames stays 0, relaying is not arriving (see FLASH-README.md). rx_err =
               measurements the radio flagged (rx_state != 0), since start. 2.1: extra frames sent,
               skipped (nothing fitted in the time left), failed; the latest end of one since the
               last TWF, us after our slot start by our clock (0: none) */
            int k = snprintf(line, sizeof(line), "TWF,%u,usb_drops=%lu,queue_full=%lu,send_err=%lu,heap=%lu,"
                             "queue=%lu,csi=%lu,frames=%lu,rx_err=%lu,extra_sent=%lu,extra_skipped=%lu,"
                             "extra_err=%lu,extra_end_us=%ld\n", s_id, (unsigned long)s_usb_drops,
                             (unsigned long)s_q_full, (unsigned long)s_send_err,
                             (unsigned long)esp_get_free_heap_size(), (unsigned long)uxQueueMessagesWaiting(s_q),
                             (unsigned long)s_csi_count, (unsigned long)s_node.n_rx_frames, (unsigned long)s_rx_err,
                             (unsigned long)s_node.n_extra_sent, (unsigned long)s_node.n_extra_skipped,
                             (unsigned long)s_extra_err,
                             (long)(s_extra_end_max == INT32_MIN ? 0 : s_extra_end_max));
            s_extra_end_max = INT32_MIN;
            /* 2.2: where the frames' time goes (mean/max, us; lengths in bytes); how busy each core
               was over these 10 s (%; Wi-Fi runs on core 0, our work task on core 1), and the
               lowest free internal memory since start (bytes). 2.3: the normal frame's build time;
               the receive callback after a frame's end (mean/max, us); the radio's receive-time
               offset (us; 0 = not learned); extra frames built ahead but dropped (too late), sent
               from the send-done callback, sent by this task instead (the callback had not got
               it, or ESP-NOW refused it there: x_refused), since start; the hand-over to
               send-done less the air, running mean / spread (us), that extra frames are cut for */
            char b1[24], b2[24], b3[24], b4[24], b5[24], b6[24], b7[24], b8[24];
            int kok;
            int32_t rxk = rxk_get(&kok);
            unsigned busy[2];
            core_load(busy);
            if (k > 0 && k < (int)sizeof(line)) {
                k--;                    /* over the line end */
                k += snprintf(line + k, sizeof(line) - (size_t)k,
                              ",tx_at=%s,tx_radio=%s,x_wake=%s,x_radio=%s,tx_len=%s,x_len=%s,"
                              "core0=%u,core1=%u,heap_min=%lu,psram_free=%lu,build=%s,rxcb=%s,rx_k=%ld,"
                              "extra_late=%lu,x_cb=%lu,x_task=%lu,x_refused=%lu,ovh=%ld/%ld\n",
                              tstat_text(&s_t_at, b1, sizeof(b1)), tstat_text(&s_t_radio, b2, sizeof(b2)),
                              tstat_text(&s_t_wake, b3, sizeof(b3)), tstat_text(&s_t_xradio, b4, sizeof(b4)),
                              tstat_text(&s_t_len, b5, sizeof(b5)), tstat_text(&s_t_xlen, b6, sizeof(b6)),
                              busy[0], busy[1],
                              (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                              tstat_text(&s_t_build, b7, sizeof(b7)), tstat_text(&s_t_rxcb, b8, sizeof(b8)),
                              (long)rxk, (unsigned long)s_node.n_extra_late, (unsigned long)s_x_cb,
                              (unsigned long)s_x_task, (unsigned long)s_x_refused, (long)s_ovh, (long)s_ovh_dev);
            }
            out(line, k);
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    usb_serial_jtag_driver_config_t usb_cfg = {.tx_buffer_size = 32768, .rx_buffer_size = 256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));
    s_id = board_id();
    tw_init(&s_node, s_id, 0, esp_random());
    s_node.access_us = X_ACCESS_US;
    tw_set_info(&s_node, FW_VERSION "|" __DATE__ " " __TIME__);     /* sent to the computer every 5 s */
    load_settings();
    s_q = xQueueCreate(48, sizeof(ev_t));
    wifi_init();
    espnow_init();
    csi_init();
    apply_settings();                   /* the power kept from before a restart, if any */
    net_load();                         /* the settings as network keys (3.0: their only way) */
    tw_chan_init(&s_node, (uint32_t)esp_timer_get_time());     /* 3.2: on its channel */
    s_radio_ch = s_node.chan_home;
    led_load();                         /* 2.4: the LED, if this board has one set */
    led_start();
    ESP_LOGW(TAG, "%s: board %u, address " MACSTR ", channel %u, frames up to %d bytes", FW_VERSION, s_id,
             MAC2STR(s_fake_mac), s_node.chan_home, s_max_payload);
    xTaskCreatePinnedToCore(work_task, "tw_work", 8192, NULL, 5, NULL, 1);
}
