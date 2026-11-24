# TD-H9 self-contained locking - materials for assembly

Owner's radio security function (for the owner, no bypassing protection).
Two changes that together give offline lock when turned on:

1. **`stub/`**- loader patch (`stub.s`) H9 Toolbox firmware.
   At each turn on / restart checks the password in resource flash`0x716FF8`If the radio is locked and not yet unlocked in this session,
   launcher`PASSLOCK`(input + rendering), does not draw the screen itself.
   Session unlock flag - spare-byte`0x2B8DF`satshare
   (Experiencing only soft-reset)`cpu_reset`; after a cold start,
   i.e. turn off / turn on the power, the flag again requires entering the password.
2. **`passlock/`**updating the module`PASSLOCK`: added**unlock-only**call-out`module_main(api, 0xFFFF)`Hard screen without exit
   to the correct password (menu/personal reset is not available).

## Files.

| File. | Appointment |
| --- | --- |
| `stub/stub.s` | the full source of the bootloader with lock-checking (for copying in`module-loader/src/stub.s`) |
| `stub/stub.patch` | git-diff patch to current`stub.s`h9toolbox repository |
| `menu/menu.c` | Menu with owner-lock boot path`skip_check == 2`launch`PASSCODE`or fail-open; to copy in`module-loader/modules/menu/menu.c`) |
| <TD-H9-V1.0.38DKXCE.fw> | Firmware with stub (lock-check on inclusion) |

folder`stub/`additionally`stub_base.s`(source loader) and`make_diff.py`(generation)`stub.patch`). The module itself`PASSLOCK`live`myapps/passlock/src/passlock.c`and collected separately (see below).

## Finished assembly

| File. | Appointment |
| --- | --- |
| <TD-H9-V1.0.38DKXCE.fw> | stub + passlock |
| [TD-H9-V1.0.38DKXCE APRS mute.fw](TD-H9-V1.0.38DKXCE_APRS_mute.fw) | stub + passlock + aprs mute |
| [TD-H9-V1.0.38DKXCE APRS mute bb beacon.fw.](TD-H9-V1.0.38DKXCE_APRS_mute_bb_beacon.fw) | stub + passlock + aprs mute + beep + beep + send beacon |

The manager of applications (widgets) in all builds opens with retention`#`.

### Assembly of TD-H9-V1.0.38DKXCE.fw

The basis for all other firmware:
- Opens with a hold.`#`.
Supports the selection and download of applications.
- Supports locking the radio with a password.
Supports full keyboard lock mode. (see para.[lock-pass. md](../lock-pass.md) )

### Build TD-H9-V1.0.38DKXCE APRS mute.fw

Includes assembly functionality TD-H9-V1.0.38DKXCE.fw and:
- Turns off the speaker when APRS is on on the B-channel, see the flow below

```
Начало проверки
       │
       ▼
APRS включен? ────────────(Нет)──► [Динамик ВКЛЮЧЕН (Unmute)]
       │
     (Да)
       ▼
RX-сторона == B? ─────────(Нет)──► [Динамик ВКЛЮЧЕН (Unmute)]
       │
     (Да)
       ▼
Текущая частота == B? ────(Нет)──► [Динамик ВКЛЮЧЕН (Unmute)]
       │
     (Да)
       ▼
Частота A != Частота B? ──(Нет)──► [Динамик ВКЛЮЧЕН (Unmute)]
       │
     (Да)
       ▼
[Динамик ВЫКЛЮЧЕН (Mute)]
```

### Build TD-H9-V1.0.38DKXCE APRS mute bb beacon.fw

Includes assembly functionality TD-H9-V1.0.38DKXCE.fw and TD-H9-V1.0.38DKXCE APRS mute.fw as well as:
Supports assigning a side button to send an APRS packet (beacon) instead of a stock Weather (Weather can all open retention) n`0`)
Supports beep-beep sound with APRS POPUP (if APRS has Resport Sats enabled) see the flow below

```
Пришел APRS-пакет
       │
       ▼
APRS -> Advanced Set -> RX Auto POPUP включен? ───(Нет)──► [Нет звука]
       │
     (Да)
       ▼
APRS -> Beacon Set -> Report Sats включен? ──(Нет)──► [Нет звука]
       │
     (Да)
       ▼
[Beep-Beep]
```

## Autobuild through GitHub Actions (recommended)

Installation:
1. README and files are already in the repository`dkxce/tdh9apps`
(`custom_firmware/`near`firmware/`workflow in`.github/workflows/`).
2. Make sure in the release`tdh9apps`tagged n`toolchain`asset`.tar.xz`with towel (folder inside contains)`pi32v2/bin/clang`), and in a tag release n`buildable`asset`.zip`(minimum archive h9toolbox).
3. Actions →**Build TD-H9-...**Run workflow.
4. Artifact`.zip`contains`*.fw`(firmware with stub), application menu`menu.h9mod`and manifesto.

Pour:`h9_flash.py --fw <файл.fw>`(HID, PTT+2).

The PASSLOCK module is assembled as a separate workflow`Build myapps modules *.h9mod`from`myapps/passlock/src/passlock.c`and established
via App Manager.

## Where to put and how to collect (manually)

### 1. The loader (`stub. s`)

Repository:`dkxce/tdh9apps`.
- Prepare.`custom_firmware/stub/stub.s`- Prepare.`custom_firmware/stub/aprs_mute.s`- Prepare.`custom_firmware/stub/aprs_ext.s`- Launch it.`make_diff.py`- Collect firmware through CI.`Build TD-H9-...`Put it on the radio.

### 2. Module`passlock. c.`)

* Prepare.`myapps/passlock/src/passlock.c`.
* Build a module through CI`Build myapps modules *.h9mod`
  (`build_one.py passlock`You get it.`passlock.h9mod`Put it on the radio.

## Order on the radios.

1. Fill the firmware with a new stub and install`passlock.h9mod`.
2. Set/check password:`python td9_kpi.py pass --password 123456`.
3. Turn off and turn on the radio (cold boot). If it is locked, a screen will appear.
   Input; without the correct password, the radio does not work.
4. `passclear`BLE resets the password (owner).

## Safety note

* The logic is written so that in the absence of an installed module`PASSLOCK`lock-up**unblocked silently**- she stays locked up.
  (The session flag shall be placed only after the actual run of the module.)
* No tips / methods of bypassing: this is a regular function of the owner.