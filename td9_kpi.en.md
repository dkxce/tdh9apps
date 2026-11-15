# TD-H9 Bluetooth Image Package (DKXCEPIC)

`td9_kpi.py`load**picture-set**on resource flash radios TD-H9
Bluetooth. Pictures are stored in a free area - the "tail" of the font party
?`0x717000..0x7FFFFF`), which the stock firmware does not use.

Watch pictures on the radio will be an app**DKXCEPIC**(Back menu)
DKXCEPIC; Up/Down scrolls through the loop, Back is the output.

## Format of the field

Title + pictures (raw RGB565 big-endian):

```
0x717000  "DKXCEPI"    7 байт magic
0x717007  count         1 байт, число картинок далее
0x717008  картинка 0    160×128 RGB565 big-endian (40 960 Б)
          картинка 1    ...
          ...
```

## Teams.

All through.`poetry run`.

### Check that the area is free

```
poetry run python td9_kpi.py check [--addr 0x717000] [--span 0x1000]
```

* Default reads 4 KBS`0x717000`.
* If there is already a DKXCEPIC packet, it recognizes it (a number of pictures).
  size, how much is free after and offers`write`/`forcewrite`/`erase`.

### Record pictures.

```
poetry run python td9_kpi.py write a.png b.jpg c.bin [--addr 0x717000]
```

* `png/jpg`Pillow: resize to 160×128 → RGB565`.bin`- ready-made raw.
* First reads the area:
* if already recorded**identically**- omitted (Already written identically);
* if**busy**other – erases the occupied sectors and writes;
* if**loosely**- She writes right away.
* At the end, the record is verified (high-speed bulk reading).

### It has to be written down and written down.

```
poetry run python td9_kpi.py forcewrite a.png b.png [--addr ...]
```

* **Always laundry**sectors before recording without relying on
  content check.

### Erase recorded pictures

```
poetry run python td9_kpi.py erase [--addr 0x717000]
```

* If`--addr`DKXCEPIC packet - reads the count, calculates the exact
  size and erases exactly the sectors of the package.
* If there is no headline, erase it.`--span`(Default 4 KB).
* Once erased, the sectors correspond to 0xFF - the area is free again.

### Unload area bytes

```
poetry run python td9_kpi.py dump --addr 0x717000 --span 0x1000 --out файл.bin
```

## Parameters

| Argument | By default. | Appointment |
| --- | --- | --- |
| `--addr` | `0x717000` | field address |
| `--span` | `0x1000` | size`check`/`dump`(4 KIB) |
| `--out` | `kpi-dump.bin` | output`dump` |

## Speed and protocol

* Write via S2 Write Service over 32-byte blocks
  (erase → program → verify)`td9_logo.py`.
* **Reading (check/check/check/dump)**exploit**bulk-read (S1 fast)**:
  The radio streams data in packs of ~240B per request instead of 32B.
  verification is several times faster (about 3-4 KB / s instead of long ones).
  32-byte cycles.

## Spelling errors (important)

* After`write`see`read timeout`on`Verifying`- It's old.
  32B read problem; restart (now verified is bulk)
* `erase`/`write`pre-erase**sector-specific CRC**into
  erase-command and check the result; if the radio rejects – will appear
  A cause message (e.g., “sector CRC mismatch”) rather than a false success.

## Dependencies

`pip install bleak`? +`Pillow`for png/jpg.
Radio: firmware with write service (S2), Bluetooth programming mode.