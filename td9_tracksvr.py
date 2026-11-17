#!/usr/bin/env python3
"""
td9_tracksvr.py -- Read / write the TRACKER SAVER point table on the TD-H9
over BLE, and edit it as JSON on the PC. Companion to the myapps/tracksvr app.

Point table format (written by the tracksvr app, replicated here):
  sector 0x7FE000 (4 KiB, one flash sector):
    [0..14]  magic "DKXCETRACKSAVES"   (15 bytes)
    [15]     0xFF                      (count byte, unused, stays erased)
    ...      records, each REC=16 bytes, packed with no gaps:
               name[8]  + lat(int32 LE) + lon(int32 LE)
    ...      rest of the sector = 0xFF (erased)
  The number of points = scan from record 0 to the first 0xFF byte.
  MAX_SAVES = (4096 - 16) / 16 = 255.

JSON format (PC side). Coordinates are decimal degrees; the radio stores
micro-degrees (1e-6). Names are stored as raw bytes, up to 8 characters.
  {
    "points": [
      { "name": "HOME", "lat": 55.7558, "lon": 37.6173 },
      { "name": "WORK", "lat": 59.9390, "lon": 30.3158 }
    ]
  }

Usage / Использование:
  python td9_tracksvr.py dump [--file tracksvr_points.json] [--addr 0x7FE000]
      -- read the current point table from the radio and save it as JSON
  python td9_tracksvr.py load [--file tracksvr_points.json] [--addr 0x7FE000]
      -- read the JSON file and write the whole sector to the radio
         (erases and rebuilds the sector; same as a C app full rewrite)
  python td9_tracksvr.py show [--addr 0x7FE000]
      -- print the current table without writing a file

  --file  defaults to tracksvr_points.json; a custom path is allowed.
  The radio must be on with Bluetooth programming mode (H9 Toolbox firmware,
  S2 write service). Depends on: pip install bleak.

Safety: `load` always writes the whole 4 KiB sector, so it replaces the
current table entirely. Before any erase a backup of the old sector is dumped
to tracksvr-sector-backup.bin (override with --backup).
"""

import argparse
import asyncio
import os
import json
import struct
import sys

from td9_kpi import (
    Radio,
    connect_radio,
    write_range,
    make_progress,
    SECTOR,
    ERASED,
)

SAVE_ADDR = 0x7FE000
HEAD = 16                       # 15 magic + 1 count(0xFF)
REC = 16                        # name[8] + lat[4] + lon[4]
MAGIC = b"DKXCETRACKSAVES"      # 15 bytes
MAX_SAVES = (SECTOR - HEAD) // REC   # 255


# ============================================================================
# Pack / parse
# ============================================================================
def pack_record(p):
    """One point dict -> 16 raw bytes."""
    name = p.get("name", "")
    nb = name.encode("utf-8", "replace")[:8]
    nb = nb.ljust(8, b"\x00")
    lat = int(round(float(p["lat"]) * 1e6))
    lon = int(round(float(p["lon"]) * 1e6))
    return nb + struct.pack("<ii", lat, lon)


def parse_record(raw):
    """16 raw bytes -> point dict (degrees as floats, name decoded)."""
    nb = raw[:8]
    name = nb.decode("utf-8", "replace").rstrip("\x00 \x20")
    lat, lon = struct.unpack_from("<ii", raw, 8)
    return {
        "name": name,
        "lat": round(lat / 1e6, 6),
        "lon": round(lon / 1e6, 6),
    }


def parse_table(sector):
    """Full 4 KiB sector bytes -> list[dict]. Raises on missing magic."""
    if sector[:15] != MAGIC or sector[15] != ERASED:
        raise RuntimeError("No TRACKER SAVER header at the start of the sector "
                           "(starts %s); refusing to parse." % sector[:15].hex())
    pts = []
    for i in range(MAX_SAVES):
        rec = sector[HEAD + i * REC:HEAD + (i + 1) * REC]
        if len(rec) < REC or rec[0] == ERASED:
            break
        pts.append(parse_record(rec))
    return pts


def build_table(points):
    """list[dict] -> whole 4 KiB sector bytes."""
    if not (0 <= len(points) <= MAX_SAVES):
        raise RuntimeError("Expected 0..%d points, got %d" % (MAX_SAVES, len(points)))
    sector = bytearray(MAGIC) + bytes([ERASED])
    for p in points:
        sector += pack_record(p)
    sector += bytes([ERASED]) * (SECTOR - len(sector))
    return bytes(sector)


# ============================================================================
# JSON load / save
# ============================================================================
def read_json(path):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, list):
        points = data
    elif isinstance(data, dict) and "points" in data:
        points = data["points"]
    else:
        raise RuntimeError("JSON must be a list or an object with a 'points' key.")
    for p in points:
        if "lat" not in p or "lon" not in p:
            raise RuntimeError("Each point needs 'lat' and 'lon': %r" % p)
        if len(str(p.get("name", "")).encode("utf-8", "replace")) > 8:
            raise RuntimeError("Point name too long (max 8 bytes): %r" % p.get("name"))
    return points


def write_json(path, points):
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"points": points}, f, ensure_ascii=False, indent=2)
        f.write("\n")


# ============================================================================
# CLI
# ============================================================================
async def cmd_dump(args):
    client, radio = await connect_radio(timeout=args.scan_timeout)
    try:
        await radio.enter_programming()
        try:
            sector = await radio.read_fixed(args.addr, SECTOR, make_progress("read "))
            print()
        finally:
            await radio.leave_programming()
    finally:
        await client.disconnect()
    pts = parse_table(sector)
    if args.command == "show":
        print("%d point(s) at %s:" % (len(pts), hex(args.addr)))
        for i, p in enumerate(pts, 1):
            print("  %2d. %-8s  %+.6f  %+.6f" % (i, p["name"], p["lat"], p["lon"]))
        return 0
    write_json(args.file, pts)
    print("Saved %d point(s) to %s" % (len(pts), args.file))
    return 0


async def cmd_load(args):
    if not os.path.exists(args.file):
        raise RuntimeError("File not found: %s" % args.file)
    points = read_json(args.file)
    final = build_table(points)
    client, radio = await connect_radio(timeout=args.scan_timeout)
    try:
        # backup the current sector before overwriting
        await radio.enter_programming()
        try:
            prev = await radio.read_fixed(args.addr, SECTOR)
        finally:
            await radio.leave_programming()
        with open(args.backup, "wb") as f:
            f.write(prev)
        print("Backup of current sector saved to %s" % args.backup)
        n = await write_range(radio, args.addr, final, make_progress("write "))
        print("Done: %d sector(s) written and verified at %s (%d point(s))."
              % (n, hex(args.addr), len(points)))
    finally:
        await client.disconnect()
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="TD-H9 TRACKER SAVER point editor / Редактор точек TRACKER SAVER")
    ap.add_argument("command", choices=["dump", "load", "show"])
    ap.add_argument("--file", default="tracksvr_points.json",
                    help="JSON file with the point table (default tracksvr_points.json)")
    ap.add_argument("--addr", default=hex(SAVE_ADDR), help="sector address (default 0x7FE000)")
    ap.add_argument("--backup", default="tracksvr-sector-backup.bin",
                    help="backup file for the sector before 'load' overwrites it")
    ap.add_argument("--scan-timeout", type=int, default=8)
    args = ap.parse_args()
    args.addr = int(args.addr, 0)
    try:
        return asyncio.run(cmd_dump(args) if args.command != "load" else cmd_load(args))
    except KeyboardInterrupt:
        print("\nInterrupted / Прервано.")
        return 130
    except Exception as e:
        print("\nError / Ошибка: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())