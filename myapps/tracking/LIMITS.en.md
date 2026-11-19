# Tracker: the limitations of the TD-H9 platform

Limitations that determine how much code and data
It is placed in the tracker and how it should be divided into modules.

## Table of contents

* [Module package](#пакет-модулей)
* [One module.](#один-модуль)
* [General condition of the tracker](#общее-состояние-трекера)
* [Body layout and size](#компоновка-и-размер-тела)
* [Division into modules](#деление-на-модули)
* [If the module does not fit](#если-модуль-не-помещается)
* [Environment of implementation](#среда-выполнения)
* [Compatibility with other modules](#совместимость-с-другими-модулями)
* [Screen and keys](#экран-и-клавиши)
* [History of change](#история-изменений)

## Module package

All applications are downloaded to the walkie-talkie in one package`.h9mod`format`H9MODS01`).
The package is written on a flash chip of resources, in a slot of modules.

| Parameter | Meaning. |
| --- | --- |
| Total package size | no more than 126,976 bytes (124 KB) |
| Area on a chip | from`0xCC1000`music catalogue`0xCE0000` |
| Number of modules | 1-255, including the menu |
| Packet header | 16 bytes |
| Catalogue entry of the module | 32 bytes: name, offset, size,`bss`, point of entry, CRC |
| Leveling the body | 4 bytes |

Limit 124 KB checks the loader`td9_apps.py`constant`MODULES_SLOT_MAX`).
The bar on the radio would miss up to 128 KB, so the loader sets the current limit.
These 124 KiBs include menus and all apps together.

## One module.

Before launch, the module is copied to a RAM slot.`0x29F00..0x2BEFF`That's 8KB.

| Parameter | Meaning. |
| --- | --- |
| Body +`bss` | No more than 8,192 bytes (checks the bootloader) |
| The mailbox "run the next one" | `0x2BEE0..0x2BEFF`, 32 bytes (`H9_REQUEST`magic`HRUN`) |
| Top of available memory | `H9_SHARED_TOP = 0x2BEE0` |

To open the next module, the current one writes his name to the mailbox.
(with spaces of up to 16 bytes) and returns 0. The menu compares all 16 bytes
name. Modules with a name for a point in the menu list are not shown.

## General condition of the tracker

Tracker modules store general data in`struct trk`Right under the mailbox.
The state undergoes module switching until a larger module is loaded.

| Parameter | Meaning. |
| --- | --- |
| Reserve.`STATE_BYTES` | `0x6F4`= 1,780 bytes |
| Size`struct trk` | flatly`0x6F4`byte; excess of reserve catches`_Static_assert` |
| Address of status | `0x2BEE0 − 0x6F4 = 0x2B7EC`(quantum 4) |
| Of course. | 153 points × 10 bytes (latitude, longitude, time) = 1,530 bytes |
| Body limit of tracker module | `0x2B7EC − 0x29F00`= 6,380 bytes |

Field`struct trk`packed: counters, flags and keycodes are stored in`uint8_t`.
course, speed and number of satellites`uint16_t`. Place the new fields like this.
so that there are no alignment holes: each byte of state is taken away
The body of all modules.

The state only adjusts.`TRACKER` (`state_init` в `tracker.c`): reset,
omissions and the latest GPS stock fix. Hidden modules are opened only
There is no code for the first launch. If the hidden module is still
It's going to be unconfigured.`trk_open`opening`TRACKER`.

Change.`STATE_BYTES`, the status address shifts. First launch.
settings and track reset: old value`magic`It's out of place.

You can increase the track only due to the body of the modules: each point takes 10 bytes.
All tracker modules at once.

### Satshare and the lock flag

Range of status`0x2B7EC..0x2BEE0`includes a stock satshare zone`0x2B880..0x2B8DF`. In order not to break the radio lock,`struct trk`nearer
This area has a field`skip_check` (`0x2B8DF`The owner-lock flag reads the stub and
menu).

`state_init`(tracker.c) saves and returns`skip_check`first
start so that the zero state does not reset the radio lock.

## Body layout and size

* Component`ld.py`lay down`.text`then the whole`.rodata`file-order`build.json`.
* Unused functions**stay out**. Everything that's in the plug-in.
  The C-file enters the module even if the module does not call it.
* The last file of each module is`trk_end.c`constant`trk_end`.
  Her address marks the end of the body.
* `trk_open`When starting, compares the end of the body with the address of the state. When applied
  output`MODULE TOO BIG +N`where N is the byte overrun, and the module
  It's not starting. Without a tail check,`.rodata`(lines, tables)
  It would have been wiped out and the screen would have remained empty.
* The sizes of individual modules in the CI log are not visible, there is only the total package size.
  The overrun is only visible on the N radio.

## Division into modules

Since the linker does not throw out excess, the common files are divided by consumers:

| File. | What's inside | Connect. |
| --- | --- | --- |
| `trk_core.c` | state, NMEA, track start and record, navigation mathematics, keys, text | everything |
| `trk_track.c` | cleaning and track limit, distance and time of the point,`time_text` | Tracker, .TRK SETUP, .TRK POINTS |
| `trk_deg.c` | `deg_text` | TRACKER, .TRK FIXED, .TRK RADAR |
| `trk_menu.c` | menu, status line,`trk_msg`, the width of the line for centering | Tracker, .TRK FIXED, .TRK SETUP, .TRK POINTS |
| `trk_view.c` | trimmed primitives, value panel, record icon, frame, record key | .TRK RADAR, .TRK MAP |
| `trk_end.c` | marker | last. |

Put the new common function in a file that only its customers connect.
The function that one module needs remains in its own file.

## If the module does not fit

The procedure for increasing the price:

1. Remove from shared files what this module does not need (see table above).
2. Replace tables with computation and the repetitive figure with a common function.
3. Shorten the lines: they are in`.rodata`and count in the body.
4. Split the screen into two modules and switch through the mailbox.
   The total volume of the package (124 KB) almost never interferes with 8 KB per module.

## Environment of implementation

* No module.`.data` и `.bss`: Global variables only`const`.
  Modified data is stored in`struct trk`Or on the stack.
* The task stack is small, do not start large buffers on the stack.
* no libc;`memcpy` и `memset`general`gfx.c`.
* Assembly flags:`-Oz -ffreestanding -fno-builtin -Wall -Werror`.
  Any warning puts the assembly in CI.
* Assembly is carried out only in CI Tulchain JieLi, the module cannot be assembled locally.
* While the module is running, the radio's stock cycle is standing. GPS can be read by the module itself.
  modem-ring`0x16634` в `trk_service`So the track is written,
  Only while one of the tracker modules is open.
* System timer`0x1D8B0`100 Hz.

## Compatibility with other modules

The modules are loaded into one RAM slot.`0x29F00..0x2BEFF`. Starting another module
It overwrites the slot, so**At the same time, the slot can only be loaded
module**. It follows:

* **Tracker and Tracker Saver work together**: tracksvr reads`struct trk`tracker`0x2B7EC`As long as the tracker is alive in the slot, that’s enough.
  Save Current / Save GPS / Save Start took coordinates.
* **Tracker and tracksvr do not work with GPS/APRS modules**GPS module
  loaded into the same slot and overlaid`struct trk` (`0x2B7EC`) and/or
  The satshare zone. His body ends at an address close to`0x2B7EC`; on coincidence, it wipes the state of the tracker.
* To ensure that the two functions work simultaneously through the application menu
  It is necessary to combine into one module (or switch between modules through the
  mailbox`H9_REQUEST`) rather than keeping separate`.h9mod`.

## Screen and keys

* The screen is 160 × 128, color RGB565.
* Font 6 × 8 points per scale coefficient, only capital:
  lowercase letters are displayed in capital letters. At scale 1 in a row is placed
  26 characters.
* Keys`h9_key`byte`0xC3AF`): d → d + 2, Menu 12, Up 13 .
  Down 14, Back 15,`*`0x13 .`#`0x14. You need an antidrug for two identical
  Reading in a row, he's in`trk_key`.
* On MAP, the numbers 2/4/6/8 shift the map (panning around the center).`*`/`#`They change scale.
* Retention`#`about 0.4 seconds opens the application menu (stab).