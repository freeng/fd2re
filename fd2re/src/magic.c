/* magic.c — 魔法/大招效果执行层骨架（IDA 0x1C000-0x22000）
 *
 * 结构（docs/architecture.md §14，2026-09-04）：
 *   magic_effect_dispatch ← battle_act_menu 魔法项(5..0x18) / fx_scene_special(32-35)
 *   实现层同构模式：fx_field_anim(地图动画, FDOTHER blk6 帧表
 *   g_fx_field_frame_base/count/sfx_first @0x51F33/54/75) → 逐目标结算
 *   （伤害 fx_damage_adjust、免疫 fx_hit_immune_test、状态写 ent[+34..39]）
 *   → dmg/miss 弹出队列（g_popup_* @0x53C6C..53EC4）→ popup_flush_float 上浮。
 * 内嵌调试串 "Out of memory at Get_EasyMagic" 实锤家族身份。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fd2.h"
#include "magic.h"
#include "battle.h"
#include "duel.h"
#include "entities.h"
#include "audio.h"
#include "timer.h"
#include "resource.h"
#include "text.h"
#include "video.h"
#include "states.h"

uint8_t g_popup_glyph[FD2_POPUP_CAPACITY]; /* 0x53C6C */
uint8_t g_popup_x[FD2_POPUP_CAPACITY];     /* 0x53D34 */
uint8_t g_popup_ent[FD2_POPUP_CAPACITY];   /* 0x53DFC */
int32_t g_popup_count;                     /* 0x53EC4 */

void fx_magic_sfx_load(void)
{
    /* sub_1D4CB: every magic dispatch replaces the shared FDOTHER block 80
     * package before any field/flash effect starts. */
    g_magic_sfx_pkg = dat_load_block("FDOTHER.DAT", g_magic_sfx_pkg, 80);
}

void fx_magic_sfx_stop(void)
{
    /* sub_1D4F6 pushes g_magic_sfx_pkg before jumping to the shared
     * free-at-0x1A80A tail (the package is not retained between actions). */
    sfx_play(g_magic_sfx_pkg, -1, 1);
    free(g_magic_sfx_pkg);
    g_magic_sfx_pkg = NULL;
}

/* ---- 效果参数表（dseg03 @0x619FD，7B/条；访问器 effect_param_entry=0x4E866
 * = base + 7*effect，原版无界检查）。[0..1]i16 威力 [2]命中率 [3]射程
 * [4]作用形状 [5]MP 消耗 [6]敌我(0=敌/1=己)。
 * 嵌入 252B=36 条（2026-09-05 修正：原 280B 越界吃进 g_enemy_base_table@
 * 0x61AF9 前 28B）。消费上界 effect id=24（分发 §14.5；武器表 [13] 实测
 * ∈{0,13,17..22,24}）；0x61AC1（条 28）起字段模式渐失配，真实条数 29..36
 * 挂号；36 为邻表 g_enemy_base_table@0x61AF9 边界倒推的硬上限。 ---- */
static const uint8_t s_effect_params_raw[252] = {
    0x32,0x00,0x5A,0x05,0x00,0x02,0x00,0x78,0x00,0x5A,0x05,0x00,0x06,0x00,
    0xFA,0x00,0x5A,0x05,0x01,0x14,0x00,0xF4,0x01,0x55,0x05,0x01,0x2A,0x00,
    0x28,0x00,0x55,0x04,0x01,0x04,0x00,0x64,0x00,0x50,0x04,0x01,0x0F,0x00,
    0xDC,0x00,0x50,0x04,0x02,0x1E,0x00,0xC2,0x01,0x50,0x04,0x02,0x3C,0x00,
    0xB8,0x01,0x64,0x08,0x00,0x18,0x00,0xE7,0x03,0x32,0x03,0x00,0x1E,0x00,
    0x50,0x00,0x5F,0x00,0x05,0x12,0x00,0xA0,0x00,0x5A,0x00,0x07,0x2D,0x00,
    0x54,0x01,0x5A,0x00,0x09,0x50,0x00,0x46,0x00,0x00,0x04,0x00,0x03,0x01,
    0x8C,0x00,0x00,0x04,0x01,0x0A,0x01,0x04,0x01,0x00,0x05,0x02,0x14,0x01,
    0xF4,0x01,0x00,0x05,0x03,0x28,0x01,0x00,0x00,0x00,0x04,0x02,0x05,0x01,
    0x00,0x00,0x00,0x04,0x02,0x05,0x01,0x00,0x00,0x00,0x04,0x02,0x08,0x01,
    0x00,0x00,0x00,0x04,0x02,0x05,0x01,0x00,0x00,0x00,0x04,0x02,0x05,0x01,
    0x00,0x00,0x00,0x04,0x02,0x08,0x00,0x00,0x00,0x00,0x03,0x00,0x14,0x03,
    0x00,0x00,0x00,0x05,0x01,0x16,0x00,0x00,0x00,0x00,0x03,0x01,0x18,0x01,
    0x0A,0x00,0x32,0x04,0x02,0x08,0x00,0x0A,0x00,0x32,0x04,0x02,0x0A,0x00,
    0x00,0x00,0x00,0x01,0x00,0x16,0x00,0x00,0x00,0x00,0x00,0x02,0x1A,0x00,
    0x00,0x00,0x00,0x14,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x02,0x1A,0x00,
    0x20,0x03,0x5A,0x05,0x03,0x4C,0x00,0x00,0x00,0x00,0x05,0x03,0x34,0x01,
    0x00,0x00,0x00,0x05,0x03,0x1C,0x01,0x00,0x00,0x00,0x04,0x02,0x24,0x00,
};

/* 职业系威力系数表 @0x51F96（16×dword，索引=职业系-1；dump 2026-09-04） */
static const int32_t s_class_power_lut[16] = {
    10, 10, 10, 10, 7, 7, 10, 10, 10, 9, 10, 5, 5, 8, 10, 10,
};

/* 0x1C269（2026-09-04 实现）：ent[+26..+30] 位图展开为法术 id 列表。 */
int spells_collect(int unit, uint8_t *out)
{
    const uint8_t *rec = g_ent_table[unit];
    int n = 0;
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 8; j++)
            if ((rec[26 + i] >> j) & 1)
                out[n++] = (uint8_t)(j + 8 * i);
    return n;
}

const uint8_t *effect_param_entry(int effect_id)
{
    /* 0x4E866：dseg03 base + 7*effect（无界检查，与原版一致） */
    return s_effect_params_raw + 7 * effect_id;
}

/* 0x1C75E（2026-09-04 实现化）：命中判定链——
 * 伤害基数 = s_class_power_lut[职业系-1] × 威力 / 10；
 * effect 10..12 且目标地形免疫（sub_1F183）→ 无伤（返回 0）；
 * 命中 = rand%100 < 效果[2] → fx_damage_adjust（返回伤害，非零=命中）。 */
int fx_hit_immune_test(int target, int effect_id)
{
    const uint8_t *fx = effect_param_entry(effect_id);
    int power = (int16_t)(fx[0] | (fx[1] << 8));
    int hit = fx[2];
    uint8_t *rec = g_ent_table[target];
    int base = s_class_power_lut[rec[32] - 1] * power / 10;

    if (effect_id > 9 && effect_id < 13 && sub_1F183(target))
        return 0;
    if ((int)(fd2_rand() % 100) < hit)
        /* IDA declares the helper void, but its hit path falls through with
         * fx_damage_adjust's EAX intact; callers use that value for popups. */
        return fx_damage_adjust(target, base);
    return 0;
}

/* 0x20C6F（2026-09-04 实现化）：魔法/道具效果分发器。
 * 分发键 = weapon_table[item][13]；参数 param = [14..15] u16。
 * 收尾：g_exp_gained=0 → 收集掉落 → 死亡动画 → 结算显示（原版
 * 另有 sub_1D4F6 内部步，见 §14.5）。 */
void magic_effect_dispatch(int actor, int unit, int slot, int n_targets,
                           const uint8_t *targets)
{
    fx_magic_sfx_load();
    g_popup_count = 0;              /* 0x20C8C：分发入口清弹字队列 */
    const uint8_t *w = weapon_table_entry(inventory_get_item(unit, slot));
    int type = w[13];
    int param = w[14] | (w[15] << 8);
    int consume = 0;

    switch (type) {
    case 5:                                             /* 群体伤害，消耗道具 */
    case 13:                                            /* 群体伤害，不消耗 */
        fx_mass_damage(actor, n_targets, targets, param);
        consume = (type == 5);
        break;
    case 6:
        battle_award_exp_list(targets, n_targets, 37);  /* 经验道具 A */
        consume = 1;
        break;
    case 7:
        battle_award_exp_list(targets, n_targets, 38);  /* 经验道具 B */
        consume = 1;
        break;
    case 8: case 9: case 10:                            /* 永久成长 +55/57/62 */
        fx_stat_boost(actor, param, slot, n_targets, targets,
                      targets ? targets[0] : 0, 17 + (type - 8),
                      55 + 2 * (type - 8) + (type == 10 ? 3 : 0));
        break;
    case 11: {                                          /* MP 药：MaxMP≠0 者恢复 */
        /* 0x20DCD/0x20DE6 pass animation/effect id 0x0D, independent of
         * the weapon category (11). */
        fx_field_anim(13, n_targets, targets);
        fx_effect_apply(actor, 13, n_targets, targets);
        for (int i = 0; i < n_targets; i++) {
            int t = targets[i];
            if (*(uint16_t *)(g_ent_table[t] + 70)) {
                int amount = mp_restore(t, param);
                dmg_popup_queue(amount, 105, t);
            } else {
                miss_popup_queue(t);
            }
        }
        /* 0x20E44：原版 flush 前有 anim_tick_update(0)（清 fx_effect_apply
         * 剪影后浮字），fd2re 此前缺失。 */
        anim_tick_update(0);
        popup_flush_float();
        consume = 1;
        break;
    }
    case 12: fx_drain_third(actor, n_targets, targets); break;
    /* Native 0x20E74 pushes status offset 0x26 and animation 0x1B. */
    case 14: fx_aoe_status(actor, 27, n_targets, targets, 38); break;
    case 15: fx_drain_def(actor, n_targets, targets);   break;
    case 16: fx_drain_atk(actor, n_targets, targets);   break;
    case 17:                                            /* 永久 MaxHP(+66) */
        fx_stat_boost(actor, param, slot, n_targets, targets,
                      targets ? targets[0] : 0, 13, 66);
        break;
    case 18:                                            /* 永久 MaxMP(+70) */
        fx_stat_boost(actor, param, slot, n_targets, targets,
                      targets ? targets[0] : 0, 13, 70);
        break;
    case 19: {                                          /* exp 道具：参数写入组合字段，保留当前经验 */
        int t = targets[0];
        uint8_t old_exp = g_ent_table[t][60];
        fx_stat_boost(actor, param, slot, n_targets, targets, t, 19, 59);
        /* Native 0x20F53 restores ent[+60] after fx_stat_boost updates
         * ent[+59]; the item parameter is the actual increment. */
        g_ent_table[t][60] = old_exp;
        break;
    }
    case 20: case 24:                                   /* 壳：flash+hit+弹字 */
        /* 0x20F6D uses weapon[param] as the effect id for all three calls;
         * the category only selects this branch. */
        fx_field_anim(param, n_targets, targets);
        fx_unit_flash(param, n_targets, targets);
        for (int i = 0; i < n_targets; i++) {
            int t = targets[i];
            if (fx_hit_immune_test(t, param))
                dmg_popup_queue(t, 94, t);
            else
                miss_popup_queue(t);
        }
        anim_tick_update(0);
        popup_flush_float();
        break;
    case 21: fx_mass_strike(actor, param, n_targets, targets); break;
    case 22: fx_aoe_status(actor, 22, n_targets, targets, 39); break;
    case 23: fx_targeted_strike(actor, n_targets, targets); break;
    default: break;
    }

    if (consume)
        inventory_remove_item(unit, slot);

    g_exp_gained = 0;
    fx_magic_sfx_stop();
    /* 0x21058..0x21072 逐指令：ebx = collect_drops 返回值、push esp =
     * drops 缓冲——原版实参 (unit, n_drops, drops)。旧代码误传
     * (unit, n_targets, targets)，把目标单位号当掉落记录解析（type 0-3
     * → 头像拾获/金钱/事件脚本/事件文本窗），即"攻击前闪头像动画、
     * 每段一次"的根因。 */
    uint8_t drops[15][3];
    int n_drops = battle_collect_drops(drops);
    battle_death_anim();
    battle_show_results(unit, n_drops, (const uint8_t *)drops);
}

void fx_field_anim(int effect_id, int n_targets, const uint8_t *targets)
{
    /* 0x1C4CC：FDOTHER blk6 帧（帧号=三表查 effect_id）贴到每个目标地块，
     * blit_rows 裁 312×192，逐帧音效（g_magic_sfx_pkg@0x53B13）。
     *
     * 宿主渲染（2026-09-12 落地，对齐 IDA 0x1C555 循环体）：备份
     * 456×336 shadow 面 → 每帧整面恢复 → 逐目标（视口 ±1 裁剪）把
     * blk6 帧 frame_base[effect]+i 以 rle_blit_transparent 贴到带内
     * 锚点 32904 + 24*(x-sx) + 456*24*(y-sy) - 2736（= 实体渲染锚
     * 30168 族）→ blit_rows 312×192 上屏。 */
    if (effect_id < 0 || effect_id >= 33)
        return;

    anim_tick_update(0);
    int frames = g_fx_field_frame_count[effect_id];
    int first_sfx = g_fx_field_sfx_first[effect_id];

    enum { FX_SHADOW_BYTES = 456 * 336 };
    uint8_t *shadow = battle_shadow_layer();
    uint8_t *backup = (uint8_t *)malloc(FX_SHADOW_BYTES);
    int have_snap = backup && field_shadow_snapshot(backup, FX_SHADOW_BYTES) == 0;
    const uint8_t *pkg6 = (const uint8_t *)g_pkg_fdother_6;

    for (int frame = 0; frame < frames; frame++) {
        if (have_snap)
            memcpy(shadow, backup, FX_SHADOW_BYTES);
        if (pkg6 && targets && g_ent_table) {
            const uint8_t *fr = package_frame_ptr(
                pkg6, g_fx_field_frame_base[effect_id] + frame);
            for (int j = 0; j < n_targets; j++) {
                if (targets[j] >= (uint8_t)g_ent_count)
                    continue;
                uint8_t *rec = g_ent_table[targets[j]];
                int x = rec[0], y = rec[1];
                if (x < (int)g_scroll_x - 1
                    || x > (int)g_view_w + (int)g_scroll_x
                    || y < (int)g_scroll_y - 1
                    || y > (int)g_view_h + (int)g_scroll_y + 1)
                    continue;
                rle_blit_transparent(
                    fr,
                    shadow + 32904 + 24 * (x - (int)g_scroll_x)
                        + (ptrdiff_t)456 * 24 * (y - (int)g_scroll_y) - 2736,
                    456);
            }
        }
        blit_rows(vram_base() + 1284, 320, shadow + 32904, 456, 312, 192);

        if (frame == 0 && first_sfx)
            sfx_play(g_magic_sfx_pkg, first_sfx, 1);

        if (effect_id == 22 && frame == 7)
            sfx_play(g_magic_sfx_pkg, 3, 1);
        else if (effect_id == 25 && (frame == 3 || frame == 6))
            sfx_play(g_magic_sfx_pkg, 5, 1);
        else if (effect_id == 18 && frame == 4)
            sfx_play(g_magic_sfx_pkg, 7, 1);
        else if (effect_id == 19 && (frame == 3 || frame == 6))
            sfx_play(g_magic_sfx_pkg, 8, 1);
        else if (effect_id == 8 && (frame == 3 || frame == 6))
            sfx_play(g_magic_sfx_pkg, 10, 1);
        else if (effect_id == 9 && (frame == 15 || frame == 19))
            sfx_play(g_magic_sfx_pkg, 15, 1);

        wait_bios_ticks(1);
    }
    free(backup);
    anim_tick_update(0);
}

void dmg_popup_queue(int dmg, int glyph_base, int ent)
{
    /* 0x1E0DB：4 字符十进制入队（目标需在视口内）：
     * g_popup_glyph/g_popup_x/g_popup_ent/g_popup_count。 */
    char digits[20];
    int digit_pos = 0;
    if (!g_ent_table || ent < 0 || ent >= g_ent_count
        || g_popup_count < 0 || g_popup_count + 4 > FD2_POPUP_CAPACITY)
        return;
    uint8_t *rec = g_ent_table[ent];
    int x = rec[0];
    int y = rec[1];

    if (x <= (int)g_scroll_x - 1 || x >= g_view_w + (int)g_scroll_x
        || y < (int)g_scroll_y - 1 || y > g_view_h + (int)g_scroll_y)
        return;

    sprintf(digits, "%d", dmg);
    for (int i = 0; i < 4; i++) {
        int slot = g_popup_count + i;
        g_popup_x[slot] = (uint8_t)(5 * i + 2);
        g_popup_ent[slot] = (uint8_t)ent;
        if ((int)strlen(digits) <= 3 - i)
            g_popup_glyph[slot] = 0;
        else
            g_popup_glyph[slot] = (uint8_t)(digits[digit_pos++] + glyph_base - '0');
    }
    g_popup_count += 4;
}

void miss_popup_queue(int ent)
{
    /* 0x1E1DC：MISS 四字形（g_miss_glyph@0x5204A）。 */
    static const uint8_t miss_glyph[4] = { 0x74, 0x75, 0x76, 0x76 };
    if (!g_ent_table || ent < 0 || ent >= g_ent_count
        || g_popup_count < 0 || g_popup_count + 4 > FD2_POPUP_CAPACITY)
        return;
    uint8_t *rec = g_ent_table[ent];
    int x = rec[0];
    int y = rec[1];

    if (x <= (int)g_scroll_x - 1 || x >= g_view_w + (int)g_scroll_x
        || y < (int)g_scroll_y - 1 || y > g_view_h + (int)g_scroll_y)
        return;

    for (int i = 0; i < 4; i++) {
        int slot = g_popup_count + i;
        g_popup_x[slot] = (uint8_t)(5 * i + (i == 1 ? 3 : 2));
        g_popup_ent[slot] = (uint8_t)ent;
        g_popup_glyph[slot] = miss_glyph[i];
    }
    g_popup_count += 4;
}

void popup_flush_float(void)
{
    /* 0x1DF58：22 帧弹字动画。逐帧把 death_fx(blk5) 字形盖到战斗视口带,
     * 垂直偏移取 0x5202C 起 25 字节波形表(索引 (j%4)+i),每帧
     * delay(2),末帧画面保持 delay(500)。原版不在本函数清队列
     * (g_popup_count 由下一次 dispatch 入口清零)。 */
    static const uint8_t wave[25] = {
        0x0F, 0x0F, 0x0F, 0x0F, 0x07, 0x03, 0x01, 0x00, 0x00, 0x01,
        0x03, 0x07, 0x0F, 0x0F, 0x0B, 0x09, 0x08, 0x08, 0x09, 0x0B,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    };

    if (g_popup_count <= 0)
        return;

    uint8_t *band = battle_shadow_layer() + 32904;
    const uint8_t *pkg5 = (const uint8_t *)g_death_fx_pkg;
    void *work[FD2_POPUP_CAPACITY];

    for (int i = 0; i < 22; i++) {
        int stamped = 0;
        for (int j = 0; j < g_popup_count && j < FD2_POPUP_CAPACITY; j++) {
            if (!g_popup_glyph[j] || !g_ent_table
                || g_popup_ent[j] >= (uint8_t)g_ent_count)
                continue;
            uint8_t *rec = g_ent_table[g_popup_ent[j]];
            int col = 24 * (rec[0] - (int)g_scroll_x) + g_popup_x[j];
            int row = 24 * (rec[1] - (int)g_scroll_y)
                    + (int)wave[(j % 4) + i] - 3;
            void *w = pkg_frame_blit(pkg5, band, 456, col, row,
                                     g_popup_glyph[j]);
            if (w)
                work[stamped++] = w;
        }
        blit_rows(vram_base() + 1284, 320, band, 456, 312, 192);
        delay_ms(2);
        while (stamped > 0)
            pkg_frame_free(band, 456, work[--stamped]);
    }
    delay_ms(500);
}

void fx_mass_strike(int actor, int effect, int n, const uint8_t *targets)
{
    /* 0x2111A: the fourth argument is the effect id (the dispatcher passes
     * weapon[param], while fx_scene_special passes 0x20).  IDA shows the
     * exact order below and a shared tail at 0x21190. */
    fx_field_anim(effect, n, targets);
    fx_unit_flash_alt(actor, effect, n, targets);
    for (int i = 0; i < n; i++) {
        int ent = targets[i];
        if (fx_hit_immune_test(ent, effect))
            dmg_popup_queue(ent, 94, ent);
        else
            miss_popup_queue(ent);
    }
    anim_tick_update(0);
    popup_flush_float();
}

void fx_mass_damage(int actor, int n, const uint8_t *targets, int dmg)
{
    /* 0x211A4: fixed animation 13, effect application, then the original
     * heal_apply helper (its return value is the unclamped random amount) for
     * each target.  The final jump lands in the 0x21190 animation tail. */
    fx_field_anim(13, n, targets);
    fx_effect_apply(actor, 13, n, targets);
    for (int i = 0; i < n; i++) {
        int ent = targets[i];
        int amount = heal_apply(ent, dmg);
        dmg_popup_queue(amount, 105, ent);
    }
    anim_tick_update(0);
    popup_flush_float();
}

void fx_drain_atk(int actor, int n, const uint8_t *targets)
{
    /* 0x22721：动画 17；ent[+34]=rand%4+2 标记；ATK += ATK*ratio+1 弹数(105)；
     * 玩家得 2×等级经验。效果 34 三连之一。 */
    fx_field_anim(17, n, targets);
    fx_effect_apply(actor, 17, n, targets);
    for (int i = 0; i < n; i++) {
        int ent = targets[i];
        uint8_t *rec = g_ent_table[ent];
        int level = rec[33];
        if (rec[32] > 8 && rec[32] < 0x19)
            level += 30;
        if (rec[34]) {
            miss_popup_queue(ent);
            continue;
        }
        rec[34] = (uint8_t)((int)fd2_rand() % 4 + 2);
        int amount = (int)lrint((double)*(uint16_t *)(rec + 72) * 0.15 + 1.0);
        dmg_popup_queue(amount, 105, ent);
        *(uint16_t *)(rec + 72) = (uint16_t)(*(uint16_t *)(rec + 72) + amount);
        g_exp_gained += 2 * level;
    }
    anim_tick_update(0);
    popup_flush_float();
}

void fx_drain_def(int actor, int n, const uint8_t *targets)
{
    /* 0x22866：动画 18；ent[+35] 标记；DEF 增幅。效果 34 三连之二。 */
    fx_field_anim(18, n, targets);
    fx_effect_apply(actor, 18, n, targets);
    for (int i = 0; i < n; i++) {
        int ent = targets[i];
        uint8_t *rec = g_ent_table[ent];
        int level = rec[33];
        if (rec[32] > 8 && rec[32] < 0x19)
            level += 30;
        if (rec[35]) {
            miss_popup_queue(ent);
            continue;
        }
        rec[35] = (uint8_t)((int)fd2_rand() % 4 + 2);
        int amount = (int)lrint((double)*(uint16_t *)(rec + 74) * 0.15 + 1.0);
        dmg_popup_queue(amount, 105, ent);
        *(uint16_t *)(rec + 74) = (uint16_t)(*(uint16_t *)(rec + 74) + amount);
        g_exp_gained += 2 * level;
    }
    anim_tick_update(0);
    popup_flush_float();
}

void fx_drain_third(int actor, int n, const uint8_t *targets)
{
    /* 0x22997：第三属性变体（效果 34 三连之三）。 */
    fx_field_anim(19, n, targets);
    fx_effect_apply(actor, 19, n, targets);
    for (int i = 0; i < n; i++) {
        int ent = targets[i];
        uint8_t *rec = g_ent_table[ent];
        int level = rec[33];
        if (rec[32] > 8 && rec[32] < 0x19)
            level += 30;
        if (rec[36]) {
            miss_popup_queue(ent);
            continue;
        }
        int status_roll = (int)fd2_rand();
        rec[36] = (uint8_t)(status_roll % 4 + 2);
        *(uint16_t *)(rec + 76) = (uint16_t)(*(uint16_t *)(rec + 76) + 15);
        *(uint16_t *)(rec + 78) = (uint16_t)(*(uint16_t *)(rec + 78) + 15);
        /* 0x22A59 pushes the literal 15, independent of the status roll. */
        dmg_popup_queue(15, 105, ent);
        g_exp_gained += 2 * level;
    }
    anim_tick_update(0);
    popup_flush_float();
}

void fx_aoe_status(int actor, int anim, int n, const uint8_t *targets, int status_off)
{
    /* 0x22D1B：跳过已状态化/职业25/26；50% 命中；10 伤+ent[status_off]=rand%4+2；
     * 8×等级经验。效果 35 三调：anim 26/22/27 → off 37/39/38。 */
    fx_field_anim(anim, n, targets);
    fx_effect_apply(actor, anim, n, targets);
    for (int i = 0; i < n; i++) {
        int ent = targets[i];
        uint8_t *rec = g_ent_table[ent];
        if (rec[status_off] || rec[32] == 25 || rec[32] == 26) {
            miss_popup_queue(ent);
            continue;
        }
        if ((int)fd2_rand() % 100 >= 50) {
            miss_popup_queue(ent);
            continue;
        }
        int amount = fx_damage_adjust(ent, 10);
        dmg_popup_queue(amount, 94, ent);
        rec[status_off] = (uint8_t)((int)fd2_rand() % 4 + 2);
        g_exp_gained += 8 * rec[33];
    }
    anim_tick_update(0);
    if (g_popup_count)
        popup_flush_float();
}

/* 0x21082（类型 8/9/A/11/12/13）：永久属性提升道具——ent[field_off](u16) +=
 * boost（字段 0x37/39/3E/42/46 = §10.3 五项成长属性；0x3B 组合型保留 exp）→
 * 弹数 94 → ent_recompute_derived → 消耗道具。 */
void fx_stat_boost(int actor, int boost, int item, int n, const uint8_t *targets,
                   int target0, int anim, int field_off)
{
    /* IDA 0x21082 updates only the first selected target, displays the raw
     * boost, recomputes derived stats, then removes the used inventory slot. */
    fx_field_anim(anim, n, targets);
    fx_effect_apply(actor, anim, n, targets);
    uint8_t *rec = g_ent_table[target0];
    uint16_t old = *(uint16_t *)(rec + field_off);
    *(uint16_t *)(rec + field_off) = (uint16_t)(old + boost);
    dmg_popup_queue(boost, 94, target0);
    anim_tick_update(0);
    popup_flush_float();
    ent_recompute_derived(target0);
    inventory_remove_item(actor, item);
}

/* 0x1C9DD（类型 0xB）：MP(+0x44) += 9*power/10 + power*(rand%100)/1000，
 * clamp MaxMP(+0x46)。（2026-09-04 实现，公式逆向自原版） */
int mp_restore(int target, int power)
{
    uint8_t *rec = g_ent_table[target];
    uint16_t mp = *(uint16_t *)(rec + 68);
    uint16_t max_mp = *(uint16_t *)(rec + 70);

    int amount = power * (int)(fd2_rand() % 100) / 1000 + 9 * power / 10;
    int healed = amount + mp;
    if (healed > max_mp)
        healed = max_mp;
    *(uint16_t *)(rec + 68) = (uint16_t)healed;

    /* Native 0x1CA58..0x1C9CF awards MP-restoration experience for player
     * icons (< 0x4B): restored * level * 40 / MaxMP.  Unlike heal_apply,
     * this path does not add the class-family +30 level adjustment. */
    if (rec[7] < 0x4B)
        g_exp_gained += (healed - mp) * rec[33] * 40 / max_mp;
    return amount;
}

/* 0x1C916 heal_apply（fx_amount_compute 0x1C8ED 的实体，治疗类结算）：
 * HP(+0x40) += 9*power/10 + power*(rand%100)/1000，clamp MaxHP(+0x42)；
 * 玩家方（职业<0x4B）得经验 = 治疗量×40×(等级+30 若职业系 9..24)/MaxHP。 */
int heal_apply(int target, int power)
{
    uint8_t *rec = g_ent_table[target];
    uint16_t hp = *(uint16_t *)(rec + 64);
    uint16_t max_hp = *(uint16_t *)(rec + 66);

    int amount = power * (int)(fd2_rand() % 100) / 1000 + 9 * power / 10;
    int healed = amount + hp;
    if (healed > max_hp)
        healed = max_hp;
    *(uint16_t *)(rec + 64) = (uint16_t)healed;

    if (rec[7] < 0x4B) {
        int lv = rec[33];
        if (rec[32] > 8 && rec[32] < 0x19)
            lv += 30;
        g_exp_gained += (healed - hp) * 40 * lv / max_hp;
    }
    return amount;
}

/* 0x1C81F fx_damage_adjust：实际伤害 = raw×(rand%100)/1000 + 9×raw/10
 * （0.9~1.0 倍随机），HP -= 伤害 clamp 0；敌方（职业>=0x44，敌表=职业-68）
 * 经验 = 等级×敌表[9]，未死再乘 伤害/MaxHP 受伤比例 → g_exp_gained。
 * 返回实际伤害量（对齐 duel.h 声明；原版尾跳无返回值语义）。 */
int fx_damage_adjust(int target, int raw)
{
    uint8_t *rec = g_ent_table[target];
    uint16_t hp = *(uint16_t *)(rec + 64);
    uint16_t max_hp = *(uint16_t *)(rec + 66);

    int dmg = raw * (int)(fd2_rand() % 100) / 1000 + 9 * raw / 10;
    int left = hp - dmg;
    if (left < 0)
        left = 0;
    *(uint16_t *)(rec + 64) = (uint16_t)left;

    if (rec[7] >= 0x44) {
        int exp = rec[33] * enemy_base_table_entry(rec[7] - 68)[9];
        if (left)
            exp = dmg * exp / max_hp;
        g_exp_gained += exp;
    }
    return dmg;
}

/* ---- 立绘闪烁族共用(0x1CB94 宿主等价)：锚点/帧取用/场景合成 ----
 * 实体带内锚点 = 32904 + 24*(x-sx) + 456*24*(y-sy) - 2736；立绘帧 =
 * 12*slot + (cursor_phase==3 ? 2 : cursor_phase)(0x1CD17/0x1CB94 原式)。 */
static uint8_t *unit_band_anchor(uint8_t *shadow, uint8_t *rec)
{
    return shadow + 32904
         + 24 * (rec[0] - (int)g_scroll_x)
         + (ptrdiff_t)456 * 24 * (rec[1] - (int)g_scroll_y) - 2736;
}

static const uint8_t *unit_standing_frame(uint8_t *rec)
{
    const uint8_t *stream = NULL;
    size_t len = 0;
    int phase = field_cursor_phase();

    if (icon_frame_get(rec[2], phase == 3 ? 2 : phase, &stream, &len) != 0)
        return NULL;
    return stream;
}

static int unit_in_viewport(uint8_t *rec)
{
    return rec[0] >= (int)g_scroll_x - 1
        && rec[0] <= (int)g_view_w + (int)g_scroll_x
        && rec[1] >= (int)g_scroll_y - 1
        && rec[1] <= (int)g_view_h + (int)g_scroll_y + 1;
}

/* 0x1CB94：视口重渲 → 目标盖 blk6 帧 fxframe，其余单位画立绘。 */
static void flash_scene_compose(uint8_t *buf, int fxframe, int n,
                                const uint8_t *targets)
{
    const uint8_t *pkg6 = (const uint8_t *)g_pkg_fdother_6;

    field_render_viewport_ext(buf + 32904, 456, 13, 8,
                              (int)g_scroll_x, (int)g_scroll_y);
    if (!g_ent_table || !pkg6)
        return;
    const uint8_t *fx = package_frame_ptr(pkg6, fxframe);
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        if ((rec[5] & 1u) || !unit_in_viewport(rec))
            continue;
        uint8_t *dst = unit_band_anchor(buf, rec);
        int hit = 0;
        for (int j = 0; j < n && targets; j++)
            if (targets[j] == i)
                hit = 1;
        if (hit) {
            rle_blit_transparent(fx, dst, 456);
        } else {
            const uint8_t *stream = unit_standing_frame(rec);
            if (stream)
                sprite24_stamp_transparent(stream, dst, 456);
        }
    }
}

/* 0x1CD17（类型 0x14/0x18）：目标 10 帧闪烁。每帧恢复净场 → 目标立绘以
 * glyph[effect] 基色 + (7-i%8) 亮度做八级调色坡重画(0x4DF84)→
 * 上屏 1 tick；末帧回净场。glyph 表 @0x5200A(26 字节)。 */
void fx_unit_flash(int effect, int n, const uint8_t *targets)
{
    static const uint8_t s_flash_glyph[26] = {
        0x08, 0x08, 0x08, 0x08, 0xC8, 0x08, 0x08, 0x08, 0x08, 0x08,
        0x10, 0x10, 0x10, 0x10, 0x08, 0x08, 0x10, 0x10, 0x10, 0x08,
        0x08, 0x10, 0x10, 0x10, 0x08, 0x08,
    };

    enum { FX_SHADOW_BYTES = 456 * 336 };
    uint8_t *shadow = battle_shadow_layer();
    uint8_t *backup = (uint8_t *)malloc(FX_SHADOW_BYTES);
    int have_snap = backup && field_shadow_snapshot(backup, FX_SHADOW_BYTES) == 0;
    uint8_t base = s_flash_glyph[effect > 25 ? 25 : effect];

    for (int i = 0; i < 10; i++) {
        if (have_snap)
            memcpy(shadow, backup, FX_SHADOW_BYTES);
        if (g_ent_table && targets)
            for (int j = 0; j < n; j++) {
                if (targets[j] >= (uint8_t)g_ent_count)
                    continue;
                uint8_t *rec = g_ent_table[targets[j]];
                if (!unit_in_viewport(rec))
                    continue;
                const uint8_t *stream = unit_standing_frame(rec);
                if (stream)
                    sprite24_stamp_flash(stream,
                                         unit_band_anchor(shadow, rec),
                                         456, base, 7 - i % 8);
            }
        blit_rows(vram_base() + 1284, 320, shadow + 32904, 456, 312, 192);
        wait_bios_ticks(1);
    }
    if (have_snap)
        blit_rows(vram_base() + 1284, 320, backup + 32904, 456, 312, 192);
    free(backup);
}

/* 0x2218A（类型 0x17）：单体选定打击。两调 cutscene_walk_step：
 * ①(ent,-1,-1,tx,ty) 目标位演出后实体置 (-1,-1) 离场；②滚回
 * 0x51CF9/FD(传送目的)后 (ent,wx,wy,wx,wy) 落位演出，实体落在
 * 该点。宿主以 battle_cutscene_stand(0x22253 from==to 等价)接线。 */
void fx_targeted_strike(int actor, int n, const uint8_t *targets)
{
    if (!targets || !g_ent_table || targets[0] >= (uint8_t)g_ent_count)
        return;

    int target = targets[0];
    camera_focus_ent(target);
    fx_field_reset(actor, 23);

    uint8_t *rec = g_ent_table[target];
    int level = rec[33];
    if (rec[32] > 8 && rec[32] < 0x19)
        level += 30;
    g_exp_gained += 10 * level;

    int tx = rec[0], ty = rec[1];
    battle_cutscene_stand(target, tx, ty);
    rec[0] = 0xFF;
    rec[1] = 0xFF;

    g_text_busy = 0;
    cursor_scroll_to(g_warp_dest_x, g_warp_dest_y);
    g_text_busy = 1;
    battle_cutscene_stand(target, g_warp_dest_x, g_warp_dest_y);
}

/* ---- 剩余法术壳（§14，2026-09-04；均 fx_field_anim + 弹字尾 0x21190） ---- */

/* 0x21227 壳A：场地动画 + fx_unit_flash + 逐目标免疫→94 伤害/MISS。 */
void fx_aoe_damage_flash(int actor, int effect, int n, const uint8_t *targets)
{
    g_popup_count = 0;
    fx_field_anim(effect, n, targets);
    fx_unit_flash(effect, n, targets);
    fx_field_reset(actor, effect);
    for (int i = 0; i < n; i++) {
        int ent = targets[i];
        int amount = fx_hit_immune_test(ent, effect);
        if (amount)
            dmg_popup_queue(amount, 94, ent);
        else
            miss_popup_queue(ent);
    }
    anim_tick_update(0);
    popup_flush_float();
}

/* 0x213B7 壳B：同 A，fx_unit_flash_alt 替代 flash（视觉变体）。 */
void fx_aoe_damage_alt(int actor, int effect, int n, const uint8_t *targets)
{
    g_popup_count = 0;
    fx_field_anim(effect, n, targets);
    fx_unit_flash_alt(actor, effect, n, targets);
    fx_field_reset(actor, effect);
    for (int i = 0; i < n; i++) {
        int ent = targets[i];
        int amount = fx_hit_immune_test(ent, effect);
        if (amount)
            dmg_popup_queue(amount, 94, ent);
        else
            miss_popup_queue(ent);
    }
    anim_tick_update(0);
    popup_flush_float();
}

/* 0x214AD：单体场地打击（effect 固定 9），内联弹字尾。 */
void fx_single_field_strike(int actor, int effect, const uint8_t *target)
{
    g_popup_count = 0;
    fx_field_anim(effect, 1, target);
    fx_field_reset(actor, effect);
    int ent = target[0];
    int amount = fx_hit_immune_test(ent, effect);
    if (amount)
        dmg_popup_queue(amount, 94, ent);
    else
        miss_popup_queue(ent);
    anim_tick_update(0);
    popup_flush_float();
}

/* 0x21548 地震术（串串 "Out of memory at Earth Quack !!"）：
 * 全图 tile 指针表 + 3 遍整体位移渲染（ent[+5]bit0 阵亡跳过）+
 * 60 帧 4 缓冲抖屏（每 6 帧 g_magic_sfx_pkg）→ 标准 免疫/伤害/MISS。 */
/* 0x1F558 宿主等价：定点采样缩放。源 = scratch 带(15x10 tile，
 * 起于 (scroll-1))，输出 312x192 逐像素采样，步进 step(1/128px
 * 定点)，中心 (x0_fp,y0_fp)，图外为黑。 */
static void quake_sample_view(uint8_t *dst_band, const uint8_t *src_band,
                              int x0_fp, int y0_fp, int step)
{
    int org_x = 24 * ((int)g_scroll_x - 1);
    int org_y = 24 * ((int)g_scroll_y - 1);

    for (int i = 0; i < 192; i++) {
        int sy = ((y0_fp - 96 * step + i * step) >> 7) - org_y;
        uint8_t *drow = dst_band + 456 * i;
        const uint8_t *srow;

        if (sy < 0 || sy >= 240) {
            memset(drow, 0, 312);
            continue;
        }
        srow = src_band + 456 * sy;
        int fx = x0_fp - 156 * step;
        for (int j = 0; j < 312; j++) {
            int sx = (fx >> 7) - org_x;
            drow[j] = (sx >= 0 && sx < 360) ? srow[sx] : 0;
            fx += step;
        }
    }
}

void fx_earthquake(int actor, int effect, int n, const uint8_t *targets)
{
    /* 0x21548 全量：预合成三幅场景——tile 层以定点步进 131/128/125
     * 缩放(表 0x52096：x={128,0,-128} y={128,0,128} step={131,128,125}，
     * 即中心 ±1px 抖 + 微缩放"呼吸")，实体不缩放叠画；60 帧按
     * n%4={缩出,正常,缩入,正常}(第 4 缓冲 = 第 2 缓冲，0x21607)
     * 交替上屏，每帧 delay(10)，帧 0,6..42 播 sfx 13。原版三缓冲
     * = live shadow + 两 malloc；分配失败退回纯时序。 */
    static const int dx_fp[3] = { 128, 0, -128 };
    static const int dy_fp[3] = { 128, 0, 128 };
    static const int step_fp[3] = { 131, 128, 125 };

    g_popup_count = 0;
    fx_field_reset(actor, effect);

    enum { QUAKE_SHADOW_BYTES = 456 * 336 };
    uint8_t *live = battle_shadow_layer();
    uint8_t *scratch = (uint8_t *)malloc(QUAKE_SHADOW_BYTES);
    uint8_t *buf_b = (uint8_t *)malloc(QUAKE_SHADOW_BYTES);
    uint8_t *buf_c = (uint8_t *)malloc(QUAKE_SHADOW_BYTES);
    uint8_t *bufs[3] = { live, buf_b, buf_c };
    int composed = scratch && buf_b && buf_c;

    if (composed) {
        memset(scratch + 32904, 0, 456 * 240);
        field_render_viewport_ext(scratch + 32904, 456, 15, 10,
                                  (int)g_scroll_x - 1, (int)g_scroll_y - 1);
        for (int k = 0; k < 3; k++) {
            int x0 = 3072 * (int)g_scroll_x + 1536 * (int)g_view_w + dx_fp[k];
            int y0 = 3072 * (int)g_scroll_y + 1536 * (int)g_view_h + dy_fp[k];

            quake_sample_view(bufs[k] + 32904, scratch + 32904,
                              x0, y0, step_fp[k]);
            field_render_entities(bufs[k]);
        }
    }
    for (int frame = 0; frame < 60; frame++) {
        if (frame < 43 && (frame % 6) == 0)
            sfx_play(g_magic_sfx_pkg, 13, 1);
        if (composed) {
            int k = frame % 4;

            if (k == 3)
                k = 1;
            blit_rows(vram_base() + 1284, 320, bufs[k] + 32904,
                      456, 312, 192);
        }
        delay_ms(10);
    }
    free(scratch);
    free(buf_b);
    free(buf_c);
    for (int i = 0; i < n; i++) {
        int ent = targets[i];
        int amount = fx_hit_immune_test(ent, effect);
        if (amount)
            dmg_popup_queue(amount, 94, ent);
        else
            miss_popup_queue(ent);
    }
    anim_tick_update(0);
    popup_flush_float();
}

/* 0x21B18 群体固定量效果（治疗/吸收类）：fx_effect_apply +
 * fx_amount_compute → 弹字 105；无免疫/MISS，必定生效。 */
void fx_aoe_fixed_amount(int actor, int effect, int n, const uint8_t *targets)
{
    g_popup_count = 0;
    fx_field_anim(effect, n, targets);
    fx_effect_apply(actor, effect, n, targets);
    fx_field_reset(actor, effect);
    for (int i = 0; i < n; i++)
        dmg_popup_queue(fx_amount_compute(targets[i], effect), 105, targets[i]);
    /* 0x21B90 JUMPOUT 0x21190 共享尾：anim_tick_update(0) 先重渲 live
     * 层——清掉 fx_effect_apply 盖入的色 200 剪影，浮字阶段才不显
     * 白精灵（缺失即治疗术最后一屏全白剪影的根因）。 */
    anim_tick_update(0);
    popup_flush_float();
}

/* 0x22C04 圣光/退魔（effect 25）：ent[+5]bit7 不死系标记——
 * 清位→MISS；置位→清除摧毁 + g_exp_gained += 8×lv(+30 若职业系 9..24)。 */
void fx_exorcism_undead(int actor, int effect, int n, const uint8_t *targets)
{
    g_popup_count = 0;
    fx_field_reset(actor, effect);
    fx_field_anim(effect, n, targets);
    fx_effect_apply(actor, effect, n, targets);
    for (int i = 0; i < n; i++) {
        int ent = targets[i];
        uint8_t *rec = g_ent_table[ent];
        if ((int8_t)rec[5] >= 0) {
            miss_popup_queue(ent);
            continue;
        }
        rec[5] &= (uint8_t)0x7F;
        int level = rec[33];
        if (rec[32] > 8 && rec[32] < 0x19)
            level += 30;
        g_exp_gained += 8 * level;
    }
    anim_tick_update(0);
    if (g_popup_count)
        popup_flush_float();
}

/* 共享效果原语 */
void fx_effect_apply(int actor, int effect, int n, const uint8_t *targets)
{
    (void)actor; (void)effect;
    /* 0x1C309：样本 1 → 目标立绘以固定色 200（原版 = pitch 456 低字节）
     * 剪影盖到 live shadow → 5 轮"净场/剪影场"各 1 tick 交替 → 末帧
     * 回净场。 */
    sfx_play(g_magic_sfx_pkg, 1, 1);

    enum { FX_SHADOW_BYTES = 456 * 336 };
    uint8_t *shadow = battle_shadow_layer();
    uint8_t *backup = (uint8_t *)malloc(FX_SHADOW_BYTES);
    int have_snap = backup && field_shadow_snapshot(backup, FX_SHADOW_BYTES) == 0;

    if (g_ent_table && targets)
        for (int i = 0; i < n; i++) {
            if (targets[i] >= (uint8_t)g_ent_count)
                continue;
            uint8_t *rec = g_ent_table[targets[i]];
            if (!unit_in_viewport(rec))
                continue;
            const uint8_t *stream = unit_standing_frame(rec);
            if (stream)
                sprite24_stamp_silhouette(stream,
                                          unit_band_anchor(shadow, rec),
                                          456, 200);
        }

    if (have_snap) {
        for (int j = 0; j < 5; j++) {
            blit_rows(vram_base() + 1284, 320, backup + 32904,
                      456, 312, 192);
            wait_bios_ticks(1);
            blit_rows(vram_base() + 1284, 320, shadow + 32904,
                      456, 312, 192);
            wait_bios_ticks(1);
        }
        blit_rows(vram_base() + 1284, 320, backup + 32904, 456, 312, 192);
    }
    free(backup);
}
int fx_amount_compute(int target, int effect)
{
    const uint8_t *fx = effect_param_entry(effect);
    return heal_apply(target, (int16_t)(fx[0] | ((uint16_t)fx[1] << 8)));
}
void fx_field_reset(int actor, int effect)
{
    const uint8_t *fx = effect_param_entry(effect);
    uint16_t *mp = (uint16_t *)(g_ent_table[actor] + 68);
    *mp = (uint16_t)(*mp - fx[5]);                                  /* 0x1CA89 */
}
void fx_unit_flash_alt(int actor, int effect, int n, const uint8_t *targets)
{
    (void)actor; (void)effect;
    /* 0x1CAC7：live shadow 合成"目标盖 blk6 帧 74"场景，另备缓冲合成
     * 帧 75 场景，4 轮各 90ms 交替上屏；收尾 anim_tick 重建净场。 */
    enum { FX_SHADOW_BYTES = 456 * 336 };
    uint8_t *shadow = battle_shadow_layer();
    uint8_t *alt = (uint8_t *)malloc(FX_SHADOW_BYTES);

    flash_scene_compose(shadow, 74, n, targets);
    if (alt)
        flash_scene_compose(alt, 75, n, targets);
    for (int i = 0; i < 4; i++) {
        blit_rows(vram_base() + 1284, 320, shadow + 32904, 456, 312, 192);
        delay_ms(90);
        if (alt)
            blit_rows(vram_base() + 1284, 320, alt + 32904, 456, 312, 192);
        delay_ms(90);
    }
    free(alt);
    anim_tick_update(0);
}

/* ==== 法术 exec 表（g_spell_exec_funcs@0x51D01，38 项 9..27 段全解，
 * 2026-09-08 §13.39）=================================================== */

/* 0x1D6C8 fx_palette_flash(spell)：施法前闪屏——sfx(magic_sfx_pkg#0) +
 * 4 拍"色 0 = 表 RGB ↔ 黑"（VGA 3C8/3C9 直写；三表 @0x51AAD/51AD1/
 * 51AF5 按 spell 索引，dump 40B/表）。fd2re 以 g_palette_ptr[0..2]
 * 临时改写 + palette_apply_range(0,0) 等价。 */
static const uint8_t s_flash_rgb[3][40] = {
    {0x3F,0x3F,0x3F,0x3F,0x2B,0x2B,0x2B,0x2B,0x3F,0x23,0x2E,0x2E,0x2E,0x3F,0x3F,
     0x3F,0x3F,0x32,0x32,0x32,0x3F,0x3F,0x23,0x1E,0x00,0x3F,0x0A,0x23,0x3F,0x3F,
     0x3F,0x3F,0x2B,0x3F,0x3F,0x2B,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x32,0x32,0x32,0x32,0x3F,0x10,0x28,0x28,0x28,0x3D,0x3D,
     0x28,0x28,0x32,0x32,0x32,0x28,0x28,0x00,0x2A,0x00,0x3D,0x1F,0x19,0x3F,0x3F,
     0x3F,0x3F,0x32,0x3F,0x00,0x32,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x3C,0x3C,0x3C,0x3C,0x3F,0x08,0x1E,0x1E,0x1E,0x2E,0x2E,
     0x1E,0x1E,0x32,0x32,0x32,0x1E,0x1E,0x00,0x23,0x00,0x2E,0x00,0x00,0x3F,0x3F,
     0x3F,0x3F,0x3C,0x3F,0x00,0x3C,0xB4,0x05,0x02,0x00},
};

void fx_palette_flash(int spell)
{
    uint8_t *pal = (uint8_t *)g_palette_ptr;

    sfx_play(g_magic_sfx_pkg, 0, 1);
    if (!pal)
        return;
    uint8_t keep[3] = { pal[0], pal[1], pal[2] };
    for (int i = 0; i < 4; i++) {
        pal[0] = s_flash_rgb[0][spell];
        pal[1] = s_flash_rgb[1][spell];
        pal[2] = s_flash_rgb[2][spell];
        palette_apply_range(0, 0, 0);
        wait_bios_ticks(1);
        pal[0] = pal[1] = pal[2] = 0;
        palette_apply_range(0, 0, 0);
        wait_bios_ticks(1);
    }
    pal[0] = keep[0]; pal[1] = keep[1]; pal[2] = keep[2];
    palette_apply_range(0, 0, 0);
}

/* 0x51D01 表 9..27 段逐项定参薄包装（disasm 定案 2026-09-08；当轮
 * 逐指令复核全表 28 项与 fd2re 逐分支一致，仅 case 10 旧多弹 sfx(2)
 * 已勘误删除）：
 *  9 =0x214AD 单体雷击（fd2re fx_single_field_strike 重建体，含扣
 *     效果 9 MP/命中 94/MISS/浮字）
 * 10 =0x21527 fx_earthquake(10) 直调，无前奏音效
 * 11 =sfx2 + cast_beam_rise(unit,15,10) + fx_earthquake(11)
 * 12 =sfx2 + cast_beam_rise(unit,30,16) + fx_earthquake(12)
 * 13..16/24 =sfxB + 蓄力动画 cast_charge_anim(r0,dr) +
 *     fx_aoe_fixed_amount (13:(1,2) 14:(2,4) 15:(8,4) 16:(6,6) 24≡16)
 * 17/18/19 =popup0 + fx_field_reset(actor,18/18/19) + fx_drain_atk/
 *     def/third
 * 20/21 =sub_22AA8：popup0 + fx_field_reset + battle_award_exp_list
 *     (状态 37/38) + popup_flush
 * 22/26/27 =sub_22CDA：popup0 + fx_field_reset + fx_aoe_status
 *     (22→39, 26→37, 27→38)
 * 23 =fx_targeted_strike；25 =fx_exorcism_undead
 * cast_beam_rise（0x2189A 半径语义）与 cast_charge_anim（0x21EB1）
 * 已随 2026-09-08 半径语义定案全量接线（duel.c 真管线）。 */
void spell_execute(int actor, int spell, int n, const uint8_t *targets)
{
    switch (spell) {
    /* 0..8 = ICF 折叠薄包装（2026-09-08 表界勘误后接线）：表项
     * [0..3][8] 跳 0x21206 尾（fx_aoe_damage_flash 族），[4..7] 跳
     * 0x21396 尾（fx_aoe_damage_alt 族）。g_spell_exec_funcs 实为
     * 28 项——0x51D71 起是状态 exit 表（旧"40 项"读越界）。 */
    case 0: case 1: case 2: case 3: case 8:
        fx_aoe_damage_flash(actor, spell, n, targets);
        break;
    case 4: case 5: case 6: case 7:
        fx_aoe_damage_alt(actor, spell, n, targets);
        break;
    case 9:
        fx_single_field_strike(actor, 9, targets);
        break;
    case 10:
        /* 0x21527 spell10_earthquake：直调 fx_earthquake(10)，无前奏
         * 音效（sfx(2) 只在 11/12 的包装里；quake 自带抖屏 sfx13）。
         * 2026-09-08 逐指令复核：旧版多弹的 sfx(2) 已删。 */
        fx_earthquake(actor, 10, n, targets);
        break;
    case 11: case 12: {
        /* 0x2185F/0x21A9E：sfx → 光晕前摇 → 地震结算。 */
        uint8_t *rec = g_ent_table[actor];
        sfx_play(g_magic_sfx_pkg, 2, 1);
        cast_beam_rise(24 * ((int)rec[0] - (int)g_scroll_x) + 12,
                       24 * ((int)rec[1] - (int)g_scroll_y) + 18,
                       spell == 11 ? 15 : 30, spell == 11 ? 10 : 16);
        fx_earthquake(actor, spell, n, targets);
        break;
    }
    case 13: case 14: case 15: case 16: case 24: {
        /* 0x21AD9/0x21B99/0x2211C/0x22153：蓄力动画实参
         * 13:(1,2) 14:(2,4) 15:(8,4) 16/24:(6,6)（15 的 push 序
         * 4;8 → (8,4)，旧注 (4,8) 已勘误）。 */
        static const int charge_r0[] = { 1, 2, 8, 6, 6 };
        static const int charge_dr[] = { 2, 4, 4, 6, 6 };
        int ci = (spell == 24) ? 3 : spell - 13;
        sfx_play(g_magic_sfx_pkg, 0xB, 1);
        cast_charge_anim(charge_r0[ci], charge_dr[ci]);
        fx_aoe_fixed_amount(actor, spell == 24 ? 16 : spell, n, targets);
        break;
    }
    case 17:
        g_popup_count = 0;
        fx_field_reset(actor, 18);
        fx_drain_atk(actor, n, targets);
        break;
    case 18:
        g_popup_count = 0;
        fx_field_reset(actor, 18);
        fx_drain_def(actor, n, targets);
        break;
    case 19:
        g_popup_count = 0;
        fx_field_reset(actor, 19);
        fx_drain_third(actor, n, targets);
        break;
    case 20: case 21:
        g_popup_count = 0;
        fx_field_reset(actor, spell);
        battle_award_exp_list(targets, n, spell == 20 ? 37 : 38);
        if (g_popup_count)
            popup_flush_float();
        break;
    case 22: case 26: case 27:
        g_popup_count = 0;
        fx_field_reset(actor, spell);
        fx_aoe_status(actor, spell, n, targets,
                      spell == 22 ? 39 : (spell == 26 ? 37 : 38));
        break;
    case 23:
        fx_targeted_strike(actor, n, targets);
        break;
    case 25:
        fx_exorcism_undead(actor, 25, n, targets);
        break;
    default:
        break;                                   /* 范围外由调用方分流 */
    }
}
