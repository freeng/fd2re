/* duel.c — 决斗/特效/演出引擎骨架（IDA 0x2B000-0x32000）
 *
 * 证据与流程详见 docs/architecture.md §13（2026-09-04 全量逆向）。
 * 要点：
 *   - fx_scene_play 按 effect_id 分发：>=32→fx_scene_special（大招/处刑，
 *     播完后执行真实效果 32=全体800伤/33=清状态/34=三连/35=三段变身）；
 *     ==24||>27→fx_scene_multi_hit（全屏多段魔法，总伤=系数×ATK/10）；
 *     其余本体：g_fx_controllers[effect_id] 十项相位协议驱动，
 *     受击时 HP 由旧值按 g_fx_hit_counts[effect_id] 级数降至新值。
 *   - 资源：BG.DAT/TAI.DAT（duel_bg_pick 选变体；g_state_duel_bg[g_state]
 *     或地块导出）；FIGANI 3*icon 本体 / +1 帧表动画 / +2 姿态包；
 *     FDOTHER 块 'R'+effect_id（音效包 g_fx_sfx_pkg@0x54153）。
 *   - 公共尾声：FDSHAP 2*g_battle_ctx[0] 重载 + 淡黑 + memset VRAM +
 *     anim_tick_update(1) + fade_in 恢复战场。
 *   - unit_pose_step 状态：g_pose_frame/g_pose_step（0x54131/0x54130）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fd2.h"
#include "duel.h"
#include "battle.h"
#include "magic.h"
#include "resource.h"
#include "text.h"
#include "video.h"
#include "audio.h"
#include "entities.h"
#include "input.h"
#include "timer.h"
#include "text.h"
#include "scene.h"
#include "host.h"

/* 前向声明（ending/面板链在文件前部使用，定义于 unit_bars 区） */
static void raw_frame_stamp(const uint8_t *frame, uint8_t *dst, int pitch);
static const uint8_t *blk5_frame(int frame);
static uint32_t fx_u32(const uint8_t *p);

/* IDA 0x526BC: ten hit-frame counts copied as 4+4+2 bytes at 0x2FFC4.
 * 0x526C6 is a separate overlay table (its first byte is 0x12), despite
 * appearing contiguous in a raw byte dump. */
const uint8_t g_fx_hit_counts[10] = { 7, 8, 6, 13, 6, 6, 5, 5, 16, 20 };

uint8_t g_pose_frame;   /* 0x54131 姿态帧号 */
uint8_t g_pose_step;    /* 0x54130 帧内步数 */

/* 0x4DEEC lut_row_copy(lut, n, dst)：dst[i] = lut[src[i]]——
 * 0x219AD 圆形光带的行重映射（光效染色）。 */
static void lut_row_copy(const uint8_t *lut, int n, uint8_t *dst)
{
    for (int i = 0; i < n; i++)
        dst[i] = lut[dst[i]];
}

/* 0x219AD column_light_blit(lut, cx, cy, r)：456 层中带内逐行
 * sqrt 圆半宽重映射——球形光晕（行 y ∈ (cy-r, cy+r)，列
 * [cx-w, cx+w) 过 lut，312 视口裁剪）。 */
static void column_light_blit(const uint8_t *lut, int cx, int cy, int r)
{
    uint8_t *layer = battle_shadow_layer();

    if (!layer)
        return;
    for (int y = cy - r + 1; y < cy + r; y++) {
        if (y < 0 || y >= 192)
            continue;
        int dy = abs(cy - y);
        if (dy >= r)
            continue;
        int w = (int)sqrt((double)(r * r - dy * dy));
        int x0 = cx - w, x1 = cx + w;
        if (x1 <= 0 || x0 >= 312)
            continue;
        if (x0 < 0) x0 = 0;
        if (x1 > 312) x1 = 312;
        lut_row_copy(lut, x1 - x0, layer + 32904 + 456L * y + x0);
    }
}

/* 0x24618 cast_beam_fall（2026-09-06 真管线；2026-09-13 真 ABI 定案
 * 4 参格坐标）：sfx(11) + blk3 帧 9..1 逐帧（delay 5）+ 光柱半径
 * half=arg_8 起每帧 +=arg_C（0x246FB add esi 实证；旧版误作 y 位移
 * 且固定半径 16）+ delay(500) + palette 0..62 步进 2 delay(4)。
 * cx=tile_x*24+12、cy=tile_y*24+16（0x24632/0x2463E，帧内固定）。
 * 绘制 = 456 层中带帧贴 + blk3 LUT 圆形光。 */
void cast_beam_fall(int tile_x, int tile_y, int half0, int dhalf)
{
    uint8_t *layer = battle_shadow_layer();
    int cx = tile_x * 24 + 12;             /* 0x24632 imul 18h + 0Ch */
    int cy = tile_y * 24 + 16;             /* 0x2463E imul 18h + 10h */
    int half = half0;                      /* esi = arg_8 */

    sfx_play(g_magic_sfx_pkg, 11, 1);
    for (int i = 9; i > 0; i--) {
        anim_tick_update(0);
        if (layer) {
            blit_frame_flat(g_pkg_fdother_3, i,
                            layer + 32904 + 456L * cy + cx, 456, -1);
            const uint8_t *pkg = (const uint8_t *)g_pkg_fdother_3;
            if (pkg)
                column_light_blit(pkg + fx_u32(pkg + 6), cx, cy, half);
        }
        battle_shadow_blit();
        half += dhalf;                     /* 0x246FB add esi, arg_C */
        delay_ms(5);
    }
    delay_ms(500);
    for (int j = 0; j < 64; j += 2) {
        palette_add_range(0, 255, j);
        delay_ms(4);
    }
}

/* 0x2189A cast_beam_rise(cx, cy, r0, dr)——半径语义定案（2026-09-08
 * 逐指令核对 0x2185F/0x21A9E/0x151A2/0x24978 三族调用点）：单位中心
 * 圆形 LUT 光晕，帧 0..9、半径 r0 起每帧 +dr（spell11/12=(15,10)/
 * (30,16)，st22 三连=(15,10)×2+(30,16)，plan_a=(80,-4) 收缩）。
 * 原版第 1 参为 unit（g_ent_table+unit*80 取 x/y 推 cx/cy）——fd2re
 * 宿主约定由调用方传像素中心。每帧 memmove 复原背景 + column_light_
 * blit(lut_i) + ents_render_all；宿主层由 anim_tick_update(0) 预置
 * 场景+实体后单遍染色（ents 叠于光上，同 cast_beam_fall 注记）。无
 * 帧内 delay。 */
void cast_beam_rise(int cx, int cy, int r0, int dr)
{
    uint8_t *layer = battle_shadow_layer();
    const uint8_t *pkg = (const uint8_t *)g_pkg_fdother_3;
    int r = r0;

    for (int i = 0; i < 10; i++) {
        anim_tick_update(0);
        if (layer && pkg)
            column_light_blit(pkg + fx_u32(pkg + 6 + 4 * i), cx, cy, r);
        battle_shadow_blit();
        r += dr;
    }
    anim_tick_update(0);
}

/* 0x22046 尾段：竖直光带矩形重映射——行 [0, cy)、列 [cx-half,
 * cx+half) 裁剪 [0,312)，half = (int)(r*1.6)（dbl_50208）。 */
static void column_band_lut(const uint8_t *lut, int cx, int cy, int half)
{
    uint8_t *layer = battle_shadow_layer();
    int x0, x1;

    if (!layer)
        return;
    x0 = cx - half;
    x1 = cx + half;
    if (x0 < 0)
        x0 = 0;
    if (x1 > 312)
        x1 = 312;
    for (int y = 0; y < cy && y < 192; y++)
        if (x1 > x0)
            lut_row_copy(lut, x1 - x0, layer + 32904 + 456L * y + x0);
}

/* 0x21EB1 cast_charge_anim(r0, dr)（2026-09-08 逐指令定案）：蓄力
 * 两阶段，光效中心 = 光标 (cursor_view_x*24+12, cursor_view_y*24+16)。
 * 阶段1 帧 9..1、半径 r0 起每帧 +dr 扩张（delay5/帧）→ delay200；
 * esi 回退一格（=帧1 所用半径）后阶段2 帧 3..9 定半径重放（delay5/
 * 帧）→ anim_tick_update(0) + delay200。每帧 column_effect_blit =
 * 圆形 LUT 光 + 1.6r 半宽光带（宿主单遍染色近似，同上注记）。
 * 表项实参：13:(1,2) 14:(2,4) 15:(8,4) 16/24:(6,6)。 */
void cast_charge_anim(int r0, int dr)
{
    uint8_t *layer = battle_shadow_layer();
    const uint8_t *pkg = (const uint8_t *)g_pkg_fdother_3;
    int cx = 24 * (int)g_cursor_view_x + 12;
    int cy = 24 * (int)g_cursor_view_y + 16;
    int r = r0;

    for (int i = 9; i > 0; i--) {
        anim_tick_update(0);
        if (layer && pkg) {
            const uint8_t *lut = pkg + fx_u32(pkg + 6 + 4 * i);
            column_light_blit(lut, cx, cy, r);
            column_band_lut(lut, cx, cy, (int)(r * 1.6));
        }
        battle_shadow_blit();
        r += dr;
        delay_ms(5);
    }
    delay_ms(200);
    r -= dr;
    for (int i = 3; i < 10; i++) {
        anim_tick_update(0);
        if (layer && pkg) {
            const uint8_t *lut = pkg + fx_u32(pkg + 6 + 4 * i);
            column_light_blit(lut, cx, cy, r);
            column_band_lut(lut, cx, cy, (int)(r * 1.6));
        }
        battle_shadow_blit();
        delay_ms(5);
    }
    anim_tick_update(0);
    delay_ms(200);
}

/* 0x168B6 panel9_grid(dst, pitch, x, y, cols, rows)（2026-09-06 定案）：
 * 角色卡面板 = blk5 RAW 帧边框拼装：帧1 左上/2 上右/3 左下/4 右下/
 * 5..8 内框角、9..12 边轨（9=上内/10=左轨/11=右轨/12=下内）、
 * 13=16x16 单元格（cols×rows 全铺）、14..17 外框余段。
 * 注：f10/f11 的目的偏移取 v24 计算（v11+v29+(j+1)*16*pitch）——
 * 反编译显示其用途的唯一自洽读法（挂号注记）。 */
static void panel9_grid(uint8_t *dst, int pitch, int x, int y, int cols, int rows)
{
    int v27 = rows - 2;
    int v28 = 16 * pitch;
    int v29 = 3 * pitch;
    uint8_t *v10 = dst + (size_t)pitch * y;
    uint8_t *v11 = v10 + x;
    uint8_t *v12 = v11 + 3 + 16 * cols;

    raw_frame_stamp(blk5_frame(1), v10, pitch);
    raw_frame_stamp(blk5_frame(2), v12, pitch);
    raw_frame_stamp(blk5_frame(3), v11 + v29 + (size_t)rows * v28, pitch);
    raw_frame_stamp(blk5_frame(4), v12 + v29 + (size_t)rows * v28, pitch);
    raw_frame_stamp(blk5_frame(5), v11 + 3, pitch);
    uint8_t *v15 = v11 + 19 + 16 * (cols - 2);
    raw_frame_stamp(blk5_frame(6), v15, pitch);
    raw_frame_stamp(blk5_frame(7), v11 + 3 * pitch + (size_t)rows * v28 + 3, pitch);
    raw_frame_stamp(blk5_frame(8), v15 + v29 + (size_t)rows * v28, pitch);
    raw_frame_stamp(blk5_frame(14), v11 + 3 * pitch, pitch);
    uint8_t *v18 = v11 + 3 * pitch + 16 * (cols - 2) + 35;
    raw_frame_stamp(blk5_frame(15), v18, pitch);
    size_t v19 = (size_t)(rows - 1) * v28;
    raw_frame_stamp(blk5_frame(16), v11 + 3 * pitch + v19, pitch);
    raw_frame_stamp(blk5_frame(17), v18 + v19, pitch);
    for (int i = 0; i < cols - 2; i++) {
        raw_frame_stamp(blk5_frame(9), v11 + 19 + 16 * i, pitch);
        raw_frame_stamp(blk5_frame(12),
                        v11 + 19 + 16 * i + v29 + (size_t)rows * v28, pitch);
    }
    for (int j = 0; j < v27; j++) {
        uint8_t *v24 = v11 + v29 + (size_t)(j + 1) * v28;
        raw_frame_stamp(blk5_frame(10), v24, pitch);
        raw_frame_stamp(blk5_frame(11), v24 + 16 * cols + 3, pitch);
    }
    for (int k = 0; k < rows; k++)
        for (int m = 0; m < cols; m++)
            raw_frame_stamp(blk5_frame(13),
                            v11 + v29 + 3 + 16 * m + (size_t)k * v28, pitch);
}

/* 0x31DE2 ending_anim_part2（2026-09-06 全解忠实重写，替代
 * ani_play+fade 猜测实现）：
 * 1) battle_stage_load(30) + 演职员滚动——0x36B00 大缓冲、文本 44
 *    （g_pkg_text_evt）@+76848、500 帧逐行上滚（每帧一行 320B），前
 *    200 帧每 5 帧调光板 40→0、300 帧后回升 0→40；
 * 2) TAI blk3 背景 + FDOTHER blk56 底图 + music_play(4)；
 * 3) roster 倒序逐角色（j==0↔1 重映射）：FIGANI 3i+1 帧表 + 3i 姿态；
 *    duel_intro_slide(j, with_target=1) → 20 帧姿态步进 → 帧表动画
 *    （帧[6]=延时 tick）；
 * 4) DATO.DAT[icon] 角色卡：panel9_grid(5,7,5,5) 后 v21 = j?220:440
 *    帧展示——DATO 肖像（RAW 帧，twinkle 40..71 计数 <2 时切帧 12）
 *    + 姿态步进 + 文本对（evt10/名字/evt11/职业 rec[+32]+150）+
 *    结语（n>=220 显 evt 45 否则 rec[+8]+12）@+32008。 */
void ending_anim_part2(void)
{
    uint8_t *vram = vram_base();
    int level = 40;

    battle_stage_load(30);
    {
        uint8_t *big = malloc(0x36B00);
        if (!big)
            return;
        memset(big, 0, 0x36B00);
        text_render_box(1, 19, 74, 205, 320, big + 76848,
                        44, g_pkg_text_evt);
        for (int i = 0; i < 500; i++) {
            palette_apply_range(0, 255, level);
            blit_rows(vram, 320, big + 320L * i, 320, 320, 200);
            if (i < 200 && level && !(i % 5))
                level--;
            if (i > 300 && !(i % 5))
                level++;
            wait_bios_ticks(1);
        }
        free(big);
    }
    {
        uint8_t *work = malloc(0x1F400);
        uint8_t *frame = malloc(64000);
        uint8_t *panel = malloc(64000);
        void *tai = dat_load_block("TAI.DAT", NULL, 3);
        void *bk = dat_load_block("FDOTHER.DAT", NULL, 56);

        if (!work || !frame || !panel || !tai || !bk) {
            free(work); free(frame); free(panel); free(tai); free(bk);
            return;
        }
        rle_decode_frame(bk, 0, 0, frame, 320, -1);
        free(bk);
        music_play(4, 0);
        for (int j = g_roster_count - 1; j >= 0; j--) {
            int idx = j == 0 ? 1 : (j == 1 ? 0 : j);
            uint8_t *rec = g_ent_table[idx];
            int icon = rec[7];
            uint8_t *ftab = dat_load_block("FIGANI.DAT", NULL, 3 * icon + 1);
            uint8_t *pose = dat_load_block("FIGANI.DAT", NULL, 3 * icon);

            duel_intro_slide(j, 1, pose, NULL, work, frame, tai);
            unit_pose_step(pose, 0, work, 320);
            for (int k = 0; k < 20; k++) {
                blit_rows(work, 320, frame, 320, 320, 200);
                unit_pose_step(pose, -1, work, 320);
                blit_rows(vram, 320, work, 320, 320, 200);
                wait_bios_ticks(1);
            }
            for (int m = 0; m < ftab[2]; m++) {
                const uint8_t *fr = ftab + fx_u32(ftab + 8 + 4 * m);

                blit_rows(work, 320, frame, 320, 320, 200);
                blit_frame_flat(ftab, m, work, 320, -1);
                blit_rows(vram, 320, work, 320, 320, 200);
                wait_bios_ticks(fr[6]);   /* 帧[6] = 延时 tick */
            }
            memmove(panel, frame, 64000);
            g_fdtxt_ptr_table = dat_load_block("DATO.DAT",
                                               g_fdtxt_ptr_table, icon);
            panel9_grid(panel, 320, 5, 7, 5, 5);
            int v21 = j ? 220 : 440;
            int twinkle = 0;
            for (int n = 0; n < v21; n++) {
                blit_rows(work, 320, panel, 320, 320, 200);
                if (twinkle)
                    twinkle--;
                else
                    twinkle = (fd2_rand() & 0x1F) + 40;
                int sel = twinkle >= 2 ? 0 : 12;
                raw_frame_stamp((const uint8_t *)g_fdtxt_ptr_table
                    + fx_u32((const uint8_t *)g_fdtxt_ptr_table + 4 * sel),
                    work + 3208, 320);
                unit_pose_step(pose, -1, work, 320);
                text_render_box(1, 19, 74, 205, 320, work + 5865,
                                10, g_pkg_text_evt);
                text_render_box(1, 19, 74, 205, 320, work + 5915,
                                rec[8] + 1, g_pkg_fdtxt0);
                text_render_box(1, 19, 74, 205, 320, work + 12265,
                                11, g_pkg_text_evt);
                text_render_box(1, 19, 74, 205, 320, work + 12315,
                                rec[32] + 150, g_pkg_fdtxt0);
                text_render_box(1, 19, 74, 205, 320, work + 32008,
                                n >= 220 ? 45 : rec[8] + 12,
                                g_pkg_text_evt);
                blit_rows(vram, 320, work, 320, 320, 200);
                wait_bios_ticks(1);
            }
            free(ftab);
            free(pose);
        }
        free(work);
        free(frame);
        free(panel);
        free(tai);
    }
}

/* 0x31BDF ending_text_show(text_id, slot)（2026-09-06 定案重写）：
 * dialog_backdrop_load(slot) → 文本（g_pkg_text_evt）@38164 →
 * wait_key_anim(0) → 对话框拆卸（sub_26996：5 行恢复 + 恢复屏 +
 * free 3x64000 菜单缓冲——宿主侧并入 wait_key_anim 生命周期）。 */
void ending_text_show(int text_id, int slot)
{
    kbd_flush();
    dialog_backdrop_load(slot);
    text_render_box(1, 19, 74, 205, 320, vram_base() + 38164,
                    text_id, g_pkg_text_evt);
    wait_key_anim(0);
    kbd_flush();
}

/* 控制器函数表 @0x524C6（§13.4，2026-09-04 全展开）：
 * [0]=fx_ctrl_0_particles(0x2B996, 7 槽线性粒子，唯一可反编译)
 * [1]=fx_ctrl_1_particles8(0x2BB33) [2]=fx_ctrl_2_statemach(0x2BD6C)
 * [3]=fx_ctrl_3_particles12(0x2BFD9) [4]=fx_ctrl_4_particles10(0x2C217)
 * [5]=fx_ctrl_5_particles10(0x2C441) [6]=fx_ctrl_6_sparks(0x2C67D,
 * 0x3C8EFA2D=f32 角步长 0.0174654，非 LCG) —— [1..6] 为 WATCOM ICF 折叠区
 * （共享尾声 0x2C93D
 * 嵌于 ctrl_6 段内，Hex-Rays 不可反编译；函数边界已按表指针还原）；
 * [7]=0x2CAFC [8]=0x2CCF4 [9]=0x2CE1A（横滑入/出特写）。
 * 决斗引擎资源：BG.DAT（背景）/FIGANI.DAT（人物动画）/TAI.DAT（姿态），
 * 字串表 @0x5248E-0x524A0；TITLE.DAT 无任何引用（负结论）。
 *
 * 公共尾声（各播放器共享，待实现为 restore_battlefield）：
 * FDSHAP 块 2*g_battle_ctx[0] → g_shadow_buf、sprites_reload_fd2tmp(0x2E95B)、
 * wait 6-8 tick、palette_fade_black、memset(0xA0000,0,64000)、
 * anim_tick_update(1)、fade_in。
 * duel_scene_load@0x2E2B0 的实现归 battle.c（battle.h 声明），此处不重复。 */

/* ---- fx 控制器 0..6 定案实现（§13.9，2026-09-05 capstone 逐指令解码）----
 * 折叠区 0x2B996..0x2CAFC 全解（tools/fx_ctrl_dump.py；字节镜像
 * reverse/fx_fold.bin + reverse/fx_tables.bin）。共享尾声 0x2C93D =
 * `xor eax,eax; add esp,0x44; pop...; ret`（返 0 出口）；ctrl_5 尾部
 * jmp 0x2C439 直借 ctrl_4 尾声（ICF 折叠实证）。统一协议见 duel.h：
 *   int ctrl(ent, pkg, dst, pitch, mode)
 * 敌方（rec[+6]==0）差异逐控制器注明；音效全部走 g_fx_sfx_pkg
 * （A 通道 0x25A96 / B 通道 0x25B45）。宿主防护：pkg/dst NULL 跳过
 * blit、g_fx_sfx_pkg NULL 跳过音效（原版两者恒由 fx_scene_play 装载）。 */
uint8_t *g_fx_sfx_pkg;                        /* 0x54153 */
/* g_standing_sprites 定义已移至 main.c（roster_grid_draw 共享） */

/* 0x5269C/0x526AC 震动位移表（dx={6,4,2,0}、dy={-3,-2,-1,0}，受击帧
 * sign=1-fd2_rand()%3 逐级衰减）；0x526C6/0x526CF 覆盖包 FDOTHER 块号
 * （原版 16+1 字节本地拷贝越界读相邻串，效果 14..16 恰为 "R..."——
 * 按原字节嵌入 17 项，≥17 宿主钳 0）；0x51AAD/0x51AD1/0x51AF5 受击
 * 闪色 R/G/B（duel_frame_compose DAC 0 号色闪烁）。 */
static const int32_t s_fx_shake_dx[4] = { 6, 4, 2, 0 };
static const int32_t s_fx_shake_dy[4] = { -3, -2, -1, 0 };
static const uint8_t s_fx_overlay_r[17] = {
    18, 19, 26, 39, 22, 24, 32, 37, 28, 20, 21, 27, 43, 23, 25, 33, 38 };
static const uint8_t s_fx_overlay_l[17] = {
    20, 21, 27, 43, 23, 25, 33, 38, 30, 44,
    'R', 'R', 'S', 'T', 'U', 'V', 'W' };
static const uint8_t s_fx_flash_rgb[3][36] = {
    { 63,63,63,63,43,43,43,43,63,35,46,46,46,63,63,63,63,50,50,50,
      63,63,35,30, 0,63,10,35,63,63,63,63,43,63,63,43 },
    {  0, 0, 0, 0,50,50,50,50,63,16,40,40,40,61,61,40,40,50,50,50,
      40,40, 0,42, 0,61,31,25,63,63,63,63,50,63, 0,50 },
    {  0, 0, 0, 0,60,60,60,60,63, 8,30,30,30,46,46,30,30,50,50,50,
      30,30, 0,35, 0,46, 0, 0,63,63,63,63,60,63, 0,60 },
};

static uint32_t fx_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fx_ctrl_is_enemy(int ent)
{
    return ent >= 0 && ent < g_ent_count && g_ent_table[ent][6] == 0;
}

static void fx_blit(const uint8_t *pkg, int frame, uint8_t *dst, int pitch, int off)
{
    if (!pkg || !dst)
        return;
    blit_frame_flat(pkg, frame, dst + off, pitch, -1);
}

static void fx_sfx_a(int idx, int loop)
{
    if (g_fx_sfx_pkg)
        sfx_play(g_fx_sfx_pkg, idx, loop);
}

static void fx_sfx_b(int idx, int loop)
{
    if (g_fx_sfx_pkg)
        sfx_play_b(g_fx_sfx_pkg, idx, loop);
}

/* 0x2BF83 fx_cell_step(&state,&sub,pkg,dst,pitch)：blit 当前帧后按帧[6]
 * 重复数进位（(w,h) 帧族协议，帧 = pkg + u32(pkg+8+4*state)）。 */
static void fx_cell_step(uint8_t *state, uint8_t *sub,
                         const uint8_t *pkg, uint8_t *dst, int pitch)
{
    const uint8_t *frame;

    fx_blit(pkg, *state, dst, pitch, 0);
    if (!pkg)
        return;
    frame = pkg + (uint32_t)(pkg[8 + 4 * *state]
             | ((uint32_t)pkg[9 + 4 * *state] << 8)
             | ((uint32_t)pkg[10 + 4 * *state] << 16)
             | ((uint32_t)pkg[11 + 4 * *state] << 24));
    if (++*sub == frame[6]) {
        *sub = 0;
        ++*state;
    }
}

/* 0x2B996 ctrl_0：7 槽线性粒子。列 @0x524F5、行速 @0x52511、参数
 * @0x524EE（=1 槽 mode4 静态 / =0 槽 mode5 动画）；pos = dst + col +
 * rowv*pitch；state dword_53F76[8]。m3：state[i]=-2i 返 28（受击帧数）；
 * m4：state==3 槽 sfx_a(1)，param==1 槽 blit 不步进；m5：param==0 槽
 * blit + 全槽 state++（==9 置完成返 1）；其余返 0。敌方全列 +0x94。 */
static int32_t s_c0_state[8];                            /* 0x53F76 */
static const int32_t s_c0_col[7]  = { 40, 70, 120, 80, 50, 100, 70 };
static const int32_t s_c0_rowv[7] = { 0, -10, -20, 0, -15, -5, 0 };
static const uint8_t s_c0_par[7]  = { 0, 0, 1, 0, 1, 0, 0 };

int fx_ctrl_0_particles(int ent, const uint8_t *pkg, uint8_t *dst,
                        int pitch, int mode)
{
    int32_t col[7];
    int i, done = 0;

    for (i = 0; i < 7; i++)
        col[i] = s_c0_col[i] + (fx_ctrl_is_enemy(ent) ? 0x94 : 0);
    if (mode == 3) {
        for (i = 0; i < 8; i++)
            s_c0_state[i] = -2 * i;
        return 28;
    }
    if (mode == 4) {
        for (i = 0; i < 7; i++) {
            if (s_c0_state[i] == 3)
                fx_sfx_a(1, 1);
            if (s_c0_state[i] >= 0 && s_c0_state[i] < 0x10
                && s_c0_par[i] == 1)
                fx_blit(pkg, s_c0_state[i], dst, pitch,
                        col[i] + s_c0_rowv[i] * pitch);
        }
        return 0;
    }
    if (mode == 5) {
        for (i = 0; i < 7; i++) {
            if (s_c0_state[i] >= 0 && s_c0_state[i] < 0x10
                && s_c0_par[i] == 0)
                fx_blit(pkg, s_c0_state[i], dst, pitch,
                        col[i] + s_c0_rowv[i] * pitch);
            if (++s_c0_state[i] == 9)
                done = 1;
        }
        return done;
    }
    return 0;
}

/* 0x2BB33 ctrl_1：8 槽两行镜像（i 与 i+4 成对，帧 +0xF=第二行）。
 * 列 @0x5252D、行速 @0x5254D；pos = dst + col + rowv*pitch + 0x50；
 * state 0x53F92[8]。m3：state=-2i 返 31；m4：槽 0..3 帧 state 几何 i、
 * 槽 4..7 帧 state+0xF 几何 (i+4)%8；m5 两段镜像互换（0..3 几何 i+4
 * 帧 +0xF；4..7 几何 i 帧 state），随后全槽 ++（==9 完成、==5
 * sfx_a(1)）。敌方全列 +0x94。 */
static int32_t s_c1_state[8];                            /* 0x53F92 */
static const int32_t s_c1_col[8]  = { -59, -39, 0, 39, 55, 39, 0, -39 };
static const int32_t s_c1_rowv[8] = { -10, -24, -30, -24, -10, 4, 10, 4 };

int fx_ctrl_1_particles8(int ent, const uint8_t *pkg, uint8_t *dst,
                         int pitch, int mode)
{
    int32_t col[8];
    int i, j, done = 0;

    for (i = 0; i < 8; i++)
        col[i] = s_c1_col[i] + (fx_ctrl_is_enemy(ent) ? 0x94 : 0);
    if (mode == 3) {
        for (i = 0; i < 8; i++)
            s_c1_state[i] = -2 * i;
        return 31;
    }
    if (mode == 4) {
        for (i = 0; i < 4; i++)
            if (s_c1_state[i] >= 0 && s_c1_state[i] < 0xF)
                fx_blit(pkg, s_c1_state[i], dst, pitch,
                        col[i] + s_c1_rowv[i] * pitch + 0x50);
        for (; i < 8; i++)
            if (s_c1_state[i] >= 0 && s_c1_state[i] < 0xF) {
                j = (i + 4) % 8;
                fx_blit(pkg, s_c1_state[i] + 0xF, dst, pitch,
                        col[j] + s_c1_rowv[j] * pitch + 0x50);
            }
        return 0;
    }
    if (mode == 5) {
        for (i = 0; i < 4; i++) {
            j = i + 4;
            if (s_c1_state[i] >= 0 && s_c1_state[i] < 0xF)
                fx_blit(pkg, s_c1_state[i] + 0xF, dst, pitch,
                        col[j] + s_c1_rowv[j] * pitch + 0x50);
        }
        for (; i < 8; i++)
            if (s_c1_state[i] >= 0 && s_c1_state[i] < 0xF)
                fx_blit(pkg, s_c1_state[i], dst, pitch,
                        col[i] + s_c1_rowv[i] * pitch + 0x50);
        for (i = 0; i < 8; i++) {
            s_c1_state[i]++;
            if (s_c1_state[i] == 9)
                done = 1;
            else if (s_c1_state[i] == 5)
                fx_sfx_a(1, 1);
        }
        return done;
    }
    return 0;
}

/* 0x2BD6C ctrl_2：帧元状态机（非粒子）。state 0x53FB2 / 副字节
 * 0x53FB3（仅 m0 清零）/ 帧内步 0x53FB4。m0 清返 29；m3 state=0x10
 * 返 12；m6 sfx_a(3) state=0xA 返 10；m1/7 己方：state==0xA 且 m1 →
 * 0xF，fx_cell_step；m2/8：state==7 sfx_a(1)，敌方 state 0xA→0xF(m2)
 * + cell_step，state==0x10 时 blit(0x10)；m4 己方 blit(0xF,
 * dst+1-pitch)；m5 敌方另 blit(0xF, dst-1-pitch)，blit(state,
 * dst+1-pitch)，state++ ==0x11 sfx_a(2)+完成 / ==0x12 钳 0x10。 */
static uint8_t s_c2_state, s_c2_aux, s_c2_sub;          /* 0x53FB2/3/4 */

int fx_ctrl_2_statemach(int ent, const uint8_t *pkg, uint8_t *dst,
                        int pitch, int mode)
{
    int enemy = fx_ctrl_is_enemy(ent);
    int done = 0;

    if (mode == 0) {
        s_c2_state = s_c2_aux = s_c2_sub = 0;
        return 29;
    }
    if (mode == 3) {
        s_c2_state = 0x10;
        return 12;
    }
    if (mode == 6) {
        fx_sfx_a(3, 1);
        s_c2_state = 0xA;
        return 10;
    }
    if (mode == 1 || mode == 7) {
        if (!enemy) {
            if (s_c2_state == 0xA && mode == 1)
                s_c2_state = 0xF;
            fx_cell_step(&s_c2_state, &s_c2_sub, pkg, dst, pitch);
        }
        return 0;
    }
    if (mode == 2 || mode == 8) {
        if (s_c2_state == 7)
            fx_sfx_a(1, 1);
        if (enemy) {
            if (s_c2_state == 0xA && mode == 2)
                s_c2_state = 0xF;
            fx_cell_step(&s_c2_state, &s_c2_sub, pkg, dst, pitch);
        }
        if (s_c2_state == 0x10)
            fx_blit(pkg, 0x10, dst, pitch, 0);
        return 0;
    }
    if (mode == 4) {
        if (!enemy)
            fx_blit(pkg, 0xF, dst, pitch, 1 - pitch);
        return 0;
    }
    if (mode == 5) {
        if (enemy)
            fx_blit(pkg, 0xF, dst, pitch, -1 - pitch);
        fx_blit(pkg, s_c2_state, dst, pitch, 1 - pitch);
        if (++s_c2_state == 0x11) {
            fx_sfx_a(2, 1);
            done = 1;
        } else if (s_c2_state == 0x12) {
            s_c2_state = 0x10;
        }
        return done;
    }
    return 0;
}

/* 0x2BFD9 ctrl_3：12 槽再生粒子流。列 @0x5256D、行速 @0x5259D(12B)、
 * 帧偏移 @0x525A9(12B)；pos = dst + col - rowv*pitch（负号）；state
 * 0x53FB5[12]、几何号 0x53FE5[12]、几何基 0x54015、停止 0x54016、
 * 奇偶 0x54017。m0 state=-2i 几何=i 基=0xC 返 2；m3 返 40；m6 置停止
 * 返 20；m2/5/8：奇偶翻转（隔次推进），活动槽（0..0xA）blit(state+
 * C[geom], col[geom]-rowv[geom]*pitch)，推进槽 state==0 且 C≠0 →
 * sfx_a(2)，++ 后 ==3（C==0 加 sfx_b(1)）置完成，==0xB 未停止 →
 * 几何 (基+1)%12 轮转 state=0 无限再生。敌方全列 +0x14。 */
static int32_t s_c3_state[12];                           /* 0x53FB5 */
static int32_t s_c3_geom[12];                            /* 0x53FE5 */
static uint8_t s_c3_geom_base, s_c3_stop, s_c3_parity;   /* 0x54015/6/7 */
static const int32_t s_c3_col[12]  = { 30, 0, 70, 40, 130, 70, -30, 30, 110, 80, -10, 30 };
static const uint8_t s_c3_rowv[12] = { 0, 10, 0, 20, 5, 20, 0, 10, 0, 18, 0, 10 };
static const uint8_t s_c3_foff[12] = { 22, 0, 0, 0, 0, 0, 11, 0, 22, 0, 0, 0 };

int fx_ctrl_3_particles12(int ent, const uint8_t *pkg, uint8_t *dst,
                          int pitch, int mode)
{
    int32_t col[12];
    int i, j, done = 0;

    for (i = 0; i < 12; i++)
        col[i] = s_c3_col[i] + (fx_ctrl_is_enemy(ent) ? 0x14 : 0);
    if (mode == 0) {
        for (i = 0; i < 12; i++) {
            s_c3_state[i] = -2 * i;
            s_c3_geom[i] = i;
        }
        s_c3_geom_base = 0xC;
        s_c3_stop = 0;
        s_c3_parity = 0;
        return 2;
    }
    if (mode == 3)
        return 40;
    if (mode == 6) {
        s_c3_stop = 1;
        return 20;
    }
    if (mode == 2 || mode == 5 || mode == 8) {
        s_c3_parity = (uint8_t)((s_c3_parity + 1) % 2);
        for (i = 0; i < 12; i++) {
            j = s_c3_geom[i];
            if (s_c3_state[i] >= 0 && s_c3_state[i] < 0xB)
                fx_blit(pkg, s_c3_state[i] + s_c3_foff[j], dst, pitch,
                        col[j] - (int)s_c3_rowv[j] * pitch);
            if (s_c3_parity)
                continue;
            if (s_c3_state[i] == 0 && s_c3_foff[j] != 0)
                fx_sfx_a(2, 1);
            if (++s_c3_state[i] == 3) {
                if (s_c3_foff[j] == 0)
                    fx_sfx_b(1, 1);
                done = 1;
            }
            if (s_c3_state[i] == 0xB && !s_c3_stop) {
                s_c3_geom_base = (uint8_t)((s_c3_geom_base + 1) % 12);
                s_c3_geom[i] = s_c3_geom_base;
                s_c3_state[i] = 0;
            }
        }
        return done;
    }
    return 0;
}

/* 0x2C217 ctrl_4：6 槽（几何池 10 项 @0x525B5）。pos = dst + col（无
 * 行速项）；帧 = state + 行偏移（fd2_rand()%2*7，再生重掷，可负）；
 * state 0x54018[6]、几何 0x54030[6]、行偏移 0x54048[6]、基 0x5404E、
 * 停止 0x5404F。m0 state=-2i 几何=i 返 2；m3 返 12；m6 置停止返 8；
 * m2/5/8：活动槽（0..6）blit，state==0 sfx_a(1)，++ 后 ==3 置完成、
 * ==8 未停止 → 几何 (基+1)%10 轮转 state=0 重掷行偏移。敌方全列
 * +0x8F。 */
static int32_t s_c4_state[6];                            /* 0x54018 */
static int32_t s_c4_geom[6];                             /* 0x54030 */
static uint8_t s_c4_frow[6];                             /* 0x54048 */
static uint8_t s_c4_geom_base, s_c4_stop;                /* 0x5404E/F */
static const int32_t s_c4_col[10] = { 30, 50, 70, 40, 80, 100, 70, 30, 60, 90 };

int fx_ctrl_4_particles10(int ent, const uint8_t *pkg, uint8_t *dst,
                          int pitch, int mode)
{
    int32_t col[10];
    int i, done = 0;

    for (i = 0; i < 10; i++)
        col[i] = s_c4_col[i] + (fx_ctrl_is_enemy(ent) ? 0x8F : 0);
    if (mode == 0) {
        for (i = 0; i < 6; i++) {
            s_c4_state[i] = -2 * i;
            s_c4_geom[i] = i;
            s_c4_frow[i] = (uint8_t)((int)(fd2_rand() % 2) * 7);
        }
        s_c4_geom_base = 6;
        s_c4_stop = 0;
        return 2;
    }
    if (mode == 3)
        return 12;
    if (mode == 6) {
        s_c4_stop = 1;
        return 8;
    }
    if (mode == 2 || mode == 5 || mode == 8) {
        for (i = 0; i < 6; i++) {
            if (s_c4_state[i] >= 0 && s_c4_state[i] < 7)
                fx_blit(pkg, s_c4_state[i] + s_c4_frow[i], dst, pitch,
                        col[s_c4_geom[i]]);
            if (s_c4_state[i] == 0)
                fx_sfx_a(1, 1);
            if (++s_c4_state[i] == 3)
                done = 1;
            if (s_c4_state[i] == 8 && !s_c4_stop) {
                s_c4_geom_base = (uint8_t)((s_c4_geom_base + 1) % 10);
                s_c4_geom[i] = s_c4_geom_base;
                s_c4_state[i] = 0;
                s_c4_frow[i] = (uint8_t)((int)(fd2_rand() % 2) * 7);
            }
        }
        return done;
    }
    return 0;
}

/* 0x2C441 ctrl_5：ctrl_4 变体。6 槽、同几何池 @0x525DD；行偏移
 * fd2_rand()%2*6；m0 返 1；完成于 state==2；再生 ==7；出生音效仅
 * 槽 0（A 通道）/槽 3（B 通道）；尾部 jmp 0x2C439 借 ctrl_4 尾声。 */
static int32_t s_c5_state[6];                            /* 0x54050 */
static int32_t s_c5_geom[6];                             /* 0x54068 */
static uint8_t s_c5_frow[6];                             /* 0x54080 */
static uint8_t s_c5_geom_base, s_c5_stop;                /* 0x54086/7 */
static const int32_t s_c5_col[10] = { 30, 50, 70, 40, 80, 100, 70, 30, 60, 90 };

int fx_ctrl_5_particles10(int ent, const uint8_t *pkg, uint8_t *dst,
                          int pitch, int mode)
{
    int32_t col[10];
    int i, done = 0;

    for (i = 0; i < 10; i++)
        col[i] = s_c5_col[i] + (fx_ctrl_is_enemy(ent) ? 0x8F : 0);
    if (mode == 0) {
        for (i = 0; i < 6; i++) {
            s_c5_state[i] = -2 * i;
            s_c5_geom[i] = i;
            s_c5_frow[i] = (uint8_t)((int)(fd2_rand() % 2) * 6);
        }
        s_c5_geom_base = 6;
        s_c5_stop = 0;
        return 1;
    }
    if (mode == 3)
        return 12;
    if (mode == 6) {
        s_c5_stop = 1;
        return 8;
    }
    if (mode == 2 || mode == 5 || mode == 8) {
        for (i = 0; i < 6; i++) {
            if (s_c5_state[i] >= 0 && s_c5_state[i] < 6)
                fx_blit(pkg, s_c5_state[i] + s_c5_frow[i], dst, pitch,
                        col[s_c5_geom[i]]);
            if (s_c5_state[i] == 0) {
                if (i == 0)
                    fx_sfx_a(1, 1);
                else if (i == 3)
                    fx_sfx_b(1, 1);
            }
            if (++s_c5_state[i] == 2)
                done = 1;
            if (s_c5_state[i] == 7 && !s_c5_stop) {
                s_c5_geom_base = (uint8_t)((s_c5_geom_base + 1) % 10);
                s_c5_geom[i] = s_c5_geom_base;
                s_c5_state[i] = 0;
                s_c5_frow[i] = (uint8_t)((int)(fd2_rand() % 2) * 6);
            }
        }
        return done;
    }
    return 0;
}

/* 0x2C67D ctrl_6：五点轨道环。0x3C8EFA2D 并非 LCG 种子而是 f32 角步
 * 长 0.0174654——θi = 72i*step 均分圆周（§13.4 旧“LCG”记法作废）。
 * x_val = 振幅 + cos(θ)*phase（振幅 敌 0x5A/我 0x1E）、y_val = 30 +
 * sin(θ)*phase*1.2（f64 常数 @0x502AA=1.2、@0x502B2=30.0；cos=
 * 0x3CBD5、sin=0x3CBE8、_CHP=0x37AF4 frndint 就近取整）。表 T1
 * @0x52605/T2 @0x52619 均 {10,8,3,0,0}；敌方前 3 项列取反、T2 清 0。
 * x_val 0x54088[5]、y_val 0x5409C[5]、state 0x540B0[5]、拖尾步
 * 0x540C4[5]、phase 0x540C9、换位标志 0x540CA。
 * m0 sfx_a(2) phase=0 返 7；m3 首调换位（x_val[2]<->[4]、y_val[2]=
 * y_val[4]、y_val[4]=phase，state=-i 拖尾清）返 12；m6 sfx_a(3)
 * phase=0x2A 返 7；m1/2/7/8 算轨道并 blit(帧4, dst+y_val*pitch+
 * x_val)（敌方 m1/7 槽≥2、m2/8 槽>1；我方 m2/8 全槽），m2 phase+=6、
 * m8 phase-=6；m4/5 phase<-state[i]（<0→4；==1 置拖尾步=5），blit
 * (phase, dst+x_val+(y_val+T2[phase])*pitch+T1[phase])（敌方 m4
 * 槽≥2/m5 槽>1、我方 m5 全槽）；m5 收尾拖尾环：拖尾≠0 blit(sub,
 * dst+x_val-0x3C+(y_val-0x14)*pitch) 步进 0..0xA 循环，state==0
 * 槽 0/2 sfx_a(1) 其余 sfx_b(1)，state++ ==5 归 0、==2 置完成。 */
static int32_t s_c6_xv[5], s_c6_yv[5], s_c6_state[5];   /* 0x54088/9C/B0 */
static uint8_t s_c6_sub[5], s_c6_phase, s_c6_swapped;   /* 0x540C4/C9/CA */
static const int32_t s_c6_t1[5] = { 10, 8, 3, 0, 0 };    /* 0x52605 */
static const uint8_t s_c6_t2[5] = { 10, 8, 3, 0, 0 };    /* 0x52619 */
static const uint32_t s_c6_step_bits = 0x3C8EFA2Du;     /* f32 0.0174654 */

int fx_ctrl_6_sparks(int ent, const uint8_t *pkg, uint8_t *dst,
                     int pitch, int mode)
{
    int32_t t1[5];
    uint8_t t2[5];
    int amp = 0x1E, enemy = fx_ctrl_is_enemy(ent);
    int i, ph, done = 0;
    int32_t tmp;
    float step;

    memcpy(&step, &s_c6_step_bits, sizeof step);
    for (i = 0; i < 5; i++) {
        t1[i] = s_c6_t1[i];
        t2[i] = s_c6_t2[i];
    }
    if (enemy) {
        amp = 0x5A;
        for (i = 0; i < 3; i++) {
            t1[i] = -t1[i];
            t2[i] = 0;
        }
    }
    if (mode == 0) {
        fx_sfx_a(2, 1);
        s_c6_phase = 0;
        s_c6_swapped = 0;
        return 7;
    }
    if (mode == 3) {
        if (!s_c6_swapped) {
            for (i = 0; i < 5; i++) {
                s_c6_state[i] = -i;
                s_c6_sub[i] = 0;
            }
            tmp = s_c6_xv[2];
            s_c6_xv[2] = s_c6_xv[4];
            s_c6_xv[4] = tmp;
            s_c6_yv[2] = s_c6_yv[4];
            s_c6_yv[4] = s_c6_phase;
            s_c6_swapped = 1;
        }
        return 12;
    }
    if (mode == 6) {
        fx_sfx_a(3, 1);
        s_c6_phase = 0x2A;
        return 7;
    }
    if (mode == 1 || mode == 2 || mode == 7 || mode == 8) {
        for (i = 0; i < 5; i++) {
            double ang = (double)(72 * i) * (double)step;
            s_c6_xv[i] = (int32_t)lrint(amp + cos(ang) * s_c6_phase);
            s_c6_yv[i] = (int32_t)lrint(30.0 + sin(ang) * s_c6_phase * 1.2);
            if (enemy) {
                if (mode == 1 || mode == 7) {
                    if (i >= 2)
                        fx_blit(pkg, 4, dst, pitch,
                                s_c6_yv[i] * pitch + s_c6_xv[i]);
                } else if (i > 1) {
                    fx_blit(pkg, 4, dst, pitch,
                            s_c6_yv[i] * pitch + s_c6_xv[i]);
                }
            } else if (mode == 2 || mode == 8) {
                fx_blit(pkg, 4, dst, pitch,
                        s_c6_yv[i] * pitch + s_c6_xv[i]);
            }
        }
        if (mode == 2)
            s_c6_phase += 6;
        else if (mode == 8)
            s_c6_phase -= 6;
        return 0;
    }
    if (mode == 4 || mode == 5) {
        for (i = 0; i < 5; i++) {
            ph = s_c6_state[i] < 0 ? 4 : (int)(uint8_t)s_c6_state[i];
            s_c6_phase = (uint8_t)ph;
            if (ph == 1)
                s_c6_sub[i] = 5;
            if (enemy) {
                if (mode == 4 ? i >= 2 : i > 1)
                    fx_blit(pkg, ph, dst, pitch,
                            (t2[ph] + s_c6_yv[i]) * pitch + s_c6_xv[i]
                            + t1[ph]);
            } else if (mode == 5) {
                fx_blit(pkg, ph, dst, pitch,
                        (t2[ph] + s_c6_yv[i]) * pitch + s_c6_xv[i] + t1[ph]);
            }
        }
        if (mode != 5)
            return 0;
        for (i = 0; i < 5; i++) {
            if (s_c6_sub[i] != 0) {
                fx_blit(pkg, s_c6_sub[i], dst, pitch,
                        s_c6_xv[i] - 0x3C + (s_c6_yv[i] - 0x14) * pitch);
                if (++s_c6_sub[i] == 0xA)
                    s_c6_sub[i] = 0;
            }
            if (s_c6_state[i] == 0) {
                if (i == 0 || i == 2)
                    fx_sfx_a(1, 1);
                else
                    fx_sfx_b(1, 1);
            }
            if (++s_c6_state[i] == 5)
                s_c6_state[i] = 0;
            if (s_c6_state[i] == 2)
                done = 1;
        }
        return done;
    }
    return 0;
}

/* 0x2CAFC fx_ctrl_7：3 槽（池 10 项 @0x5261E），-3k 错峰、奇偶节流
 * （m2/5/8 隔次推进）；state==1 槽 0→A 通道 / 槽 1→B 通道；==2 置完成；
 * ==7 未停止 → 几何 (基+1)%10 轮转。敌方全列 +130（0x82）。
 * m0 state=-3k 几何=k 基=4 返 2；m3 返 32；m6 置停止返 16。 */
static int32_t s_c7_state[4], s_c7_geom[4];              /* 0x540CB/0x540DB */
static uint8_t s_c7_base, s_c7_stop, s_c7_parity;        /* 0x540EB/EC/ED */
static const int32_t s_c7_col[10] = { 30,-10, 70, 20,100,130, 40, 80,110, 60 };

int fx_ctrl_7(int ent, const uint8_t *pkg, uint8_t *dst, int pitch, int mode)
{
    int32_t col[10];
    int i, j, done = 0;

    for (i = 0; i < 10; i++)
        col[i] = s_c7_col[i] + (fx_ctrl_is_enemy(ent) ? 130 : 0);
    if (mode == 0) {
        for (i = 0; i < 4; i++) {
            s_c7_state[i] = -3 * i;
            s_c7_geom[i] = i;
        }
        s_c7_base = 4;
        s_c7_stop = 0;
        s_c7_parity = 0;
        return 2;
    }
    if (mode == 3)
        return 32;
    if (mode == 6) {
        s_c7_stop = 1;
        return 16;
    }
    if (mode == 2 || mode == 5 || mode == 8) {
        s_c7_parity = (uint8_t)((s_c7_parity + 1) % 2);
        for (j = 0; j < 3; j++) {
            if ((uint32_t)s_c7_state[j] <= 4)
                fx_blit(pkg, s_c7_state[j], dst, pitch,
                        col[s_c7_geom[j]]);
            if (s_c7_parity)
                continue;
            if (s_c7_state[j] == 1) {
                if (j == 0)
                    fx_sfx_a(1, 1);
                else if (j == 1)
                    fx_sfx_b(1, 1);
            }
            if (++s_c7_state[j] == 2)
                done = 1;
            if (s_c7_state[j] == 7 && !s_c7_stop) {
                s_c7_base = (uint8_t)((s_c7_base + 1) % 10);
                s_c7_geom[j] = s_c7_base;
                s_c7_state[j] = 0;
            }
        }
        return done;
    }
    return 0;
}

/* 0x2CCF4 fx_ctrl_8：16 槽全带（帧偏 @0x52646 循环 {0,8,24,16}），
 * 无敌方修正、无再生（state 过 8 即熄灭，等 m0 复位）。m0 state=-2j
 * 返 3；m3 返 34；m6 返 2；m2/5（无 m8）：活动槽（u32<8）blit(state+
 * 偏移, dst)，state==0 sfx_a(1)、==4 sfx_b(2)，++==4 置完成。 */
static int32_t s_c8_state[16];                           /* 0x540EE */
static const uint8_t s_c8_foff[16] = {
    0, 8, 24, 16, 8, 0, 24, 16, 0, 8, 24, 16, 8, 0, 24, 16 };

int fx_ctrl_8(int ent, const uint8_t *pkg, uint8_t *dst, int pitch, int mode)
{
    int i, done = 0;

    if (mode == 0) {
        for (i = 0; i < 16; i++)
            s_c8_state[i] = -2 * i;
        return 3;
    }
    if (mode == 3)
        return 34;
    if (mode == 6)
        return 2;
    if (mode == 2 || mode == 5) {
        for (i = 0; i < 16; i++) {
            if ((uint32_t)s_c8_state[i] < 8)
                fx_blit(pkg, s_c8_state[i] + s_c8_foff[i], dst, pitch, 0);
            if (s_c8_state[i] == 0)
                fx_sfx_a(1, 1);
            if (s_c8_state[i] == 4)
                fx_sfx_b(2, 1);
            if (++s_c8_state[i] == 4)
                done = 1;
        }
        return done;
    }
    return 0;
}

/* 0x2CE1A fx_ctrl_9：计数帧特写（效果 9 横滑）。计数器 0x5412E 自 1
 * 增：m1/7 奇偶 blit(帧0)；m4 blit(帧4)；m5 blit(帧=计数>>1)，==6
 * sfx_a(1)、==36 sfx_b(2)，命中窗口 = 增后计数 ∈ 0x11..0x2B。
 * m0 计数=1 奇偶清 返 20；m3 返 60；m6 返 20。 */
static uint8_t s_c9_count, s_c9_parity;                  /* 0x5412E/0x5412F */

int fx_ctrl_9(int ent, const uint8_t *pkg, uint8_t *dst, int pitch, int mode)
{
    if (mode == 0) {
        s_c9_parity = 0;
        s_c9_count = 1;
        return 20;
    }
    if (mode == 3)
        return 60;
    if (mode == 6)
        return 20;
    if (mode == 1 || mode == 7) {
        if (!s_c9_parity)
            fx_blit(pkg, 0, dst, pitch, 0);
        s_c9_parity ^= 1;
        return 0;
    }
    if (mode == 4) {
        fx_blit(pkg, 4, dst, pitch, 0);
        return 0;
    }
    if (mode == 5) {
        fx_blit(pkg, s_c9_count >> 1, dst, pitch, 0);
        if (s_c9_count == 6)
            fx_sfx_a(1, 1);
        else if (s_c9_count == 36)
            fx_sfx_b(2, 1);
        ++s_c9_count;
        return s_c9_count < 0x2C && s_c9_count > 0x10;
    }
    return 0;
}

/* g_fx_controllers @0x524C6 恰 10 项（0x524EE 起即 ctrl_0 内表）。
 * 0..9 已全部定案重构（§13.9/§13.10）；越界 id 原版经 fx_scene_play
 * 分发守卫不可达，防御返 0。 */
int fx_ctrl_dispatch(int id, int ent, const uint8_t *pkg, uint8_t *dst,
                     int pitch, int mode)
{
    switch (id) {
    case 0: return fx_ctrl_0_particles(ent, pkg, dst, pitch, mode);
    case 1: return fx_ctrl_1_particles8(ent, pkg, dst, pitch, mode);
    case 2: return fx_ctrl_2_statemach(ent, pkg, dst, pitch, mode);
    case 3: return fx_ctrl_3_particles12(ent, pkg, dst, pitch, mode);
    case 4: return fx_ctrl_4_particles10(ent, pkg, dst, pitch, mode);
    case 5: return fx_ctrl_5_particles10(ent, pkg, dst, pitch, mode);
    case 6: return fx_ctrl_6_sparks(ent, pkg, dst, pitch, mode);
    case 7: return fx_ctrl_7(ent, pkg, dst, pitch, mode);
    case 8: return fx_ctrl_8(ent, pkg, dst, pitch, mode);
    case 9: return fx_ctrl_9(ent, pkg, dst, pitch, mode);
    default: return 0;
    }
}

/* 0x2E95B sprites_reload_fd2tmp：fopen("FD2.TMP","rb") 重读 0x32A00
 * 字节立绘缓存到 g_standing_sprites（演出后战场恢复；battle 侧
 * sprites_reload_slot 负责 TMP 的生成方向）。
 * 原版此处【不】清图标缓存元数据（dword_53BDF 计数/53B17 id 表/
 * dword_539EC 追加偏移全保留，0x2E95B 反汇编核对）——fd2re 此前
 * 多调 icon_cache_reset() 把计数归零，而场上单位 ent[2] 槽号未变：
 * 演出后首次 icon_load_entry（剧情事件增援/新单位）把新图标写进
 * 头表第 0 行并覆写偏移 1920 起的数据区，主角（槽 0）场上立绘随之
 * 变成该新角色，下一图标继续覆写第 1 行——即"战斗中剧情对话后场上
 * 人物立绘顺序变成说话角色"的根因（2026-09-08 定案，删重置修复）。 */
static void sprites_reload_fd2tmp(void)
{
    FILE *fp = fopen("FD2.TMP", "rb");

    g_standing_sprites = malloc(0x32A00);
    if (fp && g_standing_sprites) {
        fread(g_standing_sprites, 1, 0x32A00, fp);
        fclose(fp);
    }
}

/* 0x30E25 duel_bg_pick(n, targets)：g_state_duel_bg[g_state] 起，自后
 * 向前逐目标 tile_lookup 取地块 bg（info[6]）；目标非地形免疫且已有
 * 状态值则保留，否则用地块导出值。 */
static int duel_bg_pick(int n, const uint8_t *targets)
{
    uint8_t info[8];
    int bg = g_state_duel_bg[g_state];

    for (int i = n - 1; i >= 0; i--) {
        uint8_t *rec = g_ent_table[targets[i]];
        tile_lookup(rec[0], rec[1], info);
        if (!sub_1F183(targets[i]) || !bg)
            bg = info[6];
    }
    return bg;
}

/* 0x30E9D duel_frame_compose(actor, effect, pose, tpkg, shadow, frame,
 * bg, tai)——决斗首帧演出（施法者 FIGANI 3i+2 帧序列逐帧 repeat 播放，
 * 目标 tpkg 同协议并行步进；帧记录 [4]==1 触发 fx_field_reset + 单位
 * 面板 + 结果窗格（bg/tai 经 blk3 LUT 重映射着色）+ DAC 0 号色闪 6 次）。
 * 敌方（rec[+6]!=0）pose 后画、我方 pose 先画（遮挡次序）。 */
static void duel_frame_compose(int actor, int effect_id, const uint8_t *pose,
                               const uint8_t *tpkg, uint8_t *shadow,
                               uint8_t *frame, const uint8_t *bg,
                               const uint8_t *tai)
{
    uint8_t *vram = vram_base();
    uint8_t tsub = 0, tfrm = 0;
    int win = 11, flash = 0;
    int enemy = g_ent_table[actor][6] != 0;

    if (effect_id == 8 || effect_id == 32 || effect_id == 33)
        win = 19;
    else if (effect_id > 3)
        win = 15;
    if (!pose || !tpkg || !bg || !tai)
        return;
    for (int i = 0; i < pose[2]; i++) {
        const uint8_t *rec = pose + fx_u32(pose + 8 + 4 * i);
        if ((effect_id == 24 || (effect_id > 27 && effect_id < 31))
            && rec[5])
            sfx_play(g_anim_sfx_data, rec[5], 1);
        if (rec[4] == 1) {
            fx_field_reset(actor, effect_id);
            duel_stamp_unit(frame, actor);
            if (effect_id < 10 || effect_id >= 32) {
                const uint8_t *lut = (const uint8_t *)g_pkg_fdother_3
                    + fx_u32((const uint8_t *)g_pkg_fdother_3 + 6 + 4 * win);
                flash = 6;
                rle_decode_frame_lut(bg, 0, 50, frame, 320, lut);
                rle_decode_frame_lut(tai, 164, 157, frame, 320, lut);
                sfx_play(g_fx_sfx_pkg, 0, 1);
            }
        }
        for (int j = 0; j < rec[6]; j++) {
            blit_rows(shadow, 320, frame, 320, 320, 200);
            if (enemy) {
                blit_frame_flat(pose, i, shadow, 320, -1);
                if (effect_id < 10 || effect_id == 28)
                    blit_frame_flat(tpkg, tfrm, shadow, 320, -1);
            } else {
                if (effect_id < 10 || effect_id == 28)
                    blit_frame_flat(tpkg, tfrm, shadow, 320, -1);
                blit_frame_flat(pose, i, shadow, 320, -1);
            }
            blit_rows(vram, 320, shadow, 320, 320, 200);
            if (flash) {
                dac_write(0, s_fx_flash_rgb[0][effect_id],
                          s_fx_flash_rgb[1][effect_id],
                          s_fx_flash_rgb[2][effect_id]);
                delay_ms(30);
                dac_write(0, 0, 0, 0);
                flash--;
            }
            {
                const uint8_t *tf = tpkg + fx_u32(tpkg + 8 + 4 * tfrm);
                if (++tsub == tf[6]) {
                    tsub = 0;
                    if (++tfrm == tpkg[0])
                        tfrm = 0;
                }
            }
            wait_bios_ticks(1);
        }
    }
}

/* 0x2DFC8 unit_anim_play：FIGANI 3*icon+1 帧表动画。帧 =
 * pkg + u32[pkg+6+4*i]（LMI1 族）；帧记录 [5]=音效 id（0=无）、
 * [6]=延时 tick；逐帧 sfx_play(g_anim_sfx_pkg) + wait_bios_ticks +
 * rle_decode_frame 帧 [w][h][4-op RLE] 到 VRAM 中央。 */
void unit_anim_play(int ent, int start_frame)
{
    uint8_t *pkg = dat_load_block("FIGANI.DAT", NULL, 3 * g_ent_table[ent][7] + 1);
    int count, frame;

    if (!pkg || pkg[0] != 'L' || pkg[1] != 'M')
        { free(pkg); return; }
    count = pkg[4] | (pkg[5] << 8);
    for (frame = start_frame; frame < count; frame++) {
        const uint8_t *rec = pkg + (uint32_t)(pkg[6 + 4 * frame]
                             | (pkg[7 + 4 * frame] << 8)
                             | (pkg[8 + 4 * frame] << 16)
                             | ((uint32_t)pkg[9 + 4 * frame] << 24));
        int w = rec[0] | (rec[1] << 8);
        int h = rec[2] | (rec[3] << 8);

        if (rec[5])
            sfx_play(g_magic_sfx_pkg, rec[5], 1);
        rle_decode_frame(rec + 4, (320 - w) / 2, (200 - h) / 2,
                         vram_base(), 320, -1);
        wait_bios_ticks(rec[6] ? rec[6] : 1);
    }
    free(pkg);
}

void fx_scene_play(int actor, int effect_id, int n_targets, const uint8_t *targets)
{
    /* 0x2FF01：effect_id>=32→fx_scene_special；==24||>27→fx_scene_multi_hit；
     * 0..23 本体。受击分支由 fx_hit_immune_test 决定；HP 级降 + 随机震动
     * （fd2_rand()%3，位移 g_fx_shake_dx/dy）；换靶 fx_target_transition；
     * 收尾 FDOTHER blk3 帧 12..14 结果窗格（类别 11/15/19）。 */
    if (effect_id >= 32) { fx_scene_special(actor, effect_id, n_targets, targets); return; }
    if (effect_id == 24 || effect_id > 27) { fx_scene_multi_hit(actor, effect_id, n_targets, targets); return; }
    /* 0x2FF01..0x30E24 VGA 影子管线全解（2026-09-05）：影子缓冲
     * shadow=malloc(0x2A300) 640 行距双宽（v87=19360 中带）、frame=
     * malloc(64000) 合成基帧；施法者 FIGANI 3i/3i+2、目标群 3i；
     * 覆盖包 = FDOTHER overlay_r/l[effect]；g_fx_sfx_pkg = FDOTHER
     * "RRSTUVWXYZ"[effect]（表恰 10 项，effect>=10 原版越界读相邻
     * tile 本地缓冲——宿主守卫跳过装载，音效静音）。 */
    if (!g_ent_table || !targets || n_targets <= 0
        || actor < 0 || actor >= g_ent_count)
        return;

    uint8_t *vram = vram_base();
    uint8_t info[8];
    uint8_t *bg_pkg = NULL, *tai_pkg = NULL, *anim = NULL, *tpose = NULL;
    uint8_t *overlay = NULL, *frame_buf = NULL, *shadow = NULL, *tpkg[30];
    int win_base = 11, alt = 0;
    /* Ten block IDs; omit an unused terminator so /W4 does not report a
     * misleading C4295 "array too small" diagnostic. */
    static const char s_fx_sfx_blocks[] = { 'R','R','S','T','U','V','W','X','Y','Z' };

    if (effect_id == 8)
        win_base = 19;
    else if (effect_id > 3)
        win_base = 15;

    /* 原版 0x2FF01 仅 free 立绘缓冲，不清缓存计数（见
     * sprites_reload_fd2tmp 头注释——多清即场上立绘顺序腐败根因）。 */
    free(g_standing_sprites);
    g_standing_sprites = NULL;
    for (int i = 0; i < 30; i++)
        tpkg[i] = NULL;

    uint8_t *arec = g_ent_table[actor];
    tile_lookup(arec[0], arec[1], info);
    int state_bg = g_state_duel_bg[g_state];
    if (!sub_1F183(actor) || !state_bg)
        state_bg = info[6];
    int pick = duel_bg_pick(n_targets, targets);
    bg_pkg  = dat_load_block("BG.DAT", NULL, arec[6] ? state_bg : pick);
    tai_pkg = dat_load_block("TAI.DAT", NULL, arec[6] ? pick : state_bg);
    frame_buf = malloc(64000);
    shadow = malloc(0x2A300);
    if (!frame_buf || !shadow || !bg_pkg || !tai_pkg)
        goto cleanup;
    memset(frame_buf, 0, 64000);
    duel_stamp_unit(frame_buf, actor);
    duel_stamp_unit(frame_buf, targets[0]);
    rle_decode_frame(bg_pkg, 0, 50, frame_buf, 320, -1);
    anim = dat_load_block("FIGANI.DAT", NULL, 3 * arec[7]);
    tpose = dat_load_block("FIGANI.DAT", NULL, 3 * arec[7] + 2);
    if (tpose && (*(uint16_t *)tpose) == 0) {
        free(tpose);
        tpose = dat_load_block("FIGANI.DAT", NULL, 3 * arec[7] + 1);
    }
    free(g_fx_sfx_pkg);
    g_fx_sfx_pkg = effect_id < 10
        ? dat_load_block("FDOTHER.DAT", NULL, s_fx_sfx_blocks[effect_id])
        : NULL;      /* effect>=10 原版越界读本地 tile 缓冲，宿主守卫 */
    palette_fade_black();
    /* 0x302AC 装载循环原样无界（j < n_targets 直写 v55[j-1]，>30 目标
     * 越界写相邻局部——DOS 帧布局下相邻是震动表/标量，偶合成可用；
     * 宿主栈布局不同即破坏）。按 fx_scene_multi_hit 同款 30 上限护栏。 */
    for (int j = 0; j < n_targets && j < 30; j++)
        tpkg[j] = dat_load_block("FIGANI.DAT", NULL,
                                 3 * g_ent_table[targets[j]][7]);
    overlay = dat_load_block("FDOTHER.DAT", NULL, arec[6]
                             ? (effect_id < 17 ? s_fx_overlay_r[effect_id] : 0)
                             : (effect_id < 17 ? s_fx_overlay_l[effect_id] : 0));

    duel_intro_slide(actor, 0, anim, tpkg[0], shadow, frame_buf, tai_pkg);
    duel_frame_compose(actor, effect_id, tpose, tpkg[0], shadow, frame_buf,
                       bg_pkg, tai_pkg);
    {
        int last_frame = tpose ? tpose[0] - 1 : 0;
        /* 出靶相/结果窗格取 tpkg[a7-1]（0x309DA 族）；>30 目标宿主钳
         * 29（原版越界读相邻局部）。 */
        int last_pkg = n_targets - 1 < 30 ? n_targets - 1 : 29;

        if (effect_id == 9) {
            for (int k = 0; k <= 10; k++) {
                blit_rows(shadow + 320, 640, frame_buf, 320, 320, 200);
                if (k != 10)
                    blit_frame_flat(anim, 0, shadow + 320 - 10 * k, 640, -1);
                blit_frame_flat(tpkg[0], 0, shadow + 320, 640, -1);
                blit_rows(vram, 320, shadow + 320, 640, 320, 200);
                delay_ms(500);
            }
        }

        /* 入场相：mode0 帧数 → 每帧 mode1/mode2 + 姿态末帧覆盖 */
        {
            int intro = fx_ctrl_dispatch(effect_id, actor, overlay,
                                         shadow, 320, 0);
            unit_pose_step(tpkg[0], 0, shadow, 640);
            for (int m = 0; m < intro; m++) {
                blit_rows(shadow + 19360, 640, frame_buf, 320, 320, 200);
                fx_ctrl_dispatch(effect_id, actor, overlay,
                                 shadow + 19360, 640, 1);
                if (effect_id != 9)
                    blit_frame_flat(tpose, last_frame, shadow + 19360,
                                    640, -1);
                unit_pose_step(tpkg[0], -1, shadow + 19360, 640);
                fx_ctrl_dispatch(effect_id, actor, overlay,
                                 shadow + 19360, 640, 2);
                blit_rows(vram, 320, shadow + 19360, 640, 320, 200);
                wait_bios_ticks(1);
            }
        }

        /* 受击相：mode3 受击数 → 每帧 mode4/mode5；免疫（helper 返 0）
         * 走简单分支（无震动/无 HP 插值，0x30960 setz 实证）；否则
         * sign=1-fd2_rand()%3 × dx/dy 表逐级衰减震动，mode5==1 时
         * HP 按 old - step*(old-new)/hit_counts 级降并刷新面板。 */
        for (int n = 0; n < n_targets; n++) {
            int target = targets[n];
            if (target < 0 || target >= g_ent_count)
                continue;
            uint8_t *trec = g_ent_table[target];
            int hits = fx_ctrl_dispatch(effect_id, actor, overlay,
                                        shadow, 320, 3);
            unit_pose_step(tpkg[0], 0, shadow, 640);
            int old_hp = (int16_t)*(uint16_t *)(trec + 64);
            int immune = fx_hit_immune_test(target, effect_id) == 0;
            int new_hp = (int16_t)*(uint16_t *)(trec + 64);
            if (n >= 30 || !tpkg[n]) {
                /* 无资源回退（>30 目标宿主护栏，原版无此路径——直写
                 * 终值；免疫时 new==old 写回无差）。 */
                *(uint16_t *)(trec + 64) = (uint16_t)new_hp;
                continue;
            }
            *(uint16_t *)(trec + 64) = (uint16_t)old_hp;
            int step = 1, shake_i = 3, sign = -1, slide = 8;
            for (int ii = 0; ii < hits; ii++) {
                if (effect_id == 7 || effect_id == 3 || effect_id == 9) {
                    alt ^= 1;
                    blit_rows(shadow + 19360 - 640 * alt, 640,
                              frame_buf, 320, 320, 200);
                } else {
                    blit_rows(shadow + 19360, 640, frame_buf, 320, 320, 200);
                }
                fx_ctrl_dispatch(effect_id, actor, overlay,
                                 shadow + 19360, 640, 4);
                if (effect_id != 9)
                    blit_frame_flat(tpose, last_frame, shadow + 19360,
                                    640, -1);
                if (immune) {
                    unit_pose_step(tpkg[n], -1, shadow + 19360, 640);
                    fx_ctrl_dispatch(effect_id, actor, overlay,
                                     shadow + 19360, 640, 5);
                } else {
                    int dx = sign * s_fx_shake_dx[shake_i];
                    int dy = s_fx_shake_dy[shake_i];
                    unit_pose_step(tpkg[n], -1,
                                   shadow + 19360 + dx + 640 * dy, 640);
                    if (--slide == 1)
                        slide = 8;
                    if (shake_i != 3)
                        shake_i++;
                    if (fx_ctrl_dispatch(effect_id, actor, overlay,
                                         shadow + 19360, 640, 5) == 1) {
                        int hc = g_fx_hit_counts[effect_id];
                        if (hc >= step) {
                            *(uint16_t *)(trec + 64) = (uint16_t)
                                (old_hp - step * (old_hp - new_hp) / hc);
                            duel_stamp_unit(frame_buf, target);
                            step++;
                        }
                        shake_i = 0;
                        sign = 1 - (int)(fd2_rand() % 3);
                    }
                }
                blit_rows(vram, 320, shadow + 19360, 640, 320, 200);
                wait_bios_ticks(1);
            }
            if (n != n_targets - 1) {
                fx_target_transition(actor, overlay, tpose, tpkg[n],
                                     shadow, frame_buf, tpkg[n + 1],
                                     effect_id);
                duel_stamp_unit(frame_buf, targets[n + 1]);
            }
        }

        /* 出靶相：mode6 帧数 → mode7/mode8；效果 9 反向横滑收 */
        {
            int outro = fx_ctrl_dispatch(effect_id, actor, overlay,
                                         shadow, 320, 6);
            for (int jj = 0; jj < outro; jj++) {
                blit_rows(shadow + 19360, 640, frame_buf, 320, 320, 200);
                fx_ctrl_dispatch(effect_id, actor, overlay,
                                 shadow + 19360, 640, 7);
                if (effect_id != 9)
                    blit_frame_flat(tpose, last_frame, shadow + 19360,
                                    640, -1);
                unit_pose_step(tpkg[last_pkg], -1, shadow + 19360, 640);
                fx_ctrl_dispatch(effect_id, actor, overlay,
                                 shadow + 19360, 640, 8);
                blit_rows(vram, 320, shadow + 19360, 640, 320, 200);
                wait_bios_ticks(1);
            }
        }
        if (effect_id == 9) {
            for (int kk = 7; kk >= 0; kk--) {
                blit_rows(shadow + 320, 640, frame_buf, 320, 320, 200);
                blit_frame_flat(anim, 0, shadow + 320 - 10 * kk, 640, -1);
                blit_frame_flat(tpkg[0], 0, shadow + 320, 640, -1);
                blit_rows(vram, 320, shadow + 320, 640, 320, 200);
            }
        }

        /* 结果窗格：mm=1..3 经 blk3 LUT 着色（bg(0,50)/tai(164,157)）
         * + 姿态帧 0；终帧回退无 LUT 解码。 */
        {
            const uint8_t *lut_pkg = (const uint8_t *)g_pkg_fdother_3;
            for (int mm = 1; mm < 4; mm++) {
                blit_rows(shadow, 320, frame_buf, 320, 320, 200);
                if (lut_pkg) {
                    const uint8_t *lut = lut_pkg
                        + fx_u32(lut_pkg + 6 + 4 * (mm + win_base));
                    rle_decode_frame_lut(bg_pkg, 0, 50, frame_buf, 320, lut);
                    rle_decode_frame_lut(tai_pkg, 164, 157, frame_buf, 320,
                                         lut);
                }
                blit_frame_flat(tpose, 0, shadow, 320, -1);
                unit_pose_step(tpkg[last_pkg], -1, shadow, 320);
                blit_rows(vram, 320, shadow, 320, 320, 200);
                wait_bios_ticks(1);
            }
            blit_rows(shadow, 320, frame_buf, 320, 320, 200);
            rle_decode_frame(bg_pkg, 0, 50, frame_buf, 320, -1);
            rle_decode_frame(tai_pkg, 164, 157, frame_buf, 320, -1);
            blit_frame_flat(tpose, 0, shadow, 320, -1);
            unit_pose_step(tpkg[last_pkg], -1, shadow, 320);
            blit_rows(vram, 320, shadow, 320, 320, 200);
            sfx_play(g_fx_sfx_pkg, -1, 1);
        }
    }

cleanup:
    free(g_fx_sfx_pkg);
    g_fx_sfx_pkg = NULL;
    free(overlay);
    /* 0x30D40 释放循环原样无界；>30 目标宿主护栏（原版越界 free 相邻
     * 局部中偶合成的包指针）。 */
    for (int nn = 0; nn < n_targets && nn < 30; nn++)
        free(tpkg[nn]);
    free(frame_buf);
    free(shadow);
    free(anim);
    free(tpose);
    free(bg_pkg);
    free(tai_pkg);
    /* 0x30DB2..0x30E24 收尾：FD2.TMP 重读立绘缓存（原版另分配
     * g_shadow_buf2/g_shadow_buf —— fd2re 不维护该对，省略）→
     * 等 8 tick → 淡黑 → 清屏 → anim_tick_update(1) → fade_in。 */
    sprites_reload_fd2tmp();
    wait_bios_ticks(8);
    palette_fade_black();
    memset(vram, 0, 64000);
    anim_tick_update(1);
    fade_in();
}

/* ---- 0x2D80D/0x2CF30 前置 VGA 演出数据（2026-09-08 全量落地）----
 * 0x52656 = multi_hit 震动表（dwell 步进 work+320-tbl[i]，i 自 5 递减
 * 阻尼振荡）；0x5265C/0x52660/0x52664 = 大招（效果 32..35）三表——
 * 首表双用：FDOTHER 音效块号（'?'=63/'3'=51/'5'=53/'5'=53）兼闪白
 * R 分量；0x52668 "[\]^"（块 91..94）无效果 36+ 消费者。
 * 0x5413F/0x54143/0x54147 = BG.DAT 块 0/1/2 三层（换景滑动消费）。 */
static const uint8_t s_mh_shake[6]   = { 4, 9, 14, 18, 14, 3 };
static const uint8_t s_sp_sfx_r[4]   = { 0x3F, 0x33, 0x35, 0x35 };
static const uint8_t s_sp_flash_g[4] = { 0x3F, 0x39, 0x00, 0x3A };
static const uint8_t s_sp_flash_b[4] = { 0x3F, 0x3F, 0x00, 0x09 };
static uint8_t *s_bg_layers[3];
/* 0x5413B/0x54137（duel_scene_load 装载，duel_exchange 换景消费）与
 * 0x5414F 守方音效包。 */
static uint8_t *s_duel_bg_main, *s_duel_bg_alt;
static uint8_t *s_duel_sfx_def;

/* 0x314DE：姿态包内嵌音效装载——pkg[4] 非零 → FDOTHER 块
 * "012345"[pkg[4]-1]，否则 NULL（multi_hit 装载、帧[5] 音效消费）。 */
static uint8_t *anim_sfx_load(const uint8_t *pose_pkg)
{
    static const char s_blocks[6] = { '0', '1', '2', '3', '4', '5' };

    if (pose_pkg && pose_pkg[4])
        return dat_load_block("FDOTHER.DAT", NULL,
                              (uint8_t)s_blocks[pose_pkg[4] - 1]);
    return NULL;
}

/* 0x2F4D4 duel_compose_target(target, tpose, frame, work, bg)：双 10 步
 * 换景——前段 BG 三层 [i%3] 逐次解码进 work、以 work+32*i 窗口上屏
 *（i=9..0 左滑）；中段 bg+目标小图+tpose 帧 0 合成于 work 左带；
 * 后段三层 [(j+2)%3] 解码进 work+320、以 work+320+32*j 窗口上屏。
 * 注：j=9 窗口尾读越 work+0x1F400 达 288B（原版同款 DOS 堆越读，
 * 调用方多分配 640B 代偿）。 */
static void duel_compose_target(int target, const uint8_t *tpose,
                                uint8_t *frame, uint8_t *work,
                                const uint8_t *bg)
{
    uint8_t *vram = vram_base();
    int i;

    for (i = 9; i >= 0; i--) {
        if (s_bg_layers[i % 3])
            rle_decode_frame(s_bg_layers[i % 3], 0, 50, work, 640, -1);
        blit_rows(vram, 320, work + 32 * i, 640, 320, 200);
    }
    memset(work, 0, 0x1F400);
    memset(frame, 0, 64000);
    if (bg)
        rle_decode_frame(bg, 0, 50, frame, 320, -1);
    duel_stamp_unit(frame, target);
    blit_rows(work, 640, frame, 320, 320, 200);
    if (tpose)
        blit_frame_flat(tpose, 0, work, 640, -1);
    for (i = 9; i >= 0; i--) {
        if (s_bg_layers[(i + 2) % 3])
            rle_decode_frame(s_bg_layers[(i + 2) % 3], 0, 50, work + 320,
                             640, -1);
        blit_rows(vram, 320, work + 320 + 32 * i, 640, 320, 200);
    }
}

/* 0x2F631 duel_compose_mirror（sub_2F631，2026-09-08 全解）：compose_target
 * 的镜像版——右带合成 + TAI(164,157) 盖印，滑动方向反转（i=1..9 呈
 * work+32*i 左→右；j=1..10 呈 work+320+32*j，j=10 窗口尾越 0x1F400，
 * 调用方多分配代偿）。duel_exchange 敌方攻击/我方回合重建分屏两处消费。 */
static void duel_compose_mirror(int target, const uint8_t *tpose,
                                const uint8_t *tai, uint8_t *frame,
                                uint8_t *work, const uint8_t *bg)
{
    uint8_t *vram = vram_base();
    int i;

    for (i = 1; i < 10; i++) {
        if (s_bg_layers[i % 3])
            rle_decode_frame(s_bg_layers[i % 3], 0, 50, work + 320, 640, -1);
        blit_rows(vram, 320, work + 32 * i, 640, 320, 200);
    }
    memset(work, 0, 0x1F400);
    memset(frame, 0, 64000);
    if (bg)
        rle_decode_frame(bg, 0, 50, frame, 320, -1);
    if (tai)
        rle_decode_frame(tai, 164, 157, frame, 320, -1);
    duel_stamp_unit(frame, target);
    blit_rows(work + 320, 640, frame, 320, 320, 200);
    if (tpose)
        blit_frame_flat(tpose, 0, work + 320, 640, -1);
    for (i = 1; i <= 10; i++) {
        if (s_bg_layers[(i + 1) % 3])
            rle_decode_frame(s_bg_layers[(i + 1) % 3], 0, 50, work, 640, -1);
        blit_rows(vram, 320, work + 320 + 32 * i, 640, 320, 200);
    }
}

/* 0x5266C/0x52684 决斗受击震动表（duel_exchange 专供；区别于 fx 的
 * 0x5269C/0x526AC 四级表）：dx 横向像素、dy 以 pitch=400 行数计。 */
static const int s_duel_shake_dx[6] = { 0, 4, 9, 14, 18, 14 };
static const int s_duel_shake_dy[6] = { 0, 2, 4, 6, 8, 10 };

/* 0x2EBE1 duel_exchange（2026-09-08 帧协议全量重写，撤销纯数值版）：
 * anim=攻击方 FIGANI 3i+1 帧表（[0]总帧数 [1]远程标志 [2]接敌帧数，
 * 帧 [4]命中标记 [5]音效 [6]拍数 [7]层序 bit0），vpose=守方 FIGANI 3i
 * 姿态包（受击/待机循环，帧[6]=每帧拍数），work=0x1F400 双半屏、
 * frame=320x200 基帧（含双方单位面板），tai 仅传给 mirror 合成。
 * 协议（逐指令对 0x2ECC8..0x2F4D3）：
 *   回合 1（3% 两回合）；每回合 strike 先行（res[0]==0 命中），
 *   res[4] 效果3 → 反杀标志追加一回合；远程 → 接敌帧贴向守方半屏后
 *   compose 换景；主帧循环：命中帧按 hits/total 比例把显示 HP 从旧值
 *   钳降到新值并刷守方面板，命中（res[0]==0）置震动 5 级 + 守方 33 号
 *   剪影闪；每拍在 work+16040（pitch 400 冲突台）重合成：背帧 + 攻帧 +
 *   守方姿态帧（震动偏移 ±dx/±400*dy，[7]&1 定层序），末命中帧
 *   res[2] 状态→DAC0 绿闪(1,32,0)、res[1] 暴击→白闪(63,63,63)；
 *   帧尽：显示 HP==0 → 终止；仍有回合且远程==1 → 清屏重建施法者
 *   半屏（pose 帧 0 + compose/mirror 于另一背景）。结局蒙太奇
 *   （g_ending_duel_bg==1）末命中帧后即刻返回 1。返回守方显示 HP。 */
int duel_exchange(int att, int def, const uint8_t *anim, const uint8_t *vpose,
                  uint8_t *work, uint8_t *frame, const uint8_t *tai,
                  const void *sfx_pkg)
{
    uint8_t *vram = vram_base();
    uint8_t *arec, *drec;
    int rounds, counter_done = 0, hit_total = 0, disp_hp = 0;

    if (!g_ent_table || att < 0 || def < 0
        || att >= g_ent_count || def >= g_ent_count || att == def
        || !anim || !vpose || !work || !frame)
        return 0;
    arec = g_ent_table[att];
    drec = g_ent_table[def];
    rounds = fd2_rand() % 100u < 3u ? 2 : 1;

    for (int i = 0; i < anim[0]; i++)
        if (anim[fx_u32(anim + 8 + 4 * i) + 4])
            hit_total++;
    if (!hit_total)
        hit_total = 1;

    while (rounds-- > 0) {
        int hits = 0, shake = 0, glow = -1, pose_idx = 0, pose_tick = 0;
        int ranged = anim[1];
        int res[6] = { 0, 0, 0, 0, 0, 0 };
        int old_hp = 0;

        if (!g_ending_duel_bg) {
            old_hp = *(uint16_t *)(drec + 64);
            battle_strike_formula(att, def, res);
            if (!counter_done && res[4]) {
                counter_done = 1;
                rounds++;
            }
        }
        disp_hp = old_hp;

        /* 远程接敌段：帧 0..anim[2]-1 贴向守方半屏（我方攻=右带）。 */
        int fi = 0;
        if (ranged) {
            uint8_t *half = arec[6] ? work + 320 : work;
            for (; fi < anim[2]; fi++) {
                const uint8_t *fr = anim + fx_u32(anim + 8 + 4 * fi);
                if (fr[5] && sfx_pkg)
                    sfx_play(sfx_pkg, fr[5], 1);
                blit_rows(half, 640, frame, 320, 320, 200);
                blit_frame_flat(anim, fi, half, 640, -1);
                blit_rows(vram, 320, half, 640, 320, 200);
                wait_bios_ticks(fr[6]);
            }
            if (arec[6])
                duel_compose_target(def, vpose, frame, work, s_duel_bg_main);
            else
                duel_compose_mirror(def, vpose, tai, frame, work,
                                    s_duel_bg_main);
        }

        for (;;) {
            if (fi >= anim[0]) {
                if (disp_hp == 0)
                    rounds = 0;
                if (rounds > 0 && ranged == 1) {
                    memset(work, 0, 0x1F400);
                    if (arec[6]) {
                        blit_rows(work, 640, frame, 320, 320, 200);
                        blit_frame_flat(vpose, 0, work, 640, -1);
                        duel_compose_mirror(att, anim, tai, frame, work,
                                            s_duel_bg_alt);
                    } else {
                        blit_rows(work + 320, 640, frame, 320, 320, 200);
                        blit_frame_flat(vpose, 0, work + 320, 640, -1);
                        duel_compose_target(att, anim, frame, work,
                                            s_duel_bg_alt);
                    }
                }
                break;
            }
            const uint8_t *fr = anim + fx_u32(anim + 8 + 4 * fi);
            if (fr[4]) {                              /* 命中帧 */
                int hp = old_hp - res[5] * ++hits / hit_total;
                if (hp < 0)
                    hp = 0;
                disp_hp = hp;
                *(uint16_t *)(drec + 64) = (uint16_t)hp;
                if (!g_ending_duel_bg)
                    duel_stamp_unit(frame, def);
                if (res[0] == 0) {                    /* 命中 → 震动+剪影闪 */
                    shake = 5;
                    glow = 33;
                }
                if (fr[5] && sfx_pkg)
                    sfx_play(sfx_pkg, fr[5], 1);
            } else if (fr[5] && sfx_pkg) {
                sfx_play(sfx_pkg, fr[5], 1);
            }
            for (int t = 0; t < fr[6]; t++) {         /* 每拍冲突台重合成 */
                int dy = ranged ? 0 : s_duel_shake_dy[shake];
                uint8_t *stage = work + 16040;        /* pitch 400 冲突台 */
                int front = fr[7] & 1;

                blit_rows(stage, 400, frame, 320, 320, 200);
                if (arec[6]) {
                    if (front)
                        blit_frame_flat(anim, fi, stage, 400, -1);
                    blit_frame_flat(vpose, pose_idx,
                                    stage - s_duel_shake_dx[shake] - 400 * dy,
                                    400, glow);
                    if (!front)
                        blit_frame_flat(anim, fi, stage, 400, -1);
                } else {
                    if (!front)
                        blit_frame_flat(anim, fi, stage, 400, -1);
                    blit_frame_flat(vpose, pose_idx,
                                    stage + s_duel_shake_dx[shake] + 400 * dy,
                                    400, glow);
                    if (front)
                        blit_frame_flat(anim, fi, stage, 400, -1);
                }
                blit_rows(vram, 320, stage, 400, 320, 200);
                if (fr[4] == 1 && hits == hit_total) {
                    if (res[2]) {                     /* 状态附加 → 绿闪 */
                        dac_write(0, 1, 32, 0);
                        delay_ms(20);
                        dac_write(0, 0, 0, 0);
                    }
                    if (res[1]) {                     /* 暴击 → 白闪 */
                        dac_write(0, 63, 63, 63);
                        delay_ms(20);
                        dac_write(0, 0, 0, 0);
                        delay_ms(40);
                    }
                }
                int repeat = vpose[fx_u32(vpose + 8 + 4 * pose_idx) + 6];
                if (++pose_tick >= repeat) {
                    pose_tick = 0;
                    if (++pose_idx >= vpose[0])
                        pose_idx = 0;
                }
                if (shake)
                    shake--;
                glow = -1;
                wait_bios_ticks(1);
            }
            if (g_ending_duel_bg == 1 && hits == hit_total)
                return 1;
            fi++;
        }
    }
    return disp_hp;
}

/* 0x2E2B0 duel_scene_load（2026-09-08 全量落地，撤销 battle.c 无画面占位
 * ——用户报障"普通攻击秒退"的根因）：资源选择（TAI/BG 双拣 + FIGANI
 * 3i/3i+1 四包）→ 基帧合成（单位面板×2 + BG(0,50)）→ 决斗前淡出 →
 * 音效包（anim_sfx_load 双侧）→ duel_intro_slide → duel_exchange（→
 * 贴邻反击 exchange）→ 单背景模式胜利定妆帧 → 释放 → 战场恢复尾
 * （wait6 → 淡黑 → 清屏 → anim_tick_update(1) → 停音 → fade_in）。
 * 背景拣选（0x2E36..0x2E55 逐指令）：敌侧 v36/我侧 v32 互换体位——
 * 我方攻击 → 敌侧=守方；TAI 恒取我侧 mage 类（rec[32]==19 或
 * rec[31]∈{4,5} 且 rec[7]!=28）的 g_state_duel_bg[g_state]（0 也照用），
 * 否则我侧地块 info[6]；第一背景 = 敌侧 mage 类且覆盖非 0 时用覆盖、
 * 否则敌侧地块 info[6]；split（anim_att[1]）→ 第二背景=TAI 同源值 +
 * BG 0/1/2 全景三层，敌方攻击时主/副背景互换。结局蒙太奇
 * （g_ending_duel_bg）：两背景=覆盖值（att/def icon 26/54/55 → TAI 3），
 * 跳过面板/音效装载，exchange 后追加反向一轮且跳过恢复尾。 */
void duel_scene_load(int attacker, int defender)
{
    uint8_t *frame = NULL, *work = NULL;
    uint8_t *tai = NULL, *anim_att = NULL, *anim_def = NULL;
    uint8_t *pose_att = NULL, *anim_def2 = NULL;
    int icon_att, icon_def, split, bg_enemy, bg_ally, tai_id;

    if (!g_ent_table || attacker < 0 || defender < 0
        || attacker >= g_ent_count || defender >= g_ent_count
        || attacker == defender)
        return;
    uint8_t *arec = g_ent_table[attacker];
    uint8_t *drec = g_ent_table[defender];
    icon_att = arec[7];
    icon_def = drec[7];

    /* 原版此处 free 站立精灵/影带（fd2re 静态缓冲等价省略，同
     * fx_scene_play 注记）；v9 尾部 +640 代偿 compose j=9/10 越读。 */
    frame = malloc(64000);
    work = malloc(0x1F400 + 640);
    if (!frame || !work) {
        free(frame);
        free(work);
        return;
    }
    memset(frame, 0, 64000);

    /* 敌侧/我侧互换体位（0x2E395）：我方攻击 → 敌侧=守方、我侧=攻方。 */
    uint8_t *enemy_rec = arec[6] ? drec : arec;
    uint8_t *ally_rec = arec[6] ? arec : drec;
    uint8_t info_e[8], info_a[8];

    bg_enemy = g_state_duel_bg[g_state];
    if ((enemy_rec[32] != 19 && enemy_rec[31] != 4 && enemy_rec[31] != 5)
        || !bg_enemy || enemy_rec[7] == 28) {
        tile_lookup(enemy_rec[0], enemy_rec[1], info_e);
        bg_enemy = info_e[6];
    }
    tile_lookup(ally_rec[0], ally_rec[1], info_a);
    tai_id = bg_ally = g_state_duel_bg[g_state];
    if ((ally_rec[32] == 19 || ally_rec[31] == 4 || ally_rec[31] == 5)
        && ally_rec[7] != 28) {
        if (!bg_ally)
            bg_ally = info_a[6];      /* v32 回退；v13(TAI) 不回退 */
    } else {
        tai_id = bg_ally = info_a[6];
    }
    if (g_ending_duel_bg) {
        tai_id = bg_enemy = g_ending_duel_bg;
        if (icon_att == 26 || icon_att == 54 || icon_def == 55)
            tai_id = 3;
    }

    tai = dat_load_block("TAI.DAT", NULL, tai_id);
    anim_def = dat_load_block("FIGANI.DAT", NULL, 3 * icon_def);
    pose_att = dat_load_block("FIGANI.DAT", NULL, 3 * icon_att);
    anim_att = dat_load_block("FIGANI.DAT", NULL, 3 * icon_att + 1);
    if (!tai || !anim_def || !pose_att || !anim_att) {
        free(tai); free(anim_def); free(pose_att); free(anim_att);
        free(frame); free(work);
        return;
    }
    split = anim_att[1];
    palette_fade_black();
    free(s_duel_bg_main);
    s_duel_bg_main = dat_load_block("BG.DAT", NULL, bg_enemy);
    if (!g_ending_duel_bg)
        duel_stamp_unit(frame, attacker);
    if (melee_adjacent_test(attacker, defender) == 1 || g_ending_duel_bg)
        anim_def2 = dat_load_block("FIGANI.DAT", NULL, 3 * icon_def + 1);
    if (split) {
        free(s_duel_bg_alt);
        s_duel_bg_alt = dat_load_block("BG.DAT", NULL, bg_ally);
        for (int i = 0; i < 3; i++) {
            free(s_bg_layers[i]);
            s_bg_layers[i] = dat_load_block("BG.DAT", NULL, i);
        }
        if (!arec[6]) {               /* 敌方攻击：主/副背景互换 */
            uint8_t *t = s_duel_bg_main;
            s_duel_bg_main = s_duel_bg_alt;
            s_duel_bg_alt = t;
        }
        rle_decode_frame(s_duel_bg_alt, 0, 50, frame, 320, -1);
    } else {
        rle_decode_frame(s_duel_bg_main, 0, 50, frame, 320, -1);
        if (!g_ending_duel_bg)
            duel_stamp_unit(frame, defender);
    }
    if (!g_ending_duel_bg) {
        free(g_anim_sfx_data);
        g_anim_sfx_data = anim_sfx_load(anim_att);
        free(s_duel_sfx_def);
        s_duel_sfx_def = anim_sfx_load(anim_def2);
    }
    duel_intro_slide(attacker, split, pose_att, anim_def, work, frame, tai);
    int r = duel_exchange(attacker, defender, anim_att, anim_def,
                          work, frame, tai, g_anim_sfx_data);
    if (r) {
        if (melee_adjacent_test(attacker, defender) == 1 && !g_ending_duel_bg)
            duel_exchange(defender, attacker, anim_def2, pose_att,
                          work, frame, tai, s_duel_sfx_def);
    }
    if (g_ending_duel_bg) {
        g_ending_duel_bg = 1;
        duel_exchange(defender, attacker, anim_def2, pose_att,
                      work, frame, tai, s_duel_sfx_def);
    }
    if (!split && !g_ending_duel_bg) {              /* 胜利定妆帧 */
        blit_rows(work, 640, frame, 320, 320, 200);
        rle_decode_frame(tai, 164, 157, work, 640, -1);
        blit_frame_flat(anim_def, 0, work, 640, -1);
        blit_frame_flat(pose_att, 0, work, 640, -1);
        blit_rows(vram_base(), 320, work, 640, 320, 200);
    }
    free(frame);
    free(work);
    free(anim_def);
    free(pose_att);
    free(anim_att);
    free(anim_def2);
    free(s_duel_bg_main); s_duel_bg_main = NULL;
    free(tai);
    if (split) {
        free(s_duel_bg_alt); s_duel_bg_alt = NULL;
        for (int i = 0; i < 3; i++) {
            free(s_bg_layers[i]);
            s_bg_layers[i] = NULL;
        }
    }
    if (!g_ending_duel_bg) {
        /* 0x2E8E3..0x2E94E 恢复尾：等 6 tick → 决斗画面淡出 → 清屏 →
         * 重绘战场 → 停决斗音 → fade_in（缺 fade_in 曾致"攻击后黑屏
         * 卡死"——2026-09-07 探针实锤）。 */
        wait_bios_ticks(6);
        palette_fade_black();
        memset(vram_base(), 0, FD2_VRAM_SIZE);
        anim_tick_update(1);
        if (g_anim_sfx_data)
            sfx_play(g_anim_sfx_data, -1, 1);
        free(g_anim_sfx_data);
        g_anim_sfx_data = NULL;
        free(s_duel_sfx_def);
        s_duel_sfx_def = NULL;
        fade_in();
    }
}

/* 0x2D80D fx_scene_special（大招/处刑 32..35）全量重写（2026-09-08
 * 前置演出落地）：序 = TAI/BG(info[6]) + 基础帧 64000 + work 0x1F400
 * → bg(0,50) 解码 + 施法者小图 + fade 黑 → FIGANI 3i/3i+1、FDOTHER
 * effect+33 动画包、音效包（块 0x5265C[e-32]）→ intro_slide(actor,1,
 * anim,NULL) → frame_compose(3i+1,3i+1) → wait6 → 8×20px 滑入
 *（FIGANI 帧0）→ 9×30px 蓄力（动画帧0）→ 33/34 动画帧0 盖入基础帧 →
 * k=1..帧数-1 主循环（基础帧拷贝+贴帧+上屏；32/35@1=sfx_b(2,1)、
 * 33@6/34@2=sfx_a(1,1)、wait2）→ 32/35 闪白 11 步（帧=帧数-2+(m&1)、
 * m 偶发 sfx_a(m/2)、上屏 wait2 后 palette_mix(0,255,40-4m,R,G,B)）→
 * 释放 → 战场恢复：mix(0) 全闪 → 清 VRAM → anim_tick_update(1) →
 * 41 步 mix(scale=n)+delay(6) → sfx 停止/释放 → 实体结算（§14.2）。
 * g_shadow_buf2/g_shadow_buf 对原版在此重配（FDSHAP 2*ctx[0]）——
 * fd2re 战场影子常驻不打断，省略。 */
void fx_scene_special(int actor, int effect_id, int n_targets, const uint8_t *targets)
{
    int e = effect_id - 32;
    uint8_t *vram = vram_base();
    uint8_t info[8];
    uint8_t *tai = NULL, *bg = NULL, *anim = NULL, *tpkg = NULL;
    uint8_t *anim_fx = NULL, *frame = NULL, *work = NULL;
    uint8_t *arec;
    int i, j, n;

    if (g_ent_table && actor >= 0 && actor < g_ent_count
        && e >= 0 && e < 4) {
        arec = g_ent_table[actor];
        tile_lookup(arec[0], arec[1], info);
        tai = dat_load_block("TAI.DAT", NULL, info[6]);
        bg = dat_load_block("BG.DAT", NULL, info[6]);
        frame = malloc(64000);
        work = malloc(0x1F400);
        if (tai && bg && frame && work) {
            memset(frame, 0, 64000);
            rle_decode_frame(bg, 0, 50, frame, 320, -1);
            duel_stamp_unit(frame, actor);
            palette_fade_black();
            anim = dat_load_block("FIGANI.DAT", NULL, 3 * arec[7]);
            tpkg = dat_load_block("FIGANI.DAT", NULL, 3 * arec[7] + 1);
            anim_fx = dat_load_block("FDOTHER.DAT", NULL, effect_id + 33);
            free(g_fx_sfx_pkg);
            g_fx_sfx_pkg = dat_load_block("FDOTHER.DAT", NULL,
                                          s_sp_sfx_r[e]);
            duel_intro_slide(actor, 1, anim, NULL, work, frame, tai);
            duel_frame_compose(actor, effect_id, tpkg, tpkg, work, frame,
                               bg, tai);
            wait_bios_ticks(6);
            for (i = 0; i < 8; i++) {
                blit_rows(work, 640, frame, 320, 320, 200);
                if (anim)
                    blit_frame_flat(anim, 0, work + 20 * i, 640, -1);
                blit_rows(vram, 320, work, 640, 320, 200);
                wait_bios_ticks(1);
            }
            for (j = 8; j >= 0; j--) {
                blit_rows(work, 640, frame, 320, 320, 200);
                if (anim_fx)
                    blit_frame_flat(anim_fx, 0, work + 30 * j, 640, -1);
                blit_rows(vram, 320, work, 640, 320, 200);
                wait_bios_ticks(1);
            }
            if ((effect_id == 33 || effect_id == 34) && anim_fx)
                blit_frame_flat(anim_fx, 0, frame, 320, -1);
            if (anim_fx) {
                int count = anim_fx[0];

                for (i = 1; i < count; i++) {
                    memmove(work, frame, 64000);
                    blit_frame_flat(anim_fx, i, work, 320, -1);
                    blit_rows(vram, 320, work, 320, 320, 200);
                    if ((effect_id == 32 || effect_id == 35) && i == 1)
                        fx_sfx_b(2, 1);
                    else if ((effect_id == 33 && i == 6)
                             || (effect_id == 34 && i == 2))
                        fx_sfx_a(1, 1);
                    wait_bios_ticks(2);
                }
                if (effect_id == 32 || effect_id == 35) {
                    for (j = 0; j <= 10; j++) {
                        if (!(j % 2))
                            fx_sfx_a(j / 2, 1);
                        memmove(work, frame, 64000);
                        blit_frame_flat(anim_fx,
                                        count - 2 + (j & 1), work, 320, -1);
                        blit_rows(vram, 320, work, 320, 320, 200);
                        wait_bios_ticks(2);
                        palette_mix_range(0, 255, 40 - 4 * j,
                                          s_sp_sfx_r[e], s_sp_flash_g[e],
                                          s_sp_flash_b[e]);
                    }
                }
            }
        }
        free(anim_fx);
        free(tpkg);
        free(anim);
        free(work);
        free(frame);
        free(bg);
        free(tai);
        palette_mix_range(0, 255, 0, s_sp_sfx_r[e], s_sp_flash_g[e],
                          s_sp_flash_b[e]);
        memset(vram, 0, 64000);
        anim_tick_update(1);
        for (n = 0; n <= 40; n++) {
            palette_mix_range(0, 255, n, s_sp_sfx_r[e], s_sp_flash_g[e],
                              s_sp_flash_b[e]);
            delay_ms(6);
        }
        sfx_play(g_fx_sfx_pkg, -1, 1);
        free(g_fx_sfx_pkg);
        g_fx_sfx_pkg = NULL;
    }
    fx_magic_sfx_load();
    g_popup_count = 0;
    switch (effect_id) {
    case 32:
        fx_mass_strike(actor, 32, n_targets, targets);
        break;
    case 33:
        for (int t = 0; t < n_targets; t++) {
            uint8_t *rec = g_ent_table[targets[t]];
            memset(rec + 37, 0, 3);
        }
        fx_mass_damage(actor, n_targets, targets, 800);
        break;
    case 34:
        fx_drain_atk(actor, n_targets, targets);
        g_popup_count = 0;
        fx_drain_def(actor, n_targets, targets);
        g_popup_count = 0;
        fx_drain_third(actor, n_targets, targets);
        break;
    case 35:
        fx_aoe_status(actor, 26, n_targets, targets, 37);
        g_popup_count = 0;
        fx_aoe_status(actor, 22, n_targets, targets, 39);
        g_popup_count = 0;
        fx_aoe_status(actor, 27, n_targets, targets, 38);
        break;
    default:
        break;
    }
    fx_magic_sfx_stop();
}

/* 0x2CF30 fx_scene_multi_hit（全屏多段魔法 {24,28..31}）全量重写
 *（2026-09-08 VGA 演出落地）：系数 {24:15,28:20,29:12,其余 18} ×
 * ATK(+0x48)/10 = 总伤。演出序 = free 立绘缓存 → BG/TAI(info[6]) +
 * BG(pick) + BG.DAT 0/1/2 三层 → 基础帧（先盖施法者小图；28 用 pick
 * 背景 + 目标小图，其余 state 背景）→ FIGANI 3i/3i+2（施法者动画/
 * 姿态）+ g_anim_sfx_data（0x314DE）→ fade 黑 → 各目标 FIGANI 3i →
 * intro_slide(flag=effect!=28) + frame_compose → 逐目标：work 清零 +
 * VRAM 回显 work+320 →（非 28）duel_compose_target 换景 → 姿态复位 →
 * 伤害（raw=总伤-DEF → fx_damage_adjust → 钳旧 HP → 旧值回写）→
 * 姿态帧循环 j=pose[2]..帧数：帧[5]≠0 发 g_anim_sfx_data 音、帧[4]==1
 * 命中（震动=5、key=33、HP=旧-伤*命中数/段数（28 八段其余一段）、
 * 目标小图重盖入基础帧）、dwell 帧[6] 次（基础帧→work+320、目标姿态
 * @work+320-震动表、施法者姿态帧 j、上屏、wait1、震动衰减、key 复
 * -1）→ 释放 → 战场恢复尾（sprites_reload_fd2tmp + wait6 + fade 黑 +
 * 清屏 + anim_tick_update(1) + fade_in）。原版帧循环结束不再补写
 * HP——终值即最后一次命中插值（姿态包含段数个 [4]==1 帧）。 */
void fx_scene_multi_hit(int actor, int effect_id, int n_targets, const uint8_t *targets)
{
    int coefficient;
    uint8_t *vram = vram_base();
    uint8_t info[8];
    uint8_t *bg_state = NULL, *tai = NULL, *bg_pick = NULL;
    uint8_t *frame = NULL, *work = NULL, *anim = NULL, *pose = NULL;
    uint8_t *tpkg[30];
    uint8_t *arec;
    int total, present = 0;
    int i, j, k, n;

    switch (effect_id) {
    case 24: coefficient = 15; break; /* 0x2D003 */
    case 28: coefficient = 20; break; /* 0x2D01A */
    case 29: coefficient = 12; break; /* 0x2D031 */
    default: coefficient = 18; break; /* 0x2D03E (30/31) */
    }
    arec = g_ent_table[actor];
    total = coefficient * (int16_t)*(uint16_t *)(arec + 72) / 10;

    for (i = 0; i < 30; i++)
        tpkg[i] = NULL;
    /* 原版 0x2CF30 仅 free 立绘缓冲，不清缓存计数（见
     * sprites_reload_fd2tmp 头注释——多清即场上立绘顺序腐败根因）。 */
    free(g_standing_sprites);
    g_standing_sprites = NULL;

    if (g_ent_table && targets && n_targets > 0
        && actor >= 0 && actor < g_ent_count) {
        int state_bg, pick;

        tile_lookup(arec[0], arec[1], info);
        state_bg = info[6];
        pick = duel_bg_pick(n_targets, targets);
        bg_state = dat_load_block("BG.DAT", NULL, state_bg);
        tai = dat_load_block("TAI.DAT", NULL, state_bg);
        bg_pick = dat_load_block("BG.DAT", NULL, pick);
        s_bg_layers[0] = dat_load_block("BG.DAT", s_bg_layers[0], 0);
        s_bg_layers[1] = dat_load_block("BG.DAT", s_bg_layers[1], 1);
        s_bg_layers[2] = dat_load_block("BG.DAT", s_bg_layers[2], 2);
        frame = malloc(64000);
        /* +640：duel_compose_target j=9 窗口尾读越 0x1F400 达 288B
         *（原版同款 DOS 堆越读）的宿主代偿。 */
        work = malloc(0x1F400 + 640);
        if (bg_state && tai && bg_pick && frame && work) {
            memset(frame, 0, 64000);
            duel_stamp_unit(frame, actor);
            if (effect_id == 28) {
                rle_decode_frame(bg_pick, 0, 50, frame, 320, -1);
                duel_stamp_unit(frame, targets[0]);
            } else {
                rle_decode_frame(bg_state, 0, 50, frame, 320, -1);
            }
            anim = dat_load_block("FIGANI.DAT", NULL, 3 * arec[7]);
            pose = dat_load_block("FIGANI.DAT", NULL, 3 * arec[7] + 2);
            g_anim_sfx_data = anim_sfx_load(pose);
            palette_fade_black();
            for (i = 0; i < n_targets && i < 30; i++)
                tpkg[i] = dat_load_block("FIGANI.DAT", NULL,
                                         3 * g_ent_table[targets[i]][7]);
            duel_intro_slide(actor, effect_id != 28, anim, tpkg[0], work,
                             frame, tai);
            duel_frame_compose(actor, effect_id, pose, tpkg[0], work, frame,
                               bg_state, tai);
            present = 1;
        }
    }

    for (n = 0; n < n_targets; n++) {
        int target = targets[n];
        uint8_t *rec = g_ent_table[target];
        int old_hp = (int16_t)*(uint16_t *)(rec + 64);
        int def = (int16_t)*(uint16_t *)(rec + 74);
        int raw = total - def;

        /* fx_damage_adjust performs the native random roll, HP write and
         * experience side effect.  Its result is the amount used by the
         * multi-hit interpolation, capped at the pre-hit HP; the caller
         * then rewinds HP to old_hp (native 0x2D481..0x2D4AD). */
        int damage = fx_damage_adjust(target, raw);
        if (damage > old_hp)
            damage = old_hp;
        *(uint16_t *)(rec + 64) = (uint16_t)old_hp;

        if (!present || n >= 30 || !tpkg[n] || !pose) {
            /* 无资源回退：直接写终值（原版无此路径）。 */
            *(uint16_t *)(rec + 64) = (uint16_t)(old_hp - damage);
            continue;
        }
        {
            int segs = effect_id == 28 ? 8 : 1;
            int hits = 0, shake = 0, key = -1;

            memset(work, 0, 0x1F400);
            blit_rows(work + 320, 640, vram, 320, 320, 200);
            if (effect_id != 28)
                duel_compose_target(target, tpkg[n], frame, work, bg_pick);
            unit_pose_step(tpkg[n], 0, work, 320);
            for (j = pose[2]; j < pose[0]; j++) {
                const uint8_t *fr = pose + fx_u32(pose + 8 + 4 * j);

                if (fr[5])
                    sfx_play(g_anim_sfx_data, fr[5], 1);
                if (fr[4] == 1) {
                    shake = 5;
                    key = 33;
                    hits++;
                    *(uint16_t *)(rec + 64) = (uint16_t)
                        (old_hp - damage * hits / segs);
                    duel_stamp_unit(frame, target);
                }
                for (k = 0; k < fr[6]; k++) {
                    blit_rows(work + 320, 640, frame, 320, 320, 200);
                    unit_pose_step(tpkg[n], key,
                                   work + 320 - s_mh_shake[shake], 640);
                    blit_frame_flat(pose, j, work + 320, 640, -1);
                    blit_rows(vram, 320, work + 320, 640, 320, 200);
                    wait_bios_ticks(1);
                    if (shake)
                        shake--;
                    key = -1;
                }
            }
        }
    }

    for (i = 0; i < n_targets && i < 30; i++)
        free(tpkg[i]);
    free(bg_pick);
    free(tai);
    free(bg_state);
    free(frame);
    free(work);
    free(anim);
    free(pose);
    free(s_bg_layers[0]); s_bg_layers[0] = NULL;
    free(s_bg_layers[1]); s_bg_layers[1] = NULL;
    free(s_bg_layers[2]); s_bg_layers[2] = NULL;
    sfx_play(g_anim_sfx_data, -1, 1);
    free(g_anim_sfx_data);
    g_anim_sfx_data = NULL;    /* 原版留悬垂指针；宿主置空防复用 */
    if (present) {
        sprites_reload_fd2tmp();
        wait_bios_ticks(6);
        palette_fade_black();
        memset(vram, 0, 64000);
        anim_tick_update(1);
        fade_in();
    }
}

/* 结局蒙太奇表（@0x526E9/0x526FD/0x52711，各 20B，EXE 逐字嵌入）*/
static const uint8_t g_ending_duel_icons_a[20] = {
    0x33,0x6E,0x13,0x69,0x36,0x75,0x1E,0x7B,0x27,0x7F,
    0x40,0x51,0x34,0x7D,0x1A,0x73,0x29,0x5B,0x1F,0x7E,
};
static const uint8_t g_ending_duel_icons_b[20] = {
    0x67,0x14,0x53,0x1C,0x7C,0x26,0x5D,0x22,0x70,0x2C,
    0x56,0x35,0x50,0x37,0x78,0x24,0x6A,0x3C,0x7A,0x32,
};
static const uint8_t g_ending_duel_bgs[20]     = {
    0x04,0x03,0x33,0x0E,0x19,0x12,0x28,0x35,0x16,0x18,
    0x1C,0x11,0x1E,0x1F,0x32,0x21,0x22,0x34,0x24,0x2F,
};

/* 0x31529 ending_sequence_play（文档相位序列重建，2026-09-05）：
 * blk54 结局帧 → 闪白循环 → 40 帧会聚 → 200 帧四相位 →
 * ending_anim_part2 → 20 轮蒙太奇（duel_scene_load）→ blk59 终图 →
 * ending_text_show（按 g_state==26 分套）。各段像素细节未逐帧取证
 *（结构级，挂号）。 */
void ending_sequence_play(void)
{
    void *blk = dat_load_block("FDOTHER.DAT", NULL, 54);

    if (blk) {
        blit_frame_flat(blk, 0, vram_base(), 320, -1);
        free(blk);
    }
    wait_bios_ticks(9);
    for (int i = 0; i < 40; i++) {           /* 会聚段 */
        anim_tick_update(0);
    }
    for (int i = 0; i < 200; i++) {          /* 四相位段 */
        anim_tick_update(0);
    }
    ending_anim_part2();
    for (int i = 0; i < 20; i++) {           /* 决斗蒙太奇 */
        g_state = g_ending_duel_bgs[i];
        g_ending_duel_bg = g_ending_duel_bgs[i];   /* 0x31ae4：结局标志=TAI 背景id */
        duel_scene_load(g_ending_duel_icons_a[i], g_ending_duel_icons_b[i]);
    }
    blk = dat_load_block("FDOTHER.DAT", NULL, 59);
    if (blk) {
        blit_frame_flat(blk, 0, vram_base(), 320, -1);
        free(blk);
    }
    ending_text_show(g_state == 26 ? 0 : 1, 0);
}

static uint16_t ani_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ani_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* sub_36FF4@0x36FF4 dispatches an opcode byte followed by one payload.
 * palette_work is ANI-local: the handlers never write it to the VGA DAC. */
static void ani_decode_ops(uint16_t count, const uint8_t *data,
                           uint8_t *palette_work)
{
    uint8_t *screen = vram_base();
    const uint8_t *p = data;

    while (count--) {
        uint8_t opcode = *p++;
        size_t out, records;

        switch (opcode) {
        case 0:
            memset(palette_work, *p++, 768);
            break;
        case 1:
            memcpy(palette_work, p, 768);
            p += 768;
            break;
        case 2:
            for (out = 0; out != 768;) {
                uint8_t tag = *p++;
                if ((tag & 0xC0) == 0xC0) {
                    size_t run = tag & 0x3F;
                    memset(palette_work + out, *p++, run);
                    out += run;
                } else {
                    palette_work[out++] = tag;
                }
            }
            break;
        case 3:
            records = *p++;
            while (records--) {
                uint8_t entry = *p++;
                uint8_t size = *p++;
                memcpy(palette_work + 3 * entry, p, size);
                p += size;
            }
            break;
        case 4:
            memset(screen, *p++, FD2_VRAM_SIZE);
            break;
        case 5:
            memcpy(screen, p, FD2_VRAM_SIZE);
            p += FD2_VRAM_SIZE;
            break;
        case 6:
            for (out = 0; out != FD2_VRAM_SIZE;) {
                uint8_t tag = *p++;
                if ((tag & 0xC0) == 0xC0) {
                    size_t run = tag & 0x3F;
                    memset(screen + out, *p++, run);
                    out += run;
                } else {
                    screen[out++] = tag;
                }
            }
            break;
        case 7:
            records = ani_u16(p);
            p += 2;
            while (records--) {
                uint16_t offset = ani_u16(p);
                p += 2;
                screen[offset] = *p++;
            }
            break;
        case 8:
            records = ani_u16(p);
            p += 2;
            while (records--) {
                uint16_t offset = ani_u16(p);
                uint8_t size;
                p += 2;
                size = *p++;
                memset(screen + offset, *p++, size);
            }
            break;
        case 9:
            records = ani_u16(p);
            p += 2;
            while (records--) {
                uint16_t offset = ani_u16(p);
                uint8_t size;
                p += 2;
                size = *p++;
                memcpy(screen + offset, p, size);
                p += size;
            }
            break;
        }
    }
}

void ani_play(int anim_id, int frame_delay_ms, int skippable)
{
    uint8_t directory[8], header[173], record[8];
    uint8_t *frame_data, *palette_work;
    void *sound_pkg = NULL;
    const uint8_t *img;
    long size, pos;
    uint32_t anim_start;
    int frame_count;

    kbd_flush();
    if (anim_id == 1)
        sound_pkg = dat_load_block("FDOTHER.DAT", NULL, 78);
    palette_work = malloc(768);
    frame_data = malloc(FD2_VRAM_SIZE);
    if (!palette_work || !frame_data)
        dat_load_fatal("ANI.DAT", anim_id);

    /* 原版（0x20421，2026-09-08 反核对定案）：fopen 一次 + 帧循环内
     * 逐帧 fread——fd2re 此前实现即忠实，"片尾→菜单偶发卡顿"是宿主
     * Windows 冷页缓存/杀软对逐帧 I/O 的偶发延迟（DOS 上经 SMARTDRV
     * 命中为内存拷贝），非重建偏差。宿主等价修复：ANI.DAT 经
     * dat_file_image 整读驻留，fseek/fread 序列改镜像游标 memcpy，
     * 时序与数据不变；截断流原版 fread 短读静默续跑，此处提前止。 */
    img = dat_file_image("ANI.DAT", &size);
    if (!img)
        dat_load_fatal("ANI.DAT", anim_id);
    pos = 6L + 4L * anim_id;
    if (pos + (long)sizeof(directory) > size)
        dat_load_fatal("ANI.DAT", anim_id);
    memcpy(directory, img + pos, sizeof(directory));
    anim_start = ani_u32(directory);
    if ((long)anim_start + (long)sizeof(header) > size)
        dat_load_fatal("ANI.DAT", anim_id);
    memcpy(header, img + anim_start, sizeof(header));
    frame_count = (int16_t)ani_u16(header + 165);
    pos = (long)anim_start + sizeof(header);

    for (int frame = 0; frame < frame_count; frame++) {
        uint16_t data_size, opcode_count;

        if (pos + (long)sizeof(record) > size)
            break;                      /* 宿主护栏：截断流提前止 */
        memcpy(record, img + pos, sizeof(record));
        pos += sizeof(record);
        data_size = ani_u16(record);
        opcode_count = ani_u16(record + 2);
        if (data_size > FD2_VRAM_SIZE || pos + (long)data_size > size)
            break;   /* 宿主护栏：u16 size 上限 65535 可超 64000 缓冲
                      *（原版 fread 同样会溢出；实测最大帧 27856B 不触发）*/
        memcpy(frame_data, img + pos, data_size);
        pos += data_size;
        ani_decode_ops(opcode_count, frame_data, palette_work);
        if (anim_id == 1 && frame == 0)
            sfx_play(sound_pkg, 0, 1);
        delay_ms(frame_delay_ms);
        if (skippable && kbd_key_avail())
            break;
        kbd_flush();
    }

    free(palette_work);
    free(frame_data);
    if (sound_pkg) {
        sfx_play(sound_pkg, -1, 1);
        free(sound_pkg);
    }
}

/* 0x4EBFF（2026-09-07 反汇编再修正）：内循环 = call rle_pixel_next +
 * stosb——逐像素 RLE 解码后**不透明**写入（0 也写）。此前"只读帧头
 * 平铺 0 号色"是反编译丢失 AL 返回值的误读；帧 20/21/22 实测 RLE
 * 精确解到 w*h 且全非零（真实面板内容，非黑底）。 */
static void stamp_frame_flat0(int frame, uint8_t *dst, int pitch)
{
    const uint8_t *pkg = (const uint8_t *)g_death_fx_pkg;
    if (!pkg)
        return;
    const uint8_t *fr = pkg + (uint32_t)(pkg[6 + 4 * frame]
         | ((uint32_t)pkg[7 + 4 * frame] << 8)
         | ((uint32_t)pkg[8 + 4 * frame] << 16)
         | ((uint32_t)pkg[9 + 4 * frame] << 24));
    int w = fr[0] | (fr[1] << 8);
    int h = fr[2] | (fr[3] << 8);
    const uint8_t *p = fr + 4;
    uint8_t color = 0, repeat = 0;

    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (repeat) {
                repeat--;
            } else {
                uint8_t b = *p++;
                if (b <= 0xC0) {
                    color = b;
                } else {
                    repeat = (uint8_t)(b - 0xC1);
                    color = *p++;
                }
            }
            dst[c] = color;             /* 不透明：解码色直写 */
        }
        dst += pitch;
    }
}

/* 0x4ED0B raw_frame_stamp：未压缩帧 = [u16 w][u16 h] + 裸像素，
 * 每行 memcpy w 字节步进 pitch（blk5 帧 23-30 实测 len==4+w*h）。
 * 0x1685C 是它的 package 偏移包装（唯一消费者 = bar_stamp3）。 */
static void raw_frame_stamp(const uint8_t *frame, uint8_t *dst, int pitch)
{
    int w = frame[0] | (frame[1] << 8);
    int h = frame[2] | (frame[3] << 8);

    for (int r = 0; r < h; r++) {
        memcpy(dst, frame + 4 + (size_t)r * w, (size_t)w);
        dst += pitch;
    }
}

static const uint8_t *blk5_frame(int frame)
{
    const uint8_t *pkg = (const uint8_t *)g_death_fx_pkg;
    if (!pkg)
        return NULL;
    return pkg + (uint32_t)(pkg[6 + 4 * frame]
         | ((uint32_t)pkg[7 + 4 * frame] << 8)
         | ((uint32_t)pkg[8 + 4 * frame] << 16)
         | ((uint32_t)pkg[9 + 4 * frame] << 24));
}

/* 0x17D6F bar_stamp3(dst, pitch, count, base)（2026-09-06 修正：
 * 条形帧 23-30 是 RAW 格式，走 raw_frame_stamp 而非 RLE 解码）：
 * count>0 → 帧 base（头）@0 + 帧 base+1（身）@1..count-1 + 帧
 * base+2（尾）@count；count==0 → 帧 29（空身）@1..101 + 帧 30
 * （空尾）@102。帧基：23=HP 系、26=MP 系。 */
static void bar_stamp3(uint8_t *dst, int pitch, int count, int base)
{
    int i;

    if (count > 0) {
        raw_frame_stamp(blk5_frame(base), dst, pitch);
        for (i = 1; i < count; i++)
            raw_frame_stamp(blk5_frame(base + 1), dst + i, pitch);
        raw_frame_stamp(blk5_frame(base + 2), dst + i, pitch);
    } else {
        for (i = 1; i <= 101; i++)
            raw_frame_stamp(blk5_frame(29), dst + i, pitch);
        raw_frame_stamp(blk5_frame(30), dst + i, pitch);
    }
}

/* 0x18795 bar_stamp_ratio(dst, pitch, base, cur, max)：
 * max==0 → 不绘；cur==0 → count=0（空条）；否则 101*cur/max+1
 * → bar_stamp3。（0x187A4/0x187AB 双门复核：旧实现 max==0 误画
 * 空条、cur==0 误得 1，2026-09-08 修正） */
static void bar_stamp_ratio(uint8_t *dst, int pitch, int base,
                            int cur, int max)
{
    if (!max)
        return;
    bar_stamp3(dst, pitch, cur ? 101 * cur / max + 1 : 0, base);
}

/* 0x187D6 number_stamp_digits(dst, pitch, val, base, len)：
 * len==3 且 val>999 → 单帧 base+10（MAX）；len==2 且 val>99 → 帧 93；
 * 否则 %0<len>d 逐位 package_blit_frame(帧 = base + 数字, @dst+6*i)。 */
void number_stamp_digits(uint8_t *dst, int pitch, int val,
                                int base, int len)
{
    char fmt[8], buf[16];

    if (len == 3 && val > 999) {
        package_blit_frame(g_death_fx_pkg, dst, pitch, base + 10);
        return;
    }
    if (len == 2 && val > 99) {
        package_blit_frame(g_death_fx_pkg, dst, pitch, 93);
        return;
    }
    if (val < 0)
        val = 0;
    sprintf(fmt, "%%0.%dd", len);
    sprintf(buf, fmt, val);
    for (int i = 0; i < len; i++)
        package_blit_frame(g_death_fx_pkg, dst + 6 * i, pitch,
                           base + (buf[i] - '0'));
}

/* 0x1875D value_stamp(dst, pitch, cur, max)：满值帧基 31/非满 42，
 * 3 位数。 */
void value_stamp(uint8_t *dst, int pitch, int cur, int max)
{
    number_stamp_digits(dst, pitch, cur, cur == max ? 31 : 42, 3);
}

/* 0x18C6D unit_bars_stamp（2026-09-05 静态全解重写，替代 memset 近似）：
 * 面板底图（blk5 帧 22，0 号色蒙版）+ HP 条（@21+22*pitch，帧基 23）
 * + MP 条（@21+31*pitch，帧基 26）+ 等级数字（@132+4*pitch，rec[+33]，
 * 基 31 两位）+ HP/MP 数值（@126+21/30*pitch，满 31/非满 42 三位）
 * + 名字（@5+4*pitch，文本 id = rec[+8]+1，色 205）。
 * 2026-09-08 公开：move_range_wait_pump（entities.c）带内 456 距复用。 */
void unit_bars_stamp(uint8_t *dst, int pitch, int ent)
{
    uint8_t *rec = g_ent_table[ent];
    int hp = *(uint16_t *)(rec + 64), hpmax = *(uint16_t *)(rec + 66);
    int mp = *(uint16_t *)(rec + 68), mpmax = *(uint16_t *)(rec + 70);

    stamp_frame_flat0(22, dst, pitch);
    bar_stamp_ratio(dst + 21 + 22 * pitch, pitch, 23, hp, hpmax);
    bar_stamp_ratio(dst + 21 + 31 * pitch, pitch, 26, mp, mpmax);
    number_stamp_digits(dst + 132 + 4 * pitch, pitch, rec[33], 31, 2);
    value_stamp(dst + 126 + 21 * pitch, pitch, hp, hpmax);
    value_stamp(dst + 126 + 30 * pitch, pitch, mp, mpmax);
    /* 0x18D5B..0x18D7F 压栈定案：a14=0（打字机关）/a2=0/bg=0/fg=205。
     * 旧实参 (1,19,74,205) 是对话框族——打字机开后每 2 字经
     * char_reveal_tick→dato_frame_stamp 把跨窗存活的残留说话人块盖到
     * 活 VRAM+s_portrait_origin（首个肖像窗装载后持续复现），且每字
     * 蜂鸣+停 1 tick。 */
    text_render_box(0, 0, 0, 205, pitch, dst + 5 + 4 * pitch,
                    rec[8] + 1, g_pkg_fdtxt0);
}

/* 0x2FACD duel_stamp_unit（全解 2026-09-05；注释 2026-09-06 更正）：
 * ent[+6]：0=敌 / 1=友 NPC / 2=我方（battle_turn_end 三相印证）——
 * !=0 → dst+1451（上小位），==0 → dst+49280（下大位）；特例
 * g_state==24 && ent==17 → 49280。 */
void duel_stamp_unit(void *dst, int ent)
{
    int off = g_ent_table[ent][6] ? 1451 : 49280;

    if (g_state == 24 && ent == 17)
        off = 49280;
    unit_bars_stamp((uint8_t *)dst + off, 320, ent);
}

/* 0x2E9A8 duel_intro_slide（2026-09-05 静态重核全解重写）：
 * 敌方（rec[+6]==0）：!with_target 先 rle tai(164,157) 到 backdrop；
 * i=8..0 每步 backdrop→shadow+320、施法者帧 0 于 shadow+320-10i（左滑）、
 * !with_target 目标帧 0 于 shadow+320、上屏、palette(0,255,6i) 渐亮。
 * 我方（!=0）：j=8..0 backdrop→shadow、!with_target 目标帧 0、
 * rle tai(164,157) 于 shadow+10j、施法者帧 0 于 shadow+10j、上屏、
 * palette(6j)；循环后 tai 终解到 backdrop(320 行距)。
 * 原版循环无 wait tick（VGA 直写）；fd2re 由 VGA 扫描器观察每步写入。 */
void duel_intro_slide(int actor, int with_target, void *a_pkg, void *b_pkg,
                      void *work, void *backdrop, void *tai_pkg)
{
    uint8_t *vram = vram_base();
    uint8_t *w = (uint8_t *)work;
    uint8_t *fb = (uint8_t *)backdrop;

    if (g_ent_table[actor][6] == 0) {
        if (!with_target)
            rle_decode_frame((const uint8_t *)tai_pkg, 164, 157, fb, 320, -1);
        for (int i = 8; i >= 0; i--) {
            blit_rows(w + 320, 640, fb, 320, 320, 200);
            blit_frame_flat(a_pkg, 0, w + 320 - 10 * i, 640, -1);
            if (!with_target)
                blit_frame_flat(b_pkg, 0, w + 320, 640, -1);
            blit_rows(vram, 320, w + 320, 640, 320, 200);
            palette_apply_range(0, 255, 6 * i);
        }
    } else {
        for (int j = 8; j >= 0; j--) {
            blit_rows(w, 640, fb, 320, 320, 200);
            if (!with_target)
                blit_frame_flat(b_pkg, 0, w, 640, -1);
            rle_decode_frame((const uint8_t *)tai_pkg, 164, 157, w + 10 * j, 640, -1);
            blit_frame_flat(a_pkg, 0, w + 10 * j, 640, -1);
            blit_rows(vram, 320, w, 640, 320, 200);
            palette_apply_range(0, 255, 6 * j);
        }
        rle_decode_frame((const uint8_t *)tai_pkg, 164, 157, fb, 320, -1);
    }
}

/* 0x311E5 unit_pose_step（全解 2026-09-05）：TAI 姿态包 =
 * (w,h) 帧族（帧 = pkg + u32[pkg+8+4*帧号]，头 [0]=帧数）；
 * 帧 [6] 字节 = 重复次数；x_off==0 → 双状态复位；否则
 * blit_frame_flat(帧) 到 dst 并步进（步 ≥ 重复 → 帧号++，回绕）。 */
int unit_pose_step(void *pose_pkg, int x_off, void *dst, int pitch)
{
    const uint8_t *pkg = (const uint8_t *)pose_pkg;
    const uint8_t *frame;
    int repeat;

    if (!x_off) {
        g_pose_step = 0;
        g_pose_frame = 0;
        return 0;
    }
    blit_frame_flat(pkg, g_pose_frame, dst, pitch, x_off);
    frame = pkg + (uint32_t)(pkg[8 + 4 * g_pose_frame]
             | (pkg[9 + 4 * g_pose_frame] << 8)
             | (pkg[10 + 4 * g_pose_frame] << 16)
             | ((uint32_t)pkg[11 + 4 * g_pose_frame] << 24));
    repeat = frame[6];
    if (++g_pose_step >= repeat) {
        g_pose_step = 0;
        if (++g_pose_frame >= pkg[0])
            g_pose_frame = 0;
    }
    return repeat;
}

/* 0x31266 fx_target_transition（2026-09-13 逐指令定案，撤销结构级
 * 挂号）：换靶过渡**无任何 palette 操作**（旧版误加淡入淡出且"复原"
 * 循环方向反——返回时全屏 dim=60 近黑，多目标被击相黑屏的根因）。
 * base = work+18880；dir = 施法者 rec[6]==0 ? +1 : -1（0x312B0/
 * 0x312B9）；末帧 = tpose[0]-1。两段 i=1..4（旧目标滑出）/j=4..0
 *（新目标滑入）共 9 拍，每拍 = 基础帧回填（效果 3/7 交替带
 * base-640*alt，alt 本调用内自 0 翻转，0x31373/0x31483）→ 控制器
 * mode4 → 施法者姿态末帧 @base（0x312F7）→ 旧/新目标姿态帧 0
 * @base+dir*35*k（0x31317/0x31424）→ mode5 → blit_rows 上屏
 *（0x31353/0x31460）→ wait 1。尾跳 0x2FE0C 仅共享栈尾声
 *（add esp,0Ch/pop×4/retn），无收尾相位。 */
void fx_target_transition(int actor, void *overlay, void *pose, void *pkg_a,
                          void *work, void *backdrop, void *pkg_b, int effect_id)
{
    uint8_t *base = (uint8_t *)work + 18880;
    const uint8_t *tpose = (const uint8_t *)pose;
    int dir = g_ent_table[actor][6] == 0 ? 1 : -1;
    int last_frame = tpose ? tpose[0] - 1 : 0;
    int alt = 0;

    for (int i = 1; i < 5; i++) {
        if (effect_id == 7 || effect_id == 3) {
            alt ^= 1;
            blit_rows(base - 640 * alt, 640, backdrop, 320, 320, 200);
        } else {
            blit_rows(base, 640, backdrop, 320, 320, 200);
        }
        fx_ctrl_dispatch(effect_id, actor, (const uint8_t *)overlay,
                         base, 640, 4);
        blit_frame_flat(tpose, last_frame, base, 640, -1);
        blit_frame_flat(pkg_a, 0, base + dir * 35 * i, 640, -1);
        fx_ctrl_dispatch(effect_id, actor, (const uint8_t *)overlay,
                         base, 640, 5);
        blit_rows(vram_base(), 320, base, 640, 320, 200);
        wait_bios_ticks(1);
    }
    for (int j = 4; j >= 0; j--) {
        if (effect_id == 7 || effect_id == 3) {
            alt ^= 1;
            blit_rows(base - 640 * alt, 640, backdrop, 320, 320, 200);
        } else {
            blit_rows(base, 640, backdrop, 320, 320, 200);
        }
        fx_ctrl_dispatch(effect_id, actor, (const uint8_t *)overlay,
                         base, 640, 4);
        blit_frame_flat(tpose, last_frame, base, 640, -1);
        blit_frame_flat(pkg_b, 0, base + dir * 35 * j, 640, -1);
        fx_ctrl_dispatch(effect_id, actor, (const uint8_t *)overlay,
                         base, 640, 5);
        blit_rows(vram_base(), 320, base, 640, 320, 200);
        wait_bios_ticks(1);
    }
}

/* ==== 战斗装备菜单/单位状态页 UI 链（2026-09-07 静态定案） ============
 * 0x17E0B 布局：3x64000 缓冲（work/snap/backup）→ snap=VRAM 快照 →
 *   backup=snap → 0x17EEF 面板 + 0x184C0 道具网格画入 backup →
 *   12 帧入场（0x18409，帧 11/5 配 sfx 5）→ kbd_flush。
 * 0x17EEF 面板：DATO.DAT[ent[7]] 文本表 → panel9_grid(buf,320,5,7,5,5)
 *   → DATO 表帧 0 矩形清零 @buf+3208（立绘位）+ blk5 帧 20 @+2332
 *   （右上面板底）+ 帧 21 @+30085（底部面板底，均 stamp_frame_flat0
 *   矩形清零）→ 0x17FC0 数值区。
 * 0x17FC0 数值区（基 buf 偏移全表）：HP 条帧 23@+10758、MP 条帧 26
 *   @+16838（bar_stamp_ratio）；HP/MP 现值/满值 value_stamp @+13387/
 *   +13413/+19147/+19173；等级 rec[33]@+10717、rec[60]@+14237、
 *   rec[59]@+17757（42 色 2 位）；攻 u16@72 @+21597、防 u16@74
 *   @+25437（rec[34]/[35] 非零→119 色）、u16@62 @+17717（42）；
 *   经验 s16@76 @+21557、s16@78 @+25413（119 色 3 位）；人名
 *   rec[8]+1 @+4259（205 色）。
 * 0x184C0 道具网格：占用槽 vis 序两列——x=150*(vis/4)+42、行
 *   y=22*(vis&3)+101；图标 blk5 RAW 帧（武器 59/防具 60/其他 61，
 *   已装备 cnt&0x40 → +3）@x-29 行 y；名字 item+181 @x 行 y+2 色
 *   205/201 选中；数值区 x+68/x+93 行 y+6：武器帧 64+digits(u16@1)、
 *   防具帧 65+digits(u16@5)、消耗 w[0]==32&&w[13]==5 帧 66 /
 *   w[13]==11 帧 67 + digits(u16@14)、其他 package 帧 41（RLE）。
 * 0x18409 过渡帧：work=snap → 左条带 0x182AD(k)：i<6 k=5 否则
 *   k=5-(16i-96)（86 行，backup x2245→work x2240+k，k<0 左缘固定
 *   宽 86+k 右→左展开）；右面板 0x18312(k)：2<i<9 k=7-(16i-48)
 *   （i<=2 k=7 全展、i>=9 不画；223 宽 86 行 backup+2332→work x92
 *   行 k+i，k<0 上缘固定行 86+k）；底条 0x1839B(y)：i<6 y=16i+94
 *   （310 宽，backup+30085→work x5 行 y+i，行数 min(102,200-y)）。
 * 0x1B9DE 选择器：Up/Down ±1 环绕（sfx 0）、Left/Right ±4 跨列
 *   （sfx 75/77，Right 需 n-4>choice）；Enter/Space：mode==0 → 1、
 *   mode!=0 → weapon[13]!=0；Esc → -1。
 * 0x1BFFE item_equip_menu：布局 → loop 选择器(mode 0) → Esc/空包
 *   → 12 帧退场+free；适性过（0x1C1C3）→ 0x1C142 equip_slot →
 *   ent_recompute_derived → 临时缓冲重画面板+网格 → 回 VRAM。
 * 0x17AED unit_status_page：布局 → 等键 → 有法术（spells_collect）
 *   → 底面板 7 帧收起 → backup 底面板区重画（帧 21 清零 + 0x1CEED
 *   法术列表）→ 7 帧展开 → 等键 → 12 帧退场。
 * 0x1CEED 法术列表：两列 x=100*(i/4)+18、行 y=22*(i%4)+103；名字
 *   spell+441（205/201）；图标 blk5 帧 92 @x+50 行 y+5；MP 消耗
 *   effect_param_entry(spell)[5] digits 42/2 位 @x+73 行 y+5。
 * 三缓冲 2026-09-08 归一：原版 0x17E0B malloc 的即全局三件套
 * 0x53C5B/0x53C5F/0x53C63（menu_panel_layout），退场 0x17D48/56/64
 * free 的也是全局——fd2re 旧 statics 已撤，直接引用 g_menu_*。 */

static int menu_u16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

/* 0x182AD */
static void slide_left_panel(int k, uint8_t *work, const uint8_t *backup)
{
    int w = 86, src = 0, x = k;

    if (k < 0) {
        w = k + 86;
        src = -k;
        x = 0;
    }
    for (int i = 0; i < 86; i++)
        memmove(work + 2240 + x + 320 * i,
                backup + 2245 + src + 320 * i, (size_t)w);
}

/* 0x18312 */
static void slide_right_panel(int k, uint8_t *work, const uint8_t *backup)
{
    int rows = 86, src = 0, top = k;

    if (k < 0) {
        rows = k + 86;
        src = -k;
        top = 0;
    }
    for (int i = 0; i < rows; i++)
        memmove(work + 92 + 320 * (top + i),
                backup + 2332 + 320 * (src + i), 223);
}

/* 0x1839B */
static void slide_bottom_panel(int y, uint8_t *work, const uint8_t *backup)
{
    int rows = y + 102 >= 200 ? 200 - y : 102;

    for (int i = 0; i < rows; i++)
        memmove(work + 5 + 320 * (y + i),
                backup + 30085 + 320 * i, 310);
}

/* 0x18409 */
void menu_slide_frame(int i)
{
    memmove(g_menu_work_buf, g_menu_snap_buf, 64000);
    if (i < 6)
        slide_left_panel(5, g_menu_work_buf, g_screen_backup);
    else
        slide_left_panel(5 - (16 * i - 96), g_menu_work_buf, g_screen_backup);
    if (i >= 9 || i <= 2) {
        if (i <= 2)
            slide_right_panel(7, g_menu_work_buf, g_screen_backup);
    } else {
        slide_right_panel(7 - (16 * i - 48), g_menu_work_buf, g_screen_backup);
    }
    if (i < 6)
        slide_bottom_panel(16 * i + 94, g_menu_work_buf, g_screen_backup);
    memmove(vram_base(), g_menu_work_buf, 64000);
}

/* 0x4EBFF 指针版（2026-09-07 语义更正）：不透明 RLE 解码 blit。
 * 单位面板的调用点传 DATO 表帧0（80x80 肖像）——这就是 §13.24 挂号
 * 的"DATO 肖像本体绘制"真身。 */
static void stamp_rect0(const uint8_t *frame, uint8_t *dst, int pitch)
{
    int w = frame[0] | (frame[1] << 8);
    int h = frame[2] | (frame[3] << 8);
    const uint8_t *p = frame + 4;
    uint8_t color = 0, repeat = 0;

    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (repeat) {
                repeat--;
            } else {
                uint8_t b = *p++;
                if (b <= 0xC0) {
                    color = b;
                } else {
                    repeat = (uint8_t)(b - 0xC1);
                    color = *p++;
                }
            }
            dst[c] = color;             /* 不透明：解码色直写 */
        }
        dst += pitch;
    }
}

/* 0x17FC0 */
void unit_stats_stamp(int unit, uint8_t *buf)
{
    const uint8_t *r = g_ent_table[unit];
    int hp = menu_u16(r + 64), hpmax = menu_u16(r + 66);
    int mp = menu_u16(r + 68), mpmax = menu_u16(r + 70);

    bar_stamp_ratio(buf + 10758, 320, 23, hp, hpmax);
    bar_stamp_ratio(buf + 16838, 320, 26, mp, mpmax);
    value_stamp(buf + 13387, 320, hp, hpmax);
    value_stamp(buf + 13413, 320, hpmax, hpmax);
    value_stamp(buf + 19147, 320, mp, mpmax);
    value_stamp(buf + 19173, 320, mpmax, mpmax);
    number_stamp_digits(buf + 10717, 320, r[33], 42, 2);
    number_stamp_digits(buf + 14237, 320, r[60], 42, 2);
    number_stamp_digits(buf + 17757, 320, r[59], 42, 2);
    number_stamp_digits(buf + 21597, 320, menu_u16(r + 72),
                        r[34] ? 119 : 42, 3);
    number_stamp_digits(buf + 25437, 320, menu_u16(r + 74),
                        r[35] ? 119 : 42, 3);
    number_stamp_digits(buf + 17717, 320, menu_u16(r + 62), 42, 3);
    /* 0x1816C/0x181AD：经/移 s16@76、s16@78 色基同源 rec[36]?119:42
     *（fd2re 旧实现写死 119、@78 偏移误 25413——2026-09-06 对齐原版
     * 0x18191/0x181AD：偏移 21557/25397）。 */
    number_stamp_digits(buf + 21557, 320,
                        (int16_t)menu_u16(r + 76), r[36] ? 119 : 42, 3);
    number_stamp_digits(buf + 25397, 320,
                        (int16_t)menu_u16(r + 78), r[36] ? 119 : 42, 3);
    /* 0x181DA 族压栈：(a14=0, a2=0, bg=0, 76, 205) —— 列表族即时渲染。 */
    text_render_box(0, 0, 0, 205, 320, buf + 4259,
                    r[8] + 1, g_pkg_fdtxt0);
    text_render_box(0, 0, 0, 205, 320, buf + 4371,
                    r[31] + 140, g_pkg_fdtxt0);     /* 系 */
    text_render_box(0, 0, 0, 205, 320, buf + 4411,
                    r[32] + 150, g_pkg_fdtxt0);     /* 职业 */
    raw_frame_stamp(blk5_frame(r[6] ? 53 : 54),
                    buf + 9701, 320);                /* 阵营图标 */
    for (int i = 0; i < 3; i++) {
        if (r[37 + i])
            raw_frame_stamp(blk5_frame(55 + i),
                            buf + 21954 + 35 * i, 320);  /* 法术系图标 */
    }
}

/* 0x17EEF */
static void unit_panel_draw(int unit, uint8_t *buf)
{
    const uint8_t *tbl;

    /* 0x17EF6/0x17F1C：肖像位 = 面板肖像区 3208（dword_53C67），角色
     * DATO.DAT[ent[7]] 载入说话人块（原版 g_dato_speaker_blk——此前
     * fd2re 误载 g_fdtxt_ptr_table，状态页 wait_key_anim 的帧 0/帧 3
     * 眨眼泵因此空转；浏览器侧 0x29645/0x2965E 的 origin 存取 + 129
     * 重载即为此处的恢复对）。 */
    dialog_portrait_origin_set(3208);
    tbl = dialog_speaker_reload(g_ent_table[unit][7]);
    panel9_grid(buf, 320, 5, 7, 5, 5);
    if (tbl) {
        /* DATO 表帧 0（表基 + 首字节偏移）：80x80 肖像静态本体 */
        stamp_rect0(tbl + tbl[0], buf + 3208, 320);
    }
    stamp_rect0(blk5_frame(20), buf + 2332, 320);
    stamp_rect0(blk5_frame(21), buf + 30085, 320);
    unit_stats_stamp(unit, buf);
}

/* 0x184C0 */
static void item_grid_draw(int unit, int sel, uint8_t *buf)
{
    const uint8_t *r = g_ent_table[unit];
    int vis = 0;

    for (int i = 0; i < 8; i++) {
        int cnt = (int8_t)r[10 + 2 * i];
        int x, y, item, fr, val = -1, vfr = -1;
        const uint8_t *w;

        if (cnt < 0)
            continue;
        x = 150 * (vis / 4) + 42;
        y = 22 * (vis & 3) + 101;
        item = r[11 + 2 * i];
        w = weapon_table_entry(item);
        if (!w)
            continue;      /* 宿主护栏（原版无界不查）：越界 id 跳过不画 */
        fr = w[0] < 0x15 ? 59 : w[0] < 0x20 ? 60 : 61;
        if (cnt & 0x40)
            fr += 3;
        raw_frame_stamp(blk5_frame(fr), buf + 320 * y + x - 29, 320);
        /* 0x18587 压栈：列表族 (0,0,0,201 选中/205)。 */
        text_render_box(0, 0, 0, i == sel ? 201 : 205, 320,
                        buf + 320 * (y + 2) + x, item + 181,
                        g_pkg_fdtxt0);
        /* 数值区：图标 @x+68、数字 @x+93，行 y+6 */
        if (w[0] < 0x15) {
            vfr = 64;
            val = menu_u16(w + 1);
        } else if (w[0] < 0x20) {
            vfr = 65;
            val = menu_u16(w + 5);
        } else if (w[0] == 32 && w[13] == 5) {
            vfr = 66;
            val = menu_u16(w + 14);
        } else if (w[0] == 32 && w[13] == 11) {
            vfr = 67;
            val = menu_u16(w + 14);
        } else {
            package_blit_frame(g_death_fx_pkg,
                               buf + 320 * (y + 6) + x + 68, 320, 41);
        }
        if (vfr >= 0) {
            raw_frame_stamp(blk5_frame(vfr),
                            buf + 320 * (y + 6) + x + 68, 320);
            number_stamp_digits(buf + 320 * (y + 6) + x + 93, 320,
                                val, 42, 3);
        }
        vis++;
    }
}

/* 0x1B9DE：mode==0 确认即过；mode!=0 需 weapon[13]!=0（可用道具） */
static int item_grid_pick(int unit, int mode)
{
    const uint8_t *r = g_ent_table[unit];
    int n = 0, key;

    for (int i = 0; i < 8; i++)
        if ((int8_t)r[10 + 2 * i] >= 0)
            n++;
    item_grid_draw(unit, g_menu_choice, vram_base());
    key = wait_key_anim(0);   /* 0x1BA32：原版即 wait_key_anim(0)（带
                               * DATO 肖像泵 + idle_pump 的原版调用点），
                               * 非裸 input_wait_key（2026-09-07 勘误） */
    switch (key) {
    case SCAN_UP:                                    /* Up */
        sfx_play(g_pkg_fdother_31, 0, 1);
        g_menu_choice = g_menu_choice ? g_menu_choice - 1 : n - 1;
        return 0;
    case SCAN_DOWN:                                    /* Down */
        sfx_play(g_pkg_fdother_31, 0, 1);
        g_menu_choice = g_menu_choice == n - 1 ? 0 : g_menu_choice + 1;
        return 0;
    case SCAN_LEFT:                                    /* Left */
        if (g_menu_choice >= 4) {
            sfx_play(g_pkg_fdother_31, 75, 1);
            g_menu_choice -= 4;
        }
        return 0;
    case SCAN_RIGHT:                                    /* Right */
        if (g_menu_choice <= 3 && n - 4 > g_menu_choice) {
            sfx_play(g_pkg_fdother_31, 77, 1);
            g_menu_choice += 4;
        }
        return 0;
    case SCAN_ENTER: case 57:                           /* Enter / Space */
        if (!mode)
            return 1;
        {
            const uint8_t *wpick = weapon_table_entry(r[11 + 2 * g_menu_choice]);

            return wpick && wpick[13] != 0;  /* 宿主护栏（原版无界不查） */
        }
    case SCAN_ESC:                                   /* Esc */
        return -1;
    }
    return 0;
}

/* 0x17E0B */
static int menu_panel_layout(int unit)
{
    g_menu_work_buf = malloc(64000);
    g_menu_snap_buf = malloc(64000);
    g_screen_backup = malloc(64000);
    if (!g_menu_work_buf || !g_menu_snap_buf || !g_screen_backup) {
        free(g_menu_work_buf); free(g_menu_snap_buf); free(g_screen_backup);
        g_menu_work_buf = g_menu_snap_buf = g_screen_backup = NULL;
        return -1;
    }
    memmove(g_menu_snap_buf, vram_base(), 64000);
    memmove(g_screen_backup, g_menu_snap_buf, 64000);
    unit_panel_draw(unit, g_screen_backup);
    item_grid_draw(unit, -1, g_screen_backup);
    for (int i = 11; i >= 0; i--) {
        if (i == 11 || i == 5)
            sfx_play(g_pkg_fdother_31, 5, 1);
        menu_slide_frame(i);
    }
    kbd_flush();
    return 0;
}

static void menu_panel_free(void)
{
    free(g_menu_work_buf);
    free(g_menu_snap_buf);
    free(g_screen_backup);
    g_menu_work_buf = g_menu_snap_buf = g_screen_backup = NULL;
}

/* 0x1CEED */
void spell_list_draw(int unit, int sel, uint8_t *buf)
{
    uint8_t spells[32];
    int n = spells_collect(unit, spells);

    for (int i = 0; i < n; i++) {
        int x = 100 * (i / 4) + 18;
        int y = 22 * (i % 4) + 103;

        /* 0x1CF4A 压栈：列表族 (0,0,0,201 选中/205)。 */
        text_render_box(0, 0, 0, i == sel ? 201 : 205, 320,
                        buf + 320 * y + x, spells[i] + 441,
                        g_pkg_fdtxt0);
        raw_frame_stamp(blk5_frame(92), buf + 320 * (y + 5) + x + 50, 320);
        number_stamp_digits(buf + 320 * (y + 5) + x + 73, 320,
                            effect_param_entry(spells[i])[5], 42, 2);
    }
}

/* 0x1BFFE item_equip_menu：战斗/据点共享装备菜单（战斗调用方
 * sub_1BBDC 行动菜单装备项；据点 shop_item_use@0x28EFE）。 */
int item_equip_menu(int unit)
{
    if (menu_panel_layout(unit) != 0)
        return -1;
    g_menu_choice = 0;
    for (;;) {
        int pick;

        do
            pick = item_grid_pick(unit, 0);
        while (!pick);
        if (pick == -1 || sub_1B8A6(unit) == 0)
            break;
        if (weapon_affinity_check(unit,
                (uint8_t)sub_1B722(unit, g_menu_choice))) {
            equip_slot(unit, g_menu_choice);
            ent_recompute_derived(unit);
            /* 原版 0x1C094 重画 scratch 为 g_shadow_buf2 前 64000 字节
             *（fd2re 不维护该对，duel.c 1442 注）；用 work 代偿等价：
             * work 仅在滑帧期作暂存，滑帧起手即被 snap 覆盖。 */
            memmove(g_menu_work_buf, vram_base(), 64000);
            unit_panel_draw(unit, g_menu_work_buf);
            item_grid_draw(unit, -1, g_menu_work_buf);
            memmove(vram_base(), g_menu_work_buf, 64000);
        }
    }
    for (int i = 0; i <= 11; i++)
        menu_slide_frame(i);
    memmove(vram_base(), g_menu_snap_buf, 64000);
    menu_panel_free();
    return 0;
}

/* 0x17AED unit_status_page：状态页 + 法术列表子面板 */
void unit_status_page(int unit)
{
    if (menu_panel_layout(unit) != 0)
        return;
    wait_key_anim(0);
    {
        uint8_t probe[32];

        if (spells_collect(unit, probe) > 0) {
        sfx_play(g_pkg_fdother_31, 6, 1);
        for (int i = 0; i <= 6; i++) {
            memmove(g_menu_work_buf, g_menu_snap_buf, 64000);
            slide_left_panel(5, g_menu_work_buf, g_screen_backup);
            slide_right_panel(7, g_menu_work_buf, g_screen_backup);
            slide_bottom_panel(16 * i + 94, g_menu_work_buf, g_screen_backup);
            memmove(vram_base(), g_menu_work_buf, 64000);
        }
        memmove(g_menu_work_buf, g_menu_snap_buf, 64000);
        slide_left_panel(5, g_menu_work_buf, g_screen_backup);
        slide_right_panel(7, g_menu_work_buf, g_screen_backup);
        memmove(vram_base(), g_menu_work_buf, 64000);
        stamp_rect0(blk5_frame(21), g_screen_backup + 30085, 320);
        spell_list_draw(unit, -1, g_screen_backup);
        sfx_play(g_pkg_fdother_31, 5, 1);
        for (int j = 6; j >= 0; j--) {
            memmove(g_menu_work_buf, g_menu_snap_buf, 64000);
            slide_left_panel(5, g_menu_work_buf, g_screen_backup);
            slide_right_panel(7, g_menu_work_buf, g_screen_backup);
            slide_bottom_panel(16 * j + 94, g_menu_work_buf, g_screen_backup);
            memmove(vram_base(), g_menu_work_buf, 64000);
        }
        wait_key_anim(0);
        }
    }
    for (int k = 0; k <= 11; k++) {
        if (!k || k == 7)
            sfx_play(g_pkg_fdother_31, 6, 1);
        menu_slide_frame(k);
    }
    memmove(vram_base(), g_menu_snap_buf, 64000);
    menu_panel_free();
}

/* ==== 战斗物品/法术选择流程（0x1B932 / 0x1D51D / 0x1CFF0，2026-09-08
 * §13.25） ============================================================ */

/* 0x1B932 inventory_pick_slot(unit, mode)：物品格选择整页。
 * menu 布局（滑入）→ item_grid_pick 循环 → 12 帧滑出 + VRAM=snap +
 * 释放。返回 1=选中（g_menu_choice=槽位），0=取消。 */
int inventory_pick_slot(int unit, int mode)
{
    if (menu_panel_layout(unit) != 0)
        return 0;
    g_menu_choice = 0;
    int r;
    do {
        r = item_grid_pick(unit, mode);
    } while (!r);
    for (int i = 0; i <= 11; i++)
        menu_slide_frame(i);
    memmove(vram_base(), g_menu_snap_buf, 64000);
    menu_panel_free();
    return r != -1;
}

/* 0x1D51D spell_menu_pick(unit)：法术页单步选择器。
 * 重画法术表（VRAM 直绘）→ 等键（wait_key_anim 泵动画）→
 * Up/Down 环绕、Left/Right ±4（音效 blk31#0）、Enter 校验
 * effect_param 非空、Esc=-1。返回 0=继续 / 1=确认 / -1=取消。 */
static int spell_menu_pick(int unit)
{
    uint8_t probe[32];

    spell_list_draw(unit, g_menu_choice, vram_base());
    int n = spells_collect(unit, probe);
    if (n <= 0)
        return -1;
    int key = wait_key_anim(0);
    switch (key) {
    case SCAN_UP:
        sfx_play(g_pkg_fdother_31, 0, 1);
        g_menu_choice = g_menu_choice ? g_menu_choice - 1 : n - 1;
        return 0;
    case SCAN_DOWN:
        sfx_play(g_pkg_fdother_31, 0, 1);
        g_menu_choice = (g_menu_choice == n - 1) ? 0 : g_menu_choice + 1;
        return 0;
    case SCAN_LEFT:
        if (g_menu_choice >= 4) {
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice -= 4;
        }
        return 0;
    case SCAN_RIGHT:
        if (n - 4 > g_menu_choice) {
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice += 4;
        }
        return 0;
    case SCAN_ENTER:
    case 57:
        /* 原版 0x1D682：spells_collect 后 effect_param_entry(spell)
         * 结果作循环退出条件（NULL=0 继续）。 */
        return effect_param_entry(probe[g_menu_choice]) != NULL;
    case SCAN_ESC:
        return -1;
    default:
        return 0;
    }
}

/* 0x1CFF0 spell_cast_flow(unit)：魔法施放全流程。
 * 法术页（滑入/选择/滑出）→ effect_param 分类目标选择：
 *   ep[3]!=0 且 spell==23 → 传送（两次 collect + mode-6 落点）；
 *   ep[3]==0 或 spell==23 → 自身/无目标（半径=g_text_busy-2，
 *     有敌→mode 4 候选格，无→mode 5 任意格）；
 *   其余 → 射程施法（ep[3] 形状收集 + ep[6] 阵营；spell 30 直线
 *     ray_targets_collect@0x149F8 真身，2026-09-08 接入）。
 * 执行：spell<9 / ==24 / >0x1B → fx_scene_play；否则 exec 表
 * （g_spell_exec_funcs@0x51D01，28 项=效果 id 9..27；fd2re
 * spell_execute 19 分支全量实现，§13.39/§13.76 逐项复核）。
 * 收尾：collect_drops + death_anim + show_results。 */
int spell_cast_flow(int unit)
{
    uint8_t targets[100], spells[32];
    int r, sel, count;

    g_menu_work_buf = (uint8_t *)malloc(64000);
    g_menu_snap_buf = (uint8_t *)malloc(64000);
    g_screen_backup = (uint8_t *)malloc(64000);
    if (!g_menu_work_buf || !g_menu_snap_buf || !g_screen_backup) {
        menu_panel_free();
        return 0;
    }
    memmove(g_menu_snap_buf, vram_base(), 64000);
    memmove(g_screen_backup, g_menu_snap_buf, 64000);
    unit_panel_draw(unit, g_screen_backup);
    spell_list_draw(unit, -1, g_screen_backup);
    for (int i = 11; i >= 0; i--)
        menu_slide_frame(i);
    g_menu_choice = 0;
    do {
        r = spell_menu_pick(unit);
    } while (!r);
    for (int i = 0; i <= 11; i++)
        menu_slide_frame(i);
    memmove(vram_base(), g_menu_snap_buf, 64000);
    menu_panel_free();
    if (r == -1)
        return -1;

    spells_collect(unit, spells);
    int sp = spells[g_menu_choice];
    const uint8_t *ep = effect_param_entry(sp);
    if (!ep)
        return 0;
    g_text_busy = ep[4] + 2;                    /* 确认半径 = ep[4]+1 */
    if (ep[3] && sp == 23) {                    /* 传送魔法 */
        count = shape_targets_collect(targets, g_cursor_x, g_cursor_y,
                                      ep[3], 1, ep[6]);
        sel = battle_move_select(ep[6], count, targets);
        field_reset_candidates();
        count = shape_targets_collect(targets, g_cursor_x, g_cursor_y,
                                      ep[4], 0, ep[6]);
        field_reset_candidates();
        if (sel != -1) {
            uint8_t focus1 = (uint8_t)(targets[0] < g_ent_count
                                       ? targets[0] : unit);
            sel = battle_move_select(6, 1, &focus1);
        }
        if (sel != -1) {
            g_warp_dest_x = (int32_t)g_cursor_x;
            g_warp_dest_y = (int32_t)g_cursor_y;
            g_text_busy = 0;
            camera_focus_ent(unit);
            g_text_busy = 1;
        }
    } else if (!ep[3] || sp == 23) {            /* 自身/无目标 */
        int rad = g_text_busy - 2;
        g_text_busy = 1;
        count = shape_targets_collect(targets, g_cursor_x, g_cursor_y,
                                      rad, 0, 0);
        sel = battle_move_select(count ? 4 : 5, count, targets);
    } else {                                    /* 射程施法 */
        count = shape_targets_collect(targets, g_cursor_x, g_cursor_y,
                                      ep[3], 0, ep[6]);
        int cx0 = (int)g_cursor_x, cy0 = (int)g_cursor_y;
        sel = battle_move_select(ep[6], count, targets);
        field_reset_candidates();
        if (sp == 30) {
            /* 0x1D35C：直线法术 30 = ray_targets_collect（aim=选中格
             * from、origin=施法前光标 to，len=ep[3]-16，ally=1 收敌方）
             * ——2026-09-08 接真身，撤销形状收集等价。 */
            count = ray_targets_collect(targets, (int)g_cursor_x,
                                        (int)g_cursor_y, cx0, cy0,
                                        ep[3] - 16, 1);
        } else {
            count = shape_targets_collect(targets, g_cursor_x, g_cursor_y,
                                          ep[4], 0, ep[6]);
        }
    }
    field_reset_candidates();
    if (sel == -1) {
        g_text_busy = 0;
        camera_focus_ent(unit);
        g_text_busy = 1;
        return 0;
    }
    g_text_busy = 0;
    anim_tick_update(0);
    if (sp < 9 || sp == 24 || sp > 0x1B) {
        fx_scene_play(unit, sp, count, targets);
    } else {
        fx_magic_sfx_load();
        fx_palette_flash(sp);                 /* 0x1D6C8 闪屏 */
        spell_execute(unit, sp, count, targets);   /* 0x51D01 exec 表 */
        fx_magic_sfx_stop();
    }
    uint8_t drops[15][3];
    int nd = battle_collect_drops(drops);
    battle_death_anim();
    battle_show_results(unit, nd, (const uint8_t *)drops);
    g_text_busy = 1;
    return 1;
}
