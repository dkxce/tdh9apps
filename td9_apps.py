#!/usr/bin/env python3
"""
td9_apps.py -- Install/remove H9 Toolbox apps on the TD-H9 over Bluetooth (BLE).

A Python port of dist/apps (App Manager) + dist/doom/upload.mjs + dist/boot-logo
(resource S1/S2 service) from https://github.com/melidosian/h9toolbox.

What it does / Что делает:
  - bundles the chosen .h9mod apps (plus the MENU, automatic) into one
    H9MODS01 pack;
  - writes that pack into the modules slot of the 16 MiB resource flash
    (directory 0xCC0000, data 0xCC1000..0xCFFFFF) through the S2 write
    service (same write UART ff02 as the settings tool);
  - reads back and verifies before marking the upload COMPLETE, exactly like
    the App Manager page.

Requires H9 Toolbox Firmware on the radio (S2 write service) or *dkxce_passlock.fw
Требует прошивку H9 Toolbox Firmware (S2 write service) или *dkxce_passlock.fw

Usage / Использование:
  python td9_apps.py list                                  -- show apps in ./apps_catalog
  python td9_apps.py install gps                           -- install GPS Info (+MENU auto)
  python td9_apps.py install gps aprs                      -- install several apps
  python td9_apps.py peek                                  -- read current modules slot dir
  python td9_apps.py remove                                -- remove all apps (erase slot)

Dependencies / Зависимости: pip install bleak
Radio on + Bluetooth programming mode.
"""

import argparse
import asyncio
import os
import struct
import sys

# ============================================================================
# BLE service / characteristics (same clone service FF00 as the settings tool)
# ============================================================================
SERVICE_FF00 = "0000ff00-0000-1000-8000-00805f9b34fb"
CHAR_WRITE = "0000ff02-0000-1000-8000-00805f9b34fb"
CHAR_NOTIFY = "0000ff01-0000-1000-8000-00805f9b34fb"

MAGIC = bytes([0x50, 0x56, 0x4F, 0x4A, 0x48, 0x5C, 0x14])
IDENTITY = bytes([0x54, 0x44, 0x48, 0x39, 0xFF, 0xFF, 0xFF])

# Resource flash service geometry (dist/boot-logo/resource.mjs)
CHIP_SIZE = 0x1000000
BANK_SIZE = 0x8000
BLOCK = 0x20
SECTOR = 0x1000
STATUS_BASE = 0x4000          # status block for a bank
DATA_BASE = 0x8000            # data window within a bank
CONTROL_ADDRESS = 0x4000      # S2 control slot
DATA_ADDRESS = 0x4020         # S2 data slot
OP_DISARM = 0
OP_ERASE = 1
OP_PROGRAM = 2

# Modules slot (resource chip)
MODULES_DIRECTORY = 0xCC0000
MODULES_DATA = 0xCC1000
MUSIC_DIRECTORY = 0xCE0000
MODULES_SLOT_MAX = MUSIC_DIRECTORY - MODULES_DATA   # 0x1F000 = 126976

RESULTS = {
    0: 'ok', 1: 'bad magic', 2: 'flash not ready', 3: 'partition refused',
    4: 'misaligned', 5: 'sector CRC mismatch', 6: 'erase verify failed',
    7: 'not armed', 8: 'bad W checksum', 9: 'out of range',
    10: 'target not erased', 11: 'program verify failed', 12: 'bad op',
    0xfe: 'no command since boot',
}

ERASED_CRC = 0xE03E
CHECK_EVERY = 16  # status refresh each 16 blocks (SECTOR/BLOCK = 128)

# H9MODS01 bundle constants (dist/apps/bundle.mjs)
BUNDLE_MAGIC = b'H9MODS01'
BUNDLE_VERSION = 1
RAM_SLOT = 0x2000

# H9DATA01 directory constants (dist/doom/upload.mjs)
DIR_MAGIC = b'H9DATA01'
COMPLETE = b'H9DATA-COMPLETE'
NAME_BYTES = 32


def hex_bytes(b):
    return " ".join("%02x" % x for x in b)


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


def is_erased(b):
    return all(x == 0xFF for x in b)


# ============================================================================
# Bundle (H9MODS01) build / parse  -- port of bundle.mjs
# ============================================================================
def build_bundle(modules):
    """modules: list of {'name','body','bss','entry'}. First == MENU."""
    if len(modules) < 1 or len(modules) > 255:
        raise RuntimeError("A bundle holds 1-255 modules.")
    for m in modules:
        if len(m['body']) + m['bss'] > RAM_SLOT:
            raise RuntimeError("%s needs too much RAM." % m['name'])
        if not (0 <= m['entry'] < len(m['body'])):
            raise RuntimeError("%s entry outside body." % m['name'])
    # layout
    offset = 16 + 32 * len(modules)
    offsets = []
    for m in modules:
        offset += (-offset) & 3
        offsets.append(offset)
        offset += len(m['body'])
    if offset > MODULES_SLOT_MAX:
        raise RuntimeError("Bundle (%d B) larger than modules slot." % offset)
    out = bytearray(offset)
    out[:] = bytes([0xFF]) * offset
    out[0:8] = BUNDLE_MAGIC
    struct.pack_into('<H', out, 8, BUNDLE_VERSION)
    struct.pack_into('<H', out, 10, len(modules))
    struct.pack_into('<I', out, 12, offset)
    for i, m in enumerate(modules):
        e = 16 + 32 * i
        out[e:e + 16] = m['name'].encode('latin1')[:16].ljust(16, b'\x00')
        struct.pack_into('<I', out, e + 16, offsets[i])
        struct.pack_into('<I', out, e + 20, len(m['body']))
        struct.pack_into('<I', out, e + 24, m['bss'])
        struct.pack_into('<H', out, e + 28, m['entry'])
        struct.pack_into('<H', out, e + 30, crc16(m['body']))
        out[offsets[i]:offsets[i] + len(m['body'])] = m['body']
    return bytes(out)


def _read_h9mod(path):
    b = open(path, 'rb').read()
    if b[:8] != BUNDLE_MAGIC:
        raise RuntimeError("%s: not an H9MODS01 bundle." % path)
    count = struct.unpack_from('<H', b, 10)[0]
    modules = []
    for i in range(count):
        e = 16 + 32 * i
        name = b[e:e + 16].split(b'\x00')[0].decode('latin1')
        off, size, bss = struct.unpack_from('<III', b, e + 16)
        entry, crc = struct.unpack_from('<HH', b, e + 28)
        body = b[off:off + size]
        if crc16(body) != crc:
            raise RuntimeError("%s: module %s fails CRC." % (path, name))
        modules.append({'name': name, 'body': body, 'bss': bss, 'entry': entry, 'crc': crc})
    return modules


# ============================================================================
# S2 resource service helpers -- port of resource.mjs
# ============================================================================
def status_address(bank):
    return STATUS_BASE + bank * BLOCK


def data_address(chip):
    return DATA_BASE + (chip % BANK_SIZE)


def write_frame(address, payload):
    if len(payload) != BLOCK:
        raise RuntimeError("W payload must be 32 bytes.")
    chk = sum(payload) & 255
    return bytes([0x57, address >> 8 & 255, address & 255, BLOCK]) + payload + bytes([chk])


def control_frame(op, chip=0, crc=0):
    payload = bytearray(BLOCK)
    payload[0:10] = bytes([0x48, 0x39, 0x57, 0x52, op,
                           chip & 255, (chip >> 8) & 255, (chip >> 16) & 255,
                           crc & 255, crc >> 8])
    return write_frame(CONTROL_ADDRESS, bytes(payload))


def data_frame(payload):
    return write_frame(DATA_ADDRESS, payload)


def parse_status(block, bank):
    if block[:4] != b'H9RS':
        raise RuntimeError("No resource service (got %s)." % hex_bytes(block[:4]))
    if block[4] not in (1, 2):
        raise RuntimeError("Unsupported service version %d." % block[4])
    if block[11] != 0x5A:
        raise RuntimeError("Status block trailer wrong.")
    echoed = block[9] | (block[10] << 8)
    if echoed != bank:
        raise RuntimeError("Radio selected bank %d, expected %d." % (echoed, bank))
    status = {'version': block[4], 'ready': block[8] == 1, 'jedec': bytes(block[5:8]), 'bank': echoed}
    if block[4] == 2:
        status['write'] = {
            'result': block[12],
            'resultText': RESULTS.get(block[12], "unknown %d" % block[12]),
            'programming': block[13] == 2,
            'pointer': block[14] | (block[15] << 8) | (block[16] << 16),
            'operations': block[17],
            'crc': block[18] | (block[19] << 8),
        }
    return status


# ============================================================================
# Radio: BLE transport + S1/S2 commands  -- port of radio.mjs
# ============================================================================
class Radio:
    def __init__(self, client, write_char):
        self.client = client
        self.write_char = write_char
        self.queue = bytearray()
        self.wake = None
        self.read_mode = "auto"
        self.log = lambda *a: None

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
        reply = await self.read_exact(1)
        if reply[0] != 0x06:
            raise RuntimeError("%s: expected ACK 06, got %s." % (label, hex_bytes(reply)))

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
        if not IDENTITY == ident[:7] or ident[7] not in (0x4E, 0x48, 0x47):
            raise RuntimeError("Unexpected TD-H9 identity: %s." % hex_bytes(ident))
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

    async def read_block(self, address, timeout=2.5):
        """Read one 32-byte block via the clone 0x52 command."""
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
            raise RuntimeError("Invalid read header at 0x%04x: %s"
                               % (address, hex_bytes(header)))
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

    # -- S1/S2 service -------------------------------------------------
    async def select_bank(self, bank):
        return parse_status(await self.read_block(status_address(bank)), bank)

    async def check_service(self):
        await self.enter_programming()
        try:
            return await self.select_bank(0)
        finally:
            await self.leave_programming()

    async def write_status(self, address):
        state = await self.select_bank(address // BANK_SIZE)
        if state['version'] != 2:
            raise RuntimeError("No write service (version %d)." % state['version'])
        if not state['ready']:
            raise RuntimeError("Flash not ready.")
        return state['write']

    async def expect_write(self, address, label, check=None):
        state = await self.write_status(address)
        if state['result'] != 0 or (check and not check(state)):
            text = state['resultText']
            raise RuntimeError("%s: radio reports %s (pointer 0x%x, CRC 0x%x)."
                               % (label, text, state['pointer'], state['crc']))
        return state

    async def send_write(self, frame):
        await self.send(frame)
        await self.expect_ack("Write")

    # Read a contiguous aligned range via slow 32-byte reads (works on any S1).
    async def read_range_slow(self, start, length, on_progress=None):
        if start % BLOCK or length % BLOCK:
            raise RuntimeError("read_range needs 32-byte alignment.")
        out = bytearray(length)
        bank = -1
        pos = start
        end = start + length
        steps_done = 0
        blocks = length // BLOCK
        while pos < end:
            b = pos // BANK_SIZE
            if b != bank:
                st = await self.select_bank(b)
                if not st['ready']:
                    raise RuntimeError("Flash stopped reporting ready.")
                bank = b
            out[pos - start: pos - start + BLOCK] = await self.read_block(data_address(pos))
            pos += BLOCK
            steps_done += 1
            if on_progress and (steps_done % 16 == 0 or steps_done == blocks):
                on_progress(steps_done / blocks)
        return bytes(out)


# ============================================================================
# Directory (H9DATA01) build/parse -- port of upload.mjs buildDirectory/parseDirectory
# ============================================================================
def build_directory(pack, name, directory=MODULES_DIRECTORY, data_start=MODULES_DATA,
                    data_end=MUSIC_DIRECTORY):
    length = len(pack)
    if not (0 < length <= data_end - data_start):
        raise RuntimeError("Pack does not fit the modules slot.")
    segments = [{'address': data_start, 'length': length}]
    sector = bytearray(SECTOR)
    for i in range(SECTOR):
        sector[i] = 0xFF
    sector[0:8] = DIR_MAGIC
    struct.pack_into('<I', sector, 8, 1)
    struct.pack_into('<I', sector, 12, length)
    # sha256
    import hashlib
    struct.pack_into('<32s', sector, 16, hashlib.sha256(pack).digest())
    label = name.encode('latin1')[:NAME_BYTES].ljust(NAME_BYTES, b'\x00')
    sector[48:48 + NAME_BYTES] = label
    struct.pack_into('<I', sector, 80, len(segments))
    seg_off = 84
    for i, s in enumerate(segments):
        struct.pack_into('<I', sector, seg_off + 8 * i, s['address'])
        struct.pack_into('<I', sector, seg_off + 8 * i + 4, s['length'])
    end = seg_off + 8 * len(segments)
    struct.pack_into('<H', sector, end, crc16(bytes(sector[:end])))
    return bytes(sector), segments


def parse_directory(sector):
    if sector[:8] != DIR_MAGIC:
        return None
    count = struct.unpack_from('<I', sector, 80)[0]
    if count < 1 or count > 7:
        return None
    end = 84 + 8 * count
    if struct.unpack_from('<H', sector, end)[0] != crc16(bytes(sector[:end])):
        return None
    segments = []
    for i in range(count):
        addr = struct.unpack_from('<I', sector, 84 + 8 * i)[0]
        length = struct.unpack_from('<I', sector, 84 + 8 * i + 4)[0]
        segments.append({'address': addr, 'length': length})
    if not all(MODULES_DIRECTORY + 0x1000 <= s['address'] and
               s['address'] + s['length'] <= MUSIC_DIRECTORY for s in segments):
        return None
    name = sector[48:80].split(b'\x00')[0].decode('latin1', 'replace')
    compl = bytes(sector[SECTOR - BLOCK: SECTOR - BLOCK + len(COMPLETE)]) == COMPLETE
    return {'length': struct.unpack_from('<I', sector, 12)[0],
            'sha256': bytes(sector[16:48]), 'name': name,
            'segments': segments, 'complete': compl}


# ============================================================================
# Upload -- port of upload.mjs uploadPack / writePlan / restore
# ============================================================================
class AppRadioshell:
    def __init__(self, radio):
        self.radio = radio

    async def read_directory(self):
        if hasattr(self.radio, 'read_range_slow'):
            sector = await self.radio.read_range_slow(MODULES_DIRECTORY, SECTOR)
        else:
            sector = b'\xff' * SECTOR
        return sector

    async def read_ranges(self, ranges, on_progress=None):
        """ranges: list of {'address','length'} (already padded). Returns list of bytes."""
        out = []
        total = sum(r['length'] for r in ranges)
        done = 0 if total else 1
        for r in ranges:
            if hasattr(self.radio, 'read_range_slow'):
                data = await self.radio.read_range_slow(r['address'], r['length'],
                                                        lambda f: on_progress((done + f * r['length']) / total) if on_progress else None)
            else:
                data = b'\xff' * r['length']
            out.append(data)
            done += r['length']
        if on_progress and total:
            on_progress(1.0)
        return out

    def pad(self, data):
        out = bytearray(data)
        if len(out) % SECTOR:
            out += bytes([0xFF]) * (SECTOR - len(out) % SECTOR)
        return bytes(out)

    def pad_len(self, n):
        return n if n % SECTOR == 0 else (n // SECTOR + 1) * SECTOR

    def sector_plan(self, address, current, next_):
        plan = []
        for off in range(0, len(current), SECTOR):
            old = current[off:off + SECTOR]
            fresh = next_[off:off + SECTOR]
            if old == fresh:
                continue
            erase = not is_erased(old)
            plan.append({'address': address + off, 'offset': off,
                         'erase': erase, 'oldCrc': crc16(old),
                         'program': not is_erased(fresh)})
        return plan

    async def write_plan(self, plan, next_):
        programmed = 0
        for entry in plan:
            addr = entry['address']
            label = "0x%x" % addr
            if entry['erase']:
                await self.radio.send_write(control_frame(OP_ERASE, addr, entry['oldCrc']))
                await self.radio.expect_write(addr, "Erase %s" % label,
                                              lambda s: s['crc'] == ERASED_CRC)
            if entry['program']:
                data = next_[entry['offset']: entry['offset'] + SECTOR]
                await self.radio.send_write(control_frame(OP_PROGRAM, addr))
                await self.radio.expect_write(addr, "Begin %s" % label,
                                              lambda s: s['programming'] and s['pointer'] == addr)
                try:
                    for off in range(0, SECTOR, BLOCK):
                        await self.radio.send_write(data_frame(data[off:off + BLOCK]))
                        if (off // BLOCK) % CHECK_EVERY == CHECK_EVERY - 1:
                            await self.radio.expect_write(
                                addr, "Program %s+0x%x" % (label, off + BLOCK),
                                lambda s, o=off: s['pointer'] == addr + o + BLOCK and
                                s['programming'] == (o + BLOCK < SECTOR))
                except Exception:
                    try:
                        await self.radio.send_write(control_frame(OP_DISARM))
                    except Exception:
                        pass
                    raise
                programmed += SECTOR
        return programmed

    async def program_block(self, address, block):
        await self.radio.send_write(control_frame(OP_PROGRAM, address))
        await self.radio.expect_write(address, "Begin 0x%x" % address,
                                      lambda s: s['programming'] and s['pointer'] == address)
        await self.radio.send_write(data_frame(block))
        await self.radio.expect_write(address, "Program 0x%x" % address,
                                      lambda s: s['pointer'] == address + BLOCK and not s['programming'])

    async def restore(self, on_progress=None):
        dir_sector = await self.read_directory()
        dir_ = parse_directory(dir_sector)
        if not dir_:
            if is_erased(dir_sector):
                return 0
            raise RuntimeError("Directory holds unknown data; use H9 Toolbox to deep-restore.")
        # pad segment lengths up to a whole sector (as rangesOf does in upload.mjs)
        ranges = [{'address': s['address'], 'length': self.pad_len(s['length'])}
                  for s in dir_['segments']]
        ranges += [{'address': MODULES_DIRECTORY, 'length': SECTOR}]
        current = await self.read_ranges(ranges, on_progress)
        n = 0
        for i, r in enumerate(ranges):
            plan = self.sector_plan(r['address'], current[i], bytes([0xFF]) * r['length'])
            for entry in plan:
                if entry['erase']:
                    await self.radio.send_write(control_frame(OP_ERASE, entry['address'], entry['oldCrc']))
                    await self.radio.expect_write(entry['address'], "Erase 0x%x" % entry['address'],
                                                  lambda s: s['crc'] == ERASED_CRC)
                    n += 1
        return n

    async def upload_pack(self, pack, name="H9 modules", on_progress=None):
        import hashlib
        digest = hashlib.sha256(pack).digest()
        directory, segments = build_directory(pack, name)
        dir_target = directory
        regions = [{'address': MODULES_DIRECTORY, 'length': SECTOR}]
        targets = [dir_target]
        offset = 0
        nseg = []
        for s in segments:
            part = self.pad(pack[offset: offset + s['length']])
            nseg.append({'address': s['address'], 'length': len(part)})
            regions.append({'address': s['address'], 'length': len(part)})
            targets.append(part)
            offset += s['length']
        segments = nseg

        await self.radio.enter_programming()
        try:
            # already complete & identical?
            try:
                already_sector = await self.read_directory()
                already = parse_directory(already_sector)
            except Exception:
                already = None
            if already and already['complete'] and already['length'] == len(pack) and already['sha256'] == digest:
                return {'already': True, 'name': already['name']}

            if on_progress:
                on_progress(0.05, "Reading target sectors")
            current = await self.read_ranges(regions, lambda f: on_progress(0.05 + 0.15 * f, "Reading target sectors"))
            previous = parse_directory(current[0])
            same = previous and previous['length'] == len(pack) and previous['sha256'] == digest

            if previous and not same:
                if on_progress:
                    on_progress(0.2, "Erasing previous upload")
                await self.restore(lambda f: on_progress(0.2 * f, "Erasing previous upload"))
                current = await self.read_ranges(regions, lambda f: on_progress(0.2 + 0.1 * f, "Re-reading target sectors"))
                same = False
            dir_actual = current[0] if (same and previous and previous['complete']) else dir_target

            plans = []
            for i, r in enumerate(regions):
                plans.append(self.sector_plan(r['address'], current[i], targets[i] if i else dir_actual))
            sectors = sum(len(p) for p in plans)
            written = 0
            programmed = 0
            if sectors:
                if on_progress:
                    on_progress(0.35, "Writing")
                for i, plan in enumerate(plans):
                    prog = await self.write_plan(plan, targets[i] if i else dir_actual)
                    programmed += prog
                    written += len(plan)
                    if on_progress:
                        on_progress(0.35 + 0.45 * written / sectors, "Writing sector %d of %d" % (written, sectors))

            if on_progress:
                on_progress(0.8, "Verifying")
            check = await self.read_ranges(regions, lambda f: on_progress(0.8 + 0.15 * f, "Verifying"))
            for i in range(1, len(regions)):
                if check[i] != targets[i]:
                    raise RuntimeError("Read-back differs at 0x%x." % regions[i]['address'])
            if bytes(check[0][:SECTOR - BLOCK]) != bytes(dir_target[:SECTOR - BLOCK]):
                raise RuntimeError("Directory read-back differs.")
            if not parse_directory(check[0]).get('complete'):
                await self.radio.send_write(control_frame(OP_PROGRAM, MODULES_DIRECTORY + SECTOR - BLOCK))
                pb = bytearray(BLOCK)
                pb[0:len(COMPLETE)] = COMPLETE
                await self.radio.send_write(data_frame(bytes(pb)))
            if on_progress:
                on_progress(1.0, "Done")
            return {'already': False, 'sectorsWritten': sectors, 'bytesProgrammed': programmed}
        finally:
            await self.radio.leave_programming()


# ============================================================================
# BLE connect / main
# ============================================================================
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


def list_catalog():
    print("Available apps / Доступные приложения:")
    for catalog, note in (("apps_catalog", "catalog"), ("myapps", "own builds / свои")):
        if not os.path.isdir(catalog):
            continue
        print("  [%s] / %s:" % (note, catalog))
        for fn in sorted(os.listdir(catalog)):
            if fn.endswith(".h9mod"):
                name = fn[:-6]
                if name == "menu":
                    print("    menu             (system module / системный: adds the app list; not a visible app)")
                    continue
                try:
                    mods = _read_h9mod(os.path.join(catalog, fn))
                    desc = "%s (%d module(s))" % (name, len(mods))
                except Exception as e:
                    desc = "%s (??) %s" % (name, e)
                print("    %-16s %s" % (name, desc))


def find_app_h9mod(name):
    """Look up <name>.h9mod in apps_catalog/ first, then myapps/ (own builds)."""
    for base in ("apps_catalog", "myapps"):
        p = os.path.join(base, name + ".h9mod")
        if os.path.isfile(p):
            return p
    return None


def load_requested(names):
    if not names:
        raise RuntimeError("No app names given. Use 'list' to see the catalog.")
    menu = None
    modules = []
    for nm in names:
        path = find_app_h9mod(nm)
        if path is None:
            raise RuntimeError("Unknown app '%s' (looked in apps_catalog/ and myapps/)." % nm)
        ms = _read_h9mod(path)
        if menu is None:
            menu_path = find_app_h9mod("menu")
            if menu_path is None:
                raise RuntimeError("No menu.h9mod found in apps_catalog/ or myapps/.")
            menu = _read_h9mod(menu_path)[0]
        # replace same-name modules
        for m in ms:
            modules = [x for x in modules if x['name'] != m['name']]
            modules.append(m)
    bundle = build_bundle([menu] + modules)
    return bundle, [m['name'] for m in ([menu] + modules)]


async def cmd_install(names, radio):
    bundle, names_list = load_requested(names)
    print("Bundling %d module(s): %s" % (len(names_list), ", ".join(names_list)))
    print("Bundle size / размер: %d bytes (%d KiB)" % (len(bundle), len(bundle) // 1024))
    shell = AppRadioshell(radio)

    def prog(f, text=""):
        sys.stdout.write("\r  %d%% %s  " % (int(f * 100), text))
        sys.stdout.flush()

    await radio.enter_programming()
    try:
        status = await radio.select_bank(0)
        print("Service v%d, ready=%s, JEDEC=%s" % (status['version'], status['ready'], hex_bytes(status['jedec'])))
        if status['version'] != 2:
            raise RuntimeError("No S2 write service. Need H9 Toolbox Firmware.")
    finally:
        await radio.leave_programming()
    result = await shell.upload_pack(bundle, "H9 modules", on_progress=prog)
    print()
    if result.get('already'):
        print("Already on the radio / уже установлено (%s)." % result['name'])
    else:
        print("Written and verified / записано и проверено: %d sectors, %d bytes."
              % (result['sectorsWritten'], result['bytesProgrammed']))
    print("Hold Back on the radio for the menu / Удерживайте Back для меню.")


async def cmd_peek(radio):
    shell = AppRadioshell(radio)
    await radio.enter_programming()
    try:
        sector = await shell.read_directory()
    finally:
        await radio.leave_programming()
    d = parse_directory(sector)
    if not d:
        print("Modules slot: empty or unreadable / пустой или непрочитан.")
        return
    print("Name: %s" % d['name'])
    print("Length: %d bytes" % d['length'])
    print("Complete: %s" % d['complete'])
    print("Segments: %s" % ", ".join("0x%x+%d" % (s['address'], s['length']) for s in d['segments']))
    if not d['complete']:
        print("(upload unfinished / загрузка не завершена)")


async def cmd_remove(radio):
    shell = AppRadioshell(radio)

    def prog(f, text=""):
        sys.stdout.write("\r  %d%% %s  " % (int(f * 100), text))
        sys.stdout.flush()

    await radio.enter_programming()
    try:
        status = await radio.select_bank(0)
        if status['version'] != 2:
            raise RuntimeError("No S2 write service.")
        n = await shell.restore(lambda f: prog(f, "erasing"))
    finally:
        await radio.leave_programming()
    print()
    print("Erased %d sector(s) / стёрто секторов: %d" % (n, n))


async def main_async(args):
    from bleak import BleakScanner, BleakClient
    if args.cmd == "list":
        list_catalog()
        return 0
    client = None
    try:
        client, radio = await connect_radio(timeout=args.scan_timeout)
        if args.cmd == "install":
            await cmd_install(args.apps, radio)
        elif args.cmd == "peek":
            await cmd_peek(radio)
        elif args.cmd == "remove":
            await cmd_remove(radio)
        return 0
    finally:
        if client:
            try:
                await client.disconnect()
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser(description="TD-H9 App Manager over Bluetooth / Приложения TD-H9 по Bluetooth")
    ap.add_argument("cmd", choices=["list", "install", "peek", "remove"])
    ap.add_argument("apps", nargs="*", help="app name(s) for 'install' (e.g. gps aprs)")
    ap.add_argument("--scan-timeout", type=int, default=8)
    args = ap.parse_args()
    try:
        return asyncio.run(main_async(args))
    except KeyboardInterrupt:
        print("\nInterrupted / Прервано.")
        return 130
    except Exception as e:
        print("\nError / Ошибка: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())