#!/usr/bin/env python3
"""
td9_kpi.py -- Store / view picture packs on the TD-H9 resource chip over BLE.

A picture pack is a raw 160x128 RGB565 big-endian image (40,960 bytes, the
radio's native screen format) stored in a free area of the resource flash.
The companion menu app "DKXCEPIC" displays them and flips Up/Down in a loop.

Area format (written by this tool, read by the app):
  header  44 4B 58 43 45 50 49            "DKXCEPI"
          01                               count byte
  ...      pictures, each 160x128 RGB565 big-endian (40960 bytes)

Free area: the resource flash "font tail" (0x717000..0x7FFFFF), which stock
never writes. This tool VERIFIES the target is fully 0xFF before writing and
refuses otherwise.

Usage / Использование:
  python td9_kpi.py check [--addr 0x717000] [--span 0x80000]
      -- verify a range is free (all 0xFF); default 0x717000..0x7FFFFF
  python td9_kpi.py write img1.bin [img2.bin ...]
      -- write a pack of raw RGB565 .bin pictures (+ .png/.jpg via Pillow)
  python td9_kpi.py dump --addr <start> --out <file>
      -- dump raw bytes of a range (for inspection)

Depends on: pip install bleak, (optional) Pillow for png/jpg input.
Radio on + Bluetooth programming mode, H9 Toolbox Firmware (S2 write service).
"""

import argparse
import asyncio
import os
import struct
import sys

# ============================================================================
# BLE / resource service (reused from td9_logo.py)
# ============================================================================
SERVICE = "0000ff00-0000-1000-8000-00805f9b34fb"
CHAR_WRITE = "0000ff02-0000-1000-8000-00805f9b34fb"
CHAR_NOTIFY = "0000ff01-0000-1000-8000-00805f9b34fb"

MAGIC = bytes([0x50, 0x56, 0x4F, 0x4A, 0x48, 0x5C, 0x14])
BANK_SIZE = 0x8000
BLOCK = 0x20
SECTOR = 0x1000
STATUS_BASE = 0x4000
DATA_BASE = 0x8000
CONTROL_ADDRESS = 0x4000
DATA_ADDRESS = 0x4020
OP = {"disarm": 0, "erase": 1, "program": 2}
CHECK_EVERY = 16
ERASED = 0xFF

# Picture pack format / формат пакета картинок
KPI_MAGIC = b"DKXCEPI"
KPI_HEADER = 7 + 1          # magic (7) + count (1)
IMG_W, IMG_H = 160, 128
IMG_BYTES = IMG_W * IMG_H * 2   # 40960

# Free area (resource chip): font tail, stock never writes.
# Свободная область: хвост font-партиции, сток не пишет.
DEFAULT_START = 0x717000
DEFAULT_END = 0x800000
SECTOR_SIZE = 0x1000

# Radio lock password: 8 bytes just before the picture area.
# Пароль блокировки рации: 8 байт непосредственно перед областью картинок.
PASS_ADDR = 0x716FF8
PASS_LEN = 8

# Bulk read (S1 fast) — порт из web resource.mjs / radio.mjs. Наибольший
# выигрыш: каждая операция читает до 2 КиБ пачкой notify, а не 32 байта.
BULK_BASE = 0x3C00
BULK_SIZES = [16, 48, 112, 176, 240]
LINK_STOCK_ADDRESS = BULK_BASE + 17 * BLOCK
LINK_FIXED_ADDRESS = BULK_BASE + 18 * BLOCK
BULK_HEADER = 4


def parse_bulk_reply(block):
    """32-byte status block -> dict or None. / Статус-блок bulk-чтения."""
    if block[:4] != b"H9BK":
        return None
    result = int.from_bytes(block[4:8], "little")
    counts = int.from_bytes(block[8:12], "little")
    cursor = int.from_bytes(block[12:16], "little")
    return {
        "status": result >> 24,
        "start": result & 0xFFFFFF,
        "count": counts & 0xFFF,
        "capacity": (counts >> 12) & 0x3FF,
        "cursor": cursor & 0xFFFFFF,
        "cursorValid": cursor >> 24 in (0xB7, 0xB8),
        "cursorRam": cursor >> 24 == 0xB8,
    }


def bulk_slot_address(size_index, wait=False):
    return BULK_BASE + (size_index + (8 if wait else 0)) * BLOCK


def is_bulk_packet(bytez):
    return len(bytez) >= 4 and bytez[0] == 0xB7 and bytez[1] == 0x5A


def assemble_bulk(packets, reply, size):
    total = 0
    for i, p in enumerate(packets):
        if p[2] != (i & 255):
            raise RuntimeError("bulk seq mismatch")
        dl = p[3]
        total += dl
    if total != reply["count"]:
        raise RuntimeError("bulk len mismatch: got %d, radio %d" % (total, reply["count"]))
    out = bytearray()
    for p in packets:
        out += p[BULK_HEADER:]
    return bytes(out)


def hex_bytes(b):
    return " ".join("%02x" % x for x in b[:24])


# ============================================================================
# Dirty-import transport: reuse td9_logo.py's working S1/S2 implementation
# ============================================================================
def _load_logo_mod():
    import importlib.util
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("td9_logo", os.path.join(here, "td9_logo.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---- standalone bits (not relying on logo module where trivial) ----
class Radio:
    """Minimal BLE radio with S1 read / S2 write, decoupled from logo tool."""

    def __init__(self, client, write_char):
        self.client = client
        self.write_char = write_char
        self.queue = bytearray()
        self.wake = None
        self.read_mode = "auto"
        self.bulk_partial = None
        self.bulk_packets = []
        self.bulk_active = False

    def receive(self, data):
        rest = bytes(data)
        # Если bulk-режим активен и есть B7 5A заголовок — собираем пакет.
        while rest:
            if self.bulk_partial is not None:
                take = min(len(rest), self.bulk_partial["rem"])
                self.bulk_partial["bytes"] += rest[:take]
                self.bulk_partial["rem"] -= take
                rest = rest[take:]
                if self.bulk_partial["rem"] == 0:
                    self.bulk_packets.append(self.bulk_partial["bytes"])
                    self.bulk_partial = None
            elif self.bulk_active and len(rest) >= 3 and rest[0] == 0xB7 and rest[1] == 0x5A:
                # B7 5A seq len | payload(len bytes)
                hdr = rest[2]
                dlen = rest[3]
                self.bulk_partial = {"rem": dlen + BULK_HEADER, "bytes": bytearray()}
                # обработаем заголовок этих 4 байт
                take = min(len(rest), self.bulk_partial["rem"])
                self.bulk_partial["bytes"] += rest[:take]
                self.bulk_partial["rem"] -= take
                rest = rest[take:]
                if self.bulk_partial["rem"] == 0:
                    self.bulk_packets.append(self.bulk_partial["bytes"])
                    self.bulk_partial = None
            else:
                self.queue += rest
                rest = b""
        if self.wake:
            w, self.wake = self.wake, None
            w.set()

    async def read_exact(self, n, timeout=3.0):
        out = bytearray()
        loop = asyncio.get_event_loop()
        deadline = loop.time() + timeout
        while len(out) < n:
            if self.queue:
                take = min(n - len(out), len(self.queue))
                out += self.queue[:take]
                del self.queue[:take]
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
        await self.client.write_gatt_char(self.write_char, bytearray(data), response=False)

    async def expect_ack(self, label="ACK"):
        r = await self.read_exact(1)
        if r[0] != 0x06:
            raise RuntimeError("%s: expected ACK 06, got %s." % (label, hex_bytes(r)))

    async def enter_programming(self):
        self.queue.clear()
        self.read_mode = "auto"
        await self.send(MAGIC)
        await self.expect_ack("Handshake")
        await self.send(bytes([0x02]))
        first = (await self.read_exact(1))[0]
        for _ in range(4):
            if first != 0x06:
                break
            first = (await self.read_exact(1))[0]
        ident = bytes([first]) + await self.read_exact(7)
        if ident[:7] != bytes([0x54, 0x44, 0x48, 0x39, 0xFF, 0xFF, 0xFF]):
            raise RuntimeError("Unexpected identity: %s" % hex_bytes(ident))
        await self.send(bytes([0x06]))
        await self.expect_ack("Enter programming")
        return ident

    async def leave_programming(self):
        try:
            await self.send(bytes([0x45]))
        except Exception:
            pass
        await asyncio.sleep(0.3)
        self.queue.clear()

    async def read_block(self, address, timeout=3.0):
        cmd = bytes([0x52, (address >> 8) & 255, address & 255, BLOCK])
        await self.send(cmd)
        expected = bytes([0x57, (address >> 8) & 255, address & 255, BLOCK])
        header = await self.read_exact(4, timeout)
        if header != expected:
            if self.read_mode != "yes":
                nxt = await self.read_exact(1, timeout)
                shifted = bytes([header[1], header[2], header[3], nxt[0]])
                if shifted == expected:
                    header = shifted
                    self.read_mode = "yes"
        if header != expected:
            raise RuntimeError("Invalid read header at 0x%04x: %s" % (address, hex_bytes(header)))
        data = await self.read_exact(BLOCK, timeout)
        mode = self.read_mode
        if mode == "yes":
            await self.read_exact(1, timeout)
        elif mode == "auto":
            try:
                await self.read_exact(1, timeout=0.15)
                mode = "yes"
            except TimeoutError:
                mode = "no"
            self.read_mode = mode
        return data

    async def select_bank(self, bank):
        block = await self.read_block(STATUS_BASE + bank * BLOCK)
        if block[:4] != b"H9RS":
            raise RuntimeError("No resource service.")
        st = {"version": block[4], "ready": block[8] == 1}
        # S2: bytes 12-19 carry write status (result, programming, pointer, crc)
        if st["version"] == 2:
            st["write"] = {
                "result": block[12],
                "resultText": self._result_text(block[12]),
                "programming": block[13] == 2,
                "pointer": block[14] | (block[15] << 8) | (block[16] << 16),
                "operations": block[17],
                "crc": block[18] | (block[19] << 8),
            }
        return st

    @staticmethod
    def _result_text(code):
        return {
            0: "ok", 1: "bad magic", 2: "flash not ready", 3: "partition refused",
            4: "misaligned", 5: "sector CRC mismatch", 6: "erase verify failed",
            7: "not armed", 8: "bad W checksum", 9: "out of range",
            10: "target not erased", 11: "program verify failed", 12: "bad op",
            0xfe: "no command since boot",
        }.get(code, "unknown %d" % code)

    async def write_status(self, address):
        st = await self.select_bank(address // BANK_SIZE)
        if st["version"] != 2:
            raise RuntimeError("No write service v2.")
        if not st["ready"]:
            raise RuntimeError("Flash not ready.")
        return st

    async def expect_write(self, address, label):
        st = await self.write_status(address)
        w = st.get("write")
        if w and w["result"] != 0:
            raise RuntimeError("%s @0x%x: radio reports %s (pointer 0x%x, crc 0x%x)"
                               % (label, address, w["resultText"], w["pointer"], w["crc"]))
        return st

    # -- reads ------------------------------------------------------------
    async def send_write(self, frame):
        """Send a W (write) frame and await the 0x06 ACK.
        Отправляет W-кадр (запись) и ждёт ACK 0x06 — иначе ACK остаётся в
        очереди и ломает следующий read (лишний 06 в header)."""
        await self.send(frame)
        await self.expect_ack("Write/запись")

    async def read_fixed(self, start, length, on_progress=None):
        out = bytearray()
        pos = start
        end = start + length
        bank = -1
        retries = 0
        while pos < end:
            b = pos // BANK_SIZE
            if b != bank:
                st = await self.select_bank(b)
                if not st["ready"]:
                    raise RuntimeError("Flash not ready bank %d" % b)
                bank = b
            try:
                out += await self.read_block(DATA_BASE + (pos % BANK_SIZE))
            except TimeoutError:
                retries += 1
                if retries > 5:
                    raise
                # re-sync on the current bank and retry the block
                st = await self.select_bank(b)
                if not st["ready"]:
                    raise RuntimeError("Flash not ready bank %d" % b)
                continue
            retries = 0
            pos += BLOCK
            if on_progress and ((pos - start) % (BLOCK * 16) == 0 or pos >= end):
                on_progress(min(pos - start, length), length)
        return bytes(out[:length])

    # ---- bulk fast read (S1 fast; fails fast if unsupported) --------------
    # Порт readFast из web radio.mjs: выбираем банк, ставим курсор, просим
    # фиксированный 15 мс линк, затем bulk-слоты стримят пачками до 240 Б.
    def _reset_bulk(self):
        self.bulk_partial = None
        self.bulk_packets = []
        self.bulk_active = False

    async def read_bulk_chunk(self, size_index):
        """Request one bulk slot; returns (reply, data). Radia streams pакеты
        (B7 5A seq len ...) before the 32-byte bulk reply."""
        self.bulk_packets = []
        self.bulk_partial = None
        self.bulk_active = True
        block = await self.read_block(bulk_slot_address(size_index, False))
        reply = parse_bulk_reply(block)
        if reply is None:
            self.bulk_active = False
            raise RuntimeError("No bulk reply (radio without S1 fast?)")
        # collect any queued packets already in `self.queue`
        packets = list(self.bulk_packets)
        self.bulk_active = False
        return reply, packets

    async def read_fast(self, start, length, on_progress=None):
        """Bulk fast read from a 32 KiB-aligned start; falls back to read_fixed."""
        if start % BANK_SIZE:
            raise RuntimeError("bulk read needs 32 KiB-aligned start")
        out = bytearray()
        pos = start
        end = start + length
        # link fixed 15ms
        self._reset_bulk()
        await self.select_bank(pos // BANK_SIZE)
        link = parse_bulk_reply(await self.read_block(LINK_FIXED_ADDRESS))
        link_ok = bool(link and link["status"] == 0)
        if not link_ok:
            # Not a bulk-capable image -> fall back to slow read
            self._reset_bulk()
            return await self.read_fixed(start, length, on_progress)
        await asyncio.sleep(1.5)  # let the radio settle the 15 ms link
        size_index = BULK_SIZES.index(176) if 176 in BULK_SIZES else 2
        try:
            while pos < end:
                reply, pkts = await self.read_bulk_chunk(size_index)
                if reply["status"] != 0 or reply["count"] == 0:
                    # back off size or treat as end
                    raise RuntimeError("bulk read status %d" % reply["status"])
                data = assemble_bulk(pkts, reply, BULK_SIZES[size_index])
                take = min(len(data), end - pos)
                out += data[:take]
                pos += len(data)
                if on_progress:
                    on_progress(min(len(out), length), length)
        finally:
            self._reset_bulk()
            # restore stock link
            try:
                await self.read_block(LINK_STOCK_ADDRESS)
            except Exception:
                pass
        return bytes(out[:length])

    async def read_any(self, start, length, on_progress=None):
        """Use bulk fast read when possible, otherwise slow 32-byte read.
        Bulk needs a 32 KiB-aligned start, so read a little extra and slice.
        / Быстрое чтение: bulk требует выравнивания по 32 КиБ, читаем с
        запасом и обрезаем до нужного среза."""
        lead = start % BANK_SIZE
        base = start - lead
        # читать от выровненного base, захватывая lead..lead+length
        if lead == 0 and length >= BANK_SIZE:
            return await self.read_fast(start, length, on_progress)
        if length + lead >= BANK_SIZE:
            try:
                raw = await self.read_fast(base, length + lead, on_progress)
                return raw[lead:lead + length]
            except RuntimeError:
                pass
        return await self.read_fixed(start, length, on_progress)


def data_frame(payload):
    chk = sum(payload) & 255
    return bytes([0x57, DATA_ADDRESS >> 8, DATA_ADDRESS & 255, BLOCK]) + payload + bytes([chk])


def control_frame(op, chip=0, crc=0):
    payload = bytearray(BLOCK)
    payload[0:10] = bytes([0x48, 0x39, 0x57, 0x52, op,
                          chip & 255, (chip >> 8) & 255, (chip >> 16) & 255,
                          crc & 255, crc >> 8])
    return bytes([0x57, CONTROL_ADDRESS >> 8, CONTROL_ADDRESS & 255, BLOCK]) + bytes(payload) + bytes([(sum(payload) & 255)])


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


# ============================================================================
# Pack build / parse
# ============================================================================
def build_pack(images):
    """images: list of bytes (each exactly IMG_BYTES). Returns pack bytes."""
    if len(images) < 1 or len(images) > 255:
        raise RuntimeError("1..255 images expected, got %d" % len(images))
    for im in images:
        if len(im) != IMG_BYTES:
            raise RuntimeError("Each image must be %d bytes (got %d)" % (IMG_BYTES, len(im)))
    pack = bytearray()
    pack += KPI_MAGIC                 # 7
    pack += bytes([len(images)])      # 1  count
    imagesize = KPI_HEADER + len(images) * IMG_BYTES
    if imagesize > (DEFAULT_END - DEFAULT_START):
        raise RuntimeError("Pack too big for the free area.")
    for im in images:
        pack += im
    return bytes(pack)


def load_image(path):
    """Load one image: raw .bin RGB565, or png/jpg via Pillow -> RGB565."""
    if path.lower().endswith(".bin"):
        with open(path, "rb") as f:
            data = f.read()
        if len(data) != IMG_BYTES:
            raise RuntimeError("%s: raw RGB565 must be %d bytes (got %d)" % (path, IMG_BYTES, len(data)))
        return data
    # else: Pillow -> RGB565 big-endian
    from PIL import Image
    img = Image.open(path).convert("RGB").resize((IMG_W, IMG_H), Image.LANCZOS)
    out = bytearray()
    for py in range(IMG_H):
        for px in range(IMG_W):
            r, g, b = img.getpixel((px, py))
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += bytes([v >> 8, v & 255])
    return bytes(out)


# ============================================================================
# S2 write (erase->program->verify), ported from td9_logo.py write_logo
# ============================================================================
async def write_range(radio, start, final_bytes, on_progress=None):
    """final_bytes must be whole sectors starting at `start`. Erase changed
    sectors and program final_bytes. Verify by read-back."""
    length = len(final_bytes)
    if length % SECTOR:
        raise RuntimeError("final_bytes must fill whole 4 KiB sectors")
    await radio.enter_programming()
    try:
        n = 0
        for off in range(0, length, SECTOR):
            sector = final_bytes[off:off + SECTOR]
            addr = start + off
            print("  Sector @ 0x%05x: erase..." % addr)
            # erase требует CRC текущего содержимого сектора, иначе
            # результат "sector CRC mismatch". Читаем сектор и считаем CRC.
            prev = await radio.read_fixed(addr, SECTOR)
            old = crc16(prev)
            await radio.send_write(control_frame(OP["erase"], addr, old))
            await radio.expect_write(addr, "erase")
            if all(x == ERASED for x in sector):
                pass  # erase target blank: leave erased
            else:
                await radio.send_write(control_frame(OP["program"], addr))
                await radio.expect_write(addr, "begin program")
                for o in range(0, SECTOR, BLOCK):
                    await radio.send_write(data_frame(sector[o:o + BLOCK]))
                    if (o // BLOCK) % CHECK_EVERY == CHECK_EVERY - 1:
                        await radio.expect_write(addr, "program")
                n += 1
            if on_progress:
                on_progress(off + SECTOR, length)
    finally:
        await radio.leave_programming()
    # verify (needs its own programming session; read_any/bulk require it)
    print("  Verifying...")
    await radio.enter_programming()
    try:
        check = await radio.read_any(start, length)
    finally:
        await radio.leave_programming()
    if check != final_bytes:
        d = sum(1 for a, b in zip(check, final_bytes) if a != b)
        raise RuntimeError("Read-back differs in %d bytes." % d)
    return n


async def erase_sector(radio, addr):
    """Erase one 4 KiB sector, verifying the radio accepted it (CRC + result0).
    / Стирает один сектор: читает текущий CRC, посылает erase с CRC, ждёт ok."""
    prev = await radio.read_any(addr, SECTOR)
    old = crc16(prev)
    await radio.send_write(control_frame(OP["erase"], addr, old))
    await radio.expect_write(addr, "erase @0x%x" % addr)


# ============================================================================
# CLI
# ============================================================================
def make_progress(label=""):
    def _p(done, total):
        pct = done * 100 // total if total else 0
        sys.stdout.write("\r  %s%d/%d (%d%%)" % (label, done, total, pct))
        sys.stdout.flush()
        if done >= total:
            sys.stdout.write("\n")
    return _p


async def connect_radio(timeout=8.0):
    from bleak import BleakScanner, BleakClient
    print("Scanning for TD-H9... / Сканирую TD-H9...")
    devices = await BleakScanner.discover(timeout=timeout)
    dev = next((d for d in devices if (d.name or "").strip().startswith("TD-H9")), None)
    if dev is None:
        raise RuntimeError("TD-H9 not found / рация не найдена")
    print("Connecting to %s (%s)..." % (dev.name, dev.address))
    client = BleakClient(dev.address)
    await client.connect()
    write_char = client.services.get_characteristic(CHAR_WRITE)
    radio = Radio(client, write_char)
    await client.start_notify(CHAR_NOTIFY, lambda _c, d: radio.receive(bytes(d)))
    return client, radio


async def async_main(args):
    client = None
    try:
        client, radio = await connect_radio(timeout=args.scan_timeout)
        await radio.enter_programming()
        try:
            st = await radio.select_bank(0)
            sv = st["version"]
        finally:
            await radio.leave_programming()
        print("Resource service v%d, ready=%s" % (sv, st["ready"]))
        if args.command == "check":
            start = int(args.addr, 0)
            span = int(args.span, 0)
            print("Checking %s..%s for free space..." % (hex(start), hex(start + span)))
            await radio.enter_programming()
            try:
                data = await radio.read_any(start, span, make_progress("check "))
            finally:
                await radio.leave_programming()
            free = sum(1 for b in data if b == ERASED)
            print("\nFree bytes: %d / %d (%.1f%%)" % (free, span, free * 100.0 / span))
            blocked = span - free
            if blocked == 0:
                print("Area is fully free (0xFF). Safe to write. / Область свободна.")
            else:
                print("Area has %d non-0xFF bytes; not fully free." % blocked)
                # Распознать наш пакет DKXCEPIC
                if data[:7] == KPI_MAGIC:
                    count = data[7]
                    psize = KPI_HEADER + count * IMG_BYTES
                    print("Found a DKXCEPIC picture pack here: %d image(s), %d bytes "
                          "(our data)." % (count, psize))
                    if count > 0 and len(data) >= psize:
                        tail_free = sum(1 for b in data[psize:span] if b == ERASED)
                        print("Pack occupies the first %d bytes; %d byte(s) after it are free."
                              % (psize, tail_free))
                    print("To overwrite use: write / forcewrite. To clear: erase.")
                elif blocked > 0:
                    print("Not a DKXCEPIC pack; leftover/other data. forcewrite will erase it.")
            return 0

        elif args.command in ("write", "forcewrite"):
            force = args.command == "forcewrite"
            start = int(args.addr, 0)
            imgs = [load_image(p) for p in args.images]
            pack = build_pack(imgs)
            npad = ((len(pack) + SECTOR - 1) // SECTOR) * SECTOR
            final = pack + bytes([ERASED]) * (npad - len(pack))
            print("Pack: %d image(s), %d bytes (total aligned %d at %s)"
                  % (len(imgs), len(pack), npad, hex(start)))
            if force:
                print("forcewrite: erasing the area unconditionally / стираю область принудительно.")
            else:
                # verify free first / проверка свободности
                await radio.enter_programming()
                try:
                    data = await radio.read_any(start, npad, make_progress("check "))
                finally:
                    await radio.leave_programming()
                if data == final:
                    print("Already written identically / уже записано идентично. Nothing to do.")
                    return 0
                # Область занята прошлым пакетом — стереть сектора (1→0 требует erase).
                if any(b != ERASED for b in data):
                    bad = sum(1 for b in data if b != ERASED)
                    print("Area holds %d non-0xFF bytes; erasing before rewrite / "
                          "область занята, стираю перед перезаписью." % bad)
                    force = True
            if force:
                await radio.enter_programming()
                try:
                    for i, off in enumerate(range(0, npad, SECTOR)):
                        await erase_sector(radio, start + off)
                        make_progress("erase ")((i + 1) * SECTOR, npad)
                finally:
                    await radio.leave_programming()
                print()
            print("\nTarget area ready. Writing...")
            n = await write_range(radio, start, final, make_progress("write"))
            print("Done: %d sector(s) written and verified at %s." % (n, hex(start)))
            print("Install the DKXCEPIC app to view them. / Установите приложение DKXCEPIC, чтобы просматривать.")
            return 0

        elif args.command == "erase":
            long_start = int(args.addr, 0)
            # Вычислить размер пакета из каталога, если он есть; иначе --span.
            await radio.enter_programming()
            try:
                head = await radio.read_fixed(long_start, 8)
            finally:
                await radio.leave_programming()
            if head[:7] == KPI_MAGIC:
                count = head[7]
                nbytes = KPI_HEADER + count * IMG_BYTES
                span = ((nbytes + SECTOR - 1) // SECTOR) * SECTOR
                print("Found DKXCEPIC pack: %d image(s), %d bytes / %d sector(s) to erase."
                      % (count, nbytes, span // SECTOR))
            else:
                span = int(args.span, 0) if int(args.span, 0) > 0 else SECTOR
                print("No DKXCEPIC header (starts %s); erasing default %d bytes."
                      % (hex_bytes(head), span))
            await radio.enter_programming()
            try:
                nsec = span // SECTOR
                for i, off in enumerate(range(0, span, SECTOR)):
                    await erase_sector(radio, long_start + off)
                    make_progress("erase ")((i + 1) * SECTOR, span)
            finally:
                await radio.leave_programming()
            print("\nErased %d sector(s) at %s..%s / стёрто." % (span // SECTOR, hex(long_start), hex(long_start + span)))
            return 0

        elif args.command in ("pass", "passclear", "passread"):
            # Radio lock password and radio lock reminder
            if args.command == "passread":
                await radio.enter_programming()
                try:
                    cur = await radio.read_any(PASS_ADDR, PASS_LEN)
                finally:
                    await radio.leave_programming()
                print("Lock password @%s: %s" % (hex(PASS_ADDR),
                      ' '.join('%02X' % b for b in cur)))
                ascii_str = ''.join(chr(b) for b in cur if 32 <= b < 127)
                print("  ASCII: \"%s\"" % ascii_str)
                if all((b >= ord('0') and b <= ord('9')) or b == 0x20 for b in cur) and \
                   any(b >= ord('0') and b <= ord('9') for b in cur):
                    print("  -> LOCKED (digits/spaces only, at least one digit).")
                elif all(b == ERASED for b in cur):
                    print("  -> not locked (all 0xFF).")
                else:
                    print("  -> not locked (non-digit/non-space byte present).")
                return 0
            # pass / passclear write the 8-byte record at PASS_ADDR.
            # Format: up to 8 digits; shorter passwords pad with spaces (0x20).
            # Cleared (no password): the record is all 0xFF (erased).
            pw_bytes = None
            if args.command == "pass":
                if isinstance(args.password, str) and args.password:
                    pw = args.password.strip()
                    if not pw.isdigit() or len(pw) > PASS_LEN:
                        raise RuntimeError("password must be 1-%d digits" % PASS_LEN)
                    raw = pw.encode("ascii")
                    pw_bytes = raw + bytes([0x20]) * (PASS_LEN - len(raw))
                else:
                    raise RuntimeError("pass requires --password (1-8 digits)")
            else:  # passclear
                pw_bytes = bytes([ERASED]) * PASS_LEN
            # Program ONLY into erased cells: the lock record shares a 4 KiB
            # sector with the font glyphs, so erasing the sector would destroy
            # the font. We require every target byte to be 0xFF before
            # programming (busy rule). S2 program writes 32-byte data frames at
            # the write pointer (DATA_ADDRESS); use data_frame + a begin program
            # on the PASS_ADDR sector, but never the erase operation.
            await radio.enter_programming()
            try:
                cur = await radio.read_any(PASS_ADDR, PASS_LEN)
                if any(b != ERASED for b in cur):
                    raise RuntimeError("lock record not erased (0xFF); refusing to overwrite "
                                       "without erasing the font sector")
                # Align: program a 32-byte block covering PASS_ADDR.
                blk = PASS_ADDR & ~(BLOCK - 1)
                blk_off = PASS_ADDR - blk
                curblk = bytearray(await radio.read_any(blk, BLOCK))
                curblk[blk_off:blk_off + PASS_LEN] = pw_bytes
                await radio.send_write(control_frame(OP["program"], blk))
                await radio.expect_write(blk, "begin program")
                await radio.send_write(data_frame(bytes(curblk)))
                await radio.expect_write(blk, "program")
            finally:
                await radio.leave_programming()
            # verify
            await radio.enter_programming()
            try:
                chk = await radio.read_any(PASS_ADDR, PASS_LEN)
            finally:
                await radio.leave_programming()
            if chk != pw_bytes:
                raise RuntimeError("read-back mismatch: %s" % ' '.join('%02X' % b for b in chk))
            print("Lock password %s at %s: %s" % (
                "set" if args.command == "pass" else "cleared", hex(PASS_ADDR),
                ' '.join('%02X' % b for b in pw_bytes)))
            return 0

        elif args.command == "dump":
            start = int(args.addr, 0)
            span = int(args.span, 0)
            await radio.enter_programming()
            try:
                data = await radio.read_fixed(start, span, make_progress("dump "))
            finally:
                await radio.leave_programming()
            with open(args.out, "wb") as f:
                f.write(data)
            print("\nDumped %d bytes to %s (%s..%s)" % (len(data), args.out, hex(start), hex(start + span)))
            return 0
    finally:
        if client:
            try:
                await client.disconnect()
            except Exception:
                pass
    return 0


def main():
    ap = argparse.ArgumentParser(description="TD-H9 picture pack writer / Пакет картинок TD-H9 по Bluetooth")
    ap.add_argument("command", choices=["check", "write", "forcewrite", "erase", "dump", "pass", "passclear", "passread"])
    ap.add_argument("images", nargs="*", help="image file(s) for 'write'/'forcewrite' (raw .bin RGB565, or png/jpg)")
    ap.add_argument("--addr", default=hex(DEFAULT_START), help="start address of the free area (default 0x717000)")
    ap.add_argument("--span", default="0x1000", help="span bytes for check/dump (default 4 KiB = one sector; writing checks its exact pack size)")
    ap.add_argument("--out", default="kpi-dump.bin", help="output file for 'dump'")
    ap.add_argument("--password", default=None, help="lock password (1-8 digits) for 'pass'")
    ap.add_argument("--scan-timeout", type=int, default=8)
    args = ap.parse_args()
    if args.command in ("write", "forcewrite") and not args.images:
        ap.error("'%s' requires at least one image file" % args.command)
    try:
        return asyncio.run(async_main(args))
    except KeyboardInterrupt:
        print("\nInterrupted / Прервано.")
        return 130
    except Exception as e:
        print("\nError / Ошибка: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())