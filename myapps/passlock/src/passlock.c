/* passlock.c: on-radio lock-password manager for the TD-H9.
 *
 * The lock password lives in the resource flash just before the picture
 * area: 8 bytes at 0x716FF8.
 *   - UNLOCKED: at least one byte is not a digit '0'-'9' and not a space
 *     (0x20). The mod/stock writes 0xFF (erased) when the radio is unlocked.
 *   - LOCKED: every byte is a digit or a space (trailing spaces pad a
 *     password shorter than 8 digits; at least one digit is required).
 *
 * Menu flow (opened by holding Back -> run this module from the toolbox):
 *   - If locked, it asks for the password first and only lets you into the
 *     lock menu when the digits match (or the password is cleared).
 *   - The lock menu offers:
 *        LOCK   - enter a new password (up to 8 digits) and write it.
 *        UNLOCK - clear the password (write 0xFF, erased state).
 *   - Back exits to the toolbox menu.
 *
 * Freestanding: no globals, no libc. Pi32v2, linked for RAM 0x29F00.
 * write_pass() erases the whole lock sector (0x716000, verified free apart
 * from the 8 lock bytes) and programs the record directly via the stock
 * resource-flash erase/program calls (0x01E50000 / 0x01E5006E).
 *
 * <RADIO LOCK FLOW>
 *
 * skip_check = 0 # включение питания (0x2B8DF)
 * <stub>
 *   ├─ (skip_check == 0 || skip_check == 0xFF) -> # первый проход
 *   │   ├── Читаем 0x716FF8 - 8 байт
 *   │   ├── (Пароль отсутствует) -> 
 *   │   │   └─► skip_check = 1 # все ок
 *   │   └─► (Пароль присутствует) -> 
 *   │       └─► skip_check = 2 # надо проверить
 *   │
 *   ├─ (Удержан back или skip_check > 1) -> menu app
 *   │ <menu>
 *   │   ├── (skip_check == 2) ->
 *   │   │   ├── (Модуля passcode нет) ->
 *   │   │   │   ├── skip_check = 1
 *   │   │   │   └─► Выход из меню, возврат к стоку  
 *   │   │   └─► (Модуль passcode есть) -> passcode app
 *   │   │   <passcode>
 *   │   │       ├─ (skip_check == 2) -> 
 *   │   │       │   ├─ Читаем 0x716FF8 - 8 байт
 *   │   │       │   ├─ (Пароль отсутствует) -> 
 *   │   │       │   │   ├─ skip_check = 1 # все ок
 *   │   │       │   │   └─► Выход из меню, возврат к стоку  
 *   │   │       │   └─► (Пароль присутствует) ->
 *   │   │       │       ├─ ввод и проверка пароля - сверяем  ◀────────┐
 *   │   │       │       ├─ пароли совпадают ->                        │
 *   │   │       │       │   ├─ skip_check = 1                         │
 *   │   │       │       │   └─► Выход из меню, возврат к стоку        │  
 *   │   │       │       └─► пароли различны ->                        │
 *   │   │       │           └─► Вернуться к вводу и проверке пароля ──┘
 *   │   │       └─► (skip_check != 2) -> 
 *   │   │           └─► меню блокировки рации
 *   │   │               ├─ разблокировать рацию (удаление пароля), если есть пароль
 *   │   │               ├─ заблокировать рацию (просмотр, ввод и запись пароля)
 *   │   │               ├─ заблокировать сейчас ->
 *   │   │               │   ├─ skip_check = 2
 *   │   │               │   └─► Выход из меню, возврат к стоку  
 *   │   │               └─► Вернуться в меню menu app
 *   │   └─► (skip_check != 2) ->
 *   │       └─► Вывод меню модулей
 *   │           ├── passcode
 *   │           │    └──► passcode: flow аналогичный вышеприведенному
 *   │           ├──► другой модуль
 *   │           ├──► ...
 *   │           └──► другой модуль
 *   └─► loop() 
 *
 */
#include "h9mod.h"
#include "gfx.h"            

#define PASS_ADDR 0x716FF8u
#define PASS_LEN  8u

/* Owner-lock flag, shared with the main-flash stub (stub.s mlk_check) and the
 * toolbox menu (menu.c). 0 = fresh, 1 = unlocked this session, 2 = locked:
 * the stub opened the toolbox to verify the password. The module clears it to
 * 1 when the password is accepted. */
#define SKIP_CHECK ((volatile unsigned char *)0x2B8DFu) 
#define SKIP_UNLOCKED 1u
#define SKIP_LOCKED   2u
#define SKIP_NORMAL   3u

/* Loader API 2 (H9 Toolbox Firmware v0.1.4): a module other than the menu can
 * return 0xFFFF to leave for the radio at once, without loading the toolbox
 * menu. The shared h9mod.h here is API 1, so define the constant locally. */
#define H9_RETURN_RADIO_LOCAL 0xFFFFu

/* Stock resource-flash primitives (SPI2 NOR): 4 KiB sector erase by chip
 * sector index, and 256-byte page program by chip address. The lock record
 * shares a whole 4 KiB sector (0x716000) whose rest is free (0xFF) -- we
 * verified on the radio that the only non-0xFF bytes in it are the 8 lock
 * bytes -- so erasing that sector to change the password is safe: it does
 * not touch the picture area (0x717000) nor any font glyphs. */
#define STOCK_ERASE(sector)      ((void (*)(unsigned))0x01E50000)(sector)
#define STOCK_PROGRAM(src, addr, len) ((void (*)(const void *, unsigned, unsigned))0x01E5006E)(src, addr, len)
#define LOCK_SECTOR 0x716000u
#define SECTOR_SIZE 0x1000u

static int is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int is_space(unsigned char c) { return c == 0x20; }

static int read_pass(const struct h9_api *api, unsigned char *out)
{
    api->read(out, PASS_ADDR, PASS_LEN);
    return 0;
}

static int pass_locked(const unsigned char *p)
{
    unsigned digits = 0;
    for (unsigned i = 0; i < PASS_LEN; i++) {
        if (is_digit(p[i]))        { digits++; continue; }
        if (is_space(p[i]))         continue;
        return 0;                   /* non-digit, non-space -> unlocked */
    }
    return digits > 0;              /* all digit/space and at least one digit -> locked */
}

/* Write `pass` (PASS_LEN bytes) as the lock password; 0xFF clears it.
 * Erases the whole lock sector (0x716000, verified free apart from the lock
 * record) then programs the 8 bytes, then verifies by read-back. Returns 0
 * on success, non-zero on failure. */
static int write_pass(const struct h9_api *api, const unsigned char *pass)
{
    STOCK_ERASE(LOCK_SECTOR / SECTOR_SIZE);       /* sector index = 0x716 */
    STOCK_PROGRAM(pass, PASS_ADDR, PASS_LEN);

    unsigned char check[PASS_LEN];
    api->read(check, PASS_ADDR, PASS_LEN);
    for (unsigned i = 0; i < PASS_LEN; i++)
        if (check[i] != pass[i]) return -1;
    return 0;
}

static void title_line(void)
{
    gfx_fill(0, 0, H9_WIDTH, 22, GFX_BLUE);
    gfx_text(18, 3, "RADIO LOCK", 12, 2, GFX_WHITE, GFX_BLUE);
}

static void clear_all(void)
{
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
}

static void clear_body(void)
{
    gfx_fill(0, 22, H9_WIDTH, H9_HEIGHT - 22, GFX_BLACK);
}

static void status_line(const unsigned char *p)
{
    gfx_text(34, 28, pass_locked(p) ? "STATUS: LOCKED" : "STATUS: UNLOCKED",
             16, 1, pass_locked(p) ? GFX_RED : GFX_GREEN, GFX_BLACK);
}

/* ASCII password (digits + trailing spaces) from raw 8 bytes. */
static void pass_ascii(const unsigned char *p, char *out)
{
    unsigned n = 0;
    for (unsigned i = 0; i < PASS_LEN; i++) {
        if (is_digit(p[i])) out[n++] = (char)p[i];
        else if (is_space(p[i])) break;    /* trailing spaces terminate */
        else { out[n++] = (char)p[i]; }    /* shouldn't happen when locked */
    }
    out[n] = 0;
}

/* Enter up to 8 digits. `mask` = 1 shows '*' instead of the digits (used on
 * the RADIO IS LOCK screen); 0 shows the digits (used when setting a new
 * password in the menu). Returns 0 on Menu-confirm, 1 on Back-cancel,
 * and, when `mask` = 1, 2 on Up/Down (LOCK NOW -- leave locked, exit to the
 * radio). */
static int enter_digits(char *buf, unsigned maxlen, const char *prompt, unsigned y, int mask)
{
    unsigned len = 0;
    buf[0] = 0;
    gfx_fill(0, y, H9_WIDTH, 16, GFX_BLACK);
    gfx_text(50, y, prompt, 20, 1, GFX_YELLOW, GFX_BLACK);
    gfx_wait_release();
    for (;;) {
        char shown[PASS_LEN + 1];
        unsigned s;
        for (s = 0; s < len; s++) shown[s] = mask ? '*' : buf[s];
        shown[s] = 0;
        gfx_fill(4, y + 11, H9_WIDTH - 8, 22, GFX_BLACK);
        gfx_text(40, y + 12, shown, maxlen, 2, GFX_WHITE, GFX_BLACK);
        unsigned k = gfx_wait_key();
        if (k == H9_KEY_UP || k == H9_KEY_DOWN) {
            if (mask) return 2;            /* LOCK NOW on the lock screen */
        } else if (k >= 2 && k <= 11) {     /* digits 0-9: key = digit + 2 */
            if (len < maxlen) buf[len++] = (char)('0' + (k - 2));
            buf[len] = 0;
        } else if (k == H9_KEY_BACK) {
            if (len) buf[--len] = 0;
            else return 1;
        } else if (k == H9_KEY_MENU) {
            buf[len] = 0;
            return 0;
        }
    }
}

/* Menu of the radio-lock manager. Returns 1 (H9_RETURN_RADIO_LOCAL) when the
 * user chose LOCK NOW (password written, leave to the radio immediately with
 * the lock armed); otherwise returns 0 (back to the toolbox menu). */
static unsigned menu_loop(const struct h9_api *api, unsigned char *pass)
{
    enum { LOCK, UNLOCK, EXIT } sel = LOCK;
    for (;;) {
        title_line();
		clear_body();
        status_line(pass);
        gfx_text(4, 44, sel == LOCK    ? "[LOCK]      Set pass" : " LOCK       Set pass", 24, 1,
                 sel == LOCK ? GFX_YELLOW : GFX_WHITE, GFX_BLACK);
        gfx_text(4, 60, sel == UNLOCK  ? "[UNLOCK]    Clear pass" : " UNLOCK     Clear pass", 24, 1,
                 sel == UNLOCK ? GFX_YELLOW : GFX_WHITE, GFX_BLACK);
		gfx_text(4, 92, sel == EXIT    ? "[EXIT]      Menu" : " EXIT       Menu", 24, 1,
                 sel == EXIT ? GFX_YELLOW : GFX_WHITE, GFX_BLACK);
        gfx_text(2, 118, "UP/DN: MOVE      BACK: OK", 30, 1, GFX_GREY, GFX_BLACK);
        gfx_wait_release();

        unsigned k = gfx_wait_key();
        if (k == H9_KEY_UP)       { sel = sel == LOCK ? EXIT : (unsigned)(sel - 1); }
        else if (k == H9_KEY_DOWN){ sel = (sel == EXIT) ? LOCK : (unsigned)(sel + 1); }
        else if (k == H9_KEY_MENU) {
            char buf[PASS_LEN + 1];
            unsigned char newpass[PASS_LEN];
            unsigned i;
            if (sel == LOCK) {
				clear_body();
                if (enter_digits(buf, PASS_LEN, "NEW PASSW:", 56, 0) == 0) {
                    /* pad to 8 with spaces after the typed digits */
                    i = 0; while (buf[i]) i++;
                    for (unsigned j = 0; j < PASS_LEN; j++)
                        newpass[j] = j < i ? (unsigned char)buf[j] : 0x20;
                    int saved = write_pass(api, newpass);
                    if (saved == 0) {
                        api->read(pass, PASS_ADDR, PASS_LEN);
                        gfx_text(4, 100, "SAVED", 20, 1, GFX_GREEN, GFX_BLACK);
                    } else {
                        gfx_text(4, 100, "WRITE FAIL", 12, 1, GFX_RED, GFX_BLACK);
                    }
                    h9_delay(70);                    
                }
			} else if (sel == UNLOCK) {
                for (i = 0; i < PASS_LEN; i++) newpass[i] = 0xFF;
                if (write_pass(api, newpass) == 0) {
                    api->read(pass, PASS_ADDR, PASS_LEN);
                    gfx_text(4, 100, "UNLOCKED", 10, 1, GFX_GREEN, GFX_BLACK);
                } else {
                    gfx_text(4, 100, "WRITE FAIL", 12, 1, GFX_RED, GFX_BLACK);
                }
                h9_delay(60);
            } else if (sel == EXIT) {
                return 0;
            }
        }
        else if (k == H9_KEY_BACK) return 0;
    }
	return 0;
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    unsigned char pass[PASS_LEN];
    read_pass(api, pass);    

    if (*SKIP_CHECK == 2) {
        if (!pass_locked(pass)) {
			*SKIP_CHECK = SKIP_NORMAL;
			clear_all();
			return H9_RETURN_RADIO_LOCAL;
		}
        char buf[PASS_LEN + 1];
        okb: ;
        clear_all();
        gfx_text(4, 26, "RADIO IS LOCK", 14, 2, GFX_RED, GFX_BLACK);
        gfx_text(4, 118, "ENTER PASSWORD TO UNLOCK", 30, 1, GFX_GREY, GFX_BLACK);
        if (enter_digits(buf, PASS_LEN, "PASSWORD:", 56, 1) == 2) goto okb;
        unsigned char cur[PASS_LEN];
        read_pass(api, cur);
        char ascii[PASS_LEN + 1];
        pass_ascii(cur, ascii);
        unsigned i = 0;
        while (buf[i] && ascii[i] && buf[i] == ascii[i]) i++;
        if (!(buf[i] == 0 && ascii[i] == 0)) {          /* mismatch */
            gfx_text(30, 96, "WRONG PASSWORD", 16, 1, GFX_RED, GFX_BLACK);
            h9_delay(90);
            goto okb;
        }
        *SKIP_CHECK = SKIP_NORMAL;
        clear_all();
        return H9_RETURN_RADIO_LOCAL;
    }

    if (pass_locked(pass)) {
        char buf[PASS_LEN + 1];
        ok: ;
        clear_all();
        gfx_text(4, 26, "RADIO IS LOCK", 14, 2, GFX_RED, GFX_BLACK);
        gfx_text(4, 118, "NEED PASSWORD TO RESET", 30, 1, GFX_GREY, GFX_BLACK);
        int rc = enter_digits(buf, PASS_LEN, "PASSWORD:", 56, 1);
        if (rc == 1) return 0;  /* Back cancels to menu */
        if (rc == 2) goto ok; 
        unsigned char cur[PASS_LEN];
        read_pass(api, cur);
        char ascii[PASS_LEN + 1];
        pass_ascii(cur, ascii);
        unsigned i = 0;
        while (buf[i] && ascii[i] && buf[i] == ascii[i]) i++;
        if (!(buf[i] == 0 && ascii[i] == 0)) {          /* mismatch */
            gfx_text(30, 96, "WRONG PASSWORD", 16, 1, GFX_RED, GFX_BLACK);
            h9_delay(90);
            goto ok;
        }
    }
	
    unsigned r = menu_loop(api, pass);
	clear_body();
	
    return r;
}