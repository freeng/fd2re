/* startup.c — 开场 + 主菜单（F-006；菜单子帧表与按键已动态取证） */
#include <stdlib.h>
#include <string.h>
#include "startup.h"
#include "resource.h"
#include "video.h"
#include "input.h"
#include "audio.h"
#include "duel.h"
#include "timer.h"
#include "save.h"

/* FDOTHER.DAT 块 7（23,377B）子帧：
 * 0: 320x200 标题/背景；1/2: 条目0 普通/高亮；3/4: 条目1；5/6: 条目2。
 * 绘制目标行 164/173/182，列 129（320 宽直写 0xA0000）。 */
#define MENU_FRAME_BG        0
#define MENU_ITEM0_NORMAL    1
#define MENU_ITEM0_HILITE    2
#define MENU_ITEM1_NORMAL    3
#define MENU_ITEM1_HILITE    4
#define MENU_ITEM2_NORMAL    5
#define MENU_ITEM2_HILITE    6

static const struct { int row, normal, hilite; } menu_items[3] = {
    { 164, MENU_ITEM0_NORMAL, MENU_ITEM0_HILITE },
    { 173, MENU_ITEM1_NORMAL, MENU_ITEM1_HILITE },
    { 182, MENU_ITEM2_NORMAL, MENU_ITEM2_HILITE },
};

static void intro_restore_scroll(const uint8_t *canvas, int row)
{
    g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, 101);
    blit_rows(vram_base(), FD2_SCREEN_W, canvas + FD2_SCREEN_W * row,
              FD2_SCREEN_W, FD2_SCREEN_W, FD2_SCREEN_H);
    fade_in();
}

static void intro_show_card(int image_block, int palette_block,
                            const uint8_t *canvas, int row)
{
    void *image;

    palette_fade_black();
    memset(vram_base(), 0, FD2_VRAM_SIZE);
    g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, palette_block);
    image = dat_load_block("FDOTHER.DAT", NULL, image_block);
    rle_decode_frame((const uint8_t *)image, 0, 0,
                     vram_base(), FD2_SCREEN_W, -1);
    fade_in();
    wait_bios_ticks(1);
    wait_bios_ticks(6);
    palette_fade_black();
    /* sub_1F73F returns without freeing this image; preserve that lifetime
     * (the original title path leaks the two card buffers). */
    intro_restore_scroll(canvas, row);
}

static void intro_play_ani(int anim_id, int frame_delay, int palette_block)
{
    if (palette_block != -1) {
        memset(vram_base(), 0, FD2_VRAM_SIZE);
        g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, palette_block);
    }
    palette_apply_range(0, 255, 0);
    ani_play(anim_id, frame_delay, 0);
    palette_fade_black();
}

static void intro_play_scroll(void *sound_pkg, void *first_block)
{
    static const int sound_rows[] = {
        520, 430, 410, 340, 310, 300, 240, 180, 150, 130, 110, 87, 64, 22, 1000
    };
    uint8_t *canvas = calloc(735, FD2_SCREEN_W);
    /* The original keeps block 74 as the reusable load buffer; the first
     * block-69 load releases it inside dat_load_block(old_block=...). */
    void *roll_image = first_block;
    int sound_index = 0;
    int flash_phase = 0;

    if (!canvas)
        dat_load_fatal("FDOTHER.DAT", 69);
    for (int i = 0; i < 5; i++) {
        roll_image = dat_load_block("FDOTHER.DAT", roll_image, 69 + i);
        rle_decode_frame((const uint8_t *)roll_image, 0, 147 * i,
                         canvas, FD2_SCREEN_W, -1);
    }

    /* 0x1FA63: the title gate discards any prior entity table and leaves the
     * two-record bootstrap table expected by the subsequent menu/game path. */
    if (g_ent_table)
        free(g_ent_table);
    g_ent_table = malloc(160);

    kbd_flush();
    for (int row = 535; row >= 0; row--) {
        blit_rows(vram_base(), FD2_SCREEN_W, canvas + FD2_SCREEN_W * row,
                  FD2_SCREEN_W, FD2_SCREEN_W, FD2_SCREEN_H);
        if (row == 535)
            fade_in();

        switch (row) {
        case 450:
            intro_show_card(100, 99, canvas, row);
            break;
        case 330:
            palette_fade_black();
            intro_play_ani(4, 90, 99);
            /* sub_1F81E(5, 50, 0): helper clears the frame and installs
             * FDOTHER palette block 0 before the second ANI stream. */
            intro_play_ani(5, 50, 0);
            intro_restore_scroll(canvas, row);
            break;
        case 210:
            palette_fade_black();
            intro_play_ani(6, 90, 99);
            /* sub_1F81E(7, 50, 0), matching the 0x1FB74..0x1FB7A
             * argument setup in the original switch arm. */
            intro_play_ani(7, 50, 0);
            intro_restore_scroll(canvas, row);
            break;
        case 110:
            /* 0x1FB84 explicitly fades to black before
             * sub_1F81E(8, 90, 99). */
            palette_fade_black();
            intro_play_ani(8, 90, 99);
            intro_restore_scroll(canvas, row);
            break;
        case 25:
            /* sub_1F81E(0, 15, 0), not the no-palette variant. */
            intro_play_ani(0, 15, 0);
            intro_restore_scroll(canvas, row);
            break;
        case 10:
            intro_show_card(75, 76, canvas, row);
            break;
        default:
            break;
        }

        if (row == sound_rows[sound_index]) {
            flash_phase = 0;
            sfx_play(sound_pkg, 0, 1);
            g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, 102);
            palette_apply_range(0, 255, 0);
            sound_index++;
        }
        if (flash_phase == 11) {
            g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, 101);
            palette_apply_range(0, 255, 0);
        }
        flash_phase++;
        delay_ms(30);
        if (row == 0)
            delay_ms(1000);
        if (kbd_key_avail())
            break;
    }

    for (int scale = 40; scale >= 0; scale--) {
        /* IDA 0x1FC6D: palette_mix_range(0, 255, scale, 63, 0, 0).
         * The title gate fades the scroll into red before the menu. */
        palette_mix_range(0, 255, scale, 63, 0, 0);
        delay_ms(8);
    }
    delay_ms(100);
    kbd_flush();
    free(canvas);
    free(roll_image);
}

void menu_draw_entries(const void *pkg, int selected, int entry_count)
{
    for (int i = 0; i < entry_count; i++) {
        int frame = (selected == i) ? menu_items[i].hilite : menu_items[i].normal;
        package_blit_frame(pkg,
                           vram_base() + 320L * menu_items[i].row + 129,
                           FD2_SCREEN_W, frame);
    }
}

int startup_intro_menu(void)
{
    void *pkg, *sound_pkg;
    int entry_count, selected = 0, confirmed = 0, scan;

    sound_pkg = dat_load_block("FDOTHER.DAT", NULL, 77);
    memset(vram_base(), 0, FD2_VRAM_SIZE);
    g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, 76);
    palette_apply_range(0, 255, 64);
    void *intro = dat_load_block("FDOTHER.DAT", NULL, 74);
    memset(vram_base(), 0, FD2_VRAM_SIZE);
    rle_decode_frame((const uint8_t *)intro, 0, 0,
                     vram_base(), FD2_SCREEN_W, -1);
    fade_in();
    wait_bios_ticks(1);
    wait_bios_ticks(30);
    palette_fade_black();

    g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, 99);
    memset(vram_base(), 0, FD2_VRAM_SIZE);
    palette_apply_range(0, 255, 0);
    ani_play(3, 90, 1);
    palette_fade_black();

    memset(vram_base(), 0, FD2_VRAM_SIZE);
    g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, 101);
    palette_apply_range(0, 255, 64);
    intro_play_scroll(sound_pkg, intro);

    pkg = dat_load_block("FDOTHER.DAT", NULL, 7);
    g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, 8);
    memset(vram_base(), 0, FD2_VRAM_SIZE);
    palette_apply_range(0, 255, 0);
    ani_play(1, 15, 1);
    sfx_play_b(sound_pkg, 3, 1);   /* 0x1FD24 call sfx_play_b（B 通道，非 0x25A96） */
    palette_add_range(0, 255, 64);
    package_blit_frame(pkg, vram_base(), FD2_SCREEN_W, MENU_FRAME_BG);
    for (int scale = 0; scale <= 40; scale++) {
        palette_mix_range(0, 255, scale, 56, 60, 63);
        delay_ms(8);
    }
    kbd_flush();
    entry_count = save_check() ? (save_progress(save_record_buf()) != -1 ? 3 : 2) : 1;

    while (!confirmed) {
        menu_draw_entries(pkg, selected, entry_count);
        /* 0x1FE48：int386(0x16) 直读（无空闲回调）——不得用
         * input_wait_key（其 idle_pump 的 DAC 色循环会覆盖菜单
         * 高亮/背景调色板 224..239，2026-09-07 回归根因）。helper 已把
         * E0 前缀/KP_ENTER 归一到 SCAN_ENTER，0x52（小键盘 0/Ins）按
         * 原版 0x1FECA 原样直通。 */
        scan = input_read_key_blocking();
        if (scan == 0x48) {                       /* Up：环绕 */
            sfx_play(sound_pkg, 2, 1);            /* blk77 idx2，loop=1 */
            selected = (selected == 0) ? entry_count - 1 : selected - 1;
        } else if (scan == 0x50) {                /* Down：环绕（已动态取证） */
            sfx_play(sound_pkg, 2, 1);            /* blk77 idx2，loop=1 */
            selected = (selected == entry_count - 1) ? 0 : selected + 1;
        } else if (scan == SCAN_ENTER || scan == 0x39 /*Space*/
                   || scan == 0x52 /*Numpad0/Ins，0x1FECA 原版确认键*/) {
            sfx_play(sound_pkg, 1, 1);            /* blk77 idx1，loop=1 */
            confirmed = 1;
        }
    }
    for (int pass = 0; pass < 4; pass++) {
        menu_draw_entries(pkg, -1, entry_count);
        delay_ms(80);
        menu_draw_entries(pkg, selected, entry_count);
        delay_ms(80);
    }
    palette_fade_black();
    memset(vram_base(), 0, FD2_VRAM_SIZE);
    free(pkg);
    sfx_play(sound_pkg, -1, 1);
    free(sound_pkg);
    return selected;
}
