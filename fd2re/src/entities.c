/* entities.c — 实体/动画调度（字段语义经战斗子系统验证） */
#include "entities.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "resource.h"
#include "input.h"
#include "timer.h"
#include "text.h"
#include "video.h"
#include "script.h"
#include "fd2.h"
#include "duel.h"

/* 0x13A44; kept as an external here to avoid making the event module part of
 * the entity renderer's public dependency graph. */
extern void tile_event_check(int x, int y, int mode);

void *g_field_map;
int32_t g_field_map_size;
void *g_shape_tiles;
int32_t g_shape_tiles_size;
void *g_shape_map;
int32_t g_shape_map_size;
int32_t g_field_w, g_field_h;

uint8_t (*g_ent_table)[FD2_ENT_REC_SIZE];
int32_t  g_ent_count;

/* IDA 0x11CAC composes into g_shadow_buf2 (456-byte pitch) and copies its
 * 312x192 crop to VGA+1284.  The host keeps the same geometry instead of
 * rendering map cells directly into a 320-byte screen. */
#define FIELD_SHADOW_PITCH 456
#define FIELD_SHADOW_ROWS  336
#define FIELD_RENDER_OFF   32904u
#define ENTITY_RENDER_OFF  30168u
#define VGA_FIELD_OFF      1284u
static uint8_t s_field_shadow[FIELD_SHADOW_PITCH * FIELD_SHADOW_ROWS];
/* 0x53C0B/0x53C07: the stationary and moving sprite clocks are separate.
 * 0x53C1F is a third, 20-step clock selecting FDOTHER block 3 LUT frames;
 * 0x53C0F/0x539F4 are their shared last-tick anchors (single originals). */
static uint8_t s_cursor_phase;
static uint8_t s_entity_phase;
static uint8_t s_lut_frame;
static uint8_t s_tile_anim_flip;
static uint32_t s_last_phase_tick;
static uint32_t s_last_lut_tick;
static int s_field_shadow_valid;

/* ---- 0x10652 战场动画背景层（2026-09-07 定案，仅 st9/24/25/28/29）----
 * 0x53AFF = FDOTHER blk15（9/24/25）/ blk55（28/29）包；0x53B03 =
 * malloc(64000) scratch，由 0x4EEE0 每 tick 重建（192 行 × 自
 * pkg+4+320r+byte_627C8[行相位] 拷 312B；相位=行+帧 mod 16，帧每 tick
 * +1 → 0..4px 横向波光）。field_tile_render 战斗分支恒将其 blit 到
 * 带+32904+g_walk_pix_off = 恰为当前可见区 → 背景**屏幕固定**，从不随
 * 镜头卷动——"滚动重算点"不存在（§13.39 挂号就此关闭）。序章态
 * 31/32 不在装载开关内：无背景层。 */
static const uint8_t s_backdrop_phases[16] = {      /* byte_627C8 */
    2, 3, 3, 4, 4, 4, 3, 3, 2, 1, 1, 0, 0, 0, 1, 1
};
static const uint8_t *s_backdrop_pkg;               /* 0x53AFF（战斗态原始包）*/
static size_t s_backdrop_pkg_size;
static uint8_t *s_backdrop_scratch;                 /* 0x53B03（战斗态 scratch）*/
static uint8_t s_backdrop_frame;                    /* 0x539FC */
static uint8_t s_backdrop_scroll_speed;             /* 0x51A10（23 态卷速）*/
static uint8_t *s_backdrop_band;                    /* 0x53AFF（ADV/23 解码带）*/
static int s_backdrop_band_pitch;                   /* 462/408/312 */
static int s_backdrop_band_rows;
static uint32_t s_last_backdrop_tick;               /* 0x539F8 */
static int s_walk_pix_off;                          /* 0x53AF5 g_walk_pix_off */
static int s_walk_cnt_x;                            /* 0x53B07 g_walk_cnt_x */
static int s_walk_cnt_y;                            /* 0x53B0B g_walk_cnt_y */

void battle_backdrop_load(void)
{
    int block;

    free((void *)s_backdrop_pkg);
    s_backdrop_pkg = NULL;
    s_backdrop_pkg_size = 0;
    free(s_backdrop_scratch);
    s_backdrop_scratch = NULL;
    free(s_backdrop_band);
    s_backdrop_band = NULL;
    s_backdrop_band_pitch = 0;
    switch (g_state) {
    case 9: case 24: case 25: block = 15; break;
    case 28: case 29:         block = 55; break;
    default:                  block = -1; break;
    }
    if (block >= 0) {
        /* 战斗态：波光包 + scratch。 */
        s_backdrop_pkg = (const uint8_t *)dat_load_block("FDOTHER.DAT",
                                                         NULL, block);
        s_backdrop_pkg_size = (size_t)g_last_block_size;
        if (!s_backdrop_pkg || s_backdrop_pkg_size < 4u + 320u * 192u) {
            free((void *)s_backdrop_pkg);
            s_backdrop_pkg = NULL;
            s_backdrop_pkg_size = 0;
            return;
        }
        s_backdrop_scratch = malloc(64000);
        if (!s_backdrop_scratch) {
            free((void *)s_backdrop_pkg);
            s_backdrop_pkg = NULL;
            s_backdrop_pkg_size = 0;
        }
        return;
    }
    /* ADV 对（17/21/22/27）与施法态 23：解码静态带（0x10652 case 组）。
     * 17/27 保持 462 宽（仅 21/22 覆写 408——0x106FF 内 switch 直证）。 */
    {
        int pitch, rows, blk;
        void *pkg;

        switch (g_state) {
        case 17: pitch = 462; rows = 226; blk = 16; break;
        case 21: pitch = 408; rows = 276; blk = 35; break;
        case 22: pitch = 408; rows = 256; blk = 40; break;
        case 27: pitch = 462; rows = 244; blk = 46; break;
        case 23: pitch = 312; rows = 192; blk = 42; break;
        default: return;
        }
        s_backdrop_band = malloc((size_t)pitch * (size_t)rows);
        if (!s_backdrop_band)
            return;
        s_backdrop_band_pitch = pitch;
        s_backdrop_band_rows = rows;
        pkg = dat_load_block("FDOTHER.DAT", NULL, blk);
        if (pkg) {
            rle_decode_frame((const uint8_t *)pkg, 0, 0,
                             s_backdrop_band, pitch, -1);
            free(pkg);
        }
        if (g_state != 23) {
            pkg = dat_load_block("FDOTHER.DAT", NULL, blk + 1);
            if (pkg) {
                rle_decode_frame((const uint8_t *)pkg, 0, rows / 2,
                                 s_backdrop_band, pitch, -1);
                free(pkg);
            }
        } else {
            s_backdrop_scroll_speed = 0;    /* 0x51A10：st23_enter 置速 */
            battle_backdrop_scroll(0);      /* 0x10849 尾 sub_24D22(0) */
        }
    }
}

/* 0x24D22：n!=0 → 置 23 态卷速（byte_51A10）；n==0 → 53AFF 带整体下移
 * 卷速行（底 n 行回卷至顶）= 背景逐 tick 上卷。312 pitch 专属。 */
void battle_backdrop_scroll(int n)
{
    uint8_t *tmp;

    if (n) {
        s_backdrop_scroll_speed = (uint8_t)n;
        return;
    }
    n = s_backdrop_scroll_speed;
    if (!s_backdrop_band || s_backdrop_band_pitch != 312 || n <= 0)
        return;
    tmp = malloc(312 * (size_t)n);
    if (!tmp)
        return;
    memcpy(tmp, s_backdrop_band + 312 * (192 - n), 312 * (size_t)n);
    for (int i = 191 - n; i >= 0; --i)
        memmove(s_backdrop_band + 312 * (size_t)(i + n),
                s_backdrop_band + 312 * (size_t)i, 312);
    memcpy(s_backdrop_band, tmp, 312 * (size_t)n);
    free(tmp);
}

/* 地形通道前置背景层（0x11F4E 各 case）：dst 恒为带内当前可见区
 * （FIELD_RENDER_OFF+g_walk_pix_off）。按态分三路：
 * - 9/24/25/28/29 波光：tick 变化时 0x4EEE0 重建 scratch（帧 0..15），
 *   恒定 blit（320 pitch）——屏幕固定。
 * - 17/21/22/27 视差：src = 3*map_x0 + 2*pitch*map_y0 + cnt_x/2 +
 *   pitch*(cnt_y/3)（横 1/8、纵 1/12 视差；g_walk_cnt_x/y 做步内平滑
 *   插值——原版 0x12094 公式直译）。静态带，无逐 tick 动画。
 * - 23 施法：tick 变化时 battle_backdrop_scroll(0) 上卷（0x120B1），
 *   恒定 blit（312 pitch）。 */
void field_backdrop_render(uint8_t *band, int map_x0, int map_y0)
{
    uint32_t now = bios_tick();

    if (!band)
        return;
    switch (g_state) {
    case 9: case 24: case 25: case 28: case 29: {
        if (!s_backdrop_pkg || !s_backdrop_scratch)
            return;
        if (now != s_last_backdrop_tick) {
            int f = s_backdrop_frame;
            for (int row = 0; row < 192; row++) {
                memcpy(s_backdrop_scratch + 320 * row,
                       s_backdrop_pkg + 4u + 320u * row
                           + s_backdrop_phases[f & 15],
                       312);
                f++;
            }
            s_backdrop_frame = (uint8_t)((s_backdrop_frame + 1u) & 15u);
            s_last_backdrop_tick = now;
        }
        blit_rows(band + FIELD_RENDER_OFF + s_walk_pix_off,
                  FIELD_SHADOW_PITCH, s_backdrop_scratch, 320, 312, 192);
        return;
    }
    case 17: case 21: case 22: case 27: {
        int pitch = (g_state == 17 || g_state == 27) ? 462 : 408;
        ptrdiff_t off;

        if (!s_backdrop_band || s_backdrop_band_pitch != pitch)
            return;
        off = (ptrdiff_t)3 * map_x0 + (ptrdiff_t)2 * pitch * map_y0
            + s_walk_cnt_x / 2 + (ptrdiff_t)pitch * (s_walk_cnt_y / 3);
        if (off < 0
            || off + 312 + (ptrdiff_t)pitch * 191
               > (ptrdiff_t)pitch * s_backdrop_band_rows)
            return;
        blit_rows(band + FIELD_RENDER_OFF + s_walk_pix_off,
                  FIELD_SHADOW_PITCH, s_backdrop_band + off, pitch,
                  312, 192);
        return;
    }
    case 23:
        if (!s_backdrop_band || s_backdrop_band_pitch != 312)
            return;
        if (now != s_last_backdrop_tick) {
            battle_backdrop_scroll(0);
            s_last_backdrop_tick = now;
        }
        blit_rows(band + FIELD_RENDER_OFF + s_walk_pix_off,
                  FIELD_SHADOW_PITCH, s_backdrop_band, 312, 312, 192);
        return;
    default:
        return;
    }
}

void field_render_entities(uint8_t *shadow);
static int shape_tile_stream(uint16_t tile, const uint8_t **stream,
                             size_t *stream_len, int *tile_w, int *tile_h);

/* 456 影子层访问器（2026-09-06：供 fx 特效管线复用——布局与原版
 * g_shadow_buf2 逐字一致：pitch 456 / 中带 +32904 / 312x192 -> (4,4)）。 */
uint8_t *battle_shadow_layer(void)
{
    return s_field_shadow;
}

void battle_shadow_blit(void)
{
    blit_rows(vram_base() + VGA_FIELD_OFF, FD2_SCREEN_W,
              s_field_shadow + FIELD_RENDER_OFF, FIELD_SHADOW_PITCH,
              312, 192);
}

/* 0x1AEB1 pct_value_stamp：地形攻/防% 标签（blk5 帧 131，负值帧 132）
 * + 2 位数字（色 31，@+8）。 */
static void pct_value_stamp(uint8_t *dst, int pitch, int val)
{
    int frame = 131;
    if (val < 0) {
        frame = 132;
        val = -val;
    }
    package_blit_frame(g_death_fx_pkg, dst, pitch, frame);
    number_stamp_digits(dst + 8, pitch, val, 31, 2);
}

/* 0x1ACF3 tile_info_panel_draw —— 战场格子信息区（底部随光标避让换边：
 * 光标视图 y<=5 且 x<3 → x=242 右置；y>5 且 x>9 → x=1 左置；其余沿用
 * 上次位置 0x51A0C）。底板 = blk5 帧 130；地形贴图 = FDSHAP 偶块 tile 流
 * sprite24 直贴 @+5*pitch+6；ATK/DEF% @+8/+19*pitch+43；光标单位
 * （ent[7]!=121 且非 (ent[31]==10 && ent[6]==1)）→ standing 帧
 * g_cursor_phase(3→1) 贴地形位 + value_stamp HP @+21*pitch+9。
 * 门条件 byte_51AAB（g_save_flag_51aab）&& g_transition_busy。 */
void tile_info_panel_draw(uint8_t *dst, int pitch)
{
    static int s_panel_x;                     /* 0x51A0C */
    uint8_t info[8];
    uint8_t *v6;
    const uint8_t *stream;
    size_t stream_len;
    int tw, th;
    int u;

    if (!g_save_flag_51aab || !g_transition_busy || !g_death_fx_pkg)
        return;
    if (g_cursor_view_y <= 5 || g_cursor_view_x >= 3) {
        if (g_cursor_view_y > 5 && g_cursor_view_x > 9)
            s_panel_x = 1;
    } else {
        s_panel_x = 242;
    }
    v6 = dst + s_panel_x + 157 * pitch;
    package_blit_frame(g_death_fx_pkg, v6, pitch, 130);
    tile_lookup((int)g_cursor_x, (int)g_cursor_y, info);
    if (shape_tile_stream((uint16_t)(info[0] | (info[1] << 8)),
                          &stream, &stream_len, &tw, &th) == 0)
        /* 0x1ADC4 调 sub_4E22A（透明版）——op11 跳过保留面板底。 */
        sprite24_stamp_transparent(stream, v6 + 5 * pitch + 6, pitch);
    pct_value_stamp(v6 + 8 * pitch + 43, pitch, g_tile_atk_pct[info[5]]);
    pct_value_stamp(v6 + 19 * pitch + 43, pitch, g_tile_def_pct[info[5]]);
    u = ent_at_cursor();
    if (u != -1) {
        uint8_t *rec = g_ent_table[u];
        if (rec[7] != 121 && (rec[31] != 10 || rec[6] != 1)) {
            int phase = s_cursor_phase == 3 ? 1 : s_cursor_phase;
            const uint8_t *tbl;
            if (g_standing_sprites && rec[2] < 40) {
                tbl = g_standing_sprites + 4u * (12u * rec[2] + (uint32_t)phase);
                /* 0x1AE86 同为 sub_4E22A 透明版。 */
                sprite24_stamp_transparent(g_standing_sprites
                               + ((uint32_t)tbl[0] | ((uint32_t)tbl[1] << 8)
                                  | ((uint32_t)tbl[2] << 16)
                                  | ((uint32_t)tbl[3] << 24)),
                               v6 + 5 * pitch + 6, pitch);
            }
            value_stamp(v6 + 21 * pitch + 9, pitch,
                        rec[64] | (rec[65] << 8), rec[66] | (rec[67] << 8));
        }
    }
}

/* 0x1297D anim_phase_tick：光标相位钟（距上次锚点 >4 tick 或回绕则 +1，
 * 0..3 循环）+ 实体相位钟（每次调用 +1）。anim_tick_update 与行走原语
 * 逐帧驱动，共享同一锚点（原版单一 0x53C0F）。 */
static void anim_phase_tick(void)
{
    uint32_t now = bios_tick();

    if ((int32_t)(now - s_last_phase_tick) > 4 || now < s_last_phase_tick) {
        s_cursor_phase = (uint8_t)((s_cursor_phase + 1u) & 3u);
        s_last_phase_tick = now;
    }
    s_entity_phase = (uint8_t)((s_entity_phase + 1u) & 3u);
}

/* 0x1297D anim_phase_tick 公开包装（confirm 等待期推进实体相位钟；
 * 影带本体经 battle_shadow_layer() 访问）。 */
void field_phase_tick(void)
{
    anim_phase_tick();
}

/* 0x53C07/0x53C0B 相位钟只读访问（0x1DA16 单位立绘重选帧用）：
 * 行走中（rec[4]!=0）取实体钟，静止取光标钟；3 折叠为 1 由调用方做。 */
int field_entity_phase(void)
{
    return s_entity_phase;
}

int field_cursor_phase(void)
{
    return s_cursor_phase;
}

/* 0x126F7 field_cell_marker(x, y, marker)：视口剔除后把 FDOTHER blk1 帧
 * [marker]（24x24 4-op 透明流）盖到影带渲染区 (x,y) 格——选框/攻击
 * 指示形状的格级原语。偏移 = ((y-scroll_y)*456+(x-scroll_x))*24。 */
static void field_cell_marker(int x, int y, int marker)
{
    ptrdiff_t off;

    if (!g_pkg_fdother_1
        || x < (int)g_scroll_x || x >= (int)g_scroll_x + g_view_w
        || y < (int)g_scroll_y || y >= (int)g_scroll_y + g_view_h)
        return;
    off = (ptrdiff_t)FIELD_RENDER_OFF
        + 24 * (ptrdiff_t)(x - (int)g_scroll_x)
        + 24 * (ptrdiff_t)FIELD_SHADOW_PITCH * (ptrdiff_t)(y - (int)g_scroll_y);
    sprite24_stamp_transparent(package_frame_ptr(g_pkg_fdother_1, marker),
                                s_field_shadow + off, FIELD_SHADOW_PITCH);
}

/* 0x122DC field_marker_layer —— 地形之后/实体之前的选框/攻击指示层，
 * 由 g_text_busy 选模式（2026-09-08 全分支逐指令定案）：
 *   1 = 光标格帧 0（回合开始的当前格选框）；
 *   2 = 光标格帧 1；
 *   3 = 十字 5 格（中心 14 + 2/3/4/5）；
 *   4 = 曼哈顿≤2 全形（帧 1..13）；
 *   5 = 曼哈顿≤3 全形（帧 1..13 + 15..18）；
 *   6 = 非绘制——清光标格洪泛候选字节（field_map[(cy*w+cx)*4+7]=0）；
 *   0/其他 = 无。行走帧路径（walk_frame_compose）不含本层（原版同）。 */
void field_marker_layer(void)
{
    int cx = (int)g_cursor_x, cy = (int)g_cursor_y;

    switch (g_text_busy) {
    case 1:
        field_cell_marker(cx, cy, 0);
        break;
    case 2:
        field_cell_marker(cx, cy, 1);
        break;
    case 3:
        field_cell_marker(cx, cy, 14);
        field_cell_marker(cx, cy - 1, 2);
        field_cell_marker(cx - 1, cy, 3);
        field_cell_marker(cx + 1, cy, 4);
        field_cell_marker(cx, cy + 1, 5);
        break;
    case 4:
        field_cell_marker(cx, cy, 1);
        field_cell_marker(cx, cy - 2, 2);
        field_cell_marker(cx - 2, cy, 3);
        field_cell_marker(cx + 2, cy, 4);
        field_cell_marker(cx, cy + 2, 5);
        field_cell_marker(cx - 1, cy - 1, 6);
        field_cell_marker(cx + 1, cy - 1, 7);
        field_cell_marker(cx - 1, cy + 1, 8);
        field_cell_marker(cx + 1, cy + 1, 9);
        field_cell_marker(cx, cy - 1, 10);
        field_cell_marker(cx - 1, cy, 11);
        field_cell_marker(cx + 1, cy, 12);
        field_cell_marker(cx, cy + 1, 13);
        break;
    case 5:
        field_cell_marker(cx, cy, 1);
        field_cell_marker(cx, cy - 3, 2);
        field_cell_marker(cx - 3, cy, 3);
        field_cell_marker(cx + 3, cy, 4);
        field_cell_marker(cx, cy + 3, 5);
        field_cell_marker(cx - 1, cy - 2, 6);
        field_cell_marker(cx - 2, cy - 1, 6);
        field_cell_marker(cx + 1, cy - 2, 7);
        field_cell_marker(cx + 2, cy - 1, 7);
        field_cell_marker(cx - 1, cy + 2, 8);
        field_cell_marker(cx - 2, cy + 1, 8);
        field_cell_marker(cx + 1, cy + 2, 9);
        field_cell_marker(cx + 2, cy + 1, 9);
        field_cell_marker(cx, cy - 2, 10);
        field_cell_marker(cx - 2, cy, 11);
        field_cell_marker(cx + 2, cy, 12);
        field_cell_marker(cx, cy + 2, 13);
        field_cell_marker(cx - 1, cy - 1, 15);
        field_cell_marker(cx + 1, cy - 1, 16);
        field_cell_marker(cx - 1, cy + 1, 17);
        field_cell_marker(cx + 1, cy + 1, 18);
        break;
    case 6:
        if (g_field_map && g_field_w > 0)
            ((uint8_t *)g_field_map)[4u * (size_t)(cy * g_field_w + cx) + 7]
                = 0;
        break;
    default:
        break;
    }
}

void anim_tick_update(int arg)
{
    /* IDA 0x11CAC: anim_phase_tick 先行，arg==0 时 idle_pump，随后
     * 地形 → 选框层(0x122DC) → 实体 → 信息面板 → 整带呈现。 */
    anim_phase_tick();
    if (!arg)
        idle_pump();
    if (g_field_map && g_shape_tiles && g_field_w > 0 && g_field_h > 0) {
        memset(s_field_shadow, 0, sizeof(s_field_shadow));
        /* 0x11F4E 战斗态背景层先于地形通道（屏幕固定，仅 st9/24/25/28/29）。 */
        field_backdrop_render(s_field_shadow, (int)g_scroll_x, (int)g_scroll_y);
        field_render_viewport(s_field_shadow + FIELD_RENDER_OFF,
                              FIELD_SHADOW_PITCH);
        /* 0x11CF0 sub_122DC：选框/攻击指示层（g_text_busy 选模式）。 */
        field_marker_layer();
        /* ents_render_all@0x127A9 is intentionally after terrain. */
        field_render_entities(s_field_shadow);
        /* 0x11D0A sub_1ACF3：格子信息区在实体之后、blit 之前入带。 */
        tile_info_panel_draw(s_field_shadow + FIELD_RENDER_OFF,
                             FIELD_SHADOW_PITCH);
        blit_rows(vram_base() + VGA_FIELD_OFF, FD2_SCREEN_W,
                  s_field_shadow + FIELD_RENDER_OFF, FIELD_SHADOW_PITCH,
                  312, 192);
        s_field_shadow_valid = 1;
    }
}

/* ---- 0x18B84 move_range_wait_pump（2026-09-08 落地，§13.63 挂号关闭）----
 * battle_unit_turn 洪泛移动域后、battle_move_select(4) 前的"等首键"段
 * （全库唯一调用点 0x18973）。每轮 idle_pump；每 BIOS tick 双 idle_pump
 * （0x18BB4 + 0x18BC5）+ 整场重绘，序列 = anim_tick_update 再插入
 * unit_bars_stamp：地形→选框层→实体→单位血条面板→信息区→blit。
 * 面板 x 随**进门时** g_cursor_view_x 定左右半屏（>=7 → 带内 +2285 =
 * 5 行 +5 列；否则 +2436 = 5 行 +156 列——面板避开光标侧，循环内不再
 * 重估）。键不消费：留给 battle_move_select 首个等待读取。tick 种子
 * 原版取当时 esi（单位记录指针），首轮必渲染；以 -1 等价近似。 */
void move_range_wait_pump(int unit)
{
    uint32_t seen = bios_tick() - 1u;
    size_t off = FIELD_RENDER_OFF
               + (g_cursor_view_x >= 7 ? 2285u : 2436u);

    while (!kbd_key_avail()) {
        idle_pump();
        if (bios_tick() != seen) {
            seen = bios_tick();
            idle_pump();                     /* 0x18BC5 第二次 */
            anim_phase_tick();
            if (g_field_map && g_shape_tiles && g_field_w > 0 && g_field_h > 0
                && g_ent_table && unit >= 0 && unit < g_ent_count) {
                memset(s_field_shadow, 0, sizeof(s_field_shadow));
                field_backdrop_render(s_field_shadow, (int)g_scroll_x,
                                      (int)g_scroll_y);
                field_render_viewport(s_field_shadow + FIELD_RENDER_OFF,
                                      FIELD_SHADOW_PITCH);
                field_marker_layer();
                field_render_entities(s_field_shadow);
                unit_bars_stamp(s_field_shadow + off, FIELD_SHADOW_PITCH,
                                unit);
                tile_info_panel_draw(s_field_shadow + FIELD_RENDER_OFF,
                                     FIELD_SHADOW_PITCH);
                blit_rows(vram_base() + VGA_FIELD_OFF, FD2_SCREEN_W,
                          s_field_shadow + FIELD_RENDER_OFF,
                          FIELD_SHADOW_PITCH, 312, 192);
                s_field_shadow_valid = 1;
            }
        }
    }
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* FDSHAP even block: [u16 w][u16 h][u16 count][u32 offset[count]]...
 * Offsets are block-relative.  This accessor deliberately returns failure
 * on malformed resource data; it must not reinterpret the property block as
 * tile image data. */
static int shape_tile_stream(uint16_t tile, const uint8_t **stream,
                             size_t *stream_len, int *tile_w, int *tile_h)
{
    const uint8_t *shape = (const uint8_t *)g_shape_tiles;
    size_t size;
    uint16_t count;
    size_t table_end;
    uint32_t off, next;

    if (!shape || g_shape_tiles_size < 6)
        return -1;
    size = (size_t)g_shape_tiles_size;
    count = rd_u16(shape + 4);
    table_end = 6u + 4u * (size_t)count;
    if (table_end > size || tile >= count)
        return -1;
    off = rd_u32(shape + 6u + 4u * tile);
    next = (tile + 1u < count)
         ? rd_u32(shape + 6u + 4u * (tile + 1u))
         : (uint32_t)size;
    if (off < table_end || off >= next || next > size)
        return -1;
    *stream = shape + off;
    *stream_len = (size_t)(next - off);
    *tile_w = rd_u16(shape);
    *tile_h = rd_u16(shape + 2);
    return *tile_w > 0 && *tile_h > 0 ? 0 : -1;
}

static int shape_tiles_validate(void)
{
    const uint8_t *shape = (const uint8_t *)g_shape_tiles;
    size_t size, table_end;
    uint16_t count;

    if (!shape || g_shape_tiles_size < 6)
        return -1;
    size = (size_t)g_shape_tiles_size;
    if (rd_u16(shape) != 24 || rd_u16(shape + 2) != 24)
        return -1;
    count = rd_u16(shape + 4);
    table_end = 6u + 4u * (size_t)count;
    if (!count || table_end > size)
        return -1;
    for (uint16_t i = 0; i < count; i++) {
        uint32_t off = rd_u32(shape + 6u + 4u * i);
        uint32_t next = (i + 1u < count)
                      ? rd_u32(shape + 6u + 4u * (i + 1u))
                      : (uint32_t)size;
        if (off < table_end || off >= next || next > size)
            return -1;
    }
    return 0;
}

static const uint8_t *field_lut_frame(void)
{
    static const uint8_t frame_seq[20] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
        10, 9, 8, 7, 6, 5, 4, 3, 2, 1
    };
    const uint8_t *pkg = (const uint8_t *)g_pkg_fdother_3;
    size_t size, table_end, off;
    uint16_t count;
    uint8_t frame;

    if (!pkg || g_pkg_fdother_3_size < 6)
        return NULL;
    size = (size_t)g_pkg_fdother_3_size;
    count = rd_u16(pkg + 4);
    table_end = 6u + 4u * (size_t)count;
    frame = frame_seq[s_lut_frame];
    if (table_end > size || frame >= count)
        return NULL;
    off = rd_u32(pkg + 6u + 4u * frame);
    if (off > size || size - off < 256u)
        return NULL;
    return pkg + off;
}

/* 0x4E016/0x4E0A2, directly over the tile4 stream.  Normal terrain maps
 * destination pixels during transparent runs; the high-terrain overlay
 * leaves them in place.  The interleaved-write opcode leaves its skipped
 * pixels untouched in both variants, exactly as the two original routines. */
static int field_tile_stamp_lut(const uint8_t *stream, size_t stream_len,
                                uint8_t *dst, int pitch, const uint8_t *lut,
                                int transparent_maps_dst)
{
    size_t in = 0;

    for (int row = 0; row < 24; row++) {
        int remaining = 24;
        uint8_t *out = dst + (size_t)row * (size_t)pitch;
        while (remaining) {
            uint8_t ctrl;
            int n;
            if (in >= stream_len)
                return -1;
            ctrl = stream[in++];
            n = (ctrl & 0x3f) + 1;
            switch (ctrl >> 6) {
            case 0: {
                uint8_t color;
                if (n > remaining || in >= stream_len)
                    return -1;
                color = lut[stream[in++]];
                memset(out, color, (size_t)n);
                out += n;
                remaining -= n;
                break;
            }
            case 1: {
                uint8_t color;
                if (2 * n > remaining || in >= stream_len)
                    return -1;
                color = lut[stream[in++]];
                for (int i = 0; i < n; i++) {
                    out[1] = color;
                    out += 2;
                }
                remaining -= 2 * n;
                break;
            }
            case 2:
                if (n > remaining || in > stream_len || stream_len - in < (size_t)n)
                    return -1;
                for (int i = 0; i < n; i++)
                    out[i] = lut[stream[in + (size_t)i]];
                in += (size_t)n;
                out += n;
                remaining -= n;
                break;
            default:
                if (n > remaining)
                    return -1;
                if (transparent_maps_dst) {
                    for (int i = 0; i < n; i++)
                        out[i] = lut[out[i]];
                }
                out += n;
                remaining -= n;
                break;
            }
        }
    }
    return 0;
}

static void field_tile_stamp(uint8_t *dst, int pitch, int x, int y,
                             uint16_t tile, const uint8_t *lut,
                             int transparent_maps_dst)
{
    const uint8_t *stream;
    size_t stream_len;
    int tile_w, tile_h;

    if (shape_tile_stream(tile, &stream, &stream_len, &tile_w, &tile_h) != 0
        || tile_w != 24 || tile_h != 24
        || (lut && field_tile_stamp_lut(stream, stream_len,
                                        dst + (size_t)y * (size_t)pitch + x,
                                        pitch, lut, transparent_maps_dst) != 0)) {
        static int reported;
        if (!reported) {
            fprintf(stderr, "field renderer: invalid FDSHAP tile stream\n");
            reported = 1;
        }
        return;
    }
    if (lut)
        return;
    {
        uint8_t decoded[24 * 24] = {0};
        if (tile4_decode(stream, stream_len, tile_w, tile_h, decoded) < 0) {
            fprintf(stderr, "field renderer: invalid flat tile stream\n");
            return;
        }
        for (int row = 0; row < 24; row++) {
            for (int col = 0; col < 24; col++) {
                uint8_t color = decoded[24 * row + col];
                if (color)
                    dst[(size_t)(y + row) * (size_t)pitch + x + col] = color;
            }
        }
    }
}

/* 0x12AC6 tile_stamp_shaded —— 高地形遮挡盖（仅 ents_shadow_render 调用）。
 * 帧表寻址：+10 读取 = 主表(+6) 的 tile+1 项（原版同）；bit3 双帧动画
 * (+2*g_tile_anim_flip) 先于帧表、bit7(0x80)=高块判定取自调整前的属性
 * 字节；格[3]==255 平贴 sub_4E22A，否则带地形通道同帧钟 LUT
 * (tile4_stamp_lut_alt 0x4E0A2，跳过段不重映射)。2026-09-08 修复：此前
 * 恒走 LUT —— 非高亮格的遮挡盖被 20 帧脉动高亮 LUT 整片变色（帧 0 有
 * 249/256 项重映射），即"角色进建筑物后面被遮挡时错误闪烁"根因；
 * bit3 调整缺失一并补齐。 */
static void field_tile_stamp_shaded(uint8_t *shadow, int map_x, int map_y)
{
    const uint8_t *map = (const uint8_t *)g_field_map;
    const uint8_t *props = (const uint8_t *)g_shape_map;
    const uint8_t *stream;
    size_t stream_len;
    const uint8_t *cell;
    uint8_t prop;
    uint16_t tile;
    int tile_w, tile_h;
    ptrdiff_t dst_off;

    if (!shadow || !map || !props || map_x < (int)g_scroll_x - 1
        || map_x > (int)g_scroll_x + g_view_w
        || map_y < (int)g_scroll_y - 1
        || map_y > (int)g_scroll_y + g_view_h + 1
        || map_x < 0 || map_y < 0 || map_x >= g_field_w || map_y >= g_field_h)
        return;
    cell = map + 4u + 4u * (size_t)(map_x + g_field_w * map_y);
    tile = (uint16_t)(rd_u16(cell) & 0x3ffu);
    if ((size_t)tile * 4u + 4u > (size_t)g_shape_map_size)
        return;
    prop = props[4u * tile];
    if (prop & 0x08u)                              /* 0x12B5F 双帧动画 */
        tile = (uint16_t)(tile + 2u * s_tile_anim_flip);
    if (!(prop & 0x80u))                           /* 0x12B6E 高块判定 */
        return;
    if (shape_tile_stream((uint16_t)(tile + 1u), &stream, &stream_len,
                          &tile_w, &tile_h) != 0
        || tile_w != 24 || tile_h != 24) {
        fprintf(stderr, "field renderer: invalid high-terrain tile stream %u\n",
                (unsigned)(tile + 1u));
        return;
    }
    /* This pass deliberately includes the one-cell border around the
     * viewport.  Keep that coordinate delta signed: FIELD_RENDER_OFF is an
     * unsigned size constant, so adding a top/left border (-1) directly
     * would otherwise wrap before pointer arithmetic. */
    dst_off = (ptrdiff_t)FIELD_RENDER_OFF
            + 24 * (ptrdiff_t)(map_x - (int)g_scroll_x)
            + 24 * (ptrdiff_t)FIELD_SHADOW_PITCH
              * (ptrdiff_t)(map_y - (int)g_scroll_y);
    if (dst_off < 0
        || dst_off + 23 * (ptrdiff_t)FIELD_SHADOW_PITCH + 24
           > (ptrdiff_t)sizeof(s_field_shadow)) {
        fprintf(stderr, "field renderer: high-terrain destination outside shadow\n");
        return;
    }
    if (cell[3] == 255u)                           /* 0x12BE0 平贴 */
        sprite24_stamp_transparent(stream, shadow + dst_off,
                                   FIELD_SHADOW_PITCH);
    else {                                         /* 0x12C00 带 LUT */
        const uint8_t *lut = field_lut_frame();
        if (!lut || field_tile_stamp_lut(stream, stream_len,
                                         shadow + dst_off,
                                         FIELD_SHADOW_PITCH, lut, 0) != 0) {
            fprintf(stderr, "field renderer: invalid high-terrain LUT or stream\n");
            return;
        }
    }
}

void field_render_viewport(uint8_t *dst, int pitch)
{
    field_render_viewport_ext(dst, pitch, 13, 8,
                              (int)g_scroll_x, (int)g_scroll_y);
}

/* 0x11EEE 地形通道通用形。行走原语带窗口几何调用：上跟随时 13x9
 * @map_y=scroll_y-1（多出的一行在顶）、scroll==0 时 13x9 @map_y=0（多出
 * 的一行在底）、左跟随时 14x8 @map_x=scroll_x-1、右跟随时 14x8。LUT 帧
 * 钟（0x12130：距锚点 >2 tick 推进 20 帧表）随每次地形渲染走——原版在
 * field_tile_render 内部，与调用方无关。 */
void field_render_viewport_ext(uint8_t *dst, int pitch, int cols, int rows,
                               int map_x0, int map_y0)
{
    const uint8_t *map = (const uint8_t *)g_field_map;
    const uint8_t *lut;
    uint32_t now;

    if (!dst || pitch < FD2_SCREEN_W || !map || !g_shape_tiles
        || g_field_w <= 0 || g_field_h <= 0)
        return;

    /* 0x11F0D flips this clock once per distinct BIOS tick. */
    now = bios_tick();
    {
        static uint32_t last_tile_tick;
        if (now != last_tile_tick) {
            s_tile_anim_flip ^= 1u;
            last_tile_tick = now;
        }
    }
    /* 0x12130 LUT clock, >2 ticks per step. */
    if ((int32_t)(now - s_last_lut_tick) > 2 || now < s_last_lut_tick) {
        s_lut_frame = (uint8_t)((s_lut_frame + 1u) % 20u);
        s_last_lut_tick = now;
    }
    lut = field_lut_frame();
    if (!lut) {
        fprintf(stderr, "field renderer: FDOTHER block 3 LUT is unavailable\n");
        return;
    }

    /* The caller owns clearing the 456-wide shadow buffer.  This routine is
     * only 0x11EEE's terrain stamp pass. */
    for (int vy = 0; vy < rows; vy++) {
        int map_y = map_y0 + vy;
        if (map_y < 0 || map_y >= g_field_h)
            continue;
        for (int vx = 0; vx < cols; vx++) {
            int map_x = map_x0 + vx;
            const uint8_t *cell;
            uint16_t tile;
            if (map_x < 0 || map_x >= g_field_w)
                continue;
            cell = map + 4u + 4u * (size_t)(map_x + g_field_w * map_y);
            tile = (uint16_t)(rd_u16(cell) & 0x3ffu);
            if ((size_t)tile * 4u + 4u > (size_t)g_shape_map_size)
                return;
            if (((const uint8_t *)g_shape_map)[4u * tile] & 0x08u)
                tile = (uint16_t)(tile + 2u * s_tile_anim_flip);
            else if (((const uint8_t *)g_shape_map)[4u * tile] & 0x10u)
                tile = (uint16_t)(tile + s_cursor_phase / 2u);
            else if (((const uint8_t *)g_shape_map)[4u * tile] & 0x04u)
                tile = (uint16_t)(tile + s_tile_anim_flip);
            field_tile_stamp(dst, pitch, 24 * vx, 24 * vy, tile,
                             cell[3] == 255u ? NULL : lut,
                             cell[3] != 255u);
        }
    }
}

/* 0x127E0 normal entity path.  ent[5] bit7（sub_13512 已行动/演出标记）
 * 0x12949 负标志支选 sub_4E1A6：同一 RLE 流、逐写出像素改 (v&7)+24 ——
 * 调入暗色带 24..31（已行动单位变暗）。fd2re 解码后重映射等价（跳过段
 * 仍透明；流内字面 0 像素原版会写 24，fd2re 统一视 0 为透明——与正常
 * 路径同一局限）。 */
void field_render_entity_offset(uint8_t *shadow, int unit, int row_offset)
{
    uint8_t decoded[24 * 24] = {0};
    const uint8_t *stream;
    size_t stream_len;
    const uint8_t *ent;
    int x, y, dir, phase, frame, dimmed;
    ptrdiff_t base;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return;
    ent = g_ent_table[unit];
    x = ent[0];
    y = ent[1];
    if (x < (int)g_scroll_x - 1 || x > g_view_w + (int)g_scroll_x
        || y < (int)g_scroll_y - 1 || y > g_view_h + (int)g_scroll_y + 1)
        return;
    dimmed = (int8_t)ent[5] < 0;
    dir = ent[3];
    if (dir < 0 || dir > 3) {
        fprintf(stderr, "entity renderer: invalid direction %d for unit %d\n",
                dir, unit);
        return;
    }
    phase = ent[4] ? s_entity_phase : s_cursor_phase;
    if (phase == 3)
        phase = 1;
    if (ent[38])
        phase = 0;
    frame = 3 * dir + phase;
    if (icon_frame_get(ent[2], frame, &stream, &stream_len) != 0
        || tile4_decode(stream, stream_len, 24, 24, decoded) < 0) {
        fprintf(stderr, "entity renderer: invalid cached frame for unit %d\n",
                unit);
        return;
    }

    base = (ptrdiff_t)ENTITY_RENDER_OFF
         + 24 * (x - (int)g_scroll_x)
         + 24 * FIELD_SHADOW_PITCH * (y - (int)g_scroll_y)
         + (ptrdiff_t)row_offset * FIELD_SHADOW_PITCH;
    if (dir == 0)
        base += 1824 * ent[4];
    else if (dir == 1)
        base -= 4 * ent[4];
    else if (dir == 2)
        base -= 1824 * ent[4];
    else
        base += 4 * ent[4];

    for (int row = 0; row < 24; row++) {
        for (int col = 0; col < 24; col++) {
            ptrdiff_t at = base + row * FIELD_SHADOW_PITCH + col;
            uint8_t color = decoded[row * 24 + col];
            if (dimmed && color)
                color = (uint8_t)((color & 7u) + 24u);  /* sub_4E1A6 */
            if (color && at >= 0 && at < (ptrdiff_t)sizeof(s_field_shadow))
                shadow[at] = color;
        }
    }
}

void field_render_entities(uint8_t *shadow)
{
    if (!shadow)
        return;
    for (int i = 0; i < g_ent_count; i++) {
        if (!(g_ent_table[i][5] & 1u))
            field_render_entity_offset(shadow, i, 0);
    }
    field_render_entity_shadows(shadow);
}

void field_render_entity_shadows(uint8_t *shadow)
{
    if (!shadow || !g_ent_table)
        return;
    for (int i = 0; i < g_ent_count; i++) {
        const uint8_t *ent = g_ent_table[i];
        int x, y;

        if ((ent[5] & 1u) || sub_1F183(i))
            continue;
        x = ent[0];
        y = ent[1];
        field_tile_stamp_shaded(shadow, x, y);
        field_tile_stamp_shaded(shadow, x, y - 1);
        if (!ent[4])
            continue;
        switch (ent[3]) {
        case 0:
            field_tile_stamp_shaded(shadow, x, y + 1);
            break;
        case 1:
            field_tile_stamp_shaded(shadow, x - 1, y);
            field_tile_stamp_shaded(shadow, x - 1, y - 1);
            break;
        case 2:
            field_tile_stamp_shaded(shadow, x, y - 2);
            break;
        default:
            field_tile_stamp_shaded(shadow, x + 1, y);
            field_tile_stamp_shaded(shadow, x + 1, y - 1);
            break;
        }
    }
}

int field_shadow_snapshot(uint8_t *dst, size_t size)
{
    if (!dst || size != sizeof(s_field_shadow) || !s_field_shadow_valid)
        return -1;
    memcpy(dst, s_field_shadow, sizeof(s_field_shadow));
    return 0;
}

/* 四向原语共享帧体（0x13185/0x12EAA/0x1300D/0x13315 每帧，时钟已由调用
 * 方走）：g_walk_pix_off(53AF5) 累计 → 战斗态背景层（屏幕固定，见
 * field_backdrop_render）→ 地形超大窗 → 实体层（行走者分数位移由
 * 0x127E0 的 方向系数×ent[4] 项承担）→ 影带滑动 blit（src = band+
 * 32904+g_walk_pix_off，可见区在超大窗内逐帧平移 4px = 平滑镜头）→
 * 等 tick。不清带、不画格子信息区（窗内不透明地形每帧全覆盖）。 */
static void walk_frame_compose(int follow_pix, int win_off, int cols,
                               int rows, int map_x0, int map_y0)
{
    uint8_t *band = battle_shadow_layer();

    s_walk_pix_off += follow_pix;
    field_backdrop_render(band, map_x0, map_y0);
    field_render_viewport_ext(band + win_off, FIELD_SHADOW_PITCH,
                              cols, rows, map_x0, map_y0);
    field_render_entities(band);
    blit_rows(vram_base() + VGA_FIELD_OFF, FD2_SCREEN_W,
              band + FIELD_RENDER_OFF + s_walk_pix_off,
              FIELD_SHADOW_PITCH, 312, 192);
    wait_bios_ticks(1);
}

/* 0x13185 ent_walk_up_one_tile(ent)：单格六帧向上行走（2026-09-07 全解
 * 落地）。ent[3]=2；scroll_y!=0 且 y-scroll_y<2 → 跟随分支（53AF5 每帧
 * -1824 = 456pitch×4px，步末 scroll_y-1），否则 --cursor_view_y（可至
 * -1，原版 0x11B48/0x11B9B jge/jle 有符号语义）。每帧：footstep_sfx +
 * idle_pump + ent[4]=f + anim_phase_tick(0x1297D) + 帧体。上走窗口恒
 * 13x9：scroll!=0 → band+21960 @map_y=scroll_y-1（53AF1=24 为背景窗
 * 下移一 tile，host 背景层以可见区直贴等价）；scroll==0 → band+32904
 * @map_y=0（多的一行在底）。步末 y-1、scroll_y+=跟随量、--cursor_y
 * （无条件，u32 回绕）、ent[4]=0、偏移清零（共享尾 0x12FF0）、
 * tile_event_check(cursor_x, cursor_y, 0)（0x13167）。 */
void ent_walk_up_one_tile(int unit)
{
    uint8_t *ent;
    int old_y;
    int follow_pix = 0;
    int follow_scroll = 0;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return;
    ent = g_ent_table[unit];
    old_y = ent[1];
    ent[3] = 2;

    if ((int)old_y - (int)g_scroll_y < 2 && g_scroll_y) {
        follow_pix = -1824;
        follow_scroll = -1;
    } else {
        --g_cursor_view_y;
    }
    s_walk_cnt_y = g_scroll_y ? 6 : 0;      /* 0x131CC：53B0B 初值 */

    for (int frame = 1; frame <= 6; frame++) {
        footstep_sfx(unit);
        idle_pump();
        ent[4] = (uint8_t)frame;
        anim_phase_tick();
        s_walk_cnt_y += follow_scroll;      /* 0x132A0 */
        if (g_scroll_y) {
            /* 21960 = 32904 - 456*24：九行窗基 = 正常窗上移一 tile。 */
            walk_frame_compose(follow_pix, (int)FIELD_RENDER_OFF - 10944,
                               13, 9, (int)g_scroll_x, (int)g_scroll_y - 1);
        } else {
            walk_frame_compose(0, (int)FIELD_RENDER_OFF,
                               13, 9, (int)g_scroll_x, 0);
        }
    }

    ent[1] = (uint8_t)(old_y - 1);
    if (follow_scroll)
        g_scroll_y = (uint32_t)((int)g_scroll_y + follow_scroll);
    --g_cursor_y;   /* 0x1330A 原版无条件递减（u32 回绕），无 >0 保护 */
    ent[4] = 0;
    s_walk_pix_off = 0;
    s_walk_cnt_y = 0;
    tile_event_check((int)g_cursor_x, (int)g_cursor_y, 0);
}

/* 0x12EAA ent_walk_down_one_tile(ent)：dir=0；y-scroll_y>5 且y-scroll_y>5 且
 * scroll_y != field_h-view_h → 跟随（53AF5 每帧 +1824、步末 scroll_y+1、
 * 窗 13x9 多的一行在底）；否则 ++cursor_view_y、窗 13x8。blit src 每帧
 * +1824 = 镜头平滑下滚。步末 y+1、++cursor_y、尾 0x12FF0 +
 * tile_event_check。g_walk_cnt_y(53B0B) 战斗态渲染无消费者，不建模。 */
void ent_walk_down_one_tile(int unit)
{
    uint8_t *ent;
    int old_y;
    int follow = 0;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return;
    ent = g_ent_table[unit];
    old_y = ent[1];
    ent[3] = 0;

    if (!((int)old_y - (int)g_scroll_y <= 5
          || g_scroll_y == (uint32_t)(g_field_h - g_view_h)))
        follow = 1;
    else
        ++g_cursor_view_y;
    s_walk_cnt_y = 0;                       /* 0x12EE8：53B0B 初值 0 */

    for (int frame = 1; frame <= 6; frame++) {
        footstep_sfx(unit);
        idle_pump();
        ent[4] = (uint8_t)frame;
        anim_phase_tick();
        s_walk_cnt_y += follow;             /* 0x12F8E */
        walk_frame_compose(follow ? 1824 : 0, (int)FIELD_RENDER_OFF,
                           13, follow ? 9 : 8,
                           (int)g_scroll_x, (int)g_scroll_y);
    }

    ent[1] = (uint8_t)(old_y + 1);
    if (follow)
        ++g_scroll_y;
    ++g_cursor_y;
    ent[4] = 0;
    s_walk_pix_off = 0;
    s_walk_cnt_y = 0;
    tile_event_check((int)g_cursor_x, (int)g_cursor_y, 0);
}

/* 0x1300D ent_walk_left_one_tile(ent)：dir=1；x-scroll_x<2 且 scroll_x!=0
 * → 跟随（53AF5 每帧 -4、53AED=24 背景窗右移一 tile——host 背景以可见
 * 区直贴等价、步末 scroll_x-1）；否则 --cursor_view_x。窗恒 14x8
 * @map_x=scroll_x-1、带基 32880（=32904-24，多的一列在左；非跟随时
 * 该列在可见区外）。blit src 每帧 -4 = 镜头平滑左滚。步末 x-1、
 * --cursor_x、偏移清零、tile_event_check。 */
void ent_walk_left_one_tile(int unit)
{
    uint8_t *ent;
    int old_x;
    int follow_pix = 0;
    int follow_scroll = 0;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return;
    ent = g_ent_table[unit];
    old_x = ent[0];
    ent[3] = 1;

    if ((int)old_x - (int)g_scroll_x < 2 && g_scroll_x) {
        follow_pix = -4;
        follow_scroll = -1;
    } else {
        --g_cursor_view_x;
    }
    s_walk_cnt_x = 6;                       /* 0x13049：53B07 初值 6 */

    for (int frame = 1; frame <= 6; frame++) {
        footstep_sfx(unit);
        idle_pump();
        ent[4] = (uint8_t)frame;
        anim_phase_tick();
        s_walk_cnt_x += follow_scroll;      /* 0x130BA */
        walk_frame_compose(follow_pix, (int)FIELD_RENDER_OFF - 24,
                           14, 8, (int)g_scroll_x - 1, (int)g_scroll_y);
    }

    ent[0] = (uint8_t)(old_x - 1);
    if (follow_scroll)
        g_scroll_x = (uint32_t)((int)g_scroll_x + follow_scroll);
    --g_cursor_x;
    ent[4] = 0;
    s_walk_pix_off = 0;
    s_walk_cnt_x = 0;
    tile_event_check((int)g_cursor_x, (int)g_cursor_y, 0);
}

/* 0x13315 ent_walk_right_one_tile(ent)：dir=3；x-scroll_x>10 且
 * scroll_x != field_w-view_w → 跟随（53AF5 每帧 +4、窗 14x8 多的一列
 * 在右、步末 scroll_x+1）；否则 ++cursor_view_x、窗 13x8。blit src
 * 每帧 +4 = 镜头平滑右滚。步末 x+1、++cursor_x、尾 0x1314F +
 * tile_event_check。 */
void ent_walk_right_one_tile(int unit)
{
    uint8_t *ent;
    int old_x;
    int follow = 0;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return;
    ent = g_ent_table[unit];
    old_x = ent[0];
    ent[3] = 3;

    if (!((int)old_x - (int)g_scroll_x <= 10
          || g_scroll_x == (uint32_t)(g_field_w - g_view_w)))
        follow = 1;
    else
        ++g_cursor_view_x;
    s_walk_cnt_x = 0;                       /* 0x13352：53B07 初值 0 */

    for (int frame = 1; frame <= 6; frame++) {
        footstep_sfx(unit);
        idle_pump();
        ent[4] = (uint8_t)frame;
        anim_phase_tick();
        s_walk_cnt_x += follow;             /* 0x133F8 */
        if (follow)
            walk_frame_compose(4, (int)FIELD_RENDER_OFF,
                               14, 8, (int)g_scroll_x, (int)g_scroll_y);
        else
            walk_frame_compose(0, (int)FIELD_RENDER_OFF,
                               13, 8, (int)g_scroll_x, (int)g_scroll_y);
    }

    ent[0] = (uint8_t)(old_x + 1);
    if (follow)
        ++g_scroll_x;
    ++g_cursor_x;
    ent[4] = 0;
    s_walk_pix_off = 0;
    s_walk_cnt_x = 0;
    tile_event_check((int)g_cursor_x, (int)g_cursor_y, 0);
}

int ent_at_cursor(void)
{
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        if (rec[0] == g_cursor_x && rec[1] == g_cursor_y
            && !(rec[5] & 1))
            return i;
    }
    return -1;
}
void cursor_scroll_to(int x, int y)
{
    /* IDA 0x12CEA: move one grid step at a time through the directional
     * handlers, waiting one BIOS tick only while text/animation is active. */
    anim_tick_update(0);
    while ((int)g_cursor_x != x) {
        if (x >= (int)g_cursor_x) cursor_right();
        else                     cursor_left();
        if (g_text_busy && g_text_busy != 6) wait_bios_ticks(1);
        kbd_flush();
    }
    while ((int)g_cursor_y != y) {
        if (y >= (int)g_cursor_y) cursor_down();
        else                     cursor_up();
        if (g_text_busy && g_text_busy != 6) wait_bios_ticks(1);
        kbd_flush();
    }
}
void camera_focus_ent(int unit)
{
    cursor_scroll_to(g_ent_table[unit][0], g_ent_table[unit][1]);
}
/* 0x11B48/0x11B9B/0x11C59/0x11BFA cursor_*（2026-09-08 与原版逐指令
 * 对齐，撤销旧实现两处自造守卫）：非跟随支的 view 轴减一为【无条件】
 * （view_y/x 可到 -1，原版 0x11B81/0x11C92 无 if）；边界与滚动上限均
 * 为【等值】比较（0x11BAB/0x11BC4/0x11C0A/0x11C23 cmp+jz），非 >=/<。 */
void cursor_up(void)
{
    if (g_cursor_y == 0) return;
    if (g_cursor_view_y < 2 && g_scroll_y > 0) {
        --g_cursor_y; --g_scroll_y;
    } else {
        --g_cursor_y;
        --g_cursor_view_y;
        if (!g_text_busy) return;
    }
    anim_tick_update(0);
}

void cursor_down(void)
{
    if ((uint32_t)g_field_h - 1u == g_cursor_y) return;
    if (g_cursor_view_y > 5 && (uint32_t)g_field_h - 8u != g_scroll_y) {
        ++g_cursor_y; ++g_scroll_y;
    } else {
        ++g_cursor_y;
        ++g_cursor_view_y;
        if (!g_text_busy) return;
    }
    anim_tick_update(0);
}

void cursor_left(void)
{
    if (g_cursor_x == 0) return;
    if (g_cursor_view_x < 2 && g_scroll_x > 0) {
        --g_cursor_x; --g_scroll_x;
    } else {
        --g_cursor_x;
        --g_cursor_view_x;
        if (!g_text_busy) return;
    }
    anim_tick_update(0);
}

void cursor_right(void)
{
    if ((uint32_t)g_field_w - 1u == g_cursor_x) return;
    if (g_cursor_view_x > 10 && (uint32_t)g_field_w - 13u != g_scroll_x) {
        ++g_cursor_x; ++g_scroll_x;
    } else {
        ++g_cursor_x;
        ++g_cursor_view_x;
        if (!g_text_busy) return;
    }
    anim_tick_update(0);
}
void tile_lookup(int x, int y, uint8_t out[8])
{
    memset(out, 0, 8);
    if (!g_field_map || !g_shape_map || x < 0 || y < 0
        || x >= g_field_w || y >= g_field_h)
        return;
    const uint8_t *cell = (const uint8_t *)g_field_map
                        + 4 + 4 * (x + g_field_w * y);
    uint16_t tile = (uint16_t)(cell[0] | ((cell[1] & 3u) << 8));
    uint16_t flags = (uint16_t)(cell[2] & 0x1Fu);
    out[0] = (uint8_t)tile;
    out[1] = (uint8_t)(tile >> 8);
    out[2] = (uint8_t)flags;
    out[3] = (uint8_t)(flags >> 8);
    if ((size_t)tile * 4u + 4u > (size_t)g_shape_map_size)
        return;
    memcpy(out + 4, (const uint8_t *)g_shape_map + 4u * tile, 4);
}

void field_reset_candidates(void)
{
    if (!g_field_map || g_field_w <= 0 || g_field_h <= 0)
        return;
    uint8_t *cell = (uint8_t *)g_field_map + 4;
    size_t count = (size_t)g_field_w * (size_t)g_field_h;
    for (size_t i = 0; i < count; i++, cell += 4) {
        cell[1] &= 0x03u;
        cell[2] &= 0x1Fu;
        cell[3] = 0xFFu;
    }
}

/* 0x1B750 ent_recompute_derived（2026-09-08 逐指令全量重写，撤销缺装备
 * 循环的半实现——战场实体 ATK/DEF/HIT/EVA 因此全程缺少武器加成，玩家
 * HIT 18 vs 原版 108（武器 45 的 HIT+90），"普攻打不中"的直接根因）：
 *   ATK(+72) = (基值+55 + Σ装备武器[1..2]) × (rec[34] ? 1.15 : 1)
 *   DEF(+74) = (基值+57 + Σ装备武器[5..6]) × (rec[35] ? 1.15 : 1)
 *   HIT(+76) = 基值+62 (+15 若 rec[36]) + Σ武器[3..4]
 *   EVA(+78) = 基值+62 (+15 若 rec[36]) + Σ武器[7..8]
 * 1.15 = dbl_5018D（0x3FF2666666666666，非旧注 1.05），经 fmul+CHP 截断；
 * EVA 经 0x114FB ICF 共享尾写入（v11 累加器此前被丢弃——旧 fd2re 因此
 * 整个 EVA 字段未写）。消费方：standing_sprite_build 0x11003、
 * exp_levelup_check、道具/商店/转职链（原版 xrefs 12 处）。
 * 边界：原版 0x1B750 无检查，standing_sprite_build 以 g_ent_count（自增
 * 前 = 正在追加的记录下标，0x10FFD..0x11008）调用——守卫须放行 ==
 * g_ent_count 的追加中记录（此前 >= 拦截使战场敌人 ATK/DEF/HIT/EVA
 * 恒为基值/0，即"敌 DP=0 仍零伤害、我方 AP 极低"的根因），仅防越界
 * FD2_ENT_MAX。 */
void ent_recompute_derived(int unit)
{
    if (!g_ent_table || unit < 0
        || unit > g_ent_count || unit >= FD2_ENT_MAX)
        return;
    uint8_t *rec = g_ent_table[unit];
    int atk = *(int16_t *)(rec + 55);
    int dfn = *(int16_t *)(rec + 57);
    int hit_eva = *(int16_t *)(rec + 62);
    if (rec[36])
        hit_eva += 15;
    int hit = hit_eva, eva = hit_eva;

    for (int i = 0; i < 8; i++) {
        if (!(rec[10 + 2 * i] & 0x40u))
            continue;
        const uint8_t *w = weapon_table_entry(rec[11 + 2 * i]);
        if (!w)
            continue;
        atk += *(int16_t *)(w + 1);
        dfn += *(int16_t *)(w + 5);
        hit += *(int16_t *)(w + 3);
        eva += *(int16_t *)(w + 7);
    }
    if (rec[34])
        atk = (int)((double)atk * 1.15);   /* fmul dbl_5018D + CHP 截断 */
    if (rec[35])
        dfn = (int)((double)dfn * 1.15);
    *(uint16_t *)(rec + 72) = (uint16_t)atk;
    *(uint16_t *)(rec + 74) = (uint16_t)dfn;
    *(uint16_t *)(rec + 76) = (uint16_t)hit;
    *(uint16_t *)(rec + 78) = (uint16_t)eva;
}

int field_load_for_state(int state, int shape_id)
{
    /* 31/32 are not entries in the gameplay state tables; they are still
     * real FDFIELD/FDSHAP scenes used by st00_exit_prologue. */
    if (state < 0 || state >= FD2_STAGE_COUNT || shape_id < 0)
        return -1;
    /* A flashback starts from the most recently completed compositor frame.
     * Loading a map invalidates that frame until anim_tick_update submits the
     * new terrain and entity surface. */
    s_field_shadow_valid = 0;
    g_field_map = dat_load_block("FDFIELD.DAT", g_field_map, 3 * state);
    g_field_map_size = g_last_block_size;
    g_shape_tiles = dat_load_block("FDSHAP.DAT", g_shape_tiles, 2 * shape_id);
    g_shape_tiles_size = g_last_block_size;
    g_shape_map = dat_load_block("FDSHAP.DAT", g_shape_map, 2 * shape_id + 1);
    g_shape_map_size = g_last_block_size;
    if (!g_field_map || !g_shape_tiles || !g_shape_map)
        return -1;
    if (g_field_map_size < 4 || shape_tiles_validate() != 0)
        return -1;
    const uint8_t *hdr = (const uint8_t *)g_field_map;
    g_field_w = (int16_t)(hdr[0] | (hdr[1] << 8));
    g_field_h = (int16_t)(hdr[2] | (hdr[3] << 8));
    if (g_field_w <= 0 || g_field_h <= 0
        || 4u + 4u * (size_t)g_field_w * (size_t)g_field_h
           > (size_t)g_field_map_size)
        return -1;
    g_view_w = g_field_w > 13 ? 13 : g_field_w;
    g_view_h = g_field_h > 8 ? 8 : g_field_h;
    field_reset_candidates();
    return (g_field_w > 0 && g_field_h > 0) ? 0 : -1;
}
void sub_17AED(int unit)
{
    /* 0x17AED 单位状态页全解（2026-09-07，duel.c unit_status_page）：
     * 面板布局 + 法术列表子面板 + 12 帧退场——撤销"等一键"最小实现。
     * 原版无任何守卫直调（战斗 0x12C0D/0x12C5A、营地酒馆菜单0 选人
     * 0x29656 同路）。场景上下文 g_ent_table=g_roster_table 而
     * g_ent_count 保持 0（state_run_scene 清零，scene_page_dispatch
     * 只设表不同步计数），旧守卫 unit>=g_ent_count 在营地/整备页恒
     * 拦截 → 选人后状态页不显示；仅保留宿主 NULL/负索引护栏。 */
    if (!g_ent_table || unit < 0)
        return;
    unit_status_page(unit);
}
int sub_1F183(int unit)
{
    /* IDA 0x1F183: terrain-immune units are icon 28, class 19, or AI
     * subtypes 4/5. */
    if (unit < 0 || unit >= g_ent_count)
        return 0;
    const uint8_t *rec = g_ent_table[unit];
    if (rec[7] == 28)
        return 0;
    return rec[32] == 19 || rec[31] == 4 || rec[31] == 5;
}
/* 0x1145A（2026-09-05 反汇编定案，roster_spawn_ent@0x112A5 收尾调用）：
 * 装备加成重算。遍历 8 个道具槽（数量字段 bit6=已装备），按武器表累加：
 *   ent[+72](u16 ATK) = ent[+55] + Σ weapon[1..2]
 *   ent[+74](u16 DEF) = ent[+57] + Σ weapon[5..6]
 *   ent[+76](u16 HIT) = ent[+62] + Σ weapon[3..4]
 *   ent[+78](u16 EVA) = ent[+62] + Σ weapon[7..8]（i16 可负）
 * 原版操作 g_roster_table（80B 记录）、无界检查、不跳过无效道具 id；
 * fd2re 防御 roster_idx 范围与 weapon_table_entry 越界（回退见 tables.c）。 */
void ent_stats_apply_equipment(int roster_idx)
{
    uint8_t *r;
    const uint8_t *w;
    int atk, def, hit, eva;

    /* 0x1145A 原版无边界检查；roster_spawn_ent 以 g_roster_count（自增前
     * = 正在构建的下一条记录下标）调用——守卫须放行 == g_roster_count
     * 的追加中记录（此前 >= 拦截使 roster 的 +72..78 派生值从未计算，
     * 面板显示裸基值即此根因），仅防越界 FD2_ROSTER_MAX。 */
    if (!g_roster_table || roster_idx < 0
        || roster_idx > g_roster_count || roster_idx >= FD2_ROSTER_MAX)
        return;
    r = g_roster_table[roster_idx];
    atk = *(uint16_t *)(r + 55);
    def = *(uint16_t *)(r + 57);
    hit = *(int16_t *)(r + 62);
    eva = *(int16_t *)(r + 62);
    for (int i = 0; i < 8; i++) {
        if (!(r[10 + 2 * i] & 0x40))
            continue;
        w = weapon_table_entry(r[11 + 2 * i]);
        if (!w)
            continue;
        atk += *(int16_t *)(w + 1);
        hit += *(int16_t *)(w + 3);
        def += *(int16_t *)(w + 5);
        eva += *(int16_t *)(w + 7);
    }
    *(uint16_t *)(r + 72) = (uint16_t)atk;
    *(uint16_t *)(r + 74) = (uint16_t)def;
    *(uint16_t *)(r + 76) = (uint16_t)hit;
    *(uint16_t *)(r + 78) = (uint16_t)eva;
}

/* ---- 库存四件套（2026-09-04 逆向，docs §15.7；槽位语义见 entities.h） ---- */

/* 0x1B8A6：非空槽计数（ent[+10+2i] signed >= 0）。 */
int inventory_used_count(int unit)
{
    int n = 0;
    for (int i = 0; i < 8; i++)
        if ((int8_t)g_ent_table[unit][10 + 2 * i] >= 0)
            n++;
    return n;
}

/* 0x1B722：槽内道具 id（ent[+11+2*slot]）。 */
int inventory_get_item(int unit, int slot)
{
    return g_ent_table[unit][11 + 2 * slot];
}

/* 0x1B8E7：移除并压缩——slot 后的 (id,数量) 对前移，末槽数量标 0x80（空）。 */
/* 0x241xx 族（0x24B14 语义）：inventory_find_item_slot(unit, item)
 * ——单位 8 道具槽中找 item id，返回槽号或 -1。 */
int inventory_find_item_slot(int unit, int item)
{
    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return -1;
    const uint8_t *rec = g_ent_table[unit];
    for (int s = 0; s < 8; s++)
        if (rec[11 + 2 * s] == (uint8_t)item
            && (int8_t)rec[10 + 2 * s] >= 0)
            return s;
    return -1;
}

void inventory_remove_item(int unit, int slot)
{
    uint8_t *rec = g_ent_table[unit];
    memmove(rec + 10 + 2 * slot, rec + 12 + 2 * slot,
            (size_t)(2 * (7 - slot)));
    rec[24] = 0x80;
}

/* 0x1BB8C：入包——首个空槽（数量<0）置数量 0 + 道具 id；满包返回 -1。 */
int ent_inventory_add(int unit, int item)
{
    for (int i = 0; i < 8; i++) {
        uint8_t *qty = &g_ent_table[unit][10 + 2 * i];
        if ((int8_t)*qty < 0) {
            *qty = 0;
            qty[1] = (uint8_t)item;
            return 1;
        }
    }
    return -1;
}

/* 0x1B83D：已装备槽查找——数量字段(+10+2i) bit6=装备中标记；
 * want_high=0 → 道具 id<0x80（普通武器），1 → >=0x80。 */
int equipped_slot_find(int unit, int want_high)
{
    for (int i = 0; i < 8; i++) {
        const uint8_t *qty = &g_ent_table[unit][10 + 2 * i];
        if (*qty & 0x40 && ((qty[1] >= 0x80) == (want_high != 0)))
            return i;
    }
    return -1;
}
uint16_t fd2_rand(void)
{
    /* IDA 0x4EBE3: xor eax,eax; mov ax,state; ROL16(state-28652, 3).
     * The zeroed EAX is observable by the signed idiv callers: the 16-bit
     * state is returned as a non-negative 32-bit value. */
    static uint16_t state;
    state = (uint16_t)(state - 28652u);
    state = (uint16_t)((state << 3) | (state >> 13));
    return state;
}
