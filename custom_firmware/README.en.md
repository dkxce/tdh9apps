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
| `passlock/passlock.c` | PASSLOCK module with unlock-only branch (for copying into`myapps/passlock/src/passlock.c`) |
| `build-lock.yml` | GitHub Actions workflow: build firmware with stub through your CI (see below) |

## Finished assembly

| File. | Appointment |
| --- | --- |
| [TD-H9-V1.0.38 dkxce passlock.fw](TD-H9-V1.0.38_dkxce_passlock.fw) | Passlock support, download through Back |
| [TD-H9-V1.0.38 dkxce passlock sharp. fw.fw.](TD-H9-V1.0.38_dkxce_passlock_sharp.fw.fw) | Passlock support, download through # |

## Autobuild through GitHub Actions (recommended)

Without access to`melidosian/h9toolbox`You don't need: workflow`build-lock.yml`Cloning it as a public vendor`stub/stub.patch`extract
stock`app.bin`from`firmware/TD-H9-V1.0.38.fw`in your repos, download JieLi
toolchain from your release`toolchain`And he collects firmware.

Installation:
1. Put it down.`custom_firmware/`repository`dkxce/tdh9apps`near`firmware/`).
2. Copy.`custom_firmware/build-lock.yml` в
`.github/workflows/build_lock.yml`.
3. Make sure in the release`tdh9apps`tagged n`toolchain`asset`.tar.xz`with towel (folder inside contains)`pi32v2/bin/clang`).
4. Actions**Build TD-H9 lock firmware**Run workflow.
5. Artifact`lock-firmware`contains`h9_toolbox_modules_OFFLINE_EXPERIMENT.fw`(Stub firmware),`h9_modules.bin`and manifesto.

Pour:`h9_flash.py --fw <файл.fw>`(HID, PTT+2).

Note: the same firmware collects modules (menu/APRS/...). The PASSLOCK module with
Unlock-only (self=0xFFFF) is collected by a separate workflow`Build myapps modules`from`myapps/passlock/src/passlock.c`and established
via App Manager.

## Where to put and how to collect (manually)

### 1. The loader (`stub. s`)

Repository:`melidosian/h9toolbox`.
- Copy it.`stub/stub.s` в `firmware-research/module-loader/src/stub.s`(or apply)`stub.patch`: `git apply stub.patch`).
- Collect the firmware.`firmware-research/module-loader/README.md`.
`JIELI_TOOLCHAIN=<toolchain> python3 -I build.py --stock-image <app.bin> --stock-package <TD-H9-V1.0.38.fw>`Check locally (without tulchain): assembler and linker assemble
(stub = 1448 bytes, cavern = 1632, stock 184 bytes).

### 2. Module`passlock. c.`)

Repository:`dkxce/tdh9apps`(TDH9Unbrick).
- Copy it.`passlock/passlock.c` в `myapps/passlock/src/passlock.c`.
Build the module through CI`Build myapps modules`
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