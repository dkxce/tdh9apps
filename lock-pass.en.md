# TD-H9 RADIO LOCK / Password locking

Owner's radio security function (for the owner, no bypassing protection).

## Password storage

| Thing. | Meaning. |
| --- | --- |
| Address | flash**`0x716FF8`**8 bytes |
| Where | area before pictures (`0x717000`); last sector font (`0x716000`) |
| Parolene. | all 8 bytes = numbers`0-9`gap`0x20`(The gaps finish the tail, at least 1 digit) |
| Not paroled. | At least one byte is not a number and not a space (mod writes).`0xFF`, erased cell ) |

Record format 8 bytes:
-`[0..len)`- password numbers (1.8),
-`[len..8)`- gaps`0x20`(filling shorter than 8).
- Reset (not passworded): all bytes`0xFF`.

The condition "is parole":`pass_locked`= all bytes {`0-9`, `0x20`} **и**There's at least one number.

## fly

Common flag`skip_check`lives in RAM`0x2B8DF`(free byte, end of satshare zone):`0`- fresh / not set (when power is turned on; on some firmware debris`0xFF`)`1`unlocked by this session (no password or password accepted),`2`- Locked: there is a password, you need to check.

```
<RADIO LOCK FLOW>

skip_check = 0 # включение питания (0x2B8DF)
<stub>
  ├─ (skip_check == 0 || skip_check == 0xFF) -> # первый проход
  │   ├── Читаем 0x716FF8 - 8 байт
  │   ├── (Пароль отсутствует) -> 
  │   │   └─► skip_check = 1 # все ок
  │   └─► (Пароль присутствует) -> 
  │       └─► skip_check = 2 # надо проверить
  │
  ├─ (Удержан # (или *) или skip_check > 1) -> menu app
  │ <menu>
  │   ├── (skip_check == 2) ->
  │   │   ├── (Модуля passcode нет) ->
  │   │   │   ├── skip_check = 1
  │   │   │   └─► Выход из меню, возврат к стоку  
  │   │   └─► (Модуль passcode есть) -> passcode app
  │   │   <passcode>
  │   │       ├─ (skip_check == 2) -> 
  │   │       │   ├─ Читаем 0x716FF8 - 8 байт
  │   │       │   ├─ (Пароль отсутствует) -> 
  │   │       │   │   ├─ skip_check = 1 # все ок
  │   │       │   │   └─► Выход из меню, возврат к стоку  
  │   │       │   └─► (Пароль присутствует) ->
  │   │       │       ├─ ввод и проверка пароля - сверяем  ◀────────┐
  │   │       │       ├─ пароли совпадают ->                        │
  │   │       │       │   ├─ skip_check = 1                         │
  │   │       │       │   └─► Выход из меню, возврат к стоку        │  
  │   │       │       └─► пароли различны ->                        │
  │   │       │           └─► Вернуться к вводу и проверке пароля ──┘
  │   │       └─► (skip_check != 2) -> 
  │   │           └─► меню блокировки рации
  │   │               ├─ разблокировать рацию (удаление пароля), если есть пароль
  │   │               ├─ заблокировать рацию (просмотр, ввод и запись пароля)
  │   │               ├─ заблокировать сейчас ->
  │   │               │   ├─ skip_check = 2
  │   │               │   └─► Выход из меню, возврат к стоку  
  │   │               └─► Вернуться в меню menu app
  │   └─► (skip_check != 2) ->
  │       └─► Вывод меню модулей
  │           ├── passcode
  │           │    └──► passcode: flow аналогичный вышеприведенному
  │           ├──► другой модуль
  │           ├──► ...
  │           └──► другой модуль
  └─► loop()
```

## Implementation

| Component | File. | Status |
| --- | --- | --- |
| Stubble: exhibit`skip_check`along`0x716FF8`+ entrance to the menu by`#`or`skip_check>1` | patch`custom_firmware/stub/stub.s` | working |
| Menu:`skip_check==2`search and launch`PASSLOCK`If you fail-open; if you fail-open.`skip_check!=2`Regular list of modules | `custom_firmware/menu/menu.c`base`menu_base.c`) | working |
| Module`PASSLOCK`: boot branch (`skip_check==2`Input + verification, the usual branch (`menu_loop`: Lock/UNLOCK/LOCK NOW | `myapps/passlock/src/passlock.c` | working |
| Reading through`api->read` | `0x716FF8`8 bytes | confirmed |
| Record password via S2 (erase+program, bypass font protection) | `td9_kpi.py pass / passclear / passread` | working |

Implementation notes:
- stub reads.`0x716FF8`through`ml_read`(stock)`0x01E4FF22`) only on the first passage; further`skip_check`Auto-entry to the menu (if 2) or regular hold-#`0x14`).
-`ml_main`in stub made return (keep frame) - menu and PASSCODE do not drop / do not reclaim the radio when leaving.
- The menu.`skip_check==2`It does not draw a list, but immediately returns the index.`PASSLOCK`(or fail-open)`skip_check=1`in his absence).
-`PASSLOCK`upon`skip_check==2`Only displays the password entry screen, before matching (incorrect password → re-enter).
Locked: Locked Menu (LOCK/UNLOCK/LOCK NOW) is available when you log in to PASSCODE from the module list.`skip_check != 2`).

## Tool (BLE)

```
python td9_kpi.py pass       --password 123456   # установить пароль
python td9_kpi.py passclear                       # сбросить (0xFF)
python td9_kpi.py passread                        # прочитать 8 байт
```

Writes in`0x716FF8`via S2: Programs a 32-byte block without touching the font sector (write only in erased ones).`0xFF`cells.

## It's important.

When you firmware the walkie-talkie module passcode and set the password, it is recommended to leave the walkie-talkie with bluetooth enabled. When you make sure everything is working properly, you can turn it off.

If you try to enter the widget menu when the keyboard is blocked, the radio will be blocked before restarting. (We have to hold on)`#`when the keyboard is locked.
So you can activate the full keyboard lock mode (without the possibility of its unlocking by standard rusting).`*`), the walkie-talkie will be running in effluent mode.
When you activate the full keyboard lock mode at the top of the screen next to the signal scale, a red horizontal line will appear.
To unlock the radio, turn it on and off, enter the password when you turn on (if it is set) and remove the keyboard lock with an asterisk.

If you need to leave the walkie-talkie working in the drain mode but at the same time disable the ability to unlock the keyboard you can
Use a password starting from zero (0). In this case, when power is turned on, the radio will block button retention`*` и `#`.
That is, if you have activated the keyboard lock, you will not be able to unlock the walkie-talkie (complete keyboard lock mode).
When this mode is activated, a yellow horizontal line will appear at the top of the screen next to the signal scale.
To turn off when you turn on the mode of full keyboard lock hold`#`- a password will be requested upon activation.

## Stack.

The following components must be installed to operate the lock function:
- Firmware[TD-H9-V1.0.38DKXCE.fw](custom_firmware/TD-H9-V1.0.38DKXCE.fw)- Menu.[menu. h9mod](myapps/menu.h9mod)- Application module[passlock. h9mod](myapps/passlock.h9mod)