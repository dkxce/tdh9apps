# Addressing radio memory TD-H9

TIDRADIO TD-H9 memory card (JieLi AC695X)

* RAM addresses are the absolute physical addresses of the controller’s RAM inside the chip
  ?`0x11AD0`, `0x72400`, `0xC0F0`etc.).
* Stock unencrypted compiled firmware`app.bin`or any
  custom firmware) is physically loaded into NOR-flash at the address`0x5100`.
  The preceding 256 bytes:`0x5000 .. 0x5100`File area table app area head.
* Addressing CPU/XIP (execute-in-place) code when running:
  NOR-flash starting with`0x5100`(compiled firmware) mapping into the area
  starting from 0x01E00100.`Смещение в файле app.bin (код): 0x525A0
  Физически лежит во flash: 0xA76A0 = 0x5100 + 0x525A0
  Адрес исполнения на CPU / XIP: 0x1E526A0 = 0x01E00100 + 0x525A0`
* Notes:**[HW]**- checked on the radio,**[STATIC]**- from reverse/listing,**[DISASM]**- disassembled`app.bin`(address in CPU space),**[UNVERIFIED]**- not confirmed on the iron.

---

## 0. NOR-flash area with firmware

Table of file areas at the beginning of the image (decrypted from the official image)`TD-H9-V1.0.38.fw`): 32 B entry (fields + 16-B name)`app_area_head`,
`app.bin`, `VM`, `PRCT`, `BTIF`, `EXIF`).

| The Domain | Range. | Size | What is it? |
| --- | --- | --- | --- |
| `PRCT` | `0x00000–0xDA000` | `0xDA000` | Protected area - the whole image |
| `boot region` | `0x00000–0x04FFF` | `0x5000` | downloader / boot area; there is no separate batch in the file table (covered)`PRCT`); HID recovery cannot read/write [HW] |
| `app_area_head` | `0x05000–0x050FF` | `0x100` | file area table (`app.bin`, `VM`, `PRCT`, `BTIF`, `EXIF`); the first u32 is the control address, the second is the boot address`0x01E00100` |
| `app.bin` | `0x05100–0xD8D2C` | `0x0D3C2C`= 867 372 | application code (compiled firmware); executed by XIP`0x01E00100` |

Physical layout of the application area (NOR-flash)`0x5000`and further:

```
flash 0x5000  ┌──────────────────────────┐
              │  0x100: app_area_head    │  ← таблица файловых областей
              ├──────────────────────────┤
flash 0x5100  │                          │
              │  0x0D3C2C: app.bin (код) │  ← исполняется по XIP 0x01E00100
              │                          │
              └──────────────────────────┘
flash 0xD8D2C (конец области приложения)
```

---

## 1. RAM - key addresses

| Address RAM | Appointment |
| --- | --- |
| `0xC3AF` | Byte of the stock scan button:**d. d. d+2**, Menu 12, Up 13, Down 14, Back 15 [HW] |
| `0xC3C7` | keyboard`*`[STATIC] |
| `0xC3C8` | keyboard`#`[STATIC] |
| `0xC3DC` | Byte counter 0/1: increment with zero at`>= 2`tick`0x01E896C8`[DISASM] |
| `0xC0F0` | **RAM-cache base of radio settings**(see § 1.1) [DISASM] |
| `0x11AC2` | **`KB_LOCKED`Real keyboard lock flag**().0 = locked up). Stock puts through codeplug`0x300C`bit`0x20`(keyboard lock), reads the loader`0x01E722CE`; reset at kbhardlock-unlock (`*KB_LOCKED = 0`) [HW] |
| `0x11AD0` | App core state block: +100 river mode, +101 tech mode; +138 BLE descriptor; +6636 DAC descriptor [HW] |
| `0x11AD0+0x1BC` | Current frequency (u32, 10 Hz) [HW, VFO Probe] |
| `0x11AD0+0x1C8`grow,`+0x1CC`offset TX,`+0x789`direction of the shift [STATIC] |  |
| `0x11AD0+0x1D0` / `0x1D8` | VFO A/VFO B (u32, 10 Hz) [HW, VFO PROBE] |
| `0x11AD0+0x10CD` | Byte range: 1 = UHF, 0 = VHF [HW] |
| `0x11AEC` | APRS route byte (write)`pl_aprs.c` / `sf_aprs.c`) [STATIC] |
| `0x12234` | NOR marker "ready" =`0x5AA5`[HW] |
| `0x12550` / `0x12558` / `0x12560` | GPS fix: validity/latitude/longitude (signed micro-degrees) [HW, TRACKING] |
| `0x13D10` | Last decoded AX.25 packet (324B) [STATIC] |
| `0x16634` | APRS serial ring (2048 B; record indices)`h[0x16634]`→`0x16E34`reading`0x16E36`) [HW] |
| `0x1D8B0` | 100-Hz tick counter`TICK`in all annexes) [HW] |
| `0x1D7D0` | Heap free-bit map (blocks of 128B) |
| `0x1D8A8` / `0x1D8AA` | Heap: blocks occupied/all [HW] |
| `0x1E4D8–0x2BEFF` | First Range of Stock Heap [STATIC] |
| `0x29F00–0x2BEFF` | A piece of stock heap, which the module-loader selects for itself when downloading, and the module code links there. |
| `0x29F00–0x2BEFF` | **Module-loader**RAM-slot of modules (8 KB): a piece of stock heap, which the module-loader selects for itself, the code of modules links here (`LOAD=0x29F00`, `SLOT_END=0x2BF00`max. 0x2000) [HW] |
| `0x2BEE0–0x2BEFF` | "Run next" mailbox:`HRUN`, `name[16]`, `spare[3]`(see paragraph 1.3) [HW] |

### 1.1 RAM cache settings`0xC0F0`(copy of radio settings)

Stock holds a copy of the radio settings in RAM:`RAM = 0xC0F0 + (codeplug_offset − 0x2CCC)`.

`codeplug_offset`This is the displacement of the settings field inside the full image of the radio settings.

In the radio settings exist in two places:
1.**Settings VM**Permanent storage in NOR-flash (in §0 and §2 this area)`VM`range`0xDA000–0xFC000`).
2.**Ram.**Stock keeps a working copy of settings in RAM based`0xC0F0`Not to read NOR-Flash on every tick.

| Ram. | Codeplug | Field. |
| --- | --- | --- |
| `0xC4A0`(0944) | `0x307C` | APRS on |
| `0xC50F`(1055) | `0x30EB` | Report Voltage |
| `0xC510`(1056) | `0x30EC` | **Report Sats**(Stock checks in front of satellites in beacon:)`0x01E5C04E`) |
| `0xC511`(1057) | `0x30ED` | Report Mileage |
| `0xC512`(1058) | `0x30EE` | PTT link |
| `0xC513`(1059) | `0x30EF` | Timed beacon |
| `0xC514`(1060) | `0x30F0` | Beacon interval (u16le) |
| `0xC532`(1090) | `0x310E` | RX channel |
| `0xC533`(1091) | `0x310F` | TX channel (beacon frame type)`> 4`= silent |
| `0xC534`(1092) | `0x3110` | PTT priority |
| `0xC537`(1095) | `0x3113` | RX popup (it depends on the pop-up request) |
| `0xC1CA`(218) | — | APRS/TX enabled (the sender of the beacon is available at`!= 1`) |

### Status of the annex (`0x72400`)

| Displacement | Appointment |
| --- | --- |
| `+22` | Status bar byte: bit`0x20`= locking icon (sets)`0x01E895F0`) [DISASM] |
| `+43`, `+46` | Mode flags (checked by key handler and action manager) [DISASM] |
| `+78` | Byte "transmission is coming" (timed-beacon path: when`1`It doesn't call the end of the transmission. |
| `+79` | Byte that the runoff checks before the beep of the key; in practice always 0 [DISASM] |
| `+87` | Tick counter (byte) [DISASM] |
| `+504` | Byte Flag [DISASM] |
| `+512` | Counter (word) [DISASM] |
| `+540` | Timed Beacon Tick Counter (Threshold)`interval*100`) [HW] |
| `+1892` | magic`0x5AA5`(Structure Initialized Verification) [DISASM] |
| `+2688` | APRS state ready (beacon does not fly)`0`) [DISASM] |
| `+2704`, `+2708` | Position for beacon: both 0 → sending skipped [DISASM] |
| `+4304`, `+4308` | Gate beep keys (`+4308`= Keypad Beep setup) [DISASM] |
| `+4332`, `+4334` | Mode bytes (compared to 7) [DISASM] |

### 1.3 Module slot`0x29F00–0x2BEFF`

| Range. | Who uses it? |
| --- | --- |
| `0x29F00` | Module loading point (`load` в `h9_api`); all modules [HW] link here. |
| `< 0x2AC00` | Menu Body (Module 0) [STATIC] |
| `0x2AC00–0x2B87F` | `lockresearch`(structure)`lr`magic`LKR1`) - pressed against`0x2B880`, crosses the tracker area [STATIC] |
| `0x2B7EC–0x2BEE0` | `struct trk`(TRACKING/TRACKER),`STATE_BYTES=0x6F4`magic`TRK2`); satshare overlaps with it [HW] |
| `0x2B880` | Upper Body Limit of Modules: Satshare (SAT PASS), Bodies must not cross it |
| `0x2B8B4` | magic`CSRT`Resident Tracker (Satshare) [STATIC] |
| `0x2B8DE` | `MDL_LOADED`- the module worked [STATIC] |
| `0x2B8DF` | `skip_check`owner-lock: 0 fresh,`1`=UNLOCKED unlocked,`2`=LOCKED locked,`3`=NORMAL to crack,`4`=KBLCKD key block,`5`=2BLCKD needs a key block [HW] |
| `0x2B8E0–0x2BEE0` | State of play:`struct sf`(SEAFIGHT),`struct pl`(APRS 8-BALL) Covers the top.`struct trk`- does not live at the same time as Tracking [HW] |
| `0x2BEE0–0x2BF00` | Mailbox`HRUN`: `magic`, `name[16]`, `spare[3]`[HW] |
| `0x2BEF2`, `0x2BEF3` | Free bytes in the tail`name[16]`[STATIC] |
| `0x2BEF4` | `GFCR`- The magic of the GPS recorder tab [HW] |
| `0x2BEF8`, `0x2BEFC` | Last saved lat/lon recorder (i32) [HW] |

Important: ranges`0x2AC00–0x2BEE0`crossover
?`lockresearch`, `struct trk`This is normal, because in the slot
Only one module works at the same time. a permanent state which should
live between launches, keep lower`0x2BEE0`; mailbox itself and GPS recorder
tab`GFCR`, lat/lon - tail`0x2BEE0–0x2BF00`[STATIC]

---

## 2. Mainstream flash

| Range. | Appointment |
| --- | --- |
| `0x00000–0x04FFF` | **Boot region**. HID recovery cannot read/write [HW] |
| `0x05000–0xD8D2C` | **Appropriate image** (`app_area_head`/`app.bin`, see §0): encrypted on 32-B XOR blocks with CRC-flow key`0xF181 ^ ((flash−0x5000)>>2)`; XIP address`0x01E00100`[HW] |
| `0xDA000–0xFC000` | **Settings VM** (`VM`): radio settings + free zone; two banks of 32 KB`0xDA000`, `0xE2000`) [STATIC] |
| `0xEA000–0xFBFFF` | Free (72 KB), plain, executed on site`0x01EE5000`[HW] |
| `0xFD000–0xFE000` | `BTIF`Bluetooth I/F (4 KB) [STATIC] |
| `0xFE000–0xFF000` | `EXIF`EXIF data (4 KB) [STATIC] |
| `0xFC000–0xFFFFF` | Stock data (leave alone) [HW] |

---

## Resource chip (16 MB SPI NOR, SPI2)

| Address | Appointment | Writes |
| --- | --- | --- |
| `0x3A7000–0x3FFFFF` | The tail of the voice party: erased, runoff does not write [HW] | — |
| `0x3FE000` | User VFO backup (log 16-B records) [STATIC] | — |
| `0x3FF000` | **Last GPS fix**log`GFX1`, 256×16 B) [HW] | loader stub |
| `0x716000` | Radio Password Sector: 8B Lock Records`0x716FF8`, the rest erased (0xFF) [HW] | PASSLOCK |
| `0x717000–0x7FFFFF` | Font party tail;**DKXCEPIC pack** (`DKXCEPI`) [HW] | `td9_kpi.py` |
| `0x7FE000` | **Tracker Saver Points**(4 KIB),`DKXCETRACKSAVES`) [HW] | TRACKER SAVER |
| `0x802000`, `0x80B606` | APRS inbox/beacon list`sys_data`) [STATIC] | runoff |
| `0x900000` | `rw_freq`VFO settings,**Stock writes here**[HW] | runoff |
| `0xA00000` | **Boot logo**(160×128 RGB565, 40,960 B) [HW] | `td9_logo.py` |
| `0xA1F000` | Game slot dir.`H9DATA01`) data`0xA20000–0xCC0000`[HW] | — |
| `0xCC0000` | **Modules slot dir** (`H9MODS01` @ `0xCC1000`, ≤124 KiB) [HW] | ours`td9_apps.py` |
| `0xCE0000` | Music slot dir, data`0xCE1000–0xCFFFFF`[HW] | — |

---

## 4. APRS/GNSS settings (Settings VM, read/written in BLE)

Tool:`td9_aprs.py`. Range of settings in memory =`0x3066–0x3113`(shifts from the beginning of Settings VM). Read in 32-byte blocks; fields can
cross the boundaries of the blocks. In RAM, the stock holds a copy - see § 1. 1st
?`RAM = 0xC0F0 + offset − 0x2CCC`).

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
| Route 2 name/count | `0x30E2` / `0x30EA` | text8/byte |
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

## 5. Key functions of runoff (disasm)`app.bin`)

| CPU | Appointment |
| --- | --- |
| `0x01E525A0` | Key handler; binding table`0x1EAF150`(Step 24B: keycode, handler) |
| `0x01E52BD0` | Keyboard beep wrapper`(tone, 20, 1)`; tone 42 =`tone/d_sound.*`[HW] |
| `0x01E50128` | Accessor settings; table descriptors`0x1EB6170`(step 20 B) |
| `0x01E5C3D4` | APRS beacon sender (no arguments) [HW] |
| `0x01E5CDEC` | End of transmission`r0=0`); without it, the PTT remains clamped [HW] |
| `0x01E82FB2` | Timed beacon; call site`app+0x87494` |
| `0x01E55FC2` | APRS pop-up request; website`app+0x862C6`[HW] |
| `0x01E7E318` | Side button action manager:`r0`1-based, 8 actions (None, FM Radio, GNSS SW, Tone, Alarm)**Weather=6**, PTT2, OD PTT |
| `0x01E595F8` | Application of the NOAA channel;`0x01E5969C`Weather stop; frequency`0x01EB3214` |
| `0x01E895F0` | Drawing of the lock icon (`lock_icon` `0x1EB1DA6`), puts the bit`0x20` в `b[0x72400+22]` |
| `0x01E53306` / `0x01E58E3E` | Row redrawing / full RF-refresh (calls)`menu.c`) |

Lines and tables:`0x01EB1993`signature of the action`Weather`(renamed as`SBeacon`)`0x01E956AE` — `Weather Site`(do not touch); lists of action names`0x1ECD2C0` и `0x1ECD320`.

---

## 6. Entry modes

| How | Regime. |
| --- | --- |
| **PTT + 2**switch-on | Vendor HID`4C4A:4155`(web flasher) [HW] |
| Retention**8**switch-on | ROM USB loader`4C4A:2342`, mass storage [HW] |
| **PTT + 1 / +3 / +7** | Serial / USB-serial updater / APRS [HW] |
| Retention**#**(modular loader) | Application menu [HW] |