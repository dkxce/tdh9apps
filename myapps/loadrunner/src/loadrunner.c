#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

/* Коды кнопок TD-H9 */
enum {
    KEY_1    = 3,
    KEY_2    = 4,
    KEY_3    = 5,
    KEY_4    = 6,
    KEY_5    = 7,
    KEY_6    = 8,
    KEY_7    = 9,
    KEY_8    = 10,
    KEY_9    = 11,
    KEY_BACK = 15,
    KEY_STAR = 0x13
};

/* Сетка уровня: 20x14 клеток по 7x8 пикселей (140x112 px) */
#define MAP_W  20
#define MAP_H  14
#define CELL_W 7
#define CELL_H 8
#define OFF_X  10
#define OFF_Y  2

/* Типы клеток */
enum {
    T_EMPTY  = 0, /* Пустота */
    T_BRICK  = 1, /* Прожигаемый кирпич */
    T_SOLID  = 2, /* Неразрушаемый бетон */
    T_LADDER = 3, /* Лестница */
    T_ROPE   = 4, /* Перекладина / веревка */
    T_GOLD   = 5, /* Золотой слиток */
    T_EXIT   = 6  /* Скрытая лестница выхода (появляется после сбора всего золота) */
};

/* Цвета RGB565 */
#define COL_BRICK  0xC200 /* Кирпич */
#define COL_SOLID  0x8410 /* Серый бетон */
#define COL_LADDER 0x07FF /* Голубая лестница */
#define COL_ROPE   0xC618 /* Белая перекладина */
#define COL_GOLD   0xFFE0 /* Золото */
#define COL_PLAYER 0x07E0 /* Зеленый бегун */
#define COL_ENEMY  0xF800 /* Красный охранник */

/* Упакованные уровни: по 4 бита на клетку (2 клетки/байт), 20 клеток в строке -> 10 байт/строка, 14 строк/уровень. */
enum {
    C_EMP = 0, /* '.' пусто */
    C_BRK = 1, /* '#' кирпич */
    C_SLD = 2, /* '@' бетон */
    C_LAD = 3, /* 'H' лестница */
    C_ROP = 4, /* '-' веревка */
    C_GLD = 5, /* '$' золото */
    C_EXT = 6, /* 'S' скрытый выход */
    C_PLR = 7, /* 'P' игрок */
    C_ENM = 8  /* 'E' враг */
};

#define R(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t) { \
    ((uint8_t)(((a)<<4)|(b))), ((uint8_t)(((c)<<4)|(d))), ((uint8_t)(((e)<<4)|(f))), \
    ((uint8_t)(((g)<<4)|(h))), ((uint8_t)(((i)<<4)|(j))), ((uint8_t)(((k)<<4)|(l))), \
    ((uint8_t)(((m)<<4)|(n))), ((uint8_t)(((o)<<4)|(p))), ((uint8_t)(((q)<<4)|(r))), \
    ((uint8_t)(((s)<<4)|(t))) }

#define NUM_LEVELS 12

static const uint8_t LEVELS[NUM_LEVELS][MAP_H][10] = {
    {
        R(0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(0,0,0,0,5,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(1,1,1,1,1,1,1,3,1,1,1,1,1,1,0,0,0,0,0,0),
        R(0,0,0,0,0,0,0,3,4,4,4,4,4,0,0,0,0,0,0,0),
        R(0,0,0,0,0,0,0,3,0,0,8,0,0,3,0,0,0,5,0,0),
        R(0,0,0,0,1,1,1,1,1,1,1,3,1,1,1,3,1,1,1,1),
        R(0,0,0,0,3,0,0,0,0,0,0,3,0,0,0,3,0,0,0,0),
        R(0,0,0,0,3,0,0,5,0,0,0,3,0,0,0,3,0,0,0,0),
        R(1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1),
        R(0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0),
        R(0,0,5,0,0,0,0,3,4,4,4,4,4,0,0,5,0,0,0,0),
        R(1,1,1,1,3,1,1,1,1,1,0,0,1,1,1,1,3,1,1,1),
        R(0,0,0,0,3,0,0,7,0,0,0,0,0,0,0,0,3,0,8,0),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(0,0,5,0,0,0,0,0,0,6,0,0,0,0,0,0,5,0,0,0),
        R(1,1,1,1,3,0,0,0,0,6,0,0,0,0,3,1,1,1,1,1),
        R(0,0,0,0,3,4,4,4,4,4,4,4,4,4,3,0,0,0,0,0),
        R(0,0,0,0,3,0,0,8,0,0,0,8,0,0,3,0,0,0,0,0),
        R(0,0,1,1,1,1,1,3,0,0,0,3,1,1,1,1,1,0,0,0),
        R(0,0,0,0,0,0,0,3,0,5,0,3,0,0,0,0,0,0,0,0),
        R(0,0,0,0,1,1,1,1,1,3,1,1,1,1,1,0,0,0,0,0),
        R(0,5,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,5,0,0),
        R(1,1,1,1,3,4,4,4,4,4,4,4,4,4,3,1,1,1,1,1),
        R(0,0,0,0,3,0,0,0,0,8,0,0,0,0,3,0,0,0,0,0),
        R(0,0,1,1,1,1,1,0,0,7,0,0,1,1,1,1,1,0,0,0),
        R(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0),
        R(0,0,0,0,6,0,0,0,0,0,0,0,0,5,0,0,0,0,0,0),
        R(0,0,1,1,1,1,1,3,0,0,0,1,1,1,1,1,3,0,0,0),
        R(0,0,0,0,0,0,0,3,4,4,4,4,4,4,4,4,3,0,0,0),
        R(0,0,5,0,0,0,0,3,0,0,0,8,0,0,0,0,3,0,0,0),
        R(1,1,1,1,1,3,1,1,1,1,0,0,0,0,1,1,1,1,3,1),
        R(0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,3,0),
        R(0,0,0,0,0,3,0,0,0,5,0,0,0,0,5,0,0,0,3,0),
        R(0,0,1,1,1,1,1,1,3,1,1,1,1,1,1,3,1,1,1,1),
        R(0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,3,0,0,0,0),
        R(0,0,0,0,8,0,0,0,3,4,4,4,4,4,4,3,0,0,5,0),
        R(0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,1,1,1,1,1),
        R(0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,8,0,0,0,0),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(0,0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0),
        R(0,0,0,0,0,0,3,0,0,5,0,0,3,0,0,0,0,0,0,0),
        R(0,0,0,0,0,1,1,1,1,3,1,1,1,1,0,0,0,0,0,0),
        R(0,0,0,0,3,0,0,0,0,3,0,0,0,0,3,0,0,0,0,0),
        R(0,0,0,1,1,1,1,3,1,1,1,3,1,1,1,1,0,0,0,0),
        R(0,0,3,0,0,5,0,3,0,8,0,3,0,5,0,0,3,0,0,0),
        R(0,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,0,0),
        R(0,0,0,0,0,0,0,0,3,0,3,0,0,0,0,0,0,0,0,0),
        R(0,0,5,0,0,8,0,0,3,0,3,0,0,8,0,0,5,0,0,0),
        R(1,1,1,1,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1),
        R(0,0,0,0,0,3,0,0,0,7,0,0,0,3,0,0,0,0,0,0),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0),
        R(0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0),
        R(1,1,1,1,3,4,4,4,4,4,4,4,4,4,4,4,4,4,3,1),
        R(0,0,0,0,3,0,0,0,0,0,0,8,0,0,0,0,0,0,3,0),
        R(0,0,0,0,3,0,0,0,1,1,1,1,1,1,1,0,0,0,3,0),
        R(0,0,5,0,0,0,0,0,3,0,0,5,0,0,3,0,0,0,5,0),
        R(1,1,1,1,1,3,1,1,1,1,0,0,0,1,1,1,1,3,1,1),
        R(0,0,0,0,0,3,4,4,4,4,4,4,4,4,4,4,4,3,0,0),
        R(0,0,8,0,0,3,0,0,0,0,0,0,0,0,0,0,0,3,0,0),
        R(1,1,1,1,1,1,1,0,0,1,1,1,1,1,0,0,1,1,1,1),
        R(0,0,0,0,0,0,0,0,0,3,0,5,0,3,0,0,0,0,0,0),
        R(0,0,0,0,7,0,0,0,0,3,0,0,0,3,0,0,0,0,8,0),
        R(1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0),
        R(0,0,0,0,3,0,5,0,5,0,5,0,5,0,3,0,0,0,0,0),
        R(0,0,0,0,3,1,1,1,1,1,1,1,1,1,3,0,0,0,0,0),
        R(0,0,0,0,3,0,0,0,0,8,0,0,0,0,3,0,0,0,0,0),
        R(0,0,1,1,1,1,1,3,4,4,4,3,1,1,1,1,1,0,0,0),
        R(0,0,0,0,0,0,0,3,0,0,0,3,0,0,0,0,0,0,0,0),
        R(0,0,5,0,0,0,0,3,0,8,0,3,0,0,0,0,5,0,0,0),
        R(1,1,1,1,3,1,1,1,1,1,1,1,1,1,3,1,1,1,1,1),
        R(0,0,0,0,3,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0),
        R(0,0,0,0,3,0,0,0,0,7,0,0,0,0,3,0,0,8,0,0),
        R(1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0),
        R(0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0),
        R(1,1,1,1,3,4,4,4,4,4,4,4,4,4,4,4,1,1,1,1),
        R(0,0,0,0,3,0,0,0,0,0,8,0,0,0,0,0,3,0,0,0),
        R(0,0,5,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0),
        R(1,1,1,1,1,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1),
        R(0,0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,0,0),
        R(0,0,0,0,0,0,3,0,0,0,5,0,0,0,3,0,0,0,0,0),
        R(1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,3,1,1,1,1),
        R(0,0,0,0,3,4,4,4,4,4,8,4,4,4,4,4,3,0,0,0),
        R(0,0,5,0,3,0,0,0,0,0,0,0,0,0,0,0,3,0,5,0),
        R(1,1,1,1,1,1,1,0,0,0,0,0,0,0,1,1,1,1,1,1),
        R(0,0,0,0,7,0,0,0,0,0,0,0,0,0,8,0,0,0,0,0),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(0,0,5,0,0,0,0,0,0,6,0,0,0,0,0,0,5,0,0,0),
        R(1,1,1,1,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1),
        R(0,0,0,0,0,3,4,4,4,8,4,4,4,3,0,0,0,0,0,0),
        R(0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,0,0,0),
        R(0,0,5,0,0,3,0,0,5,5,5,0,0,3,0,0,5,0,0,0),
        R(1,1,1,1,1,1,1,3,1,1,1,3,1,1,1,1,1,1,1,1),
        R(0,0,0,0,0,0,0,3,0,8,0,3,0,0,0,0,0,0,0,0),
        R(0,0,0,0,0,0,0,3,4,4,4,3,0,0,0,0,0,0,0,0),
        R(0,0,5,0,0,0,0,3,0,0,0,3,0,0,0,0,5,0,0,0),
        R(1,1,1,1,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1),
        R(0,0,0,0,0,3,0,0,0,7,0,0,0,3,0,0,0,8,0,0),
        R(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(0,0,0,0,5,0,0,0,0,6,0,0,0,0,5,0,0,0,0,0),
        R(0,0,1,1,1,1,1,3,1,1,1,3,1,1,1,1,1,0,0,0),
        R(0,0,0,0,0,0,0,3,4,4,4,3,0,0,0,0,0,0,0,0),
        R(0,0,0,0,8,0,0,3,0,0,0,3,0,0,8,0,0,0,0,0),
        R(1,1,1,1,3,1,1,1,1,0,1,1,1,1,3,1,1,1,1,1),
        R(0,0,0,0,3,0,0,0,0,5,0,0,0,0,3,0,0,0,0,0),
        R(0,0,0,0,3,0,0,1,1,1,1,1,0,0,3,0,0,0,0,0),
        R(0,5,0,0,3,4,4,4,4,4,4,4,4,4,3,0,0,5,0,0),
        R(1,1,1,1,1,1,0,0,0,0,0,0,0,1,1,1,1,1,1,1),
        R(0,0,0,0,0,0,3,0,0,8,0,0,3,0,0,0,0,0,0,0),
        R(0,0,0,0,0,0,3,0,0,7,0,0,3,0,0,0,0,0,0,0),
        R(1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0),
        R(0,0,6,0,0,0,0,5,0,0,0,0,5,0,0,0,0,0,0,0),
        R(1,1,1,1,1,1,3,1,1,1,1,1,1,3,1,1,1,1,1,1),
        R(0,0,0,0,0,0,3,0,0,0,0,0,0,3,0,0,0,0,0,0),
        R(0,0,8,0,0,0,3,4,4,4,4,4,4,3,0,0,0,8,0,0),
        R(1,1,1,1,3,1,1,1,0,0,0,0,1,1,1,3,1,1,1,1),
        R(0,0,0,0,3,0,0,0,0,5,5,0,0,0,0,3,0,0,0,0),
        R(0,0,0,0,3,0,0,1,1,1,1,1,1,0,0,3,0,0,0,0),
        R(0,5,0,0,3,0,0,0,0,8,0,0,0,0,0,3,0,0,5,0),
        R(1,1,1,1,1,1,3,4,4,4,4,4,4,3,1,1,1,1,1,1),
        R(0,0,0,0,0,0,3,0,0,0,0,0,0,3,0,0,0,0,0,0),
        R(0,0,0,0,0,0,3,0,0,7,0,0,0,3,0,0,0,0,0,0),
        R(1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0,0),
        R(0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0,0),
        R(1,1,1,3,4,4,4,4,4,4,4,4,4,4,4,4,3,1,1,1),
        R(0,0,0,3,0,0,0,5,0,0,0,0,5,0,0,0,3,0,0,0),
        R(0,0,0,3,0,0,1,1,1,0,0,1,1,1,0,0,3,0,0,0),
        R(0,0,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,0,0),
        R(1,1,1,1,1,3,4,4,4,4,4,4,4,4,3,1,1,1,1,1),
        R(0,0,0,0,0,3,0,0,0,5,5,0,0,0,3,0,0,0,0,0),
        R(0,0,0,0,0,3,0,0,1,1,1,1,0,0,3,0,0,0,0,0),
        R(0,5,0,0,1,1,1,0,0,8,0,0,0,1,1,1,0,0,5,0),
        R(1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1),
        R(0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0),
        R(1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
    {
        R(0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0),
        R(1,1,1,1,1,1,1,3,1,1,1,3,1,1,1,1,1,1,1,1),
        R(0,0,5,0,0,0,0,3,0,0,0,3,0,0,0,0,5,0,0,0),
        R(1,1,1,1,1,0,0,3,0,5,0,3,0,0,1,1,1,1,1,1),
        R(0,0,0,0,0,0,0,3,1,1,1,3,0,0,0,0,0,0,0,0),
        R(0,0,8,0,0,0,0,3,0,8,0,3,0,0,0,0,8,0,0,0),
        R(1,1,1,1,1,3,1,1,1,0,1,1,1,3,1,1,1,1,1,1),
        R(0,0,0,0,0,3,0,0,0,0,0,0,0,3,0,0,0,0,0,0),
        R(0,0,5,0,0,3,0,0,1,1,1,0,0,3,0,0,5,0,0,0),
        R(1,1,1,1,1,1,1,0,0,5,0,0,1,1,1,1,1,1,1,1),
        R(0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0),
        R(1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1),
        R(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2),
    },
};
;

struct hole {
    int8_t x, y;
    uint32_t restore_tick;
    uint8_t active;
};

struct guard {
    int8_t x, y;
    uint8_t alive;
    int8_t trapped_hole; /* Индекс ямы, если упал в нее (-1 если свободен) */
    uint32_t climb_tick;
};

static uint8_t map[MAP_H][MAP_W];
static struct hole holes[6];
static struct guard guards[3];

static int8_t px, py;
static uint8_t p_alive;
static uint8_t gold_left;
static uint8_t exit_open;
static uint8_t stage;
static uint32_t score;

static void num_to_str(char *d, uint32_t v)
{
    char t[10];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n) *d++ = t[--n];
    *d = 0;
}

/* Опрос клавиш с подавлением дребезга (сброс при отпускании) */
static unsigned key_last = 0, key_seen = 0;
static unsigned poll_key(void)
{
    unsigned k = h9_key();
    if (k != key_last) { key_last = k; return 0; }
    if (k == key_seen) return 0;
    if (k == H9_KEY_NONE) { key_seen = H9_KEY_NONE; return 0; }
    key_seen = k;
    return k;
}

/* Отрисовка одной клетки уровня */
static void draw_tile(int8_t x, int8_t y)
{
    int sx = OFF_X + x * CELL_W;
    int sy = OFF_Y + y * CELL_H;
    uint8_t t = map[y][x];

    gfx_fill(sx, sy, CELL_W, CELL_H, GFX_BLACK);

    if (t == T_BRICK) {
        gfx_fill(sx, sy, CELL_W, CELL_H, COL_BRICK);
        gfx_fill(sx, sy + 3, CELL_W, 1, GFX_BLACK);
        gfx_fill(sx + 3, sy, 1, 4, GFX_BLACK);
    } else if (t == T_SOLID) {
        gfx_fill(sx, sy, CELL_W, CELL_H, COL_SOLID);
        gfx_fill(sx + 1, sy + 1, CELL_W - 2, CELL_H - 2, 0xC618);
    } else if (t == T_LADDER || (t == T_EXIT && exit_open)) {
        uint16_t c = (t == T_EXIT) ? GFX_YELLOW : COL_LADDER;
        gfx_fill(sx + 1, sy, 1, CELL_H, c);
        gfx_fill(sx + 5, sy, 1, CELL_H, c);
        gfx_fill(sx + 1, sy + 2, 5, 1, c);
        gfx_fill(sx + 1, sy + 6, 5, 1, c);
    } else if (t == T_ROPE) {
        gfx_fill(sx, sy + 1, CELL_W, 1, COL_ROPE);
    } else if (t == T_GOLD) {
        gfx_fill(sx + 1, sy + 4, 5, 3, COL_GOLD);
        gfx_fill(sx + 2, sy + 3, 3, 1, GFX_WHITE);
    }
}

static void draw_actor(int8_t x, int8_t y, uint16_t color)
{
    int sx = OFF_X + x * CELL_W;
    int sy = OFF_Y + y * CELL_H;
    gfx_fill(sx + 2, sy + 1, 3, 2, GFX_WHITE); /* Голова */
    gfx_fill(sx + 1, sy + 3, 5, 3, color);     /* Тело */
    gfx_fill(sx + 1, sy + 6, 2, 2, color);     /* Ноги */
    gfx_fill(sx + 4, sy + 6, 2, 2, color);
}

static void draw_hud(void)
{
    char buf[12];
    gfx_fill(0, 116, H9_WIDTH, 12, GFX_BLACK);
    gfx_text(4,   118, "PTS:", 4, 1, GFX_WHITE, GFX_BLACK);
    num_to_str(buf, score);
    gfx_text(30,  118, buf, 7, 1, GFX_YELLOW, GFX_BLACK);

    gfx_text(82,  118, "GLD:", 4, 1, GFX_WHITE, GFX_BLACK);
    num_to_str(buf, gold_left);
    gfx_text(108, 118, buf, 3, 1, COL_GOLD, GFX_BLACK);

    gfx_text(124, 118, "*:P", 3, 1, GFX_YELLOW, GFX_BLACK);
    gfx_text(140, 118, "B:X", 3, 1, GFX_RED,    GFX_BLACK);
}

/* Проверка, твердая ли клетка под ногами */
static int is_solid(int8_t x, int8_t y)
{
    if (x < 0 || x >= MAP_W || y >= MAP_H) return 1;
    if (y < 0) return 0;
    uint8_t t = map[y][x];
    if (t == T_BRICK || t == T_SOLID) return 1;
    /* Охранник в яме служит мостиком, по которому можно пройти! */
    for (int i = 0; i < 3; i++) {
        if (guards[i].alive && guards[i].trapped_hole >= 0 && guards[i].x == x && guards[i].y == y)
            return 1;
    }
    return 0;
}

/* Есть ли опора (не падает ли персонаж вниз) */
static int has_support(int8_t x, int8_t y)
{
    uint8_t cur = map[y][x];
    if (cur == T_LADDER || cur == T_ROPE || (cur == T_EXIT && exit_open)) return 1;
    if (y + 1 >= MAP_H) return 1;
    uint8_t below = map[y + 1][x];
    if (below == T_LADDER || (below == T_EXIT && exit_open)) return 1;
    return is_solid(x, y + 1);
}

/* Прожигание ямы слева (-1) или справа (+1) */
static void dig_hole(int8_t dir)
{
    int8_t tx = px + dir;
    int8_t ty = py + 1;
    if (tx < 0 || tx >= MAP_W || ty >= MAP_H) return;
    if (map[ty][tx] != T_BRICK) return;
    /* Над кирпичом должно быть пусто */
    if (map[py][tx] != T_EMPTY && map[py][tx] != T_ROPE) return;

    for (int i = 0; i < 6; i++) {
        if (!holes[i].active) {
            holes[i].x = tx;
            holes[i].y = ty;
            holes[i].restore_tick = TICK + 550u; /* Яма затянется через 5.5 сек */
            holes[i].active = 1;
            map[ty][tx] = T_EMPTY;
            draw_tile(tx, ty);
            break;
        }
    }
}

/* Загрузка уровня */
static void load_level(uint8_t st)
{
    uint8_t idx = st % NUM_LEVELS;
    gold_left = 0;
    exit_open = 0;
    int g_cnt = 0;

    for (int i = 0; i < 6; i++) holes[i].active = 0;
    for (int i = 0; i < 3; i++) guards[i].alive = 0;

    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            uint8_t packed = LEVELS[idx][y][x >> 1];
            uint8_t code = (x & 1) ? (packed & 0x0F) : (packed >> 4);
            uint8_t t = T_EMPTY;
            if (code == C_BRK) t = T_BRICK;
            else if (code == C_SLD) t = T_SOLID;
            else if (code == C_LAD) t = T_LADDER;
            else if (code == C_ROP) t = T_ROPE;
            else if (code == C_GLD) { t = T_GOLD; gold_left++; }
            else if (code == C_EXT) t = T_EXIT;
            else if (code == C_PLR) { px = (int8_t)x; py = (int8_t)y; p_alive = 1; }
            else if (code == C_ENM && g_cnt < 3) {
                guards[g_cnt].x = (int8_t)x;
                guards[g_cnt].y = (int8_t)y;
                guards[g_cnt].alive = 1;
                guards[g_cnt].trapped_hole = -1;
                g_cnt++;
            }
            map[y][x] = t;
            draw_tile((int8_t)x, (int8_t)y);
        }
    }

    draw_actor(px, py, COL_PLAYER);
    for (int i = 0; i < 3; i++) {
        if (guards[i].alive) draw_actor(guards[i].x, guards[i].y, COL_ENEMY);
    }
    draw_hud();
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    score = 0;
    stage = 0;
    load_level(stage);

    uint32_t last_grav = TICK;
    uint32_t last_ai = TICK;
    key_last = key_seen = h9_key();

    for (;;) {
        uint32_t now = TICK;
        unsigned k = poll_key();

        /* Выход по Back */
        if (k == KEY_BACK) return 0;

        /* Пауза по * */
        if (k == KEY_STAR) {
            gfx_text(50, 56, "PAUSED", 10, 1, GFX_YELLOW, GFX_BLACK);
            gfx_wait_release();
            for (;;) {
                unsigned pk = poll_key();
                if (pk == KEY_STAR) break;
                if (pk == KEY_BACK) return 0;
                h9_delay(10);
            }
            gfx_wait_release();
            for (int y = 0; y < MAP_H; y++)
                for (int x = 0; x < MAP_W; x++) draw_tile((int8_t)x, (int8_t)y);
            if (p_alive) draw_actor(px, py, COL_PLAYER);
            for (int i = 0; i < 3; i++) if (guards[i].alive) draw_actor(guards[i].x, guards[i].y, COL_ENEMY);
            draw_hud();
            continue;
        }

        /* 1. Затягивание прожженных ям */
        for (int i = 0; i < 6; i++) {
            if (holes[i].active && now >= holes[i].restore_tick) {
                holes[i].active = 0;
                int8_t hx = holes[i].x, hy = holes[i].y;
                map[hy][hx] = T_BRICK;
                draw_tile(hx, hy);

                /* Если игрок застрял в яме — гибель */
                if (px == hx && py == hy) p_alive = 0;

                /* Если в яме сидел охранник — он возрождается наверху карты */
                for (int g = 0; g < 3; g++) {
                    if (guards[g].alive && guards[g].x == hx && guards[g].y == hy) {
                        guards[g].trapped_hole = -1;
                        guards[g].y = 1;
                        draw_actor(guards[g].x, guards[g].y, COL_ENEMY);
                    }
                }
            }
        }

        /* 2. Управление игроком (2468 - ход, 1/7 - копать слева, 3/9 - копать справа) */
        if (p_alive && has_support(px, py)) {
            int8_t nx = px, ny = py;
            int moved = 0;

            if (k == KEY_4) { nx--; moved = 1; }
            else if (k == KEY_6) { nx++; moved = 1; }
            else if (k == KEY_2 && (map[py][px] == T_LADDER || (map[py][px] == T_EXIT && exit_open))) {
                ny--; moved = 1;
            } else if (k == KEY_8 || k == KEY_5) {
                ny++; moved = 1;
            } else if (k == KEY_1 || k == KEY_7) {
                dig_hole(-1);
            } else if (k == KEY_3 || k == KEY_9) {
                dig_hole(1);
            }

            if (moved && nx >= 0 && nx < MAP_W && ny >= 0 && ny < MAP_H && !is_solid(nx, ny)) {
                draw_tile(px, py);
                px = nx; py = ny;

                /* Сбор золота */
                if (map[py][px] == T_GOLD) {
                    map[py][px] = T_EMPTY;
                    score += 100;
                    if (gold_left > 0) gold_left--;
                    if (gold_left == 0) {
                        exit_open = 1;
                        for (int y = 0; y < MAP_H; y++)
                            for (int x = 0; x < MAP_W; x++)
                                if (map[y][x] == T_EXIT) draw_tile((int8_t)x, (int8_t)y);
                    }
                    draw_hud();
                }
                draw_actor(px, py, COL_PLAYER);

                /* Победа на уровне при достижении верхней строки по открытой лестнице */
                if (exit_open && py == 0) {
                    stage++;
                    score += 500;
                    gfx_text(34, 56, "STAGE CLEAR", 12, 1, GFX_YELLOW, GFX_BLACK);
                    h9_delay(1200);
                    load_level(stage);
                    last_grav = last_ai = TICK;
                    continue;
                }
            }
        }

        /* 3. Гравитация (падение игрока и врагов в пустоту) каждые 150 мс */
        if ((now - last_grav) >= 15u) {
            last_grav = now;
            if (p_alive && !has_support(px, py)) {
                draw_tile(px, py);
                py++;
                draw_actor(px, py, COL_PLAYER);
            }
        }

        /* 4. Интеллект охранников каждые 380 мс */
        if ((now - last_ai) >= 38u) {
            last_ai = now;
            for (int i = 0; i < 3; i++) {
                struct guard *g = &guards[i];
                if (!g->alive) continue;

                /* Если охранник сидит в яме */
                if (g->trapped_hole >= 0) {
                    if (now >= g->climb_tick) {
                        /* Выбирается из ямы вверх и в сторону игрока */
                        draw_tile(g->x, g->y);
                        g->trapped_hole = -1;
                        if (g->y > 0 && !is_solid(g->x, g->y - 1)) g->y--;
                        int8_t step = (px > g->x) ? 1 : -1;
                        if (g->x + step >= 0 && g->x + step < MAP_W && !is_solid(g->x + step, g->y))
                            g->x += step;
                        draw_actor(g->x, g->y, COL_ENEMY);
                    }
                    continue;
                }

                /* Падение охранника */
                if (!has_support(g->x, g->y)) {
                    draw_tile(g->x, g->y);
                    g->y++;
                    /* Проверка, не упал ли он в прожженную яму */
                    for (int h = 0; h < 6; h++) {
                        if (holes[h].active && holes[h].x == g->x && holes[h].y == g->y) {
                            g->trapped_hole = (int8_t)h;
                            g->climb_tick = now + 300u; /* Сидит в яме 3 секунды */
                            score += 50;
                            draw_hud();
                            break;
                        }
                    }
                    draw_actor(g->x, g->y, COL_ENEMY);
                    continue;
                }

                /* Погоня за игроком по лестницам и этажам */
                int8_t nx = g->x, ny = g->y;
                if (g->y > py && (map[g->y][g->x] == T_LADDER || (map[g->y][g->x] == T_EXIT && exit_open)) && !is_solid(g->x, g->y - 1)) {
                    ny--;
                } else if (g->y < py && !is_solid(g->x, g->y + 1)) {
                    ny++;
                } else if (px < g->x && g->x > 0 && !is_solid(g->x - 1, g->y)) {
                    nx--;
                } else if (px > g->x && g->x + 1 < MAP_W && !is_solid(g->x + 1, g->y)) {
                    nx++;
                }

                if (nx != g->x || ny != g->y) {
                    draw_tile(g->x, g->y);
                    g->x = nx; g->y = ny;
                    draw_actor(g->x, g->y, COL_ENEMY);
                }

                /* Поимка игрока */
                if (g->trapped_hole < 0 && g->x == px && g->y == py) {
                    p_alive = 0;
                }
            }
        }

        if (!p_alive) {
            gfx_text(44, 56, "GAME OVER", 10, 1, GFX_RED, GFX_BLACK);
            gfx_wait_key();
            return 0;
        }

        h9_delay(10);
    }
}
