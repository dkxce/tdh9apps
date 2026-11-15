# TD-H9 applications: installation and disposal

`td9_apps.py`Download and remove applications (H9 Toolbox modules) on the radio
TD-H9 via Bluetooth**S2 write service**. The radio should be on firmware.**H9 Toolbox Firmware**(It has written service v2).

## Teams.

All examples through`poetry run`Like the other tools of the project.

### List of available applications

```
poetry run python td9_apps.py list
```

It shows two sources:
-`apps_catalog/`- ready-made applications from the h9toolbox catalog;
-`myapps/`- Collected extended applications.

Item`menu`System (this is the menu list itself), not a visible application.

### Install annex(s)

```
poetry run python td9_apps.py install gps
poetry run python td9_apps.py install bootscreen
poetry run python td9_apps.py install gps apron aprs_monitor vfo_probe
```

* Seeking.`.h9mod`first`apps_catalog/`then`myapps/`.
* The menu is added automatically (module 0).
* The package is written in modules-slot resource flash
  catalogue`0xCC0000`data`0xCC1000`), read back and checked,
  marker`H9DATA-COMPLETE`.

After installation: hold**Back**on the radio → menu with applications.

### Read what is established

```
poetry run python td9_apps.py peek
```

Shows the modules-slot catalog: name, size, completeness of the download.

### Delete all applications

```
poetry run python td9_apps.py remove
```

Erase the sectors listed in the directory and return them to 0xFF.

## How to Write Your Applications

Sources lie in`myapps/<имя>/src/*.c`, common layer and tulchain - in`myapps/shared/`. It is built through GitHub Actions.
?`.github/workflows/build-module.yml`): downloads JieLi toolchain,
trigger`myapps/build_all.py`artifact`myapps/*.h9mod`.

New Application = Create a New Application`myapps/<имя>/src/<имя>.c`function`module_main`), start, run workflow.

For applications of several modules - add`myapps/<имя>/build.json`with a list of modules and their source.

## Requirements

* Radio: H9 Toolbox Firmware (write service v2)
* Bluetooth radio mode on;
* `pip install bleak`.