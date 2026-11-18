#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

/* --- RF / APRS-модем --- */
#define MYCALL        ((const volatile uint8_t *)0xC4A1)
#define RING          ((volatile uint8_t *)0x16634)
#define RING_WRITE    (*(volatile uint16_t *)0x16E34)
#define RING_READ     (*(volatile uint16_t *)0x16E36)
#define RING_MASK     2047u

#define RF_ASLEEP     (*(volatile uint8_t *)0x11B01)
#define RF_WAKE()     ((void (*)(void))0x01E5969Cu)()
#define AF_SELECT(m)  ((void (*)(unsigned))0x01E593E4u)(m)
#define RF_WRITE(r,v) ((void (*)(unsigned, unsigned))0x01E5070Au)((r), (v))
#define SPEAKER(on)   ((void (*)(unsigned))0x01E52328u)(on)
#define STOCK_KISS_TX ((void (*)(const uint8_t *, unsigned))0x01E5DBCCu)

#define SIDE_BYTE  (*(volatile uint8_t *)0x11B50)
#define ROUTE_BYTE (*(volatile uint8_t *)0x11AEC)
#define RF_SETUP(x) ((void (*)(unsigned))0x01E58E3Eu)(x)
#define RX_CH (*(const volatile uint8_t *)0xC532)
#define TX_CH (*(const volatile uint8_t *)0xC533)

enum { K_1 = 3, K_2 = 4, K_3 = 5, K_4 = 6, K_5 = 7, K_6 = 8, K_7 = 9,
       K_8 = 10, K_9 = 11, K_MENU = 12, K_BACK = 15 };

static uint8_t rx_frame[128];
static uint8_t rx_len;

static void aprs_quiet(void)
{
    if (RF_ASLEEP) RF_WAKE();
    AF_SELECT(1);
    RF_WRITE(0x30, 0xBFF5);
    SPEAKER(0);
}

static void aprs_side(unsigned side)
{
    SIDE_BYTE = (uint8_t)((SIDE_BYTE & 0x7F) | side << 7);
    ROUTE_BYTE = (uint8_t)side;
    RF_SETUP(0);
    aprs_quiet();
}

static unsigned aprs_rx_side(void) { return RX_CH == 1 ? 0 : RX_CH == 2 ? 1 : 2; }

/* Упаковка одного 7-байтового адреса AX.25. */
static void pack_addr(uint8_t *dst, const char *call, int last)
{
    int i = 0;
    for (; i < 6 && call[i] && call[i] != '-'; i++) {
        uint8_t c = (uint8_t)call[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i] = (uint8_t)(c << 1);
    }
    for (; i < 6; i++) dst[i] = (uint8_t)(' ' << 1);
    dst[6] = (uint8_t)(0x60 | (last ? 1 : 0));
}

/* Отправить тестовый UI-кадр: dst=APH9BS, src=mycall, payload ">BS<T>:TEST<x>". */
static void tx_test(char tag)
{
    static uint8_t frame[64], kiss[88];
    static char srccall[8];
    int p = 0, k = 0, n = 0;

    pack_addr(frame + p, "APH9BS", 0); p += 7;

    for (; n < 6 && MYCALL[n] > ' ' && MYCALL[n] <= '~'; n++) srccall[n] = (char)MYCALL[n];
    srccall[n] = 0;
    if (n == 0) { srccall[0] = 'N'; srccall[1] = '0'; srccall[2] = 'C';
                  srccall[3] = 'A'; srccall[4] = 'L'; srccall[5] = 'L'; srccall[6] = 0; }
    pack_addr(frame + p, srccall, 1); p += 7;

    frame[p++] = 0x03;       /* UI */
    frame[p++] = 0xF0;       /* no L3 */
    frame[p++] = '>';
    frame[p++] = 'B';
    frame[p++] = 'S';
    frame[p++] = '0';
    frame[p++] = '1';
    frame[p++] = ':';
    frame[p++] = 'T';
    frame[p++] = 'E';
    frame[p++] = 'S';
    frame[p++] = 'T';
    frame[p++] = tag;        /* уникальный символ 1..9 */

    /* KISS: C0 00 <escaped frame> C0 */
    kiss[k++] = 0xC0; kiss[k++] = 0x00;
    for (int i = 0; i < p; i++) {
        if (frame[i] == 0xC0) { kiss[k++] = 0xDB; kiss[k++] = 0xDC; }
        else if (frame[i] == 0xDB) { kiss[k++] = 0xDB; kiss[k++] = 0xDD; }
        else kiss[k++] = frame[i];
    }
    kiss[k++] = 0xC0;

    STOCK_KISS_TX(kiss, (unsigned)k);
    aprs_quiet();
}

/* Печать строки в позицию (x, y): максимум max символов, вернёт числo. */
static void show(unsigned x, unsigned y, const char *s, unsigned max)
{
    gfx_text(x, y, s, max, 1, GFX_WHITE, GFX_BLACK);
}

static void set_status(const char *s)
{
    gfx_fill(0, 118, H9_WIDTH, 10, GFX_BLACK);
    gfx_text(2, 119, s, 40, 1, 0x07E0, GFX_BLACK);
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(20, 8, "APRS TEST", 16, 1, 0xF81F, GFX_BLACK);

    /* показываем текущее состояние бокса */
    char s[24];
    unsigned n;
    for (n = 0; n < 6 && MYCALL[n] > ' ' && MYCALL[n] <= '~'; n++) s[n] = (char)MYCALL[n];
    s[n] = 0;
    show(4, 24, "MYCALL=", 8); show(60, 24, s, 8);

    aprs_side(aprs_rx_side());
    aprs_quiet();

    show(4, 40, "KEY 1-9: SEND TEST", 28);
    show(4, 50, "RX RAW:", 8);
    show(70, 50, "CNT:", 4);

    for (;;) {
        unsigned write = RING_WRITE & RING_MASK;
        unsigned read  = RING_READ & RING_MASK;
        /* выводим позиции кольца */
        static char rtxt[16];
        unsigned rn = 0;
        static char hx[] = "0123456789ABCDEF";
        rtxt[rn++] = hx[(write >> 8) & 15]; rtxt[rn++] = hx[(write >> 4) & 15]; rtxt[rn++] = hx[write & 15];
        rtxt[rn++] = '/';
        rtxt[rn++] = hx[(read >> 8) & 15]; rtxt[rn++] = hx[(read >> 4) & 15]; rtxt[rn++] = hx[read & 15];
        rtxt[rn] = 0;
        show(88, 40, rtxt, 8);
        while (read != write) {
            uint8_t b = RING[read];
            read = (read + 1u) & RING_MASK;
            if (b == 0xC0) {
                /* закрывающий C0 — если успели накопить кадр, выводим его */
                if (rx_len > 2 && rx_frame[0] == 0x00) {
                    static unsigned rxcount = 0;
                    rxcount++;
                    unsigned at = 1;
                    while (at + 6 < rx_len) {
                        if (rx_frame[at + 6] & 1u) { at += 7; break; }
                        at += 7;
                    }
                    at += 2; /* ctrl + pid */
                    static char out[40];
                    unsigned o = 0;
                    for (unsigned q = at; q < rx_len && o < 36 &&
                         rx_frame[q] >= 0x20 && rx_frame[q] < 0x7F; q++)
                        out[o++] = (char)rx_frame[q];
                    out[o] = 0;
                    gfx_fill(0, 60, H9_WIDTH, 56, GFX_BLACK);
                    show(4, 60, out, 40);
                    /* счётчик рядом с "CNT:" */
                    char ctxt[5];
                    ctxt[0] = (char)('0' + (rxcount / 10) % 10);
                    ctxt[1] = (char)('0' + rxcount % 10);
                    ctxt[2] = 0;
                    show(100, 50, ctxt, 3);
                    set_status(out);
                }
                rx_len = 0;          /* начинаем новый кадр */
            } else if (rx_len < sizeof rx_frame) {
                rx_frame[rx_len++] = b;
            }
        }

        unsigned kk = h9_key();
        if (kk && kk != H9_KEY_NONE) {
            static unsigned last = 0;
            if (kk != last) {
                last = kk;
                if (kk >= K_1 && kk <= K_9) {
                    tx_test((char)('1' + (kk - K_1)));
                    set_status("TX SENT");
                    gfx_wait_release();
                    h9_delay(60);
                    set_status("READY");
                }
                if (kk == K_BACK) break;
            }
        }
        h9_delay(5);
    }
    return 0;
}