# Addressing radio memory TD-H9

TIDRADIO TD-H9 memory card (JieLi AC695X) RAM addresses - physical
?`0x11AD0`etc.); code in flash is displayed by`CPU = 0x01E00000 +
(flash − 0x5000)`.

---

## 1. RAM - key addresses

| Address | Appointment |
| --- | --- |
| `0xC3AF` | Byte of the stock scan button:**d. d. d+2**, Menu 12, Up 13, Down 14, Back 15 [HW] |
| `0xC4A1` / `0xC4A7` | APRS callsign (6 bytes) / SSID [STATIC] |
| `0x11AD0` | App core state block: +100 river mode, +101 tech mode; +138 BLE descriptor; +6636 DAC descriptor [HW] |
| `0x11AD0+0x1BC` | Current frequency (u32, 10 Hz) [HW, VFO Probe] |
| `0x11AD0+0x1C8`grow`+0x1CC`offset TX,`+0x789`direction of the shift [STATIC] |  |
| `0x11AD0+0x1D0` / `0x1D8` | VFO A/VFO B (u32, 10 Hz) [HW, VFO PROBE] |
| `0x11AD0+0x10CD` | Byte range: 1 = UHF, 0 = VHF [HW] |
| `0x12234` | NOR marker "ready" =`0x5AA5`[HW] |
| `0x12550` / `0x12558` / `0x12560` | GPS fix: validity/latitude/longitude (signed micro-degrees) [STATIC] |
| `0x13D10` | Last decoded AX.25 packet (324B) [STATIC] |
| `0x16634` | APRS serial ring (2048 B; record indexes)`h[0x16634]`→`0x16E34`reading`0x16E36`) [HW] |
| `0x1D8B0` | 100-Hz tick counter [HW] |
| `0x1D7D0` | Heap free-bit map (128 B blocks) |
| `0x1D8A8` / `0x1D8AA` | Heap: blocks occupied/all [HW] |
| `0x1E4D8–0x2BF00` | First Range of Stock Heap [STATIC] |
| `0x29F00–0x2BEFF` | **Module-loader**: 8 KiB modules slot; "run next" mailbox in the after 32 B`0x2BEE0`magic`HRUN`) [HW] |
| `0x2B8B4` | magic`CSRT`Resident Tracker (Satshare) [STATIC] |

---

## 2. Main flash (1 MiB)

| Range. | Appointment |
| --- | --- |
| `0x00000–0x04FFF` | **Boot region**. HID recovery cannot read/write [HW] |
| `0x05000–0xD9FFF` | **Appropriate image**Encrypted: Each 32-B XOR block with a CRC key stream`0xF181 ^ ((flash−0x5000)>>2)`[HW] |
| `0xDA000–0xE9FFF` | **Settings VM**two banks of 32 KB (`0xDA000`, `0xE2000`) [STATIC] |
| `0xEA000–0xFBFFF` | Free (72 KB), plain, executed on site`0x01EE5000`[HW] |
| `0xFC000–0xFFFFF` | Stock data (leave alone) [HW] |

---

## Resource chip (16 MiB SPI NOR, SPI2)

| Address | Appointment | Writes. |
| --- | --- | --- |
| `0x3A7000–0x3FFFFF` | The tail of the voice party: erased, runoff does not write [HW] | — |
| `0x3FE000` | User VFO backup (log 16-B records) – overlap with the voice of the game [STATIC] | - |
| `0x3FF000` | **Last GPS fix.**log`GFX1`, 256×16 B) [HW] | loader stub |
| `0x717000–0x7FFFFF` | The tail of the font party [HW] | — |
| `0x802000`, `0x80B606` | APRS inbox/beacon list`sys_data`) [STATIC] | runoff |
| `0x900000` | `rw_freq`VFO settings,**Stock writes here**[HW] | runoff |
| `0xA00000` | **Boot logo**(160×128 RGB565, 40,960 B) [HW] | - |
| `0xA1F000` | Game slot dir.`H9DATA01`) data`0xA20000–0xCC0000`[HW] | - |
| `0xCC0000` | **Modules slot dir** (`H9MODS01` @ `0xCC1000`, ≤124 KiB) [HW] | ours`td9_apps.py` |
| `0xCE0000` | Music slot dir, data`0xCE1000–0xCFFFFF`[HW] | — |

---

## 4. APRS/GNSS settings (Settings VM, read/written in BLE)

Tool:`td9_aprs.py`. Range of settings in memory =`0x3066–0x3113`(shifts from the beginning of Settings VM). Read in 32-byte blocks; fields can
cross the boundaries of the blocks.

| Field. | Displacement | Type/length |
| --- | --- | --- |
| GPS | `0x3066` | byte |
| Position format | `0x3067` | byte |
| Time zone | `0x3068` | byte |
| Speed/dist/alt unit | `0x3069` / `0x306A` / `0x306B` | byte |
| Fixed position (lat/lon/alt) | `0x306C–0x307B` | 4×u32be + markers |
| Station type | `0x307B` | yes |
| APRS on | `0x307C` | yes |
| Callsign | `0x307D` | text |
| SSID | `0x3083` | byte |
| Icon table | `0x3084` | byte |
| Comment | `0x3095` | text |
| Icon symbol | `0x30D6` | byte |
| Mic-E start/type | `0x30D7` / `0x30D8` | yesno/byte |
| Route 1 name/count | `0x30D9` / `0x30E1` | text8/byte |
| Route 2 name / count | `0x30E2` / `0x30EA` | text8/byte |
| Reports (volt/sats/mil) | `0x30EB` / `0x30EC` / `0x30ED` | yes |
| PTT link | `0x30EE` | yes |
| Timed beacon | `0x30EF` | yes |
| Beacon interval | `0x30F0` | u16le |
| Digis forward channel | `0x30F2` | byte |
| DIGI1 en/name | `0x30F3` / `0x30F4` | yesno / text8 |
| DIGI2 en/name | `0x30FC` / `0x30FD` | yesno / text8 |
| Forward wait | `0x3105` | byte |
| Remote password | `0x3106` | text8 |
| RX/TX channel | `0x310E` / `0x310F` | byte |
| PTT priority | `0x3110` | yes |
| RX popup | `0x3113` | yes |

Note: full Settings VM card (structure of banks and headers) - from
handbook section 3 [STATIC]; field table from our tools.

---

## 5. Entry modes

| How | Regime. |
| --- | --- |
| **PTT + 2**switch-on | Vendor HID`4C4A:4155`(web flasher) [HW] |
| Retention**8**switch-on | ROM USB loader`4C4A:2342`, mass storage [HW] |
| **PTT + 1 / +3 / +7** | Serial / USB-serial updater / APRS [HW] |
| Retention**Back**(modular loader) | Application menu [HW] |