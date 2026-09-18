/* event.h — 事件/剧情脚本子系统（g_event_script_table，90 项）
 *
 * 2026-09-04 逆向定位：IDA 0x34531-0x3644E 的 90 个处理器即本表全部内容
 * （90 项互不相同；此前文档称 g_action_post_hook/"动作后钩子"，实际语义
 * 是**通用剧情事件脚本**，已更名 g_event_script_table）。
 *
 * 触发源（写入 g_pending_event_id，原 g_last_action_id，255=无）：
 *   1. tile_event_check(0x13A44)：单位走到带事件的地块 —— 地块号经
 *      g_battle_ctx 附表 (+51=event_id, +52=触发模式) 映射（村庄/门/
 *      宝箱/剧情点）。ai_unit_act 移动后逐格调用。
 *   2. battle_show_results(0x1AA1D) 结果记录 type=2：延时 200ms 后
 *      直接按 id 调用本表（道具/魔法使用后的追加演出）。
 *   3. battle_system_menu / battle_unit_turn / battle_phase_ally_npc /
 *      battle_phase_enemy：菜单与敌方回合的脚本事件直接置号。
 *
 * 分发点：anim_pump@0x1199C 尾部 ——
 *   if (g_pending_event_id != 255)
 *       g_event_script_table[g_pending_event_id](cursor_unit);
 *   g_pending_event_id = 255;
 *
 * 处理器形态（全部为"原语序列"，无解释器循环）：
 *   camera_scroll_to / sprites_reload_slot / ui_script_exec /
 *   text_render_box(对白) / cutscene_transition(闪白转场) /
 *   ents_misc_set_range(群体变形/开关) / roster_spawn_ent / ents_kill_from /
 *   ent_flag1_test(一次性事件防重入) / delay —— 见 event.c 各项注释。
 *
 * 注意：表项 [23]=0x34B9A、[24]=0x34C52 是 IDA 未拆分的段内入口
 * （物理上位于 sub_34B6F 函数体内）。
 */
#ifndef FD2_EVENT_H
#define FD2_EVENT_H

#include "fd2.h"

/* 事件脚本处理器原型：参数 = 光标处单位 idx（anim_pump 分发时）。 */
typedef void (*fd2_event_script_fn)(int unit);

/* IDA 0x51B91 —— 90 项事件脚本表（0x34531-0x3644E）。 */
extern fd2_event_script_fn g_event_script_table[90];

/* IDA 0x51A8F —— 待触发事件号（255=无；anim_pump 消费后复位）。 */
extern uint8_t g_pending_event_id;

/* anim_pump 尾部的统一分发（0x11994-0x119A6 结构）。 */
void event_script_dispatch(int unit);

/* IDA 0x13A44 tile_event_check(x, y, mode)：地块事件探测。
 * tile_lookup 取逻辑地块号 t（返回[0..1]）与属性标志 f（返回[2..3]）；
 * f&0x60==0 且 t!=0 时查
 * g_battle_ctx[2*(t-1)]+51/+52（event_id/触发模式），匹配 mode 则
 * g_pending_event_id = event_id。 */
void tile_event_check(int x, int y, int mode);
void battle_turn_event_scan(int phase);   /* 0x1A813 回合事件扫描（ctx 16×3B 槽） */

/* ---- 事件脚本共享原语 ---------------------------------------------- */

/* IDA 0x135DD —— 镜头逐格卷动到 (x,y)（光标同步；52 处调用）。 */
void camera_scroll_to(int x, int y);

/* IDA 0x134E4 —— 全体实体朝向复位(+3=0) + delay(20)。 */
void ents_face_reset(void);

/* IDA 0x10B4E —— 按槽位重载立绘：FDICON.B24 + FDFIELD.DAT 块
 * 3*g_state+2；对每个 battle_ctx[i](26B 记录).field152==slot 的单位调
 * sub_10C50 载入 g_standing_sprites，并 fwrite 到 FD2.TMP 缓存。 */
void sprites_reload_slot(int slot);

/* IDA 0x112A5 —— 从 roster/class 数据派生一个新实体（事件增援/入队）。 */
void roster_spawn_ent(int unit_id);

/* IDA 0x344F2 —— 区间设置实体 +52 低半字节（保留高 4 位）：
 * 群体变形/开关（值 7/3/0/11 已观测；含义待与 battle_ctx spawn 定义对齐）。 */
void ents_misc_set_range(int first, int last, int val);

/* IDA 0x34894 —— 实体 +5 bit0 查询（一次性事件防重入/已编入）。 */
int  ent_flag1_test(int unit);

/* IDA 0x32975 —— 实体 +5 bit0 置位（标记已处理）。 */
void ent_mark_flag1(int unit);

/* IDA 0x35B78 cutscene_transition(x, y, slot)：卷动 + 换立绘 + 闪白
 * （palette_add_range(0,255,255) → (0,255,0)）+ 延时收尾。 */
void cutscene_transition(int x, int y, int slot);

/* IDA 0x35F10 —— 从 from 起全部实体 HP(+0x40)=0 → battle_death_anim
 * （清场：友军 NPC 撤退/群众解散）。 */
void ents_kill_from(int from);

/* IDA 0x32999 —— 12 帧群像回放：FDOTHER blk95+blk9，逐帧 spawn/摆位
 * 实体，i==1 时发音效（序章回忆杀式过场，也用于事件 [1]/[2]）。 */
void cutscene_flashback(int ent_slot);

/* IDA 0x12263 —— 扫描全图：地块标志&0x20（村庄/刷新点）且
 * g_spawner_flags[tile] 已置位的格子，占用计数 +1（dword_53A51 数组）。 */
void map_spawn_count_refresh(void);
/* 0x13512：单位置"已行动"标记（ent[5] |= 0x80）。 */
void sub_13512(int unit);

#endif /* FD2_EVENT_H */
