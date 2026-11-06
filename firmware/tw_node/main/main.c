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
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "esp_csi_gain_ctrl.h"

#include "tw_proto.h"

#define FW_VERSION              "tw_node 2.1"
#define TW_CHANNEL              11
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
            uint16_t len;
            float    gain;
            int8_t   csi[CSI_MAX];
        } c;
        struct {
            uint16_t len;
            uint8_t  data[TW_MAX_PAYLOAD];
        } f;
        struct {
            uint8_t  ok;                /* ESP-NOW's send status */
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
static uint32_t s_extra_err;
static int32_t s_extra_end_max = INT32_MIN;     /* latest end of our extra frames since the last TWF */
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
    uint8_t seq = 0, pwr = 0, flg = 0;
    if (nvs_open("tw", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "pseq", &seq);
        nvs_get_u8(h, "pwr", &pwr);
        nvs_get_u8(h, "rflg", &flg);
        nvs_close(h);
    }
    if (pwr >= TW_POWER_MIN_Q && pwr <= TW_POWER_MAX_Q) {
        s_node.set_seq = seq;
        s_node.set_power = pwr;
        s_node.set_flags = flg;
        s_node.set_pending = 1;
    }
}

/* apply a new setting to the radio and keep it (called once a second) */
static void apply_settings(void)
{
    if (s_node.set_pending) {
        s_node.set_pending = 0;
        if (s_node.set_power) {
            if (esp_wifi_set_max_tx_power((int8_t)s_node.set_power) != ESP_OK) {
                ESP_LOGW(TAG, "could not set transmit power %u/4 dBm", s_node.set_power);
            }
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
    ESP_ERROR_CHECK(esp_wifi_set_channel(TW_CHANNEL, WIFI_SECOND_CHAN_BELOW));
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
    ev.kind = EV_SENT;
    ev.local_us = (uint32_t)esp_timer_get_time();
    ev.s.ok = status == ESP_NOW_SEND_SUCCESS;
    if (xQueueSendToFront(s_q, &ev, 0) != pdTRUE) {
        s_q_full++;
    }
}

static void espnow_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)"pmk1234567890123"));
    esp_now_peer_info_t peer = {
        .channel = TW_CHANNEL,
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

static const uint8_t BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* ESP-NOW says a frame of ours has gone out: after the normal frame, the extra one if planned */
static void sent(const ev_t *ev)
{
    if (s_tx_state == 1) {
        s_tx_state = 0;
        int len = tw_build_extra(&s_node, (uint32_t)esp_timer_get_time(), s_frame2, s_max_payload);
        if (len > 0) {
            if (esp_now_send(BCAST, s_frame2, (size_t)len) == ESP_OK) {
                s_tx_state = 2;
            } else {
                s_extra_err++;
            }
        }
    } else if (s_tx_state == 2) {
        s_tx_state = 0;
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
        tw_note_frame(&s_node, &h, ev->local_us);
        if (h.src >= 1 && h.src <= TW_MAX_NODES && !(h.flags & TW_F_EXTRA)) {
            s_last_hdr[h.src] = h;
        }
        rctx_t c = {h.src, ev->local_us};
        if (tw_parse_frame(ev->f.data, ev->f.len, &h, relay_cb, &c) < 0) {
            s_node.n_bad_frames++;
        }
        return;
    }
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
    char line[160];
    int k = snprintf(line, sizeof(line), "TWH,%u,%02x:%02x:%02x:%02x:%02x:%02x,%s,%s %s,%d\n", s_id,
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], FW_VERSION, __DATE__, __TIME__,
                     s_max_payload);
    out(line, k);
}

static void work_task(void *arg)
{
    (void)arg;
    static ev_t ev;
    static uint8_t frame[TW_MAX_PAYLOAD];
    static char line[300];
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t next_tx = now + 50000u + esp_random() % 50000u;
    uint32_t next_status = now + 1000000u;
    uint32_t next_hello = now;
    for (;;) {
        now = (uint32_t)esp_timer_get_time();
        int32_t wait = (int32_t)(next_tx - now);
        if (wait <= 0) {
            s_node.is_exit = (uint8_t)usb_up();
            int len = tw_build_frame(&s_node, now, frame, s_max_payload);
            if (esp_now_send(BCAST, frame, (size_t)len) != ESP_OK) {
                s_send_err++;
                s_tx_state = 0;
            } else {
                s_tx_state = 1;         /* its send-done brings the extra frame, if one is planned */
            }
            next_tx = tw_next_tx_local(&s_node, (uint32_t)esp_timer_get_time());
            continue;
        }
        if (wait < 1500) {              /* close to our slot: wait precisely */
            esp_rom_delay_us((uint32_t)wait);
            continue;
        }
        TickType_t ticks = pdMS_TO_TICKS((wait - 1000) / 1000);
        if (xQueueReceive(s_q, &ev, ticks) == pdTRUE) {
            handle(&ev);
        }
        now = (uint32_t)esp_timer_get_time();
        if ((int32_t)(now - next_status) >= 0) {
            next_status += 1000000u;
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
            poll_commands();
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
    load_settings();
    s_q = xQueueCreate(48, sizeof(ev_t));
    wifi_init();
    espnow_init();
    csi_init();
    apply_settings();                   /* the power kept from before a restart, if any */
    ESP_LOGW(TAG, "%s: board %u, address " MACSTR ", channel %d, frames up to %d bytes", FW_VERSION, s_id,
             MAC2STR(s_fake_mac), TW_CHANNEL, s_max_payload);
    xTaskCreatePinnedToCore(work_task, "tw_work", 8192, NULL, 5, NULL, 1);
}
