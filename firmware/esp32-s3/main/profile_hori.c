// HORI-style wired pad profile.
//
// Nintendo consoles accept this licensed wired-pad class over USB without the
// Pro Controller's proprietary handshake, which is what makes it a good
// low-latency first profile: the console just polls an 8-byte HID report.
//
// Report (8 bytes):
//   u16 buttons  Y B A X L R ZL ZR - + LS RS HOME CAPTURE . .
//   u8  hat      0..7 clockwise from up, 8 = centred
//   u8  lx ly rx ry   0..255, 128 centre, +Y down
//   u8  vendor   0

#include <string.h>

#include "console_profile.h"

#define HORI_VID 0x0F0D
#define HORI_PID 0x0092

static const uint8_t hori_report_desc[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x35, 0x00,       //   Physical Minimum (0)
    0x45, 0x01,       //   Physical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x10,       //   Report Count (16)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x10,       //   Usage Maximum (16)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x05, 0x01,       //   Usage Page (Generic Desktop)
    0x25, 0x07,       //   Logical Maximum (7)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x65, 0x14,       //   Unit (Eng Rot: Degrees)
    0x09, 0x39,       //   Usage (Hat switch)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null)
    0x65, 0x00,       //   Unit (None)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x01,       //   Input (Const) - hat padding
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x46, 0xFF, 0x00, //   Physical Maximum (255)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x09, 0x32,       //   Usage (Z)
    0x09, 0x35,       //   Usage (Rz)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x04,       //   Report Count (4)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined)
    0x09, 0x20,       //   Usage (0x20)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x0A, 0x21, 0x26, //   Usage (0x2621)
    0x95, 0x08,       //   Report Count (8)
    0x91, 0x02,       //   Output (Data,Var,Abs)
    0xC0,             // End Collection
};

static const tusb_desc_device_t hori_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = HORI_VID,
    .idProduct = HORI_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x00,
    .bNumConfigurations = 0x01,
};

#define EPNUM_HID_OUT 0x02
#define EPNUM_HID_IN 0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t hori_config[] = {
    // Remote wakeup is advertised so the console *may* let us wake it from
    // sleep; whether it actually arms the feature is reported to the host.
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
    // 1 ms polling interval: the lowest the full-speed bus allows.
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(hori_report_desc), EPNUM_HID_OUT, EPNUM_HID_IN,
                             64, 1),
};

static const char *hori_strings[] = {
    (const char[]){0x09, 0x04}, // 0: English (US)
    "HORI CO.,LTD.",            // 1: Manufacturer
    "Hikari Wired Pad",         // 2: Product
};

static inline uint8_t axis8(uint16_t v) { return (uint8_t)(v >> 8); }

static void hori_build(const hk_pad_state *s, uint32_t extra, uint8_t *out)
{
    const uint32_t b = s->buttons | extra;
    const uint16_t face = (uint16_t)(b & HK_BTN_FACE_MASK);
    out[0] = (uint8_t)face;
    out[1] = (uint8_t)(face >> 8);
    out[2] = hk_dpad_to_hat(b);
    out[3] = axis8(s->lx);
    out[4] = axis8(s->ly);
    out[5] = axis8(s->rx);
    out[6] = axis8(s->ry);
    out[7] = 0;
}

static void hori_output(uint8_t report_id, const uint8_t *buf, uint16_t len)
{
    (void)report_id;
    (void)buf;
    (void)len; /* this class has no rumble/LED outputs worth forwarding */
}

const console_profile g_profile_hori = {
    .name = "HORI wired pad",
    .id = HK_PROFILE_HORI_PAD,
    .device_descriptor = &hori_device,
    .configuration_descriptor = hori_config,
    .hid_report_descriptor = hori_report_desc,
    .hid_report_descriptor_len = sizeof(hori_report_desc),
    .strings = hori_strings,
    .string_count = sizeof(hori_strings) / sizeof(hori_strings[0]),
    .report_len = 8,
    .build_report = hori_build,
    .on_output = hori_output,
};
