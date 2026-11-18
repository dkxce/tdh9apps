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
    KEY_STAR = 0x13,
    KEY_HASH = 0x14
};

/* Поле 13x11 клеток по 10x10 пикселей (130x110 px) */
#define MAP_W   13
#define MAP_H   11
#define TILE_SZ 10
#define OFF_X   2
#define OFF_Y   14

/* Типы клеток */
enum {
    T_EMPTY  = 0,
    T_WALL   = 1, /* Неразрушаемый блок */
    T_BRICK  = 2, /* Разрушаемый кирпич */
    T_DOOR   = 3, /* Выход на следующий этап */
    T_FIRE   = 4, /* Бонус: +1 к дальности взрыва */
    T_BOMB   = 5, /* Бонус: +1 одновременная бомба (до 3) */
    T_DETON  = 6, /* Бонус: ручной детонатор (кнопка 0) */
    T_PASS   = 7  /* Бонус: хождение сквозь кирпичи */
};

/* Типы врагов */
enum {
    E_BALLOM = 0, /* Обычный розовый шарик */
    E_CHASER = 1, /* Быстрый синий охотник */
    E_GHOST  = 2  /* Зеленый призрак (ходит сквозь кирпичи) */
};

#define MAX_BOMBS   3
#define MAX_ENEMIES 5

struct bomb {
    int8_t x, y;
    uint32_t explode_tick;
    uint8_t active;
};

struct enemy {
    int8_t x, y;
    uint8_t dir;
    uint8_t type;
    uint8_t alive;
};

/* Палитры биомов (стена, кирпич) */
static const uint16_t THEME_WALL[4]  = { 0x8410, 0x03E0, 0x041F, 0x8000 };
static const uint16_t THEME_BRICK[4] = { 0xC200, 0x8200, 0x5DFF, 0xFD20 };

static uint8_t map[MAP_H][MAP_W];
static uint8_t hidden[MAP_H][MAP_W];

static int8_t px, py;
static uint8_t p_alive;
static uint8_t blast_range;
static uint8_t max_bombs;
static uint8_t has_detonator;
static uint8_t has_wallpass;

static uint32_t score;
static uint8_t stage;
static uint8_t theme;

static struct bomb bombs[MAX_BOMBS];
static struct enemy enemies[MAX_ENEMIES];
static uint32_t rng_state;

/* Антидребезг клавиатуры */
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

/* Отрисовка клетки карты */
static void draw_tile(int8_t x, int8_t y)
{
    int sx = OFF_X + x * TILE_SZ;
    int sy = OFF_Y + y * TILE_SZ;
    uint8_t t = map[y][x];

    gfx_fill(sx, sy, TILE_SZ, TILE_SZ, GFX_BLACK);

    if (t == T_WALL) {
        gfx_fill(sx, sy, TILE_SZ, TILE_SZ, THEME_WALL[theme]);
        gfx_fill(sx + 2, sy + 2, TILE_SZ - 4, TILE_SZ - 4, 0xC618);
    } else if (t == T_BRICK) {
        gfx_fill(sx, sy, TILE_SZ, TILE_SZ, THEME_BRICK[theme]);
        gfx_fill(sx, sy + 4, TILE_SZ, 1, GFX_BLACK);
        gfx_fill(sx + 5, sy, 1, 4, GFX_BLACK);
        gfx_fill(sx + 2, sy + 5, 1, 5, GFX_BLACK);
    } else if (t == T_DOOR) {
        gfx_fill(sx + 1, sy + 1, TILE_SZ - 2, TILE_SZ - 2, 0x07E0);
        gfx_fill(sx + 3, sy + 3, TILE_SZ - 6, TILE_SZ - 6, GFX_BLACK);
    } else if (t >= T_FIRE) {
        /* Рамка бонуса */
        uint16_t bg = (t == T_FIRE) ? GFX_RED : (t == T_BOMB) ? 0x001F : (t == T_DETON) ? 0x8010 : 0x03E0;
        gfx_fill(sx + 1, sy + 1, TILE_SZ - 2, TILE_SZ - 2, bg);
        gfx_fill(sx + 3, sy + 3, 4, 4, GFX_YELLOW);
    }
}

static void draw_player(void)
{
    if (!p_alive) return;
    int sx = OFF_X + px * TILE_SZ;
    int sy = OFF_Y + py * TILE_SZ;
    gfx_fill(sx + 2, sy + 1, 6, 4, GFX_WHITE);
    gfx_fill(sx + 3, sy + 2, 4, 2, 0xFFE0);
    gfx_fill(sx + 2, sy + 5, 6, 4, has_wallpass ? 0xF81F : 0x07FF);
}

static void draw_enemy(const struct enemy *e)
{
    if (!e->alive) return;
    int sx = OFF_X + e->x * TILE_SZ;
    int sy = OFF_Y + e->y * TILE_SZ;
    uint16_t col = (e->type == E_BALLOM) ? 0xF81F : (e->type == E_CHASER) ? 0x07FF : 0x07E0;

    gfx_fill(sx + 1, sy + 2, 8, 6, col);
    gfx_fill(sx + 2, sy + 1, 6, 8, col);
    gfx_fill(sx + 3, sy + 3, 1, 2, GFX_WHITE);
    gfx_fill(sx + 6, sy + 3, 1, 2, GFX_WHITE);
}

static void draw_bombs(void)
{
    uint16_t col = ((TICK / 12u) & 1u) ? 0x07FF : GFX_RED;
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (!bombs[i].active) continue;
        int sx = OFF_X + bombs[i].x * TILE_SZ;
        int sy = OFF_Y + bombs[i].y * TILE_SZ;
        gfx_fill(sx + 2, sy + 2, 6, 6, col);
        gfx_fill(sx + 4, sy + 1, 2, 2, GFX_YELLOW);
    }
}

static void draw_hud(void)
{
    char buf[12];
    gfx_fill(34, 2, 44, 9, GFX_BLACK);
    num_to_str(buf, score);
    gfx_text(34, 2, buf, 7, 1, GFX_YELLOW, GFX_BLACK);

    gfx_fill(106, 2, 22, 9, GFX_BLACK);
    num_to_str(buf, stage + 1);
    gfx_text(106, 2, buf, 3, 1, 0x07E0, GFX_BLACK);

    /* Индикаторы активных улучшений справа */
    gfx_text(136, 84, has_detonator ? "#:DET" : "     ", 5, 1, 0xF81F, GFX_BLACK);
    gfx_text(136, 96, has_wallpass  ? "PASS " : "     ", 5, 1, 0x07E0, GFX_BLACK);
}

static int bomb_at(int8_t x, int8_t y)
{
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (bombs[i].active && bombs[i].x == x && bombs[i].y == y) return 1;
    }
    return 0;
}

static int player_can_enter(int8_t x, int8_t y)
{
    if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return 0;
    uint8_t t = map[y][x];
    if (t == T_WALL) return 0;
    if (t == T_BRICK && !has_wallpass) return 0;
    if (bomb_at(x, y)) return 0;
    return 1;
}

static int enemy_can_enter(int8_t x, int8_t y, uint8_t type)
{
    if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return 0;
    uint8_t t = map[y][x];
    if (t == T_WALL) return 0;
    if (t == T_BRICK && type != E_GHOST) return 0;
    if (bomb_at(x, y)) return 0;
    return 1;
}

/* Взрыв конкретной бомбы */
static void detonate_bomb(int idx)
{
    static const int8_t DX[4] = { 0, 1, 0, -1 };
    static const int8_t DY[4] = { -1, 0, 1, 0 };

    int8_t bx = bombs[idx].x;
    int8_t by = bombs[idx].y;
    bombs[idx].active = 0;

    /* Рисуем вспышку огня и проверяем урон */
    int8_t fire_x[20], fire_y[20];
    int f_cnt = 0;

    fire_x[f_cnt] = bx; fire_y[f_cnt] = by; f_cnt++;

    for (int d = 0; d < 4; d++) {
        for (uint8_t r = 1; r <= blast_range; r++) {
            int8_t tx = bx + DX[d] * (int8_t)r;
            int8_t ty = by + DY[d] * (int8_t)r;
            if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H) break;
            uint8_t t = map[ty][tx];
            if (t == T_WALL) break;

            if (f_cnt < 20) {
                fire_x[f_cnt] = tx;
                fire_y[f_cnt] = ty;
                f_cnt++;
            }
            if (t == T_BRICK) {
                map[ty][tx] = hidden[ty][tx];
                hidden[ty][tx] = T_EMPTY;
                score += 10;
                break;
            }
        }
    }

    for (int i = 0; i < f_cnt; i++) {
        int8_t fx = fire_x[i], fy = fire_y[i];
        gfx_fill(OFF_X + fx * TILE_SZ + 1, OFF_Y + fy * TILE_SZ + 1, TILE_SZ - 2, TILE_SZ - 2, 0xFD20);
        if (px == fx && py == fy) p_alive = 0;
        for (int e = 0; e < MAX_ENEMIES; e++) {
            if (enemies[e].alive && enemies[e].x == fx && enemies[e].y == fy) {
                enemies[e].alive = 0;
                score += (uint32_t)(enemies[e].type + 1) * 100u;
            }
        }
    }

    draw_hud();
    h9_delay(140);

    /* Очистка пламени и восстановление тайлов */
    for (int i = 0; i < f_cnt; i++) {
        draw_tile(fire_x[i], fire_y[i]);
    }
    if (p_alive) draw_player();
    for (int e = 0; e < MAX_ENEMIES; e++) {
        if (enemies[e].alive) draw_enemy(&enemies[e]);
    }
}

/* Генерация этапа Bomberman 2 */
static void init_stage(void)
{
    int8_t bx_list[80], by_list[80];
    int b_cnt = 0;
    theme = (stage / 2u) % 4u;

    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            hidden[y][x] = T_EMPTY;
            if (x == 0 || y == 0 || x == MAP_W - 1 || y == MAP_H - 1 || ((x & 1) == 0 && (y & 1) == 0)) {
                map[y][x] = T_WALL;
            } else if (x <= 2 && y <= 2) {
                map[y][x] = T_EMPTY;
            } else if ((fast_rand() % 100u) < 44u) {
                map[y][x] = T_BRICK;
                if (b_cnt < 80) {
                    bx_list[b_cnt] = (int8_t)x;
                    by_list[b_cnt] = (int8_t)y;
                    b_cnt++;
                }
            } else {
                map[y][x] = T_EMPTY;
            }
            draw_tile((int8_t)x, (int8_t)y);
        }
    }

    /* Прячем дверь выхода и случайный бонус под кирпичи */
    if (b_cnt > 0) {
        int d = (int)(fast_rand() % (unsigned)b_cnt);
        hidden[by_list[d]][bx_list[d]] = T_DOOR;
        if (b_cnt > 1) {
            int p = (d + 1) % b_cnt;
            uint8_t bonus = (uint8_t)(T_FIRE + (stage % 4u));
            hidden[by_list[p]][bx_list[p]] = bonus;
        }
    } else {
        map[MAP_H - 2][MAP_W - 2] = T_DOOR;
        draw_tile(MAP_W - 2, MAP_H - 2);
    }

    for (int i = 0; i < MAX_BOMBS; i++) bombs[i].active = 0;

    px = 1; py = 1; p_alive = 1;
    draw_player();

    /* Количество и типы врагов растут с номером уровня */
    int e_total = 3 + (stage > 2 ? 2 : stage);
    if (e_total > MAX_ENEMIES) e_total = MAX_ENEMIES;

    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemies[i].alive = 0;
        if (i >= e_total) continue;
        for (int tries = 0; tries < 45; tries++) {
            int8_t ex = (int8_t)(1 + (fast_rand() % (MAP_W - 2)));
            int8_t ey = (int8_t)(1 + (fast_rand() % (MAP_H - 2)));
            if ((ex + ey) > 6 && map[ey][ex] == T_EMPTY) {
                enemies[i].x = ex;
                enemies[i].y = ey;
                enemies[i].dir = (uint8_t)(fast_rand() & 3u);
                enemies[i].type = (uint8_t)(i % (stage >= 2 ? 3 : (stage + 1)));
                enemies[i].alive = 1;
                draw_enemy(&enemies[i]);
                break;
            }
        }
    }
    draw_hud();
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(2,   2, "PTS:", 4, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(84,  2, "ST:",  3, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(128, 2, "B:EX", 4, 1, GFX_RED,   GFX_BLACK);

    gfx_text(136, 18, "2/4/6", 5, 1, 0x07FF,  GFX_BLACK);
    gfx_text(136, 26, "5/8:D", 5, 1, 0x07FF,  GFX_BLACK);
    gfx_text(136, 40, "0:BMB", 5, 1, 0xFFE0,  GFX_BLACK);
    gfx_text(136, 52, "#:DET", 5, 1, 0xF81F,  GFX_BLACK);
    gfx_text(136, 66, "*:PAU", 5, 1, 0x07E0,  GFX_BLACK);

    rng_state = TICK ^ 0xB0B2B0B2u;
    if (!rng_state) rng_state = 2026;
    score = 0;
    stage = 0;
    blast_range = 1;
    max_bombs = 1;
    has_detonator = 0;
    has_wallpass = 0;
    key_last = key_seen = h9_key();

    init_stage();
    uint32_t last_ai = TICK;

    for (;;) {
        uint32_t now = TICK;
        unsigned k = poll_key();

        if (k == KEY_BACK) return 0;

        /* Пауза по * */
        if (k == KEY_STAR) {
            gfx_text(36, 60, "PAUSED", 12, 1, 0x07E0, GFX_BLACK);
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
            if (p_alive) draw_player();
            draw_bombs();
            for (int i = 0; i < MAX_ENEMIES; i++) if (enemies[i].alive) draw_enemy(&enemies[i]);
            draw_hud();
            continue;
        }

        /* Управление игроком */
        if (p_alive && k != 0) {
            int8_t nx = px, ny = py;
            int moved = 0;

            if (k == KEY_2)      { ny--; moved = 1; }
            else if (k == KEY_8 || k == KEY_5) { ny++; moved = 1; }
            else if (k == KEY_4) { nx--; moved = 1; }
            else if (k == KEY_6) { nx++; moved = 1; }
            else if (k == KEY_0 && !bomb_at(px, py)) {
                /* Установка новой бомбы (если не превышен лимит max_bombs) */
                int active_cnt = 0, free_slot = -1;
                for (int i = 0; i < MAX_BOMBS; i++) {
                    if (bombs[i].active) active_cnt++;
                    else if (free_slot < 0) free_slot = i;
                }
                if (active_cnt < max_bombs && free_slot >= 0) {
                    bombs[free_slot].x = px;
                    bombs[free_slot].y = py;
                    bombs[free_slot].explode_tick = now + 240u;
                    bombs[free_slot].active = 1;
                    draw_bombs();
                }
            } else if (k == KEY_HASH && has_detonator) {
                /* Ручной подрыв всех установленных бомб по кнопке # */
                for (int i = 0; i < MAX_BOMBS; i++) {
                    if (bombs[i].active) detonate_bomb(i);
                }
            }

            if (moved && player_can_enter(nx, ny)) {
                draw_tile(px, py);
                if (bomb_at(px, py)) draw_bombs();
                px = nx; py = ny;

                uint8_t t = map[py][px];
                if (t >= T_FIRE) {
                    if (t == T_FIRE && blast_range < 4) blast_range++;
                    if (t == T_BOMB && max_bombs < MAX_BOMBS) max_bombs++;
                    if (t == T_DETON) has_detonator = 1;
                    if (t == T_PASS)  has_wallpass = 1;
                    map[py][px] = T_EMPTY;
                    score += 50;
                    draw_hud();
                } else if (t == T_DOOR) {
                    int alive_cnt = 0;
                    for (int e = 0; e < MAX_ENEMIES; e++) if (enemies[e].alive) alive_cnt++;
                    if (alive_cnt == 0) {
                        stage++;
                        gfx_text(34, 60, "STAGE CLEAR", 12, 1, 0x07E0, GFX_BLACK);
                        h9_delay(1200);
                        init_stage();
                        last_ai = TICK;
                        continue;
                    }
                }
                draw_player();
            }
        }

        /* Проверка таймеров бомб */
        int any_bomb = 0;
        for (int i = 0; i < MAX_BOMBS; i++) {
            if (!bombs[i].active) continue;
            if (!has_detonator && now >= bombs[i].explode_tick) {
                detonate_bomb(i);
            } else {
                any_bomb = 1;
            }
        }
        if (any_bomb) draw_bombs();

        /* Логика 3 типов врагов */
        uint32_t ai_step = (stage < 6) ? (42u - stage * 4u) : 18u;
        if ((now - last_ai) >= ai_step) {
            last_ai = now;
            static const int8_t DX[4] = { 0, 1, 0, -1 };
            static const int8_t DY[4] = { -1, 0, 1, 0 };

            for (int i = 0; i < MAX_ENEMIES; i++) {
                struct enemy *e = &enemies[i];
                if (!e->alive) continue;

                /* Синий охотник (E_CHASER) разворачивается в сторону игрока, если видит его */
                if (e->type == E_CHASER) {
                    if (e->x == px) e->dir = (py < e->y) ? 0 : 2;
                    else if (e->y == py) e->dir = (px < e->x) ? 3 : 1;
                } else if ((fast_rand() & 3u) == 0) {
                    e->dir = (uint8_t)(fast_rand() & 3u);
                }

                int8_t nx = e->x + DX[e->dir];
                int8_t ny = e->y + DY[e->dir];

                draw_tile(e->x, e->y);
                if (enemy_can_enter(nx, ny, e->type)) {
                    e->x = nx;
                    e->y = ny;
                } else {
                    e->dir = (uint8_t)(fast_rand() & 3u);
                }
                draw_enemy(e);

                if (e->x == px && e->y == py) p_alive = 0;
            }
        }

        if (!p_alive) {
            gfx_text(36, 60, "GAME OVER", 10, 1, GFX_RED, GFX_BLACK);
            gfx_wait_key();
            return 0;
        }

        h9_delay(10);
    }
}
