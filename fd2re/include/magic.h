/* magic.h — 魔法/大招效果执行层（IDA 0x1C000-0x1E000 工具层 + 0x21000-0x22000 实现）
 * 证据与结构见 docs/architecture.md §14（2026-09-04）。
 * 入口：magic_effect_dispatch@0x20C6F（battle_act_menu 魔法项，effect 5..0x18）
 * 与 fx_scene_special@0x2D80D（大招 32-35，见 duel.h）共用本层。
 */
#ifndef FD2RE_MAGIC_H
#define FD2RE_MAGIC_H

#include "fd2.h"

void magic_effect_dispatch(int actor, int unit, int slot, int n_targets,
                           const uint8_t *targets);      /* 0x20C6F：分发键 =
                           weapon_table[item][13]，参数 = [14..15] u16 */
void fx_magic_sfx_load(void);                             /* 0x1D4CB: FDOTHER blk80 */
void fx_magic_sfx_stop(void);                             /* 0x1D4F6: stop current SFX */

/* 共享工具层（§14.1） */
void fx_field_anim(int effect_id, int n_targets, const uint8_t *targets); /* 0x1C4CC */
void dmg_popup_queue(int dmg, int glyph_base, int ent);  /* 0x1E0DB */
void miss_popup_queue(int ent);                          /* 0x1E1DC */
void popup_flush_float(void);                            /* 0x1DF58 */

/* IDA 0x53C6C/0x53D34/0x53DFC/0x53EC4：弹字队列，三块 200B 平行数组。
 * 每次命中或 MISS 占四槽；绘制器消费其字形、横向偏移和目标实体索引。 */
#define FD2_POPUP_CAPACITY 200
extern uint8_t g_popup_glyph[FD2_POPUP_CAPACITY];
extern uint8_t g_popup_x[FD2_POPUP_CAPACITY];
extern uint8_t g_popup_ent[FD2_POPUP_CAPACITY];
extern int32_t g_popup_count;

/* 效果实现（§14.2/§14.5，分发键 = g_weapon_table[13]，参数 = [14..15] u16） */
void fx_mass_strike(int actor, int effect, int n,
                    const uint8_t *targets);                         /* 0x2111A 类型0x15 */
void fx_mass_damage(int actor, int n, const uint8_t *targets, int dmg); /* 0x211A4 类型5/0xD */
void fx_drain_atk(int actor, int n, const uint8_t *targets);        /* 0x22721 类型0x10 */
void fx_drain_def(int actor, int n, const uint8_t *targets);        /* 0x22866 类型0xF */
void fx_drain_third(int actor, int n, const uint8_t *targets);      /* 0x22997 类型0xC */
void fx_aoe_status(int actor, int anim, int n, const uint8_t *targets,
                   int status_off);                       /* 0x22D1B 类型0xE/0x16 */
void fx_stat_boost(int actor, int boost, int item, int n,
                   const uint8_t *targets, int target0,
                   int anim, int field_off);              /* 0x21082 类型8/9/A/11/12/13 永久属性提升 */
int  mp_restore(int target, int power);                   /* 0x1C9DD 类型0xB MP 药；返回未截断恢复量 */
void fx_unit_flash(int effect, int n, const uint8_t *targets); /* 0x1CD17 类型0x14/0x18 */
void fx_targeted_strike(int actor, int n, const uint8_t *targets); /* 0x2218A 类型0x17 */

/* 剩余法术壳（§14，2026-09-04 展开；均走 fx_field_anim + 弹字尾 0x21190） */
void fx_aoe_damage_flash(int actor, int effect, int n,
                         const uint8_t *targets);         /* 0x21227 壳A：+unit_flash+免疫判定 */
void fx_aoe_damage_alt(int actor, int effect, int n,
                       const uint8_t *targets);           /* 0x213B7 壳B：flash 变体 1CAC7 */
void fx_single_field_strike(int actor, int effect,
                            const uint8_t *target);       /* 0x214AD 单体（effect 固定 9） */
void fx_earthquake(int actor, int effect, int n,
                   const uint8_t *targets);               /* 0x21548 地震（Earth Quack 串串） */
void fx_aoe_fixed_amount(int actor, int effect, int n,
                         const uint8_t *targets);         /* 0x21B18 固定量（弹字105，必中） */
void fx_exorcism_undead(int actor, int effect, int n,
                        const uint8_t *targets);          /* 0x22C04 圣光：ent[+5]bit7 不死
                                                              即死，exp=8×(lv+30 若系9..24) */

/* 共享效果原语（壳内使用） */
void fx_effect_apply(int actor, int effect, int n, const uint8_t *targets); /* 0x1C2DA */
int  fx_amount_compute(int target, int effect);           /* 0x1C8ED 固定量结算 */
int  heal_apply(int target, int power);                   /* 0x1C916 治疗结算（实现版）：
                                                             HP += 0.9P+P×r/1000 clamp，
                                                             玩家方得经验 */
void fx_field_reset(int actor, int effect);               /* 0x1CA89 场地特效复位 */
void fx_unit_flash_alt(int actor, int effect, int n, const uint8_t *targets); /* 0x1CAC7 */

/* 0x1C75E（实现版）：命中判定——命中 = rand%100 < 效果表[2]；返回0表示未命中/
 * 地形免疫，否则返回 fx_damage_adjust 的实际伤害量；伤害基数 =
 * 职业系系数表(unk_51F96)[系-1] × 效果表威力 /10；effect 10-12 走地形
 * 免疫分支(sub_1F183)。效果参数表 = effect_param_entry(effect)
 * （[0..1]i16 威力/[2] 命中率/[3..6] 参数区，dseg03 @0x619FD）。 */
int  fx_hit_immune_test(int target, int effect_id);

/* 0x4E866 效果参数表访问器（@0x619FD，252B=36 条上限已嵌 magic.c；
 * 消费界 = exec 表 28 项/效果 id 0..27——28..31 模式似真、32..35 与
 * 敌基表交界失配，均无消费者，2026-09-08 定案）：
 * [0..1] i16 威力 / [2] 命中率 / [3] 射程 / [4] 作用形状 /
 * [5] MP 消耗 / [6] 敌我（0=敌方，1=己方，治疗类=1）。
 * 消费方：结算层 6 函数 + AI 战术群 5 函数（ai_score_targets 评分用
 * [3][4][5][6]）。 */
const uint8_t *effect_param_entry(int effect_id);

/* 0x51D01 exec 表【28 项，效果 id 0..27】全解：0..8 ICF 薄包装
 * （flash/alt 两族）+ 9..27 定参薄包装（§13.39）。旧"40 项"读越界
 * ——0x51D71 起为状态 exit 表（2026-09-08 字节级证实）。 */
void spell_execute(int actor, int spell, int n, const uint8_t *targets);
/* 0x1D6C8：施法前调色板闪屏（色 0 表 RGB ↔ 黑 ×4 拍）。 */
void fx_palette_flash(int spell);

/* 0x1C269：法术位图展开——ent[+26..+30] 5 字节 40 位 → id 列表
 * （id = bit + 8*byte），返回数量。与 spell_learn_add 配对。 */
int  spells_collect(int unit, uint8_t *out);
int  fx_damage_adjust(int target, int raw);              /* 0x1C81F 实现版 */

/* 0x4E8BC：武器/道具表访问器（g_weapon_table @0x602AD，23B/条；
 * [9]=附魔类型 [10]=附魔参数 [11..12]=射程 [13]=类别/效果id（武器=槽位，
 * 法术道具=5..0x18）[14..15] u16=效果参数 [16]攻击距离(>0xF=257)
 * [17]敌我 [18]作用形状 [19..20] u16=价格；[1..8] 加成区（0x1145A ent_stats_apply_equipment 定案，
 * 2026-09-05：[1..2]ATK [3..4]HIT [5..6]DEF [7..8]EVA，i16 可负）。
 * 原始 dump 见 reverse/data-tables/dump.md。 */
const uint8_t *weapon_table_entry(int item);

/* fx_field_anim 三表（33B/effect，tables.c 已嵌真实数据）：
 * 帧基号/帧数/首音效（0=无）。硬编码特殊音效点：e22@7 e25@3,6
 * e18@4 e19@3,6 e8@3,6 e9@15,19。 */
extern const uint8_t g_fx_field_frame_base[33];
extern const uint8_t g_fx_field_frame_count[33];
extern const uint8_t g_fx_field_sfx_first[33];

#endif /* FD2RE_MAGIC_H */
