# TD-H9 RADIO LOCK / Password locking

Owner's radio safety feature (for the owner, no bypassing protection).

## Password storage

| Thing. | Meaning. |
| --- | --- |
| Address | flash**`0x716FF8`**8 bytes |
| Where | region before pictures (`0x717000`); last sector font (`0x716000`) |
| Parolene. | all 8 bytes = numbers`0-9`gap`0x20`(The gaps finish the tail, at least 1 digit) |
| Not paroled. | At least one byte is not a number and not a space (mod writes).`0xFF`, erased cell ) |

Record format 8 bytes:
-`[0..len)`- password numbers (1.8),
-`[len..8)`- gaps`0x20`(filling shorter than 8).
- Reset (not passworded): all bytes`0xFF`.

The condition "is parole":`pass_locked`= all bytes {`0-9`, `0x20`} **и**There's at least one number.

## fly

Common flag`skip_check`lives in RAM`0x2B8DF`(free byte, end of satshare zone):`0`- fresh / not specified (when power is turned on; on some firmware debris`0xFF`)`1`unlocked by this session (no password or password accepted),`2`There is a password, you need to check.

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
| Stub: Exposition`skip_check`along`0x716FF8`+ entrance to the menu by`#`or`skip_check>1` | patch`custom_firmware/stub/stub.s` | working |
| Menu:`skip_check==2`search and launch`PASSLOCK`If you fail-open; if you fail-open.`skip_check!=2`Regular list of modules | `custom_firmware/menu/menu.c`base`menu_base.c`) | working |
| Module`PASSLOCK`: boot branch (`skip_check==2`Input + verification, the usual branch (`menu_loop`: Lock/UNLOCK/LOCK NOW | `myapps/passlock/src/passlock.c` | working |
| Reading through`api->read` | `0x716FF8`8 bytes | confirmed |
| Record password via S2 (erase+program, bypass font protection) | `td9_kpi.py pass / passclear / passread` | working |

## Tool (BLE)

```
python td9_kpi.py pass       --password 123456   # установить пароль
python td9_kpi.py passclear                      # сбросить (0xFF)
python td9_kpi.py passread                       # прочитать 8 байт
```

Writes in`0x716FF8`via S2: Programs a 32-byte block without touching the font sector (write only in erased ones)`0xFF`cells.

## It's important.

When you firmware the walkie-talkie module passcode and set the password, it is recommended to leave the walkie-talkie with bluetooth enabled. When you make sure everything is working properly, you can turn it off.

## Features of use

Passwording of the radio takes place through the PASSLOCK module.

At work:
- If the radio is paroled and the keyboard is blocked, then when rusting,`#`radio activates the full keyboard lock mode**kbhardlock**.
If the radio is passworded and is in full keyboard lock mode**kbhardlock**You can unblock it with standard rust.`*`and entering a password.

When activated:
If the radio is passworded and the keyboard is not blocked, the radio will ask for a password, the work of the stock firmware is blocked.
If the radio is passworded and the keyboard is blocked, the radio activates the full keyboard lock mode.**kbhardlock**You can unblock it with standard rust.`*`and entering a password.
If the radio is passworded and the keyboard is blocked, then when it is turned on with retention`#`The radio will ask for a password to exit the full keyboard lock mode**kbhardlock**.

Status alarm:
When activating the full keyboard lock mode**kbhardlock**retention`#`When the keyboard is locked at the top of the screen next to the signal scale, a red horizontal line will appear.
When activating the full keyboard lock mode**kbhardlock**and refusal to enter the unlock password in the upper part of the screen next to the signal scale will appear purple horizontal line.
When activating the full keyboard lock mode**kbhardlock**When the radio is turned on, a purple horizontal line will appear at the top of the screen next to the signal scale.
- On hold.`#`If the keyboard is locked and the radio is not passworded, a blue horizontal line will appear at the top of the screen next to the signal scale.
- On hold.`#`If the keyboard is not blocked, the application menu will be opened.

You can use the full keyboard lock mode.**kbhardlock**if
You need to leave the radio working in the drain mode, but at the same time turn off
The ability to unlock the keyboard.

Flow lock and unlock the radio.

```
                    ┌─────────────────────────────────────────────┐
                    │         ВКЛЮЧЕНИЕ рации (power on)          │
                    └─────────────────────────────────────────────┘
                                           │
                                           ▼
                    ┌─────────────────────────────────────────────┐
                    │            Рация запаролена?                │
                    └─────────────────────────────────────────────┘
                    Нет │                                      │ Да
                        ▼                                      ▼
      ┌──────────────────────────────┐   ┌─────────────────────────────────────────┐
      │  РАБОТА (стоково, без пароля)│   │  Клавиатура заблокирована (KB_LOCKED)?  │
      └──────────────────────────────┘   └─────────────────────────────────────────┘
                        │               Нет │                          │ Да
                        │                   ▼                          ▼
                        │   ┌──────────────────────────────┐   ┌──────────────────────────────────────┐
                        │   │  ВОПРОС ПАРОЛЯ (startup)     │   │  Активируется kbhardlock             │
                        │   │  стоковая работа блокируется │   │  ─ фиолетовая линия у шкалы сигнала  │
                        │   └──────────────────────────────┘   └──────────────────────────────────────┘
                        │                   │                                                    │
                        │                   ▼                                                    │
                        │   ┌──────────────────────────────┐                                     │
                        │   │  Корректный пароль?          │                                     │
                        │   └──────────────────────────────┘                                     │
                        │        Нет │                │ Да                                       │
                        │            ▼                ▼                                          │
                        │    повторить ввод       РАЗБЛОКИРОВАНО                                 │
                        │  (рация не даёт идти)       ▼                                          │
                        │                    ┌──────────────────────────┐                        │
                        │                    │  РАБОТА (стоково)        │                        │
                        │                    └──────────────────────────┘                        │
                        │                              │                                         │
                        │                 (в любой момент, при работе)                           │
                        │                              ▼                                         │
                        │                 ┌───────────────────┐                                  │
                        │                 │  удержание `#`    │                                  │
                        │                 └───────────────────┘                                  │
                        │                           │                                            │ 
                        │                           ▼                                            │
                        │                      Запаролена?                                       │
                        │                 Нет │           │ Да                                   │
                        │                     ▼           ▼                                      │
                        │                ┌───────────┐ ┌────────────────────────────────────┐    │
                        │                │ ОТКРЫТО   │ │   Активируется kbhardlock          │    │
                        │                │ меню      │ │  ─ красная линия у шкалы сигнала   │    │
                        │                │ приложений│ └────────────────────────────────────┘    │
                        │                └───────────┘                │                          │ 
                        │                                             │      ┌───────────────────┘ 
                        │                                             ▼      ▼
                        │                        ┌───────────────────────────────┐
                        │                        │ Удержание `*` + ввод пароля   │
                        │                        │ (стандартная разблокировка)   │
                        │                        └───────────────────────────────┘
                        │                       Отмена   │              РАЗБЛОКИРОВАНО
                        │                                ▼                   │
                        │               ┌───────────────────────────┐        │
                        │               │  Активируется kbhardlock  │        │
                        │               └───────────────────────────┘        │
                        │                                                    ▼
                        ▼
```

Flow unlock the radio when the keyboard is locked when turned on.

```
Питание + удержание `#` (рация уже запаролена + клавиатура заблокирована)
        │
        ▼
┌────────────────────────────────────────────────┐
│  ВОПРОС ПАРОЛЯ (запрос на выход из kbhardlock) │
└────────────────────────────────────────────────┘
        │
        ▼
┌────────────────────────────────────────────────┐
│  Правильный пароль?                            │
└────────────────────────────────────────────────┘
   Нет │              │ Да                  │ Отмена
       ▼              ▼                     ▼
  повтор      ┌─────────────────────┐     ┌───────────────────────────┐
              │  выход из kbhardlock│     │  Активируется kbhardlock  │
              │  → обычная работа   │     └───────────────────────────┘
              └─────────────────────┘
```

## Stack.

The following components must be installed to operate the lock function:
- The firmware is one of
(a)[TD-H9-V1.0.38DKXCE.fw](custom_firmware/TD-H9-V1.0.38DKXCE.fw)(b)[TD-H9-V1.0.38DKXCE APRS mute.fw](custom_firmware/TD-H9-V1.0.38DKXCE_APRS_mute.fw)(c)[TD-H9-V1.0.38DKXCE APRS mute bb beacon.fw](custom_firmware/TD-H9-V1.0.38DKXCE_APRS_mute_bb_beacon.fw)- Menu.[menu. h9mod](myapps/menu.h9mod)- Application module[passlock. h9mod](myapps/passlock.h9mod)