#!/usr/bin/env python3
"""
td9_aprs.py -- Read/write TD-H9 APRS & GNSS settings over Bluetooth (BLE).

Usage / Использование:
  python td9_aprs.py dump                 -- raw hex области APRS/GNSS
  python td9_aprs.py show                 -- все настройки APRS+GNSS
  python td9_aprs.py get callsign         -- одно поле
  python td9_aprs.py set callsign R2AKA   -- записать поле
  python td9_aprs.py set beacon_interval 600

Dependencies / Зависимости: pip install bleak
Рация включена + Bluetooth programming mode.
"""

import argparse
import asyncio
import os
import struct
import sys

# ============================================================================
# BLE service / характеристики (клон-сервис FF00)
# ============================================================================
SERVICE_FF00 = "0000ff00-0000-1000-8000-00805f9b34fb"
CHAR_WRITE = "0000ff02-0000-1000-8000-00805f9b34fb"
CHAR_NOTIFY = "0000ff01-0000-1000-8000-00805f9b34fb"

MAGIC = bytes([0x50, 0x56, 0x4F, 0x4A, 0x48, 0x5C, 0x14])
MEM_SIZE = 0x3140
IDENT_SIZE = 8
IMAGE_SIZE = MEM_SIZE + IDENT_SIZE   # 0x3148
BLOCK = 0x20


def hex_bytes(b):
    return " ".join("%02x" % x for x in b)


# ============================================================================
# Image wrapper (порт settings_parser.py)
# ============================================================================
class TDH9Image:
    """Wrap a full clone image; fields index the MEMORY portion (image[8:])."""

    def __init__(self, image):
        if len(image) != IMAGE_SIZE:
            raise ValueError("Expected %d bytes, got %d" % (IMAGE_SIZE, len(image)))
        self._image = image
        self._mem = image[IDENT_SIZE:]

    @property
    def identity(self):
        return self._image[:IDENT_SIZE]

    @property
    def mode(self):
        return {0x4E: "Normal", 0x48: "HAM", 0x47: "GMRS"}.get(
            self._image[7] if len(self._image) > 7 else 0xFF, "Unknown")

    def byte(self, off):
        return self._mem[off]

    def u16le(self, off):
        return self._mem[off] | (self._mem[off + 1] << 8)

    def u32be(self, off):
        return struct.unpack(">I", self._mem[off:off + 4])[0]

    def i32be(self, off):
        return struct.unpack(">i", self._mem[off:off + 4])[0]

    def chars(self, off, length):
        raw = self._mem[off:off + length]
        end = -1
        for i, b in enumerate(raw):
            if b in (0, 0xFF):
                end = i
                break
        if end < 0:
            end = len(raw)
        chars = raw[:end]
        if not chars:
            return ""
        if all(32 <= b <= 126 for b in chars):
            return chars.decode("ascii", errors="replace")
        return "Raw: " + " ".join("%02x" % b for b in raw)

    def is_all_ff(self, off, length):
        return all(b == 0xFF for b in self._mem[off:off + length])


# ============================================================================
# Поля и их отображение (из settings_parser.py — ПРОВЕРЕНО)
# ============================================================================
SSID_CHOICES = {i: str(i) for i in range(16)}
ICON_TABLE_CHOICES = {0x2F: "Primary /", 0x5C: "Alternate \\"}
MIC_TYPES = ["Off duty", "En route", "In service", "Returning",
             "Committed", "Special", "Priority", "Emergency"]
TX_CHANNEL_CHOICES = {0: "A locked", 1: "B locked", 2: "A auto", 3: "B auto", 4: "A + B"}
RX_CHANNEL_CHOICES = {0: "Off", 1: "A", 2: "B"}
DIGI_FWD_CHOICES = {0: "A", 1: "B", 2: "A + B"}
POS_SOURCE_CHOICES = {0: "Fixed coordinates", 1: "GNSS"}
FORMAT_CHOICES = {0: "Degrees", 1: "Degrees + minutes", 2: "Degrees + minutes + seconds"}
SPEED_CHOICES = {0: "km/h", 1: "knots", 2: "m/s"}
DIST_CHOICES = {0: "km", 1: "nautical miles", 2: "miles"}
ALT_CHOICES = {0: "meters", 1: "feet"}


def _opt(mapping, key):
    return mapping.get(key, "Unknown (%d)" % key)


# ============================================================================
# BLE радио (чтение полного образа по протоколу ble_radio.py)
# ============================================================================
class Radio:
    def __init__(self, client, write_char):
        self.client = client
        self.write_char = write_char
        self.queue = bytearray()
        self.wake = None
        self.read_mode = "auto"

    def receive(self, data):
        self.queue += data
        if self.wake:
            w, self.wake = self.wake, None
            w.set()

    async def read_exact(self, n, timeout=2.5):
        out = bytearray()
        loop = asyncio.get_event_loop()
        deadline = loop.time() + timeout
        while len(out) < n:
            need = n - len(out)
            if self.queue:
                take = self.queue[:need]
                del self.queue[:need]
                out += take
                continue
            if loop.time() > deadline:
                raise TimeoutError("read timeout / таймаут чтения")
            ev = asyncio.Event()
            self.wake = ev
            try:
                await asyncio.wait_for(ev.wait(), timeout=max(0.05, deadline - loop.time()))
            except asyncio.TimeoutError:
                if self.wake is ev:
                    self.wake = None
                raise TimeoutError("read timeout / таймаут чтения")
        return bytes(out)

    async def send(self, data):
        # Write-without-response: the radio replies every command with a 0x06
        # ACK over the notify characteristic (ff01). Using write-without-response
        # lets that ACK arrive freely in the notify queue; write-with-response
        # can stall waiting on a GATT ack contesting the same link.
        # Запись без ответа: рация отвечает на каждую команду ACK 0x06 по notify
        # (ff01). write-without-response не блокируется на GATT-ACK и позволяет
        # этому ACK свободно прийти в очередь notify.
        await self.client.write_gatt_char(self.write_char, bytearray(data), response=False)

    async def enter_programming(self):
        """Handshake; returns the 8-byte identity.

        Рукопожатие; возвращает 8-байтную идентичность. """
        self.queue.clear()
        self.read_mode = "auto"
        await self.send(MAGIC)
        ack0 = (await self.read_exact(1))[0]
        if ack0 != 0x06:
            raise RuntimeError("Handshake ACK expected / Ожидался ACK рукопожатия")
        await self.send(bytes([0x02]))
        # skip late 0x06 (ack of a previous exit) before identity
        first = (await self.read_exact(1))[0]
        for _ in range(4):
            if first != 0x06:
                break
            first = (await self.read_exact(1))[0]
        ident = bytes([first]) + await self.read_exact(7)
        await self.send(bytes([0x06]))
        ack2 = (await self.read_exact(1))[0]
        if ack2 != 0x06:
            raise RuntimeError("Programming ACK expected / Ожидался ACK режима")
        return ident

    async def leave_programming(self):
        """Exit programming mode (0x45). / Выход из режима программирования."""
        try:
            await self.send(bytes([0x45]))
        except Exception:
            pass

    async def read_block(self, address):
        """Read one 32-byte block via 0x52 (public wrapper). / Читает блок (публично)."""
        cmd = bytes([0x52, (address >> 8) & 255, address & 255, BLOCK])
        await self.send(cmd)
        return await self._read_block(address)

    async def _read_block(self, address):
        expected = bytes([0x57, (address >> 8) & 255, address & 255, BLOCK])
        header = await self.read_exact(4)
        if header != expected:
            nxt = await self.read_exact(1)
            shifted = bytes([header[1], header[2], header[3], nxt[0]])
            if shifted == expected:
                header = shifted
                self.read_mode = "yes"
        if header != expected:
            raise RuntimeError("Invalid read header at 0x%04x: %s"
                               % (address, header.hex()))
        data = await self.read_exact(BLOCK)
        mode = self.read_mode
        if mode == "yes":
            await self.read_exact(1)
        elif mode == "auto":
            try:
                await self.read_exact(1, timeout=0.15)
                mode = "yes"
            except TimeoutError:
                mode = "no"
            self.read_mode = mode
        return data

    async def read_image(self, on_progress=None, leave=True):
        """Read the full image: 8-byte identity + 0x3140 memory blocks.

        Читает полный образ: 8 байт identity + блоки памяти 0x3140 по 32.
        If leave=False, stays in programming mode (to write right after).
        Если leave=False, остаёмся в режиме программирования (чтобы сразу писать)."""
        image = bytearray(IMAGE_SIZE)
        ident = await self.enter_programming()
        image[:8] = ident
        for address in range(0, MEM_SIZE, BLOCK):
            data = await self.read_block(address)
            image[address + 8: address + 8 + BLOCK] = data
            if on_progress:
                on_progress(address + BLOCK, MEM_SIZE)
        if leave:
            await self.leave_programming()
        return bytes(image)


def make_progress(label=""):
    return lambda d, t: sys.stdout.write("\r  %s%d/%d" % ("%s " % label if label else "", d, t)) or sys.stdout.flush()


# ============================================================================
# Чтение/отображение полей (из settings_parser.py)
# ============================================================================

def format_fixed_position(img):
    if img.is_all_ff(0x306C, 0x307B - 0x306C + 1):
        return "Not configured"
    lon = img.u32be(0x306C) / 1_000_000.0
    lat = img.u32be(0x3071) / 1_000_000.0
    alt = img.i32be(0x3076) / 1_000_000.0
    ew = chr(img.byte(0x3070)) if img.byte(0x3070) in (ord("E"), ord("W")) else "?"
    ns = chr(img.byte(0x3075)) if img.byte(0x3075) in (ord("N"), ord("S")) else "?"
    if lon > 180 or lat > 90:
        return "Stored bytes are not a valid fixed position"
    return "%f%s, %f%s, %.1fm" % (lat, ns, lon, ew, alt)


def parse_aprs(img):
    """Return an ordered dict of human-readable labels (keys match _field_labels)."""
    out = {}

    def label(key, raw, text):
        out[key] = text

    # APRS identity / beacon
    label("aprs_on", img.byte(0x307C), "On" if img.byte(0x307C) == 1 else "Off")
    label("callsign", img.chars(0x307D, 6), img.chars(0x307D, 6))
    label("ssid", img.byte(0x3083), _opt(SSID_CHOICES, img.byte(0x3083)))
    label("icon_table", img.byte(0x3084), _opt(ICON_TABLE_CHOICES, img.byte(0x3084)))
    label("comment", img.chars(0x3095, 64), img.chars(0x3095, 64))
    label("icon_symbol", img.byte(0x30D6), str(img.byte(0x30D6)))
    label("mic_e_start", img.byte(0x30D7), "On" if img.byte(0x30D7) == 1 else "Off")
    label("mic_e_type", img.byte(0x30D8),
          MIC_TYPES[img.byte(0x30D8)] if img.byte(0x30D8) < len(MIC_TYPES) else str(img.byte(0x30D8)))

    # routes
    for prefix, base in (("route1", 0x30D9), ("route2", 0x30E2)):
        name = img.chars(base, 8)
        count = img.byte(base + 8)
        label(prefix + "_name", name, name if name else "—")
        label(prefix + "_count", count, str(count))

    # reports & beacon
    for key, off in (("report_voltage", 0x30EB), ("report_sats", 0x30EC),
                     ("report_mileage", 0x30ED), ("ptt_link", 0x30EE),
                     ("timed_beacon", 0x30EF)):
        label(key, img.byte(off), "On" if img.byte(off) == 1 else "Off")
    interval = img.u16le(0x30F0)
    label("beacon_interval", interval, "%d s" % interval if interval else "—")

    # relay / routing
    label("digi_fwd_channel", img.byte(0x30F2), _opt(DIGI_FWD_CHOICES, img.byte(0x30F2)))
    label("digi1_enabled", img.byte(0x30F3), "On" if img.byte(0x30F3) == 1 else "Off")
    label("digi1_name", img.chars(0x30F4, 8), img.chars(0x30F4, 8))
    label("digi2_enabled", img.byte(0x30FC), "On" if img.byte(0x30FC) == 1 else "Off")
    label("digi2_name", img.chars(0x30FD, 8), img.chars(0x30FD, 8))
    label("forward_wait", img.byte(0x3105), "%d s" % img.byte(0x3105))
    label("remote_password", img.chars(0x3106, 8), img.chars(0x3106, 8))
    label("rx_channel", img.byte(0x310E), _opt(RX_CHANNEL_CHOICES, img.byte(0x310E)))
    label("tx_channel", img.byte(0x310F), _opt(TX_CHANNEL_CHOICES, img.byte(0x310F)))
    label("ptt_priority", img.byte(0x3110), "APRS" if img.byte(0x3110) == 1 else "Call")
    label("rx_popup", img.byte(0x3113), "On" if img.byte(0x3113) == 1 else "Off")

    # GNSS
    label("gps_on", img.byte(0x3066), "On" if img.byte(0x3066) == 1 else "Off")
    label("position_format", img.byte(0x3067), _opt(FORMAT_CHOICES, img.byte(0x3067)))
    label("time_zone", img.byte(0x3068), _opt({i: "UTC%+d" % (i - 12) for i in range(25)}, img.byte(0x3068)))
    label("speed_unit", img.byte(0x3069), _opt(SPEED_CHOICES, img.byte(0x3069)))
    label("dist_unit", img.byte(0x306A), _opt(DIST_CHOICES, img.byte(0x306A)))
    label("alt_unit", img.byte(0x306B), _opt(ALT_CHOICES, img.byte(0x306B)))
    label("fixed_position", format_fixed_position(img), format_fixed_position(img))
    label("station_type", img.byte(0x307B), _opt(POS_SOURCE_CHOICES, img.byte(0x307B)))

    return out


# Настройки для записи (только поля, где известен адрес и тип)
WRITABLE = {
    # key: (offset, kind)  kind: 'byte'|'text'|'u16le'|'yesno'|'opt'
    "aprs_on": (0x307C, "yesno"),
    "callsign": (0x307D, "text6"),
    "ssid": (0x3083, "byte"),
    "icon_table": (0x3084, "byte"),
    "comment": (0x3095, "text64"),
    "icon_symbol": (0x30D6, "byte"),
    "mic_e_start": (0x30D7, "yesno"),
    "mic_e_type": (0x30D8, "byte"),
    "route1_name": (0x30D9, "text8"),
    "route1_count": (0x30E1, "byte"),
    "route2_name": (0x30E2, "text8"),
    "route2_count": (0x30EA, "byte"),
    "report_voltage": (0x30EB, "yesno"),
    "report_sats": (0x30EC, "yesno"),
    "report_mileage": (0x30ED, "yesno"),
    "ptt_link": (0x30EE, "yesno"),
    "timed_beacon": (0x30EF, "yesno"),
    "beacon_interval": (0x30F0, "u16le"),
    "digi_fwd_channel": (0x30F2, "byte"),
    "digi1_enabled": (0x30F3, "yesno"),
    "digi1_name": (0x30F4, "text8"),
    "digi2_enabled": (0x30FC, "yesno"),
    "digi2_name": (0x30FD, "text8"),
    "forward_wait": (0x3105, "byte"),
    "remote_password": (0x3106, "text8"),
    "rx_channel": (0x310E, "byte"),
    "tx_channel": (0x310F, "byte"),
    "ptt_priority": (0x3110, "yesno"),
    "rx_popup": (0x3113, "yesno"),
    # Имя 199-го канала (кодплаг: имена каналов @ 0x0D40 + i*8; канал 199 -> 0x1370)
    "chan199_name": (0x1370, "text8"),
    "gps_on": (0x3066, "yesno"),
    "position_format": (0x3067, "byte"),
    "time_zone": (0x3068, "byte"),
    "speed_unit": (0x3069, "byte"),
    "dist_unit": (0x306A, "byte"),
    "alt_unit": (0x306B, "byte"),
    "station_type": (0x307B, "yesno"),
}


# Byte length of a writable field by kind / длина поля (в байтах) по типу
_KIND_LEN = {
    "byte": 1, "yesno": 1, "opt": 1,
    "u16le": 2,
    "text6": 6, "text8": 8, "text64": 64,
}

# Extra value ranges for byte/opt fields, beyond the base 0..255.
# Доп. диапазоны значений для byte/opt полей, сверх базового 0..255.
_RANGES = {
    "ssid": (0, 15),          # APRS SSID 0-15 / SSID APRS 0-15
    "beacon_interval": (0, 65535),
}

# Allowed keys for choose-from-a-set fields. Values may be entered as the raw
# index (int) or, when unambiguous, as the human label string.
# Допустимые ключи для полей с фиксированным набором. Значение можно задать
# числовым индексом или (если однозначно) текстовой меткой.
_CHOICE_MAP = {
    "icon_table": {v: k for k, v in ICON_TABLE_CHOICES.items()},
    "mic_e_type": {v.lower(): i for i, v in enumerate(MIC_TYPES)},
    "tx_channel": {v.lower(): k for k, v in TX_CHANNEL_CHOICES.items()},
    "rx_channel": {v.lower(): k for k, v in RX_CHANNEL_CHOICES.items()},
    "digi_fwd_channel": {v.lower(): k for k, v in DIGI_FWD_CHOICES.items()},
    "station_type": {v.lower(): k for k, v in POS_SOURCE_CHOICES.items()},
    "position_format": {v.lower(): k for k, v in FORMAT_CHOICES.items()},
    "speed_unit": {v.lower(): k for k, v in SPEED_CHOICES.items()},
    "dist_unit": {v.lower(): k for k, v in DIST_CHOICES.items()},
    "alt_unit": {v.lower(): k for k, v in ALT_CHOICES.items()},
}


def setting_bounds(key):
    """Return (offset, length) of a writable field. Raises KeyError for unknown."""
    if key not in WRITABLE:
        raise KeyError("Unknown or read-only field '%s'. Writable: %s"
                       % (key, ", ".join(sorted(WRITABLE))))
    off, kind = WRITABLE[key]
    return off, _KIND_LEN[kind]


def decode_setting(key, data):
    """Decode a field's raw bytes (exactly `length` bytes, field starts at 0)."""
    off, length = setting_bounds(key)
    raw = bytes(data[:length])
    kind = WRITABLE[key][1]
    if kind in ("text6", "text8", "text64"):
        return raw.rstrip(b"\x00").decode("latin1", "replace")
    if kind == "u16le":
        return str(raw[0] | (raw[1] << 8) if len(raw) >= 2 else 0)
    if kind == "yesno":
        return "On" if raw and raw[0] == 1 else "Off"
    return str(raw[0] if raw else 0)


def encode_setting(key, value):
    """Encode a human value into (offset, bytes). Raises KeyError for unknown."""
    if key not in WRITABLE:
        raise KeyError("Unknown or read-only field '%s'. Writable: %s"
                       % (key, ", ".join(sorted(WRITABLE))))
    off, kind = WRITABLE[key]
    if kind in ("text6", "text8", "text64"):
        maxlen = _KIND_LEN[kind]
        enc = value.encode("latin1")
        if len(enc) > maxlen:
            print("  [warn] %s: %d chars > %d, truncated to %r"
                  % (key, len(value), maxlen, enc[:maxlen].decode("latin1")))
            enc = enc[:maxlen]
        return off, enc + b"\x00" * (maxlen - len(enc))
    if kind == "u16le":
        lo, hi = _RANGES.get(key, (0, 65535))
        v = int(value)
        if not (lo <= v <= hi):
            raise ValueError("%s must be in [%d..%d], got %r" % (key, lo, hi, value))
        return off, bytes([v & 255, v >> 8])

    # fields with a predefined set: accept index or human label.
    # (kept before yesno so station_type accepts "gnss"/"fixed", etc.)
    # Поля с фиксированным набором: принимаем индекс или метку.
    # (до yesno, чтобы station_type понимал "gnss"/"fixed" и т.п.)
    if key in _CHOICE_MAP:
        lookup = _CHOICE_MAP[key]
        sv = " ".join(str(value).lower().split())
        if sv in lookup:
            v = lookup[sv]
        else:
            try:
                v = int(value)
            except ValueError:
                raise ValueError("%s: unknown value %r; pick one: %s"
                                 % (key, value,
                                    ", ".join("%s(%s)" % (k, lbl)
                                              for lbl, k in sorted(lookup.items(),
                                                                  key=lambda x: x[1])))) \
                    from None
            keys = sorted(set(lookup.values()))
            if v not in keys:
                raise ValueError("%s must be one of %s, got %r"
                                 % (key, keys, value))
        return off, bytes([v & 255])

    # byte / yesno numeric range check / диапазонная проверка числа
    if kind == "yesno":
        v = 1 if value.lower() in ("1", "on", "yes") else 0
        return off, bytes([v])
    if key in _RANGES:
        lo, hi = _RANGES[key]
        if not (lo <= int(value) <= hi):
            raise ValueError("%s must be in [%d..%d], got %r"
                             % (key, lo, hi, value))
        return off, bytes([int(value) & 255])
    if not (0 <= int(value) <= 255):
        raise ValueError("%s must be in [0..255], got %r" % (key, value))
    return off, bytes([int(value) & 255])


# ============================================================================
# Main
# ============================================================================

def print_report(values):
    print("TD-H9 mode:      %s" % values.get("mode", ""))
    print("APRS:            %s" % values.get("aprs_on"))
    print("Callsign:        %s-%s" % (values.get("callsign"), values.get("ssid")))
    print("Icon table:      %s  (symbol %s)" % (values.get("icon_table"), values.get("icon_symbol")))
    print("Comment:         %s" % values.get("comment"))
    print("Route 1:         %s-%s" % (values.get("route1_name"), values.get("route1_count")))
    print("Route 2:         %s-%s" % (values.get("route2_name"), values.get("route2_count")))
    print("Mic-E:           %s  %s" % (values.get("mic_e_start"), values.get("mic_e_type")))
    print("Reports:         volt=%s sats=%s mil=%s" % (values.get("report_voltage"), values.get("report_sats"), values.get("report_mileage")))
    print("PTT beacon:      %s" % values.get("ptt_link"))
    print("Timed beacon:    %s  (%s)" % (values.get("timed_beacon"), values.get("beacon_interval")))
    print("RX channel:      %s" % values.get("rx_channel"))
    print("TX channel:      %s" % values.get("tx_channel"))
    print("PTT priority:    %s" % values.get("ptt_priority"))
    print("RX popup:        %s" % values.get("rx_popup"))
    print()
    print("GNSS:")
    print("  GPS:           %s" % values.get("gps_on"))
    print("  Format:        %s" % values.get("position_format"))
    print("  Time zone:     %s" % values.get("time_zone"))
    print("  Units:         %s / %s / %s" % (values.get("speed_unit"), values.get("dist_unit"), values.get("alt_unit")))
    print("  Fixed point:   %s" % values.get("fixed_position"))
    print("  Station type:  %s" % values.get("station_type"))
    print()
    print("APRS relay / routing:")
    print("  Forward ch:    %s" % values.get("digi_fwd_channel"))
    print("  DIGI1:         %s  %s" % (values.get("digi1_enabled"), values.get("digi1_name")))
    print("  DIGI2:         %s  %s" % (values.get("digi2_enabled"), values.get("digi2_name")))
    print("  Fwd wait:      %s" % values.get("forward_wait"))
    print("  Remote pwd:    %s" % (values.get("remote_password") or "—"))


async def connect_radio(args):
    from bleak import BleakScanner, BleakClient
    print("Scanning for TD-H9... / Сканирую TD-H9...")
    devices = await BleakScanner.discover(timeout=args.scan_timeout)
    dev = next((d for d in devices if (d.name or "").strip().startswith("TD-H9")), None)
    if dev is None:
        return None, None
    print("Connecting to %s (%s)..." % (dev.name, dev.address))
    client = BleakClient(dev.address)
    await client.connect()
    write_char = client.services.get_characteristic(CHAR_WRITE)
    notify_char = client.services.get_characteristic(CHAR_NOTIFY)
    radio = Radio(client, write_char)
    return client, radio, notify_char


async def main_async(args):
    from bleak import BleakScanner, BleakClient
    print("Scanning for TD-H9... / Сканирую TD-H9...")
    devices = await BleakScanner.discover(timeout=args.scan_timeout)
    dev = next((d for d in devices if (d.name or "").strip().startswith("TD-H9")), None)
    if dev is None:
        print("No TD-H9 found / Рация не найдена.")
        return 1
    print("Connecting to %s (%s)..." % (dev.name, dev.address))
    async with BleakClient(dev.address) as client:
        write_char = client.services.get_characteristic(CHAR_WRITE)
        notify_char = client.services.get_characteristic(CHAR_NOTIFY)
        radio = Radio(client, write_char)

        def internal_notify(_c, data):
            radio.receive(bytes(data))
        await client.start_notify(CHAR_NOTIFY, internal_notify)

        # Read the full image only for show/dump; for get/set we read just the
        # blocks of the single field below (much faster than 12608 bytes).
        # Полный образ читаем только для show/dump; для get/set читаем лишь
        # блоки нужного поля ниже (гораздо быстрее 12608 байт).
        if args.command in ("show", "dump"):
            raw = await radio.read_image(make_progress("read"))
            print()
            img = TDH9Image(raw)
            # We left programming mode during read; stop notify now.
            # Мы вышли из режима программирования при чтении; останавливаем notify.
            try:
                await client.stop_notify(CHAR_NOTIFY)
            except Exception:
                pass
        else:
            raw = None
            img = None

        if args.command == "dump":
            print("Identity: %s" % img.identity.hex())
            print("Mode:     %s" % img.mode)
            for base in range(0x3060, 0x3120, 16):
                print("%04X  %s" % (base, hex_bytes(raw[8 + base:8 + base + 16])))
            return 0

        if args.command == "show":
            print_report(dict(parse_aprs(img), mode=img.mode))
            return 0

        if args.command == "get":
            off, length = setting_bounds(args.field)
            start_byte = off
            end_byte_excl = off + length
            first_addr = (start_byte // BLOCK) * BLOCK
            last_addr = ((end_byte_excl - 1) // BLOCK) * BLOCK
            target_addrs = list(range(first_addr, last_addr + 1, BLOCK))
            try:
                await radio.enter_programming()
                # Read ONLY the blocks the field touches / читаем только блоки поля
                blocks = {}
                for addr in target_addrs:
                    blocks[addr] = await radio.read_block(addr)
            finally:
                await radio.leave_programming()
                try:
                    await client.stop_notify(CHAR_NOTIFY)
                except Exception:
                    pass
            # reassemble the field's raw bytes from the blocks: for each block
            # take only the slice actually covered by the field.
            # Собираем байты поля из блоков: для каждого блока берём только
            # тот срез, который реально покрывает поле.
            raw_field = bytes().join(
                blocks[addr][max(start_byte, addr) - addr: min(end_byte_excl, addr + BLOCK) - addr]
                for addr in target_addrs)
            print("%s : %s" % (args.field, decode_setting(args.field, raw_field)))
            return 0

        if args.command == "set":
            off, data = encode_setting(args.field, args.value)
            print("Setting %s @0x%x = %s" % (args.field, off, args.value))
            # The field may span two 32-byte blocks (e.g. callsign at 0x307D).
            # Field может захватывать два 32-байтных блока (напр. callsign 0x307D).
            start_byte = off                      # memory offset
            end_byte = off + len(data)            # memory offset (exclusive)
            first_addr = (start_byte // BLOCK) * BLOCK
            last_addr = ((end_byte - 1) // BLOCK) * BLOCK
            target_addrs = list(range(first_addr, last_addr + 1, BLOCK))
            print("  Affected block(s): %s" % ", ".join("0x%04x" % a for a in target_addrs))

            try:
                print("  [set] entering programming... / вход в режим...")
                ident = await radio.enter_programming()
                print("  [set] identity ok:", ident.hex())

                # Read ONLY the affected blocks, patch, then write them back.
                # Читаем ТОЛЬКО затронутые блоки, патчим и пишем обратно.
                blocks = {}
                for addr in target_addrs:
                    blocks[addr] = bytearray(await radio.read_block(addr))
                # splice the new value into the affected blocks / вписываем новое значение
                remaining = bytes(data)
                cur = start_byte
                for addr in target_addrs:
                    # bytes of this block covered by the field / байты блока, покрытые полем
                    lo = max(cur, addr) - addr
                    hi = min(end_byte, addr + BLOCK) - addr
                    if hi > lo:
                        take = hi - lo
                        blocks[addr][lo:hi] = remaining[:take]
                        remaining = remaining[take:]
                        cur = addr + hi

                for addr in target_addrs:
                    payload = bytes(blocks[addr])
                    chk = sum(payload) & 255
                    frame = bytes([0x57, (addr >> 8) & 255, addr & 255, BLOCK]) + payload + bytes([chk])
                    print("  [set] writing block 0x%04x len=%d ..." % (addr, len(frame)))
                    await radio.send(frame)
                    ack = (await radio.read_exact(1, timeout=5.0))[0]
                    if ack != 0x06:
                        raise RuntimeError("write ack expected, got %s" % hex(ack))
                    print("  [set] ACK ok for 0x%04x" % addr)
                    verify = await radio.read_block(addr)
                    if verify != payload:
                        raise RuntimeError("Readback differs at 0x%x" % addr)
                    print("  [set] verified 0x%04x" % addr)
            finally:
                await radio.leave_programming()
                try:
                    await client.stop_notify(CHAR_NOTIFY)
                except Exception:
                    pass
            print("Written / Записано. Power-cycle the radio or APRS / Перезапустите рацию или APRS.")
            return 0

    return 0


def main():
    ap = argparse.ArgumentParser(description="TD-H9 APRS/GNSS over Bluetooth / Настройки APRS/GNSS TD-H9 по Bluetooth")
    ap.add_argument("command", choices=["show", "get", "set", "dump", "list"])
    ap.add_argument("field", nargs="?", help="field key (get/set)")
    ap.add_argument("value", nargs="?", help="new value (set)")
    ap.add_argument("--scan-timeout", type=int, default=8)
    args = ap.parse_args()

    if args.command == "list":
        print("Supported writable fields / Поддерживаемые поля:")
        for key, (off, kind) in sorted(WRITABLE.items(), key=lambda kv: kv[1][0]):
            if key in _CHOICE_MAP:
                allowed = ", ".join("%s=%s" % (lbl, k) for k, lbl in
                                    sorted(_CHOICE_MAP[key].items(), key=lambda x: x[1]))
            elif kind == "yesno":
                allowed = "on/off/1/0"
            elif key in _RANGES:
                allowed = "%d..%d" % _RANGES[key]
            elif kind in ("text6", "text8", "text64"):
                allowed = "text,%d" % _KIND_LEN[kind]
            elif kind == "u16le":
                allowed = "0..65535"
            else:
                allowed = "0..255"
            print("  %-16s @0x%04X  len=%-3d (%s)  [%s]"
                  % (key, off, _KIND_LEN[kind], kind, allowed))
        return 0

    if args.command in ("get", "set") and not args.field:
        ap.error("'%s' requires a field" % args.command)
    if args.command == "set" and args.value is None:
        ap.error("'set' requires a value")

    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        print("\nInterrupted / Прервано.")
        return 130
    except Exception as e:
        print("Error / Ошибка: %s" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())