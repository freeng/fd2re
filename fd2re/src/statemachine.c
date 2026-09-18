/* statemachine.c — 状态机（主循环结构已验证；状态体为存根） */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "statemachine.h"
#include "states.h"
#include "startup.h"
#include "battle.h"
#include "resource.h"
#include "video.h"
#include "input.h"
#include "audio.h"
#include "entities.h"
#include "text.h"
#include "timer.h"
#include "save.h"
#include "scene.h"
#include "host.h"

int32_t  g_state;
int32_t  g_state_pending;
uint8_t  g_transition_busy = 1;    /* 0x51AAC 二进制初值 1（0x1AD0C 面板门） */

void   (*g_state_enter[FD2_STATE_COUNT])(void);
void   (*g_state_exit[FD2_STATE_COUNT])(void);
uint8_t  g_state_bgm[FD2_STATE_COUNT];
uint8_t  g_state_bgm_alt[FD2_STATE_COUNT];
uint8_t  g_state_roster_menu[FD2_STATE_COUNT];
uint8_t  g_state_config[FD2_STATE_COUNT][FD2_STATE_CFG_SZ] = {
    { 0x07, 0x09, 0x02, 0x05, 0x0E, 0x12, 0x00, 0x00, 0xFF, 0x0A, 0x0C, 0x0A, 0x0C, 0x02, 0x02, 0x0C, 0x12, 0x00, 0x00, 0xFF, 0x08, 0x0C, 0x08, 0x0A, 0x03, 0x03, 0x0A, 0x0F, 0x00, 0x00, 0xFF },
    { 0x00, 0x00, 0x54, 0x80, 0x81, 0x84, 0xA5, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x16, 0x35, 0xC0, 0xC1, 0x84, 0xFF, 0xFF },
    { 0x02, 0x01, 0x5F, 0x00, 0x01, 0x20, 0x84, 0xA5, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC1, 0xCE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x00, 0x02, 0x6A, 0x01, 0x21, 0x2D, 0x35, 0x81, 0x84, 0x85, 0xA5, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x60, 0xCE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x00, 0x03, 0x57, 0x01, 0x02, 0x14, 0x15, 0x85, 0x90, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xC1, 0xC4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC6, 0xC7, 0xC8, 0x5E, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x02, 0x04, 0x62, 0x01, 0x02, 0x14, 0x15, 0x21, 0x2C, 0x2D, 0x91, 0x86, 0xA6, 0xFF, 0xFF, 0xC0, 0xC1, 0xC4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC2, 0xCE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x02, 0x00, 0x6D, 0x01, 0x02, 0x15, 0x16, 0x21, 0x22, 0x2D, 0x35, 0x36, 0x85, 0x86, 0x91, 0xC0, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x58, 0x5C, 0x5D, 0xCD, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x00, 0x01, 0x5A, 0x02, 0x03, 0x15, 0x16, 0x2D, 0x22, 0x36, 0x86, 0x91, 0xA6, 0xFF, 0xFF, 0xC0, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x04, 0x17, 0xC2, 0xCE, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x00, 0x02, 0x65, 0x03, 0x16, 0x23, 0x2E, 0x3F, 0x87, 0x92, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC2, 0xCE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x02, 0x03, 0x70, 0x03, 0x23, 0x2E, 0x36, 0x3F, 0x87, 0x92, 0xA7, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xC1, 0xC4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x5E, 0x5F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x02, 0x04, 0x5D, 0x03, 0x04, 0x17, 0x18, 0x40, 0x88, 0x93, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC2, 0xCE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x01, 0x00, 0x5E, 0x04, 0x18, 0x24, 0x37, 0x40, 0x88, 0x93, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC2, 0xCE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x01, 0x01, 0x69, 0x05, 0x18, 0x24, 0x2F, 0x88, 0x93, 0xA8, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF, 0xC1, 0xC2, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x1E, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x01, 0x02, 0x56, 0x05, 0x19, 0x24, 0x2F, 0x89, 0x94, 0xA8, 0xAE, 0xFF, 0xFF, 0xFF, 0xFF, 0xC1, 0xC2, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0xFF, 0xC2, 0xCD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x01, 0x03, 0x61, 0x06, 0x19, 0x25, 0x30, 0x89, 0x94, 0xA8, 0xAE, 0xFF, 0xFF, 0xFF, 0xFF, 0xC1, 0xC2, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3, 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x01, 0x04, 0x6C, 0x06, 0x19, 0x25, 0x30, 0x89, 0x94, 0xA9, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC1, 0xC2, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0xFF, 0x5E, 0x5F, 0xC6, 0xC7, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x01, 0x00, 0x58, 0x10, 0x19, 0x25, 0x31, 0x38, 0x41, 0x8A, 0x95, 0xA9, 0x9C, 0xFF, 0xFF, 0xC1, 0xC2, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3, 0xCF, 0x61, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x01, 0x01, 0x64, 0x12, 0x1A, 0x26, 0x31, 0x38, 0x41, 0x8A, 0x95, 0xA9, 0x9C, 0xAF, 0xFF, 0xC1, 0xC2, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0xFF, 0x3A, 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x02, 0x02, 0x6F, 0x12, 0x1A, 0x26, 0x61, 0x38, 0x43, 0x8B, 0x96, 0xA9, 0x9D, 0xAF, 0xFF, 0xC1, 0xC2, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3, 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x01, 0x03, 0x5C, 0x12, 0x1A, 0x26, 0x61, 0x39, 0x43, 0x8B, 0x96, 0xAA, 0x9D, 0xB0, 0xFF, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0xC3, 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x01, 0x04, 0x67, 0x13, 0x1A, 0x27, 0x61, 0x39, 0x44, 0x8C, 0x97, 0xAA, 0x9E, 0xB0, 0xFF, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0x32, 0x45, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x01, 0x00, 0x68, 0x13, 0x1B, 0x27, 0x62, 0x39, 0x44, 0x8C, 0x97, 0xBD, 0x9E, 0xBC, 0xFF, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0xC3, 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0 }, { 0 }, { 0 },
    { 0x00, 0x04, 0x58, 0x09, 0x1C, 0x29, 0x32, 0x3C, 0x46, 0x8E, 0x99, 0xA1, 0xAB, 0xB1, 0xFF, 0xC2, 0xC3, 0xC4, 0xC5, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3, 0xCF, 0x28, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x00, 0x00, 0x63, 0x0A, 0x1F, 0x2A, 0x68, 0x3C, 0x69, 0x8F, 0x9A, 0xA2, 0xAB, 0xB1, 0xFF, 0xC3, 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3, 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x05, 0x11, 0x09, 0x01, 0x0F, 0x1A, 0x15, 0x02, 0x1A, 0x1B, 0xFF, 0xFF, 0x07, 0x14, 0x0B, 0x0E, 0x10, 0x12, 0x18, 0x16, 0x1E, 0x17, 0xFF, 0xFF, 0x12, 0x13, 0x18, 0x15, 0x1E, 0x19, 0xFF },
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x14, 0x0A, 0x18, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x04, 0x18, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x1E },
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x04, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x1B, 0x07, 0x03, 0x0D, 0x13, 0x16, 0x0B, 0xFF },
};

static uint8_t s_scene_shadow[456 * 336];
static uint8_t s_scene_shadow2[456 * 336];
/* 0x52363/0x52375: 3 backdrop variants x 6 scene phases. */
static const uint8_t g_sprite_pos_x_lut[18] = {
    0x1D, 0x29, 0x3B, 0x9A, 0xB6, 0x0A,
    0x5A, 0x21, 0x35, 0x94, 0xDE, 0xC4,
    0x3B, 0x0A, 0x3B, 0x82, 0xF2, 0x88,
};
static const uint8_t g_sprite_pos_y_lut[18] = {
    0x2E, 0x6D, 0xA3, 0x8B, 0x41, 0x0A,
    0x1E, 0x69, 0xA3, 0x8B, 0x55, 0x08,
    0x1A, 0x90, 0xA3, 0x96, 0x1F, 0x14,
};
/* 0x53F52 dword_53F52：场景/菜单共享动画钟（城镇 compose 与 sub_26EDA
 * 菜单等待均读写；0..3 循环，消费方各自映射 3→1）。 */
uint8_t g_scene_anim_clock;

/* FDOTHER block 10 is a single [u16 width][u16 height][RLE...] frame.
 * 0x26627 stamps it with stamp_frame_opaque（不透明 RLE，0 也写）at the
 * 456-pixel shadow surface +107020——真实覆盖层图像，非清零蒙版
 * （2026-09-07 取证更正）。 */
static void scene_overlay_stamp(const void *overlay, uint8_t *dst, int pitch)
{
    const uint8_t *frame = overlay;
    unsigned w, h;

    if (!overlay)
        return;
    /* 2026-09-07 事故修复：旧守卫 107020+456*191+456=194572 恒大于缓冲
     * 153216 —— 常量条件永真，场景路径一进即 abort（章节载入"退出"
     * 根因；START 序章走战斗合成不经此函数故独活）。正确边界按帧头
     * 实际 w/h 计算：blk10 实测 62x26 → 需 118482，充裕。原版在
     * 0x25680 缓冲内无条件 stamp，等价直写。 */
    w = frame[0] | ((unsigned)frame[1] << 8);
    h = frame[2] | ((unsigned)frame[3] << 8);
    if (!w || !h || 107020u + (size_t)pitch * (h - 1u) + w
                 > sizeof(s_scene_shadow2)) {
        fprintf(stderr, "scene: overlay %ux%u exceeds shadow bounds\n", w, h);
        return;                     /* 宿主护栏（原版无此分支） */
    }
    rle_blit_opaque(frame, dst + 107020, pitch);
}

static void scene_render_host(const void *overlay)
{
    const uint8_t *cfg = state_config_get(g_state);
    int bg = cfg[0];
    uint8_t *dst = s_scene_shadow2;

    memcpy(dst, s_scene_shadow, sizeof(s_scene_shadow));
    scene_overlay_stamp(overlay, dst, 456);
    /* 0x26674 实参 (pkg, 495+line, band+109764, 456, 205, 76, 74, 19, a14=0)：
     * 场景带整串即时渲染，无打字机。 */
    text_render_box(0, 19, 74, 205, 456,
                    dst + 109764, 495 + g_scene_line_idx, g_pkg_fdtxt0);
    if (g_standing_sprites) {
        int frame = g_scene_anim_clock == 3 ? 1 : g_scene_anim_clock;
        size_t table_at = 4u * (size_t)frame;
        uint32_t off = (uint32_t)g_standing_sprites[table_at]
            | ((uint32_t)g_standing_sprites[table_at + 1] << 8)
            | ((uint32_t)g_standing_sprites[table_at + 2] << 16)
            | ((uint32_t)g_standing_sprites[table_at + 3] << 24);
        int pos = 6 * bg + g_scene_line_idx;
        if (off >= 1920 && off < 0x32A00 && pos >= 0 && pos < 18)
            /* 0x266D5 调 sub_4E22A（透明版）——0x4E29C 的 op11=0x49 满铺
             * 是城镇 UI 专用，此处曾误用致立绘带色底（2026-09-08）。 */
            sprite24_stamp_transparent(g_standing_sprites + off,
                dst + 32904 + 456 * g_sprite_pos_y_lut[pos]
                    + g_sprite_pos_x_lut[pos], 456);
    }
    blit_rows(vram_base() + 1284, FD2_SCREEN_W, dst + 32904, 456, 312, 192);
}

const uint8_t *state_config_get(int state)
{
    /* 0x4E809: 0x6238D + 31*(state-1).  The extracted C table starts one
     * record earlier at 0x6236E, so the exact equivalent is table[state]. */
    if ((unsigned int)state >= FD2_STATE_COUNT) {
        fprintf(stderr, "state_config_get: invalid state %d\n", state);
        abort();
    }
    return g_state_config[state];
}

/* IDA main 内层循环（0x25DB5–0x25E91）—— 结构已验证 */
int game_inner_loop(void)
{
    for (;;) {
        int result = anim_pump();
        if (g_state_pending == 1) {
            g_transition_busy = 0;
            screen_fade_transition();
            g_transition_busy = 1;
            g_state_pending = 0;
            result = 1;
        } else if (g_state_pending == 2) {
            g_transition_busy = 0;
            music_play(-1, 1);
            g_state_enter[g_state]();       /* 对白脚本（可改 g_state）*/
            result = state_run_scene();
            if (result == 0) {
                g_state_exit[g_state]();    /* 场景后逻辑（战斗初始化等）*/
                music_play(g_state_bgm[g_state], 0);
            } else {
                /* 0x25E33 test i / 0x25E59 mov v20,1：state_run_scene 非零
                 * → main 置 v20=1 → 退出游戏。 */
                result = -1;
            }
            g_transition_busy = 1;
            kbd_flush();
            g_state_pending = 0;
        }

        /* IDA 0x25E76 do-while(!i) 以非零 i 退出；0x25E7F 仅 i==-1
         * （anim_pump 返回 -1）置 v20=1 退出游戏。其余非零——含 pending==1
         * 的淡出切换（0x25DFB i=1）与 anim_pump 其它正值——v20 保持 0，
         * main 外层 while(1) 重播 music(18)+开场+标题菜单。本函数据此
         * 编码：返回 -1=退出游戏，0=回标题。 */
        if (result != 0)
            return result == -1 ? -1 : 0;
    }
}

/* IDA 0x26152 state_run_scene —— 主干已验证：
 * 常规：fade -> music(10) -> 背景块(config[0]->LUT@0x52405->{11,61,62})
 *       -> RLE 到影子缓冲+0x8088 (456 宽) -> FDOTHER blk10 覆盖层
 *       -> scene_render_compose 交互循环（4-tick 节拍）。
 * 编队态（g_state_roster_menu[st]!=0）：清屏 -> str410 -> roster_select_menu。 */
int state_run_scene(void)
{
    FILE *icons;

    /* 0x26186..0x26265 discards the completed battlefield before it rebuilds
     * the roster icon cache.  g_battle_ctx is static in this host, while the
     * original allocated it, so only its dynamically allocated counterparts
     * are released here. */
    if (g_ent_table && g_ent_table != g_roster_table)
        free(g_ent_table);
    g_ent_table = NULL;
    g_ent_count = 0;
    free(g_field_map);
    g_field_map = NULL;
    g_field_map_size = 0;

    /* Rebuild the exact FDICON cache from the current roster. */
    icon_cache_reset();
    free(g_standing_sprites);
    g_standing_sprites = NULL;
    icons = fopen("FDICON.B24", "rb");
    if (icons) {
        for (int i = 0; i < g_roster_count; i++) {
            (void)icon_load_entry(g_roster_table[i][7], icons);
            /* 名册网格按位置读目录（0x2810B）；转职后 rec[7]（职业）
             * 重复会令去重压缩槽位 → 目录槽 i 未写 → 乱画进名字区。
             * 显式别名到位置槽，保住 position==slot 不变量。 */
            (void)icon_directory_alias(i, g_roster_table[i][7]);
        }
        fclose(icons);
    }
    if (g_state_roster_menu[g_state]) {
        int roster_result;

        /* 0x26272..0x2637A: roster-menu states first present the common
         * dialogue (410), optionally enter the chapter-save picker, then
         * hand the roster table to roster_select_menu.  The host dialogue
         * implementation supplies the same Enter/Esc decision and restores
         * its snapshot through dialog_backdrop_restore; the field redraw animation is an
         * intentionally bounded presentation placeholder. */
        memset(vram_base(), 0, FD2_VRAM_SIZE);
        palette_apply_range(0, 255, 0);
        dialog_backdrop_load(75);
        text_render_box(1, 19, 74, 205, FD2_SCREEN_W,
                        vram_base() + 38180,
                        410, g_pkg_fdtxt0);
        dato_frame_stamp(0);              /* 0x262DA dato_frame_stamp(0) 闭口肖像 */
        /* 0x262DA..0x262F3 brackets confirm_yes_no with the field-map flag;
         * the original value is a transient sentinel, not an owned map. */
        g_field_map = (void *)1;
        roster_result = confirm_yes_no();
        g_field_map = NULL;
        confirm_close_anim();
        /* 原版此处为 sub_197E5（是/否按钮 4 步收拢 + 面板行回拷）+
         * sub_26996 menu_buffers_teardown（VRAM←快照 + free 三缓冲）。
         * 宿主对话框生命周期以 dialog_backdrop_restore 一并代偿（呈现占位）。 */
        dialog_backdrop_restore();
        if (roster_result != -1 && g_menu_choice == 0) {
            g_scene_bg_pkg = dat_load_block("FDOTHER.DAT",
                                            (void *)g_scene_bg_pkg, 13);
            save_slot_write_menu(0);    /* 0x26331：编队态 picker 模式 0 */
            free((void *)g_scene_bg_pkg);
            g_scene_bg_pkg = NULL;
        }
        do {
            g_ent_table = g_roster_table;
            roster_result = roster_select_menu();
            g_ent_table = NULL;
        } while (roster_result == 0);
        palette_apply_range(0, 255, 255);
        return 0;
    }

    /* Normal ADV path (0x26384..0x2642F)：malloc 影带（宿主静态替代）→
     * cfg 指针 → fade_black → music(10,0) → line_idx=0 → 背景块
     * LUT@0x52405[cfg[0]] → RLE 解到影带+32904（456 pitch）→ free →
     * overlay blk10 → compose → fade_in → kbd_flush。
     * LUT 3 字节 {11,61,62}（get_bytes 定案；blk11@0x33A15/blk61@0x229F69/
     * blk62@0x237530 均 320×200 整屏帧——影带 456 宽中占 0..319 列，右
     * 136 列为不呈现工作区）。原版对 cfg[0] 无上界检查（30 态全为 0..2，
     * C 护栏等价）。原版亦不重置 0x53F52 动画帧计数（跨场景保持）。 */
    const uint8_t *cfg = state_config_get(g_state);
    static const uint8_t bg_lut[3] = { 11, 61, 62 };
    void *backdrop = NULL;
    void *overlay = NULL;
    int last_tick;
    int music_probe = 0;

    g_shop_stock_ptr = cfg;
    palette_fade_black();
    music_play(10, 0);
    g_scene_line_idx = 0;
    if (cfg[0] < 3) {
        backdrop = dat_load_block("FDOTHER.DAT", NULL, bg_lut[cfg[0]]);
        memset(s_scene_shadow, 0, sizeof(s_scene_shadow));
        rle_decode_frame((const uint8_t *)backdrop, 0, 0,
                         s_scene_shadow + 32904, 456, -1);
        free(backdrop);
    }
    overlay = dat_load_block("FDOTHER.DAT", NULL, 10);
    scene_render_host(overlay);
    fade_in();
    kbd_flush();
    for (;;) {
        int key;

        /* 0x26434..0x2648D 轮询式：每轮先渲染当前状态再取键——按键
         * 效果即时呈现；空闲时每 4 tick（或 0x46C 回绕）推进
         * dword_53F52 并重绘，场景立绘待机动画（0,1,2,1 帧）由此
         * 驱动。旧实现阻塞在 input_wait_key、渲染滞后一拍且空闲
         * 无重绘（"第一次按反/人物无动画"两症状根因，2026-09-08）。 */
        scene_render_host(overlay);
        last_tick = (int)bios_tick();
        while (!kbd_key_avail()) {
            int delta = (int)bios_tick() - last_tick;

            if (delta >= 4 || delta < 0) {
                g_scene_anim_clock = (g_scene_anim_clock + 1u) & 3u;
                scene_render_host(overlay);
                last_tick = (int)bios_tick();
            }
            host_idle_pump();       /* 宿主事件泵（原版 0x26434 裸轮询、
                                     * 不调 idle_pump——DAC 色循环不得
                                     * 在原版 15 个调用点之外执行） */
        }
        key = input_wait_key();
        if (key == SCAN_RIGHT) {
            if (g_scene_line_idx == 0) g_scene_line_idx = 4;
            else --g_scene_line_idx;
            sfx_play(g_pkg_fdother_31, 0, 1);    /* 0x26504：两方向同为条目 0
                                                  *（77/75 是 0x4D/0x4B 扫描码，
                                                  * 旧注释误当音效条目 2026-09-08）*/
        } else if (key == SCAN_LEFT) {
            if (++g_scene_line_idx > 4) g_scene_line_idx = 0;
            sfx_play(g_pkg_fdother_31, 0, 1);    /* 0x26536 同上 */
        } else if (key == 0x22) {
            music_probe = (music_probe + 1) % 10;
            music_play(music_probe, 0);
        } else if (key == cfg[2] && g_scene_line_idx == cfg[1]) {
            g_scene_line_idx = 5;
        } else if (key == SCAN_ENTER || key == 57 || key == 32) {
            if (g_scene_line_idx != 2)
                sfx_play(g_pkg_fdother_31, 1, 3);
            if (scene_page_dispatch()) {
                free(overlay);
                return g_scene_line_idx != 2;
            }
        }
    }
}

/* IDA 0x25EBB state_boot_driver（F-005）—— 分支结构已验证 */
int state_boot_driver(void)
{
    int r = startup_intro_menu();
    if (r == 0) {                     /* 新游戏 */
        palette_fade_black();
        g_state = 0;
        g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, 0);
        g_roster_count = 0;
        g_transition_busy = 0;
        g_state_exit[0]();            /* 原版 0x25F0B 重读 g_state 后调 exit[g_state] */
        music_play(g_state_bgm[g_state], 0);  /* 0x25F19 序章结束后重读 g_state 取 BGM */
        g_transition_busy = 1;
        kbd_flush();
        return 0;
    }
    if (r == 1) {
        /* 载入章节存档（原版 0x25F3F..0x26119，2026-09-07 补全——此前
         * 误接 save_game_load 战场续战）。流程：FDOTHER blk13 槽面板 →
         * 读+解码 FD2.SAV（无档/坏档 = 全 FF 底）→ save_slot_pick(0)：
         * 确认后槽区 = SLOT_BASE+2600*slot，roster 2560B + 尾 10B
         * （state/roster_count/gold u32/51AAB/53AF9/music/sfx）；
         * state==0xFF 空槽 → 重选；取消 → 回标题；成功 → 复现场景
         * （state_run_scene）+ g_state_exit 战场重建 + BGM。 */
        uint8_t *record = malloc(FD2_SAVE_SIZE);
        int ok = -1;

        g_scene_bg_pkg = dat_load_block("FDOTHER.DAT",
                                        (void *)g_scene_bg_pkg, 13);
        palette_fade_black();
        g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, 0);
        memset(vram_base(), 0, FD2_VRAM_SIZE);
        palette_apply_range(0, 255, 0);
        if (!record) {
            free((void *)g_scene_bg_pkg);
            g_scene_bg_pkg = NULL;
            return -1;
        }
        /* 0x25FCD..0x25FE0 reads and decodes the slot file but does not
         * compare its checksum.  Missing files are converted to an all-FF
         * slot table.  The original ignores a short fread; fd2re maps that
         * malformed edge to the same explicit all-FF placeholder.  Checksum
         * validation belongs to F-006's menu count and to the separate
         * battle-resume loader. */
        if (!save_record_read_decoded(record))
            memset(record, 0xFF, FD2_SAVE_SIZE);
        g_menu_choice = 0;
        for (;;) {
            const uint8_t *tail;
            int pick = save_slot_pick_record(0, record);
            int route = pick;               /* -1 取消 / 1 确认 / 0 重选 */

            if (route != -1) {
                tail = record + FD2_SAVE_SLOT_BASE
                             + FD2_SAVE_SLOT_STRIDE * g_menu_choice + 2560;
                /* 0x2604A..0x26098 copies the whole 2560-byte roster and the
                 * ten-byte tail before testing state.  Keep that ordering: it
                 * is observable when an empty slot is selected and then the
                 * cursor moves to another slot. */
                if (!g_roster_table)
                    g_roster_table = malloc(FD2_ROSTER_MAX
                                            * FD2_ENT_REC_SIZE);
                if (!g_roster_table) {
                    route = -1;             /* 宿主分配失败按取消路由 */
                } else {
                    memmove(g_roster_table, tail - 2560, 2560);
                    g_state = tail[0];
                    g_roster_count = tail[1];
                    g_gold = (int32_t)((uint32_t)tail[2]
                             | ((uint32_t)tail[3] << 8)
                             | ((uint32_t)tail[4] << 16)
                             | ((uint32_t)tail[5] << 24));
                    g_save_flag_51aab = tail[6];
                    g_save_flag_53af9 = tail[7];
                    g_music_gate = tail[8];
                    g_sfx_gate = tail[9];
                    if (g_state == 0xFF)
                        route = 0;          /* 0x260A9：esi=0 → 重选 */
                }
            }
            menu_buffers_teardown();        /* 0x260AB：三路共点收
                                             *（取消/空槽/成功均过）*/
            if (route == 0)
                continue;
            if (route == 1)
                ok = 1;
            break;                          /* route==-1：回标题 */
        }
        free(record);
        free((void *)g_scene_bg_pkg);
        g_scene_bg_pkg = NULL;
        if (ok == 1) {
            int result;

            g_transition_busy = 0;
            result = state_run_scene();
            if (result == 0) {
                g_state_exit[g_state]();
                music_play(g_state_bgm[g_state], 0);
            }
            g_transition_busy = 1;
            kbd_flush();
            return result;
        }
        /* 0x260D9..0x26119: esi==-1 跳过 busy=1 直达 loc_26119 的
         * kbd_flush 后返回——取消路径同样清残留键。 */
        kbd_flush();
        return ok;                          /* -1：回标题（原版重播开场） */
    }
    if (r == 2) {
        /* 继续战斗 = 中断续战（原版 loc_26124：music(-1) → save_game_load
         * → music(bg[state])，载入返回值被忽略，恒回 loc_2614C 返回 0）。
         * 此前落入 return r → main done=1 直接关窗——用户"菜单继续闪退"
         * 的根因（2026-09-07）。fd2re 唯一偏离：坏档时 save_game_load
         * 返回 -1 → 回标题重选（原版会带空状态进入内层循环）——宿主
         * 安全护栏。 */
        music_play(-1, 0);
        if (save_game_load() != 0)
            return -1;
        music_play(g_state_bgm[g_state], 0);
        return 0;
    }
    return r;
}
