/* scene.c — ADV 场景页群骨架（docs/architecture.md §15.3-15.5，2026-09-04）
 *
 * 四页结构 + 商店/整备子功能。原版细节：
 *   商店菜单：0=买入(shop_weapon_menu) 1=卖出(×3/4 价) 2=使用 3=转交；
 *   整备菜单：0=状态浏览 1=转交(共用) 2=教会复活 3=转职；
 *   复活价 = g_revive_price_lut[ent[+32]职业系] × ent[+33]等级，
 *   成交后 ent[+5]=0 且 HP=MaxHP；
 *   转职消耗 g_promote_item_lut[ent[+7]]（新职业<50 免费，52 需道具 90），
 *   成交后 ent[+32]=职业表[0]、ent[+7]=新职业，重载 FDICON.B24 图标。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fd2.h"
#include "scene.h"
#include "entities.h"
#include "battle.h"
#include "video.h"
#include "text.h"
#include "input.h"
#include "host.h"
#include "timer.h"
#include "audio.h"
#include "save.h"
#include "statemachine.h"
#include "magic.h"
#include "resource.h"
#include "duel.h"

/* ---- 场景全局（IDA 地址注释；与原版同名语义） ---- */
uint8_t  g_scene_line_idx;   /* 0x53F4A 场景相位 0-4（页面分发键） */
const uint8_t *g_shop_stock_ptr; /* 0x53F56 商店货单基址（state_run_scene
                                    设置 / save_slot_load_menu 恢复） */
const void *g_scene_bg_pkg;      /* 场景背景包（state_run_scene dat_load
                                    FDOTHER 13/14 装载）。0x53AF5 已定案为
                                    g_walk_pix_off（步行平滑滚动偏移，见
                                    entities.c/IDB），与本变量无关。 */
int32_t g_shop_cursor_mem;        /* 商店光标记忆（跨循环保持） */
int32_t g_shop_scroll_mem;        /* 商店滚动记忆 */
int32_t g_shop_list_scroll;       /* 当前列表滚动位置 */
int32_t g_scene_list_count;       /* 0x53F5E 当前列表条数（0x27178/0x2720D
                                    箭头判定读；0x53F62 系 units 数组指针，
                                    mode-2 立绘重绘 0x2727E 读——fd2re 以
                                    menu_anim_wait 的 list 形参建模） */
void   *g_fdtxt_ptr_table;         /* DATO.DAT 相位块（fd2.h 声明；状态页/
                                      道具使用后重载刷新文本窗格） */
uint8_t *g_menu_work_buf;          /* 0x53C5B 选人器/列表三缓冲（fd2.h 声明；
                                      0x26996 统一收尾释放） */
uint8_t *g_menu_snap_buf;          /* 0x53C5F 开面板前整屏快照 */
uint8_t *g_screen_backup;          /* 0x53C63 面板/列表合成源 */
static const uint8_t *s_scene_return_backdrop;

static const uint8_t g_scene_sprite_x[18] = {
    0x1D, 0x29, 0x3B, 0x9A, 0xB6, 0x0A,
    0x5A, 0x21, 0x35, 0x94, 0xDE, 0xC4,
    0x3B, 0x0A, 0x3B, 0x82, 0xF2, 0x88,
};
static const uint8_t g_scene_sprite_y[18] = {
    0x2E, 0x6D, 0xA3, 0x8B, 0x41, 0x0A,
    0x1E, 0x69, 0xA3, 0x8B, 0x55, 0x08,
    0x1A, 0x90, 0xA3, 0x96, 0x1F, 0x14,
};

/* Common 0x2953B/0x27C3D/0x29F... page-return sequence.  The dispatcher
 * owns the 64000-byte selector snapshot and keeps it alive through a page. */
static void scene_page_close(void)
{
    const uint8_t *cfg = state_config_get(g_state);
    int pos = 6 * cfg[0] + g_scene_line_idx;

    package_blit_frame(g_scene_bg_pkg, vram_base(), 320, 0);
    delay_ms(200);                 /* 0x29548：重贴帧 0 后固定停留 */
    palette_fade_black();
    if (s_scene_return_backdrop && pos >= 0 && pos < 18) {
        /* 0x29564..0x295F6 拉远 = 推近的逆序列（i=10..0 共 11 步），中心
         * 同式插值：cx=20480+((i*(x-150)/10)<<7)、cy=12800+…，i=0 回到
         * 全屏 (160,100)/palette 0（2026-09-08 更正旧固定中心 128*x）。 */
        for (int i = 10; i >= 0; i--) {
            scene_zoom_blit(20480 + ((i * (g_scene_sprite_x[pos] - 150) / 10) << 7),
                            12800 + ((i * (g_scene_sprite_y[pos] - 100) / 10) << 7),
                            s_scene_return_backdrop, 128 - 9 * i);
            palette_apply_range(0, 255, 4 * i);
        }
    }
    free((void *)g_scene_bg_pkg);
    g_scene_bg_pkg = NULL;
}

/* 道具槽扫描（inventory_find_item_slot@0x2AEDB 核心语义）：
 * ent[+10+2i]=数量/占用（signed，-1=空）、ent[+11+2i]=道具id；命中返回槽位。 */
static int roster_has_item(const uint8_t *ent, uint8_t item)
{
    for (int i = 0; i < 8; i++)
        if ((int8_t)ent[10 + 2 * i] >= 0 && ent[11 + 2 * i] == item)
            return i;
    return -1;
}

/* 转职消耗道具表 @0x523D5（前 18 项=职业 0..17 实读；0xFF/0xCD=无消耗
 * 路径；尾部 [18..51] 为 IDB 原始字节 2026-09-08 dump——原读路径仅
 * cls<18，[38+] 的 01/0B/3D/3E/D9 疑为相邻表渗入，按字节照录）。
 * 职业 9 的特殊道具 90（→新职业 52）不在此表。 */
const uint8_t g_promote_item_lut[52] = {
    0x59, 0x5D, 0xFF, 0x5D, 0xCD, 0xCD, 0xCD, 0xCD,
    0x5C, 0x58, 0x58, 0x58, 0x5B, 0x5C, 0x58, 0x5B,
    0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1,
    0x0B, 0x3D, 0x3E, 0xD9
};

/* 复活单价表 @0x52397（20×u16，索引=职业系；dump 2026-09-04）——
 * temple_revive_menu 价格 = 本表[ent[+32]职业系] × ent[+33]等级。 */
const uint16_t g_revive_price_lut[20] = {
    506, 100, 150, 100, 100, 100, 100, 100, 100,
    1200, 1600, 1000, 1000, 1200, 1400, 1200, 1600,
    100, 1800, 1200,
};

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* sub_26C9B：商店数字字形贴图。字形条 = g_scene_bg_pkg 帧 2
 * （[u16 6][u16 99] 头 + 6×99 像素条；11 位含进位位，资产证实 +4 跳头）；
 * glyph = 9*digit + 滚动偏移(0..9)，逐行 9 行 × 6 字节。 */
static void shop_digit_stamp(uint8_t *dst, int pitch, int glyph)
{
    const uint8_t *pkg = (const uint8_t *)g_scene_bg_pkg;
    if (!pkg)
        return;
    const uint8_t *src = pkg + rd_u32(pkg + 14) + 4 + 6 * glyph;
    for (int i = 0; i < 9; i++) {
        memcpy(dst, src, 6);
        dst += pitch;
        src += 6;
    }
}

/* stat_growth_roll@0x1E529：成长掷点——roll = min + fd2_rand()%(max-min)
 * （min==max 则 0）；非零 → *stat += roll + 逐行文本（文本区行基
 * 38175+6080*row；row==3 时先 dialog_scroll_text 滚行再显示于第 2 行，
 * 返回 3——2026-09-08 字节级复核补齐）；返回下一行。 */
static void shop_num_stamp(uint8_t *dst, int pitch, int val,
                           int base, int digits);
/* 转职类别信息区 @0x615BE（2026-09-08 核验扩为 68×u16 = 0x615BE..0x61645；
 * promote_bonus_entry@0x4E7DD 原始字节 = shl 1 前有 sub eax,0x20，即
 * 0x615FE+2*(cls-32) ≡ 0x615BE+2*cls 的字访问；[68]+ 与职业表 0x61646
 * 地址别名——原版即此读）：低字节=转职后职业系 ent[+32]
 *（cls32→9、cls50→17、cls51→18、cls52→21）；高字节=移动力奖励。
 * 新职业可达 53..65（cls+50 路径，cls∈{3,8..15} 持证道具），
 * 原版 class_promote_menu/promote_stat_rolls/promote_rows_draw 三处
 * 均无 <53 守卫，[53..67] 是实读数据（dump 2026-09-08）。 */
static const uint16_t g_promote_classinfo[68] = {
    0, 0, 0, 0, 14, 1024, 256, 2, 0, 34, 0, 0, 0, 0, 0, 3840, 0, 260,
    513, 256, 8704, 0, 0, 0, 0, 0, 0, 16, 1024, 257, 2, 1, 265, 10,
    265, 10, 267, 267, 267, 267, 268, 269, 270, 270, 16, 268, 269, 16,
    10, 271, 529, 18, 533, 18, 531, 531, 531, 531, 276, 278, 278, 278,
    280, 276, 278, 280, 18, 279,
};

static int stat_roll_row(uint16_t *stat, const uint8_t *growth_pair,
                         int text_id, int row)
{
    int lo = growth_pair[0];
    int roll = 0;

    if (growth_pair[1] != lo)
        roll = lo + fd2_rand() % (growth_pair[1] - lo);
    g_disp_num_b = roll;
    if (roll) {
        /* 0x1E565 行 3 特调：窗口只显示 3 行（0..2），第 4 行起先滚一行
         * 再固定显示在第 2 行（原实现误省略为常规递增）。 */
        if (row == 3) {
            row = 2;
            dialog_scroll_text();
        }
        kbd_flush();
        text_render_box(1, 19, 74, 205, 320,
                        vram_base() + 38175u + 6080u * (unsigned)row,
                        text_id, g_pkg_fdtxt0);
        *stat = (uint16_t)(*stat + roll);
        row++;
    }
    return row;
}

/* 顶部金币显示（shop/prep 页共用，0x27A63/0x29E7B 全解 2026-09-06）：
 * bg pkg 帧1 面板 stamp_frame_opaque @vram+30405 → number_stamp_digits
 * (字形基 31, 8 位) @vram+31696（row99——旧实现 36816 落进对白框内，
 * 本批更正）；两处同步盖进场景窗快照（首轮 teardown 恢复后仍可见）。 */
static void scene_gold_display(void)
{
    const uint8_t *panel = package_frame_ptr(g_scene_bg_pkg, 1);
    uint8_t *snap = dialog_backdrop_snapshot();

    if (panel) {
        rle_blit_opaque(panel, vram_base() + 30405, 320);
        number_stamp_digits(vram_base() + 31696, 320, g_gold, 31, 8);
        if (snap) {
            rle_blit_opaque(panel, snap + 30405, 320);
            number_stamp_digits(snap + 31696, 320, g_gold, 31, 8);
        }
    }
}

/* 金币面板+当前值仅盖进活动快照（0x2A62C..0x2A664 教会复活成交步，
 * 2026-09-13 补）：gold_spend_animate 只写 VRAM，随后收框整屏恢复
 * 确认窗快照会把金币盖回旧值——原版在收框前把帧1面板与 g_gold 重新
 * 盖进 g_menu_snap_buf（即活动场景窗快照），使恢复带回新值。 */
static void scene_gold_snapshot_stamp(uint8_t *snap)
{
    const uint8_t *panel = package_frame_ptr(g_scene_bg_pkg, 1);

    if (panel && snap) {
        rle_blit_opaque(panel, snap + 30405, 320);
        number_stamp_digits(snap + 31696, 320, g_gold, 31, 8);
    }
}

/* 0x29300 scene_base_camp（2026-09-06 语义更正：相位 0 = 酒馆，非
 * "据点主页"——主页即场景相位选择环本身；酒馆功能 = 存档/读档/休整/
 * 退出）：FDOTHER blk13 背景帧 0 → fade_in → delay(200) → 场景窗
 * dialog_backdrop_load(ids[0]=129)（滑入；店主大肖像位 1707，帧由
 * 后续 dato_frame_stamp 上屏）→ 文本 585@vram+38092（框全宽——无框
 * 内肖像）+ dato_frame_stamp(0) 闭口 → 四选菜单循环：光标记忆
 * g_menu_choice=上次选择 → scene_menu_draw(0) 滑入 → input →
 * draw(1) 滑出 → menu_buffers_teardown（0x293DF 每轮：框下滑收起，
 * 动作在裸场景上跑）→ 分发。0=unit_status_browser 1=save_slot_
 * write_menu(picker 模式 1) 2=save_slot_load_menu 3=退出游戏确认：
 * 415@38092→confirm→是→416@vram+44172+delay200+teardown→返 1
 * （dispatch 非零 → state_run_scene 以 line_idx!=2 返 1 → 状态机
 * v20=1 退出整个游戏）；否→teardown 重开文本 586。出击确认不在本页
 * ——在 scene_page_dispatch 相位 2。每轮动作后重开窗+文本 586+肖像。
 * Esc：无重开，直接收尾（scene_page_close = 帧 0 重贴+delay200+
 * fade_black+11 步拉远缩放+free）。本页无金币显示。 */
int scene_base_camp(void)
{
    int sel = 0;    /* 原版 0x29324 另缓存 cfg[0] 供退出缩放定位；
                     * 宿主 scene_page_close 自行重取，无需复刻 */

    g_scene_bg_pkg = dat_load_block("FDOTHER.DAT", (void *)g_scene_bg_pkg, 13);
    package_blit_frame(g_scene_bg_pkg, vram_base(), 320, 0);
    fade_in();
    delay_ms(200);
    dialog_backdrop_load(g_dialog_backdrop_ids[0]);
    text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                    585, g_pkg_fdtxt0);
    dato_frame_stamp(0);
    for (;;) {
        int r;

        g_menu_choice = sel;            /* 0x293AE 光标记忆 */
        scene_menu_draw(0);
        r = scene_menu_input();
        if (r == 1)
            sel = g_menu_choice;        /* 0x293CF 记住本轮选择 */
        scene_menu_draw(1);
        menu_buffers_teardown();        /* 0x293DF：每轮收框 */
        if (r != 1)
            break;                      /* Esc：do-while(v28==1) 退出 */
        switch (g_menu_choice) {
        case 0:
            unit_status_browser();
            break;
        case 1:
            save_slot_write_menu(1);
            break;
        case 2:
            save_slot_load_menu();
            break;
        default:
            dialog_backdrop_load(g_dialog_backdrop_ids[0]);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                            415, g_pkg_fdtxt0);
            dato_frame_stamp(0);
            {
                int yn = confirm_yes_no();     /* 0x29472 */

                confirm_close_anim();          /* 0x29479 */
                if (yn != -1 && g_menu_choice == 0) {
                    text_render_box(1, 19, 74, 205, 320,
                                    vram_base() + 44172u,
                                    416, g_pkg_fdtxt0);
                    delay_ms(200);
                    menu_buffers_teardown();   /* 0x294C3 */
                    return 1;
                }
            }
            menu_buffers_teardown();       /* 0x294D2 */
            break;
        }
        dialog_backdrop_load(g_dialog_backdrop_ids[0]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        586, g_pkg_fdtxt0);
        dato_frame_stamp(0);
    }
    scene_page_close();
    return 0;
}

/* 0x279BC scene_shop_page（对话链 2026-09-06 全解对齐）：相位 1/3/5
 * 商店页。bg blk 12/29/63 → fade_in → delay200 → 场景窗
 * dialog_backdrop_load(ids[line])（店主 128/130/132，大肖像位
 * 4283/3939/3644）→ 金币显示 → 开店文本（相位1=501 其余=440）
 * @vram+38092 + dato_frame_stamp(0) → 光标/滚动记忆清零 → 四选菜单
 * 0=买入 1=卖出(75% 价) 2=使用 3+=转交；每轮 draw(1) 后
 * menu_buffers_teardown 收框再分发；动作后重开窗+文本（相位1=503
 * 其余=440）@38092+stamp。 */
int scene_shop_page(void)
{
    static const uint8_t bg_blk[3] = { 12, 29, 63 };
    int page = g_scene_line_idx == 1 ? 0 : (g_scene_line_idx == 3 ? 1 : 2);
    int sel = 0;                    /* 0x279BC v4: 外层四选菜单光标 */

    g_scene_bg_pkg = dat_load_block("FDOTHER.DAT", (void *)g_scene_bg_pkg,
                                    bg_blk[page]);
    package_blit_frame(g_scene_bg_pkg, vram_base(), 320, 0);
    fade_in();
    delay_ms(200);
    dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
    scene_gold_display();
    text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                    g_scene_line_idx == 1 ? 501 : 440, g_pkg_fdtxt0);
    dato_frame_stamp(0);
    g_shop_cursor_mem = 0;
    g_shop_scroll_mem = 0;
    for (;;) {
        int r, stock_count;
        uint8_t stock[32];

        /* 0x27B30：买入/卖出等子菜单会复用 g_menu_choice；每轮进入
         * 外层动画前必须恢复四选菜单自己的光标，不能把商品/角色索引
         * 当成 0..3 的场景菜单索引。 */
        g_menu_choice = sel;
        scene_menu_draw(0);
        r = scene_menu_input();
        if (r == 1)
            sel = g_menu_choice;      /* 0x27B51：仅确认后记忆 */
        scene_menu_draw(1);
        /* 原版在收框前刷新货单并写入 0x53F5E；该值也负责列表动画的
         * 箭头边界，避免子菜单退出后残留上一层列表长度。 */
        stock_count = shop_stock_build(stock);
        g_scene_list_count = stock_count;
        menu_buffers_teardown();        /* 0x27B75：Esc 亦先收框再退出——
                                         * 带活跃窗退出会让下一页开窗时的
                                         * 自动收拢以陈旧快照盖掉新背景
                                         * （教会→商店背景污染根因） */
        if (r != 1)
            break;
        switch (g_menu_choice) {
        case 0: {
            if (stock_count > 0)
                shop_weapon_menu(stock_count, stock);
            break;
        }
        case 1:
            shop_item_sell();
            break;
        case 2:
            shop_item_use();
            break;
        default:
            inventory_transfer();
            break;
        }
        dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        g_scene_line_idx == 1 ? 503 : 440, g_pkg_fdtxt0);
        dato_frame_stamp(0);
    }
    scene_page_close();
    return 0;
}

/* 0x29DAA scene_prep_page：相位 4 整备页。FDOTHER blk14 背景、
 * 文本 585/586；四选 0=状态浏览 1=转交（与商店共用）2=教会复活
 * 3=转职。 */
int scene_prep_page(void)
{
    int sel = 0;                    /* 0x29ECC v5: 外层四选菜单光标 */

    g_scene_bg_pkg = dat_load_block("FDOTHER.DAT", (void *)g_scene_bg_pkg, 14);
    package_blit_frame(g_scene_bg_pkg, vram_base(), 320, 0);
    fade_in();
    delay_ms(200);
    dialog_backdrop_load(g_dialog_backdrop_ids[4]);   /* byte_5238B=131 */
    scene_gold_display();
    text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                    585, g_pkg_fdtxt0);
    dato_frame_stamp(0);
    for (;;) {
        int r;

        g_menu_choice = sel;        /* 子菜单会复用全局选择索引 */
        scene_menu_draw(0);
        r = scene_menu_input();
        if (r == 1)
            sel = g_menu_choice;    /* 0x29EED：仅确认后记忆 */
        scene_menu_draw(1);
        menu_buffers_teardown();        /* 0x29EFD：Esc 亦先收框再退出 */
        if (r != 1)
            break;
        switch (g_menu_choice) {
        case 0:
            unit_status_browser();
            break;
        case 1:
            inventory_transfer();
            break;
        case 2:
            temple_revive_menu();
            break;
        default:
            class_promote_menu();
            break;
        }
        dialog_backdrop_load(g_dialog_backdrop_ids[4]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        586, g_pkg_fdtxt0);
        dato_frame_stamp(0);
    }
    scene_page_close();
    return 0;
}

/* 0x2670E scene_page_dispatch（2026-09-08 反汇编全解重构）：music(-1,0)
 * → 相位 2 = 出击确认：对话框 blk75（0x2674C push 4Bh，旧实现误 0）+
 * 文本 513@vram+38175（0xA951F，旧实现误 38164）+ 闭口肖像 +
 * g_field_map 哨兵 + confirm；取消/否 → xor eax 返 0 留城；是 → 超
 * 16/20 人先 roster_select_menu（返 0 = 留城重选；仅返 0/1，无 -1）→
 * ebp=1 后【不提前返回】，落入公共推近转场 zoom 到出击位（pos=6*bg+2）
 * 后跳过 music(10) 直接 free/ent_table=0 返 ebp。
 * 其余相位：快照 64000B → 10 步推近（i=1..10：中心自屏心插值到立绘位
 * cx=20480+((i*(x-150)/10)<<7)、cy=12800+((i*(y-100)/10)<<7)，
 * scale=128-9i、palette=4i——旧实现固定中心 128*x 误）→ palette 64 +
 * memset 清屏（0x268E2/0x268FF，旧实现缺）→ 0=据点(13) 4=整备(11)
 * 3=商店(15) 其余含 5=商店(14) → 页返回值存 ebp → music(10,0) →
 * free 快照/ent_table=0 → 返 ebp。 */
int scene_page_dispatch(void)
{
    const uint8_t *cfg = state_config_get(g_state);
    int bg = cfg[0];
    int pos = 6 * bg + g_scene_line_idx;
    int result = 0;
    uint8_t *backdrop;

    music_play(-1, 0);
    if (g_scene_line_idx == 2) {
        int yn;

        dialog_backdrop_load(75);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38175u,
                        513, g_pkg_fdtxt0);
        dato_frame_stamp(0);             /* 0x2678B dato_frame_stamp(0) 闭口肖像 */
        g_field_map = (void *)1;     /* 0x26780 哨兵（值 1 不触发战场重绘） */
        yn = confirm_yes_no();
        g_field_map = NULL;
        confirm_close_anim();        /* 0x2679A */
        menu_buffers_teardown();     /* 0x267A5 */
        if (yn == -1 || g_menu_choice != 0)
            return 0;                /* 0x26804 xor eax,eax → 0x29867 */
        if ((g_state < 27 && g_roster_count > 16)
            || (g_state > 26 && g_roster_count > 20)) {
            g_ent_table = g_roster_table;
            result = roster_select_menu();
            g_ent_table = NULL;
            if (result == 0)
                return 0;            /* 0x267F7 test eax → 0x29867 留城 */
        }
        result = 1;                  /* 0x267FD mov ebp,1 */
    }
    /* 0x2680B 公共推近转场：原版每步画 shadow2 后 memmove 提交，宿主
     * scene_zoom_blit 直写 VRAM 等价。 */
    backdrop = malloc(FD2_VRAM_SIZE);
    if (!backdrop) {
        g_ent_table = NULL;
        return result;               /* 宿主 OOM 护栏（原版无） */
    }
    g_ent_table = g_roster_table;
    memcpy(backdrop, vram_base(), FD2_VRAM_SIZE);
    s_scene_return_backdrop = backdrop;
    if (pos >= 0 && pos < 18) {
        for (int i = 1; i <= 10; i++) {
            scene_zoom_blit(20480 + ((i * (g_scene_sprite_x[pos] - 150) / 10) << 7),
                            12800 + ((i * (g_scene_sprite_y[pos] - 100) / 10) << 7),
                            backdrop, 128 - 9 * i);
            palette_apply_range(0, 255, 4 * i);
        }
    }
    palette_apply_range(0, 255, 64);
    memset(vram_base(), 0, FD2_VRAM_SIZE);
    if (g_scene_line_idx == 0) {
        music_play(13, 0);
        result = scene_base_camp();
    } else if (g_scene_line_idx == 4) {
        music_play(11, 0);
        result = scene_prep_page();
    } else if (g_scene_line_idx == 3) {
        music_play(15, 0);
        result = scene_shop_page();
    } else if (g_scene_line_idx != 2) {
        music_play(14, 0);           /* 相位 1/5（含隐藏店） */
        result = scene_shop_page();
    }
    /* 相位 2（出击确认后）：0x26948 jz loc_2697C 跳过 music(10)。 */
    if (g_scene_line_idx != 2)
        music_play(10, 0);
    free(backdrop);
    s_scene_return_backdrop = NULL;
    g_ent_table = NULL;
    return result;
}
/* 0x2921A scene_zoom_blit（全解 2026-09-05）：最近邻缩放绘制到
 * 320×200。源采样步进 = scale/128（推近序列第 i 帧 scale=128-9*i，
 * i=0..10）；源起点 = (cx-160*scale, cy-100*scale)，带符号除 128
 * 取整；越界跳过。原版写入 g_shadow_buf2 后提交，此处直接写 VRAM。 */
void scene_zoom_blit(int cx, int cy, const void *backdrop, int scale)
{
    const uint8_t *src = (const uint8_t *)backdrop;
    uint8_t *dst = vram_base();
    int sy = cy - 100 * scale;

    if (!src)
        return;
    for (int i = 0; i < 200; i++, sy += scale, dst += 320) {
        int sx = cx - 160 * scale;
        if ((unsigned)sy >= 0x6400)
            continue;
        for (int j = 0; j < 320; j++, sx += scale) {
            if ((unsigned)sx >= 0xA000)
                continue;
            dst[j] = src[(sx >> 7) + 320 * (sy >> 7)];
        }
    }
}
void scene_menu_draw(int pass)
{
    /* 0x26CE4：据点菜单面板滑入/滑出。面板区 = 行 169..188 × x201..304
     * 填 74；四个菜单图标 = g_scene_bg_pkg 帧 3/5/7/9（2k+3），基位
     * vram+54320，滑位偏移表 @0x52408 = {-39,-13,+13,+39}：
     * pass=0 滑入（偏移/(4-j)，j=0..3 收敛到 0）、pass=1 滑出（/(j+1)
     * 发散）。图标戳印以 package_blit_frame 代原 sub_4ED34 链（LMI1
     * 帧含自带 w/h/RLE，视觉等价；4ED34 内部未展开，近似点已注）。 */
    static const int slide[4] = { -39, -13, 13, 39 };
    uint8_t *vram = vram_base();
    uint8_t *snap = malloc(64000);

    if (!snap)
        return;
    memcpy(snap, vram, 64000);
    for (int i = 0; i < 20; i++)
        memset(snap + 320 * (i + 169) + 201, 74, 104);
    for (int j = 0; j < 4; j++) {
        memcpy(vram, snap, 64000);
        for (int k = 0; k < 4; k++) {
            int frame = 2 * k + 3;
            int off = pass ? slide[k] / (j + 1) : slide[k] / (4 - j);
            /* 0x26D0E 链经 sub_4ED34（RAW 透明）——2026-09-08 更正旧
             * package_blit_frame 近似（4-op 解 RAW=噪声）。 */
            stamp_raw_transparent(package_frame_ptr(g_scene_bg_pkg, frame),
                                  vram + 54320 + off, 320);
        }
        wait_bios_ticks(1);
    }
    if (pass)
        memcpy(vram, snap, 64000);
    free(snap);
}
/* 0x27079 menu_anim_redraw（sub_26EDA 每 2-tick 窗口调用）：
 * 菜单钟 dword_53F72（53F52 奇数时进位，wrap 4，相位 3→1——立绘动画）。
 * mode 0 = 选中菜单图标脉冲：package 帧 53F52/2 + 2*choice + 3
 *   贴 vram+54320+slide[choice]（unk_52418={-39,-13,13,39}=图标静止位）；
 * mode 1/2/3 = 列表：上箭头（scroll!=0 → 帧 53F52/2+11 脉冲，否则 17）
 *   贴 vram+38698、下箭头（scroll+(mode==2?3:6)<count → 帧 53F52/2+13，
 *   否则 17）贴 vram+58218，stamp_frame_opaque（字节 RLE 帧）；
 * mode 2 另重绘前 3 行单位立绘（目录 48*slot+4*phase，sprite24_stamp
 *   0x49 满铺版）@vram+320*(26*row+117)+14——立绘随相位动；
 * mode 3 另调 roster_grid_draw(vram, choice)。 */

static void menu_anim_redraw(int mode, const uint8_t *list)
{
    static const int slide[4] = { -39, -13, 13, 39 };
    int phase;

    if (g_scene_anim_clock & 1u)
        g_roster_grid_col = (g_roster_grid_col + 1) & 3;
    phase = g_roster_grid_col == 3 ? 1 : g_roster_grid_col;
    if (mode == 0) {
        stamp_raw_transparent(package_frame_ptr(g_scene_bg_pkg,
                              g_scene_anim_clock / 2 + 2 * g_menu_choice + 3),
                              vram_base() + 54320 + slide[g_menu_choice], 320);
        return;
    }
    if (mode == 3)
        roster_grid_draw(vram_base(), g_menu_choice);
    rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg,
                     g_shop_list_scroll != 0
                         ? g_scene_anim_clock / 2 + 11 : 17),
                    vram_base() + 38698, 320);
    rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg,
                     g_shop_list_scroll + (mode == 2 ? 3 : 6)
                         < g_scene_list_count
                         ? g_scene_anim_clock / 2 + 13 : 17),
                    vram_base() + 58218, 320);
    if (mode == 2 && list) {
        int rows = g_scene_list_count > 3 ? 3 : g_scene_list_count;

        for (int i = 0; i < rows; i++) {
            const uint8_t *dir = g_standing_sprites
                + 48u * list[i + g_shop_list_scroll];
            uint32_t off = (uint32_t)dir[4 * phase]
                | ((uint32_t)dir[4 * phase + 1] << 8)
                | ((uint32_t)dir[4 * phase + 2] << 16)
                | ((uint32_t)dir[4 * phase + 3] << 24);
            sprite24_stamp(g_standing_sprites + off,
                           vram_base() + 320 * (26 * i + 117) + 14, 320);
        }
    }
}

/* 0x26EDA menu_anim_wait(mode)：动画等键（菜单/列表共用）。每 2 tick
 *（或 0x46C 回绕）推进共享钟 0x53F52（wrap 4）并重绘（上函数）+
 * 随机肖像张嘴（闭口 rand%50+8 窗倒计时 → datum 帧 3 一拍 → 回 0，
 * 再 rand%30+2）；mode 0 入场另贴全部四图标（帧 2k+3，选中 +1）。
 * 返回归一化扫描码（input_wait_key 同族：E0/52→Enter、53→Esc）。 */
static int menu_anim_wait(int mode, const uint8_t *list)
{
    int countdown = (int)(fd2_rand() % 50) + 8;
    int mouth_open = 0;
    uint32_t last;

    g_scene_anim_clock = 2;                /* 0x26F23：入场相位 */
    menu_anim_redraw(mode, list);          /* 0x26F2D：入场先重绘一次 */
    if (mode == 0) {
        static const int slide[4] = { -39, -13, 13, 39 };
        for (int i = 0; i < 4; i++)
            stamp_raw_transparent(package_frame_ptr(g_scene_bg_pkg,
                                  2 * i + 3 + (i == g_menu_choice ? 1 : 0)),
                                  vram_base() + 54320 + slide[i], 320);
    }
    last = bios_tick();
    for (;;) {
        int delta = (int)(bios_tick() - last);

        if (delta >= 2 || delta < 0) {
            g_scene_anim_clock = (g_scene_anim_clock + 1u) & 3u;
            menu_anim_redraw(mode, list);
            if (mouth_open) {
                /* 0x26FD0：DOS 直接写 VGA；宿主 VGA 扫描器会观察到
                 * 口型写入，不需要在此插入提交调用。 */
                if (dato_frame_stamp(0) == 0)
                countdown = (int)(fd2_rand() % 30) + 2;
                mouth_open = 0;
            } else if (countdown-- == 0) {
                /* 0x26FF5 dec/test 判等（非 <=0）：倒计时归零才触发，
                 * 之后重新取零扩展 fd2_rand()%30+2。 */
                if (dato_frame_stamp(3) == 0)
                mouth_open = 1;
            }
            last = bios_tick();
        }
        if (kbd_key_avail())
            break;
        host_idle_pump();   /* 宿主事件泵。原版 0x26Fxx 此处为裸轮询、
                             * 不调 idle_pump——DAC 色循环会覆盖菜单
                             * 调色板 224..239（主菜单元素消失，
                             * 2026-09-07 回归修复）。 */
    }
    return input_wait_key();
}

int  scene_menu_input(void)
{
    /* 0x26E38：←(75)/→(77) 移动 g_menu_choice 0..3 回绕（音效
     * g_pkg_fdother_31 项 0）；Enter(28)/Space(57)=1 确认；Esc(1)=-1。
     * 按键读取走 menu_anim_wait(0)（sub_26EDA mode 0）：每 2-tick 重绘
     * 选中图标脉冲帧（2c+3/2c+4 交替）+ 随机肖像张嘴——选中态由此可见
     *（旧实现裸等键无重绘，选中项不可见，2026-09-08）。 */
    int result = 0;

    do {
        int key = menu_anim_wait(0, NULL);
        switch (key) {
        case SCAN_LEFT:
            sfx_play(g_pkg_fdother_31, 0, 1);
            if (--g_menu_choice < 0)
                g_menu_choice = 3;
            break;
        case SCAN_RIGHT:
            sfx_play(g_pkg_fdother_31, 0, 1);
            if (++g_menu_choice > 3)
                g_menu_choice = 0;
            break;
        case SCAN_ENTER:
        case 57:
            result = 1;
            break;
        case SCAN_ESC:
            result = -1;
            break;
        default:
            break;
        }
    } while (!result);
    return result;
}

/* 0x29620 unit_status_browser（2026-09-08 逐指令严格重建）：
 * g_scene_list_count=g_roster_count（0x2962D，一次）→ 循环
 * {roster_pick_unit（0x29637）→ menu_buffers_teardown（0x29640：
 * 确认/取消两路均收——面板带下滑 + VRAM←快照 + free 三缓冲）→
 * -1 返出；否则 dword_53C67 围 sub_17AED 保存/恢复（0x29645 取 /
 * 0x2965E 还）→ 重载 DATO ids[0] 进说话人块（0x29664..0x2967F：
 * 原版 g_dato_speaker_blk 与状态页图标块同句柄替换式存活，状态页
 * 离开后按当前页说话人复位）→ 再选。 */
void unit_status_browser(void)
{
    g_scene_list_count = g_roster_count;         /* 0x2962D */
    for (;;) {
        int picked = roster_pick_unit();
        int origin;

        menu_buffers_teardown();                 /* 0x29640 */
        origin = dialog_portrait_origin();       /* 0x29645：edi=53C67 */
        if (picked == -1)
            return;
        sub_17AED(g_menu_choice);
        dialog_portrait_origin_set(origin);      /* 0x2965E */
        dialog_speaker_reload(g_dialog_backdrop_ids[0]);
    }
}

/* sub_29AB2：槽位摘要绘制——4 行（行 19i+119）：文本 549 槽头
 * （disp=i+1，选中色 201/未选 205）；槽进度字节==255 → 文本 514
 * （空槽）；否则文本 514+章节号（保存时的 g_state）。
 * dst：初始进 backup（0x29C6C）、移动重绘进 VRAM（0x29D39/0x29D74）。 */
static void slot_summary_draw(int sel, const uint8_t *file, uint8_t *dst)
{
    for (int i = 0; i < 4; i++) {
        int color = i == sel ? 201 : 205;
        const uint8_t *summary = file + FD2_SAVE_SLOT_BASE
                                 + FD2_SAVE_SLOT_STRIDE * i + 2560;
        int chapter = summary[0];
        uint8_t *row = dst + 320u * (unsigned)(19 * i + 119);

        /* 0x29AC7..0x29BC6 压栈：(pkg, str, dst, 320, fg=201 选中/205,
         * 76, 0, 0, 0) —— 列表族：bg=0、无打字机、即时渲染。
         * 布局：槽头 @x=10、空槽状态 @x=88、非空状态/章节 @x=40/130。 */
        g_disp_num_b = i + 1;
        text_render_box(0, 0, 0, color, 320,
                        row + 10,
                        549, g_pkg_fdtxt0);
        if (chapter == 255) {
            text_render_box(0, 0, 0, color, 320,
                            row + 88,
                            514, g_pkg_fdtxt0);
        } else {
            text_render_box(0, 0, 0, color, 320,
                            row + 40, 514 + chapter, g_pkg_fdtxt0);
            text_render_box(0, 0, 0, color, 320,
                            row + 130, 550 + chapter, g_pkg_fdtxt0);
        }
    }
}

/* 0x29BCB save_slot_pick_record(load_mode, file)（2026-09-08 逐指令
 * 严格重建，撤销本地快照近似）：三缓冲 malloc → snap=VRAM、backup=
 * snap → 面板帧 16（pkg+[pkg+0x46]，字节 RLE）盖 backup+35845 →
 * slot_summary_draw(choice, backup) → 开面板音 fdother31 项 5 →
 * 6 步滑入（sub_1974C 行 177..112）→ 等键循环：load_mode≠0 =
 * wait_key_anim(0)（0x29CBD）；==0 = 裸 int386(0x16) + E0/52→Enter、
 * 53→Esc 归一化（0x29CC9..0x29D11 = input_wait_key）。Down(0x50)
 * choice+1（==3 停）、Up(0x48) choice-1（==0 停，不回绕），移动音
 * fdother31 项 7，重绘 summary→VRAM（面板静止不重画）；Enter/Space=1、
 * Esc=-1。不重置 g_menu_choice/scroll（继承调用方——write/load 两
 * 菜单入口置 0）。尾部尾跳 loc_26A73（0x26996 收尾函数的返回
 * 尾声）：不还原不 free——调用方 0x26996 收（write 0x29800/0x29853、
 * load 0x298FB、boot 0x260AB）。 */
int save_slot_pick_record(int load_mode, const uint8_t *file)
{
    if (!file)
        return -1;
    g_menu_work_buf = malloc(FD2_VRAM_SIZE);
    g_menu_snap_buf = malloc(FD2_VRAM_SIZE);
    g_screen_backup = malloc(FD2_VRAM_SIZE);
    if (!g_menu_work_buf || !g_menu_snap_buf || !g_screen_backup) {
        free(g_menu_work_buf);
        free(g_menu_snap_buf);
        free(g_screen_backup);
        g_menu_work_buf = g_menu_snap_buf = g_screen_backup = NULL;
        return -1;
    }
    memcpy(g_menu_snap_buf, vram_base(), FD2_VRAM_SIZE);   /* 0x29C29 */
    memcpy(g_screen_backup, g_menu_snap_buf, FD2_VRAM_SIZE);
    /* 0x29C4A：帧 16（310x86 字节 RLE）盖 backup+35845。解码器族：
     * blk13 槽面板是【字节 RLE】（rle_blit_opaque），非 blk7 标题
     * 菜单帧族的 4-op 控制 RLE（2026-09-07 花屏定案）。 */
    rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 16),
                    g_screen_backup + 35845, 320);
    slot_summary_draw(g_menu_choice, file, g_screen_backup); /* 0x29C6C */
    sfx_play(g_pkg_fdother_31, 5, 1);                       /* 0x29C8A */
    for (int i = 5; i >= 0; i--) {          /* 0x29C92：滑入 177..112 */
        dialog_band_blit(13 * i + 112, g_menu_work_buf,
                         g_screen_backup, g_menu_snap_buf);
    }
    for (;;) {
        /* 0x29CB9：load 分支 wait_key_anim(0)（原版 0x29CBF）；
         * 存档分支 0x29CC9 int386 直读（无空闲回调）——不用
         * input_wait_key（DAC 色循环会覆盖槽面板调色板，
         * 2026-09-07 主菜单回归同族根因）。 */
        int key = load_mode ? wait_key_anim(0) : input_read_key_blocking();

        if (key == SCAN_DOWN && g_menu_choice != 3) {
            sfx_play(g_pkg_fdother_31, 7, 1);
            g_menu_choice++;
            slot_summary_draw(g_menu_choice, file, vram_base()); /* 0x29D44 */
        } else if (key == SCAN_UP && g_menu_choice) {
            sfx_play(g_pkg_fdother_31, 7, 1);
            g_menu_choice--;
            slot_summary_draw(g_menu_choice, file, vram_base());
        } else if (key == SCAN_ENTER || key == 57) {
            return 1;
        } else if (key == SCAN_ESC) {
            return -1;
        }
    }
}

/* 0x2968D save_slot_write_menu(pick_mode)：4 槽位章节快照写入。读旧档
 * 解码作底（无档全 FF）→ save_slot_pick(pick_mode) 选槽 → 槽区 2600B =
 * roster 2560 + 摘要 10B（[0]g_state [1]roster_count [2..5]g_gold u32
 * [6]51AAB [7]53AF9 [8]music_gate [9]sfx_gate）→ +22983 校验和 → 编码 →
 * 整档 fwrite → 再解码回内存副本；非编队界面状态写后对话框（说话人
 * 129）+文本 660@vram+38092+闭口肖像+wait_key_anim(0)。
 * pick_mode 由调用方穿透（0x2940E 据点=1 走 wait_key_anim 取键、
 * 0x26331 编队态=0 走裸 int16）。 */
void save_slot_write_menu(int pick_mode)
{
    uint8_t *file = malloc(FD2_SAVE_SIZE);
    FILE *fp;

    if (!file)
        return;
    fp = fopen(save_file_path(), "rb");
    if (fp && fread(file, 1, FD2_SAVE_SIZE, fp) == FD2_SAVE_SIZE) {
        save_transform(file, FD2_SAVE_SIZE);
    } else {
        memset(file, 255, FD2_SAVE_SIZE);
    }
    if (fp)
        fclose(fp);

    g_menu_choice = 0;
    for (;;) {
        int picked = save_slot_pick_record(pick_mode, file);

        if (picked != -1) {
            {
                uint8_t *slot = file + FD2_SAVE_SLOT_BASE
                                + FD2_SAVE_SLOT_STRIDE * g_menu_choice;
                memcpy(slot, g_roster_table, 2560);
                slot[2560] = (uint8_t)g_state;
                slot[2561] = (uint8_t)g_roster_count;
                slot[2562] = (uint8_t)((uint32_t)g_gold & 0xFF);
                slot[2563] = (uint8_t)((uint32_t)g_gold >> 8);
                slot[2564] = (uint8_t)((uint32_t)g_gold >> 16);
                slot[2565] = (uint8_t)((uint32_t)g_gold >> 24);
                slot[2566] = g_save_flag_51aab;
                slot[2567] = g_save_flag_53af9;
                slot[2568] = (uint8_t)g_music_gate;
                slot[2569] = (uint8_t)g_sfx_gate;
            }
            *(uint32_t *)(file + FD2_SAVE_SIZE - 4) =
                save_sum(file, FD2_SAVE_SIZE);
            save_transform(file, FD2_SAVE_SIZE);
            fp = fopen(save_file_path(), "wb");
            if (fp) {
                fwrite(file, 1, FD2_SAVE_SIZE, fp);
                fclose(fp);
            }
            save_transform(file, FD2_SAVE_SIZE);
            if (!g_state_roster_menu[g_state]) {
                menu_buffers_teardown();   /* 0x29800：对话前收选人器 */
                dialog_backdrop_load(0x81);
                text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                                660, g_pkg_fdtxt0);
                dato_frame_stamp(0);
                wait_key_anim(0);
            }
        }
        menu_buffers_teardown();           /* 0x29853：循环底双路收
                                            *（取消收选人器 / 660 后收框）*/
        if (picked == -1)
            break;
    }
    free(file);
}

/* 0x2986F save_slot_load_menu：槽位快照恢复（章节重玩/回据点）。
 * 读 FD2.SAV 解码（无档全 FF）→ save_slot_pick(1) → 槽进度字节
 * （=保存时 g_state）==255 空槽重选；g_state_roster_menu[saved_state]
 * → 对话框(129)+文本 479@vram+38092 拒绝重选；否则恢复 roster/状态/
 * 计数/金钱/开关 → free 立绘缓存 → FDICON.B24 逐 roster 重建 →
 * g_shop_stock_ptr=state_config_get(g_state) → 文本 478@38092（两路
 * 均接闭口肖像 + wait_key_anim + teardown）→ 退出循环。恢复后据点
 * 菜单继续；状态机在出击（scene_page_dispatch 返 1）后才切换到恢复态。 */
int save_slot_load_menu(void)
{
    uint8_t *file = malloc(FD2_SAVE_SIZE);
    FILE *fp;
    int restored = 0;

    if (!file)
        return 0;
    fp = fopen(save_file_path(), "rb");
    if (fp && fread(file, 1, FD2_SAVE_SIZE, fp) == FD2_SAVE_SIZE) {
        save_transform(file, FD2_SAVE_SIZE);
    } else {
        memset(file, 255, FD2_SAVE_SIZE);
    }
    if (fp)
        fclose(fp);

    g_menu_choice = 0;
    for (;;) {
        const uint8_t *slot;
        int saved_state;
        int picked = save_slot_pick_record(1, file);

        menu_buffers_teardown();       /* 0x298FB：选完即收（两路） */
        if (picked == -1)
            break;
        slot = file + FD2_SAVE_SLOT_BASE + FD2_SAVE_SLOT_STRIDE * g_menu_choice;
        saved_state = slot[2560];
        if (saved_state == 255)
            continue;
        if (g_state_roster_menu[saved_state]) {
            dialog_backdrop_load(0x81);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                            479, g_pkg_fdtxt0);
            dato_frame_stamp(0);
            wait_key_anim(0);
            menu_buffers_teardown();   /* 0x29A97 */
            continue;
        }
        memcpy(g_roster_table, slot, 2560);
        g_state = slot[2560];
        g_roster_count = slot[2561];
        g_gold = (int32_t)((uint32_t)slot[2562]
                 | ((uint32_t)slot[2563] << 8)
                 | ((uint32_t)slot[2564] << 16)
                 | ((uint32_t)slot[2565] << 24));
        g_save_flag_51aab = slot[2566];
        g_save_flag_53af9 = slot[2567];
        g_music_gate = slot[2568];
        g_sfx_gate = slot[2569];
        {
            /* 0x299CE：free 旧立绘 + 图标缓存计数清零后重建。 */
            FILE *icons = fopen("FDICON.B24", "rb");
            icon_cache_reset();
            free(g_standing_sprites);
            g_standing_sprites = NULL;
            if (icons) {
                for (int i = 0; i < g_roster_count; i++) {
                    icon_load_entry(g_roster_table[i][7], icons);
                    /* 同 statemachine 场景入口：目录槽按名册位置别名，
                     * 防转职业 rec[7] 重复时去重压缩槽位错画。 */
                    (void)icon_directory_alias(i, g_roster_table[i][7]);
                }
                fclose(icons);
            }
        }
        g_shop_stock_ptr = state_config_get(g_state);
        dialog_backdrop_load(0x81);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        478, g_pkg_fdtxt0);
        dato_frame_stamp(0);
        wait_key_anim(0);
        menu_buffers_teardown();       /* 0x29A97（原版裸 0x26996） */
        restored = 1;
        break;
    }
    free(file);
    return restored;
}

/* 武器类适性表 @0x6188A（sub_4E88E = base+7*职业系，直接按系索引）：
 * 29 行×7B = 6 个可用武器类别（FF=无）+ 常量 01，0x6188A..0x61955 与
 * 攻击动画指针表无缝相接（2026-09-06 IDA 字节定案；旧 [16][7]+family-1
 * 双错——截断且移位——已修正）。武器表 [0]=类别判定用。 */
static const uint8_t g_weapon_affinity_lut[29][7] = {
    { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x01 },
    { 0x01,0x15,0x16,0xFF,0xFF,0xFF,0x01 },
    { 0x04,0x15,0x16,0x17,0x18,0xFF,0x01 },
    { 0x03,0x15,0x16,0x17,0xFF,0xFF,0x01 },
    { 0x05,0x15,0x16,0xFF,0xFF,0xFF,0x01 },
    { 0x06,0x15,0x19,0xFF,0xFF,0xFF,0x01 },
    { 0x06,0x15,0x19,0xFF,0xFF,0xFF,0x01 },
    { 0x02,0x15,0x16,0xFF,0xFF,0xFF,0x01 },
    { 0x07,0x15,0x1A,0x0D,0xFF,0xFF,0x01 },
    { 0x01,0x09,0x15,0x16,0xFF,0xFF,0x01 },
    { 0x04,0x15,0x16,0x17,0x18,0xFF,0x01 },
    { 0x03,0x0A,0x15,0x16,0x17,0x18,0x01 },
    { 0x05,0x0C,0x15,0x16,0xFF,0xFF,0x01 },
    { 0x06,0x15,0x19,0xFF,0xFF,0xFF,0x01 },
    { 0x06,0x15,0x19,0xFF,0xFF,0xFF,0x01 },
    { 0x01,0x0B,0x15,0x16,0xFF,0xFF,0x01 },
    { 0x07,0x15,0x1A,0xFF,0xFF,0xFF,0x01 },
    { 0x01,0x09,0x15,0x16,0x17,0xFF,0x01 },
    { 0x04,0x15,0x16,0x17,0x18,0xFF,0x01 },
    { 0x03,0x15,0x16,0x17,0xFF,0xFF,0x01 },
    { 0x05,0x15,0x16,0x17,0xFF,0xFF,0x01 },
    { 0x06,0x15,0x19,0xFF,0xFF,0xFF,0x01 },
    { 0x06,0x15,0x19,0xFF,0xFF,0xFF,0x01 },
    { 0x01,0x0B,0x15,0x16,0xFF,0xFF,0x01 },
    { 0x07,0x15,0x1A,0xFF,0xFF,0xFF,0x01 },
    { 0x08,0x1B,0xFF,0xFF,0xFF,0xFF,0x01 },
    { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x01 },
    { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x01 },
    { 0x07,0x15,0x1A,0xFF,0xFF,0xFF,0x01 },
};

/* 0x1C1C3 weapon_affinity_check(unit, item)：职业系 ent[+32] 的
 * 6 项可用类别表包含 weapon[0] → 1。 */
int weapon_affinity_check(int unit, uint8_t item)
{
    const uint8_t *w = weapon_table_entry(item);
    int family;

    if (!w || !g_ent_table || unit < 0)
        return 0;
    /* Scene menus switch g_ent_table to g_roster_table but deliberately keep
     * g_ent_count at zero (the count belongs to the battle entity table).
     * IDA 0x1C1C3 has no count gate and indexes the caller-provided unit;
     * use the active table's own bound so every roster member is considered. */
    if (g_ent_table == g_roster_table) {
        if (unit >= g_roster_count)
            return 0;
    } else if (unit >= g_ent_count) {
        return 0;
    }
    family = g_ent_table[unit][32];
    if ((unsigned)family > 28)
        return 0;
    for (int i = 0; i < 6; i++)
        if (w[0] == g_weapon_affinity_lut[family][i])
            return 1;
    return 0;
}

/* 0x1C142 equip_slot(unit, slot)：卸下其它同半区（武器<128 / 防具
 * ≥128，以新道具 id 判）已装备槽（数量位清 0），目标槽数量置 0x40。 */
int equip_slot(int unit, int slot)
{
    uint8_t *rec = g_ent_table[unit];
    uint8_t new_id = rec[11 + 2 * slot];

    for (int i = 0; i < 8; i++) {
        uint8_t *qty = rec + 10 + 2 * i;
        if ((*qty & 0x40)
            && ((new_id < 128 && rec[11 + 2 * i] < 128)
             || (new_id >= 128 && rec[11 + 2 * i] >= 128)))
            *qty = 0;
    }
    rec[10 + 2 * slot] = 64;
    return slot;
}

/* 0x2872B shop_weapon_menu（买入主流程，全解 2026-09-05）：
 * 循环 {恢复记忆光标/滚动 → shop_list_open/pick（卖价模式 0）→
 * 取消即返；item=stock[cursor]，名称 id=item+181、价格=weapon[19..20]
 * → 武器（类别<0x20）按适性收集合格者（空→文本 nobody 表）→
 * 确认（confirm 表）→ 金币不足（insufficient 表）→ 接收者：防具
 * （≥0x20）任意角色 / 武器 eligible_unit_pick → 满包拒绝（fullbag
 * 表 + 人名 id=ent[+7]+1）→ ent_inventory_add → 武器追问"立即装备"
 * （equip 表；是→equip_slot(末槽)+ent_recompute_derived）→
 * shopkeeper_anim + gold_spend_animate }。各相位文本表 @0x52428/
 * 0x52434/0x52440/0x5244C/0x5238D 逐字嵌入，索引式与原版一致。 */
/* confirm_yes_no/confirm_close_anim 已在 text.c 全解重建（0x19953/
 * 0x197E5），旧静默近似 confirm_yn 撤销。 */

/* 0x2872B shop_weapon_menu（买入主流程；对话链/收框纪律 2026-09-06
 * 逐点对齐）：循环 {恢复记忆光标/滚动 → shop_list_open/pick（卖价
 * 模式 0）→ T1（picker 释放）→ 取消即返；item=stock[cursor]，名称
 * id=item+181、价格=weapon[19..20] → 武器（类别<0x20）按适性收集
 * 合格者（空→文本 nobody 表@38092+stamp+wait(1)+T2）→ 确认（confirm
 * 表@38092+stamp+confirm+close_anim；否/Esc→T2）→ 金币不足
 * （insufficient 表@**50252 第三行不重开窗**+stamp+wait(1)+T2）→
 * T3（选人前收框）→ 接收者：防具（≥0x20）任意角色 / 武器
 * eligible_unit_pick → T4 → 满包拒绝（fullbag 表@38092 重开窗+
 * stamp+wait(1)+T2）→ ent_inventory_add → 武器追问"立即装备"
 * （equip 表@38092+stamp+confirm+close_anim；是→equip_slot(末槽)+
 * ent_recompute_derived）→ T5 → shopkeeper_anim + gold_spend_
 * animate }。各相位文本表 @0x52428/0x52434/0x52440/0x5244C/0x5238D
 * 逐字嵌入，索引式与原版一致。 */
void shop_weapon_menu(int count, const uint8_t *stock)
{
    static const uint16_t txt_confirm[6]  = { 1, 0x1F6, 1, 0x1B7, 1, 0x1B7 };
    static const uint16_t txt_nogold[6]   = { 1, 0x1F8, 1, 0x1B6, 1, 0x1B6 };
    static const uint16_t txt_nobody[6]   = { 1, 0x1F9, 1, 0x1FB, 1, 0x1B5 };
    static const uint16_t txt_equip[6]    = { 1, 0x1FB, 1, 0x1FB, 1, 0x1FB };
    static const uint16_t txt_fullbag[5]  = { 1, 0x1FA, 1, 0x1FA, 0x1FA };
    int line = g_scene_line_idx;

    for (;;) {
        const uint8_t *w;
        uint8_t units[32];
        int n_units = 0, old_scene_list_count, pick, item, price;
        uint8_t *rec;
        int yn;

        g_menu_choice = g_shop_cursor_mem;
        g_shop_list_scroll = g_shop_scroll_mem;
        shop_list_open(count, stock, 0);
        pick = shop_list_pick(count, stock, 0);
        g_shop_cursor_mem = g_menu_choice;
        g_shop_scroll_mem = g_shop_list_scroll;
        menu_buffers_teardown();           /* T1 0x28873：picker 释放 */
        if (pick == -1)
            return;

        item = stock[g_shop_cursor_mem];
        w = weapon_table_entry(item);
        if (!w)
            continue;
        g_disp_num_a = item + 181;
        price = w[19] | (w[20] << 8);
        g_disp_num_b = price;

        for (int i = 0; i < g_roster_count; i++) {
            if ((w[0] < 0x20 && weapon_affinity_check(i, (uint8_t)item) == 1)
             || w[0] >= 0x20)
                units[n_units++] = (uint8_t)i;
        }
        if (w[0] < 0x20 && !n_units) {
            dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                            txt_nobody[line], g_pkg_fdtxt0);
            dato_frame_stamp(0);
            wait_key_anim(1);              /* 0x28814 族：指示符闪烁 */
            menu_buffers_teardown();       /* T2 0x28826 */
            continue;
        }
        old_scene_list_count = g_scene_list_count; /* 0x288B9 */
        g_scene_list_count = n_units;      /* 0x288BF：选人窗长度 */
        dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        txt_confirm[line], g_pkg_fdtxt0);
        dato_frame_stamp(0);
        yn = confirm_yes_no();
        confirm_close_anim();
        if (yn == -1 || g_menu_choice == 1) {
            menu_buffers_teardown();       /* T2 0x28826 */
            continue;
        }
        if (g_gold < price) {
            /* 0x28960：同窗第三行（+50252），确认文本保留其上 */
            text_render_box(1, 19, 74, 205, 320, vram_base() + 50252u,
                            txt_nogold[line], g_pkg_fdtxt0);
            dato_frame_stamp(0);
            wait_key_anim(1);
            menu_buffers_teardown();       /* T2 0x28826 */
            continue;
        }
        menu_buffers_teardown();           /* T3 0x28985：金币过检，选人前收框 */

        {
            /* 特殊/防具 0x289AC：全名册网格选人（严格 roster_pick_unit，
             * 2026-09-08——三缓冲/滑入/收框全对齐；选后 0x289BD 恢复
             * 进入选人器前的 g_scene_list_count，再由 0x289C3 收框）。 */
            int picked;

            if (w[0] >= 0x20) {
                g_scene_list_count = g_roster_count;
                picked = roster_pick_unit();
            } else {
                picked = eligible_unit_pick(n_units, units, item);
            }
            g_scene_list_count = old_scene_list_count; /* 0x289BD */
            if (picked != 1) {
                menu_buffers_teardown();       /* T4 0x289C3 */
                continue;
            }
        }
        if (w[0] >= 0x20) {
            for (int i = 0; i < g_roster_count; i++)
                units[i] = (uint8_t)i;
            n_units = g_roster_count;
        }
        menu_buffers_teardown();           /* T4：选人后收框 */
        rec = g_ent_table[units[g_menu_choice]];
        if (inventory_used_count(units[g_menu_choice]) == 8) {
            g_disp_num_a = rec[7] + 1;
            dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                            txt_fullbag[line], g_pkg_fdtxt0);
            dato_frame_stamp(0);
            wait_key_anim(1);
            menu_buffers_teardown();       /* T2 */
            continue;
        }
        ent_inventory_add(units[g_menu_choice], item);
        if (w[0] < 0x20) {
            dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                            txt_equip[line], g_pkg_fdtxt0);
            dato_frame_stamp(0);
            yn = confirm_yes_no();
            confirm_close_anim();
            if (yn != -1 && g_menu_choice == 0) {
                int used = inventory_used_count(units[g_menu_choice]);
                equip_slot(units[g_menu_choice], used - 1);
                ent_recompute_derived(units[g_menu_choice]);
            }
            menu_buffers_teardown();       /* T5 0x28B1C */
        }
        shopkeeper_anim();
        gold_spend_animate(price);
    }
}
/* 0x2810B roster_grid_draw（2026-09-06 静态定案实现；调用方
 * roster_pick_unit@0x27D33 见下——2026-09-08 严格重建）：
 * min(count,6/5 滚动边界) 行两列网格；立绘 = g_standing_sprites
 * [48*(i+scroll)+4*col]（col = g_roster_grid_col，==3→1）经
 * sprite24_stamp 贴 (132*(i%2)+x+14, 26*(i/2)+117)；名字
 * text_render_box(rec[+8]+1, x+132*(i%2)+320*(26*(i/2)+121)+40，
 * 色 205/201 选中）。 */
void roster_grid_draw(uint8_t *dst, int selected)
{
    int col = g_roster_grid_col;

    if (col == 3)
        col = 1;
    int rows = g_roster_count > 6 ? (g_shop_list_scroll + 6 > g_roster_count ? 5 : 6) : g_roster_count;
    for (int i = 0; i < rows; i++) {
        const uint8_t *rec = g_ent_table[i + g_shop_list_scroll];
        int x = 132 * (i % 2);
        if (g_standing_sprites) {
            const uint8_t *spr = g_standing_sprites
                + (uint32_t)(g_standing_sprites[48 * (i + g_shop_list_scroll) + 4 * col]
                  | ((uint32_t)g_standing_sprites[48 * (i + g_shop_list_scroll) + 4 * col + 1] << 8)
                  | ((uint32_t)g_standing_sprites[48 * (i + g_shop_list_scroll) + 4 * col + 2] << 16)
                  | ((uint32_t)g_standing_sprites[48 * (i + g_shop_list_scroll) + 4 * col + 3] << 24));
            sprite24_stamp(spr, dst + x + 320 * (26 * (i / 2) + 117) + 14, 320);
        }
        /* 0x28197 压栈：列表族 (a14=0, a2=0, bg=0, fg=201 选中/205)。 */
        text_render_box(0, 0, 0,
                        i + g_shop_list_scroll == selected ? 201 : 205,
                        320, dst + x + 320 * (26 * (i / 2) + 121) + 40,
                        rec[8] + 1, g_pkg_fdtxt0);
    }
}

/* 0x27D33 roster_pick_unit（2026-09-08 逐指令严格重建，撤销
 * eligible_unit_pick 网格近似）：三缓冲 malloc（work/snap/backup =
 * 0x53C5B/53C5F/53C63）→ snap=VRAM、backup=snap → 滚动/选择清零 →
 * 面板帧 16（pkg+[pkg+0x46]，字节 RLE）不透明盖 backup+35845 →
 * roster_grid_draw(backup) → 6 步滑入（sub_1974C 行 177..112 步距 13）
 * → sub_26EDA(3) 等键循环：→=+1（==count-1 停）、←=-1（==0 停）、
 * ↑=-2（<2 停）、↓=+2（count-2<=choice 停），移动音效 fdother31
 * 项 0；choice 出 6 槽窗（±2 列网格）→ scroll±2 +
 * shop_list_scroll_down/up 动画；每次移动 roster_grid_draw(VRAM)；
 * Enter/Space=1、Esc=-1。尾部不 free 不还原（原版 loc_27F45 直返）
 * ——三缓冲与画面由调用方 menu_buffers_teardown（0x26996）收。 */
int roster_pick_unit(void)
{
    g_menu_work_buf = malloc(FD2_VRAM_SIZE);
    g_menu_snap_buf = malloc(FD2_VRAM_SIZE);
    g_screen_backup = malloc(FD2_VRAM_SIZE);
    if (!g_menu_work_buf || !g_menu_snap_buf || !g_screen_backup) {
        free(g_menu_work_buf);
        free(g_menu_snap_buf);
        free(g_screen_backup);
        g_menu_work_buf = g_menu_snap_buf = g_screen_backup = NULL;
        return -1;
    }
    memcpy(g_menu_snap_buf, vram_base(), FD2_VRAM_SIZE);   /* 0x27D87 */
    memcpy(g_screen_backup, g_menu_snap_buf, FD2_VRAM_SIZE);
    g_shop_list_scroll = 0;                /* 0x27DA8 */
    g_menu_choice = 0;                     /* 0x27DAE */
    /* 0x27DB4：面板帧 = pkg+[pkg+0x46]（帧表 6+4*16 槽的字节 RLE 帧）*/
    rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 16),
                    g_screen_backup + 35845, 320);
    roster_grid_draw(g_screen_backup, g_menu_choice);   /* 0x27DE1 */
    for (int i = 5; i >= 0; i--) {         /* 0x27DE9：滑入 177..112 */
        dialog_band_blit(13 * i + 112, g_menu_work_buf,
                         g_screen_backup, g_menu_snap_buf);
    }
    for (;;) {
        int key = menu_anim_wait(3, NULL); /* 0x27E10：sub_26EDA(3) */

        if (key == SCAN_RIGHT) {
            if (g_menu_choice == g_roster_count - 1)
                continue;                  /* 0x27E2B：末位停 */
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice++;
            if (g_menu_choice - g_shop_list_scroll >= 6) {  /* 0x27E49 */
                g_shop_list_scroll += 2;
                shop_list_scroll_down();
            }
        } else if (key == SCAN_LEFT) {
            if (g_menu_choice == 0)
                continue;
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice--;
            if (g_menu_choice < g_shop_list_scroll) {       /* 0x27EA7 */
                g_shop_list_scroll -= 2;
                shop_list_scroll_up();
            }
        } else if (key == SCAN_UP) {
            if (g_menu_choice < 2)
                continue;                  /* 0x27EC7 */
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice -= 2;
            if (g_menu_choice < g_shop_list_scroll) {
                g_shop_list_scroll -= 2;
                shop_list_scroll_up();
            }
        } else if (key == SCAN_DOWN) {
            if (g_roster_count - 2 <= g_menu_choice)
                continue;                  /* 0x27EFC */
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice += 2;
            if (g_menu_choice - g_shop_list_scroll >= 6) {
                g_shop_list_scroll += 2;
                shop_list_scroll_down();
            }
        } else if (key == SCAN_ENTER || key == 57) {
            return 1;                      /* 0x27F2C */
        } else if (key == SCAN_ESC) {
            return -1;                     /* 0x27F38 */
        } else {
            continue;
        }
        roster_grid_draw(vram_base(), g_menu_choice);   /* 0x27E65 */
    }
}

/* 0x28CBD shop_item_sell（收框纪律 2026-09-06 对齐）：循环
 * {roster_pick_unit → T1 → 取消即返；收集 8 槽道具（数量≥0；空→
 * 文本 509@38092+人名 ent[+7]+1+stamp+wait(1)，窗保持重选）→
 * shop_list_open/pick（sell=1）→ T2（取消/确认两路）→ 价格 =
 * weapon[19..20]×3/4 → 确认（文本表 {508,508,508,659,508,508}@
 * 38092+stamp+confirm+close_anim；否/Esc→T3 回环）→ 是：T3' →
 * shopkeeper_anim + gold_gain_animate + inventory_remove_item +
 * ent_recompute_derived}。 */
void shop_item_sell(void)
{
    static const uint16_t txt_confirm[6] = { 508, 508, 508, 659, 508, 508 };
    int line = g_scene_line_idx;

    for (;;) {
        uint8_t items[8];
        uint8_t *rec;
        int n_items = 0, unit;
        int item, price;
        const uint8_t *w;

        {
            int picked = roster_pick_unit();

            menu_buffers_teardown();       /* T1 0x28D80：picker 释放 */
            if (picked == -1)
                return;
        }
        unit = g_menu_choice;
        rec = g_ent_table[unit];
        n_items = 0;
        for (int i = 0; i < 8; i++)
            if ((int8_t)rec[10 + 2 * i] >= 0)
                items[n_items++] = rec[11 + 2 * i];
        if (!n_items) {
            g_disp_num_a = rec[7] + 1;
            dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                            509, g_pkg_fdtxt0);
            dato_frame_stamp(0);
            wait_key_anim(1);
            continue;                      /* 0x28D96 族：窗保持重选 */
        }
        g_menu_choice = 0;
        g_shop_list_scroll = 0;
        shop_list_open(n_items, items, 1);
        if (shop_list_pick(n_items, items, 1) == -1) {
            menu_buffers_teardown();       /* T2 0x28E10：picker 释放 */
            continue;
        }
        menu_buffers_teardown();           /* T2 */
        item = items[g_menu_choice];
        w = weapon_table_entry(item);
        price = (w[19] | (w[20] << 8)) * 3 / 4;
        g_disp_num_a = item + 181;
        g_disp_num_b = price;
        dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        txt_confirm[line], g_pkg_fdtxt0);
        dato_frame_stamp(0);
        {
            int yn = confirm_yes_no();

            confirm_close_anim();
            if (yn == -1 || g_menu_choice) {
                menu_buffers_teardown();   /* T3 0x28E63 */
                continue;
            }
        }
        menu_buffers_teardown();           /* T3' 0x28EC7：成交收框——原版
                                              先于 keeper_anim/gold_gain；
                                              置于动画后会把整屏恢复到确认
                                              窗快照（旧金币），显示回跳 */
        shopkeeper_anim();
        gold_gain_animate(price);
        /* 卖出即移除该道具（首个同 id 槽） */
        for (int i = 0; i < 8; i++) {
            if ((int8_t)rec[10 + 2 * i] >= 0 && rec[11 + 2 * i] == item) {
                inventory_remove_item(unit, i);
                break;
            }
        }
        ent_recompute_derived(unit);
    }
}

/* 0x28EFE shop_item_use：循环 {g_scene_list_count=g_roster_count
 * (0x28F0E) → roster_pick_unit → T(0x28F1A，两路) → 取消即返 →
 * 53C67 围 item_equip_menu(0x1BFFE 直调) 保存/恢复（0x28F24/0x28F38，
 * 装备菜单画单位面板动 DATO 句柄）→ 重载店主说话人块
 * ids[g_scene_line_idx]（0x28F3E..0x28F5E）}。 */
void shop_item_use(void)
{
    for (;;) {
        int picked, origin;

        g_scene_list_count = g_roster_count;    /* 0x28F0E */
        picked = roster_pick_unit();
        menu_buffers_teardown();       /* 0x28F1A：picker 释放 */
        if (picked == -1)
            return;
        origin = dialog_portrait_origin();      /* 0x28F24 */
        item_equip_menu(g_menu_choice);
        dialog_portrait_origin_set(origin);     /* 0x28F38 */
        dialog_speaker_reload(g_dialog_backdrop_ids[g_scene_line_idx]);
    }
}

/* 0x28F65 inventory_transfer（2026-09-12 逐指令复核重对齐）：循环顶
 * 文本 512@38092+stamp+wait(1)+T1 → roster_pick_unit+T2 → 空道具→
 * g_disp_num_a=src[7]+1+文本 511@38092+stamp+wait(1)+T 后回循环顶
 * （0x28F9E..0x2900A，非退出）→ shop_list 选道具（sell=1 布局）→
 * T3（取消/确认两路）→ 文本 510@38092+stamp+wait(1)+T4 →
 * roster_pick_unit+T5 → 满包拒绝（g_disp_num_a=dst[7]+1，word_5238D
 * 表@38092 重开窗+stamp+wait(1)+T）→ inventory_remove_item(src)+
 * ent_inventory_add(dst)+ent_recompute_derived(仅 src，0x2920C)
 * → 0x29215 跳 loc_2900F 回循环顶（无"完成"文本）。 */
void inventory_transfer(void)
{
    for (;;) {
        uint8_t items[8];
        uint8_t *src;
        int n_items, src_unit, item, dst_unit;

        dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        512, g_pkg_fdtxt0);
        dato_frame_stamp(0);
        wait_key_anim(1);
        menu_buffers_teardown();       /* T1 0x2900A */
        if (roster_pick_unit() == -1) {
            menu_buffers_teardown();   /* T2 */
            return;
        }
        menu_buffers_teardown();       /* T2 */
        src_unit = g_menu_choice;
        src = g_ent_table[src_unit];
        n_items = 0;
        for (int i = 0; i < 8; i++)
            if ((int8_t)src[10 + 2 * i] >= 0)
                items[n_items++] = src[11 + 2 * i];
        if (!n_items) {
            /* 0x28F9E：空道具不是退出——文本 511 盖源角色名后回循环顶 */
            g_disp_num_a = src[7] + 1;
            dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                            511, g_pkg_fdtxt0);
            dato_frame_stamp(0);
            wait_key_anim(1);
            menu_buffers_teardown();   /* 0x2900A */
            continue;
        }
        g_menu_choice = 0;
        g_shop_list_scroll = 0;
        shop_list_open(n_items, items, 1);
        if (shop_list_pick(n_items, items, 1) == -1) {
            menu_buffers_teardown();   /* T3：picker 释放 */
            continue;
        }
        menu_buffers_teardown();       /* T3 */
        item = items[g_menu_choice];
        dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        510, g_pkg_fdtxt0);
        dato_frame_stamp(0);
        wait_key_anim(1);
        menu_buffers_teardown();       /* T4 0x29062 */
        if (roster_pick_unit() == -1) {
            menu_buffers_teardown();   /* T5 */
            continue;
        }
        menu_buffers_teardown();       /* T5 */
        dst_unit = g_menu_choice;
        if (inventory_used_count(dst_unit) == 8) {
            g_disp_num_a = g_ent_table[dst_unit][7] + 1;
            dialog_backdrop_load(g_dialog_backdrop_ids[g_scene_line_idx]);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                            1, g_pkg_fdtxt0);      /* 满包：fullbag 表见批次2 */
            dato_frame_stamp(0);
            wait_key_anim(1);
            menu_buffers_teardown();   /* 0x2914A */
            continue;
        }
        for (int i = 0; i < 8; i++) {
            if ((int8_t)src[10 + 2 * i] >= 0 && src[11 + 2 * i] == item) {
                inventory_remove_item(src_unit, i);
                break;
            }
        }
        ent_inventory_add(dst_unit, item);
        /* 0x29215：原版只对源方重算，成功后跳 loc_2900F 回循环顶（无 511） */
        ent_recompute_derived(src_unit);
    }
}

int shop_stock_build(uint8_t *out)
{
    /* 0x26A0D：按相位切片货单，0xFF 终止。
     * 相位1 → 基址+3 起 12 项 / 相位3 → +15 起 8 项 / 其余(5) → +23 起 8 项。 */
    const uint8_t *src;
    int n = 0, cap = 8;

    if (g_scene_line_idx == 1) {
        src = g_shop_stock_ptr + 3;
        cap = 12;
    } else if (g_scene_line_idx == 3) {
        src = g_shop_stock_ptr + 15;
    } else {
        src = g_shop_stock_ptr + 23;
    }
    for (int i = 0; i < cap; i++) {
        if (src[i] == 0xFF)
            break;
        out[n++] = src[i];
    }
    return n;
}
/* 0x27F4A eligible_unit_pick（2026-09-08 逐指令严格重建，撤销本地
 * 快照近似）：三缓冲 malloc → snap=VRAM、backup=snap → 滚动/选择
 * 清零 → 面板帧 16 盖 backup+35845 → shop_unit_list_draw(n, units,
 * item, choice, backup) → 6 步滑入（sub_1974C 177..112）→
 * sub_26EDA(2) 等键：↓=+1（==n-1 停）、↑=-1（==0 停），sfx
 * fdother31 项 0；choice 出 3 行窗 → scroll±1 + shop_list_scroll_
 * down/up；重绘 shop_unit_list_draw→VRAM；Enter/Space=1、Esc=-1。
 * g_scene_list_count 原版由调用方置（0x287B6/0x288BF/卖出 0x28DEB），
 * fd2re 在此置 n 等价。尾部尾跳 loc_26A73（0x26996 收尾函数返回
 * 尾声）：不还原不 free——调用方 0x26996 收（0x289C3/0x2A515/
 * 0x2AAE8 族）。 */
int eligible_unit_pick(int n, const uint8_t *units, int item)
{
    if (n <= 0 || !units)
        return -1;
    g_menu_work_buf = malloc(FD2_VRAM_SIZE);
    g_menu_snap_buf = malloc(FD2_VRAM_SIZE);
    g_screen_backup = malloc(FD2_VRAM_SIZE);
    if (!g_menu_work_buf || !g_menu_snap_buf || !g_screen_backup) {
        free(g_menu_work_buf);
        free(g_menu_snap_buf);
        free(g_screen_backup);
        g_menu_work_buf = g_menu_snap_buf = g_screen_backup = NULL;
        return -1;
    }
    memcpy(g_menu_snap_buf, vram_base(), FD2_VRAM_SIZE);   /* 0x27FA8 */
    memcpy(g_screen_backup, g_menu_snap_buf, FD2_VRAM_SIZE);
    g_shop_list_scroll = 0;            /* 0x27FC9 */
    g_menu_choice = 0;                 /* 0x27FCF */
    g_scene_list_count = n;            /* 0x53F5E：箭头/立绘重绘长度 */
    /* 0x27FD5：面板帧 16（310x86 字节 RLE）盖 backup+35845。 */
    rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 16),
                    g_screen_backup + 35845, 320);
    shop_unit_list_draw(n, units, item, g_menu_choice, g_screen_backup);
    for (int i = 5; i >= 0; i--) {     /* 0x28010：滑入 177..112 */
        dialog_band_blit(13 * i + 112, g_menu_work_buf,
                         g_screen_backup, g_menu_snap_buf);
    }
    for (;;) {
        /* 0x28039 sub_26EDA(2)：每 2-tick 重绘立绘相位 + 滚动箭头脉冲。 */
        int key = menu_anim_wait(2, units);

        if (key == SCAN_DOWN) {
            if (g_menu_choice == n - 1)
                continue;              /* 0x2804F：末位停 */
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice++;
            if (g_menu_choice - g_shop_list_scroll >= 3) {  /* 0x28078 */
                g_shop_list_scroll++;
                shop_list_scroll_down();
            }
        } else if (key == SCAN_UP) {
            if (g_menu_choice == 0)
                continue;
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice--;
            if (g_menu_choice < g_shop_list_scroll) {       /* 0x280CE */
                g_shop_list_scroll--;
                shop_list_scroll_up();
            }
        } else if (key == SCAN_ENTER || key == 57) {
            return 1;
        } else if (key == SCAN_ESC) {
            return -1;
        } else {
            continue;
        }
        shop_unit_list_draw(n, units, item, g_menu_choice, vram_base());
    }
}
/* 0x2A694 promote 行渲染（转职选人器行）：每行（最多 3 行）
 * 立绘（48*unit 目录 + 4*相位，相位 = 53F72==3?1:53F72，满铺版）
 * @dst+320*(26i+117)+14；名 rec[8]+1 @+320*(26i+121)+40、职业系名
 * rec[32]+150 @+130、固定文本 593 @+175、新职业系名
 * g_promote_classinfo[news[i]] 低字节+150 @+239；列表族压栈
 * (0,0,0,201 选中/205)。列偏移 2026-09-13 反汇编复核更正：旧值
 * +20/+65 系无据近似（名压立绘、系名挤进名区——与复活选人行
 * 0x2A0C2 的 +40/+130 同族，四 call 逐 push 定案）。 */
static void promote_rows_draw(int n, const uint8_t *units,
                              const uint8_t *news, int sel, uint8_t *dst)
{
    int rows = n > 3 ? 3 : n;
    int phase = g_roster_grid_col == 3 ? 1 : g_roster_grid_col;

    for (int i = 0; i < rows; i++) {
        int unit = units[i + g_shop_list_scroll];
        uint8_t *rec = g_ent_table[unit];
        uint8_t *base = dst + 320L * (26 * i + 121);
        int fg = i + g_shop_list_scroll == sel ? 201 : 205;

        if (g_standing_sprites) {
            const uint8_t *dir = g_standing_sprites + 48u * unit;
            uint32_t off = (uint32_t)dir[4 * phase]
                | ((uint32_t)dir[4 * phase + 1] << 8)
                | ((uint32_t)dir[4 * phase + 2] << 16)
                | ((uint32_t)dir[4 * phase + 3] << 24);
            sprite24_stamp(g_standing_sprites + off,
                           dst + 320L * (26 * i + 117) + 14, 320);
        }
        text_render_box(0, 0, 0, fg, 320, base + 40,
                        rec[8] + 1, g_pkg_fdtxt0);
        text_render_box(0, 0, 0, fg, 320, base + 130,
                        rec[32] + 150, g_pkg_fdtxt0);
        text_render_box(0, 0, 0, fg, 320, base + 175,
                        593, g_pkg_fdtxt0);
        text_render_box(0, 0, 0, fg, 320, base + 239,
                        (g_promote_classinfo[news[i + g_shop_list_scroll]]
                         & 0xFF) + 150, g_pkg_fdtxt0);
    }
}

/* 0x2A857 promote_unit_pick(n, units, news)：转职选人器（三缓冲族，
 * 2026-09-08 逐指令重建，撤销"复用 eligible_unit_pick"近似）：
 * 三缓冲 → snap=VRAM、backup=snap → scroll/choice 清零 → 面板帧 16 盖
 * backup+35845 → promote_rows_draw → 6 步滑入 → sub_26EDA(2) 等键：
 * ↑=choice--（==scroll → scroll-- + shop_list_scroll_up）、↓=choice++
 * （==n-1 停；出 3 行窗 → scroll++ + down）；Enter/Space=1、Esc=-1。
 * 尾跳 loc_26A73：不还原不 free（调用方 0x26996 收）。 */
int promote_unit_pick(int n, const uint8_t *units, const uint8_t *news)
{
    if (n <= 0 || !units || !news)
        return -1;
    g_menu_work_buf = malloc(FD2_VRAM_SIZE);
    g_menu_snap_buf = malloc(FD2_VRAM_SIZE);
    g_screen_backup = malloc(FD2_VRAM_SIZE);
    if (!g_menu_work_buf || !g_menu_snap_buf || !g_screen_backup) {
        free(g_menu_work_buf);
        free(g_menu_snap_buf);
        free(g_screen_backup);
        g_menu_work_buf = g_menu_snap_buf = g_screen_backup = NULL;
        return -1;
    }
    memcpy(g_menu_snap_buf, vram_base(), FD2_VRAM_SIZE);   /* 0x2A8B5 */
    memcpy(g_screen_backup, g_menu_snap_buf, FD2_VRAM_SIZE);
    g_shop_list_scroll = 0;            /* 0x2A8D6 */
    g_menu_choice = 0;                 /* 0x2A8DC */
    g_scene_list_count = n;            /* 53F5E/53F62 */
    rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 16),
                    g_screen_backup + 35845, 320);
    promote_rows_draw(n, units, news, g_menu_choice, g_screen_backup);
    for (int i = 5; i >= 0; i--) {     /* 0x2A929：滑入 177..112 */
        dialog_band_blit(13 * i + 112, g_menu_work_buf,
                         g_screen_backup, g_menu_snap_buf);
    }
    for (;;) {
        int key = menu_anim_wait(2, units);   /* 0x2A952 sub_26EDA(2) */

        if (key == SCAN_DOWN) {
            if (g_menu_choice == n - 1)
                continue;              /* 0x2A9B3：末位停 */
            g_menu_choice++;
            if (g_menu_choice - g_shop_list_scroll >= 3) {
                g_shop_list_scroll++;
                shop_list_scroll_down();
            }
        } else if (key == SCAN_UP) {
            if (g_menu_choice == 0)
                continue;
            g_menu_choice--;
            if (g_menu_choice < g_shop_list_scroll) {
                g_shop_list_scroll--;
                shop_list_scroll_up();
            }
        } else if (key == SCAN_ENTER || key == 57) {
            return 1;                  /* 0x2A9E2 */
        } else if (key == SCAN_ESC) {
            return -1;                 /* 0x2A9EE */
        } else {
            continue;
        }
        promote_rows_draw(n, units, news, g_menu_choice, vram_base());
    }
}

/* 0x28632 item_preview_calc(unit, item, out[4])：装备预演 =
 * 基值(+55/+57/+62) + 新道具加成 + 保留的异半区已装备项贡献
 * （同类旧武器被替换即不计入）。out = ATK/DEF/HIT/EVA。 */
static void item_preview_calc(int unit, uint8_t item, int out[4])
{
    const uint8_t *w = weapon_table_entry(item);
    uint8_t *rec = g_ent_table[unit];
    int cls = w[0];
    int atk, def, hit, eva, base62;

    atk = *(int16_t *)(w + 1) + *(int16_t *)(rec + 55);
    def = *(int16_t *)(w + 5) + *(int16_t *)(rec + 57);
    base62 = *(int16_t *)(rec + 62);
    hit = base62 + *(int16_t *)(w + 3);
    eva = base62 + *(int16_t *)(w + 7);
    for (int i = 0; i < 8; i++) {
        const uint8_t *e;
        uint8_t q = rec[10 + 2 * i];

        if (!(q & 0x40))
            continue;
        e = weapon_table_entry(rec[11 + 2 * i]);
        if (!e)
            continue;
        if ((cls <= 20 && e[0] > 0x14) || (cls > 20 && e[0] <= 0x14)) {
            atk += *(int16_t *)(e + 1);
            def += *(int16_t *)(e + 5);
            hit += *(int16_t *)(e + 3);
            eva += *(int16_t *)(e + 7);
        }
    }
    out[0] = atk;
    out[1] = def;
    out[2] = hit;
    out[3] = eva;
}

/* 0x2825B shop_unit_list_draw：≤3 行（行高 26，y=117 起；dword_53F72
 * ==3 时单行的特判语义挂号，默认 3 行）。每行：名 = ent[+8]+1
 * （选中 201/205）+ 现值 ATK/DEF/HIT/EVA 与预演值（item_preview_calc）
 * 并排显示；立绘取自 g_standing_sprites 偏移表（fd2re 无该缓冲，
 * 宿主侧省略——挂号）；原版差值箭头（sub_2860A：相等 31/升 119/
 * 降 42）以数字对照呈现。 */
/* 0x2860A diff_arrow_frame(old,new)：相等 31 / 升 119 / 降 42。 */
static int diff_arrow_frame(int old_v, int new_v)
{
    return old_v == new_v ? 31 : (old_v >= new_v ? 119 : 42);
}

/* 0x2825B shop_unit_list_draw（2026-09-08 反汇编逐指令重写，撤销
 * 6*k 列近似）：≤3 行（行高 26，y=117）。每行：
 *   立绘 sprite24_stamp @ (row,14)——偏移表 u32[48*unit+4*相位]
 *   （相位 = g_roster_grid_col(0x53F72)，3→1；g_standing_sprites 缺失时跳过，
 *   原版无守卫直接解引）；
 *   名字 @ (row+4,40)，列表族 (0,0,0,201 选中/205)；
 *   四维现值/预演对照——ATK/HIT 行 +3、DEF/EVA 行 +12：标签图标
 *   bg_pkg 帧 18/19/20/21 @x122/x196（stamp 字节 RLE 不透明）、
 *   现值 number_stamp_digits @x137/x214、预演值 @x165/x242、预演
 *   标记图标帧 22 @数字下一行 x157/x234；
 *   **数字帧基 = diff_arrow_frame(cur,pre)（31 相等/119 将降/42 将升
 *   ——数字字形集，随增减变色；2026-09-08 勘误：旧"箭头帧"系误读，
 *   0x283DC push edi 实证该值作 number_stamp_digits 的 base 传入，
 *   全函数无独立箭头贴图）。 */
void shop_unit_list_draw(int n, const uint8_t *units,
                         int item, int sel, void *vram)
{
    uint8_t *dst = vram ? (uint8_t *)vram : vram_base();
    int rows = n > 3 ? 3 : n;
    int phase = g_roster_grid_col == 3 ? 1 : g_roster_grid_col;

    for (int i = 0; i < rows; i++) {
        int unit = units[i + g_shop_list_scroll];
        uint8_t *rec = g_ent_table[unit];
        int row = 26 * i + 117;
        uint8_t *r3 = dst + 320 * (row + 3);
        uint8_t *r4 = dst + 320 * (row + 4);
        uint8_t *r12 = dst + 320 * (row + 12);
        uint8_t *r13 = dst + 320 * (row + 13);
        int cur[4], pre[4], base[4];

        item_preview_calc(unit, (uint8_t)item, pre);
        cur[0] = *(uint16_t *)(rec + 72);
        cur[1] = *(uint16_t *)(rec + 74);
        cur[2] = *(uint16_t *)(rec + 76);
        cur[3] = *(uint16_t *)(rec + 78);
        base[0] = diff_arrow_frame(cur[0], pre[0]);
        base[1] = diff_arrow_frame(cur[1], pre[1]);
        base[2] = diff_arrow_frame(cur[2], pre[2]);
        base[3] = diff_arrow_frame(cur[3], pre[3]);

        if (g_standing_sprites) {
            const uint8_t *dir = g_standing_sprites + 48u * unit;
            uint32_t off = (uint32_t)dir[4 * phase]
                | ((uint32_t)dir[4 * phase + 1] << 8)
                | ((uint32_t)dir[4 * phase + 2] << 16)
                | ((uint32_t)dir[4 * phase + 3] << 24);
            sprite24_stamp(g_standing_sprites + off,
                           dst + 320 * row + 14, 320);
        }
        /* 0x28392 压栈：列表族 (0,0,0,201 选中/205)。 */
        text_render_box(0, 0, 0, i + g_shop_list_scroll == sel ? 201 : 205,
                        320, r4 + 40, rec[8] + 1, g_pkg_fdtxt0);

        rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 18),
                        r3 + 122, 320);
        number_stamp_digits(r3 + 137, 320, cur[0], base[0], 3);
        rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 22),
                        r4 + 157, 320);
        number_stamp_digits(r3 + 165, 320, pre[0], base[0], 3);

        rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 19),
                        r12 + 122, 320);
        number_stamp_digits(r12 + 137, 320, cur[1], base[1], 3);
        rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 22),
                        r13 + 157, 320);
        number_stamp_digits(r12 + 165, 320, pre[1], base[1], 3);

        rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 20),
                        r3 + 196, 320);
        number_stamp_digits(r3 + 214, 320, cur[2], base[2], 3);
        rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 22),
                        r4 + 234, 320);
        number_stamp_digits(r3 + 242, 320, pre[2], base[2], 3);

        rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 21),
                        r12 + 196, 320);
        number_stamp_digits(r12 + 214, 320, cur[3], base[3], 3);
        rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 22),
                        r13 + 234, 320);
        number_stamp_digits(r12 + 242, 320, pre[3], base[3], 3);
    }
}

/* 0x27738 shop_list_open（2026-09-08 逐指令严格重建）：三缓冲
 * malloc（work/snap/backup）→ snap=VRAM、backup=snap → 面板帧 16
 * （pkg+[pkg+0x46]，字节 RLE）盖 backup+35845 → shop_item_list_draw
 * 进 backup → 6 步滑入（sub_1974C 行 177..112 步距 13）。尾部不
 * free（原版 pop ebx; ret 直返）——三缓冲与画面由调用方
 * menu_buffers_teardown（0x26996）收（买入 0x28873/卖出 0x28E10/
 * 转交 0x290F2 族）。 */
void shop_list_open(int count, const uint8_t *items, int sell)
{
    g_scene_list_count = count;    /* 0x53F62：列表长度（滚动箭头判定） */
    g_menu_work_buf = malloc(FD2_VRAM_SIZE);
    g_menu_snap_buf = malloc(FD2_VRAM_SIZE);
    g_screen_backup = malloc(FD2_VRAM_SIZE);
    if (!g_menu_work_buf || !g_menu_snap_buf || !g_screen_backup) {
        free(g_menu_work_buf);
        free(g_menu_snap_buf);
        free(g_screen_backup);
        g_menu_work_buf = g_menu_snap_buf = g_screen_backup = NULL;
        return;
    }
    memcpy(g_menu_snap_buf, vram_base(), FD2_VRAM_SIZE);   /* 0x27789 */
    memcpy(g_screen_backup, g_menu_snap_buf, FD2_VRAM_SIZE);
    /* 0x277C3/0x29C63：面板帧 16（310x86 字节 RLE）走 stamp_frame_opaque，
     * 非 4-op（同包 frame0 背景才是 4-op——两种帧格式并存，按调用点区分）。 */
    rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 16),
                    g_screen_backup + 35845, 320);
    shop_item_list_draw(count, items, g_menu_choice, g_screen_backup, sell);
    for (int i = 5; i >= 0; i--) {         /* 0x277ED：滑入 177..112 */
        dialog_band_blit(13 * i + 112, g_menu_work_buf,
                         g_screen_backup, g_menu_snap_buf);
    }
}
/* 0x275E6 shop_list_pick：→1 确认（g_menu_choice=项号）/ -1 取消。
 * 77/75 = 左右 ±1；72/80 = 上下 ±2；越 6 行窗口 → g_shop_list_scroll
 * ±2 + scroll 动画；每次移动重绘列表并播 g_pkg_fdother_31 音效 0。 */
int shop_list_pick(int count, const uint8_t *items, int sell)
{
    for (;;) {
        int key = menu_anim_wait(1, NULL);   /* 0x275FD sub_26EDA(1)：箭头脉冲 */

        if (key == SCAN_RIGHT && g_menu_choice != count - 1) {
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice++;
        } else if (key == SCAN_LEFT && g_menu_choice) {
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice--;
        } else if (key == SCAN_UP && g_menu_choice >= 2) {
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice -= 2;
        } else if (key == SCAN_DOWN && count - 2 > g_menu_choice) {
            sfx_play(g_pkg_fdother_31, 0, 1);
            g_menu_choice += 2;
        } else if (key == SCAN_ENTER || key == 57) {
            return 1;
        } else if (key == SCAN_ESC) {
            return -1;
        } else {
            continue;
        }
        if (g_menu_choice < g_shop_list_scroll) {
            g_shop_list_scroll -= 2;
            shop_list_scroll_up();
        } else if (g_menu_choice - g_shop_list_scroll >= 6) {
            g_shop_list_scroll += 2;
            shop_list_scroll_down();
        }
        shop_item_list_draw(count, items, g_menu_choice, vram_base(), sell);
    }
}
/* 商品列表数字：0x272D0 直接调用 number_stamp_digits。
 * 属性值使用帧基 42，价格使用帧基 119；二者是 blk5 中不同的
 * 数字字形颜色族，不能复用普通商店/状态页的基数 31。 */
static void shop_num_stamp(uint8_t *dst, int pitch, int val,
                           int base, int digits)
{
    number_stamp_digits(dst, pitch, val, base, digits);
}

/* 0x272D0 shop_item_list_draw：2 列×3 行（行高 26，x=10/158，y=119 起）。
 * 图标 g_death_fx_pkg 帧 59/60/61 按武器表 [0]（<0x15/<0x20/≥0x20）；
 * 名称文本 id+181（选中色 201/205）；参数区：RAW 帧 64+ATK、65+DEF，
 * [0]==32 且 [13]==5/11 时为 RAW 帧 66/67；其余效果类用 RLE 帧 41。
 * 价格为 RAW 帧 15 + weapon[19..20]（sell 模式 ×3/4）。 */
void shop_item_list_draw(int count, const uint8_t *items,
                         int sel, void *vram, int sell)
{
    uint8_t *dst = (uint8_t *)vram;
    int rows = count > 6 ? 6 : count;

    if (!dst)
        dst = vram_base();
    if (count > 6 && g_shop_list_scroll + 6 > count)
        rows = 5;
    for (int i = 0; i < rows; i++) {
        int item = items[i + g_shop_list_scroll];
        const uint8_t *w = weapon_table_entry(item);
        if (!w)
            continue;
        int x = 148 * (i % 2) + 10;
        int y = 26 * (i / 2) + 119;
        uint8_t *cell = dst + 320 * y + x;
        int color = i + g_shop_list_scroll == sel ? 201 : 205;
        int icon = w[0] >= 0x20 ? 61 : (w[0] >= 0x15 ? 60 : 59);
        int price = w[19] | (w[20] << 8);

        /* 0x27324：sub_1685C→sub_4ED0B，blk5 59..61 是 RAW 不透明帧。 */
        stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, icon), cell, 320);
        /* 0x273A2 压栈：列表族 (0,0,0,201 选中/205)。 */
        text_render_box(0, 0, 0, color, 320, cell + 320 * 3 + 28,
                        item + 181, g_pkg_fdtxt0);
        if (w[0] < 0x15) {
            /* 0x273E5/0x273F3：RAW 参数图标帧 64 + ATK。 */
            stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 64),
                             cell + 320 * 2 + 95, 320);
            shop_num_stamp(cell + 320 * 2 + 118, 320,
                           (int16_t)(w[1] | (w[2] << 8)), 42, 3);
        } else if (w[0] < 0x20) {
            /* 0x2741F/0x2742D：RAW 参数图标帧 65 + DEF。 */
            stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 65),
                             cell + 320 * 2 + 95, 320);
            shop_num_stamp(cell + 320 * 2 + 118, 320,
                           (int16_t)(w[5] | (w[6] << 8)), 42, 3);
        } else if (w[0] == 0x20 && w[13] == 5) {
            /* 0x2744A/0x27458：RAW 参数图标帧 66 + [14..15]。 */
            stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 66),
                             cell + 320 * 2 + 95, 320);
            shop_num_stamp(cell + 320 * 2 + 118, 320,
                           (int16_t)(w[14] | (w[15] << 8)), 42, 3);
        } else if (w[0] == 0x20 && w[13] == 11) {
            /* 0x2747B/0x274AF：RAW 参数图标帧 67 + [14..15]。 */
            stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 67),
                             cell + 320 * 2 + 95, 320);
            shop_num_stamp(cell + 320 * 2 + 118, 320,
                           (int16_t)(w[14] | (w[15] << 8)), 42, 3);
        } else {
            /* 0x274D6/0x274FC：无参数效果类使用 blk5 RLE 帧 41，
             * 位于参数行的下一行；不能拿 RAW 参数图标代替。 */
            package_blit_frame(g_death_fx_pkg,
                               cell + 320 * 4 + 95, 320, 41);
        }
        /* 0x27504/0x2752A：价格图标在 row+12，RAW 帧 15。 */
        stamp_raw_opaque(package_frame_ptr(g_scene_bg_pkg, 15),
                         cell + 320 * 12 + 95, 320);
        if (sell)
            price = price * 3 / 4;
        /* 0x27553/0x27574：价格数字同在 row+12，x=104，基数 119。 */
        shop_num_stamp(cell + 320 * 12 + 104, 320, price, 119, 5);
    }
}
void shop_list_scroll_down(void)
{
    /* 0x27816：列表区（x10 宽 284）平滑下滚：3 步各 74 行上移 6 行 +
     * 清底 6 行（delay 10ms）+ 终步 72 行上移 8 行 + 清 8 行；填充色 73。*/
    uint8_t *v = vram_base();

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 74; j++)
            memmove(v + 36810 + 320 * j, v + 38730 + 320 * j, 284);
        for (int k = 0; k < 6; k++)
            memset(v + 60490 + 320 * k, 73, 284);
        delay_ms(10);
    }
    for (int m = 0; m < 72; m++)
        memmove(v + 36810 + 320 * m, v + 39370 + 320 * m, 284);
    for (int n = 0; n < 8; n++)
        memset(v + 59850 + 320 * n, 73, 284);
}
void shop_list_scroll_up(void)
{
    /* 0x278E7：反向平滑上滚（行序与方向按原版逐字面）。 */
    uint8_t *v = vram_base();

    for (int i = 0; i < 3; i++) {
        for (int j = 73; j >= 0; --j)
            memmove(v + 38730 + 320 * j, v + 36810 + 320 * j, 284);
        for (int k = 0; k < 6; k++)
            memset(v + 36810 + 320 * k, 73, 284);
        delay_ms(10);
    }
    for (int m = 71; m >= 0; --m)
        memmove(v + 39370 + 320 * m, v + 36810 + 320 * m, 284);
    for (int n = 0; n < 8; n++)
        memset(v + 36810 + 320 * n, 73, 284);
}
void shopkeeper_anim(void)
{
    /* 0x28B41：店主反应动画（g_scene_bg_pkg 帧 23 起）按 g_scene_line_idx：
     * 1 → 帧 23..27 各 2 tick（立绘位 vram+14569）；
     * 3 → 帧 23 停 8 tick（+12628）；
     * 4 → 文本3 + 帧 23..31 各 2 tick（+10387）+ 调色板加亮/停留/
     *      回落（palette_add_range 0..62、10 tick、62..0）；
     * 5 → 帧 23..29 各 2 tick（+9091）。 */
    switch (g_scene_line_idx) {
    case 1:
        for (int i = 0; i < 5; i++) {
            package_blit_frame(g_scene_bg_pkg, vram_base() + 14569, 320, 23 + i);
            wait_bios_ticks(2);
        }
        break;
    case 3:
        wait_bios_ticks(1);
        package_blit_frame(g_scene_bg_pkg, vram_base() + 12628, 320, 23);
        wait_bios_ticks(8);
        break;
    case 4:
        if (dato_frame_stamp(3) == 0)
        wait_bios_ticks(2);
        for (int j = 0; j < 9; j++) {
            package_blit_frame(g_scene_bg_pkg, vram_base() + 10387, 320, 23 + j);
            wait_bios_ticks(2);
        }
        /* 0x28C1E..0x28C34：白闪上卷，每一步原版均 delay(4)。 */
        for (int k = 0; k < 64; k += 2) {
            palette_add_range(0, 255, k);
            delay_ms(4);
        }
        wait_bios_ticks(10);       /* 0x28C46：亮态停留 */
        /* 0x28C4E..0x28C67：白闪回落，不能在亮态直接结束成交动画。 */
        for (int k = 62; k >= 0; k -= 2) {
            palette_add_range(0, 255, k);
            delay_ms(4);
        }
        wait_bios_ticks(5);        /* 0x28C78：回落后的停留 */
        break;
    case 5:
        for (int m = 0; m < 7; m++) {
            package_blit_frame(g_scene_bg_pkg, vram_base() + 9091, 320, 23 + m);
            wait_bios_ticks(2);
        }
        break;
    default:
        break;
    }
    if (dato_frame_stamp(0) == 0)
    kbd_flush();
}

void gold_gain_animate(int amount)
{
    /* 0x26A7A：g_gold += amount + 8 位里程计上滚动画（vram+31376，
     * 6px/位）。滚动 glyph = 9*digit + 偏移(1..9)，每步 delay(10)ms，
     * 全位一致即止。 */
    char buf[16];
    uint8_t cur[8], tgt[8], roll[8];
    uint8_t *digits = vram_base() + 31376;   /* 0xA7A90（2026-09-06 更正旧 36816）*/

    sprintf(buf, "%0.8d", g_gold);
    for (int i = 0; i < 8; i++)
        cur[i] = (uint8_t)(buf[i] - 48);
    g_gold += amount;
    sprintf(buf, "%0.8d", g_gold);
    for (int j = 0; j < 8; j++)
        tgt[j] = (uint8_t)(buf[j] - 48);
    for (;;) {
        int done = 1;
        for (int k = 0; k < 8; k++) {
            roll[k] = cur[k] == tgt[k] ? 0 : 1;
            if (roll[k])
                done = 0;
        }
        if (done)
            break;
        for (int m = 0; m < 9; m++) {
            for (int n = 0; n < 8; n++) {
                if (!roll[n])
                    continue;
                shop_digit_stamp(digits + 6 * n, 320, roll[n] + 9 * cur[n]);
                if (++roll[n] == 10 && ++cur[n] == 10)
                    cur[n] = 0;
            }
            delay_ms(10);
        }
    }
}
void gold_spend_animate(int amount)
{
    /* 0x26B91：同布局（vram+31376）的里程计下滚倒数（glyph = 9*cur + (roll--)-1，
     * roll 自 9 递减；cur 每 do-while 轮递减，255 回绕到 9）。 */
    char buf[16];
    uint8_t cur[8], tgt[8], roll[8];
    uint8_t *digits = vram_base() + 31376;   /* 0xA7A90（2026-09-06 更正旧 36816）*/

    sprintf(buf, "%0.8d", g_gold);
    for (int i = 0; i < 8; i++)
        cur[i] = (uint8_t)(buf[i] - 48);
    g_gold -= amount;
    sprintf(buf, "%0.8d", g_gold);
    for (int j = 0; j < 8; j++)
        tgt[j] = (uint8_t)(buf[j] - 48);
    for (;;) {
        int done = 1;
        for (int k = 0; k < 8; k++) {
            if (cur[k] == tgt[k]) {
                roll[k] = 0;
            } else {
                roll[k] = 9;
                done = 0;
                if (--cur[k] == 255)
                    cur[k] = 9;
            }
        }
        if (done)
            break;
        for (int m = 0; m < 9; m++) {
            for (int n = 0; n < 8; n++) {
                if (!roll[n])
                    continue;
                shop_digit_stamp(digits + 6 * n, 320,
                                 9 * cur[n] + roll[n]-- - 1);
            }
            delay_ms(10);
        }
    }
}

/* 0x2A0C2 revive_rows_draw（复活选人行渲染，2026-09-13 逐指令重建，
 * 撤销"复用 eligible_unit_pick+item=0xFF"近似——近似路径把行渲染带进
 * shop_unit_list_draw→item_preview_calc→weapon_table_entry(0xFF) 返回
 * NULL 后 w[0] 解引用即崩（表 245 条，0xFF 越界；cpp 侧同缺陷现场））：
 * ≤3 行（行高 26，y=117 起）。每行：立绘 sprite24_stamp @ (26i+117,14)
 * （相位 = g_roster_grid_col，3→1）；名 ent[+8]+1 @ (26i+121)+40、系名
 * ent[+31]+140 @ +130（duel.c 2930 同族）、职业系名 ent[+32]+150 @ +175
 * （列表族 0,0,0,201 选中/205）；金币图标 g_scene_bg_pkg 帧 15
 * （sub_1685C→0x4ED0B RAW 不透明族，非商店行的 RLE stamp_frame_opaque）
 * @ (26i+125)+220；单价 = ent[+33]等级 × g_revive_price_lut[ent[+32]
 * 职业系] number_stamp_digits base 119 五位 @ +228。原版行内价
 * word_52399[family-1]：0x52399 = 0x52397+2，即 &lut[family]——直接
 * 按 lut[family] 取值（family=0 时原版 -1 索引同落 lut[0]，等价）。 */
static void revive_rows_draw(int n, const uint8_t *units, int sel,
                             uint8_t *dst)
{
    int rows = n > 3 ? 3 : n;
    int phase = g_roster_grid_col == 3 ? 1 : g_roster_grid_col;

    for (int i = 0; i < rows; i++) {
        int unit = units[i + g_shop_list_scroll];
        uint8_t *rec = g_ent_table[unit];
        uint8_t *name_row = dst + 320L * (26 * i + 121);
        uint8_t *coin_row = dst + 320L * (26 * i + 125);
        int fg = i + g_shop_list_scroll == sel ? 201 : 205;
        int fam = rec[32];
        /* family 域 0..28（转职/敌方模板可越 19），表仅 20 槽；域外钳
         * 表尾（cpp 2026-09-12 定案；原版裸读表后相邻数据） */
        int price = rec[33] * g_revive_price_lut[fam > 19 ? 19 : fam];

        if (g_standing_sprites) {
            const uint8_t *dir = g_standing_sprites + 48u * unit;
            uint32_t off = (uint32_t)dir[4 * phase]
                | ((uint32_t)dir[4 * phase + 1] << 8)
                | ((uint32_t)dir[4 * phase + 2] << 16)
                | ((uint32_t)dir[4 * phase + 3] << 24);
            sprite24_stamp(g_standing_sprites + off,
                           dst + 320L * (26 * i + 117) + 14, 320);
        }
        text_render_box(0, 0, 0, fg, 320, name_row + 40,
                        rec[8] + 1, g_pkg_fdtxt0);
        text_render_box(0, 0, 0, fg, 320, name_row + 130,
                        rec[31] + 140, g_pkg_fdtxt0);
        text_render_box(0, 0, 0, fg, 320, name_row + 175,
                        rec[32] + 150, g_pkg_fdtxt0);
        stamp_raw_opaque(package_frame_ptr(g_scene_bg_pkg, 15),
                         coin_row + 220, 320);
        number_stamp_digits(coin_row + 228, 320, price, 119, 5);
    }
}

/* 0x2A29D revive_unit_pick(n, units)：复活专用选人器（2026-09-13 逐指令
 * 重建，三缓冲族同 promote_unit_pick）：三缓冲 malloc → snap=VRAM、
 * backup=snap → scroll/choice 清零、g_scene_list_count=n（0x53F5E）→
 * 面板帧 16 盖 backup+35845 → revive_rows_draw → 6 步滑入（177..112）
 * → sub_26EDA(2) 等键：↑=choice--（==0 停；<scroll → scroll-- +
 * shop_list_scroll_up）、↓=choice++（==n-1 停；出 3 行窗 → scroll++ +
 * down）；**上下不播 sfx**（0x2A39C..0x2A414 反汇编实证无 fdother31
 * 调用，区别于商店 eligible_unit_pick 的 0x28057 族）；Enter(0x1C)/
 * 空格(57)=1、Esc(0x01)=-1。尾跳 loc_26A73：不还原不 free（调用方
 * 0x2A515 menu_buffers_teardown 收）。 */
static int revive_unit_pick(int n, const uint8_t *units)
{
    if (n <= 0 || !units)
        return -1;
    g_menu_work_buf = malloc(FD2_VRAM_SIZE);
    g_menu_snap_buf = malloc(FD2_VRAM_SIZE);
    g_screen_backup = malloc(FD2_VRAM_SIZE);
    if (!g_menu_work_buf || !g_menu_snap_buf || !g_screen_backup) {
        free(g_menu_work_buf);
        free(g_menu_snap_buf);
        free(g_screen_backup);
        g_menu_work_buf = g_menu_snap_buf = g_screen_backup = NULL;
        return -1;
    }
    memcpy(g_menu_snap_buf, vram_base(), FD2_VRAM_SIZE);   /* 0x2A2FB */
    memcpy(g_screen_backup, g_menu_snap_buf, FD2_VRAM_SIZE);
    g_shop_list_scroll = 0;            /* 0x2A31C */
    g_menu_choice = 0;                 /* 0x2A322 */
    g_scene_list_count = n;            /* 0x2A32E：箭头/立绘重绘长度 */
    rle_blit_opaque(package_frame_ptr(g_scene_bg_pkg, 16),
                    g_screen_backup + 35845, 320);
    revive_rows_draw(n, units, g_menu_choice, g_screen_backup);
    for (int i = 5; i >= 0; i--) {     /* 0x2A36B：滑入 177..112 */
        dialog_band_blit(13 * i + 112, g_menu_work_buf,
                         g_screen_backup, g_menu_snap_buf);
    }
    for (;;) {
        int key = menu_anim_wait(2, units);   /* 0x2A394 sub_26EDA(2) */

        if (key == SCAN_DOWN) {
            if (g_menu_choice == n - 1)
                continue;              /* 0x2A3F1：末位停 */
            g_menu_choice++;
            if (g_menu_choice - g_shop_list_scroll >= 3) {
                g_shop_list_scroll++;
                shop_list_scroll_down();
            }
        } else if (key == SCAN_UP) {
            if (g_menu_choice == 0)
                continue;
            g_menu_choice--;
            if (g_menu_choice < g_shop_list_scroll) {
                g_shop_list_scroll--;
                shop_list_scroll_up();
            }
        } else if (key == SCAN_ENTER || key == 57) {
            return 1;                  /* 0x2A420 */
        } else if (key == SCAN_ESC) {
            return -1;                 /* 0x2A42C */
        } else {
            continue;
        }
        revive_rows_draw(n, units, g_menu_choice, vram_base());
    }
}

/* 0x2A43E temple_revive_menu（收框纪律 2026-09-06 对齐）：循环
 * {dead_units_collect（空→文本 588@38092+stamp+**wait(0)**+T 返回）→
 * 文本 589@38092+stamp+wait(1)+T → **revive_unit_pick 专用选人**（0x2A29D，
 * 2026-09-13 更正：原版不复用商店 eligible_unit_pick；旧近似以 item=0xFF
 * 借道 shop_unit_list_draw→item_preview_calc→weapon_table_entry(0xFF)=
 * NULL→w[0] 崩溃，即 cpp 侧"复活列表错误退出"同源缺陷）+T（取消即返）→
 * 价格 = g_revive_price_lut[ent[+32]职业系]×ent[+33]等级 → 文本 590@
 * 38092+stamp+confirm+close_anim（否/Esc→T 回环）→ 金币不足：文本
 * 504@**50252 第三行不重开窗**+stamp+wait(1)+T → 成交：
 * gold_spend_animate + ent[+5]=0（清死亡）+ HP=MaxHP +
 * 金币快照双写 + T(0x2A667) + music_play(17,1) + shopkeeper_anim +
 * music_play(11,1)（2026-09-13 反汇编复核更正：原版收框先于店主动画，
 * 且收框前必须把新金币盖进快照防回跳；两处 music 原先漏移植）。 */
void temple_revive_menu(void)
{
    uint8_t dead[FD2_ROSTER_MAX];

    for (;;) {
        uint8_t *rec;
        int n_dead = dead_units_collect(dead);
        int price, yn;

        if (!n_dead) {
            dialog_backdrop_load(g_dialog_backdrop_ids[4]);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                            588, g_pkg_fdtxt0);
            dato_frame_stamp(0);
            wait_key_anim(0);          /* 0x2A4BC：本页唯一 wait(0) */
            menu_buffers_teardown();   /* 0x2A4AA */
            return;
        }
        dialog_backdrop_load(g_dialog_backdrop_ids[4]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        589, g_pkg_fdtxt0);
        dato_frame_stamp(0);
        wait_key_anim(1);
        menu_buffers_teardown();       /* 0x2A502 */
        {
            int picked = revive_unit_pick(n_dead, dead);

            menu_buffers_teardown();   /* 0x2A515：选人收框 */
            if (picked == -1)
                return;
        }
        rec = g_ent_table[dead[g_menu_choice]];
        g_disp_num_a = rec[8] + 1;
        /* family 域 0..28（转职/敌方模板可越 19），表仅 20 槽；域外钳
         * 表尾（cpp 2026-09-12 定案；原版 0x2A56B 裸读表后相邻数据） */
        price = g_revive_price_lut[rec[32] > 19 ? 19 : rec[32]] * rec[33];
        g_disp_num_b = price;
        dialog_backdrop_load(g_dialog_backdrop_ids[4]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        590, g_pkg_fdtxt0);
        dato_frame_stamp(0);
        yn = confirm_yes_no();
        confirm_close_anim();
        if (yn == -1 || g_menu_choice) {
            menu_buffers_teardown();   /* 0x2A52B 族：确认否收框 */
            continue;
        }
        if (g_gold < price) {
            text_render_box(1, 19, 74, 205, 320, vram_base() + 50252u,
                            504, g_pkg_fdtxt0);
            dato_frame_stamp(0);
            wait_key_anim(1);
            menu_buffers_teardown();   /* 0x2A5FF */
            continue;
        }
        gold_spend_animate(price);
        rec[5] = 0;
        *(uint16_t *)(rec + 0x40) = *(uint16_t *)(rec + 0x42);
        /* 0x2A62C..0x2A664：收框前把面板+新金币盖进活动快照（见
         * scene_gold_snapshot_stamp 头注），否则整屏恢复回跳旧值 */
        scene_gold_snapshot_stamp(dialog_backdrop_snapshot());
        menu_buffers_teardown();       /* 0x2A667：复活成交收框 */
        music_play(17, 1);             /* 0x2A670 */
        shopkeeper_anim();             /* 0x2A678：原版在收框之后 */
        music_play(11, 1);             /* 0x2A681 */
    }
}
/* 0x5413F/0x54143/0x54147：转职演出三背景块（BG.DAT 0/1/2；0x2FB2C
 * 装载/释放，0x2FE14 帧循环只读）。 */
static uint8_t *s_promo_bg[3];

/* 0x2FE14 promote_frames_play(pkg, work, ticks)：FIGANI 帧循环播放器。
 * 每拍：清 work → 背景轮换 (v+1)%3 → rle 解底图(0,50) → blit_frame_flat
 * 叠 pkg 第 plane 帧 → blit_rows 全屏 → 驻留计数：时长 = pkg[6+帧偏移]
 * （帧偏移 = u32(pkg+8+4*plane)；即帧族头 +6 的"mode"字节，此处实为
 * 驻留拍数）到点则 plane 环回（pkg[0] = 帧数）→ wait_bios_ticks(1)。
 * wait 在绘制之后——0x2FE36 是循环旋转尾（i++/判界前置），hex-rays 把
 * 它显示在循环首，字节序验证为后置。 */
static void promote_frames_play(const uint8_t *pkg, uint8_t *work, int ticks)
{
    int plane = 0, bg = 0, held = 0;

    for (int i = 0; i < ticks; ++i) {
        uint32_t off = (uint32_t)pkg[8 + 4 * plane]
                     | ((uint32_t)pkg[9 + 4 * plane] << 8)
                     | ((uint32_t)pkg[10 + 4 * plane] << 16)
                     | ((uint32_t)pkg[11 + 4 * plane] << 24);
        int dur;

        memset(work, 0, 0x1F400);
        bg = (bg + 1) % 3;
        rle_decode_frame(s_promo_bg[bg], 0, 50, work, 640, -1);
        blit_frame_flat(pkg, plane, work, 640, -1);
        blit_rows(vram_base(), 320, work, 640, 320, 200);
        dur = pkg[off + 6];
        if (++held == dur) {
            held = 0;
            if (++plane == pkg[0])
                plane = 0;
        }
        wait_bios_ticks(1);
    }
}

/* 0x2FB2C promote_fx_play(unit, new_cls)：转职演出（2026-09-08 全解）。
 * BG.DAT 0/1/2 三背景 + FIGANI 3*旧职业/3*新职业两帧包；640×200 双宽
 * work + 64K VRAM 快照 → 渐黑 → i=8..0 九步滑入（旧职业帧贴 work+10*i、
 * 亮度 6*i 递增；0x2FCB6 处读 0x46C 后 je 挂在 add esp,0xC 的陈旧 ZF
 * 上——esp 恒非零故分支永不走，按恒真背景轮换处理）→ 旧职业 16 拍帧
 * 循环 → 20 级白闪（palette_add_range 3*j + delay(10)）→ 新职业亮相
 * （bg[0]、帧贴 work+10*20、palette_apply_range 全亮）→ 24 拍帧循环 →
 * 渐黑 → 还原 VRAM 快照 → fade_in → 六块内存全释放。 */
void promote_fx_play(int unit, int new_cls)
{
    uint8_t *work, *old_pkg, *new_pkg, *snap;
    int bg = 0;

    s_promo_bg[0] = dat_load_block("BG.DAT", NULL, 0);
    s_promo_bg[1] = dat_load_block("BG.DAT", NULL, 1);
    s_promo_bg[2] = dat_load_block("BG.DAT", NULL, 2);
    work    = malloc(0x1F400);               /* 640×200 双宽 */
    old_pkg = dat_load_block("FIGANI.DAT", NULL, 3 * g_ent_table[unit][7]);
    new_pkg = dat_load_block("FIGANI.DAT", NULL, 3 * new_cls);
    snap    = malloc(64000);
    memmove(snap, vram_base(), 64000);       /* 0x2FC13 演出前 VRAM 快照 */
    palette_fade_black();
    wait_bios_ticks(1);
    for (int i = 8; i >= 0; --i) {
        memset(work, 0, 0x1F400);
        bg = (bg + 1) % 3;
        rle_decode_frame(s_promo_bg[bg], 0, 50, work, 640, -1);
        blit_frame_flat(old_pkg, 0, work + 10 * i, 640, -1);
        blit_rows(vram_base(), 320, work, 640, 320, 200);
        palette_apply_range(0, 255, 6 * i);
    }
    promote_frames_play(old_pkg, work, 16);
    for (int j = 0; j < 20; ++j) {           /* 白闪 20 级 */
        palette_add_range(0, 255, 3 * j);
        delay_ms(10);
    }
    memset(work, 0, 0x1F400);
    rle_decode_frame(s_promo_bg[0], 0, 50, work, 640, -1);
    blit_frame_flat(new_pkg, 0, work + 200, 640, -1);   /* 10*j, j==20 */
    blit_rows(vram_base(), 320, work, 640, 320, 200);
    palette_apply_range(0, 255, 0);
    promote_frames_play(new_pkg, work, 24);
    palette_fade_black();
    memmove(vram_base(), snap, 64000);       /* 还原演出前画面 */
    fade_in();
    free(snap);
    free(s_promo_bg[0]);
    free(s_promo_bg[1]);
    free(s_promo_bg[2]);
    free(old_pkg);
    free(new_pkg);
    free(work);
}

/* 0x2AA00 class_promote_menu（全解 2026-09-05；2026-09-08 字节级复核：
 * 空名单退出 wait_key_anim(0) 非 1；写回无 <53 守卫；重载图标前先
 * free(g_standing_sprites)+清缓存计数；掷点后再 kbd_flush 一次）：
 * 循环 {promote_list_build（空→退出，文本 591）→ 文本 592 选人 →
 * 文本 594 确认 → 消耗判定：新职业 52→道具 90；≥50→
 * g_promote_item_lut[当前职业]；<50 免费 → inventory_remove →
 * music16 + promote_fx_play(unit,new_cls) + music11 →
 * ent[+32]=classinfo 低字节、ent[+7]=新职业 → 重建立绘缓存重载全员
 * FDICON 图标 → promote_stat_rolls + kbd_flush}。 */
void class_promote_menu(void)
{
    uint8_t units[FD2_ROSTER_MAX], news[FD2_ROSTER_MAX];

    for (;;) {
        uint8_t *rec;
        int n, unit, new_cls, slot;

        n = promote_list_build(units, news);
        if (!n) {
            dialog_backdrop_load(g_dialog_backdrop_ids[4]);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                            591, g_pkg_fdtxt0);
            dato_frame_stamp(0);
            wait_key_anim(0);          /* 0x2AA6B push esi(=0) 纯等待 */
            menu_buffers_teardown();   /* 0x2AA73 */
            return;
        }
        dialog_backdrop_load(g_dialog_backdrop_ids[4]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        592, g_pkg_fdtxt0);
        dato_frame_stamp(0);
        wait_key_anim(1);
        menu_buffers_teardown();       /* 0x2AACB */
        kbd_flush();                   /* 0x2AAD0 */
        {
            int picked = promote_unit_pick(n, units, news);

            menu_buffers_teardown();   /* 0x2AAE8：选人收框 */
            if (picked == -1)
                return;                /* 0x2AAF0 → 函数尾声 */
        }
        unit = units[g_menu_choice];
        new_cls = news[g_menu_choice];
        rec = g_ent_table[unit];
        g_disp_num_a = rec[7] + 1;
        dialog_backdrop_load(g_dialog_backdrop_ids[4]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38092u,
                        594, g_pkg_fdtxt0);
        dato_frame_stamp(0);
        kbd_flush();                   /* 0x2AB5E */
        {
            int yn = confirm_yes_no();

            confirm_close_anim();      /* 0x2AB6A */
            menu_buffers_teardown();   /* 0x2AB6F */
            if (yn == -1 || g_menu_choice)
                continue;              /* 0x2AB77/0x2AB84 → 回环 */
        }
        if (new_cls == 52) {
            slot = roster_has_item(rec, 90);
            if (slot >= 0)
                inventory_remove_item(unit, slot);
        } else if (new_cls >= 50) {
            slot = roster_has_item(rec, g_promote_item_lut[rec[7]]);
            if (slot >= 0)
                inventory_remove_item(unit, slot);
        }
        music_play(16, 1);
        promote_fx_play(unit, new_cls);   /* 0x2ABCC 转职演出 */
        music_play(11, 0);
        /* 0x2ABE4..0x2ABF5：无条件写回（new_cls 可达 53..65，原版无
         * <53 守卫——classinfo 表按 68 项实读）。 */
        rec[32] = (uint8_t)(g_promote_classinfo[new_cls] & 0xFF);
        rec[7] = (uint8_t)new_cls;
        /* 0x2ABF8..0x2AC23：丢弃整幅立绘缓存并清图标计数（dword_53BDF=0），
         * icon_load_entry 首次调用时按现名册重分配重建。 */
        free(g_standing_sprites);
        g_standing_sprites = NULL;
        icon_cache_reset();
        {
            FILE *icons = fopen("FDICON.B24", "rb");
            if (icons) {
                for (int i = 0; i < g_roster_count; i++) {
                    icon_load_entry(g_roster_table[i][7], icons);
                    /* 同 statemachine 场景入口：目录槽按名册位置别名，
                     * 防转职业 rec[7] 重复时去重压缩槽位错画。 */
                    (void)icon_directory_alias(i, g_roster_table[i][7]);
                }
                fclose(icons);
            }
        }
        promote_stat_rolls(unit);     /* 内含收框 0x2ADE8 + kbd_flush */
        kbd_flush();                  /* 0x2AC6B：掷点页后再冲一次键缓冲 */
    }
}
int  dead_units_collect(uint8_t *out)
{
    /* 0x2A07A：收集死亡角色（ent[+5] bit0 阵亡标记），返回数量。 */
    int n = 0;
    for (int i = 0; i < g_roster_count; i++)
        if (g_roster_table[i][5] & 1)
            out[n++] = (uint8_t)i;
    return n;
}
int promote_list_build(uint8_t *units, uint8_t *news)
{
    /* 0x2AE0E：可转条件 = 等级(+33)≥20 且 职业(+7)<18 且 ≠7；
     * 新职业三档：默认 +32；持有 g_promote_item_lut[职业] → +50；
     * 职业 9 持有道具 90 → 52。 */
    int n = 0;

    for (int i = 0; i < g_roster_count; i++) {
        const uint8_t *r = g_roster_table[i];
        uint8_t cls = r[7];
        if (r[33] < 20 || cls >= 18 || cls == 7)
            continue;
        units[n] = (uint8_t)i;
        news[n] = (uint8_t)(cls + 32);
        if (roster_has_item(r, g_promote_item_lut[cls]) != -1)
            news[n] = (uint8_t)(cls + 50);
        if (cls == 9 && roster_has_item(r, 90) != -1)
            news[n] = 52;
        n++;
    }
    return n;
}
/* 0x2AC7D promote_stat_rolls（全解 2026-09-05；2026-09-08 字节级复核
 * 对齐）：掷点 5 项（+55/+57/+62/+66/+70 × growth[0/2/4/6/8]，
 * 文本 490..494 逐行，行 3 起滚动）→ 移动力奖励（文本 596，
 * classinfo[rec[7]] 高字节，无守卫）→ ent_recompute_derived →
 * menu_buffers_teardown → 等级(+33)=1、经验(+60)=0、
 * HP(+64)=MaxHP(+66)、MP(+68)=MaxMP(+70) → return kbd_flush()。
 * 首屏文本 595（新职业名 = ent[+32]+150）按同序保留。 */
int promote_stat_rolls(int unit)
{
    uint8_t *rec;
    const uint8_t *g;
    int row;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return -1;
    rec = g_ent_table[unit];
    g = growth_table_entry(rec[7]);
    if (!g)
        return -1;
    kbd_flush();
    dialog_backdrop_load(rec[7]);   /* 0x2ACB4：转职者本人肖像（ent[+7]）*/
    g_disp_num_a = rec[32] + 150;
    text_render_box(1, 19, 74, 205, 320, vram_base() + 38175u,
                    595, g_pkg_fdtxt0);
    (void)dato_frame_stamp(0);
    kbd_flush();
    row = stat_roll_row((uint16_t *)(rec + 55), g + 0, 490, 1);
    row = stat_roll_row((uint16_t *)(rec + 57), g + 2, 491, row);
    row = stat_roll_row((uint16_t *)(rec + 62), g + 4, 492, row);
    row = stat_roll_row((uint16_t *)(rec + 66), g + 6, 493, row);
    row = stat_roll_row((uint16_t *)(rec + 70), g + 8, 494, row);
    /* 0x2AD76..0x2ADD8：移动力奖励无条件读 classinfo[rec[7]] 高字节
     *（rec[7] 可达 53..65，原版无守卫）。 */
    {
        int bonus = g_promote_classinfo[rec[7]] >> 8;
        if (bonus) {
            g_disp_num_b = bonus;
            text_render_box(1, 19, 74, 205, 320,
                            vram_base() + 38175u + 6080u * (unsigned)row,
                            596, g_pkg_fdtxt0);
            wait_key_anim(0);
            rec[59] = (uint8_t)(rec[59] + (uint8_t)g_disp_num_b);
        }
    }
    ent_recompute_derived(unit);
    menu_buffers_teardown();           /* 0x2ADE8：掷点页收框 */
    rec[33] = 1;                       /* 0x2ADED 等级重置 */
    rec[60] = 0;                       /* 0x2ADF1 经验清零 */
    *(uint16_t *)(rec + 64) = *(uint16_t *)(rec + 66);
    *(uint16_t *)(rec + 68) = *(uint16_t *)(rec + 70);
    kbd_flush();                       /* 0x2AE0D 原版返回 0x41A 环头值
                                          （宿主 void，无消费者，返 0 等价） */
    return 0;
}
