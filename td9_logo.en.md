# Change boot logo TD-H9 over Bluetooth

`td9_logo.py`read and write**boot logo**TD-H9 radios on resource flash
Bluetooth via flash resource service (S1 read / S2 write).

## Logo format

| Parameter | Meaning. |
| --- | --- |
| Address on a chip | `0xA00000` |
| Size | 160 x 128 px |
| Format | Raw RGB565 big-endian (without title) |
| Size of bytes | 40,960 B (10 sectors of 4 KB) |

## Teams.

All examples through`poetry run`.

### Read the current logo/backup

```
poetry run python td9_logo.py read [--output имя_файла]
```

* Save the current logo to the disc as`td9-logo.bin`or`--output`).
* Useful.**beforehand**Save the original to return.

### Write down a new logo

```
poetry run python td9_logo.py write картинка.png [--dither]
poetry run python td9_logo.py write лого.bin
```

| Argument | Name. | What's he doing? |
| --- | --- | --- |
| `картинка.png/jpg` | — | Pillow: resize to 160×128 (LANCZOS) → RGB565 big-endian |
| `--dither` | flag | Floyd-Steinberg gradient dithering (fewer bands) |
| `лого.bin` | — | finished raw RGB565 160×128 (40,960B), used as is |

Script before recording:
1. check the write service (S2, version 2);
2.**reads the current logo**to plan sectors for rewriting;
3. erase and program only changed sectors;
4. reads back and compares.

## Comments

* **Requires firmware with write service**(H9 Toolbox / Custom) Without her (stock S1)
  The record is unavailable, he writes about it.
* `--backup`defined in the interface, but automatic backup
  copy`write` **unrealized**. Make it for the backup.`read`manually:`poetry run python td9_logo.py read -o my-original-logo.bin`
* Installation of images in a package - see.`td9_kpi.py`(DKXCEPIC)

## Dependencies

`pip install bleak`? +`Pillow`for png/jpg.
Radio enabled + Bluetooth programming mode.