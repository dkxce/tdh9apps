#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

/* Коды кнопок TD-H9 */
enum {
    KEY_0    = 2,
    KEY_2    = 4,
    KEY_4    = 6,
    KEY_5    = 7,
    KEY_6    = 8,
    KEY_8    = 10,
    KEY_BACK = 15,
    KEY_STAR = 0x13
};

/* Поле 13x11 клеток по 10x10 пикселей (130x110 px) */
#define MAP_W   13
#define MAP_H   11
#define TILE_SZ 10
#define OFF_X   2
#define OFF_Y   14

/* Типы клеток */
enum {
    T_EMPTY = 0,
    T_WALL  = 1, /* Неразрушаемый бетонный блок */
    T_BRICK = 2, /* Разрушаемый кирпич */
    T_DOOR  = 3, /* Открытая дверь на следующий уровень */
    T_POWER = 4  /* Бонус (+1 к радиусу взрыва) */
};

/* Цвета RGB565 */
#define COL_WALL   0x8410 /* Серый бетон */
#define COL_BRICK  0xC200 /* Кирпичный */
#define COL_PLAYER 0xFFE0 /* Желтый (Бомбермен) */
#define COL_ENEMY  0xF81F /* Пурпурный (враг-шарик) */
#define COL_BOMB   0x07FF /* Голубая мигающая бомба */
#define COL_FIRE   0xFD20 /* Оранжевое пламя */
#define COL_DOOR   0x07E0 /* Зеленый выход */

struct enemy {
    int8_t x, y;
    uint8_t dir;
    uint8_t alive;
};

static uint8_t map[MAP_H][MAP_W];
static uint8_t hidden[MAP_H][MAP_W]; /* Спрятанные под кирпичами дверь и бонус */

static int8_t px, py;
static uint8_t p_alive;
static uint8_t blast_range;
static uint32_t score;
static uint8_t stage;

/* Бомба игрока */
static uint8_t bomb_active;
static int8_t bx, by;
static uint32_t bomb_tick;

/* Вспышка взрыва */
static uint8_t fire_active;
static uint32_t fire_tick;

static struct enemy enemies[4];
static uint32_t rng_state;

static uint32_t fast_rand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

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

/* Отрисовка клетки карты */
static void draw_tile(int8_t x, int8_t y)
{
    int sx = OFF_X + x * TILE_SZ;
    int sy = OFF_Y + y * TILE_SZ;
    uint8_t t = map[y][x];

    if (t == T_EMPTY) {
        gfx_fill(sx, sy, TILE_SZ, TILE_SZ, GFX_BLACK);
    } else if (t == T_WALL) {
        gfx_fill(sx, sy, TILE_SZ, TILE_SZ, COL_WALL);
        gfx_fill(sx + 1, sy + 1, TILE_SZ - 2, TILE_SZ - 2, 0xC618);
    } else if (t == T_BRICK) {
        gfx_fill(sx, sy, TILE_SZ, TILE_SZ, COL_BRICK);
        gfx_fill(sx, sy + 4, TILE_SZ, 1, GFX_BLACK);
        gfx_fill(sx + 5, sy, 1, 4, GFX_BLACK);
        gfx_fill(sx + 2, sy + 5, 1, 5, GFX_BLACK);
    } else if (t == T_DOOR) {
        gfx_fill(sx + 1, sy + 1, TILE_SZ - 2, TILE_SZ - 2, COL_DOOR);
        gfx_fill(sx + 3, sy + 3, TILE_SZ - 6, TILE_SZ - 6, GFX_BLACK);
    } else if (t == T_POWER) {
        gfx_fill(sx + 2, sy + 2, TILE_SZ - 4, TILE_SZ - 4, GFX_RED);
        gfx_fill(sx + 4, sy + 3, 2, 4, GFX_YELLOW);
    }
}

static void draw_player(void)
{
    if (!p_alive) return;
    int sx = OFF_X + px * TILE_SZ;
    int sy = OFF_Y + py * TILE_SZ;
    gfx_fill(sx + 2, sy + 1, 6, 4, GFX_WHITE);  /* Шлем */
    gfx_fill(sx + 3, sy + 2, 4, 2, COL_PLAYER); /* Лицо */
    gfx_fill(sx + 2, sy + 5, 6, 4, 0x001F);     /* Синий костюм */
}

static void draw_enemy(const struct enemy *e)
{
    if (!e->alive) return;
    int sx = OFF_X + e->x * TILE_SZ;
    int sy = OFF_Y + e->y * TILE_SZ;
    gfx_fill(sx + 1, sy + 2, 8, 6, COL_ENEMY);
    gfx_fill(sx + 2, sy + 1, 6, 8, COL_ENEMY);
    gfx_fill(sx + 3, sy + 3, 1, 2, GFX_WHITE);
    gfx_fill(sx + 6, sy + 3, 1, 2, GFX_WHITE);
}

static void draw_bomb(void)
{
    if (!bomb_active) return;
    int sx = OFF_X + bx * TILE_SZ;
    int sy = OFF_Y + by * TILE_SZ;
    uint16_t col = ((TICK / 15u) & 1u) ? COL_BOMB : GFX_RED;
    gfx_fill(sx + 2, sy + 2, 6, 6, col);
    gfx_fill(sx + 4, sy + 1, 2, 2, GFX_YELLOW);
}

static void draw_hud(void)
{
    char buf[12];
    gfx_fill(38, 2, 44, 9, GFX_BLACK);
    num_to_str(buf, score);
    gfx_text(38, 2, buf, 7, 1, GFX_YELLOW, GFX_BLACK);

    gfx_fill(112, 2, 20, 9, GFX_BLACK);
    num_to_str(buf, stage + 1);
    gfx_text(112, 2, buf, 3, 1, COL_DOOR, GFX_BLACK);
}

/* Проверка проходимости клетки */
static int is_free(int8_t x, int8_t y)
{
    if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return 0;
    if (map[y][x] == T_WALL || map[y][x] == T_BRICK) return 0;
    if (bomb_active && bx == x && by == y) return 0;
    return 1;
}

/* Генерация нового уровня */
static void init_level(void)
{
    int8_t brick_x[80], brick_y[80];
    int b_cnt = 0;

    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            hidden[y][x] = T_EMPTY;
            if (x == 0 || y == 0 || x == MAP_W - 1 || y == MAP_H - 1 || ((x & 1) == 0 && (y & 1) == 0)) {
                map[y][x] = T_WALL;
            } else if ((x <= 2 && y <= 2)) {
                /* Безопасный угол для старта игрока */
                map[y][x] = T_EMPTY;
            } else if ((fast_rand() % 100u) < 42u) {
                map[y][x] = T_BRICK;
                if (b_cnt < 80) {
                    brick_x[b_cnt] = (int8_t)x;
                    brick_y[b_cnt] = (int8_t)y;
                    b_cnt++;
                }
            } else {
                map[y][x] = T_EMPTY;
            }
            draw_tile((int8_t)x, (int8_t)y);
        }
    }

    /* Прячем дверь и бонус огня под случайными кирпичами */
    if (b_cnt > 0) {
        int d = (int)(fast_rand() % (unsigned)b_cnt);
        hidden[brick_y[d]][brick_x[d]] = T_DOOR;
        if (b_cnt > 1) {
            int p = (d + 1) % b_cnt;
            hidden[brick_y[p]][brick_x[p]] = T_POWER;
        }
    } else {
        map[MAP_H - 2][MAP_W - 2] = T_DOOR;
        draw_tile(MAP_W - 2, MAP_H - 2);
    }

    px = 1; py = 1; p_alive = 1;
    bomb_active = 0;
    fire_active = 0;
    draw_player();

    /* Расстановка 4 врагов вдали от игрока */
    for (int i = 0; i < 4; i++) {
        enemies[i].alive = 0;
        for (int tries = 0; tries < 40; tries++) {
            int8_t ex = (int8_t)(1 + (fast_rand() % (MAP_W - 2)));
            int8_t ey = (int8_t)(1 + (fast_rand() % (MAP_H - 2)));
            if ((ex + ey) > 6 && map[ey][ex] == T_EMPTY) {
                enemies[i].x = ex;
                enemies[i].y = ey;
                enemies[i].dir = (uint8_t)(fast_rand() & 3u);
                enemies[i].alive = 1;
                draw_enemy(&enemies[i]);
                break;
            }
        }
    }
    draw_hud();
}

/* Отрисовка или очистка лучей взрыва */
static void process_explosion(int draw_fire)
{
    static const int8_t DX[4] = { 0, 1, 0, -1 };
    static const int8_t DY[4] = { -1, 0, 1, 0 };

    /* Центр взрыва */
    if (draw_fire) {
        gfx_fill(OFF_X + bx * TILE_SZ + 1, OFF_Y + by * TILE_SZ + 1, TILE_SZ - 2, TILE_SZ - 2, COL_FIRE);
        if (px == bx && py == by) p_alive = 0;
        for (int e = 0; e < 4; e++) {
            if (enemies[e].alive && enemies[e].x == bx && enemies[e].y == by) {
                enemies[e].alive = 0; score += 100; draw_hud();
            }
        }
    } else {
        draw_tile(bx, by);
    }

    /* 4 луча взрыва */
    for (int d = 0; d < 4; d++) {
        for (uint8_t r = 1; r <= blast_range; r++) {
            int8_t tx = bx + DX[d] * (int8_t)r;
            int8_t ty = by + DY[d] * (int8_t)r;
            if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H) break;

            uint8_t t = map[ty][tx];
            if (t == T_WALL) break;

            if (draw_fire) {
                if (t == T_BRICK) {
                    map[ty][tx] = hidden[ty][tx];
                    hidden[ty][tx] = T_EMPTY;
                    score += 10;
                    draw_hud();
                    gfx_fill(OFF_X + tx * TILE_SZ + 1, OFF_Y + ty * TILE_SZ + 1, TILE_SZ - 2, TILE_SZ - 2, COL_FIRE);
                    break; /* Взрыв не проходит сквозь кирпич дальше */
                }
                gfx_fill(OFF_X + tx * TILE_SZ + 1, OFF_Y + ty * TILE_SZ + 1, TILE_SZ - 2, TILE_SZ - 2, COL_FIRE);
                if (px == tx && py == ty) p_alive = 0;
                for (int e = 0; e < 4; e++) {
                    if (enemies[e].alive && enemies[e].x == tx && enemies[e].y == ty) {
                        enemies[e].alive = 0;
                        score += 100;
                        draw_hud();
                    }
                }
            } else {
                draw_tile(tx, ty);
            }
        }
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(2,   2, "PTS:", 5, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(88,  2, "ST:",  4, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(128, 2, "B:EX", 5, 1, GFX_RED,   GFX_BLACK);

    /* Боковая мини-памятка справа */
    gfx_text(136, 20, "2/4/6", 6, 1, COL_BOMB,   GFX_BLACK);
    gfx_text(136, 30, "5/8:D", 6, 1, COL_BOMB,   GFX_BLACK);
    gfx_text(136, 46, "0:BMB", 6, 1, COL_PLAYER, GFX_BLACK);
    gfx_text(136, 62, "*:PAU", 6, 1, COL_DOOR,   GFX_BLACK);

    rng_state = TICK ^ 0x9E3779B9u;
    if (!rng_state) rng_state = 555;
    score = 0;
    stage = 0;
    blast_range = 1;

    init_level();

    uint32_t last_ai = TICK;
    key_last = key_seen = h9_key();

    for (;;) {
        uint32_t now = TICK;
        unsigned k = poll_key();

        /* Выход по Back */
        if (k == KEY_BACK) return 0;

        /* Пауза по * */
        if (k == KEY_STAR) {
            gfx_text(36, 60, "PAUSED", 12, 1, COL_DOOR, GFX_BLACK);
            gfx_wait_release();
            for (;;) {
                unsigned pk = poll_key();
                if (pk == KEY_STAR) break;
                if (pk == KEY_BACK) return 0;
                h9_delay(10);
            }
            gfx_wait_release();
            /* перерисовать поле и объекты */
            for (int y = 0; y < MAP_H; y++)
                for (int x = 0; x < MAP_W; x++) draw_tile((int8_t)x, (int8_t)y);
            if (p_alive) draw_player();
            if (bomb_active) draw_bomb();
            for (int e = 0; e < 4; e++) if (enemies[e].alive) draw_enemy(&enemies[e]);
            draw_hud();
            continue;
        }

        /* Управление Бомберменом */
        if (p_alive) {
            int8_t nx = px, ny = py;
            int moved = 0;
            if (k == KEY_2)      { ny--; moved = 1; }
            else if (k == KEY_8 || k == KEY_5) { ny++; moved = 1; }
            else if (k == KEY_4) { nx--; moved = 1; }
            else if (k == KEY_6) { nx++; moved = 1; }
            else if (k == KEY_0 && !bomb_active) {
                bomb_active = 1;
                bx = px;
                by = py;
                bomb_tick = now;
                draw_bomb();
            }

            if (moved && is_free(nx, ny)) {
                draw_tile(px, py);
                if (bomb_active && bx == px && by == py) draw_bomb();
                px = nx;
                py = ny;

                /* Подбор бонуса или вход в дверь */
                if (map[py][px] == T_POWER) {
                    map[py][px] = T_EMPTY;
                    if (blast_range < 4) blast_range++;
                    score += 50;
                    draw_hud();
                } else if (map[py][px] == T_DOOR) {
                    int alive_cnt = 0;
                    for (int e = 0; e < 4; e++) if (enemies[e].alive) alive_cnt++;
                    if (alive_cnt == 0) {
                        stage++;
                        gfx_text(34, 60, "STAGE CLEAR", 12, 1, COL_DOOR, GFX_BLACK);
                        h9_delay(1200);
                        init_level();
                        last_ai = TICK;
                        continue;
                    }
                }
draw_player();
            }
        }

        /* Таймер бомбы (2.2 секунды = 220 тиков) */
        if (bomb_active) {
            if ((now - bomb_tick) >= 220u) {
                bomb_active = 0;
                fire_active = 1;
                fire_tick = now;
                process_explosion(1);
            } else {
                draw_bomb();
            }
        }

        /* Затухание пламени взрыва (0.4 секунды = 40 тиков) */
        if (fire_active && (now - fire_tick) >= 40u) {
            fire_active = 0;
            process_explosion(0);
            if (p_alive) draw_player();
            for (int e = 0; e < 4; e++) if (enemies[e].alive) draw_enemy(&enemies[e]);
        }

        /* Движение врагов */
        uint32_t ai_speed = (stage < 5) ? (45u - stage * 5u) : 20u;
        if ((now - last_ai) >= ai_speed) {
            last_ai = now;
            static const int8_t DX[4] = { 0, 1, 0, -1 };
            static const int8_t DY[4] = { -1, 0, 1, 0 };

            for (int i = 0; i < 4; i++) {
                if (!enemies[i].alive) continue;
                if ((fast_rand() & 3u) == 0) {
                    enemies[i].dir = (uint8_t)(fast_rand() & 3u);
                }
                int8_t nx = enemies[i].x + DX[enemies[i].dir];
                int8_t ny = enemies[i].y + DY[enemies[i].dir];

                draw_tile(enemies[i].x, enemies[i].y);
                if (is_free(nx, ny)) {
                    enemies[i].x = nx;
                    enemies[i].y = ny;
                } else {
                    enemies[i].dir = (uint8_t)(fast_rand() & 3u);
                }
                draw_enemy(&enemies[i]);

                /* Столкновение врага с игроком */
                if (enemies[i].x == px && enemies[i].y == py) {
                    p_alive = 0;
                }
            }
        }

        /* Проверка проигрыша */
        if (!p_alive) {
            gfx_text(36, 60, "GAME OVER", 10, 1, GFX_RED, GFX_BLACK);
            gfx_wait_key();
            return 0;
        }

        h9_delay(10);
    }
}
