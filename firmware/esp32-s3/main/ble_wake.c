// Switch 2 wake beacon (see ble_wake.h).
//
// Beacon layout, from community research (thiagobucca/nintendo-switch2-esp32-waker,
// docs/packet-format.md): manufacturer data after the AD header is
//   53 05 | 01 00 03 | 7E 05 | pid:u16le | 00 01 state | console addr (LSB first) | 0F | zeros
// with state 0x81 for "reconnect/wake" (0x00 is a pairing beacon, which does
// not wake). The console accepts it only from the address of a controller it
// has bonded with, so the bridge replays it from that exact address. The
// captured manufacturer data is replayed byte for byte.

#include "ble_wake.h"

#include <string.h>

#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hk_link.h"
#include "host/ble_hs.h"
#include "host_link.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"

#define NVS_KEY "ble_beacon"
#define MFG_MAX 27  // legacy advertising: 31 bytes minus flags (3) and the AD header (2)

// Offsets inside the manufacturer data (company ID first).
#define OFF_VID 5
#define OFF_PID 7
#define OFF_STATE 11
#define STATE_WAKE 0x81

typedef struct {
    uint8_t addr_type;  // BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM
    uint8_t addr[6];    // controller address, LSB first (NimBLE order)
    uint8_t mfg_len;
    uint8_t mfg[MFG_MAX];
} beacon_t;

static beacon_t s_beacon;   // learned beacon (s_have)
static bool s_have;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_stack_addr[6]; // public address the controller was started with (LSB first)
static volatile bool s_synced;
static SemaphoreHandle_t s_adv_done;

static void start_scan(void);

/* ---- learning ------------------------------------------------------------------ */

static void save_beacon(const beacon_t *b)
{
    nvs_handle_t h;
    if (nvs_open("hikari", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, b, sizeof(*b)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static bool load_beacon(beacon_t *b)
{
    nvs_handle_t h;
    size_t len = sizeof(*b);
    if (nvs_open("hikari", NVS_READONLY, &h) != ESP_OK) return false;
    const bool ok = nvs_get_blob(h, NVS_KEY, b, &len) == ESP_OK && len == sizeof(*b) && b->mfg_len <= MFG_MAX;
    nvs_close(h);
    return ok;
}

// Finds a Switch 2 wake beacon in an advertisement; returns its manufacturer data.
static const uint8_t *find_wake_mfg(const uint8_t *data, uint8_t len, uint8_t *mfg_len)
{
    for (uint8_t i = 0; i + 1 < len;) {
        const uint8_t n = data[i];
        if (n == 0 || i + 1 + n > len) return NULL;
        const uint8_t *f = data + i + 1;  // f[0] = AD type
        if (f[0] == 0xFF && n - 1 > OFF_STATE && n - 1 <= MFG_MAX) {
            const uint8_t *m = f + 1;
            if (m[0] == 0x53 && m[1] == 0x05 && m[OFF_VID] == 0x7E && m[OFF_VID + 1] == 0x05 && m[OFF_STATE] == STATE_WAKE) {
                *mfg_len = n - 1;
                return m;
            }
        }
        i += 1 + n;
    }
    return NULL;
}

static void on_advertisement(const struct ble_gap_disc_desc *d)
{
    uint8_t mfg_len;
    const uint8_t *mfg = find_wake_mfg(d->data, d->length_data, &mfg_len);
    if (!mfg) return;

    beacon_t b = {.addr_type = d->addr.type, .mfg_len = mfg_len};
    memcpy(b.addr, d->addr.val, 6);
    memcpy(b.mfg, mfg, mfg_len);

    portENTER_CRITICAL(&s_lock);
    const bool same = s_have && memcmp(&b, &s_beacon, sizeof(b)) == 0;
    if (!same) {
        s_beacon = b;
        s_have = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (same) return;

    save_beacon(&b);
    host_link_send_event(HK_EVENT_BLE_LEARNED, (uint32_t)mfg[OFF_PID] | ((uint32_t)mfg[OFF_PID + 1] << 8));
}

static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_DISC:
        on_advertisement(&e->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan();
        break;
    case BLE_GAP_EVENT_CONNECT:  // the console may connect to a connectable beacon; we only wanted it awake
        if (e->connect.status == 0) ble_gap_terminate(e->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreGive(s_adv_done);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        xSemaphoreGive(s_adv_done);
        break;
    default:
        break;
    }
    return 0;
}

static void start_scan(void)
{
    if (!s_synced || ble_gap_disc_active() || ble_gap_adv_active()) return;
    // Passive and ~30% duty: a Joy-Con 2 beacon repeats every ~20 ms for a
    // second or more, so it is always caught.
    const struct ble_gap_disc_params p = {.itvl = 160, .window = 48, .passive = 1, .filter_duplicates = 0};
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, gap_event, NULL);
}

/* ---- stack ------------------------------------------------------------------------ */

static void on_sync(void)
{
    s_synced = true;
    start_scan();
}

static void on_reset(int reason) { s_synced = false; }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// The console only accepts the beacon from the controller's own address, so
// the Bluetooth MAC is set to it before the controller starts.
static void stack_start(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool have_public = s_have && s_beacon.addr_type == BLE_ADDR_PUBLIC;
    uint8_t addr[6];
    memcpy(addr, s_beacon.addr, 6);
    portEXIT_CRITICAL(&s_lock);
    if (have_public) {
        uint8_t mac[6];
        for (int i = 0; i < 6; ++i) mac[i] = addr[5 - i];  // esp_mac order is MSB first
        esp_iface_mac_addr_set(mac, ESP_MAC_BT);
        memcpy(s_stack_addr, addr, 6);
    }
    s_synced = false;
    if (nimble_port_init() != ESP_OK) return;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}

void ble_wake_init(void)
{
    s_adv_done = xSemaphoreCreateBinary();
    s_have = load_beacon(&s_beacon);
    stack_start();
}

bool ble_wake_ready(void) { return s_have; }

uint8_t ble_wake_send(uint32_t duration_ms)
{
    portENTER_CRITICAL(&s_lock);
    const bool have = s_have;
    const beacon_t b = s_beacon;
    portEXIT_CRITICAL(&s_lock);
    if (!have) return HK_WAKE_RESULT_UNSUPPORTED;

    // Learned since boot from a new controller: restart the stack on its address.
    if (b.addr_type == BLE_ADDR_PUBLIC && memcmp(s_stack_addr, b.addr, 6) != 0) {
        nimble_port_stop();
        nimble_port_deinit();
        stack_start();
    }
    for (int i = 0; i < 200 && !s_synced; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (!s_synced) return HK_WAKE_RESULT_UNSUPPORTED;

    uint8_t own = BLE_OWN_ADDR_PUBLIC;
    if (b.addr_type == BLE_ADDR_RANDOM) {
        if (ble_hs_id_set_rnd(b.addr) != 0) return HK_WAKE_RESULT_UNSUPPORTED;
        own = BLE_OWN_ADDR_RANDOM;
    }

    uint8_t adv[31] = {0x02, 0x01, 0x06, (uint8_t)(b.mfg_len + 1), 0xFF};
    memcpy(adv + 5, b.mfg, b.mfg_len);

    ble_gap_disc_cancel();
    xSemaphoreTake(s_adv_done, 0);
    // Connectable undirected, 20 ms, all three channels: the tested configuration.
    const struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN, .itvl_min = 32, .itvl_max = 32};
    uint8_t result = HK_WAKE_RESULT_UNSUPPORTED;
    if (ble_gap_adv_set_data(adv, 5 + b.mfg_len) == 0 && ble_gap_adv_start(own, NULL, duration_ms, &p, gap_event, NULL) == 0) {
        xSemaphoreTake(s_adv_done, pdMS_TO_TICKS(duration_ms + 500));
        ble_gap_adv_stop();
        result = HK_WAKE_RESULT_SENT;
    }
    start_scan();
    return result;
}
