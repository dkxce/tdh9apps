# TD-H9 автономная блокировка — материалы для сборки

Владельческая функция безопасности рации (для владельца, не обход защиты).
Два изменения, которые вдвоём дают автономную блокировку при включении:

1. **`stub/`** — патч загрузчика (`stub.s`) в прошивке H9 Toolbox.
   При каждом включении/перезапуске проверяет пароль в resource flash
   `0x716FF8`; если рация залочена и ещё не разблокирована в этой сессии —
   запускает модуль `PASSLOCK` (ввод+отрисовка), сам не рисует экран.
   Сессионный флаг разблокировки — spare-байт `0x2B8DF` в satshare
   (переживает только soft-reset `cpu_reset`; после холодного старта,
   т.е. выключения/включения питания, флаг снова требует ввод пароля).
2. **`passlock/`** — обновление модуля `PASSLOCK`: добавлен **unlock-only**
   режим при вызове `module_main(api, 0xFFFF)` — жёсткий экран без выхода
   до верного пароля (меню/личный сброс недоступны).

## Файлы

| Файл | Назначение |
|---|---|
| `stub/stub.s` | полный источник загрузчика с lock-проверкой (для копирования в `module-loader/src/stub.s`) |
| `stub/stub.patch` | git-diff патч к текущему `stub.s` репозитория h9toolbox |
| `passlock/passlock.c` | модуль PASSLOCK с unlock-only веткой (для копирования в `myapps/passlock/src/passlock.c`) |
| `build-lock.yml` | GitHub Actions workflow: сборка прошивки со stub через ваш CI (см. ниже) |

## Готовые сборки

| Файл | Назначение |
|---|---|
| [TD-H9-V1.0.38_dkxce_passlock.fw](TD-H9-V1.0.38_dkxce_passlock.fw) | Поддержка passlock, загрузка через Back |
| [TD-H9-V1.0.38_dkxce_passlock_sharp.fw.fw](TD-H9-V1.0.38_dkxce_passlock_sharp.fw.fw) | Поддержка passlock, загрузка через # |

## Автосборка через GitHub Actions (рекомендуется)

Без доступа к `melidosian/h9toolbox` вам не нужен: workflow `build-lock.yml`
клонирует его как публичный vendor, применяет `stub/stub.patch`, извлекает
stock `app.bin` из `firmware/TD-H9-V1.0.38.fw` в вашем репо, скачивает JieLi
toolchain из вашего релиза `toolchain` и собирает прошивку.

Установка:
1. Положите `custom_firmware/` в репозиторий `dkxce/tdh9apps` (рядом с `firmware/`).
2. Скопируйте `custom_firmware/build-lock.yml` в
   `.github/workflows/build_lock.yml`.
3. Убедитесь, что в релизе `tdh9apps` с тегом `toolchain` есть asset `.tar.xz`
   с тулчейном (папка внутри содержит `pi32v2/bin/clang`).
4. Actions → **Build TD-H9 lock firmware** → Run workflow.
5. Артефакт `lock-firmware` содержит `h9_toolbox_modules_OFFLINE_EXPERIMENT.fw`
   (прошивка со stub), `h9_modules.bin` и манифест.

Заливка: `h9_flash.py --fw <файл.fw>` (HID, PTT+2).

Учтите: та же прошивка собирает и модули (меню/APRS/...). Модуль PASSLOCK со
строкой unlock-only (self==0xFFFF) собирается отдельным workflow
`Build myapps modules` из `myapps/passlock/src/passlock.c` и устанавливается
через App Manager.

## Куда класть и как собрать (вручную)

### 1. Загрузчик (`stub.s`)
Репозиторий: `melidosian/h9toolbox`.
- Скопируйте `stub/stub.s` в `firmware-research/module-loader/src/stub.s`
  (или примените `stub.patch`: `git apply stub.patch`).
- Соберите прошивку: см. `firmware-research/module-loader/README.md`.
  ```
  JIELI_TOOLCHAIN=<toolchain> python3 -I build.py --stock-image <app.bin> --stock-package <TD-H9-V1.0.38.fw>
  ```
- Проверка локально (без тулчейна): ассемблер и линкер собираются
  (stub = 1448 байт, каверна = 1632, запас 184 байта).

### 2. Модуль (`passlock.c`)
Репозиторий: `dkxce/tdh9apps` (TDH9Unbrick).
- Скопируйте `passlock/passlock.c` в `myapps/passlock/src/passlock.c`.
- Соберите модуль через CI `Build myapps modules`
  (`build_one.py passlock`), получите `passlock.h9mod`, установите на рацию.

## Порядок на рации

1. Залить прошивку с новым stub и установить `passlock.h9mod`.
2. Установить/проверить пароль: `python td9_kpi.py pass --password 123456`.
3. Выключить и включить рацию (cold boot). Если залочена — появится экран
   ввода; без верного пароля рация не работает.
4. `passclear` через BLE сбрасывает пароль (владелец).

## Примечание по безопасности

- Логика написана так, что при отсутствии установленного модуля `PASSLOCK`
  залоченная рация **не разблокируется молча** — она остаётся залоченной
  (флаг сессии ставится только после реального прогона модуля).
- Не сообщено подсказок/методов обхода: это штатная функция владельца.