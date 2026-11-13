# Приложения TD-H9: установка и удаление

`td9_apps.py` — загрузка и удаление приложений (модулей H9 Toolbox) на рацию
TD-H9 по Bluetooth через **S2 write service**. Рация должна быть на прошивке
**H9 Toolbox Firmware** (у неё есть write service v2).

## Команды

Все примеры через `poetry run`, как остальные инструменты проекта.

### Список доступных приложений
```
poetry run python td9_apps.py list
```
Показывает два источника:
- `apps_catalog/` — готовые приложения из каталога h9toolbox;
- `myapps/` — собранные ваши приложения.

Пункт `menu` — системный (это сам список меню), не видимое приложение.

### Установить приложение(я)
```
poetry run python td9_apps.py install gps
poetry run python td9_apps.py install bootscreen
poetry run python td9_apps.py install gps apron aprs_monitor vfo_probe
```
- Ищет `.h9mod` сначала в `apps_catalog/`, затем в `myapps/`.
- Меню добавляется автоматически (модуль 0);
- Пакет пишется в modules-слот ресурсной flash
  (каталог `0xCC0000`, данные `0xCC1000`), читается назад и проверяется,
  затем ставится маркер `H9DATA-COMPLETE`.

После установки: удерживайте **Back** на рации → меню с приложениями.

### Прочитать, что установлено
```
poetry run python td9_apps.py peek
```
Показывает каталог modules-слота: имя, размер, завершённость загрузки.

### Удалить все приложения
```
poetry run python td9_apps.py remove
```
Стирает сектора, указанные в каталоге, и возвращает их к 0xFF.

## Как писать свои приложения
Исходники лежат в `myapps/<имя>/src/*.c`, общий слой и тулчейн — в
`myapps/shared/`. Сборка идёт через GitHub Actions
(`.github/workflows/build-module.yml`): скачивает JieLi toolchain,
вызывает `myapps/build_all.py`, артефакт `myapps/*.h9mod`.

Новое приложение = создать `myapps/<имя>/src/<имя>.c`
(с функцией `module_main`), запушить, прогнать workflow.

Для приложений из нескольких модулей — добавить `myapps/<имя>/build.json`
со списком модулей и их исходников.

## Требования
- Рация: H9 Toolbox Firmware (write service v2),
- Bluetooth-режим рации включён;
- `pip install bleak`.