"""Tests the bridge's Pro Controller profile with this PC standing in for the console.

    python -m pip install hidapi pyserial
    python scripts/test_procon.py [--port COMx] [--no-uart] [--verbose]

Plug the board's USB port into this PC instead of the dock and keep its UART
port connected too. Close the Hikari Host (it holds the COM port) and Steam
(it grabs Pro Controllers) first.

Over UART the script asks the bridge for the Pro Controller profile, which
restarts it if it was still the HORI pad. Over USB it then runs the wired
handshake the console runs (0x80 USB commands, 0x01 subcommands, SPI flash
reads), checks every reply, checks the 0x30 input stream and finally drives
buttons and sticks over UART to check they land on the right bits.

Exit codes: 0 all passed, 1 a check failed, 2 bridge or controller not found.
"""

import argparse
import random
import struct
import sys
import threading
import time

try:
    import hid
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("Missing packages: python -m pip install hidapi pyserial")

PRO_VID, PRO_PID = 0x057E, 0x2009
HORI_VID, HORI_PID = 0x0F0D, 0x0092
PROFILE_HORI, PROFILE_PRO = 0, 1
LINK_BAUD = 2_000_000
AXIS_CENTER = 32768
RUMBLE_NEUTRAL = bytes([0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40])

# Link button bits (shared/protocol/hk_link.h) and where the Pro Controller puts them (report byte, mask).
BUTTONS = {
    "Y": (1 << 0, 3, 0x01), "X": (1 << 3, 3, 0x02), "B": (1 << 1, 3, 0x04), "A": (1 << 2, 3, 0x08),
    "R": (1 << 5, 3, 0x40), "ZR": (1 << 7, 3, 0x80),
    "MINUS": (1 << 8, 4, 0x01), "PLUS": (1 << 9, 4, 0x02), "RS": (1 << 11, 4, 0x04), "LS": (1 << 10, 4, 0x08),
    "HOME": (1 << 12, 4, 0x10), "CAPTURE": (1 << 13, 4, 0x20),
    "DOWN": (1 << 17, 5, 0x01), "UP": (1 << 16, 5, 0x02), "RIGHT": (1 << 19, 5, 0x04), "LEFT": (1 << 18, 5, 0x08),
    "L": (1 << 4, 5, 0x40), "ZL": (1 << 6, 5, 0x80),
}

VERBOSE = False
results = []  # (ok, name, detail)


def check(name, ok, detail="", why=""):
    """detail is always shown; why only when the check fails."""
    detail = detail or ("" if ok else why)
    results.append((ok, name, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))
    return ok


def info(msg):
    print(f"  ....  {msg}")


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


# ---- host link (UART) ---------------------------------------------------------


def crc16(data):  # CRC-16/CCITT-FALSE, as hk_crc16
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def cobs_encode(data):
    out, block = bytearray(), bytearray()
    for byte in data:
        if byte == 0:
            out += bytes([len(block) + 1]) + block
            block = bytearray()
        else:
            block.append(byte)
            if len(block) == 254:
                out += b"\xff" + block
                block = bytearray()
    return bytes(out + bytes([len(block) + 1]) + block)


def cobs_decode(data):
    out, i = bytearray(), 0
    while i < len(data):
        code = data[i]
        if code == 0 or i + code > len(data):
            return None
        out += data[i + 1 : i + code]
        i += code
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


class Link:
    def __init__(self, port):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = LINK_BAUD
        self.ser.timeout = 0.02
        # DTR/RTS drive EN/IO0 on ESP32 dev boards: keep them released so opening doesn't reset it.
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.rx = bytearray()
        self.session = 0
        self.seq = 0
        self.state = (0, AXIS_CENTER, AXIS_CENTER, AXIS_CENTER, AXIS_CENTER)
        self.lock = threading.Lock()
        self.running = False
        self.thread = None

    def send(self, frame_type, payload):
        raw = bytes([frame_type]) + payload
        raw += struct.pack("<H", crc16(raw))
        self.ser.write(cobs_encode(raw) + b"\x00")

    def frames(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.rx += self.ser.read(256)
            while b"\x00" in self.rx:
                chunk, _, rest = self.rx.partition(b"\x00")
                self.rx = bytearray(rest)
                raw = cobs_decode(bytes(chunk)) if chunk else None
                if raw and len(raw) >= 3 and struct.unpack("<H", raw[-2:])[0] == crc16(raw[:-2]):
                    yield raw[0], raw[1:-2]

    def hello(self, profile, timeout=1.0):
        self.session = random.getrandbits(32) | 1
        self.seq = 0
        self.send(0x02, struct.pack("<BIHB", 1, self.session, 250, profile))
        for frame_type, p in self.frames(timeout):
            if frame_type == 0x82 and len(p) == 11:
                ack = dict(zip(("link", "profile", "fw", "session", "usb", "wake", "rwake"), struct.unpack("<BBHIBBB", p)))
                if ack["session"] == self.session:
                    return ack
        return None

    def set_pad(self, buttons=0, lx=AXIS_CENTER, ly=AXIS_CENTER, rx=AXIS_CENTER, ry=AXIS_CENTER):
        with self.lock:
            self.state = (buttons, lx, ly, rx, ry)

    def _pump(self):
        while self.running:
            with self.lock:
                buttons, lx, ly, rx, ry = self.state
            self.seq += 1
            t = int(time.monotonic() * 1e6) & 0xFFFFFFFF
            self.send(0x01, struct.pack("<BBIIIIHHHHBB", 1, 0, self.session, self.seq, t, buttons, lx, ly, rx, ry, 0, 0))
            self.ser.reset_input_buffer()  # telemetry is not needed here
            time.sleep(0.008)

    def start_pump(self):
        self.running = True
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def close(self):
        if self.thread:
            self.set_pad()
            time.sleep(0.05)
            self.running = False
            self.thread.join()
        self.ser.close()


def find_bridge(port):
    if port:
        candidates = [port]
    else:
        known = {0x10C4, 0x1A86, 0x0403, 0x303A}  # CP210x, CH34x, FTDI, Espressif
        ports = serial.tools.list_ports.comports()
        candidates = [p.device for p in ports if p.vid in known] or [p.device for p in ports]
    for name in candidates:
        try:
            link = Link(name)
        except serial.SerialException as e:
            info(f"{name}: cannot open ({e}); is the Hikari Host running?")
            continue
        ack = link.hello(PROFILE_PRO)
        if ack:
            return link, ack
        link.close()
    return None, None


def usb_state_name(s):
    return {0: "detached", 1: "suspended", 2: "active"}.get(s, f"unknown {s}")


def switch_to_pro(link, ack):
    """Returns the HELLO_ACK of a bridge presenting the Pro Controller, or None."""
    print(f"Bridge on {link.ser.port}: fw {ack['fw'] >> 8}.{ack['fw'] & 0xFF}, "
          f"profile {'Pro Controller' if ack['profile'] == PROFILE_PRO else 'HORI pad'}, console USB {usb_state_name(ack['usb'])}")
    if ack["profile"] == PROFILE_PRO:
        return ack
    if ack["fw"] < 0x0101:
        check("bridge firmware supports the Pro Controller", False, "fw 1.0 is HORI only; flash the latest build")
        return None
    info("bridge is restarting as a Pro Controller")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        time.sleep(0.5)
        ack = link.hello(PROFILE_PRO, timeout=0.5)
        if ack and ack["profile"] == PROFILE_PRO:
            info(f"bridge back as Pro Controller, console USB {usb_state_name(ack['usb'])}")
            return ack
    check("bridge switched to the Pro Controller", False, "still not a Pro Controller after 10 s")
    return None


# ---- controller (USB HID) ----------------------------------------------------


def find_controller(timeout):
    deadline = time.monotonic() + timeout
    while True:
        devs = hid.enumerate(PRO_VID, PRO_PID)
        if devs:
            return devs[0]
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.25)


class Pad:
    def __init__(self, path):
        self.dev = hid.device()
        self.dev.open_path(path)
        self.counter = 0
        self.streamed = 0  # 0x30 reports seen while waiting for replies

    def write(self, report_id, data=b""):
        buf = bytes([report_id]) + bytes(data)
        buf += bytes(64 - len(buf))
        if VERBOSE:
            print(f"        -> {hexs(buf[:16])}")
        self.dev.write(buf)

    def read(self, timeout_ms):
        r = self.dev.read(64, timeout_ms)
        if r and VERBOSE and r[0] != 0x30:
            print(f"        <- {hexs(r[:24])}")
        return bytes(r)

    def wait(self, pred, timeout=0.5):
        deadline = time.monotonic() + timeout
        while (left := deadline - time.monotonic()) > 0:
            r = self.read(max(1, int(left * 1000)))
            if not r:
                continue
            if r[0] == 0x30:
                self.streamed += 1
            if pred(r):
                return r
        return None

    def drain(self):
        # Time-bounded: at a 1 ms report interval the queue may never run empty.
        last, deadline = None, time.monotonic() + 0.05
        while time.monotonic() < deadline and (r := self.read(0)):
            if r[0] == 0x30:
                last = r
        return last

    def usb_cmd(self, cmd, expect_reply=True):
        self.write(0x80, [cmd])
        if not expect_reply:
            return None
        return self.wait(lambda r: r[0] == 0x81 and r[1] == cmd)

    def subcmd(self, cmd, args=b""):
        self.counter = (self.counter + 1) & 0x0F
        self.write(0x01, bytes([self.counter]) + RUMBLE_NEUTRAL + bytes([cmd]) + bytes(args))
        return self.wait(lambda r: r[0] == 0x21 and r[14] == cmd)

    def latest(self, settle=0.1):
        time.sleep(settle)
        return self.drain() or self.wait(lambda r: r[0] == 0x30, 0.2)

    def close(self):
        self.dev.close()


def unpack12(b):
    return b[0] | ((b[1] & 0x0F) << 8), (b[1] >> 4) | (b[2] << 4)


def sticks(r):
    return unpack12(r[6:9]) + unpack12(r[9:12])


# ---- tests ----------------------------------------------------------------------


def test_descriptors(pad, devinfo):
    print("\nUSB device")
    check("manufacturer is 'Nintendo Co., Ltd.'", devinfo.get("manufacturer_string") == "Nintendo Co., Ltd.",
          repr(devinfo.get("manufacturer_string")))
    check("product is 'Pro Controller'", devinfo.get("product_string") == "Pro Controller", repr(devinfo.get("product_string")))
    check("serial is '000000000001'", devinfo.get("serial_number") == "000000000001", repr(devinfo.get("serial_number")))
    early = pad.wait(lambda r: True, 0.3)
    if early:
        info(f"controller sent report 0x{early[0]:02X} before any handshake (fine if a previous run left it streaming)")


def test_usb_commands(pad):
    print("\nUSB commands (0x80)")
    r = pad.usb_cmd(0x01)
    mac = None
    if check("0x80 0x01 status gets 81 01", r is not None, why="no reply"):
        check("status says Pro Controller (type 0x03)", r[3] == 0x03, f"type 0x{r[3]:02X}")
        mac = bytes(reversed(r[4:10]))
        check("status carries a MAC address", any(mac) and mac != b"\xff" * 6, hexs(mac))
    for cmd, name in ((0x02, "handshake"), (0x03, "3 Mbit baud rate"), (0x02, "handshake again")):
        check(f"0x80 0x{cmd:02X} {name} gets 81 {cmd:02X}", pad.usb_cmd(cmd) is not None, why="no reply")
    pad.usb_cmd(0x04, expect_reply=False)
    pad.streamed = 0
    r = pad.wait(lambda r: r[0] == 0x30, 0.3)
    check("0x80 0x04 starts the 0x30 input stream", r is not None, why="no 0x30 report within 300 ms")
    return mac


def test_subcommands(pad, mac):
    print("\nSubcommands (0x01)")

    def sub(name, cmd, args=b"", ack=0x80):
        r = pad.subcmd(cmd, args)
        if not check(f"{name} (0x{cmd:02X}) gets ack 0x{ack:02X}", r is not None and r[13] == ack,
                     "no reply" if r is None else f"ack 0x{r[13]:02X}"):
            return None
        return r

    r = sub("device info", 0x02, ack=0x82)
    if r:
        check("device info firmware version is sane", r[15] in (3, 4) and r[16] != 0, f"{r[15]}.{r[16]}")
        check("device info says Pro Controller (type 0x03)", r[17] == 0x03, f"type 0x{r[17]:02X}")
        if mac:
            check("device info MAC matches the 0x80 0x01 MAC", bytes(r[19:25]) == mac, f"{hexs(r[19:25])} vs {hexs(mac)}")
        check("device info says use SPI colours", r[26] == 0x01, f"0x{r[26]:02X}")
    sub("shipment low power state", 0x08, [0x00])

    # The reads the console makes during pairing, in its order.
    spi = {}
    for addr, n, name in ((0x6000, 0x10, "serial number"), (0x6050, 0x0D, "colours"), (0x6080, 0x18, "6-axis and left stick params"),
                          (0x6098, 0x12, "right stick params"), (0x8010, 0x18, "user stick calibration"),
                          (0x603D, 0x19, "factory stick calibration"), (0x8026, 0x1A, "user 6-axis calibration"),
                          (0x6020, 0x18, "factory 6-axis calibration")):
        r = pad.subcmd(0x10, struct.pack("<IB", addr, n))
        ok = r is not None and r[13] == 0x90 and r[15:19] == struct.pack("<I", addr) and r[19] == n
        detail = "no reply" if r is None else f"ack 0x{r[13]:02X}, echo {hexs(r[15:20])}"
        if check(f"SPI read 0x{addr:04X} {name}", ok, why=detail):
            spi[addr] = bytes(r[20 : 20 + n])
            if VERBOSE:
                print(f"        {hexs(spi[addr])}")

    cal = spi.get(0x603D)
    if cal:
        lmax, lc, lmin = unpack12(cal[0:3]), unpack12(cal[3:6]), unpack12(cal[6:9])
        rc, rmin, rmax = unpack12(cal[9:12]), unpack12(cal[12:15]), unpack12(cal[15:18])
        for side, c, lo, hi in (("left", lc, lmin, lmax), ("right", rc, rmin, rmax)):
            # Centre near mid-scale; min/max are distances from it and must stay inside 12 bits.
            ok = all(0x600 <= c[i] <= 0xA00 and 0x200 <= lo[i] <= c[i] and 0x200 <= hi[i] <= 0xFFF - c[i] for i in (0, 1))
            check(f"{side} stick factory calibration is sane", ok, f"center {c[0]:03X},{c[1]:03X} min -{lo[0]:03X} max +{hi[0]:03X}")
    imu = spi.get(0x6020)
    if imu:
        acc = struct.unpack("<3h", imu[6:12])
        gyr = struct.unpack("<3h", imu[18:24])
        check("6-axis factory calibration has sensitivities", all(acc) and all(gyr), f"accel {acc} gyro {gyr}")
    user = spi.get(0x8010)
    if user:
        no_cal = all(user[i : i + 2] != b"\xb2\xa1" for i in (0, 11))
        info(f"user stick calibration {'absent (reads factory)' if no_cal else 'present'}")
    colors = spi.get(0x6050)
    if colors:
        info(f"colours body #{colors[0:3].hex()} buttons #{colors[3:6].hex()}")

    sub("set input mode 0x30", 0x03, [0x30])
    sub("trigger buttons elapsed time", 0x04, ack=0x83)
    sub("enable 6-axis sensor", 0x40, [0x01])
    sub("enable vibration", 0x48, [0x01])
    sub("set player lights", 0x30, [0x01])
    sub("set HOME light", 0x38, [0x01, 0xF0, 0x00])
    sub("NFC/IR MCU state", 0x22, [0x01])
    sub("NFC/IR MCU config", 0x21, [0x21, 0x00, 0x00], ack=0xA0)
    r = pad.subcmd(0x50)
    if r is None:
        check("regulated voltage (0x50) gets a reply", False, why="no reply")
    else:
        info(f"regulated voltage (0x50) answered ack 0x{r[13]:02X}; a real controller answers 0xD0 with its voltage")


def test_stream(pad, link):
    print("\nInput stream (0x30)")
    pad.drain()
    t0 = time.monotonic()
    reports = []
    while time.monotonic() - t0 < 1.0:
        r = pad.read(100)
        if r and r[0] == 0x30:
            reports.append(r)
    rate = len(reports) / (time.monotonic() - t0)
    check("reports arrive at least 60 per second", rate >= 60, f"{rate:.0f}/s")
    if len(reports) < 2:
        return
    steps = [(b[1] - a[1]) & 0xFF for a, b in zip(reports, reports[1:])]
    check("timer byte advances every report", all(s >= 1 for s in steps), f"steps {sorted(set(steps))[:6]}")
    last = reports[-1]
    check("report is 64 bytes", len(last) == 64, f"{len(last)} bytes")
    check("battery/connection byte reports USB power", last[2] & 0x01 == 0x01, f"0x{last[2]:02X}")
    # Without the link the bridge sits in failsafe, which is neutral too.
    check("buttons are released at rest", last[3:6] == b"\x00\x00\x00", hexs(last[3:6]))
    lx, ly, rx, ry = sticks(last)
    check("sticks are centred at rest", all(abs(v - 0x800) <= 0x20 for v in (lx, ly, rx, ry)),
          f"L {lx:03X},{ly:03X} R {rx:03X},{ry:03X}")
    pad.write(0x10, bytes([pad.counter]) + RUMBLE_NEUTRAL)  # rumble-only output, no reply expected
    check("stream survives a rumble-only output report", pad.wait(lambda r: r[0] == 0x30, 0.3) is not None, why="stream stopped")


def test_inputs(pad, link):
    print("\nInputs end to end (UART -> USB)")
    for name, (bit, byte, mask) in BUTTONS.items():
        link.set_pad(buttons=bit)
        r = pad.latest()
        ok = r is not None and r[byte] == mask and all(r[b] == 0 for b in (3, 4, 5) if b != byte)
        check(f"{name}", ok, "no report" if r is None else f"bytes 3-5 {hexs(r[3:6])}, want byte {byte} = {mask:02X}")
    hi, lo = 0x800 + 0x500, 0x800 - 0x500
    for name, pad_state, want in (
        ("left stick right", dict(lx=65535), lambda s: s[0] > hi and abs(s[1] - 0x800) < 0x40),
        ("left stick up", dict(ly=0), lambda s: s[1] > hi and abs(s[0] - 0x800) < 0x40),
        ("right stick left", dict(rx=0), lambda s: s[2] < lo and abs(s[3] - 0x800) < 0x40),
        ("right stick down", dict(ry=65535), lambda s: s[3] < lo and abs(s[2] - 0x800) < 0x40),
    ):
        link.set_pad(**pad_state)
        r = pad.latest()
        s = sticks(r) if r else None
        check(name, s is not None and want(s), "no report" if s is None else "L {:03X},{:03X} R {:03X},{:03X}".format(*s))
    link.set_pad()
    r = pad.latest()
    check("back to neutral", r is not None and r[3:6] == b"\x00\x00\x00", "no report" if r is None else hexs(r[3:6]))


def main():
    global VERBOSE
    ap = argparse.ArgumentParser(description="Test the bridge's Pro Controller profile from this PC.")
    ap.add_argument("--port", help="bridge UART port (default: find it)")
    ap.add_argument("--no-uart", action="store_true", help="USB tests only; the bridge must already be a Pro Controller")
    ap.add_argument("--verbose", action="store_true", help="print raw reports")
    args = ap.parse_args()
    VERBOSE = args.verbose

    link = None
    if not args.no_uart:
        print("Bridge link (UART)")
        link, ack = find_bridge(args.port)
        if not link:
            print("  No bridge answered on any serial port. Check the UART cable, close the Hikari Host, or pass --port / --no-uart.")
            return 2
        if not switch_to_pro(link, ack):
            link.close()
            return 1
        link.start_pump()

    try:
        print("\nLooking for the Pro Controller on USB...")
        dev = find_controller(10)
        if not dev:
            hori = hid.enumerate(HORI_VID, HORI_PID)
            print("  The bridge is still the HORI pad (0F0D:0092); run without --no-uart to switch it." if hori else
                  "  No 057E:2009 device. Is the board's USB port (not UART) plugged into this PC?")
            return 2
        try:
            pad = Pad(dev["path"])
        except OSError as e:
            print(f"  Found the controller but cannot open it ({e}). Close Steam and anything else using controllers.")
            return 2
        try:
            test_descriptors(pad, dev)
            mac = test_usb_commands(pad)
            test_subcommands(pad, mac)
            test_stream(pad, link)
            if link:
                test_inputs(pad, link)
        finally:
            pad.close()
    finally:
        if link:
            link.close()

    failed = [r for r in results if not r[0]]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    for _, name, detail in failed:
        print(f"  FAIL  {name}" + (f"  ({detail})" if detail else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
