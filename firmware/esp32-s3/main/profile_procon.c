// Nintendo Switch Pro Controller profile (wired USB).
//
// The Switch 2 does not take the HORI pad class. It does take a wired Pro
// Controller once "Pro Controller Wired Communication" is on, but that needs a
// handshake: the console sends 0x80 USB commands and 0x01 subcommands (device
// info, SPI flash reads for colours and stick calibration, IMU/vibration
// enables...) and waits for each reply before it polls standard 0x30 reports.
//
// Every report is 64 bytes and starts with its report ID:
//   0x30 standard input   id | timer | battery | buttons[3] | lstick[3] | rstick[3] | vib | imu[36]
//   0x21 subcommand reply id | standard input[12] | ack | subcmd | data[...]
//   0x81 USB command reply
// Protocol reference: dekuNukem/Nintendo_Switch_Reverse_Engineering.

#include <string.h>

#include "console_profile.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host_link.h"

#define PRO_VID 0x057E
#define PRO_PID 0x2009
#define REPORT_LEN 64

static const uint8_t pro_report_desc[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01,             // Generic Desktop, Joystick, Collection
    0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0A, // Report 0x30: buttons 1-10
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0A, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0B, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, // buttons 11-14
    0x75, 0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x01, 0x95, 0x02, 0x81, 0x03,
    0x0B, 0x01, 0x00, 0x01, 0x00, 0xA1, 0x00,                   // Pointer, Physical
    0x0B, 0x30, 0x00, 0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00, // X, Y
    0x0B, 0x32, 0x00, 0x01, 0x00, 0x0B, 0x35, 0x00, 0x01, 0x00, // Z, Rz
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xC0,
    0x0B, 0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, // hat
    0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0F, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, // buttons 15-18
    0x75, 0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x34, 0x81, 0x03,
    0x06, 0x00, 0xFF,                                           // Vendor page
    0x85, 0x21, 0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, // input 0x21
    0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, // input 0x81
    0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, // output 0x01
    0x85, 0x10, 0x09, 0x04, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, // output 0x10
    0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, // output 0x80
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, // output 0x82
    0xC0,
};

static const tusb_desc_device_t pro_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = PRO_VID,
    .idProduct = PRO_PID,
    .bcdDevice = 0x0200,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

#define EPNUM_HID_OUT 0x01
#define EPNUM_HID_IN 0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t pro_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
    // A real Pro Controller asks for 8 ms; 1 ms keeps the bridge's latency budget.
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(pro_report_desc), EPNUM_HID_OUT, EPNUM_HID_IN, 64, 1),
};

static const char *pro_strings[] = {
    (const char[]){0x09, 0x04}, // 0: English (US)
    "Nintendo Co., Ltd.",       // 1: Manufacturer
    "Pro Controller",           // 2: Product
    "000000000001",             // 3: Serial
};

static const uint8_t kMac[6] = {0x98, 0xB6, 0xE9, 0x48, 0x1E, 0x61};  // big endian

static QueueHandle_t s_replies;   // 64-byte reports waiting for the IN endpoint
static volatile bool s_streaming; // console sent USB command 0x04
static uint8_t s_timer;

/* ---- virtual SPI flash ------------------------------------------------------ */

// 12-bit X/Y pair as stored in the calibration blocks.
#define PACK12(x, y) (uint8_t)((x) & 0xFF), (uint8_t)((((x) >> 8) & 0x0F) | (((y) & 0x0F) << 4)), (uint8_t)((y) >> 4)
#define STICK_CENTER 0x800
#define STICK_RANGE 0x600

static const uint8_t spi_imu_cal[] = {  // 0x6020: accel origin/sensitivity, gyro origin/sensitivity
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34,
};
static const uint8_t spi_stick_cal[] = {  // 0x603D: left max/center/min, right center/min/max
    PACK12(STICK_RANGE, STICK_RANGE), PACK12(STICK_CENTER, STICK_CENTER), PACK12(STICK_RANGE, STICK_RANGE),
    PACK12(STICK_CENTER, STICK_CENTER), PACK12(STICK_RANGE, STICK_RANGE), PACK12(STICK_RANGE, STICK_RANGE),
};
static const uint8_t spi_colors[] = {  // 0x6050: body, buttons, left grip, right grip
    0x32, 0x32, 0x32, 0xFF, 0xFF, 0xFF, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
};
static const uint8_t spi_stick_params[] = {  // 0x6080 (sensor + left stick) and 0x6098 (right stick)
    0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F, 0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3,
    0xD4, 0x14, 0x54, 0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
};

static const struct {
    uint32_t addr;
    const uint8_t *data;
    uint8_t len;
} kSpi[] = {
    {0x6020, spi_imu_cal, sizeof(spi_imu_cal)},
    {0x603D, spi_stick_cal, sizeof(spi_stick_cal)},
    {0x6050, spi_colors, sizeof(spi_colors)},
    {0x6080, spi_stick_params, sizeof(spi_stick_params)},
    {0x6098, spi_stick_params + 6, sizeof(spi_stick_params) - 6},
};

// Unwritten flash reads as 0xFF, which is also what "no user calibration" looks like.
static void spi_read(uint32_t addr, uint8_t *out, uint8_t len)
{
    memset(out, 0xFF, len);
    for (size_t r = 0; r < sizeof(kSpi) / sizeof(kSpi[0]); ++r)
        for (uint8_t i = 0; i < len; ++i)
            if (addr + i >= kSpi[r].addr && addr + i < kSpi[r].addr + kSpi[r].len) out[i] = kSpi[r].data[addr + i - kSpi[r].addr];
}

/* ---- reports ----------------------------------------------------------------- */

static uint16_t axis12(uint16_t v, bool invert)
{
    int d = ((int)v - 32768) * (STICK_RANGE - 1) / 32768;
    if (invert) d = -d;  // link is +Y down, the Pro Controller is +Y up
    const int out = STICK_CENTER + d;
    return (uint16_t)(out < 0 ? 0 : out > 0xFFF ? 0xFFF : out);
}

// Fills report bytes 1..12 (timer through vibrator byte).
static void fill_standard(const hk_pad_state *s, uint32_t extra, uint8_t *r)
{
    const uint32_t b = s->buttons | extra;
    r[1] = s_timer++;
    r[2] = 0x91;  // battery full + charging, Pro Controller on USB power
    r[3] = (b & HK_BTN_Y ? 0x01 : 0) | (b & HK_BTN_X ? 0x02 : 0) | (b & HK_BTN_B ? 0x04 : 0) |
           (b & HK_BTN_A ? 0x08 : 0) | (b & HK_BTN_R ? 0x40 : 0) | (b & HK_BTN_ZR ? 0x80 : 0);
    r[4] = (b & HK_BTN_MINUS ? 0x01 : 0) | (b & HK_BTN_PLUS ? 0x02 : 0) | (b & HK_BTN_RSTICK ? 0x04 : 0) |
           (b & HK_BTN_LSTICK ? 0x08 : 0) | (b & HK_BTN_HOME ? 0x10 : 0) | (b & HK_BTN_CAPTURE ? 0x20 : 0);
    r[5] = (b & HK_BTN_DPAD_DOWN ? 0x01 : 0) | (b & HK_BTN_DPAD_UP ? 0x02 : 0) | (b & HK_BTN_DPAD_RIGHT ? 0x04 : 0) |
           (b & HK_BTN_DPAD_LEFT ? 0x08 : 0) | (b & HK_BTN_L ? 0x40 : 0) | (b & HK_BTN_ZL ? 0x80 : 0);
    const uint16_t lx = axis12(s->lx, false), ly = axis12(s->ly, true);
    const uint16_t rx = axis12(s->rx, false), ry = axis12(s->ry, true);
    r[6] = (uint8_t)lx;
    r[7] = (uint8_t)((lx >> 8) | ((ly & 0x0F) << 4));
    r[8] = (uint8_t)(ly >> 4);
    r[9] = (uint8_t)rx;
    r[10] = (uint8_t)((rx >> 8) | ((ry & 0x0F) << 4));
    r[11] = (uint8_t)(ry >> 4);
    r[12] = 0x00;
}

static void pro_build(const hk_pad_state *s, uint32_t extra, uint8_t *out)
{
    memset(out, 0, REPORT_LEN);
    out[0] = 0x30;
    fill_standard(s, extra, out);
}

/* ---- handshake ------------------------------------------------------------------ */

// Replies are queued without the live input bytes; take_reply fills those in
// at send time so a reply never carries a stale button state.
static void queue(const uint8_t *report)
{
    if (s_replies) xQueueSend(s_replies, report, 0);
}

static void usb_command(uint8_t cmd)
{
    uint8_t r[REPORT_LEN] = {0x81, cmd};
    switch (cmd) {
    case 0x01:  // status: controller type and MAC (little endian)
        r[3] = 0x03;
        for (int i = 0; i < 6; ++i) r[4 + i] = kMac[5 - i];
        queue(r);
        break;
    case 0x02:  // handshake
    case 0x03:  // switch to 3 Mbit "baud rate"
        queue(r);
        break;
    case 0x04:  // stream input reports with no timeout
        s_streaming = true;
        break;
    case 0x05:  // back to handshake mode
        s_streaming = false;
        break;
    default:
        break;
    }
}

static void subcommand(const uint8_t *buf, uint16_t len)
{
    if (len < 11) return;
    const uint8_t cmd = buf[10];
    uint8_t r[REPORT_LEN] = {0x21};
    uint8_t *data = r + 15;
    r[13] = 0x80;  // plain ACK
    r[14] = cmd;
    switch (cmd) {
    case 0x01:  // Bluetooth manual pairing
        r[13] = 0x81;
        data[0] = 0x03;
        break;
    case 0x02:  // device info: firmware 3.72, Pro Controller, MAC, use SPI colours
        r[13] = 0x82;
        data[0] = 0x03;
        data[1] = 0x48;
        data[2] = 0x03;
        data[3] = 0x02;
        memcpy(data + 4, kMac, 6);
        data[10] = 0x01;
        data[11] = 0x01;
        break;
    case 0x04:  // trigger buttons elapsed time
        r[13] = 0x83;
        break;
    case 0x10: {  // SPI flash read: addr u32le, len u8; echo both, then the bytes
        if (len < 16) return;
        uint8_t n = buf[15];
        if (n > 0x1D) n = 0x1D;
        r[13] = 0x90;
        memcpy(data, buf + 11, 5);
        data[4] = n;
        spi_read((uint32_t)buf[11] | ((uint32_t)buf[12] << 8) | ((uint32_t)buf[13] << 16) | ((uint32_t)buf[14] << 24),
                 data + 5, n);
        break;
    }
    case 0x21: {  // NFC/IR MCU configuration
        static const uint8_t mcu[] = {0x01, 0x00, 0xFF, 0x00, 0x08, 0x00, 0x1B, 0x01};
        r[13] = 0xA0;
        memcpy(data, mcu, sizeof(mcu));
        data[33] = 0xC8;
        break;
    }
    default:  // input mode, player lights, IMU, vibration, HCI state...: ACK
        break;
    }
    queue(r);
}

static void pro_output(uint8_t report_id, const uint8_t *buf, uint16_t len)
{
    uint8_t tmp[REPORT_LEN];
    if (report_id) {  // control transfer: the stack stripped the ID byte
        if (len > REPORT_LEN - 1) len = REPORT_LEN - 1;
        tmp[0] = report_id;
        memcpy(tmp + 1, buf, len);
        buf = tmp;
        len++;
    }
    if (len < 2) return;
    // Trace the handshake for the host log (rumble-only 0x10 is far too chatty).
    if (buf[0] == 0x80 || buf[0] == 0x01)
        host_link_send_event(HK_EVENT_CONSOLE_CMD, ((uint32_t)buf[0] << 8) | (buf[0] == 0x80 ? buf[1] : len > 10 ? buf[10] : 0xFF));
    if (buf[0] == 0x80) usb_command(buf[1]);
    else if (buf[0] == 0x01) subcommand(buf, len);
    // 0x10 (rumble only) needs no answer.
}

static void pro_reset(void)
{
    if (!s_replies) s_replies = xQueueCreate(8, REPORT_LEN);
    xQueueReset(s_replies);
    s_streaming = false;
}

static bool pro_take_reply(const hk_pad_state *s, uint32_t extra, uint8_t *out)
{
    if (!s_replies || xQueueReceive(s_replies, out, 0) != pdTRUE) return false;
    if (out[0] == 0x21) fill_standard(s, extra, out);
    return true;
}

static bool pro_streaming(void) { return s_streaming; }

const console_profile g_profile_procon = {
    .name = "Pro Controller",
    .id = HK_PROFILE_PRO_CONTROLLER,
    .device_descriptor = &pro_device,
    .configuration_descriptor = pro_config,
    .hid_report_descriptor = pro_report_desc,
    .hid_report_descriptor_len = sizeof(pro_report_desc),
    .strings = pro_strings,
    .string_count = sizeof(pro_strings) / sizeof(pro_strings[0]),
    .report_len = REPORT_LEN,
    .build_report = pro_build,
    .on_output = pro_output,
    .reset = pro_reset,
    .take_reply = pro_take_reply,
    .streaming = pro_streaming,
};
