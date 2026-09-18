/* states.c — 30 状态处理器存根
 *
 * 已逆向语义（IDA 地址 → 行为摘要；完整反编译存档 reverse/states/f_*.c）：
 *
 *  st   enter(IDA)          行为摘要                                   exit(IDA)   行为摘要
 *  --   ------------------  -----------------------------------------  ---------  ------------------
 *   0   0x22EF6            str9 后 g_state=1                          0x3231B    序章：相位32(ui_script
 *                                                                    (st00_exit_prologue)
 *                                                                               99-105,str0-5,music11)→31
 *                                                                               (90-98,str0-9)→0(载入
 *                                                                               0/9/4/30,script0-5,str0-2)
 *   1   0x22F37            实体扫描+str6-10，→2                        0x32D18    六入口各自独立（0x51D71
 *   2   0x230F2            6号存活:field_init布阵+str7+spawn(2);       0x32E8C    表定案 2026-09-08；旧记
 *                                                                    阵亡:str6；++
 *   3   0x231BC(相位)      str4 → sync → ++（无重绘/布阵/入队）        0x32FB2    "1..6 共享 0x32D18"有误
 *   4   0x231F9(相位)      field_init(队长ent41@12,8)+str9+spawn(10)   0x33049    ——[1] 新游戏不可达，
 *                                                                    +sync → ++
 *   5   0x23296            载入13+特效3+(5,14)+script27，尾接 ++        0x3314B    [2]=一章敌军入场
 *   6   0x232E8            str5,4 后 ++                                0x33169    （script18/17/19））
 *   7   0x234BB            str3,4                                      0x33219    str0
 *   8   0x235BC            ent[11].flags 复位                          0x3327D    ent[i].team=2; str0,1
 *   9   0x235F9            实体槽50/51/52=单位15/14/16，str4,5，++      0x3332B    ent50/51 HP=100
 *  10   0x23790            str3                                        0x33367    str0,1
 * 11-13 0x237D5            str3,4 后 ++（相位 0x2389F/0x238DC）        0x333F5/46B/47C
 *  14   0x239BD            str                                         0x334D9    动态 str id (n+1,n+2)
 *  15   0x23A0A            条件(进度>18 / ent+0x42<0x140) str2-4，++    0x335A0    空
 *  16   0x23B5F            str5,7,6,8，++                               0x335AA    空
 *  17   0x23CD5            str7-10，++                                  0x335DA    str0,1
 *  18   0x23E39            str3                                         0x33674    空（18/19/20 共享）
 *  19   0x23E74            实体扫描 str11-16，++                        0x33674
 *  20   0x240FA            str5,7,8,9,10,6，++                          0x33674
 *  21   0x244B6            battle_field_init：16 单位 @0x52273          0x3367E
 *       (st21_enter_battle_init)  (x/y/team 三表)，leader=ent72，光标(16,18)
 *  22   0x24754            str8-17 + FDFIELD.DAT69 / FDSHAP.DAT46/47，  0x336A0    16×teardown +
 *       (st22_enter_battle_load)  ++                                       (st22_exit_battle_aftermath)
 *                                                                               汇总 sub_24618
 *  23   0x24C1E            str2,3，++（roster_menu 态）                  0x338C4    str0
 *  24   0x24DF2            str6,7（roster_menu 态）                      0x3396A    FDOTHER blk88 ×4 音效
 *  25   0x24E80            实体 16.. 扫描，str7,10,11，++                0x33AAE    空
 *  26   0x250CC            实体扫描 str8-16，++                          0x33AF1
 *  27   0x25464            尾接 0x231DF（++；roster_menu 态）            0x33C9D
 *  28   0x2548C            str10,11 + ent20[+7/+8]=0x7E（roster 态）     0x33DBA    str7
 *  29   0x25757            battle_field_init：20 单位 @0x52327          0x33E3C    str0-2
 *       (st29_enter_final_battle)
 *
 * 对白推进：text_render_box 只绘制文本；g_text_busy 仅由原版各调用点
 * 显式写入（anim_pump 轮询），不能由对白 helper 统一清零。多数 enter
 * 尾部 ++g_state 顺序前进。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "states.h"
#include "statemachine.h"
#include "battle.h"
#include "text.h"
#include "video.h"
#include "resource.h"
#include "entities.h"
#include "event.h"
#include "input.h"
#include "magic.h"
#include "script.h"
#include "timer.h"
#include "audio.h"
#include "duel.h"

/* ---- 状态脚本原语（2026-09-06 反汇编定案，替换未映射挂号） ---- */

/* 0x25052 fade_add_loop(level, ms)：palette_add_range(0,255,level)
 * 自 level 递减到 0，每步 delay(ms)——加法调光渐隐。 */
static void fade_add_loop(int level, int ms)
{
    while (level >= 0) {
        palette_add_range(0, 255, level);
        delay_ms(ms);
        level--;
    }
}

/* 0x33499/0x24BDE roster_has_unit(id)：任一 roster 记录[+8] == id。 */
static int roster_has_unit(int id)
{
    for (int i = 0; i < g_roster_count; i++)
        if (g_roster_table[i][8] == (uint8_t)id)
            return 1;
    return 0;
}

/* 0x24B14 inventory_has_item(item)：任一 roster 单位背包含 item。 */
static int inventory_has_item(int item)
{
    for (int i = 0; i < g_roster_count; i++) {
        uint8_t *rec = g_roster_table[i];
        for (int s = 0; s < 8; s++)
            if (rec[11 + 2 * s] == (uint8_t)item
                && (int8_t)rec[10 + 2 * s] >= 0)
                return 1;
    }
    return -1;
}

/* 0x1C220 grant_item_first_player(item)：给首个我方（rec[+6]==2）
 * 实体发道具（ent_inventory_add，成功即止）。 */
static void grant_item_first_player(int item)
{
    for (int i = 0; i < g_ent_count; i++)
        if (g_ent_table[i][6] == 2
            && ent_inventory_add(i, item) != -1)
            break;
}

/* 0x24B4D battle_quake(count)：战场震屏 count 帧（原版 = 456 宽影子
 * 窗口 ±456 交替呈现 + delay(20)；fd2re 无 456 影子——宿主适配为
 * 整屏 ±24px（1 tile）行块交替平移，机制差异已注）。 */
static void battle_quake(int count)
{
    uint8_t *vram = vram_base();
    uint8_t *backup = malloc(64000);

    anim_tick_update(0);
    if (!backup)
        return;
    for (int i = 0; i < count; i++) {
        int dx = (i & 1) ? 24 : -24;
        memcpy(backup, vram, 64000);
        for (int r = 0; r < 200; r++) {
            const uint8_t *srow = backup + 320 * r + (dx > 0 ? 0 : -dx);
            uint8_t *drow = vram + 320 * r + (dx > 0 ? dx : 0);
            int w = 320 - (dx > 0 ? dx : -dx);
            memmove(drow, srow, (size_t)w);
        }
        delay_ms(20);
        memcpy(vram, backup, 64000);
    }
    free(backup);
}

/* 0x22253 cutscene_stand（2026-09-06 全解忠实实现，0x33F78 包装的
 * from==to 路径）：blk81 音效包装 + blk6 帧 114..124 十一帧站立动画
 * @tile(x,y) 像素位 (24*dx+12, 24*dy+15) → sfx(1) → blk3 帧 0..5 六帧
 * 落位特效（delay 10）→ 位置写回 ent[0/1] → 帧帧 124 逐行揭示升起
 *（24 行/行 10ms；y==scroll_y 顶边特例 18 行）→ blk3 帧 0..9 十帧
 * 收束。原版走 456 宽影子管线，fd2re 以 anim_tick_update+VRAM 直贴
 * 等价表达（机制差异已注）。 */
static void cutscene_stand(int ent, int x, int y)
{
    uint8_t *vram = vram_base();
    void *snd = dat_load_block("FDOTHER.DAT", NULL, 81);
    int px = 24 * (x - (int)g_scroll_x) + 12;
    int py = 24 * (y - (int)g_scroll_y) + 15;
    uint8_t *tmp;

    if (ent < 0 || ent >= g_ent_count)
        { free(snd); return; }
    for (int i = 0; i < 11; i++) {
        anim_tick_update(0);
        package_blit_frame(g_pkg_fdother_6,
                           vram + 320L * py + px, 320, 114 + i);
        wait_bios_ticks(1);
    }
    sfx_play(snd, 1, 1);
    for (int i = 5; i >= 0; i--) {
        package_blit_frame(g_pkg_fdother_3,
                           vram + 320L * py + px, 320, i);
        delay_ms(10);
    }
    wait_bios_ticks(1);
    wait_bios_ticks(1);
    g_ent_table[ent][0] = (uint8_t)x;
    g_ent_table[ent][1] = (uint8_t)y;

    tmp = malloc(64000);
    if (tmp) {
        int rows = (y == (int)g_scroll_y) ? 18 : 24;

        memset(tmp, 0, 64000);
        package_blit_frame(g_pkg_fdother_6,
                           tmp + 320L * py + px, 320, 124);
        for (int i = 0; i < rows; i++) {
            memcpy(vram + 320L * (py + i) + px,
                   tmp + 320L * (py + i) + px, 24);
            delay_ms(10);
        }
        free(tmp);
    }
    for (int i = 0; i < 10; i++) {
        package_blit_frame(g_pkg_fdother_3,
                           vram + 320L * py + px, 320, i);
        wait_bios_ticks(1);
    }
    free(snd);
}

/* 0x33F78 scroll_and_cutscene(ent, x, y)：cursor_scroll_to(x, y)
 *（0x12CEA 实参 (a6,a7) 与落位坐标一致——读取 A 定案）+
 * cutscene_stand。evt82 复用（§13.75），故去 static 导出。 */
void scroll_and_cutscene(int ent, int x, int y)
{
    cursor_scroll_to(x, y);
    cutscene_stand(ent, x, y);
}

/* 0x22253 from==to 独立入口（fx_targeted_strike@0x2218A 复用）：
 * cutscene_stand 本体保持 static，此处按原版独立入口再导出一份。 */
void battle_cutscene_stand(int ent, int x, int y)
{
    cutscene_stand(ent, x, y);
}

/* 0x10652 battle_backdrop_load 已上移 entities.c 真管线（2026-09-07：
 * 波光/视差/上卷三路 + field_backdrop_render 消费者；本文件旧桩仅装
 * 不画，删除）。 */

/* 0x24336 cutscene_film_play（2026-09-06 定案）：camera(14,8) +
 * 屏幕备份 + FDOTHER blk34 帧 1..68 逐帧（wait 3）→ ani_play(0,15ms)
 * + 调光 63/0/500ms → 帧 69..100 → JUMPOUT 0x22BB7（battle 后续）。 */
static void cutscene_film_play(void)
{
    uint8_t *vram = vram_base();
    uint8_t *backup = malloc(64000);
    void *pkg;

    camera_scroll_to(14, 8);
    if (!backup)
        return;
    memcpy(backup, vram, 64000);
    pkg = dat_load_block("FDOTHER.DAT", NULL, 34);
    if (!pkg) {
        free(backup);
        return;
    }
    blit_frame_flat(pkg, 0, backup, 320, -1);
    for (int i = 1; i < 69; i++) {
        memcpy(vram, backup, 64000);
        blit_frame_flat(pkg, i, vram, 320, -1);
        idle_pump();                    /* 0x243E4：仅前段循环有色循环泵 */
        wait_bios_ticks(3);
        kbd_flush();
    }
    ani_play(0, 15, 0);
    palette_add_range(0, 255, 63);
    delay_ms(100);
    palette_add_range(0, 255, 0);
    delay_ms(500);
    for (int i = 69; i < 101; i++) {
        memcpy(vram, backup, 64000);
        blit_frame_flat(pkg, i, vram, 320, -1);
        wait_bios_ticks(3);
        kbd_flush();
    }
    free(backup);
    free(pkg);
}

/* 0x205DA battle_field_preset：战场重入预设（st15/16/18/20_exit 共用）。 */
static void battle_field_preset(void)
{
    g_text_busy = 0;
    g_state_pending = 0;
    battle_stage_load(g_state);
    memset(g_spawner_flags, 0, sizeof(g_spawner_flags));
    g_scroll_x = g_scroll_y = g_cursor_x = g_cursor_y = 0;
    g_cursor_view_x = g_cursor_view_y = 0;
    anim_tick_update(1);
    g_text_busy = 1;
    fade_in();
    g_turn_count = 1;
    kbd_flush();     /* 0x20678 jmp loc_17EE8：共享尾 = call kbd_flush */
}

/* 0x25089 roster_full_heal：全 roster 清 flag1（rec[+5]=0）+ HP/MP
 * 回满（+64←+66、+68←+70）。 */
static void roster_full_heal(void)
{
    for (int i = 0; i < g_roster_count; i++) {
        uint8_t *r = g_roster_table[i];
        r[5] = 0;
        *(uint16_t *)(r + 64) = *(uint16_t *)(r + 66);
        *(uint16_t *)(r + 68) = *(uint16_t *)(r + 70);
    }
}

/* 0x1D4F6 sfx_magic_stop：停当前魔法音效通道。 */
static void sfx_magic_stop(void)
{
    sfx_play(g_magic_sfx_pkg, -1, 1);
}

/**** 生成：st05..29 enter/exit（反编译证据驱动，2026-09-05） ****/

/* ---- battle_field_init 布阵表（2026-09-13 全量重导定案）----
 * 旧定义为 4B 间隔重叠窗，锚点/长度全错。真值按各调用点 movsd/movsb
 * 拷贝序逐指令定案，内容 = get_bytes 0x520BA..0x52363：
 * x/y 写 ent[+0]/[+1]，第三表写 ent[+3] 朝向（0=下 1=左 2=上 3=右；
 * 调用点压入 <4 常量时为全队常量朝向）。区域 0x52113..0x52128 的
 * 22B 非 battle_field_init 表（st05/其他消费者），不入此块。 */
static const uint8_t s_st06_x[9]  = { 0x0C,0x0B,0x0D,0x0A,0x0E,0x0A,0x0E,0x09,0x0F };      /* @0x520E4 */
static const uint8_t s_st06_y[9]  = { 0x04,0x04,0x04,0x05,0x05,0x06,0x06,0x07,0x07 };      /* @0x520ED */
static const uint8_t s_st06_dir[9]= { 0x00,0x00,0x00,0x03,0x01,0x03,0x01,0x03,0x01 };      /* @0x520F6 */
static const uint8_t s_st07_x[10] = { 0x0E,0x0D,0x0F,0x0C,0x0D,0x0E,0x10,0x0B,0x0F,0x11 }; /* @0x520FF */
static const uint8_t s_st07_y[10] = { 0x14,0x14,0x14,0x13,0x13,0x12,0x13,0x12,0x13,0x12 }; /* @0x52109（dir=常量2） */
static const uint8_t s_st11_x[14] = { 0x0A,0x0B,0x09,0x0C,0x08,0x0A,0x0B,0x09,0x0C,0x08,0x08,0x0C,0x08,0x0C }; /* @0x52129 */
static const uint8_t s_st11_y[14] = { 0x04,0x04,0x04,0x04,0x04,0x05,0x05,0x05,0x05,0x05,0x03,0x03,0x02,0x02 }; /* @0x52137 */
static const uint8_t s_st11_dir[14]={ 0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x03,0x01,0x03,0x01 }; /* @0x52145 */
static const uint8_t s_st13_x[16] = { 0x12,0x11,0x13,0x12,0x11,0x13,0x10,0x14,0x10,0x0F,0x0F,0x10,0x14,0x15,0x15,0x14 }; /* @0x52153 */
static const uint8_t s_st13_y[16] = { 0x0F,0x0F,0x0F,0x10,0x10,0x10,0x0F,0x0F,0x0C,0x0D,0x0E,0x0E,0x0C,0x0D,0x0E,0x0E }; /* @0x52163 */
static const uint8_t s_st13_dir[16]={ 0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x03,0x03,0x03,0x03,0x01,0x01,0x01,0x01 }; /* @0x52173 */
static const uint8_t s_st15_x[16] = { 0x1C,0x1B,0x1C,0x1D,0x1E,0x19,0x1A,0x1B,0x1A,0x1D,0x1E,0x1F,0x19,0x1A,0x1E,0x1F }; /* @0x52183（dir=常量0） */
static const uint8_t s_st15_y[16] = { 0x1C,0x1B,0x1B,0x1B,0x1B,0x1C,0x1C,0x1C,0x1B,0x1C,0x1C,0x1C,0x1D,0x1D,0x1D,0x1D }; /* @0x52193 */
static const uint8_t s_st16_x[16] = { 0x17,0x16,0x17,0x18,0x15,0x16,0x17,0x18,0x19,0x14,0x15,0x16,0x17,0x18,0x19,0x1A }; /* @0x521A3（dir=常量0） */
static const uint8_t s_st16_y[16] = { 0x12,0x13,0x13,0x13,0x14,0x14,0x14,0x14,0x14,0x15,0x15,0x15,0x15,0x15,0x15,0x15 }; /* @0x521B3 */
static const uint8_t s_unk_521C3[17] = { 0x16,0x16,0x15,0x15,0x15,0x15,0x14,0x14,0x14,0x14,0x16,0x17,0x18,0x16,0x17,0x18,0x19 };  /* @0x521C3 st17 x17（2026-09-07 get_bytes） */
static const uint8_t s_unk_521D4[17] = { 0x07,0x08,0x06,0x07,0x08,0x09,0x06,0x07,0x08,0x09,0x05,0x05,0x05,0x0A,0x0A,0x0A,0x07 };  /* @0x521D4 st17 y17 */
static const uint8_t s_unk_521E5[17] = { 0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x00,0x00,0x02,0x02,0x02,0x01 };  /* @0x521E5 st17 dir17 */
static const uint8_t s_st20_x[25] = { 0x15,0x14,0x16,0x16,0x13,0x13,0x13,0x13,0x12,0x14,0x14,0x14,0x12,0x15,0x15,0x15,0x15,0x13,0x12,0x16,0x11,0x11,0x11,0x17,0x17 }; /* @0x52228 */
static const uint8_t s_st20_y[25] = { 0x0E,0x0E,0x0D,0x0E,0x0E,0x0F,0x10,0x11,0x0E,0x0F,0x10,0x11,0x0D,0x0F,0x10,0x11,0x0B,0x0B,0x0B,0x0B,0x0C,0x0D,0x0E,0x0C,0x0D }; /* @0x52241 */
static const uint8_t s_st20_dir[25]= { 0x02,0x02,0x01,0x01,0x02,0x02,0x02,0x02,0x03,0x02,0x02,0x02,0x03,0x02,0x02,0x02,0x00,0x00,0x00,0x00,0x03,0x03,0x03,0x01,0x01 }; /* @0x5225A */
static const uint8_t s_st21_x[16] = { 0x16,0x16,0x15,0x17,0x14,0x15,0x16,0x17,0x18,0x14,0x15,0x16,0x17,0x18,0x15,0x17 }; /* @0x52273 */
static const uint8_t s_st21_y[16] = { 0x16,0x14,0x16,0x16,0x17,0x17,0x17,0x17,0x17,0x18,0x18,0x18,0x18,0x18,0x19,0x19 }; /* @0x52283 */
static const uint8_t s_st21_dir[16]= { 0x02,0x00,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02 }; /* @0x52293 */
static const uint8_t s_st22_x[17] = { 0x14,0x14,0x12,0x13,0x14,0x15,0x16,0x12,0x16,0x12,0x13,0x15,0x16,0x13,0x14,0x15,0x13 }; /* @0x522A3 */
static const uint8_t s_st22_y[17] = { 0x13,0x11,0x12,0x12,0x12,0x12,0x12,0x11,0x11,0x10,0x10,0x10,0x10,0x0F,0x0F,0x0F,0x15 }; /* @0x522B4 */
static const uint8_t s_st22_dir[17]= { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02 }; /* @0x522C5 */
static const uint8_t s_st25_x[16] = { 0x0E,0x0F,0x0F,0x0E,0x10,0x0E,0x0F,0x10,0x0D,0x0E,0x0F,0x10,0x11,0x0E,0x0F,0x10 }; /* @0x522D6 */
static const uint8_t s_st25_y[16] = { 0x06,0x09,0x06,0x09,0x09,0x0A,0x0A,0x0A,0x0B,0x0B,0x0B,0x0B,0x0B,0x0C,0x0C,0x0C }; /* @0x522E6 */
static const uint8_t s_st25_dir[16]= { 0x00,0x02,0x00,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02 }; /* @0x522F6 */
static const uint8_t s_st26_x[16] = { 0x0F,0x0F,0x0C,0x0D,0x11,0x12,0x0D,0x0E,0x10,0x11,0x0E,0x0F,0x10,0x0E,0x0F,0x10 }; /* @0x52306（dir=常量2） */
static const uint8_t s_st26_y[16] = { 0x0D,0x0B,0x0C,0x0C,0x0C,0x0C,0x0D,0x0D,0x0D,0x0D,0x0E,0x0E,0x0E,0x0F,0x0F,0x0F }; /* @0x52316 */
static const uint8_t s_st29_x[20] = { 0x16,0x16,0x14,0x15,0x17,0x18,0x14,0x15,0x17,0x18,0x14,0x15,0x16,0x17,0x18,0x14,0x15,0x16,0x17,0x18 }; /* @0x52327 */
static const uint8_t s_st29_y[20] = { 0x17,0x13,0x16,0x16,0x16,0x16,0x17,0x17,0x17,0x17,0x18,0x18,0x18,0x18,0x18,0x19,0x19,0x19,0x19,0x19 }; /* @0x5233B */
static const uint8_t s_st29_dir[20]= { 0x02,0x00,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02 }; /* @0x5234F */

void st05_enter(void)
{
    /* IDA 0x23296 全解（2026-09-13 重导定案，旧版截断在共享尾前）：
     * spawn(13) → reload(3) → camera(5,14) → script27 →
     * **say(6) → sync → ++g_state**（尾 jmp loc_231DF 共享尾）。
     * 第五章战斗胜利相位——第五章结束的对白即此 str6。 */
    roster_spawn_ent(13);
    sprites_reload_slot(3);
    camera_scroll_to(5, 14);
    ui_script_exec(27);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 6, g_pkg_text_evt);
    battle_sync_to_roster();
    ++g_state;
}

void st06_enter(void)
{
    /* IDA 0x232E8 全解（2026-09-13 真 ABI 重导）：sync →
     * g_spawner_flags[17]==1 且 ent43 存活（flag1_test==0）→ 战斗布阵
     * 路径：battle_field_init(st06表@0x520E4/ED/F6, 0..8, leader43@(12,7)
     * 朝2, cursor(6,2)) → str4 → roster_spawn_ent(12)；否则仅 str5。
     * 两支均 ++g_state（0x233BA）。 */
    battle_sync_to_roster();
    if (g_spawner_flags[17] == 1 && !ent_flag1_test(43)) {
        battle_field_init(s_st06_x, s_st06_y, s_st06_dir,
                          0, 8, 43, 12, 7, 2, 6, 2);
        text_render_box(1, 19, 74, 205, 320, vram_base(), 4, g_pkg_text_evt);
        roster_spawn_ent(12);
    } else {
        text_render_box(1, 19, 74, 205, 320, vram_base(), 5, g_pkg_text_evt);
    }
    ++g_state;
}

void st07_enter(void)
{
    /* IDA 0x234BB 全解（2026-09-13 真 ABI 重导）：
     * battle_field_init(st07.x@0x520FF, st07.y@0x52109, 常量朝向2, 0..9,
     * leader28@(14,16)朝0, cursor(8,14)) → str3+busy0+script33 →
     * str4+g_script_fade=1+busy0+script34+g_script_fade=0 →
     * palette_apply_range(0,255,64) + 清屏 0xFA00 → 共享尾 0x2327D
     * （spawn(5)+sync+inc g_state）。 */
    battle_field_init(s_st07_x, s_st07_y, (const uint8_t *)(uintptr_t)2,
                      0, 9, 28, 14, 16, 0, 8, 14);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 3, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(33);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 4, g_pkg_text_evt);
    g_script_fade = 1;
    g_text_busy = 0;
    ui_script_exec(34);
    g_script_fade = 0;
    palette_apply_range(0, 255, 64);
    memset(vram_base(), 0, 0xFA00);
    roster_spawn_ent(5);
    battle_sync_to_roster();
    ++g_state;
}

void st08_enter(void)
{
    /* IDA 0x235BC 全解（2026-09-13 重导定案，旧版截断在共享尾前）：
     * ent11[+5]=0（清标志）→ camera(6,1) → reload(4) → script36 →
     * **say(4) → sync → ++g_state**（尾 jmp loc_231C6 共享尾，str_id=4
     * 在尾内压栈——与 st03 同一尾块）。 */
    if (g_ent_table && g_ent_count > 11)
        g_ent_table[11][5] = 0;
    camera_scroll_to(6, 1);
    sprites_reload_slot(4);
    ui_script_exec(36);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 4, g_pkg_text_evt);
    battle_sync_to_roster();
    ++g_state;
}

void st09_enter(void)
{
    /* IDA 0x235F9 全解（2026-09-13 逐指令重核，旧版只剩中段对白骨架）：
     * 拷 x/y 表（0x52113/0x5211E 各 11B）→ fade_black + load_map →
     * **手写布阵循环**（非 battle_field_init）：ents 0..10 写
     * (x[i], y[i], 朝向=2) → ent50=(15,35)、ent51=(14,35)、
     * ent52=(16,35) 且 [+38]=0（52 另清 [5]=0 复活）→ ent5[5]=0 →
     * busy0 + scroll/cursor=(9,34) + view=0 → anim_tick(1) + fade_in +
     * delay(200) → 旁白4 + busy0 + script37 → 旁白5 → sync →
     * spawn(11)+spawn(6)（NPC 首领 ent51 与客座角色 ent52 入队，
     * 对应 FDTXT b010 str4/5 的槽 50/51 长对话）→ ++g_state。 */
    static const uint8_t s_st09_x[11] = { 0x0E,0x0F,0x10,0x0D,0x0E,0x0F,0x10,0x11,0x0E,0x0F,0x10 }; /* @0x52113 */
    static const uint8_t s_st09_y[11] = { 0x26,0x27,0x26,0x26,0x27,0x26,0x27,0x27,0x28,0x28,0x28 }; /* @0x5211E */
    uint8_t *e;

    palette_fade_black();
    battle_load_map();
    for (int i = 0; i <= 10; i++) {
        e = g_ent_table[i];
        e[0] = s_st09_x[i];
        e[1] = s_st09_y[i];
        e[3] = 2;
    }
    e = g_ent_table[50];
    e[0] = 15; e[1] = 35; e[38] = 0;
    e = g_ent_table[51];
    e[0] = 14; e[1] = 35; e[38] = 0;
    e = g_ent_table[52];
    e[0] = 16; e[1] = 35; e[38] = 0;
    e[5] = 0;
    g_ent_table[5][5] = 0;
    g_text_busy = 0;
    g_scroll_x = 9;
    g_scroll_y = 34;
    g_cursor_x = 9;
    g_cursor_y = 34;
    g_cursor_view_x = 0;
    g_cursor_view_y = 0;
    anim_tick_update(1);
    fade_in();
    delay_ms(200);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 4, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(37);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 5, g_pkg_text_evt);
    battle_sync_to_roster();
    roster_spawn_ent(11);
    roster_spawn_ent(6);
    ++g_state;                              /* 0x23783 */
}

void st10_enter(void)
{
    /* IDA 0x23790 全解（2026-09-13 重导定案）：say(3) → sync →
     * spawn(14) → **++g_state**（尾 jmp loc_231F2，旧版缺 ++）。 */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 3, g_pkg_text_evt);
    battle_sync_to_roster();
    roster_spawn_ent(14);
    ++g_state;
}

void st11_enter(void)
{
    /* IDA 0x237D5 全解（2026-09-13 真 ABI 重导）：
     * battle_field_init(st11表@0x52129/37/45, 0..13, leader14@(10,2)朝0,
     * cursor(4,0)) → str3+script45+str4 → sync → spawn(17)+inc g_state
     * （0x2389A jmp loc_239B1）。 */
    battle_field_init(s_st11_x, s_st11_y, s_st11_dir,
                      0, 13, 14, 10, 2, 0, 4, 0);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 3, g_pkg_text_evt);
    ui_script_exec(45);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 4, g_pkg_text_evt);
    battle_sync_to_roster();
    roster_spawn_ent(17);
    ++g_state;
}

void st12_enter(void)
{
    /* IDA loc_2389F（st11_13 函数相位入口，2026-09-13 定案）：无布阵——
     * str9 → sync → spawn(3)+inc g_state（0x238D5 push 3 + jmp
     * loc_237C8 共享尾）。旧实现误抄 st11 全套布阵/脚本，已重写。 */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 9, g_pkg_text_evt);
    battle_sync_to_roster();
    roster_spawn_ent(3);
    ++g_state;
}

void st13_enter(void)
{
    /* IDA loc_238DC（st11_13 函数相位入口，2026-09-13 真 ABI 重导）：
     * sprites_reload_slot(1) 先 → battle_field_init(st13表@0x52153/63/73,
     * 0..15, 无 leader, cursor(12,10)) → str2+busy0+script47+str3 →
     * sync+inc g_state（loc_239AC 共享尾）。 */
    sprites_reload_slot(1);
    battle_field_init(s_st13_x, s_st13_y, s_st13_dir,
                      0, 15, 0, 0, 0, 0, 12, 10);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 2, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(47);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 3, g_pkg_text_evt);
    battle_sync_to_roster();
    ++g_state;
}

void st14_enter(void)
{
    /* IDA 0x239BD 全解（2026-09-13 重导定案，旧版 str19 为臆造）：
     * 动态对白 id = roster_has_unit(12) ? 12 : 13（0x239D1 xor al,1 +
     * add al,0Ch 算术）→ sync → spawn(15) → ++g_state（尾 push 0Fh +
     * jmp loc_237C8）。 */
    text_render_box(1, 19, 74, 205, 320, vram_base(),
                    roster_has_unit(12) ? 12 : 13, g_pkg_text_evt);
    battle_sync_to_roster();
    roster_spawn_ent(15);
    ++g_state;
}

void st15_enter(void)
{
    /* IDA 0x23A0A 全解（2026-09-13 真 ABI 重导）：
     * battle_field_init(st15.x@0x52183, st15.y@0x52193, 常量朝向0, 0..15,
     * leader65@(28,30)朝2, cursor(22,25)) → ent66..73 死亡计数>4 置标记
     * → sync → turn>18 || 标记 || ent0.MaxHP(+66 u16)<0x140 分支：
     * str2+busy0+script49+str3；否则 str4+spawn(18)。尾 ++g_state。 */
    int dead = 0;
    int over = 0;

    battle_field_init(s_st15_x, s_st15_y, (const uint8_t *)(uintptr_t)0,
                      0, 15, 65, 28, 30, 2, 22, 25);
    for (int i = 66; i < 74; i++) {
        if (ent_flag1_test(i))
            dead++;
    }
    if (dead > 4)
        over = 1;
    battle_sync_to_roster();
    if (g_turn_count > 18 || over == 1 ||
        *(uint16_t *)(g_ent_table[0] + 66) < 0x140) {
        text_render_box(1, 19, 74, 205, 320, vram_base(), 2, g_pkg_text_evt);
        g_text_busy = 0;
        ui_script_exec(49);
        text_render_box(1, 19, 74, 205, 320, vram_base(), 3, g_pkg_text_evt);
    } else {
        text_render_box(1, 19, 74, 205, 320, vram_base(), 4, g_pkg_text_evt);
        roster_spawn_ent(18);
    }
    ++g_state;
}

void st16_enter(void)
{
    /* IDA 0x23B5F 全解（2026-09-13 真 ABI 重导）：sync → roster_has_unit(18)
     * 分支——有：str5+busy0+camera(17,14)+reload(3)+script52；
     * 无：battle_field_init(st16.x@0x521A3, st16.y@0x521B3, 常量朝向0,
     * 0..15, leader52@(23,23)朝2, cursor(17,17)) + str7+busy0+script50+
     * camera(17,14)+reload(3)+script51。共尾 str6+script53+str8+
     * spawn(16)+inc g_state。 */
    battle_sync_to_roster();
    if (roster_has_unit(18)) {
        text_render_box(1, 19, 74, 205, 320, vram_base(), 5, g_pkg_text_evt);
        g_text_busy = 0;
        camera_scroll_to(17, 14);
        sprites_reload_slot(3);
        ui_script_exec(52);
    } else {
        battle_field_init(s_st16_x, s_st16_y, (const uint8_t *)(uintptr_t)0,
                          0, 15, 52, 23, 23, 2, 17, 17);
        text_render_box(1, 19, 74, 205, 320, vram_base(), 7, g_pkg_text_evt);
        g_text_busy = 0;
        ui_script_exec(50);
        camera_scroll_to(17, 14);
        sprites_reload_slot(3);
        ui_script_exec(51);
    }
    text_render_box(1, 19, 74, 205, 320, vram_base(), 6, g_pkg_text_evt);
    ui_script_exec(53);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 8, g_pkg_text_evt);
    roster_spawn_ent(16);
    ++g_state;
}

void st17_enter(void)
{
    /* IDA 0x23CD5（2026-09-07 文本 id 修正：0x23D60/0x23D9B/0x23DD6/
     * 0x23E11 = 7/8/9/10，此前全 0 为臆测）；尾 ++g_state（0x23E2D）。 */
    battle_sync_to_roster();
    battle_field_init(s_unk_521C3, s_unk_521D4, s_unk_521E5,
                      0, 16, 17, 25, 8, 1, 0x12, 4);  /* nums=[0, 16, 17, 25, 8, 1, 18, 4] */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 7, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(56);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 8, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(57);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 9, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(58);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 10, g_pkg_text_evt);
    g_text_busy = 0;
    roster_spawn_ent(21);
    roster_spawn_ent(7);
    ++g_state;
}

void st18_enter(void)
{
    /* IDA 0x23E39 全解（2026-09-13 重导定案）：sync → say(3) →
     * **++g_state**（尾 jmp loc_231F2，旧版缺 ++）。 */
    battle_sync_to_roster();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 3, g_pkg_text_evt);
    ++g_state;
}

void st19_enter(void)
{
    /* IDA 0x23E74 全解（2026-09-07，get_bytes 定表）：body 与
     * battle_field_init 同构——前 16 单位布阵（x@0x521F6[0..15]、
     * y@0x521F6[16..31]、team=1），52..60 增援 9 单位（x@0x52216、
     * y@0x5221F、team=3）+ 尾 anim_tick_update(1)+fade_in+delay(200)
     * + 开场文本/脚本 + 增援段（turn<=15）。 */
    static const uint8_t x16[16] = {
        0x21,0x21,0x21,0x22,0x22,0x22,0x23,0x23,
        0x23,0x23,0x23,0x24,0x24,0x24,0x24,0x24
    };
    static const uint8_t y16[16] = {
        0x23,0x24,0x22,0x22,0x23,0x24,0x21,0x22,
        0x23,0x24,0x25,0x21,0x22,0x23,0x24,0x25
    };
    static const uint8_t x9[9] = {
        0x1e,0x1c,0x1c,0x1c,0x1c,0x1c,0x1d,0x1d,0x1d
    };
    static const uint8_t y9[9] = {
        0x23,0x25,0x24,0x23,0x22,0x21,0x24,0x23,0x22
    };

    palette_fade_black();
    battle_load_map();
    for (int i = 0; i < 16; i++) {          /* 0x23EC4 布阵 A */
        uint8_t *rec = g_ent_table[i];
        rec[0] = x16[i];
        rec[1] = y16[i];
        rec[3] = 1;
    }
    for (int j = 0; j < 9; j++) {           /* 0x23EF1 布阵 B（52..60） */
        uint8_t *rec = g_ent_table[j + 52];
        rec[0] = x9[j];
        rec[1] = y9[j];
        rec[3] = 3;
    }
    g_text_busy = 0;
    g_scroll_x = 26;
    g_scroll_y = 31;
    g_cursor_x = 26;
    g_cursor_y = 31;
    g_cursor_view_x = 0;
    g_cursor_view_y = 0;
    anim_tick_update(1);                    /* 0x23F69 原版 a14=1 */
    fade_in();
    delay_ms(200);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 11, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(59);                     /* 0x23FB6 */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 12, g_pkg_text_evt);
    g_text_busy = 0;
    roster_spawn_ent(25);                   /* 0x23FF1 */
    battle_sync_to_roster();
    /* 0x24005：turn<=15 时插入增援段（sprites_reload(1)+脚本 60-62
     * +文本 14-16 + roster_spawn_ent(28)），否则直接文本 13。 */
    if (g_turn_count <= 15) {
        sprites_reload_slot(1);
        ui_script_exec(60);
        text_render_box(1, 19, 74, 205, 320, vram_base(), 14, g_pkg_text_evt);
        g_text_busy = 0;
        ui_script_exec(61);
        text_render_box(1, 19, 74, 205, 320, vram_base(), 15, g_pkg_text_evt);
        g_text_busy = 0;
        ui_script_exec(62);
        text_render_box(1, 19, 74, 205, 320, vram_base(), 16, g_pkg_text_evt);
        roster_spawn_ent(28);
    }
    text_render_box(1, 19, 74, 205, 320, vram_base(), 13, g_pkg_text_evt);
    ++g_state;                              /* 0x240ED：st19 尾 = ++state */
}

void st20_enter(void)
{
    /* IDA 0x240FA（2026-09-14 复核更正）：init → **旁白5**（旧 str0）
     * → 清点全队 0xD1..0xD6 六件道具 → ==6：回收 + 发 item 100 +
     * say(7)+busy0+script63 + say(8)+busy0+script64 + say(9) →
     * **过场胶片 + say(10)**（0x242C9/0x242E5——仅此分支播）；否则
     * 仅 say(6)（0x242E9 直落共享渲染，跳过胶片）。两支共尾：
     * spawn(24)+spawn(23) → sync → ++g_state。 */
    battle_field_init(s_st20_x, s_st20_y, s_st20_dir,
                      0, 24, 25, 23, 14, 1, 14, 10);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 5, g_pkg_text_evt);
    {
        int total = 0;
        for (int u = 0; u < 0x10; u++)
            for (int it = -47; it < 0xD7; it++)
                if (inventory_find_item_slot(u, it) != -1)
                    total++;
        if (total == 6) {
            for (int it = -47; it < 0xD7; it++)
                for (int u = 0; u < 0x10; u++) {
                    int slot = inventory_find_item_slot(u, it);
                    if (slot != -1)
                        inventory_remove_item(u, slot);
                }
            grant_item_first_player(100);
            text_render_box(1, 19, 74, 205, 320, vram_base(), 7, g_pkg_text_evt);
            g_text_busy = 0;
            ui_script_exec(63);
            text_render_box(1, 19, 74, 205, 320, vram_base(), 8, g_pkg_text_evt);
            g_text_busy = 0;
            ui_script_exec(64);
            text_render_box(1, 19, 74, 205, 320, vram_base(), 9, g_pkg_text_evt);
            cutscene_film_play();   /* 0x242C9：==6 分支专有（blk34 帧序列） */
            text_render_box(1, 19, 74, 205, 320, vram_base(), 10, g_pkg_text_evt);
        } else {
            text_render_box(1, 19, 74, 205, 320, vram_base(), 6, g_pkg_text_evt);
        }
    }
    roster_spawn_ent(24);
    roster_spawn_ent(23);
    battle_sync_to_roster();
    ++g_state;
}

void st21_enter(void)
{
    /* IDA 0x244B6 全解（2026-09-13 disasm 逐指令定案，此前空壳）：
     * battle_field_init(st21表@0x52273/83/93, 0..15, leader72@(22,25)朝2,
     * cursor(16,18)) → str4+script65 → str5+camera(16,16)+script66 →
     * str6+camera(16,14) → cast_beam_fall(view_x, view_y+3, 10, 8) →
     * delay(500)+白屏(0xFF,0xFA00)+palette_fade_black+清屏 → 共享尾
     * loc_239AC（sync+inc g_state）。 */
    battle_field_init(s_st21_x, s_st21_y, s_st21_dir,
                      0, 15, 72, 22, 25, 2, 16, 18);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 4, g_pkg_text_evt);
    ui_script_exec(65);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 5, g_pkg_text_evt);
    camera_scroll_to(16, 16);
    ui_script_exec(66);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 6, g_pkg_text_evt);
    camera_scroll_to(16, 14);
    cast_beam_fall(g_cursor_view_x, g_cursor_view_y + 3, 10, 8);
    delay_ms(500);
    memset(vram_base(), 0xFF, 0xFA00);
    palette_fade_black();
    memset(vram_base(), 0, 0xFA00);
    battle_sync_to_roster();
    ++g_state;
}

void st22_enter(void)
{
    /* IDA 0x24754：物品/角色分支及三段登场演出逐指令对齐。 */
    battle_field_init(s_st22_x, s_st22_y, s_st22_dir,
                      0, 16, 17, 21, 21, 2, 14, 14);
    if (inventory_has_item(100) != -1) {
        text_render_box(1, 19, 74, 205, 320, vram_base(), 8, g_pkg_text_evt);
        roster_spawn_ent(22);
    } else {
        text_render_box(1, 19, 74, 205, 320, vram_base(), 9, g_pkg_text_evt);
        g_text_busy = 0;
        ui_script_exec(71);
    }
    if (roster_has_unit(13)) {
        text_render_box(1, 19, 74, 205, 320, vram_base(), 10, g_pkg_text_evt);
        g_text_busy = 0;
        ui_script_exec(72);
        ent_mark_flag1(17);
        text_render_box(1, 19, 74, 205, 320, vram_base(), 11, g_pkg_text_evt);
    } else if (g_turn_count < 15) {
        text_render_box(1, 19, 74, 205, 320, vram_base(), 13, g_pkg_text_evt);
        roster_spawn_ent(19);
    } else {
        text_render_box(1, 19, 74, 205, 320, vram_base(), 12, g_pkg_text_evt);
        g_text_busy = 0;
        ui_script_exec(72);
        ent_mark_flag1(17);
    }
    battle_sync_to_roster();
    ++g_state;
    text_render_box(1, 19, 74, 205, 320, vram_base(), 14, g_pkg_text_evt);
    delay_ms(400);
    /* 0x24978/0x249C4/0x24A10 cast_beam_rise(unit=1, 15,10)/(1,15,10)/
     * (1,30,16)：单位 1 屏位圆心的圆形 LUT 光晕，半径 15/30 起每帧
     * +10/+16 扩张（2026-09-08 半径语义定案，原像素 y 解读已废弃）。 */
    {
        uint8_t *u1 = (g_ent_table && g_ent_count > 1)
                          ? g_ent_table[1] : NULL;
        int bx = u1 ? 24 * ((int)u1[0] - (int)g_scroll_x) + 12 : 12;
        int by = u1 ? 24 * ((int)u1[1] - (int)g_scroll_y) + 18 : 18;
        cast_beam_rise(bx, by, 15, 10);
    }
    battle_quake(30);   /* 0x24B4D */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 15, g_pkg_text_evt);
    delay_ms(400);
    {
        uint8_t *u1 = (g_ent_table && g_ent_count > 1)
                          ? g_ent_table[1] : NULL;
        int bx = u1 ? 24 * ((int)u1[0] - (int)g_scroll_x) + 12 : 12;
        int by = u1 ? 24 * ((int)u1[1] - (int)g_scroll_y) + 18 : 18;
        cast_beam_rise(bx, by, 15, 10);
    }
    battle_quake(30);   /* 0x24B4D */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 16, g_pkg_text_evt);
    delay_ms(400);
    {
        uint8_t *u1 = (g_ent_table && g_ent_count > 1)
                          ? g_ent_table[1] : NULL;
        int bx = u1 ? 24 * ((int)u1[0] - (int)g_scroll_x) + 12 : 12;
        int by = u1 ? 24 * ((int)u1[1] - (int)g_scroll_y) + 18 : 18;
        cast_beam_rise(bx, by, 30, 16);
    }
    /* 0x24A18 尾段：palette 0..62 步进 2 delay(4)
     * 闪屏 → 换地形三连 FDFIELD 69 / FDSHAP 46 / 47（地形变化剧情）
     * → field_reset_candidates + battle_backdrop_load@0x10652 →
     * camera(14,29) → palette 复位 → 脚本 73 + camera(14,14) +
     * 脚本 73×2 + 文本 17。 */
    for (int i = 0; i < 64; i += 2) {
        palette_add_range(0, 255, i);
        delay_ms(4);
    }
    g_field_map = dat_load_block("FDFIELD.DAT", g_field_map, 69);
    g_field_map_size = g_last_block_size;
    if (g_field_map && g_field_map_size >= 4) {
        const uint8_t *hdr = (const uint8_t *)g_field_map;
        g_field_w = (int16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
        g_field_h = (int16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
        g_view_w = g_field_w > 13 ? 13 : g_field_w;
        g_view_h = g_field_h > 8 ? 8 : g_field_h;
    }
    g_shape_tiles = dat_load_block("FDSHAP.DAT", g_shape_tiles, 46);
    g_shape_tiles_size = g_last_block_size;
    g_shape_map = dat_load_block("FDSHAP.DAT", g_shape_map, 47);
    g_shape_map_size = g_last_block_size;
    field_reset_candidates();
    battle_backdrop_load();
    camera_scroll_to(14, 29);
    palette_add_range(0, 255, 0);
    ui_script_exec(73);
    camera_scroll_to(14, 14);
    ui_script_exec(73);
    ui_script_exec(73);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 17, g_pkg_text_evt);
}

void st23_enter(void)
{
    /* IDA 0x24C1E 全解：升腾加速演出——卷速 2..9 各 30
     * tick（sub_24D22(i) 置速 + anim_tick_update(1) 内逐 tick 上卷），
     * 再 10..14 各 12 tick 带调色板 0..59 渐白 → 清屏 64000 → 尾链
     * battle_sync_to_roster + g_state++。原版此处没有 spawn 或调色板
     * 复位的额外动作。 */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 2, g_pkg_text_evt);
    g_text_busy = 0;
    for (int i = 2; i < 10; i++) {          /* 0x24C5A 加速段 */
        battle_backdrop_scroll(i);          /* 0x24C82 sub_24D22(i) */
        for (int j = 0; j < 30; j++) {
            anim_tick_update(1);            /* 0x120B1 每 tick 上卷+blit */
            wait_bios_ticks(1);
        }
    }
    text_render_box(1, 19, 74, 205, 320, vram_base(), 3, g_pkg_text_evt);
    g_text_busy = 0;
    {                                        /* 0x24CB8 渐白段 */
        int pal = 0;
        for (int i = 10; i < 15; i++) {
            battle_backdrop_scroll(i);
            for (int j = 0; j < 12; j++) {
                palette_apply_range(0, 255, pal);
                anim_tick_update(0);
                wait_bios_ticks(1);
                pal++;
            }
        }
    }
    memset(vram_base(), 0, FD2_VRAM_SIZE);  /* 0x24D0B */
    battle_sync_to_roster();                 /* 0x24D13 */
    ++g_state;                               /* 0x24D18 */
}

void st24_enter(void)
{
    /* IDA 0x24DF2：str6 -> camera(4,16) -> reload(2) -> script75
     * -> str7 -> spawn(26) -> sync；随后落入 0x237C8 共享尾，继续
     * str3 -> sync -> spawn(14) -> ++g_state。 */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 6, g_pkg_text_evt);
    camera_scroll_to(4, 16);
    sprites_reload_slot(2);
    ui_script_exec(75);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 7, g_pkg_text_evt);
    roster_spawn_ent(26);
    battle_sync_to_roster();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 3, g_pkg_text_evt);
    battle_sync_to_roster();
    roster_spawn_ent(14);
    ++g_state;
}

void st25_enter(void)
{
    /* IDA 0x24E80 全解（2026-09-13 重导）：i=16..count-1 中 ent[+7]==31
     * 的实体移到 (16,6) → battle_field_init(st25表@0x522D6/E6/F6, 0..15,
     * 无 leader, cursor(9,5)) → str(spawner[12]+5)+busy0+script77 →
     * str7+busy0+script78 → str(spawner[12]+8)+busy0+script79 →
     * str10+busy0+script80 → str11 → sync → inc g_state（尾 jmp
     * loc_15487 = 共享返回 thunk，无 spawn）。动态文本 id 旧版全 0，
     * 已按 0x24F43/0x24FC4 修正。 */
    for (int i = 16; i < g_ent_count; i++) {
        if (g_ent_table[i][7] == 31) {
            g_ent_table[i][0] = 16;
            g_ent_table[i][1] = 6;
        }
    }
    battle_field_init(s_st25_x, s_st25_y, s_st25_dir,
                      0, 15, 0, 0, 0, 0, 9, 5);
    text_render_box(1, 19, 74, 205, 320, vram_base(),
                    g_spawner_flags[12] + 5, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(77);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 7, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(78);
    text_render_box(1, 19, 74, 205, 320, vram_base(),
                    g_spawner_flags[12] + 8, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(79);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 10, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(80);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 11, g_pkg_text_evt);
    battle_sync_to_roster();
    ++g_state;
}

void st26_enter(void)
{
    /* IDA 0x250CC（2026-09-14 逐指令复核）：
     * 清 ent[0..15].flag1 → battle_field_init(2,0,15,0,0,0,0,9,8) →
     * 文本 8 + busy0 + 脚本 82 → inventory_has_item(100)!=-1 分支：
     *   文本 9 + busy0 + 脚本 83 + 文本 10 + busy0 + camera(9,8) + 脚本 84 + 文本 11
     *   + 五级渐隐（80,5/4/3/2/2/2 + delay 500/250/100/50）
     *   + cast_beam_fall(view_x, view_y-1, 10, 10) + delay(500) + 白屏
     *   + 淡黑 + 清屏 + battle_sync + g_state++ + roster_full_heal
     *   → JUMPOUT 0x1B5EA（battle 后续）；
     * else：文本 13 + busy0 + 脚本 84 + 文本 14 + busy0 + 脚本 82 + 文本 15
     *   + fx_effect_apply(0,19,1) + cutscene_walk_step(ent1 原地
     *   (x,y)→(x,y)) + 文本 16 + busy0 + roster_full_heal +
     *   ending_sequence_play + 死循环等待。 */
    for (int u = 0; u < 16; u++)
        if (u < g_ent_count)
            g_ent_table[u][5] = 0;
    battle_field_init(s_st26_x, s_st26_y, (const uint8_t *)(uintptr_t)2,
                      0, 15, 0, 0, 0, 0, 9, 8);   /* @0x52306/16 + 常量朝向2（byte_52326=1 为死拷贝不传参） */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 8, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(82);
    if (inventory_has_item(100) != -1) {
        text_render_box(1, 19, 74, 205, 320, vram_base(), 9, g_pkg_text_evt);
        g_text_busy = 0;
        ui_script_exec(83);
        text_render_box(1, 19, 74, 205, 320, vram_base(), 10, g_pkg_text_evt);
        g_text_busy = 0;
        camera_scroll_to(9, 8);
        ui_script_exec(84);
        text_render_box(1, 19, 74, 205, 320, vram_base(), 11, g_pkg_text_evt);
        fade_add_loop(80, 5);
        text_render_box(1, 19, 74, 205, 320, vram_base(), 12, g_pkg_text_evt);
        fade_add_loop(80, 4);
        delay_ms(500);
        fade_add_loop(80, 3);
        delay_ms(250);
        fade_add_loop(80, 2);
        delay_ms(100);
        fade_add_loop(80, 2);
        delay_ms(50);
        fade_add_loop(80, 2);
        cast_beam_fall(g_cursor_view_x, g_cursor_view_y - 1, 10, 10);   /* 0x252EE 4 参格坐标 */
        delay_ms(500);
        memset(vram_base(), 255, 64000);
        palette_fade_black();
        memset(vram_base(), 0, 64000);
        battle_sync_to_roster();
        g_state++;
        roster_full_heal();
        return;   /* JUMPOUT 0x1B5EA */
    }
    text_render_box(1, 19, 74, 205, 320, vram_base(), 13, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(84);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 14, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(82);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 15, g_pkg_text_evt);
    g_text_busy = 0;
    { uint8_t t = 0; fx_effect_apply(0, 19, 1, &t); }   /* 0x253FA (0,19,1) 实参 */
    {
        uint8_t *e1 = g_ent_table[1];   /* ent 表 80B 记录 1 */
        cutscene_stand(1, e1[0], e1[1]);
    }
    text_render_box(1, 19, 74, 205, 320, vram_base(), 16, g_pkg_text_evt);
    g_text_busy = 0;
    roster_full_heal();
    ending_sequence_play();
    for (;;)
        ;   /* 原版死循环（等待 NMI/退出） */
}

void st27_enter(void)
{
    /* IDA 0x25464（2026-09-06 定案）：JUMPOUT 0x231DF 共享尾
     * = 文本(7) + battle_sync_to_roster + g_state++（§3.3）。 */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 7, g_pkg_text_evt);
    battle_sync_to_roster();
    g_state++;
}

/* flash_pulse（0x25682 调用点语义，st29_exit 同款内联的独立helper）：
 * palette_add 0..63 步进1 delay(8) + delay(400) + 62..0 回落 delay(8)。 */
static void flash_pulse(void)
{
    for (int i = 0; i < 64; i++) {
        palette_add_range(0, 255, i);
        delay_ms(8);
    }
    delay_ms(400);
    for (int i = 62; i >= 0; i--) {
        palette_add_range(0, 255, i);
        delay_ms(8);
    }
}

/* IDA 0x2548C = g_state_enter[28]（结局对白，2026-09-13 全解，此前空壳）：
 * say(10) → ents_kill_from(20) → ent20[+7]/[+8]=0x7E（职业/名号改 0x7E）
 * → say(11) → reload(9) → camera(9,8) → cursor_scroll_to(15,10) →
 * cutscene_stand(末实体,15,10)（0x25535 walk_step 恒 from==to 形）→
 * say(12)+busy0 → 震屏组 quake(20)/delay(600)×2 + quake(20) → say(13)+
 * busy0 → quake(20)/delay(200)×2 + quake(20) → say(14)+busy0 →
 * quake(20)/delay(200)+quake(20)/delay(100)+quake(40)/delay(200) →
 * flash_pulse ×3（各 delay(300)）→ say(15)（无 busy0）→ palette 0..63
 * 步进1 delay(4) → 清屏 0xFA00 → delay(800) → palette 62..0 步进1
 * delay(4) → sync → ++g_state。 */
void st28_enter(void)
{
    text_render_box(1, 19, 74, 205, 320, vram_base(), 10, g_pkg_text_evt);
    ents_kill_from(20);
    g_ent_table[20][7] = 0x7E;
    g_ent_table[20][8] = 0x7E;
    text_render_box(1, 19, 74, 205, 320, vram_base(), 11, g_pkg_text_evt);
    sprites_reload_slot(9);
    camera_scroll_to(9, 8);
    cursor_scroll_to(15, 10);
    cutscene_stand(g_ent_count - 1, 15, 10);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 12, g_pkg_text_evt);
    g_text_busy = 0;
    battle_quake(20); delay_ms(600);
    battle_quake(20); delay_ms(600);
    battle_quake(20);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 13, g_pkg_text_evt);
    g_text_busy = 0;
    battle_quake(20); delay_ms(200);
    battle_quake(20); delay_ms(200);
    battle_quake(20);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 14, g_pkg_text_evt);
    g_text_busy = 0;
    battle_quake(20); delay_ms(200);
    battle_quake(20); delay_ms(100);
    battle_quake(40); delay_ms(200);
    flash_pulse(); delay_ms(300);
    flash_pulse(); delay_ms(300);
    flash_pulse(); delay_ms(300);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 15, g_pkg_text_evt);
    for (int i = 0; i < 64; i++) {
        palette_add_range(0, 255, i);
        delay_ms(4);
    }
    memset(vram_base(), 0, 0xFA00);
    delay_ms(800);
    for (int i = 62; i >= 0; i--) {
        palette_add_range(0, 255, i);
        delay_ms(4);
    }
    battle_sync_to_roster();
    ++g_state;
}

void st29_enter(void)
{
    /* IDA 0x25757 全解（2026-09-13 disasm 逐指令定案，此前空壳）：
     * battle_field_init(st29表@0x52327/3B/4F, 0..19, 无 leader,
     * cursor(16,18)) → str9+busy0+script88 → str10+camera(16,18)+
     * cursor_scroll_to(22,23) → cast_beam_fall(view_x, view_y+1, 10, 8) →
     * roster_full_heal → inc g_state → roster_full_heal+busy0+
     * battle_stage_load(g_state[新值]) → scroll/cursor=(11,5)/view=0 →
     * anim_tick(1) → palette 62..0 步进1 delay(4) → anim_tick(0)×40
     * (每 tick wait 1) → str0+busy0+camera(11,12)+script89+str1 →
     * ending_sequence_play → 死循环。 */
    battle_field_init(s_st29_x, s_st29_y, s_st29_dir,
                      0, 19, 0, 0, 0, 0, 16, 18);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 9, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(88);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 10, g_pkg_text_evt);
    camera_scroll_to(16, 18);
    cursor_scroll_to(22, 23);
    cast_beam_fall(g_cursor_view_x, g_cursor_view_y + 1, 10, 8);
    roster_full_heal();
    ++g_state;
    roster_full_heal();
    g_text_busy = 0;
    battle_stage_load(g_state);
    g_scroll_x = 11;
    g_scroll_y = 5;
    g_cursor_x = 11;
    g_cursor_y = 5;
    g_cursor_view_x = 0;
    g_cursor_view_y = 0;
    anim_tick_update(1);
    for (int i = 62; i >= 0; i--) {
        palette_add_range(0, 255, i);
        delay_ms(4);
    }
    for (int i = 0; i < 40; i++) {
        anim_tick_update(0);
        wait_bios_ticks(1);
    }
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    g_text_busy = 0;
    camera_scroll_to(11, 12);
    ui_script_exec(89);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    ending_sequence_play();
    for (;;)
        ;   /* 0x25975 原版死循环 */
}

void st05_exit(void)
{
    /* IDA 0x3314B（g_state_exit[5]，2026-09-08 表定案）：最短开场——
     * 战场预设后仅一句旁白 + 聚焦（此前误抄 st01 序列，已重写）。 */
    battle_field_preset();
    g_text_busy = 0;
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st06_exit(void)
{
    /* IDA 0x33169（2026-09-13 全量审计重核；门控当晚 raw disasm 更正）：
     * preset → 旁白0+busy0 → **53AFA 门闸 reload(1)**（0x331A9
     * mov g_spawn_direct,1 / 0x331BA mov g_spawn_direct,0——紧凑轨迹
     * 过滤器漏掉两条 mov，曾误判"裸 reload"删门控致增援散落，已恢复）
     * → 镜头(8,1)/script28 → 镜头(8,0)/script29 → 旁白1 → 聚焦
     * （loc_33140：无 face_reset）。 */
    battle_field_preset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    g_text_busy = 0;
    g_spawn_direct = 1;
    sprites_reload_slot(1);
    g_spawn_direct = 0;
    camera_scroll_to(8, 1);
    ui_script_exec(28);
    camera_scroll_to(8, 0);
    ui_script_exec(29);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st07_exit(void)
{
    /* IDA 0x33219 全解（2026-09-13 全量审计）：preset（旧版缺）→
     * camera(7,32)/script31 → 旁白0（无 busy0）→ camera(7,23)/script32 →
     * 旁白1 → face_reset+聚焦（jmp loc_33028 → loc_3312D 共享尾，旧版
     * 缺尾三步）。 */
    battle_field_preset();
    camera_scroll_to(7, 32);
    ui_script_exec(31);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_scroll_to(7, 23);
    ui_script_exec(32);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    ents_face_reset();
    camera_focus_ent(0);
}

void st08_exit(void)
{
    /* IDA 0x3327D 全解（2026-09-13 全量审计）：preset（旧版缺）→
     * **ent[0..10] 朝向=2（上）**（0x33291 循环，旧版缺）→ camera(6,0) →
     * 旁白0+busy0 → script35 → 旁白1 → 聚焦 → face_reset（注意顺序：
     * 此尾 focus 在前 face_reset 在后，与 loc_3312D 相反）。 */
    battle_field_preset();
    if (g_ent_table)
        for (int i = 0; i < 11 && i < g_ent_count; i++)
            g_ent_table[i][3] = 2;
    camera_scroll_to(6, 0);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(35);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    camera_focus_ent(0);
    ents_face_reset();
}

void st09_exit(void)
{
    /* IDA 0x3332B 全解（2026-09-13 全量审计）：preset（旧版缺）→
     * camera(10,0) → ent50/51[+38]=100（0x33346/0x33354，旧版缺）→
     * 旁白0 → 聚焦（loc_3344D→loc_33206：无 face_reset）。 */
    battle_field_preset();
    camera_scroll_to(10, 0);
    if (g_ent_table && g_ent_count > 51) {
        g_ent_table[50][38] = 100;
        g_ent_table[51][38] = 100;
    }
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st10_exit(void)
{
    /* IDA 0x33367 全解（2026-09-13 全量审计）：preset（旧版缺）→
     * 旁白0+busy0 → camera(10,7) → reload(1) → script38 → 旁白1 →
     * **script39 → 旁白2 → face_reset+聚焦**（jmp loc_3310C 共享尾，
     * 旧版缺尾）。 */
    battle_field_preset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    g_text_busy = 0;
    camera_scroll_to(10, 7);
    sprites_reload_slot(1);
    ui_script_exec(38);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    ui_script_exec(39);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 2, g_pkg_text_evt);
    ents_face_reset();
    camera_focus_ent(0);
}

void st11_exit(void)
{
    /* IDA 0x333F5（2026-09-08 重核）：preset → 镜头(4,4) → 53AFA 门闸
     * reload(1) → script40 → 镜头(11,40) → script41 → 朝向复位 →
     * 共享尾 0x3344D：text(0) + camera_focus_ent(0)。 */
    battle_field_preset();
    camera_scroll_to(4, 4);
    g_spawn_direct = 1;
    sprites_reload_slot(1);
    g_spawn_direct = 0;
    ui_script_exec(40);
    camera_scroll_to(11, 40);
    ui_script_exec(41);
    ents_face_reset();   /* 0x无参（fd2re event.c） */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st12_exit(void)
{
    /* IDA 0x3346B 全解（2026-09-13 全量审计，旧版空壳）：preset →
     * 旁白0 → 聚焦（jmp loc_3344D→loc_33206：无 face_reset）。 */
    battle_field_preset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st13_exit(void)
{
    /* IDA 0x3347C 全解（2026-09-13 全量审计）：preset（旧版缺）→
     * camera(20,20) → 旁白0 → 聚焦（loc_3344D 链）。 */
    battle_field_preset();
    camera_scroll_to(20, 20);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st14_exit(void)
{
    /* IDA 0x334D9 全解（2026-09-14 复核更正）：preset（旧版缺）→
     * 旁白(id) → camera(24,17) → 旁白(id+1)+busy0 → script48 →
     * 旁白(**id+2**，0x33582 add ebx,2——§13.89 表误记 id、旧版照抄
     * 致第三句重复第一句，本轮 raw disasm 更正）→ 聚焦；
     * id = 3*(1-roster_has_unit(12))（有#12→0，无→3）。 */
    int id = 3 * !roster_has_unit(12);

    battle_field_preset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), id, g_pkg_text_evt);
    camera_scroll_to(24, 17);
    text_render_box(1, 19, 74, 205, 320, vram_base(), id + 1, g_pkg_text_evt);
    g_text_busy = 0;
    ui_script_exec(48);
    text_render_box(1, 19, 74, 205, 320, vram_base(), id + 2, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st15_exit(void)
{
    /* IDA 0x335A0（2026-09-13 全量审计重核）：jmp loc_33470 链 =
     * preset → 旁白0（@vram 基址——旧版 str19@+332 为误译）→ 聚焦。 */
    battle_field_preset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st16_exit(void)
{
    /* IDA 0x335AA 全解（2026-09-13 全量审计）：preset →
     * roster_has_unit(18) 为真直接跳尾；否则 reload(1) → 旁白0 →
     * 聚焦（loc_3344D 链；旧版缺尾且内联 preset）。 */
    battle_field_preset();
    if (!roster_has_unit(18))
        sprites_reload_slot(1);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st17_exit(void)
{
    /* IDA 0x335DA 全解（2026-09-13 全量审计）：preset（旧版缺）→
     * 旁白0+busy0 → camera(16,4)/script54 → 旁白1+busy0 →
     * camera(16,4)/script55 → 旁白2 → face_reset+聚焦（jmp loc_3310C
     * 共享尾，旧版缺 script55/旁白2/尾）。 */
    battle_field_preset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    g_text_busy = 0;
    camera_scroll_to(16, 4);
    ui_script_exec(54);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    g_text_busy = 0;
    camera_scroll_to(16, 4);
    ui_script_exec(55);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 2, g_pkg_text_evt);
    ents_face_reset();
    camera_focus_ent(0);
}

void st18_exit(void)
{
    /* IDA 0x33674 = exit[18/19/20] 共享（2026-09-13 全量审计重核）：
     * jmp loc_33470 链 = preset → 旁白0（@基址——旧版 str19@+332 误译）
     * → 聚焦。 */
    battle_field_preset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st19_exit(void)
{
    /* IDA 0x33674 共享尾（同 st18_exit，2026-09-13 重核）。 */
    battle_field_preset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st20_exit(void)
{
    /* IDA 0x33674 共享尾（同 st18_exit，2026-09-13 重核）。 */
    battle_field_preset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st21_exit(void)
{
    /* IDA 0x3367E 全解（2026-09-13 全量审计）：preset（旧版缺）→
     * camera(16,28) → script67（jmp loc_33440 落入 exit[11] 尾段：
     * face_reset 后顺序落入 loc_3344D）→ 旁白0 → 聚焦。 */
    battle_field_preset();
    camera_scroll_to(16, 28);
    ui_script_exec(67);
    ents_face_reset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st22_exit(void)
{
    /* IDA 0x336A0 全解（2026-09-13 全量审计，旧版空壳）：preset →
     * ent_mark_flag1(0..15) → camera(14,32) → cast_beam_fall(vx+6,
     * vy+5,10,8) → HP(+64 word)!=0 的单位 flags(+5)=0 → anim_tick(0)
     * → palette(0,255,0) → delay(500) → 旁白0 → camera(14,29)/
     * script68 → 旁白1 → script69 → 旁白2 → script70 → 旁白3+busy0
     * → camera(14,13) → reload(1) → delay(200) → palette(0,255,0xFF)
     * → delay(100) → anim_tick(0) → palette(0,255,0) → delay(500) →
     * 旁白4 → face_reset → 聚焦（尾 jmp loc_33594）。 */
    battle_field_preset();
    for (int i = 0; i < 16; i++)
        ent_mark_flag1(i);
    camera_scroll_to(14, 32);
    cast_beam_fall(g_cursor_view_x + 6, g_cursor_view_y + 5, 10, 8);
    for (int i = 0; i < 16 && i < g_ent_count; i++) {
        if (*(uint16_t *)&g_ent_table[i][64] != 0) {
            g_ent_table[i][5] = 0;
            g_ent_table[i][3] = 2;      /* 0x33710：存活者面朝上（0x33714） */
        }
    }
    anim_tick_update(0);
    palette_add_range(0, 255, 0);
    delay_ms(500);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    camera_scroll_to(14, 29);
    ui_script_exec(68);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    ui_script_exec(69);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 2, g_pkg_text_evt);
    ui_script_exec(70);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 3, g_pkg_text_evt);
    g_text_busy = 0;
    camera_scroll_to(14, 13);
    sprites_reload_slot(1);
    delay_ms(200);
    palette_add_range(0, 255, 0xFF);
    delay_ms(100);
    anim_tick_update(0);
    palette_add_range(0, 255, 0);
    delay_ms(500);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 4, g_pkg_text_evt);
    ents_face_reset();
    camera_focus_ent(0);
}

void st23_exit(void)
{
    /* IDA 0x338C4 全解（2026-09-13 全量审计）：preset（旧版缺）→
     * 旁白0 → reload(1) → 四角运镜各 delay(400) → 旁白1 → 聚焦
     * （尾 jmp loc_331EA）。 */
    battle_field_preset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    sprites_reload_slot(1);
    camera_scroll_to(0, 4);
    delay_ms(400);
    camera_scroll_to(0, 22);
    delay_ms(400);
    camera_scroll_to(26, 24);
    delay_ms(400);
    camera_scroll_to(26, 2);
    delay_ms(400);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    camera_focus_ent(0);
}

void st24_exit(void)
{
    /* IDA 0x3396A 全解：preset →
     * blk88 以空基址装载（0x33983 push 0 = 先清再载）→ camera(5,0)
     * → 旁白1 → memset(g_shadow_buf2,0,0x25680) →
     * 3×[sfx(1)+quake(20)+delay(600)] →
     * sfx(1)+quake(60) → 旁白2 → 聚焦 → sfx_magic_stop（尾跳）。 */
    battle_field_preset();
    g_magic_sfx_pkg = 0;
    g_magic_sfx_pkg = dat_load_block("FDOTHER.DAT", g_magic_sfx_pkg, 88);   /* 0x33994 */
    camera_scroll_to(5, 0);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    memset(battle_shadow_layer(), 0, 456u * 336u);
    sfx_play(g_magic_sfx_pkg, 1, 1);   /* 0x339EB 族 */
    battle_quake(20);   /* 0x24B4D */
    delay_ms(600);
    sfx_play(g_magic_sfx_pkg, 1, 1);   /* 0x339EB 族 */
    battle_quake(20);   /* 0x24B4D */
    delay_ms(600);
    sfx_play(g_magic_sfx_pkg, 1, 1);   /* 0x339EB 族 */
    battle_quake(20);   /* 0x24B4D */
    delay_ms(600);
    sfx_play(g_magic_sfx_pkg, 1, 1);   /* 0x339EB 族 */
    battle_quake(60);   /* 0x24B4D */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 2, g_pkg_text_evt);
    camera_focus_ent(0);   /* 实参 0（2026-09-06） */
    sfx_magic_stop();   /* 0x1D4F6 */
}

void st25_exit(void)
{
    /* IDA 0x33AAE 全解（2026-09-13 全量审计）：preset（旧版缺）→
     * camera(9,39)/script76 → 旁白0 → face_reset+聚焦（尾 jmp
     * loc_3312D）。 */
    battle_field_preset();
    camera_scroll_to(9, 39);
    ui_script_exec(76);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    ents_face_reset();
    camera_focus_ent(0);
}

void st26_exit(void)
{
    /* IDA 0x33AF1 全解（2026-09-13 全量审计，旧版空壳）：preset →
     * camera(9,49)/script76 → 旁白0 → inventory_has_item(100)!=-1
     * 则旁白3 → 旁白4 → camera(9,49) → beam(vx,vy+3,2,2)+palette →
     * 旁白5 → beam(vx,vy,2,2)+palette → script81 → 旁白6 →
     * beam(vx+2,vy,2,2)+palette → 旁白7 → face_reset+聚焦（尾 jmp
     * loc_3312D）。 */
    battle_field_preset();
    camera_scroll_to(9, 49);
    ui_script_exec(76);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    if (inventory_has_item(100) != -1)
        text_render_box(1, 19, 74, 205, 320, vram_base(), 3, g_pkg_text_evt);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 4, g_pkg_text_evt);
    camera_scroll_to(9, 49);
    cast_beam_fall(g_cursor_view_x, g_cursor_view_y + 3, 2, 2);
    palette_add_range(0, 255, 0);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 5, g_pkg_text_evt);
    cast_beam_fall(g_cursor_view_x, g_cursor_view_y, 2, 2);
    palette_add_range(0, 255, 0);
    ui_script_exec(81);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 6, g_pkg_text_evt);
    cast_beam_fall(g_cursor_view_x + 2, g_cursor_view_y, 2, 2);
    palette_add_range(0, 255, 0);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 7, g_pkg_text_evt);
    ents_face_reset();
    camera_focus_ent(0);
}

void st27_exit(void)
{
    /* IDA 0x33C9D 全解（2026-09-13 全量审计，旧版空壳）：preset →
     * ent_mark_flag1(0..19) → camera(29,15) → cast_beam_fall(vx+6,
     * vy+5,10,8) → HP(+64 word)!=0 的单位 flags(+5)=0 → anim_tick(0)
     * → palette(0,255,0) → delay(500) → script85 ×3 → face_reset →
     * 旁白0+busy0 → cutscene(0,16,6) → cutscene(7,16,7) → busy=1 →
     * 聚焦（尾 jmp loc_33594）。 */
    battle_field_preset();
    for (int i = 0; i < 20; i++)
        ent_mark_flag1(i);
    camera_scroll_to(29, 15);
    cast_beam_fall(g_cursor_view_x + 6, g_cursor_view_y + 5, 10, 8);
    for (int i = 0; i < 20; i++) {
        if (*(uint16_t *)&g_ent_table[i][64] != 0)
            g_ent_table[i][5] = 0;
    }
    anim_tick_update(0);
    palette_add_range(0, 255, 0);
    delay_ms(500);
    ui_script_exec(85);
    ui_script_exec(85);
    ui_script_exec(85);
    ents_face_reset();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    g_text_busy = 0;
    cutscene_transition(0, 16, 6);
    cutscene_transition(7, 16, 7);
    g_text_busy = 1;
    camera_focus_ent(0);
}

void st28_exit(void)
{
    /* IDA 0x33DBA 全解（2026-09-13 全量审计）：preset+busy0（旧版缺）
     * → camera(9,56) → script86 → 旁白7 → cutscene(9,19,8) → 旁白8
     * → face_reset+聚焦（尾 jmp loc_3312D）。 */
    battle_field_preset();
    g_text_busy = 0;
    camera_scroll_to(9, 56);
    ui_script_exec(86);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 7, g_pkg_text_evt);
    cutscene_transition(9, 19, 8);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 8, g_pkg_text_evt);
    ents_face_reset();
    camera_focus_ent(0);
}

void st29_exit(void)
{
    /* IDA 0x33E3C 全解（2026-09-13 全量审计）：preset（旧版缺）→
     * script87 → camera(16,19) → 旁白0+busy0 → camera(16,1) → 四组
     * scroll_and_cutscene(·,·,5) → 旁白1 → flash_pulse → 旁白2+busy0
     * → camera(16,14) → 三组 (·,·,18) → busy=1 → face_reset+聚焦
     * （尾 jmp loc_3313B；flash_pulse 已提炼为独立 helper）。 */
    battle_field_preset();
    ui_script_exec(87);
    camera_scroll_to(16, 19);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 0, g_pkg_text_evt);
    g_text_busy = 0;
    camera_scroll_to(16, 1);
    scroll_and_cutscene(21, 21, 5);   /* 0x33F78 */
    scroll_and_cutscene(22, 23, 5);   /* 0x33F78 */
    scroll_and_cutscene(23, 20, 5);   /* 0x33F78 */
    scroll_and_cutscene(24, 24, 5);   /* 0x33F78 */
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    flash_pulse();
    text_render_box(1, 19, 74, 205, 320, vram_base(), 2, g_pkg_text_evt);
    g_text_busy = 0;
    camera_scroll_to(16, 14);
    scroll_and_cutscene(24, 22, 18);   /* 0x33F78 */
    scroll_and_cutscene(25, 21, 18);   /* 0x33F78 */
    scroll_and_cutscene(26, 23, 18);   /* 0x33F78 */
    g_text_busy = 1;
    ents_face_reset();
    camera_focus_ent(0);
}

void st00_enter(void)
{
    /* IDA 0x22EF6（2026-09-13 重核）：say(9) → sync → state=1。
     * 原版无 busy 写（对白交由场景泵收框；旧版多写 busy0=自动跳过）。 */
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    9, g_pkg_text_evt);
    battle_sync_to_roster();
    g_state = 1;
}

/* IDA 0x205DA: common stage reset used by the prologue and ordinary state
 * exits.  The resource-only stages 31/32 are deliberately accepted here;
 * they are valid FDFIELD/FDTXT scenes despite not having state handlers. */
static void state_stage_prepare(void)
{
    g_text_busy = 0;
    g_state_pending = 0;
    if (battle_stage_load(g_state) != 0) {
        fprintf(stderr, "battle_stage_load failed for scene %d\n", (int)g_state);
        abort();
    }
    memset(g_spawner_flags, 0, sizeof(g_spawner_flags));
    g_scroll_x = 0;
    g_scroll_y = 0;
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_cursor_view_x = 0;
    g_cursor_view_y = 0;
    anim_tick_update(1);
    g_text_busy = 1;
    fade_in();
    g_turn_count = 1;
    kbd_flush();     /* 0x20678 jmp loc_17EE8：共享尾 = call kbd_flush */
}

static void prologue_walk_up(int count)
{
    for (int i = 0; i < count; i++)
        ent_walk_up_one_tile(2);
}

static void prologue_text(int str_id)
{
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    str_id, g_pkg_text_evt);
    g_text_busy = 0;
}

void st00_exit(void)
{
    /* IDA 0x3231B: two resource-only stages (32 -> 31), then the initial
     * state 0 stage.  This is the sequence seen after Start; reducing it to
     * roster_spawn_ent() skips the entire opening and leaves the wrong map. */
    g_state = 32;
    state_stage_prepare();
    camera_scroll_to(3, 34);
    ui_script_exec(99);
    prologue_walk_up(15);
    prologue_text(0);
    prologue_walk_up(13);
    prologue_text(1);

    music_play(-1, 0);
    g_script_fade = 1;
    ui_script_exec(100);
    g_script_fade = 0;

    camera_scroll_to(0, 43);
    music_play(11, 0);
    fade_in();
    ui_script_exec(101);
    prologue_text(2);
    ui_script_exec(102);
    prologue_text(3);
    ui_script_exec(103);
    prologue_text(4);
    ui_script_exec(104);
    prologue_text(5);
    g_script_fade = 1;
    g_text_busy = 0;
    ui_script_exec(105);
    g_script_fade = 0;

    g_state = 31;
    state_stage_prepare();
    g_text_busy = 0;
    camera_scroll_to(5, 42);
    sprites_reload_slot(1);
    ui_script_exec(90);
    prologue_text(0);
    ui_script_exec(91);
    prologue_text(1);
    ui_script_exec(92);
    prologue_text(2);
    sprites_reload_slot(3);
    camera_scroll_to(4, 41);
    prologue_text(3);
    ui_script_exec(93);
    prologue_text(4);
    ent_mark_flag1(2);
    sprites_reload_slot(5);
    prologue_text(5);
    ui_script_exec(94);
    prologue_text(6);
    ui_script_exec(95);
    prologue_text(7);
    ui_script_exec(96);
    prologue_text(8);
    ui_script_exec(97);
    prologue_text(9);

    music_play(-1, 0);
    g_text_busy = 0;
    g_script_fade = 1;
    ui_script_exec(98);
    g_script_fade = 0;

    g_state = 0;
    roster_spawn_ent(0);
    roster_spawn_ent(9);
    roster_spawn_ent(4);
    roster_spawn_ent(30);
    state_stage_prepare();
    g_text_busy = 0;
    camera_scroll_to(4, 12);
    ui_script_exec(0);
    delay_ms(200);
    prologue_text(0);
    delay_ms(200);
    camera_scroll_to(0, 0);
    cutscene_flashback(1);
    ui_script_exec(1);
    camera_scroll_to(0, 15);
    cutscene_flashback(2);
    ui_script_exec(2);
    prologue_text(1);
    delay_ms(200);
    ui_script_exec(5);
    ent_mark_flag1(9);
    anim_tick_update(0);
    delay_ms(100);
    prologue_text(2);
    ents_face_reset();
    camera_focus_ent(0);
    g_gold = 0;
}

void st01_enter(void)
{
    /* IDA 0x22F37 全解（2026-09-13 重导）：扫描 ent5..10 死亡标志——
     * 有任一阵亡 → say(7)；全存活 → say(6)+grant_item(198)（0x22F9A）→
     * camera(14,2)（固定，旧版误条件化）→ reload(4) → delay(100) →
     * script14 → say(8)+busy0 → delay(200) → script15 → say(9)+busy0 →
     * delay(200) → camera(14,1) → delay(200)（旧缺）→ script16 →
     * delay(200)（旧缺）→ say(10)（无 busy0，旧多）→ spawn(8) → sync →
     * state=2。 */
    int any_dead = 0;
    if (g_ent_table) {
        for (int i = 5; i < 11 && i < g_ent_count; i++)
            any_dead |= (g_ent_table[i][5] & 1u) != 0;
    }
    if (any_dead) {
        text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                        7, g_pkg_text_evt);
    } else {
        text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                        6, g_pkg_text_evt);
        grant_item_first_player(198);
    }
    camera_scroll_to(14, 2);
    sprites_reload_slot(4);
    delay_ms(100);
    ui_script_exec(14);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    8, g_pkg_text_evt); g_text_busy = 0;
    delay_ms(200);
    ui_script_exec(15);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    9, g_pkg_text_evt); g_text_busy = 0;
    delay_ms(200);
    camera_scroll_to(14, 1);
    delay_ms(200);
    ui_script_exec(16);
    delay_ms(200);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    10, g_pkg_text_evt);
    roster_spawn_ent(8);
    battle_sync_to_roster();
    g_state = 2;
}

void st01_exit(void)
{
    /* IDA 0x32D18 = g_state_exit[1]（2026-09-08 表定案，仅此状态走本序列；
     * 新游戏流程不可达——st01_enter 尾已置 g_state=2，留作旧档/完备性）：
     * preset → 镜头(13,11)/script9 → 旁白0 → script10 → 旁白1 →
     * reload(1)+anim_tick(0)+script11 → 旁白2 → 镜头(6,12) →
     * 53AFA 门闸 reload(2)（增援直接落位）→ script12 → 旁白3 → 聚焦。 */
    battle_field_preset();
    camera_scroll_to(13, 11);
    ui_script_exec(9);
    delay_ms(50);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    0, g_pkg_text_evt); g_text_busy = 0;
    delay_ms(200);
    ui_script_exec(10);
    delay_ms(200);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    1, g_pkg_text_evt); g_text_busy = 0;
    delay_ms(200);
    sprites_reload_slot(1);
    anim_tick_update(0);
    delay_ms(200);
    ui_script_exec(11);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    2, g_pkg_text_evt); g_text_busy = 0;
    ents_face_reset();
    camera_scroll_to(6, 12);
    g_spawn_direct = 1;
    sprites_reload_slot(2);
    g_spawn_direct = 0;
    ui_script_exec(12);
    ents_face_reset();
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    3, g_pkg_text_evt);
    camera_focus_ent(0);
}

/* IDA 0x230F2 = g_state_enter[2]（胜利相位，2026-09-13 重定案）：
 * sync → ent_flag1_test(6) 分支——6 号单位存活（eax==0，jz 落入）：
 *   battle_field_init(x@0x520BA, y@0x520C1, dir@0x520C8, first=0, last=6,
 *   leader=0, l1..l3=0, cursor/scroll=(2,0))
 *   → str7 → roster_spawn_ent(2)；6 号阵亡：仅 str6（无重绘/无入队）。
 * 两分支均无额外 g_text_busy 写（busy=0 由 battle_field_init 内部
 * 0x2345B 完成；str6 路径维持原值）。
 * 0x23165 处 5 个 push eax 即 first/leader/l1/l2/l3 的 0 实参（eax 此时
 * 恒 0，jz 判定本身依赖它），push 2/push 6 为 cursor_x=2 与 last=6。 */
void st02_enter(void)
{
    static const uint8_t x[7] = { 8, 7, 9, 6, 10, 8, 8 };
    static const uint8_t y[7] = { 3, 3, 3, 2, 2, 4, 1 };
    static const uint8_t dir[7] = { 2, 2, 2, 3, 1, 2, 0 };
    battle_sync_to_roster();
    if (!ent_flag1_test(6)) {
        battle_field_init(x, y, dir, 0, 6, 0, 0, 0, 0, 2, 0);
        text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                        7, g_pkg_text_evt);
        roster_spawn_ent(2);
    } else {
        text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                        6, g_pkg_text_evt);
    }
    g_state++;
}

/* IDA 0x231BC = g_state_enter[3]（胜利相位，2026-09-13 重定案）：
 * 仅 str4 对白（直盖当前 VRAM——原版此相位无任何战场重绘/布阵/入队）
 * → battle_sync_to_roster → 推进。注意 str4 非 str7，且顺序是先文本后
 * sync（与 0x230F2/0x231F9 相反）。 */
void st03_enter(void)
{
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    4, g_pkg_text_evt);
    battle_sync_to_roster();
    g_state++;
}

/* IDA 0x231F9 = g_state_enter[4]（胜利相位，2026-09-13 重定案）：
 * battle_field_init(x@0x520CF, y@0x520D6, dir@0x520DD, first=0, last=6,
 * leader=ent41@(12,8)朝0, cursor/scroll=(6,4)) → str9 →
 * roster_spawn_ent(10) → battle_sync_to_roster → 推进。 */
void st04_enter(void)
{
    static const uint8_t x[7] = { 12, 11, 13, 10, 10, 14, 14 };
    static const uint8_t y[7] = { 11, 11, 11, 9, 10, 9, 10 };
    static const uint8_t dir[7] = { 2, 2, 2, 3, 3, 1, 1 };
    battle_field_init(x, y, dir, 0, 6, 41, 12, 8, 0, 6, 4);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    9, g_pkg_text_evt);
    roster_spawn_ent(10);
    battle_sync_to_roster();
    g_state++;
}
void st02_exit(void)
{
    /* IDA 0x32E8C = g_state_exit[2]（2026-09-08 表定案）：第一章战斗开场。
     * stage2 ctx P=6、standing 组1×9+组2×12——组1 在本序列中段生成
     * （ents 6..14），组2 为战斗中增援。script18=我方 0..5 转向上；
     * script17=敌首 ent6 前出 6 步+转身；script19=敌兵 7..14 各南向
     * 行军 5 步（敌人出场行军，本章"少了的动画"即此段错播）。 */
    battle_field_preset();
    camera_scroll_to(3, 17);
    delay_ms(200);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    0, g_pkg_text_evt); g_text_busy = 0;
    ui_script_exec(18);
    sprites_reload_slot(1);
    camera_scroll_to(3, 6);
    delay_ms(200);
    ui_script_exec(17);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    1, g_pkg_text_evt); g_text_busy = 0;
    ui_script_exec(19);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    2, g_pkg_text_evt); g_text_busy = 0;
    camera_scroll_to(3, 17);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    3, g_pkg_text_evt);
    ents_face_reset();
    camera_focus_ent(0);
    /* 0x3313B 共享尾：face_reset+focus(0)+retn——无 anim_tick（2026-09-13
     * 全量审计定案，旧版多一帧 tick 已删）。 */
}

void st03_exit(void)
{
    /* IDA 0x32FB2 = g_state_exit[3]：preset → 镜头(4,11)/script20 →
     * 旁白0 → reload(1) → 镜头(4,0) → delay200 → 旁白1 → 复位 → 聚焦。 */
    battle_field_preset();
    camera_scroll_to(4, 11);
    ui_script_exec(20);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    0, g_pkg_text_evt); g_text_busy = 0;
    sprites_reload_slot(1);
    camera_scroll_to(4, 0);
    delay_ms(200);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    1, g_pkg_text_evt);
    ents_face_reset();
    camera_focus_ent(0);
}

void st04_exit(void)
{
    /* IDA 0x33049 = g_state_exit[4]：preset → 旁白0 → 镜头(3,3) →
     * delay200 → reload(1)+anim_tick(0)+delay200 → script22 → 旁白1 →
     * 镜头(8,14)/script21 → 旁白2 → 复位 → 聚焦。 */
    battle_field_preset();
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    0, g_pkg_text_evt); g_text_busy = 0;
    camera_scroll_to(3, 3);
    delay_ms(200);
    sprites_reload_slot(1);
    anim_tick_update(0);
    delay_ms(200);
    ui_script_exec(22);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    1, g_pkg_text_evt); g_text_busy = 0;
    camera_scroll_to(8, 14);
    ui_script_exec(21);
    text_render_box(1, 19, 74, 205, FD2_SCREEN_W, vram_base(),
                    2, g_pkg_text_evt);
    ents_face_reset();
    camera_focus_ent(0);
}

static void state_post_default(int unit)
{
    battle_end_check();
}

/* ---- g_state_post_action 专属处理器（表@0x51B19 实 dump 30 项，
 * 2026-09-13 逐指令反编译定案；此前 30 项全接 default 为已知缺口）----
 * 默认项 = 0x205B4 battle_end_check（team0 敌全灭→pending=2 胜；
 * ent0 亡→pending=1）。专属项多数先经 loc_205BE 重入默认核再叠加
 * 本章败北/胜利条件；17/22/28/29 原版不跑默认核（纯条件判定，
 * pending 由 battle_field_preset 0x205DA 清零后保持）。
 * 事件对白 = text_render_box(1,19,74,205,320,0xA0000,str,g_pkg_text_evt)
 * （与 event.c say() 同式，g_text_busy 由 anim_pump 轮询推进）。 */
static void post_say(int str_id)
{
    text_render_box(1, 19, 74, 205, 320, vram_base(), str_id, g_pkg_text_evt);
}

/* 0x206C5 [1]：实体 5..10 全亡 → pending=1 */
static void post01(int unit)
{
    int i;

    battle_end_check();
    for (i = 5; i < 11; i++)
        if (!ent_flag1_test(i))
            return;
    g_state_pending = 1;
}

/* 0x20707 [9]：ent50 ∥ ent51 亡 → pending=1（st09_exit 设 HP=100 的护卫） */
static void post09(int unit)
{
    battle_end_check();
    if (ent_flag1_test(50) || ent_flag1_test(51))
        g_state_pending = 1;
}

/* 0x2073D [11]：ent14 亡 → pending=1 */
static void post11(int unit)
{
    battle_end_check();
    if (ent_flag1_test(14))
        g_state_pending = 1;
}

/* 0x20765 [12]：实体 15..26 全亡 → pending=1+str10；
 * turn>5 且 ent59 亡 → pending=1+str2 */
static void post12(int unit)
{
    int i;
    uint8_t any_alive = 0;

    battle_end_check();
    for (i = 15; i < 27; i++)
        if (!ent_flag1_test(i))
            any_alive = 1;
    if (!any_alive) {
        g_state_pending = 1;
        post_say(10);
    }
    if (g_turn_count > 5 && ent_flag1_test(59)) {
        g_state_pending = 1;
        post_say(2);
    }
}

/* 0x20822 [14]：ent64 亡 → pending=1 */
static void post14(int unit)
{
    battle_end_check();
    if (ent_flag1_test(64))
        g_state_pending = 1;
}

/* 0x2084A [15]：ent65 亡 → pending=1 */
static void post15(int unit)
{
    battle_end_check();
    if (ent_flag1_test(65))
        g_state_pending = 1;
}

/* 0x20872 [16]：roster 不含单位 18 且 ent52 亡 → str2+pending=1 */
static void post16(int unit)
{
    battle_end_check();
    if (!roster_has_unit(18) && ent_flag1_test(52)) {
        post_say(2);
        g_state_pending = 1;
    }
}

/* 0x208CF [17]：无默认核。ent0/16/17 亡 → pending=1；ent52 亡 → pending=2 胜 */
static void post17(int unit)
{
    if (ent_flag1_test(0) || ent_flag1_test(16) || ent_flag1_test(17))
        g_state_pending = 1;
    if (ent_flag1_test(52))
        g_state_pending = 2;
}

/* 0x20926 [18]：默认核 + turn>6 且 ent64 亡 → pending=1 */
static void post18(int unit)
{
    battle_end_check();
    if (g_turn_count > 6 && ent_flag1_test(64))
        g_state_pending = 1;
}

/* 0x20957 [19]：实体 53..60 全亡 → pending=1+str10；ent0/52 亡 →
 * pending=1；实体 36..51 与 61..82 全亡 → pending=2 胜 */
static void post19(int unit)
{
    int i;
    uint8_t any_alive = 0, any_enemy = 0;

    battle_end_check();
    for (i = 53; i < 61; i++)
        if (!ent_flag1_test(i))
            any_alive = 1;
    if (!any_alive) {
        g_state_pending = 1;
        post_say(10);
    }
    if (ent_flag1_test(0) || ent_flag1_test(52))
        g_state_pending = 1;
    for (i = 36; i < 52; i++)
        if (!ent_flag1_test(i))
            any_enemy = 1;
    for (i = 61; i < 83; i++)
        if (!ent_flag1_test(i))
            any_enemy = 1;
    if (!any_enemy)
        g_state_pending = 2;
}

/* 0x20A51 [20]：ent16 ∥ ent17 亡 → pending=1 */
static void post20(int unit)
{
    battle_end_check();
    if (ent_flag1_test(16) || ent_flag1_test(17))
        g_state_pending = 1;
}

/* 0x20A87 [21/26/27]：ent1 亡 → pending=1 */
static void post21(int unit)
{
    battle_end_check();
    if (ent_flag1_test(1))
        g_state_pending = 1;
}

/* 0x20AAF [22]：无默认核。ent0/1/16/17 亡 → pending=1；ent18 亡 →
 * pending=2 胜（战果汇总后转 st23 编队态） */
static void post22(int unit)
{
    if (ent_flag1_test(0) || ent_flag1_test(1)
        || ent_flag1_test(16) || ent_flag1_test(17))
        g_state_pending = 1;
    if (ent_flag1_test(18))
        g_state_pending = 2;
}

/* 0x20B14 [24]：ent16 亡 → pending=1 */
static void post24(int unit)
{
    battle_end_check();
    if (ent_flag1_test(16))
        g_state_pending = 1;
}

/* 0x20B3C [25]：ent1 ∥ ent2 亡 → pending=1 */
static void post25(int unit)
{
    battle_end_check();
    if (ent_flag1_test(1) || ent_flag1_test(2))
        g_state_pending = 1;
}

/* 0x20B72 [28]：无默认核。spawner[18..20] 全开 → pending=2 胜
 * （三章门全开类目标）；ent0 亡 → pending=1；ent1 亡 → str9+pending=1 */
static void post28(int unit)
{
    if (g_spawner_flags[18] && g_spawner_flags[19] && g_spawner_flags[20])
        g_state_pending = 2;
    if (ent_flag1_test(0))
        g_state_pending = 1;
    if (ent_flag1_test(1)) {
        post_say(9);
        g_state_pending = 1;
    }
}

/* 0x20BF5 [29]（终局战）：无默认核。ent20（大魔王）亡 → pending=2 胜；
 * ent0 亡 → pending=1；ent1 亡 → str7+pending=1 */
static void post29(int unit)
{
    if (ent_flag1_test(20))
        g_state_pending = 2;
    if (ent_flag1_test(0))
        g_state_pending = 1;
    if (ent_flag1_test(1)) {
        post_say(7);
        g_state_pending = 1;
    }
}

void states_init(void)
{
    void (*enter[FD2_STATE_COUNT])(void) = {
        st00_enter, st01_enter, st02_enter, st03_enter, st04_enter,
        st05_enter, st06_enter, st07_enter, st08_enter, st09_enter,
        st10_enter, st11_enter, st12_enter, st13_enter, st14_enter,
        st15_enter, st16_enter, st17_enter, st18_enter, st19_enter,
        st20_enter, st21_enter, st22_enter, st23_enter, st24_enter,
        st25_enter, st26_enter, st27_enter, st28_enter, st29_enter,
    };
    void (*exit_[FD2_STATE_COUNT])(void) = {
        st00_exit, st01_exit, st02_exit, st03_exit, st04_exit,
        st05_exit, st06_exit, st07_exit, st08_exit, st09_exit,
        st10_exit, st11_exit, st12_exit, st13_exit, st14_exit,
        st15_exit, st16_exit, st17_exit, st18_exit, st19_exit,
        st20_exit, st21_exit, st22_exit, st23_exit, st24_exit,
        st25_exit, st26_exit, st27_exit, st28_exit, st29_exit,
    };
    void (*post[FD2_STATE_COUNT])(int) = {
        state_post_default, post01,             state_post_default,
        state_post_default, state_post_default, state_post_default,
        state_post_default, state_post_default, state_post_default,
        post09,             state_post_default, post11,
        post12,             state_post_default, post14,
        post15,             post16,             post17,
        post18,             post19,             post20,
        post21,             post22,             state_post_default,
        post24,             post25,             post21,
        post21,             post28,             post29,
    };
    for (int i = 0; i < FD2_STATE_COUNT; i++) {
        g_state_enter[i] = enter[i];
        g_state_exit[i] = exit_[i];
        g_state_post_action[i] = post[i];
    }
    memcpy(g_state_bgm, (const uint8_t[]){
        0x13,0x13,0x13,0x13,0x03,0x13,0x13,0x13,0x03,0x04,
        0x13,0x13,0x13,0x13,0x03,0x13,0x04,0x13,0x13,0x03,
        0x13,0x03,0x04,0x13,0x03,0x13,0x04,0x13,0x13,0x08
    }, FD2_STATE_COUNT);
    /* byte_51E81@0x51E81（get_bytes 2026-09-08 与 0x51E63 首表同段 dump）：
     * 敌方相 BGM。battle_turn_end 两处 music 检查 bgm[s]!=alt[s] 才先
     * music_play(-1) 停曲，切相后无条件 play 对侧表。 */
    memcpy(g_state_bgm_alt, (const uint8_t[]){
        0x0C,0x0C,0x01,0x0C,0x06,0x0C,0x0C,0x01,0x06,0x04,
        0x0C,0x01,0x0C,0x01,0x06,0x0C,0x08,0x0C,0x0C,0x06,
        0x0C,0x06,0x08,0x0C,0x06,0x01,0x08,0x01,0x01,0x08
    }, FD2_STATE_COUNT);
    memcpy(g_state_roster_menu, (const uint8_t[]){
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        1,1,1,0,0,1,1,1
    }, FD2_STATE_COUNT);
}
