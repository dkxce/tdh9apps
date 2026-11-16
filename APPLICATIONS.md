# Tidradio TD-H9 Руководство по установке виджетов

[РАЗКИРПИЧИТЬ РУК-ВО](README.md) | [UNBRICK MANUAL](README.en.md) | [RELEASES](https://github.com/dkxce/tdh9apps/releases)            
[ВИДЖЕТЫ РУК-ВО](APPLICATIONS.md) | [WIDGETS MANUAL](APPLICATIONS.md) | [RELEASES](https://github.com/dkxce/tdh9apps/releases)   

Скопируйте из [релизов](https://github.com/dkxce/tdh9apps/releases) **кастомную прошивку** для
TIDRADIO TD-H9 с набором инструментов для управления приложениями.

> В этой инструкции: «меню приложений» открывается **удержанием `#`**
> на рации. Back удерживать не нужно.

---

## Что есть в архиве

| Файл | Назначение |
|---|---|
| `firmware/*.fw` | **Кастомная прошивка** (с поддержкой блокировки по паролю и менеджером приложений) |
| `firmware/Update App.exe` | официальный прошивальщик (заливка `.fw`) |
| `h9_ui_flash.exe` | раскирпичиватель / прошивальщик через HID (`--help` для режимов) |
| `td9_apps.exe` | управление приложениями (установка/удаление `.h9mod`) |
| `td9_kpi.exe` | загрузка набора картинок (`DKXCEPIC`) + установка/сброс пароля рации |
| `td9_logo.exe` | смена boot-логотипа |
| `td9_aprs.exe` | чтение/запись APRS/GNSS-настроек |
| `myapps/*.h9mod` | готовые модули приложений (`passlock`, `dkxcepic`, `bootscreen`, `tracking`) |
| `hidapi.dll` | библиотека для связи по Bluetooth (должна лежать рядом с `.exe`) |

---

# Часть 1. Для обычных пользователей (готовые .exe)

Никакого Python не нужно. Достаточно распаковать архив и запускать `.exe`.

## 1.1. Прошить кастомную прошивку

1. Распакуйте архив в отдельную папку.
2. Зайдите в папку `firmware`.
3. Запустите **`Update App.exe`** (официальный прошивальщик).
4. Переведите рацию в режим прошивки: **выключите**, зажмите **PTT + кнопку `[2]`**
   и, не отпуская, включите рацию — она определится как HID-устройство.
   Либо обычном режиме удерживайте **8**.
   Подключите по USB (кабель с передачей данных).
5. Выберите файл **`TD-H9-V1.0.38DKXCE.fw`** или другой с расширением **.fw** и нажмите прошить.
6. После завершения отключите USB и выключите/включите рацию.

> Убедиться, что прошивка применилась, просто: удержание `#` открывает меню
> приложений (стоковая прошивка такого меню не имеет).
> Ввод DTMF-кодов отключится.

## 1.2. Установить приложения (td9_apps.exe)

`td9_apps.exe` — консольная утилита. Команды вводятся в **командной строке
(cmd/PowerShell)** внутри папки архива.

Включите на рации Bluetooth (меню рации → Bluetooth → инклуд/включить), держите её рядом.

Показать доступные приложения:
```
td9_apps.exe list
```

Установить нужные (имена берём из вывода `list`; меню ставится автоматически):
```
td9_apps.exe install dkxcepic bootscreen tracking
```

Посмотреть, что установлено:
```
td9_apps.exe peek
```

Удалить всё:
```
td9_apps.exe remove
```

После установки откройте **меню приложений** (удержание `#`) — там появятся
установленные модули.

> Совет: для того чтобы запаролить рацию необходимо устновить модуль `passlock`.

## 1.3. Загрузить картинки на рацию (td9_kpi.exe)

Устанавливает набор картинок для просмотрщика `DKXCEPIC`.

```
td9_kpi.exe check
td9_kpi.exe write photo1.png photo2.jpg
```

- Картинки автоматически сжимаются до 160×128 и конвертируются в формат рации.
- После записи найдите **DKXCEPIC** в меню приложений (удержание `#`):
  Up/Down — листать, Back — выход.

Удалить картинки:
```
td9_kpi.exe erase
```

## 1.4. Сменить boot-логотип (td9_logo.exe)

Изображение, которое видно при включении рации.

Сначала сохраните оригинал (важно, чтобы потом вернуть):
```
td9_logo.exe read -o my-original-logo.bin
```

Записать новое изображение:
```
td9_logo.exe write logo.png
```

> Картинка будет уменьшена до 160×128; для плавных градиентов добавьте `--dither`:
> `td9_logo.exe write logo.png --dither`.

## 1.5. Задать пароль рации (td9_kpi.exe)

Кастомная прошивка умеет блокировать рацию паролем (по желанию).

```
td9_kpi.exe pass --password 1234
```

- Пароль — от 1 до 8 цифр.
- После установки выключите и включите рацию: при включении спросит пароль.
- Снять блокировку: `td9_kpi.exe passclear`.

> Блокировка работает только при установленном приложении `passlock`.
> Не теряйте пароль и держите Bluetooth включённым, пока проверяете, что всё работает.

## 1.6. Сбросить «кирпич» (h9_ui_flash.exe)

Если рация перестала загружаться — вы можете перепрошить её через HID:
```
h9_ui_flash.exe --help
```

---

# Часть 2. Для тех, кто хочет запускать Python-скрипты

Та же функциональность, но исходные скрипты (`td9_apps.py`, `td9_kpi.py`,
`td9_logo.py`, `td9_aprs.py`) вместо `.exe`.

## 2.1. Подготовка окружения

Нужен Python **3.10+** (рекомендуется 3.11) и `pip`.

```powershell
# 1) Склонировать проект (или взять исходники из репозитория)
git clone https://github.com/dkxce/tdh9apps
cd tdh9apps

# 2) Создать виртуальное окружение и поставить зависимости
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt      # поэт: см. pyproject.toml / poetry install
```

Проект управляется через **poetry** (если он у вас есть):
```powershell
poetry install
```

Все дальнейшие команды — через `poetry run python ...` (или напрямую, если
окружение активировано).

## 2.2. Прошивка (HID)

Флешер работает через HID-режим (PTT+2 при включении):
```
poetry run python h9_flash.py --fw firmware/TD-H9-V1.0.38DKXCE.fw --backup before.bin
```

Справка по флагам: `h9_flash.py --help`.

## 2.3. Приложения (td9_apps.py)

```
poetry run python td9_apps.py list
poetry run python td9_apps.py install dkxcepic bootscreen tracking
poetry run python td9_apps.py peek
poetry run python td9_apps.py remove
```

## 2.4. Картинки (td9_kpi.py)

```
poetry run python td9_kpi.py check
poetry run python td9_kpi.py write photo1.png photo2.jpg
poetry run python td9_kpi.py erase
```

## 2.5. Boot-логотип (td9_logo.py)

```
poetry run python td9_logo.py read -o my-original-logo.bin
poetry run python td9_logo.py write logo.png [--dither]
```

## 2.6. Пароль (td9_kpi.py)

```
poetry run python td9_kpi.py pass --password 1234
poetry run python td9_kpi.py passclear
```

## 2.7. APRS-настройки (td9_aprs.py)

```
poetry run python td9_aprs.py show
poetry run python td9_aprs.py get callsign
poetry run python td9_aprs.py set callsign R2AKA
```

---

# Часть 3. Для тех, кто хочет собрать прошивку и приложения сам

Всё собирается вашим GitHub Actions из исходников.

## 3.1. Структура проекта

```
tdh9apps/
├── firmware/            # исходный сток-пакет .fw для извлечения app.bin
├── custom_firmware/     # патчи: stub/, menu/ + CI прошивки
├── myapps/              # собственные модули: <app>/src/<app>.c
│   └── shared/          # общий слой (gfx, font, h9mod, ...)
├── .github/workflows/   # CI: сборка модулей и прошивки
└── *.py                 # скрипты-утилиты (td9_apps, td9_kpi, ...)
```

## 3.2. Про сборку модулей (демо: новый модуль)

Модули — C-файлы без libc, линкуются под jeli-тулчейн. 
Новый модуль = просто создать каталог с одним `.c`:

```
myapps/<app>/src/<app>.c      # функция module_main(const struct h9_api*, unsigned)
```

CI workflow `build-module.yml` (GitHub Actions) скачает JieLi-тулчейн из
релиза `toolchain`, вызовет `build_one.py <app>` и отдаст `myapps/<app>.h9mod`.

Многоходовое приложение — добавить `myapps/<app>/build.json` со списком
модулей и их исходников (пример — `myapps/tracking`).

Загрузить своё: `td9_apps.py install <app>`.

## 3.3. Про сборку прошивки

Прошивка собирается в CI (`build-lock-firmware.yml`): берётся сток
`firmware/TD-H9-V1.0.38.fw`, из него извлекается `app.bin`, поверх накладывается
ваш `custom_firmware/stub/stub.s` (патч), ставится `custom_firmware/menu/menu.c`,
и собирается прошивка со встроенным менеджером приложений.

Локально (без GitHub) — см. `custom_firmware/README.md`:
```
JIELI_TOOLCHAIN=<toolchain> python -I firmware-research/.../build.py \
  --stock-image <app.bin> --stock-package <fw>
```

## 3.4 Про сборку меню приложений

Меню приложений собирается в том же CI что и прошивка (`build-lock-firmware.yml`): 
для ее корректной сборки нужен актуальный stub.

## 3.5. Стек блокировки и модулей (память)

- `menu.h9mod` (модуль 0) — список приложений; обязателен для меню по `#`.
- `passlock.h9mod` — экран ввода пароля; работает вместе с `menu.h9mod`
  и stub-прошивкой. Флаг блокировки `skip_check` лежит в satshare `0x2B8DF`.
- RAM-слот модуля: `0x29F00..0x2BEFF` (8 КиБ); состояние трекера — под
  почтовым ящиком `0x2BEE0` (`struct trk`, `STATE_BYTES=0x6F4`).

---

## Требования и замечания

- **Bluetooth**: для всех `.exe`/`.py` (кроме прошивки HID) рация должна быть
  включена, Bluetooth активен, рация рядом с ПК.
- **hidapi.dll**: должна лежать рядом с `.exe` (в архиве она уже есть в корне).
- При использовании прошивки с блокировкой держите Bluetooth включённым, пока
  не убедитесь, что `pass`/`passclear` работают корректно.
- Удержание `#` открывает меню приложений. Модули, установленные через
  `td9_apps`, появляются в этом меню.

---

## Приложение: адреса и привязки (кратко)

| Что | Значение |
|---|---|
| Область картинок DKXCEPIC | resource flash `0x717000` |
| Boot-логотип | resource flash `0xA00000` (160×128 RGB565) |
| Флаг блокировки `skip_check` | RAM `0x2B8DF` |
| Модули | resource flash `0xCC0000` (каталог), данные `0xCC1000` |
| RAM-слот модуля | `0x29F00..0x2BEFF` (8 КиБ) |

---

## Credits & Acknowledgments

Приложения основаны на тулбоксе и кастомной прошивке [melidosian/h9toolbox](https://github.com/melidosian/h9toolbox).    

---

**Автор**: [dkxce](https://github.com/dkxce)     
**Android**: [dkxce/tdh9android](https://github.com/dkxce/tdh9android)      
**Desktop**: [dkxce/tdh9desktop](https://github.com/dkxce/tdh9desktop)      
**Manual**: [dkxce/Tidradio-TD-H9](https://github.com/dkxce/Tidradio-TD-H9)      
