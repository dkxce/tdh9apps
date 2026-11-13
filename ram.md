# Адресация памяти рации TD-H9

Карта памяти TIDRADIO TD-H9 (JieLi AC695X). Адреса РАМ — физические
(`0x11AD0` и т.п.); код в flash отображается по `CPU = 0x01E00000 +
(flash − 0x5000)`.

---

## 1. RAM — ключевые адреса

| Адрес | Назначение |
|---|---|
| `0xC3AF` | Байт кнопки стокового скана: **цифра d → d+2**, Menu 12, Up 13, Down 14, Back 15 [HW] |
| `0xC4A1` / `0xC4A7` | Свой APRS callsign (6 байт) / SSID [STATIC] |
| `0x11AD0` | Блок состояния app_core: +100 рек. режим, +101 тек. режим; +138 дескриптор BLE; +6636 DAC-дескриптор [HW] |
| `0x11AD0+0x1BC` | Текущая частота приёма (u32, 10 Гц) [HW, VFO PROBE] |
| `0x11AD0+0x1C8` раст, `+0x1CC` оффсет TX, `+0x789` направление сдвига [STATIC] |
| `0x11AD0+0x1D0` / `0x1D8` | VFO A / VFO B (u32, 10 Гц) [HW, VFO PROBE] |
| `0x11AD0+0x10CD` | Байт диапазона: 1 = UHF, 0 = VHF [HW] |
| `0x12234` | Маркер NOR «ready» = `0x5AA5` [HW] |
| `0x12550` / `0x12558` / `0x12560` | GPS fix: валидность / широта / долгота (signed микро-градусы) [STATIC] |
| `0x13D10` | Последний декодированный AX.25-пакет (324 Б) [STATIC] |
| `0x16634` | APRS-серийный ring (2048 Б; индексы записи `h[0x16634]`→`0x16E34`, чтения `0x16E36`) [HW] |
| `0x1D8B0` | 100-Гц счётчик тиков [HW] |
| `0x1D7D0` | Heap free-bit map (блоки по 128 Б) [STATIC] |
| `0x1D8A8` / `0x1D8AA` | Heap: блоки занято / всего [HW] |
| `0x1E4D8–0x2BF00` | Первый диапазон стокового heap [STATIC] |
| `0x29F00–0x2BEFF` | **Module-loader**: слот модулей 8 КиБ; «run next» mailbox в послед 32 Б (`0x2BEE0`, магия `HRUN`) [HW] |
| `0x2B8B4` | Магия `CSRT` резидентного трекера (satshare) [STATIC] |

---

## 2. Main flash (1 МиБ)

| Диапазон | Назначение |
|---|---|
| `0x00000–0x04FFF` | **Boot region**. HID recovery не может читать/писать [HW] |
| `0x05000–0xD9FFF` | **App image**, зашифрован: каждый 32-Б блок XOR с CRC-потоком ключом `0xF181 ^ ((flash−0x5000)>>2)` [HW] |
| `0xDA000–0xE9FFF` | **Settings VM**, два банка по 32 КиБ (`0xDA000`, `0xE2000`) [STATIC] |
| `0xEA000–0xFBFFF` | Свободно (72 КиБ), plain, выполняется на месте `0x01EE5000` [HW] |
| `0xFC000–0xFFFFF` | Стоковые данные (leave alone) [HW] |

---

## 3. Resource chip (16 МиБ SPI NOR, SPI2)

| Адрес | Назначение | Пишет |
|---|---|---|
| `0x3A7000–0x3FFFFF` | Хвост voice-партиции: стёрт, сток не пишет [HW] | — |
| `0x3FE000` | User VFO backup (лог 16-Б записей) — перекрытие с голосом игры [STATIC] | - |
| `0x3FF000` | **Last GPS fix** (лог `GFX1`, 256×16 Б) [HW] | loader stub |
| `0x717000–0x7FFFFF` | Хвост font-партиции [HW] | — |
| `0x802000`, `0x80B606` | APRS inbox / beacon list (`sys_data`) [STATIC] | сток |
| `0x900000` | `rw_freq`: VFO-настройки, **сток пишет сюда** [HW] | сток |
| `0xA00000` | **Boot logo** (160×128 RGB565, 40 960 Б) [HW] | - |
| `0xA1F000` | Game slot dir (`H9DATA01`), данные `0xA20000–0xCC0000` [HW] | - |
| `0xCC0000` | **Modules slot dir** (`H9MODS01` @ `0xCC1000`, ≤124 КиБ) [HW] | наш `td9_apps.py` |
| `0xCE0000` | Music slot dir, данные `0xCE1000–0xCFFFFF` [HW] | — |

---

## 4. APRS/GNSS-настройки (Settings VM, читается/пишется по BLE)

Инструмент: `td9_aprs.py`. Диапазон настроек в памяти = `0x3066–0x3113`
(смещения от начала Settings VM). Читается по 32-байтным блокам; поля могут
пересекать границы блоков.

| Поле | Смещение | Тип / длина |
|---|---|---|
| GPS switch | `0x3066` | byte |
| Position format | `0x3067` | byte |
| Time zone | `0x3068` | byte |
| Speed / dist / alt unit | `0x3069` / `0x306A` / `0x306B` | byte |
| Fixed position (lat/lon/alt) | `0x306C–0x307B` | 4×u32be + маркеры |
| Station type | `0x307B` | yesno |
| APRS on | `0x307C` | yesno |
| Callsign | `0x307D` | text6 |
| SSID | `0x3083` | byte |
| Icon table | `0x3084` | byte |
| Comment | `0x3095` | text64 |
| Icon symbol | `0x30D6` | byte |
| Mic-E start / type | `0x30D7` / `0x30D8` | yesno / byte |
| Route 1 name / count | `0x30D9` / `0x30E1` | text8 / byte |
| Route 2 name / count | `0x30E2` / `0x30EA` | text8 / byte |
| Reports (volt/sats/mil) | `0x30EB` / `0x30EC` / `0x30ED` | yesno |
| PTT link | `0x30EE` | yesno |
| Timed beacon | `0x30EF` | yesno |
| Beacon interval | `0x30F0` | u16le |
| Digi forward channel | `0x30F2` | byte |
| DIGI1 en / name | `0x30F3` / `0x30F4` | yesno / text8 |
| DIGI2 en / name | `0x30FC` / `0x30FD` | yesno / text8 |
| Forward wait | `0x3105` | byte |
| Remote password | `0x3106` | text8 |
| RX / TX channel | `0x310E` / `0x310F` | byte |
| PTT priority | `0x3110` | yesno |
| RX popup | `0x3113` | yesno |

Примечание: полная карта Settings VM (структура банков и хэдеры) — из
handbook секция 3 [STATIC]; таблица полей — из наших инструментов.

---

## 5. Режимы входа

| Как | Режим |
|---|---|
| **PTT + 2** при включении | Vendor HID `4C4A:4155` (веб-флешер) [HW] |
| Удержание **8** при включении | ROM USB loader `4C4A:2342`, mass storage [HW] |
| **PTT + 1 / +3 / +7** | Serial / USB-serial updater / APRS [HW] |
| Удержание **Back** (модульный loader) | меню приложений [HW] |