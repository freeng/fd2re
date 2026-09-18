/* event.c — 事件/剧情脚本子系统（90 项 g_event_script_table，0x34531-0x3644E）
 *
 * 2026-09-04 全量逆向（IDA 反编译 + 调用图核对）：
 *  - 表 @0x51B91（原误称 g_action_post_hook）90 项互不相同；
 *    0x35B78(cutscene_transition)、0x35F10(ents_kill_from)、0x35F78(nullsub_6)
 *    是被处理器共享的原语/空桩，**不在表内**；
 *  - 触发源：地块事件 tile_event_check / 战斗结果 type-2 延时钩子 /
 *    菜单与敌我回合脚本事件（见 event.h）；
 *  - 处理器全部是"原语序列"（镜头卷动→换立绘→走位脚本→对白→群体开关），
 *    无解释器循环；共享底座即本文件上半部的原语 + script.c 的行走播放器。
 *
 * 本文件：原语实现 +
 * 90 个处理器骨架（每项头部注释 = 反编译证据摘要，地址为 IDA 静态地址）。
 *
 * 2026-09-08 共享尾段全量审计（docs/architecture.md §13.75）：WATCOM
 * 尾合并使 Hex-Rays 将尾跳折成 JUMPOUT 并吞掉前置压栈——35 处处理器
 * 已按逐函数反汇编核对补全/更正（头注只记原版语义；尾跳目标、压栈
 * 与共享尾簇证据表统一见 §13.75）。
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "event.h"
#include "script.h"
#include "entities.h"
#include "video.h"
#include "text.h"
#include "audio.h"
#include "input.h"
#include "timer.h"
#include "battle.h"
#include "magic.h"
#include "resource.h"
#include "states.h"
#include "host.h"

/* ---- 本模块引用的未定名外部符号（待逐一定名后迁入各头） ------------- */
extern void   *g_pkg_text_evt;           /* 0x53A79 事件对白文本包（FDTXT 相关） */
extern int32_t g_disp_num_a;             /* 0x53AD9 战果显示数 */
extern int     sub_1B8A6(int);           /* 0x1B8A6：单位已占用物品槽数量 */

/* 0x13512 —— 将实体标为已行动/演出完成（+5 bit7）。 */
void sub_13512(int arg)
{
    if (arg >= 0 && arg < g_ent_count)
        g_ent_table[arg][5] |= 0x80;
}

/* 0x361B0 —— 调色板渐变闪烁（i/j 两层 palette_add_range(0,255,x)+delay(8)，
 * 间以 delay(400)；事件[76] 雷光/魔法光效果，仅该事件引用）。 */
void sub_361B0(int arg)
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

/* 456 宽影子缓冲窗格目的地址（原字面量）：事件对白用 696099 / 693535 */
#define SAY_DST_SHOP   (vram_base() + 40739u)   /* 0xA9E63：商店族对白基 */
#define SAY_DST_PANEL  (vram_base() + 38175u)   /* 0xA951F：底窗对白基 */

/* =====================================================================
 * 分发（anim_pump 尾部 0x11994-0x119A6）
 * ===================================================================== */
void event_script_dispatch(int unit)
{
    if (g_pending_event_id != 255)
        g_event_script_table[g_pending_event_id](unit);
    g_pending_event_id = 255;
}

/* =====================================================================
 * 共享原语
 * ===================================================================== */

/* 0x13A44 —— 地块事件探测（ai_unit_act 每步调用）
 * 2026-09-07 修正（用户实测：踏上宝箱格误触增援脚本）：原版 0x13A6B
 * `movzx eax,[esp+2]` 取的是 tile_lookup 的 **info[2] = cell[2]&0x1F**
 * （每格持久 5 位"地块事件槽号"，0=无；field_reset_candidates 的
 * cell[2]&=0x1F 即保留此域），0x13A64 门是 **info[4]（shape 属性
 * byte0）&0x60**。旧实现误用 info[0..1] 的 tile 形状索引（0..1023）当
 * 槽号——ctx+51+2*(tile-1) 直读槽区（[51..82] 16 槽）之外的垃圾字节，
 * 宝箱格因此派发了无关脚本 id（增援）。 */
void tile_event_check(int x, int y, int mode)
{
    uint8_t info[8];
    tile_lookup(x, y, info);
    if ((info[4] & 0x60) == 0 && info[2] != 0) {
        /* 附表：g_battle_ctx + 2*(slot-1) 偏移 +51=event_id, +52=触发模式 */
        const uint8_t *slot = g_battle_ctx + 2 * (info[2] - 1);
        uint8_t event_id = slot[51];
        if (event_id != 255 && slot[52] == mode)
            g_pending_event_id = event_id;
    }
}

/* 0x1A813（2026-09-05 定案）：回合事件扫描。ctx[3..50] = 16×3B 槽
 * (u8 回合, u8 事件脚本 id, u8 相位)；g_turn_count 匹配且相位相等时
 * 派发 g_event_script_table[id]。battle_turn_end 以相位 1（0x1A4C7，
 * 友军 NPC 相前）/0（0x1A554，敌方相前）/2（0x1A78D，回合收尾）三处
 * 调用（2026-09-13 raw disasm 勘误：旧注 0/1 两相标签互换，battle.c
 * 调用序 scan(1)→ally_npc、scan(0)→enemy 一直与原版一致）；
 * 资产实证：30 关 ctx 全为此布局（st07 回合 2..7 每回合派发脚本
 * 0x1B、回合 0xF 派发 0x1C 等）。 */
void battle_turn_event_scan(int phase)
{
    if (!g_battle_ctx)
        return;
    for (int i = 0; i < 16; i++) {
        const uint8_t *rec = g_battle_ctx + 3 * i;
        if (rec[3] == g_turn_count && rec[5] == phase)
            g_event_script_table[rec[4]](0);
    }
}

/* 0x135DD —— 镜头逐格卷动（每步 anim_tick_update + kbd_flush） */
void camera_scroll_to(int x, int y)
{
    /* IDA 0x135DD clears the dialogue animation gate before moving. */
    g_text_busy = 0;
    while (x != (int)g_scroll_x) {
        if (x >= (int)g_scroll_x) { g_cursor_x++; g_scroll_x++; }
        else                      { g_cursor_x--; g_scroll_x--; }
        anim_tick_update(0);
        kbd_flush();
    }
    while (y != (int)g_scroll_y) {
        if (y >= (int)g_scroll_y) { g_cursor_y++; g_scroll_y++; }
        else                      { g_cursor_y--; g_scroll_y--; }
        anim_tick_update(0);
        kbd_flush();
    }
    /* 原版尾：0x13637 jz loc_13181 直跳共享 epilogue
     * （pop edi/esi/ebx; retn，2026-09-08 反汇编取证）——纯寄存器弹出，
     * 无光标钳制、无任何内存写；g_text_busy 仅入口 0x135F2 清 0，
     * 退出不恢复。 */
}

/* 0x134E4 —— 全体朝向复位 + delay(20) */
void ents_face_reset(void)
{
    for (int i = 0; i < g_ent_count; i++)
        g_ent_table[i][3] = 0;
    delay_ms(20);
}

/* 0x344F2 —— 区间写实体 +52 低半字节 */
void ents_misc_set_range(int first, int last, int val)
{
    for (int i = first; i <= last; i++)
        g_ent_table[i][52] = (uint8_t)(val | (g_ent_table[i][52] & 0xF0));
}

/* 0x34894 / 0x32975 —— +5 bit0 查询/置位 */
int  ent_flag1_test(int unit)  { return g_ent_table[unit][5] & 1; }
void ent_mark_flag1(int unit)  { g_ent_table[unit][5] = 1; }

/* 0x1D79C —— 学习魔法：ent[+26..+31] 位图（48 位）置位。
 * exp_levelup_check 升级习得时调用（docs §15.7）。 */
void spell_learn_add(int unit, int spell_id)
{
    g_ent_table[unit][26 + spell_id / 8] |= (uint8_t)(1u << (spell_id % 8));
}

/* 0x35F10 —— 从 from 起清场（HP=0 → 死亡动画） */
void ents_kill_from(int from)
{
    for (int i = from; i < g_ent_count; i++)
        *(uint16_t *)&g_ent_table[i][0x40] = 0;
    battle_death_anim();
}

/* 0x35B78 —— 闪白转场：卷动 + 换立绘 + 白闪 + 复原 */
void cutscene_transition(int x, int y, int slot)
{
    camera_scroll_to(x, y);
    sprites_reload_slot(slot);
    delay_ms(300);
    palette_add_range(0, 255, 255);        /* 全白 */
    delay_ms(200);
    palette_add_range(0, 255, 0);          /* 复原 */
    anim_tick_update(0);
    delay_ms(400);
}

/* 0x10B4E sprites_reload_slot —— 真实现移至 battle.c（2026-09-05，
 * 分组键语义 = 记录[0]==key 反汇编定案；本文件 cutscene_transition 仍调用）。 */

/* 0x32999 —— flashback animation.  Block 9 is a normal offset-table frame
 * package.  The animation is composited onto a 456px shadow surface, rather
 * than directly onto the 320px VGA crop: its x origin intentionally begins
 * one tile left of the viewport and its y origin six pixels above it. */
#define FLASHBACK_PITCH       456
#define FLASHBACK_ROWS        336
#define FLASHBACK_RENDER_OFF  32904u
#define FLASHBACK_VGA_OFF     1284u

static void flashback_stamp_new_entities(uint8_t *shadow, const uint8_t *frames,
                                         int frame, int first_entity)
{
    for (int unit = first_entity; unit < g_ent_count; unit++) {
        const uint8_t *ent = g_ent_table[unit];
        int ex = (int)ent[0] - (int)g_scroll_x;
        int ey = (int)ent[1] - (int)g_scroll_y;
        int x = 24 * (ex - 1);
        int y = 24 * ey - 6;
        uint8_t *dst;
        void *work;

        /* 0x32A6C 视口裁剪用有符号比较：x∈[scroll-1, view+scroll]、
         * y∈[scroll, view+scroll+1]。fd2re 的 g_scroll_x/y 是 uint32，
         * 直接写 ent[0] < g_scroll_x-1 在 scroll_x=0 时无符号回绕成
         * 0xFFFFFFFF → 左界恒真、全部 SKIP（stage0 开场两批镜头
         * (0,0)/(0,15) 与 evt02 (0,16) 全中招，水花整体吞掉）。
         * 先减成相对坐标再比较有符号界。 */
        if (ex < -1 || ex > g_view_w || ey < 0 || ey > g_view_h + 1)
            continue;
        /* 0x32A86/0x32AB5 原版无越界检查：贴帧 y=24*dy-6 在顶行实体处为
         * -6、x=24*(dx-1) 在最左列实体处为 -24（帧缘探出 312x192 裁剪
         * 窗，456 带内合法直写）。视口检查通过后 x∈[-24,288]、
         * y∈[-6,210]、行数≤234<336，写入恒在带内——此前宿主侧
         * x<0||y<0 守卫把顶行/左列实体的整套水花静默吞掉（stage0 开场
         * ent7@(6,0)、evt02 flashback(5) ent@(0,21) 实测命中），已撤。 */
        /* y/x 可为 -6/-24：先按有符号算总偏移（恒 ≥0 且在带内），再取址。 */
        long band_off = (long)FLASHBACK_RENDER_OFF
                      + (long)y * FLASHBACK_PITCH + x;
        dst = shadow + band_off;
        work = pkg_frame_blit(frames, dst, FLASHBACK_PITCH, 0, 0, frame);
        if (!work) {
            fprintf(stderr, "flashback: invalid FDOTHER block 9 frame %d\n", frame);
            abort();
        }
        /* 原版 0x15E71 全调用点清单（xref）无 flashback 族——帧留在
         * 影面不恢复；宿主仅回收工作区。 */
        pkg_frame_discard(work);
    }
}

/* 0x32999 has three distinct backing-surface rebuilds after frames 6..8.
 * They are intentionally not folded into the normal animator: frame 6
 * keeps the old terrain and shifts newly spawned entities 8 rows up; frame
 * 7 redraws terrain then shifts only those entities 5 rows up; frame 8 is
 * the first full normal redraw. */
static void flashback_rebuild_surface(uint8_t *shadow, int frame,
                                      int old_count)
{
    if (frame == 6) {
        for (int i = 0; i < old_count; i++)
            if (!(g_ent_table[i][5] & 1u))
                field_render_entity_offset(shadow, i, 0);
        for (int i = old_count; i < g_ent_count; i++)
            if (!(g_ent_table[i][5] & 1u))
                field_render_entity_offset(shadow, i, -8);
        field_render_entity_shadows(shadow);
    } else if (frame == 7) {
        field_backdrop_render(shadow, (int)g_scroll_x,
                              (int)g_scroll_y);   /* 0x32999 帧7/8 经 field_tile_render */
        field_render_viewport(shadow + FLASHBACK_RENDER_OFF, FLASHBACK_PITCH);
        for (int i = 0; i < old_count; i++)
            if (!(g_ent_table[i][5] & 1u))
                field_render_entity_offset(shadow, i, 0);
        for (int i = old_count; i < g_ent_count; i++)
            if (!(g_ent_table[i][5] & 1u))
                field_render_entity_offset(shadow, i, -5);
        field_render_entity_shadows(shadow);
    } else if (frame == 8) {
        field_backdrop_render(shadow, (int)g_scroll_x, (int)g_scroll_y);
        field_render_viewport(shadow + FLASHBACK_RENDER_OFF, FLASHBACK_PITCH);
        field_render_entities(shadow);
    }
}

void cutscene_flashback(int slot)
{
    void *scene = dat_load_block("FDOTHER.DAT", NULL, 95);
    void *frames = dat_load_block("FDOTHER.DAT", NULL, 9);
    uint8_t *saved = NULL;
    uint8_t *shadow = NULL;
    int old_count;

    if (!scene || !frames)
        dat_load_fatal("FDOTHER.DAT", !scene ? 95 : 9);
    saved = malloc(FLASHBACK_PITCH * FLASHBACK_ROWS);
    shadow = malloc(FLASHBACK_PITCH * FLASHBACK_ROWS);
    if (!saved || !shadow) {
        free(shadow);
        free(saved);
        free(frames);
        free(scene);
        dat_load_fatal("FDOTHER.DAT", 9);
    }

    if (field_shadow_snapshot(saved, FLASHBACK_PITCH * FLASHBACK_ROWS) != 0) {
        fprintf(stderr, "flashback: no completed field shadow surface\n");
        abort();
    }
    old_count = g_ent_count;
    sprites_reload_slot(slot);
    for (int frame = 0; frame < 12; frame++) {
        if (frame == 1)
            sfx_play(scene, 0, 1);
        memcpy(shadow, saved, FLASHBACK_PITCH * FLASHBACK_ROWS);
        flashback_stamp_new_entities(shadow, frames, frame, old_count);
        blit_rows(vram_base() + FLASHBACK_VGA_OFF, FD2_SCREEN_W,
                  shadow + FLASHBACK_RENDER_OFF, FLASHBACK_PITCH, 312, 192);

        if (frame >= 6 && frame <= 8) {
            /* 0x32B08：重建前先 memmove(shadow, v15) 从干净基底出发，
             * 再叠实体——回闪帧图像不得残留进 saved。 */
            memcpy(shadow, saved, FLASHBACK_PITCH * FLASHBACK_ROWS);
            flashback_rebuild_surface(shadow, frame, old_count);
            memcpy(saved, shadow, FLASHBACK_PITCH * FLASHBACK_ROWS);
        }
        kbd_flush();
        wait_bios_ticks(1);
    }
    free(shadow);
    free(saved);
    free(frames);
    free(scene);
}

static void put_u16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

void roster_spawn_ent(int unit_id)
{
    /* IDA 0x112A5：enemy_template_entry@0x4E838（24B@0x61DA1+24*id）+
     * growth_table_entry@0x4E821（11B@0x620A1+11*id）→ 初始化下一条 80B
     * roster 记录，收尾 ent_stats_apply_equipment(0x1145A) 叠装备加成。
     * 2026-09-05 换 tables.c 全量表（32 模板），修正旧 9 行内嵌表 2 处转录
     * 笔误（id0[18] 255→0、id27[7] 9→7）。
     * 公式（反汇编）：HP/MP = 基值+(lv-1)×growth[6]/[8]；ATK/DEF/+62 =
     * lv×growth[0]/[2]/[4]+模板基值；ent[+62] 同时是 HIT 与 EVA 的基数。
     * 原版 __chkstk 探针（sub_3702F）不清零记录、等级无 0 保护（全 32 模板
     * lv≥1，实测 1..25）；fd2re 保持 memset 0 以求可复现。 */
    if (unit_id < 0 || unit_id >= 32)
        return;
    if (!g_roster_table || g_roster_count < 0 || g_roster_count >= FD2_ROSTER_MAX)
        return;
    const uint8_t *d = enemy_template_entry(unit_id);
    const uint8_t *gr = growth_table_entry(unit_id);
    uint8_t *r = g_roster_table[g_roster_count];
    unsigned level = d[2];
    unsigned hp = get_u16(d + 3) + (level - 1u) * gr[6];
    unsigned mp = get_u16(d + 5) + (level - 1u) * gr[8];

    memset(r, 0, FD2_ENT_REC_SIZE);
    r[5] = 0; r[6] = 2; r[7] = (uint8_t)unit_id; r[8] = (uint8_t)unit_id;
    r[9] = 0; r[10] = 64; r[12] = 64;
    r[11] = d[12]; r[13] = d[13];
    for (int i = 0; i < 4; i++) {
        r[14 + 2*i] = d[14 + i] == 255 ? 0x80 : 0;
        r[15 + 2*i] = d[14 + i];
    }
    r[22] = 0x80; r[24] = 0x80;
    memcpy(r + 26, d + 8, 4);
    r[30] = 0; r[31] = d[0]; r[32] = d[1]; r[33] = (uint8_t)level;
    r[49] = 255; r[59] = d[7]; r[60] = 0;
    put_u16(r + 55, level * gr[0] + get_u16(d + 18));
    put_u16(r + 57, level * gr[2] + get_u16(d + 20));
    put_u16(r + 62, level * gr[4] + get_u16(d + 22));
    put_u16(r + 64, hp); put_u16(r + 66, hp);
    put_u16(r + 68, mp); put_u16(r + 70, mp);
    ent_stats_apply_equipment(g_roster_count);
    g_roster_count++;
}

/* 0x12263 —— 扫描整张地图，刷新增援刷新点占用计数。
 *
 * IDA 的 dword_53A51 是 FDFIELD 运行时复制：
 *   cell = map + 4 + 4*(x + width*y)
 *   tile_lookup 返回 tile 以及 FDSHAP 属性；
 *   属性 **byte0（info[4]）** & 0x60 == 0x20 表示增援刷新点/村庄（0x122A2
 *   反汇编定案；2026-09-07 修正——旧实现误测 info[5] 且按 tile 形状号
 *   索引 g_spawner_flags，实际两者都按 info[2] 槽号，同 tile_event_check/
 *   treasure_interact 一族）。若 g_spawner_flags[slot] 已置位，原程序将该
 *   cell 的 tile word 加一（宝箱/据点换开启动画帧）并清其 byte[2]。 */
void map_spawn_count_refresh(void)
{
    if (!g_field_map || g_field_w <= 0 || g_field_h <= 0)
        return;

    uint8_t info[8];
    uint8_t *map = (uint8_t *)g_field_map;
    for (int y = 0; y < g_field_h; y++) {
        for (int x = 0; x < g_field_w; x++) {
            tile_lookup(x, y, info);
            if ((info[4] & 0x60u) != 0x20u)
                continue;

            int slot = info[2];
            if (slot >= (int)sizeof(g_spawner_flags) || !g_spawner_flags[slot])
                continue;

            uint8_t *cell = map + 4u + 4u * (size_t)(x + g_field_w * y);
            uint16_t count = (uint16_t)(cell[0] | ((uint16_t)cell[1] << 8));
            count++;
            cell[0] = (uint8_t)count;
            cell[1] = (uint8_t)(count >> 8);
            cell[2] = 0;
        }
    }
}

/* 事件对白原语：只调用原版 text_render_box。
 * g_text_busy 不是对白渲染的隐含副作用；原版只在少数调用点显式写它，
 * 必须由各处理器按对应地址补写，不能由公共 say() 统一清零。 */
static void say(int str_id)
{
    text_render_box(1, 19, 74, 205, 320, vram_base(), str_id, g_pkg_text_evt);
}

/* =====================================================================
 * 90 个事件脚本处理器（表序，地址 = IDA 静态地址）
 * ===================================================================== */

/* [00] 0x34531：增援入队+两段走位：spawn(1)→立绘(3)→镜头(5,8)→script7→
 * 对白11→立绘(7)→script8→对白3→朝向复位 */
static void evt00(int unit)
{
    roster_spawn_ent(1);
    sprites_reload_slot(3);
    camera_scroll_to(5, 8);
    anim_tick_update(1);
    delay_ms(100);
    ui_script_exec(7);
    kbd_flush(); say(11);
    g_text_busy = 0;       /* IDA 0x345A6：仅首句后清模式字节 */
    sprites_reload_slot(7);
    anim_tick_update(1);
    delay_ms(100);
    ui_script_exec(8);
    kbd_flush(); say(3);
    ents_face_reset();
}

/* [01] 0x3460B：镜头(11,16)→回放(4)→script3→对白4 */
static void evt01(int unit)
{
    camera_scroll_to(11, 16);
    cutscene_flashback(4);
    kbd_flush();
    anim_tick_update(1);
    ui_script_exec(3);
    ents_face_reset();
    say(4);
}

/* [02] 0x34673：镜头(0,16)→回放(5)→script4→对白5 */
static void evt02(int unit)
{
    camera_scroll_to(0, 16);
    cutscene_flashback(5);
    kbd_flush();
    anim_tick_update(1);
    ui_script_exec(4);
    ents_face_reset();
    say(5);
}

/* [03] 0x346CD：镜头(11,11)→门闸 reload(6)（我方第二波援军 4 名 NPC
 * 直接落位）→script6→对白6 */
static void evt03(int unit)
{
    camera_scroll_to(11, 11);
    g_spawn_direct = 1;
    sprites_reload_slot(6);
    g_spawn_direct = 0;
    anim_tick_update(1);
    ui_script_exec(6);
    ents_face_reset();
    kbd_flush();
    say(6);
}

/* [04] 0x34738：ent13.type(+6)=1（转友军 NPC）+对白7 */
static void evt04(int unit)
{
    g_ent_table[13][6] = 1;
    say(7);
}

/* [05] 0x350BE：立绘(1)+对白1 */
static void evt05(int unit)
{
    sprites_reload_slot(1);
    say(1);
}

/* [06] 0x34778：镜头(9,1)→53AFA 门闸 reload(3)→script13→对白4；ent5..10 设
 * spawn type=26/class=15（群体变身=援军登场） */
static void evt06(int unit)
{
    camera_scroll_to(9, 1);
    delay_ms(100);
    g_spawn_direct = 1;
    sprites_reload_slot(3);
    g_spawn_direct = 0;
    ui_script_exec(13);
    delay_ms(200);
    say(4);
    for (int i = 5; i < 11; i++) {
        g_ent_table[i][53] = 26;
        g_ent_table[i][54] = 15;
    }
}

/* [07] 0x350C8：镜头(27,5)→53AFA 门闸 reload(2)→script46→朝向复位→对白8 */
static void evt07(int unit)
{
    camera_scroll_to(27, 5);
    g_spawn_direct = 1;
    sprites_reload_slot(2);
    g_spawn_direct = 0;
    ui_script_exec(46);
    ents_face_reset();
    say(8);
}

/* [08] 0x35123（2026-09-13 逐指令重核）：**仅限 unit==0（主角）**——
 * 0x3512D cmp arg_0,0/jnz 返回；满包（inventory_used_count(unit)==8）
 * 或 spawner_flags[16] 已置 → 返回；否则 ent_inventory_add(unit,89)+
 * 对白11+置标志（一次性）。槽 mode=1＝行动落格结算（玩家
 * battle_unit_turn 与 AI 公共尾均以此模式探测该格），守卫确保隐藏
 * 物品只归主角——旧译缺守卫，敌兵踩格会把剧情物品偷进敌包。 */
static void evt08(int unit)
{
    if (unit != 0)
        return;
    if (sub_1B8A6(unit) == 8)
        return;
    if (g_spawner_flags[16])
        return;
    ent_inventory_add(unit, 89);
    say(11);
    g_spawner_flags[16] = 1;
}

/* [09] 0x34818：一次性（!ent_flag1_test(6)）：reload(2)（我方增援组2×12
 * 出场，2026-09-07 对照原版补齐——此前缺失导致增援整体不生成）→
 * 镜头(3,0)→delay800→(3,17)→delay200→对白4。 */
static void evt09(int unit)
{
    if (!ent_flag1_test(6)) {
        sprites_reload_slot(2);
        camera_scroll_to(3, 0);
        delay_ms(800);
        camera_scroll_to(3, 17);
        delay_ms(200);
        say(4);
    }
}

/* [10] 0x35191：一次性（spawner_flags[16]）：misc(16,71,0)+对白1 */
static void evt10(int unit)
{
    if (!g_spawner_flags[16]) {
        ents_misc_set_range(16, 71, 0);
        say(1);
        g_spawner_flags[16] = 1;
    }
}

/* [11] 0x348BB：立绘(2)→对白2 */
static void evt11(int unit)
{
    sprites_reload_slot(2);
    say(2);
}

/* [12] 0x348EA：一次性：misc(24,27,7)+对白3 → spawner_flags[16]=1 */
static void evt12(int unit)
{
    if (!g_spawner_flags[16]) {
        ents_misc_set_range(24, 27, 7);
        say(3);
        g_spawner_flags[16] = 1;
    }
}

/* [13] 0x351E6：对白6；ent64..73 spawn type 清 0；misc(64,73,3)+misc(35,49,0) */
static void evt13(int unit)
{
    say(6);
    for (int i = 64; i <= 73; i++)
        g_ent_table[i][53] = 0;
    ents_misc_set_range(64, 73, 3);
    ents_misc_set_range(35, 49, 0);
}

/* [14] 0x34940：misc(37,40,0)+misc(13,24,0)→对白3 */
static void evt14(int unit)
{
    ents_misc_set_range(37, 40, 0);
    ents_misc_set_range(13, 24, 0);
    say(3);
}

/* [15] 0x34984：text_busy=0→53AFA 门闸 reload(2)→镜头(14,0)→script23
 * →朝向复位→misc(7,12,0)+misc(33,35,0)→对白4 */
static void evt15(int unit)
{
    g_text_busy = 0;
    g_spawn_direct = 1;
    sprites_reload_slot(2);
    g_spawn_direct = 0;
    camera_scroll_to(14, 0);
    ui_script_exec(23);
    ents_face_reset();
    ents_misc_set_range(7, 12, 0);
    ents_misc_set_range(33, 35, 0);
    say(4);
}

/* [16] 0x349EC：立绘(3)→对白5 */
static void evt16(int unit)
{
    sprites_reload_slot(3);
    say(5);
}

/* [17] 0x34A1E：misc(48,51,7)→对白6→script24→对白7 */
static void evt17(int unit)
{
    ents_misc_set_range(48, 51, 7);
    say(6);
    ui_script_exec(24);
    say(7);
}

/* [18] 0x35258：对白8→misc(16,34,0) */
static void evt18(int unit)
{
    say(8);
    ents_misc_set_range(16, 34, 0);
}

/* [19] 0x34A6C：misc(7,36,7)→对白8→ents 7..36 任一存活 → 对白11（一次）。
 * 0x34AB3..0x34AD7 循环仅收集布尔：i∈[7,0x25)，!ent_flag1_test(i)（存活）
 * → v9=1；0x34AD7 cmp v9,1 / jnz 跳过——text_render_box(str 11) 在循环外
 * 单次执行。旧译"逐实体渲染+已亡极性"两处皆误（第五章首领死后对白
 * 按阵亡数连播的根因，2026-09-13 反汇编定案）。 */
static void evt19(int unit)
{
    int any_alive = 0;
    ents_misc_set_range(7, 36, 7);
    say(8);
    for (int i = 7; i < 0x25; i++)
        if (!ent_flag1_test(i))
            any_alive = 1;
    if (any_alive)
        say(11);
}

/* [20] 0x34B07：对白1 */
static void evt20(int unit) { say(1); }

/* [21] 0x34B2F：条件（!ent_flag1_test(8)）→对白2 */
static void evt21(int unit)
{
    if (!ent_flag1_test(8))
        say(2);
}

/* [22] 0x34B6F：条件（!ent_flag1_test(8)）→立绘(1)+对白3。
 * 0x34C5C 是该项与 [24] 共用的对白尾。 */
static void evt22(int unit)
{
    if (!ent_flag1_test(8)) {
        sprites_reload_slot(1);
        say(3);
    }
}

/* [23] 0x34B9A：群体开关+文本4；turn<15 时追加两段走位，
 * 最后标记实体33并置 0x51A83 闸。 */
static void evt23(int unit)
{
    ents_misc_set_range(8, 28, 0);
    say(4);
    if (g_turn_count >= 15)
        return;
    sprites_reload_slot(2);
    camera_scroll_to(5, 17);
    ui_script_exec(25);
    say(5);
    camera_scroll_to(5, 17);
    ui_script_exec(26);
    ent_mark_flag1(33);
    g_text_busy = 1;   /* 0x34C4D 尾跳 evt72 共享尾（§13.75） */
}

/* [24] 0x34C52：共享对白尾，渲染文本3。 */
static void evt24(int unit) { say(3); }

/* [25] 0x34C7A：一次性（spawner_flags[16]==1 触发）：53AFA 门闸 reload(2)
 * →镜头(16,10)→script30→对白2 → spawner_flags[17]=1。 */
static void evt25(int unit)
{
    if (g_spawner_flags[16] == 1) {
        g_spawn_direct = 1;
        sprites_reload_slot(2);
        g_spawn_direct = 0;
        camera_scroll_to(16, 10);
        ui_script_exec(30);
        say(2);
        g_spawner_flags[17] = 1;
    }
}

/* [26] 0x34CF1（2026-09-13 逐指令重核定案）：守卫 ent[unit][6]!=0
 * （0x34D0E movzx [ent+6]、0x34D15 jz——敌方单位不触发）→
 * ents_misc_set_range(9,27,0)（首波敌 9..27 的 +52 低半字节清 0＝
 * 行为 0 标准攻击 AI；布阵初值 0/2 混合，即"伏击解锁"）→
 * g_spawner_flags[16]=1（0x34D25——武装 evt25 的第 10 回合增援门，
 * stage6 唯一写点，battle_field_preset 开场清零）。 */
static void evt26(int unit)
{
    if (g_ent_table[unit][6] == 0)
        return;
    ents_misc_set_range(9, 27, 0);
    g_spawner_flags[16] = 1;
}

/* [27] 0x34D2F：镜头(8,2)→delay(100)→立绘(turn)→delay(100) */
static void evt27(int unit)
{
    camera_scroll_to(8, 2);
    delay_ms(100);
    sprites_reload_slot(g_turn_count);
    delay_ms(100);
}

/* [28] 0x34D64：实体10..27的 +52 仅保留 bit7，清除其它位。 */
static void evt28(int unit)
{
    for (int i = 10; i <= 27 && i < g_ent_count; i++)
        g_ent_table[i][52] &= 0x80;
}

/* [29] 0x34D92：对白2→evt28 */
static void evt29(int unit)
{
    say(2);
    evt28(unit);
}

/* [30] 0x34DD0（2026-09-13 逐指令重核定案——旧译只抄了过滤轨迹里的
 * 对白+reload，四段关键逻辑全漏）：击杀 ent11 首领（icon118）触发——
 * ① ents 12..33 的 +52 行为字节清 0（守军全转猎手，总攻）；
 * ② ctx[3]=turn+1、ctx[6]=turn+2：运行时武装回合事件槽 0/1（资产
 *    turn=255 禁用态，evt=31）——evt31 将在随后两回合敌方相前触发；
 * ③ ent11 七字段变身（阵亡首领→NPC 队友）：[5]=0 复活/清标志、
 *    [6]=1（team=NPC）、[7]=6/[8]=6（class/name=6，此后对白肖像与
 *    寻名都按角色 6）、[49]=-1、[52]=0x80、HP(+64 u16)=1（MaxHP 不动）；
 * ④ say(2) → reload(1)（组1=icon83 新指挥官+4×icon82）→ say(3) →
 *    g_exp_gained=0（变身前击杀的经验作废）→ g_spawner_flags[16]=2
 *    （evt31 序贯装载器初值：首_fire reload(2)→flags++→次_fire
 *    reload(3)，组2=6×icon78、组3=2×icon87 追击波）。
 * 实体计数钉死：11 玩家+23 初始+5+6+2=47，组4 单位恰落 ent47——
 * st08_enter 的 script36（ent47 下行 5 格）与 str4 说话人依赖此序。 */
static void evt30(int unit)
{
    for (int i = 12; i <= 33 && i < g_ent_count; i++)
        g_ent_table[i][52] = 0;
    battle_ctx_turn_event_arm(0, (uint8_t)(g_turn_count + 1));
    battle_ctx_turn_event_arm(1, (uint8_t)(g_turn_count + 2));
    uint8_t *e = g_ent_table[11];
    e[5] = 0;
    e[6] = 1;
    e[7] = 6;
    e[8] = 6;
    e[49] = 0xFF;
    e[52] = 0x80;
    put_u16(e + 64, 1);
    say(2);
    sprites_reload_slot(1);
    say(3);
    g_exp_gained = 0;
    g_spawner_flags[16] = 2;
}

/* [31] 0x34EB3：立绘(flags[16])→flags[16]++→四角巡视
 * (0,0)/(12,0)/(12,11)/(0,11) 各 delay(200) */
static void evt31(int unit)
{
    sprites_reload_slot(g_spawner_flags[16]);
    g_spawner_flags[16]++;
    camera_scroll_to(0, 0);   delay_ms(200);
    camera_scroll_to(12, 0);  delay_ms(200);
    camera_scroll_to(12, 11); delay_ms(200);
    camera_scroll_to(0, 11);  delay_ms(200);
}

/* [32] 0x34F38：共享体，立绘1→对白1。 */
static void evt32(int unit) { sprites_reload_slot(1); say(1); }

/* [33] 0x34F74（2026-09-13 逐指令重核）：say(2) → **ents 12/13 的
 * +52 行为字节清 0**（0x34FA5/0x34FB3 两段裸写——stage9 回合 20 敌方
 * 相前触发，icon80 前卫两员转猎手；旧译漏此两写）。 */
static void evt33(int unit)
{
    say(2);
    if (g_ent_count > 12)
        g_ent_table[12][52] = 0;
    if (g_ent_count > 13)
        g_ent_table[13][52] = 0;
}

/* [34] 0x34FC2：共享对白尾，渲染文本3。 */
static void evt34(int unit) { say(3); }

/* [35] 0x34FCC：镜头(12,5)→53AFA 门闸 reload(2)→script42→朝向复位 */
static void evt35(int unit)
{
    camera_scroll_to(12, 5);
    g_spawn_direct = 1;
    sprites_reload_slot(2);
    g_spawn_direct = 0;
    ui_script_exec(42);
    ents_face_reset();
}

/* [36] 0x35009：实体14的 +52 直接写 0x83。 */
static void evt36(int unit)
{
    if (g_ent_count > 14)
        g_ent_table[14][52] = 0x83;
}

/* [37] 0x35022：两段走位：对白1→(15,34)/53AFA 门闸立绘3/script43→
 * (0,26)/门闸立绘4/script44 */
static void evt37(int unit)
{
    say(1);
    camera_scroll_to(15, 34);
    g_spawn_direct = 1;
    sprites_reload_slot(3);
    g_spawn_direct = 0;
    ui_script_exec(43);
    ents_face_reset();
    camera_scroll_to(0, 26);
    g_spawn_direct = 1;
    sprites_reload_slot(4);
    g_spawn_direct = 0;
    ui_script_exec(44);
    ents_face_reset();
    g_text_busy = 1;   /* 0x350B9 尾跳 evt72 共享尾（§13.75） */
}

/* [38] 0x35298：立绘(1)→对白10 */
static void evt38(int unit)
{
    sprites_reload_slot(1);
    say(10);
}

/* [39] 0x352CA：应用内置记录 {type=0,val=0xD3}，再渲染结果文本11。 */
static void evt39(int unit)
{
    const uint8_t rec[3] = { 0, 0xD3, 0 };
    battle_show_results(unit, 1, rec);
    say(11);
}

/* [40] 0x35321：立绘(2)→镜头(17,37)→对白1 */
static void evt40(int unit)
{
    sprites_reload_slot(2);
    camera_scroll_to(17, 37);
    say(1);
}

/* [41] 0x35346：对白3→战果记录 unk_52745={0,0xD5,0}→对白4 */
static void evt41(int unit)
{
    const uint8_t rec[3] = { 0, 0xD5, 0 };   /* unk_52745 */
    say(3);
    battle_show_results(unit, 1, rec);
    say(4);
}

/* [42] 0x353B5：立绘(1)→对白6。
 * 原版从 0x353E0 压入 str_id=6 后跳到 0x34F65；该共享尾只有
 * text_render_box，**不**写 g_text_busy；这里保留直接调用以显式标出
 * 共享尾边界（普通 say() 现也只封装该原语）。 */
static void evt42(int unit)
{
    sprites_reload_slot(1);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 6, g_pkg_text_evt);
}

/* [43] 0x353E7：misc(16,16,3) */
static void evt43(int unit) { ents_misc_set_range(16, 16, 3); }

/* [44] 0x353FA：misc(29,59,3) */
static void evt44(int unit) { ents_misc_set_range(29, 59, 3); }

/* [45] 0x3540F：misc(16,31,3) */
static void evt45(int unit) { ents_misc_set_range(16, 31, 3); }

/* [46] 0x35422：立绘(1)→直接渲染对白1→spawn(27)。
 * 原版文本尾是 0x34F65 的裸 text_render_box，共享尾不写
 * g_text_busy；这里保留直接调用以显式标出该边界。 */
static void evt46(int unit)
{
    sprites_reload_slot(1);
    text_render_box(1, 19, 74, 205, 320, vram_base(), 1, g_pkg_text_evt);
    roster_spawn_ent(27);
}

/* [47] 0x35468：立绘(turn/2)→wait1→四角巡视 (0,0)→(28,0)→(28,32)→
 * (0,32)→对白3 */
static void evt47(int unit)
{
    /* 2026-09-14 复核更正：四角间各 wait_bios_ticks(8)（0x3549F/0x354B5/
     * 0x354CB/0x354E1）；say(3) 仅 turn==2（0x354EB cmp/jnz——增援段
     * 回合 4/6/8 不播）。 */
    sprites_reload_slot(g_turn_count / 2);
    wait_bios_ticks(1);
    camera_scroll_to(0, 0);
    wait_bios_ticks(8);
    camera_scroll_to(28, 0);
    wait_bios_ticks(8);
    camera_scroll_to(28, 32);
    wait_bios_ticks(8);
    camera_scroll_to(0, 32);
    wait_bios_ticks(8);
    if (g_turn_count == 2)
        say(3);
}

/* [48] 0x3551C（2026-09-14 复核更正）：misc(35,42,3) 后 push(3,74,67)
 * **jmp 0x3528F**——evt18 体内 ents_misc_set_range 调用点的共享跳板
 * （借调用方压参）＝misc(67,74,3)。旧译 say(8)+misc(16,34,0) 系把
 * 跳板前后的 evt18 自有指令误并入。 */
static void evt48(int unit)
{
    ents_misc_set_range(35, 42, 3);
    ents_misc_set_range(67, 74, 3);
}

/* [49] 0x3553F：立绘(turn/2)→镜头(32,35)/(0,35) 各 wait8→
 * turn==3 时对白1 */
static void evt49(int unit)
{
    sprites_reload_slot(g_turn_count / 2);
    camera_scroll_to(32, 35);
    wait_bios_ticks(8);
    camera_scroll_to(0, 35);
    wait_bios_ticks(8);
    if (g_turn_count == 3)
        say(1);
}

/* [50] 0x355B7：立绘(2)→镜头(16,42)→wait8→spawn(20)→对白2 */
static void evt50(int unit)
{
    sprites_reload_slot(2);
    camera_scroll_to(16, 42);
    wait_bios_ticks(8);
    roster_spawn_ent(20);
    say(2);
}

/* [51] 0x355F0：战果记录 unk_52748={0,0x65,0}→对白3 */
static void evt51(int unit)
{
    const uint8_t rec[3] = { 0, 0x65, 0 };   /* unk_52748 */
    battle_show_results(unit, 1, rec);
    say(3);
}

/* [52] 0x35638：按 turn-14 选择两张转场立绘。 */
static void evt52(int unit)
{
    int slot = 2 * (g_turn_count - 14);
    cutscene_transition(2, 11, slot);
    cutscene_transition(26, 11, slot + 1);
}

/* [53] 0x35677：对白5→ents_kill_from(18)（友军/群众撤退清场） */
static void evt53(int unit)
{
    say(5);
    ents_kill_from(18);
}

/* [54] 0x356B3：立绘(turn)→镜头巡航 (0,4)/(0,22)/(26,24)/(26,2)
 * 各 delay(400) */
static void evt54(int unit)
{
    sprites_reload_slot(g_turn_count);
    camera_scroll_to(0, 4);   delay_ms(400);
    camera_scroll_to(0, 22);  delay_ms(400);
    camera_scroll_to(26, 24); delay_ms(400);
    camera_scroll_to(26, 2);  delay_ms(400);
}

/* [55] 0x35730：arg==0∧flags[0]==0 时：对白0→duel(arg,17)→死亡动画
 * →ent17 flag1 已置则 flags[0]=1+刷新+tick+战果{0,0x0B,0}；
 * 收尾 g_exp_gained=0 */
static void evt55(int unit)
{
    const uint8_t rec[3] = { 0, 0x0B, 0 };   /* unk_5274B */

    if (unit == 0 && !g_spawner_flags[0]) {
        say(0);
        duel_scene_load(unit, 17);
        battle_death_anim();
        if (ent_flag1_test(17)) {
            g_spawner_flags[0] = 1;
            map_spawn_count_refresh();
            anim_tick_update(1);
            battle_show_results(unit, 1, rec);
        }
    }
    g_exp_gained = 0;
}

/* [56] 0x357DD：镜头(6,40)→立绘(1)→script74→对白5→朝向复位 */
static void evt56(int unit)
{
    camera_scroll_to(6, 40);
    sprites_reload_slot(1);
    ui_script_exec(74);
    say(5);
    ents_face_reset();
}

/* [57] 0x35833：立绘(turn)→镜头(9,0) */
static void evt57(int unit)
{
    sprites_reload_slot(g_turn_count);
    camera_scroll_to(9, 0);
}

/* [58] 0x35854：战果对白组合：actor_icon(参数单位)→DATO 窗格+文本480、
 * 422（g_disp_num_a 数值行）→map_spawn_count_refresh */
static void evt58(int unit)
{
    kbd_flush();
    sub_1B8A6(unit);
    dialog_backdrop_load(g_ent_table[unit][7]);
    text_render_box(1, 19, 74, 205, 320, SAY_DST_SHOP, 480, g_pkg_fdtxt0);
    dato_frame_stamp(0); wait_key_anim(0); dialog_backdrop_restore();
    text_render_box(1, 19, 74, 205, 320, SAY_DST_SHOP,
                    g_disp_num_a + 422, g_pkg_fdtxt0);
    dato_frame_stamp(0); wait_key_anim(0); dialog_backdrop_restore();
    map_spawn_count_refresh();
}

/* [59] 0x35997：type!=0 时 misc(39,44,0)，否则直接返回 */
static void evt59(int unit)
{
    if (!g_ent_table[unit][6])
        return;
    ents_misc_set_range(39, 44, 0);
}

/* [60] 0x359CB：ent[unit].type!=0 时 misc(23,24,0)+misc(53,56,0) */
static void evt60(int unit)
{
    if (unit < 0 || unit >= g_ent_count || !g_ent_table[unit][6])
        return;
    ents_misc_set_range(23, 24, 0);
    ents_misc_set_range(53, 56, 0);
}

/* [61] 0x35A0D：一次性道具208剧情：无道具时对白2后返回；命中时移除
 * 槽位、对白3、播放 FDOTHER block45 的 59 帧，再置 flag12、刷新地图、
 * 重载立绘1、spawn(31)、对白4。 */
static void evt61(int unit)
{
    void *pkg;
    int slot;

    if (g_spawner_flags[12])
        return;
    dialog_backdrop_load(g_ent_table[unit][7]);
    slot = inventory_find_item_slot(unit, 208);
    if (slot == -1) {
        say(2);
        dato_frame_stamp(0);
        wait_key_anim(0);
        dialog_backdrop_restore();
        return;
    }

    inventory_remove_item(unit, slot);
    say(3);
    wait_key_anim(0);
    dialog_backdrop_restore();

    pkg = dat_load_block("FDOTHER.DAT", NULL, 45);
    for (int i = 0; i < 59; i++) {
        blit_frame_flat(pkg, i, vram_base(), 320, -1);
        wait_bios_ticks(2);
    }
    free(pkg);
    g_spawner_flags[12] = 1;
    map_spawn_count_refresh();
    sprites_reload_slot(1);
    roster_spawn_ent(31);
    say(4);
}

/* [62] 0x35BEE：一次性推进剧情回合并置 spawner_flags[17]。 */
static void evt62(int unit)
{
    if (!g_spawner_flags[17]) {
        g_battle_ctx[3] = (uint8_t)(g_turn_count + 1);
        g_spawner_flags[17] = 1;
    }
}

/* [63] 0x35C1D：transition(3,27,1)+transition(2,27,15) */
static void evt63(int unit)
{
    cutscene_transition(3, 27, 1);
    cutscene_transition(2, 27, 15);
}

/* [64] 0x35C40：flags[16] 分相位：1→对白1+三连转场
 * (9,44,3)/(0,9,4)/(17,9,5)+text_busy=1；2→对白2+kill_from(16)；
 * 末尾统一 flags[16]++ */
static void evt64(int unit)
{
    int phase = g_spawner_flags[16];

    if (phase == 1) {
        say(1);
        cutscene_transition(9, 44, 3);
        cutscene_transition(0, 9, 4);
        cutscene_transition(17, 9, 5);
        g_text_busy = 1;
    } else if (phase == 2) {
        say(2);
        ents_kill_from(16);
    }
    g_spawner_flags[16]++;
}

/* [65] 0x35CF1：一次性记录当前回合到剧情上下文。 */
static void evt65(int unit)
{
    if (!g_spawner_flags[16]) {
        g_battle_ctx[3] = (uint8_t)g_turn_count;
        g_spawner_flags[16] = 1;
    }
}

/* [66] 0x35D1E：对白3→transition(17,18,1)→对白6 */
static void evt66(int unit)
{
    say(3);
    cutscene_transition(17, 18, 1);
    say(6);
}

/* [67] 0x35D85：写入剧情上下文当前回合。 */
static void evt67(int unit) { g_battle_ctx[6] = (uint8_t)g_turn_count; }

/* [68] 0x35D9E：对白4→transition(14,7,2)→对白6→flags[18]=1 */
static void evt68(int unit)
{
    say(4);
    cutscene_transition(14, 7, 2);
    say(6);
    g_spawner_flags[18] = 1;
}

/* [69] 0x35E0E：满足实体/标志条件时记录回合并锁定一次性标志。 */
static void evt69(int unit)
{
    if (g_ent_table[unit][6] && !g_spawner_flags[17]
        && g_spawner_flags[18]) {
        g_battle_ctx[9] = (uint8_t)g_turn_count;
        g_spawner_flags[17] = 1;
    }
}

/* [70] 0x35E5B：misc(41,45,0)→对白5→转场(8,7,3)/(4,7,4)，
 * 共享尾再执行 (17,18,1)→对白6。 */
static void evt70(int unit)
{
    ents_misc_set_range(41, 45, 0);
    say(5);
    cutscene_transition(8, 7, 3);
    cutscene_transition(4, 7, 4);
    cutscene_transition(17, 18, 1);
    say(6);
}

/* [71] 0x35EC1：对白2→ents_kill_from(20) */
static void evt71(int unit)
{
    say(2);
    ents_kill_from(20);
}

/* [72] 0x35F48：transition(4,35,2)+transition(14,35,3)→text_busy=1 */
static void evt72(int unit)
{
    cutscene_transition(4, 35, 2);
    cutscene_transition(14, 35, 3);
    g_text_busy = 1;
}

/* [73] 0x35F79：置 spawner_flags[18]。 */
static void evt73(int unit) { g_spawner_flags[18] = 1; }

/* [74] 0x35F88：按剧情阶段转场并推进 spawner_flags[16]。 */
static void evt74(int unit)
{
    int phase = g_spawner_flags[16];
    cutscene_transition(10, 29, phase);
    if (phase != 7)
        g_battle_ctx[3] = (uint8_t)(g_turn_count + 1);
    g_spawner_flags[16] = (uint8_t)(phase + 1);
}

/* [75] 0x35FCF：type!=0∧flags[17]==0 时：ent[+8]==9 → 对白1+
 * flags[17]=1+ctx[6]=turn+1+flags[16]=4+ctx[3]=turn；否则
 * 对话框(0xA951F 底)+等键+restore */
static void evt75(int unit)
{
    if (!g_ent_table[unit][6])
        return;
    if (g_spawner_flags[17])
        return;

    if (g_ent_table[unit][8] == 9) {
        say(1);
        g_spawner_flags[17] = 1;
        g_battle_ctx[6] = (uint8_t)(g_turn_count + 1);
        g_spawner_flags[16] = 4;
        g_battle_ctx[3] = (uint8_t)g_turn_count;
        return;
    }
    dialog_backdrop_load(g_ent_table[unit][7]);
    text_render_box(1, 19, 74, 205, 320, SAY_DST_PANEL, 0, g_pkg_text_evt);
    dato_frame_stamp(0);
    wait_key_anim(0);
    dialog_backdrop_restore();
}

/* [76] 0x360B6：flags[17] 未到4时标记 ent1、阶段++、安排 ctx[6]=turn+1；
 * 到4时才执行 str2→reload1→flags[21]=ent_count-3→ctx[9]=turn，
 * 两次闪光（各 delay400），再对 i=3..6 逐次闪光+str(i)。 */
static void evt76(int unit)
{
    if (g_spawner_flags[17] != 4) {
        sub_13512(1);
        g_spawner_flags[17]++;
        g_battle_ctx[6] = (uint8_t)(g_turn_count + 1);
        return;
    }

    say(2);
    sprites_reload_slot(1);
    g_spawner_flags[21] = (uint8_t)(g_ent_count - 3);
    g_battle_ctx[9] = (uint8_t)g_turn_count;
    sub_361B0(0);
    delay_ms(400);
    sub_361B0(0);
    delay_ms(400);
    for (int i = 3; i <= 6; i++) {
        sub_361B0(0);
        say(i);
    }
}

/* [77] 0x36214：置 spawner_flags[19]。 */
static void evt77(int unit) { g_spawner_flags[19] = 1; }

/* [78] 0x36228：置 spawner_flags[20]。 */
static void evt78(int unit) { g_spawner_flags[20] = 1; }

/* [79] 0x3623C：当前回合+1，并从阶段基址起随机标记两实体完成。 */
static void evt79(int unit)
{
    g_battle_ctx[9] = (uint8_t)(g_turn_count + 1);
    int base = g_spawner_flags[21];
    int r = (int)fd2_rand();
    sub_13512(base + r % 3);
    sub_13512(base + (r + 1) % 3);
}

/* [80] 0x362B0：共享尾，misc(20,20,11)。 */
static void evt80(int unit) { ents_misc_set_range(20, 20, 11); }

/* [81] 0x362C5：阶段递增并写入下一回合。 */
static void evt81(int unit)
{
    g_spawner_flags[16]++;
    g_battle_ctx[3] = (uint8_t)(g_turn_count + 1);
}

/* [82] 0x362E8：镜头(16,1)→对白(flags[16]+2)→镜头(16,14)→
 * scroll_and_cutscene(24-ph,22,18)；ph==4 收束于 misc(20,20,11)，
 * 否则 reload(ph)+misc(24-ph,24-ph,0)+走位转场 (25+2ph,21,18)/
 * (26+2ph,23,18)；两路均 text_busy=1 */
static void evt82(int unit)
{
    int phase = g_spawner_flags[16];

    camera_scroll_to(16, 1);
    say(phase + 2);
    g_text_busy = 0;
    camera_scroll_to(16, 14);
    scroll_and_cutscene(0x18 - phase, 0x16, 0x12);
    if (phase == 4) {
        ents_misc_set_range(20, 20, 11);
        g_text_busy = 1;
        return;
    }
    sprites_reload_slot(phase);
    ents_misc_set_range(0x18 - phase, 0x18 - phase, 0);
    scroll_and_cutscene(0x19 + 2 * phase, 0x15, 0x12);
    scroll_and_cutscene(0x1A + 2 * phase, 0x17, 0x12);
    g_text_busy = 1;
}

/* [83] 0x363DE：对白8→ents_kill_from(20) */
static void evt83(int unit)
{
    say(8);
    ents_kill_from(20);
}

/* [84] 0x36416：共享尾直接清实体16..(ent_count-1)的低半字节；
 * 原版无 g_ent_count 守卫，空区间由区间循环自然不执行。 */
static void evt84(int unit) { ents_misc_set_range(16, g_ent_count - 1, 0); }

/* [85] 0x3642E：仅栈探测。 */
static void evt85(int unit) { }

/* [86] 0x36439：仅栈探测。 */
static void evt86(int unit) { }

/* [87] 0x36440：仅栈探测。 */
static void evt87(int unit) { }

/* [88] 0x36447：仅栈探测。 */
static void evt88(int unit) { }

/* [89] 0x3644E：仅栈探测。 */
static void evt89(int unit) { }

/* ---- 表本体（IDA 0x51B91，90 项） ----------------------------------- */
fd2_event_script_fn g_event_script_table[90] = {
    evt00, evt01, evt02, evt03, evt04, evt05, evt06, evt07, evt08, evt09,
    evt10, evt11, evt12, evt13, evt14, evt15, evt16, evt17, evt18, evt19,
    evt20, evt21, evt22, evt23, evt24, evt25, evt26, evt27, evt28, evt29,
    evt30, evt31, evt32, evt33, evt34, evt35, evt36, evt37, evt38, evt39,
    evt40, evt41, evt42, evt43, evt44, evt45, evt46, evt47, evt48, evt49,
    evt50, evt51, evt52, evt53, evt54, evt55, evt56, evt57, evt58, evt59,
    evt60, evt61, evt62, evt63, evt64, evt65, evt66, evt67, evt68, evt69,
    evt70, evt71, evt72, evt73, evt74, evt75, evt76, evt77, evt78, evt79,
    evt80, evt81, evt82, evt83, evt84, evt85, evt86, evt87, evt88, evt89,
};

uint8_t g_pending_event_id = 255;
