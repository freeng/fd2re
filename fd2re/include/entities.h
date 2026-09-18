/* entities.h — 80 字节记录的实体/动画表（战场单位 + 演出实体） */
#ifndef FD2_ENTITIES_H
#define FD2_ENTITIES_H

#include <stddef.h>
#include "fd2.h"

/* 记录布局（battle_strike_formula/save_game_load/商店·复活·转职流程验证，2026-09-04；
 * +3/+6 语义 2026-09-08 以 0x1B5F1/0x1B623 与 battle.c 全库用法更正）：
 * +0/+1 格 x/y；+3 行走方向码（0=下,1=左,2=上,3=右，ent_walk_*）；
 * +5 死亡标记（ent_flag1_test==1；temple_revive_menu 清 0 并 HP=MaxHP）；
 * +6 阵营（0=玩家、1=友军 NPC、2=敌——battlefield_alive_count/
 *   battle.c 敌我判定/ent_targets_by_distance 同读此键）；
 * +7 职业 id（=FDICON.B24 图标索引；转职改写，立绘派生源）；
 * +8 人名 id（文本 id+1）；+10..+25 8×2 道具槽：+10+2i 数量/占用(signed,-1=空)、
 *   +11+2i 道具 id；+31 AI 子类型；+32 职业系（=职业表[0]，转职刷新，
 *   复活计价 g_revive_price_lut[+32]×等级）；+33 level；
 * +34 AI 行为/参数；+37 参战；+38 已行动；+49..51 掉落；
 * +55..70 成长五维；+60 exp；+0x40 HP u16；+0x42 MaxHP u16；
 * +0x48 ATK；+0x4A DEF；+0x4C HIT；+0x4E EVADE。
 * 布阵三表示例：0x52273（16 单位）、0x52327（20 单位）。 */

/* IDA 0x11CAC anim_tick_update(0)：战斗画面单帧合成（相位钟 → idle →
 * 地形 → 选框层 → 实体 → 信息面板）+ blit 312x192 到 VGA。 */
void anim_tick_update(int arg);

/* 0x18B84 move_range_wait_pump(unit)：移动域"等首键"段——每 tick 整场
 * 重绘（anim_tick_update 序列 + 单位血条面板，x 随进门光标半屏左/右）。
 * 键不消费（留给 battle_move_select）。battle_unit_turn 唯一调用。 */
void move_range_wait_pump(int unit);

/* 0x122DC 选框/攻击指示层（anim_tick_update 内部于地形后调用）：
 * g_text_busy 选模式——1=光标格选框（blk1 帧0）；2=帧1；3=十字5格；
 * 4/5=曼哈顿≤2/≤3 攻击形状；6=清光标格洪泛候选字节；0/其他=无。 */
void field_marker_layer(void);

/* 0x10652 战场背景层装载：战斗态（9/24/25=blk15、28/29=blk55）波光
 * 包+scratch；ADV 对（17=462x226 blk16/17、21=408x276 blk35/36、
 * 22=408x256 blk40/41、27=462x244 blk46/47）上下半 RLE 解码带；
 * 23=312x192 blk42 + 0x24D22(0) 首卷。其余态释放置空（序章 31/32
 * 无背景层）。 */
void battle_backdrop_load(void);
/* 0x24D22：n!=0 置 23 态卷速（0x51A10）；n==0 带整体上卷卷速行
 * （底 n 行回卷至顶，312 pitch 专属）。 */
void battle_backdrop_scroll(int n);
/* 0x11F4E 地形通道前置：波光（9/24/25/28/29，屏幕固定）/ 视差（17/21/
 * 22/27，src=3*map_x+2*pitch*map_y+cnt_x/2+pitch*(cnt_y/3)——原版
 * 0x12094 公式，横 1/8 纵 1/12 视差+步内平滑）/ 上卷（23，每 tick）。
 * dst=带内当前可见区（含步行滑动偏移）。band=battle_shadow_layer()。 */
void field_backdrop_render(uint8_t *band, int map_x0, int map_y0);

/* 0x13185/0x12EAA/0x1300D/0x13315 四向单格六帧行走原语（2026-09-07 全解
 * 落地）：跟随分支每帧 ±4px 平滑镜头滚动 + 实体分数位移；步末落格、
 * 偏移清零、tile_event_check。序章 st00_exit 走上原语（slot 2）。 */
void ent_walk_up_one_tile(int unit);
void ent_walk_down_one_tile(int unit);
void ent_walk_left_one_tile(int unit);
void ent_walk_right_one_tile(int unit);

/* IDA 0x12C0D：光标处单位（无则 -1）。 */
int  ent_at_cursor(void);
/* IDA 0x12CEA：光标平滑滚动到 (x,y)（逐格 + 文本速度等待）。 */
void cursor_scroll_to(int x, int y);
/* IDA 0x12D7B：镜头聚焦单位（cursor_scroll_to(ent.x, ent.y)）。 */
void camera_focus_ent(int unit);
/* IDA 0x11B48/0x11B9B/0x11C59/0x11BFA：光标上/下/左/右（含视口滚动）。 */
void cursor_up(void); void cursor_down(void);
void cursor_left(void); void cursor_right(void);
/* IDA 0x12E38：格 (x,y) → 8 字节地块信息：
 * [0..1]=FDFIELD 单元值，[2..3]=单元 flags，[4..7]=FDSHAP 属性四字节。 */
/* 456 宽战场影子层（原版 g_shadow_buf2 布局等价）——fx 特效共用。 */
uint8_t *battle_shadow_layer(void);
void      battle_shadow_blit(void);
/* 0x1297D anim_phase_tick 公开包装（confirm 等待期相位钟）。 */
void      field_phase_tick(void);
/* 0x53C07/0x53C0B 实体/光标相位钟只读访问（0x1DA16 立绘重绘选帧用）。 */
int       field_entity_phase(void);
int       field_cursor_phase(void);

void tile_lookup(int x, int y, uint8_t out[8]);
/* 载入当前状态对应的 FDFIELD/FDSHAP 块（save_game_load 使用）。 */
int field_load_for_state(int state, int shape_id);
/* IDA 0x11EEE 的地形部分：把当前 13x8 视口合成到 mode-13h 索引面。 */
void field_render_viewport(uint8_t *dst, int pitch);
/* 同上通用形（行走原语的 13x9/14x8 窗口与 map 原点偏移）。 */
void field_render_viewport_ext(uint8_t *dst, int pitch, int cols, int rows,
                               int map_x0, int map_y0);
/* 0x127A9 的实体通道，供需要保留 456 宽离屏表面的过场复用。
 * 调用方须先完成地块合成；本函数只叠加未移除的实体。 */
void field_render_entities(uint8_t *shadow);
/* 0x1ACF3 战场格子信息区（底部避让光标换边）：blk5 帧 130 底板 +
 * FDSHAP 地形贴图 + 攻/防% + 光标单位立绘/HP。门条件
 * g_save_flag_51aab && g_transition_busy。 */
void tile_info_panel_draw(uint8_t *dst, int pitch);
/* 0x129EC: high-terrain overlay pass.  It is separate because the flashback
 * rebuilds use nonstandard entity positions before applying the normal
 * map-relative occlusion tiles. */
void field_render_entity_shadows(uint8_t *shadow);
/* Copy the most recently completed 456x336 field compositor surface.
 * A cutscene must begin from this surface: recreating it later loses the
 * exact frame state that the original preserved in g_shadow_buf2. */
int  field_shadow_snapshot(uint8_t *dst, size_t size);
/* 0x127E0 with an explicit vertical source-surface displacement.  The
 * flashback's frame 6/7 rebuilds use -8/-5 rows for only newly spawned
 * entities. */
void field_render_entity_offset(uint8_t *shadow, int unit, int row_offset);
/* IDA 0x4DF4C：清空 FDFIELD 每格运行时候选字段。 */
void field_reset_candidates(void);
/* IDA 0x1B750: recompute derived combat stats from growth fields and class
 * modifiers; item-table additions are applied when the item database exists. */
void ent_recompute_derived(int unit);
/* IDA 0x1145A：装备加成重算（roster 表）——8 道具槽 bit6(已装备) 累加武器表
 * [1..2]ATK/[3..4]HIT/[5..6]DEF/[7..8]EVA(i16 可负) 至 ent[+72/74/76/78]；
 * 基数 ent[+55]/[+57] 与 ent[+62]（HIT、EVA 共用）。roster_spawn_ent 收尾调用。 */
void ent_stats_apply_equipment(int roster_idx);

/* IDA 0x1D79C: 学习魔法 —— ent[+26+id/8] |= 1<<(id%8)。
 * ent[+26..+31] = 6 字节已习得魔法位图（48 位）。 */
void spell_learn_add(int unit, int spell_id);

/* ---- 库存四件套（0x1B8A6/0x1B722/0x1B8E7/0x1BB8C，2026-09-04 实现） ---- */
int  inventory_used_count(int unit);              /* 非空槽计数 */
int  inventory_get_item(int unit, int slot);      /* 槽内道具 id */
void inventory_remove_item(int unit, int slot);
int  inventory_find_item_slot(int unit, int item);  /* 槽号或 -1 */   /* 移除并压缩，末槽 0x80 标空 */
int  ent_inventory_add(int unit, int item);       /* 入包：1=成功，-1=满 */
int  equipped_slot_find(int unit, int want_high); /* 0x1B83D 已装备槽：
                                                     数量 bit6=装备中；
                                                     0→id<0x80 武器，1→特殊 */

/* IDA 0x17AED：单位状态页。 */
void sub_17AED(int unit);
/* IDA 0x1F183：特殊单位判定（不受地形修正等）。 */
int  sub_1F183(int unit);
/* IDA 0x4E84F：敌人基表（icon-68 索引，[9]=经验基数）。 */
/* IDA 0x4E84F enemy_base_table_entry：敌基表 10B/条×68 @0x61AF9（索引=icon−68），
 * [9]=经验基数。实现 tables.c（g_enemy_base_table_raw，2026-09-05 全量嵌入）。 */
const uint8_t *enemy_base_table_entry(int idx);
/* IDA 0x4E838 enemy_template_entry：敌模板 24B/条×32 @0x61DA1（敌职业 id 0..31）。
 * 字段映射见 tables.c 注释；roster_spawn_ent 消费。 */
const uint8_t *enemy_template_entry(int idx);
/* tables.c 实现（0x4E8BC，23B/条×100 @0x602AD；magic.h 同原型）——
 * ent_stats_apply_equipment 消费。 */
const uint8_t *weapon_table_entry(int item);
/* IDA 0x4E821: 32 rows of 11-byte class growth ranges at 0x620A1. */
const uint8_t *growth_table_entry(int idx);
/* IDA 0x4E807: 6 rows of 12-byte (level, spell) pairs at 0x626B3;
 * index = growth row byte [10] (255 = no learning). */
const uint8_t *spell_learn_entry(int idx);
/* IDA 0x4EBE3 fd2_rand：xor eax,eax; mov ax,state 后返回；调用方看到
 * 的是零扩展 16 位值（不能用 int16_t，否则会错误符号扩展）。 */
uint16_t fd2_rand(void);

#endif
