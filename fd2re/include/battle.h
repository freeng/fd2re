/* battle.h — 战场子系统（FDFIELD/FDSHAP，TRPG 部分）
 *
 * 2026-09-04 战斗主循环定位：main@0x25BF4 内层 do-while 每轮调 anim_pump@0x117E7，
 * 0x22BBE 只是共享尾声（先前"主循环在 0x22BBE"的判断作废）。
 * 详见 reverse/battle/digest.md 与 docs/architecture.md §10。
 */
#ifndef FD2_BATTLE_H
#define FD2_BATTLE_H

#include "fd2.h"

/* IDA 0x117E7 —— 战斗主输入泵（每次一键）：
 * Esc/','/'L' 循环下一可动单位；Enter/Space=单位指令或(空地)系统菜单；
 * PgUp=';' 全图总览；Home='<' 状态页；方向键移光标。
 * 行动完成后：exp 上限 99 → exp_levelup_check → g_state_post_action[g_state]
 * → enemy_phase_check → event_script_dispatch（g_event_script_table，
 * 见 event.h；2026-09-04 前误称 g_action_post_hook/g_last_action_id）。
 * 返回非 0 时 main 退出内层循环（回标题）。 */
int  anim_pump(void);

/* IDA 0x18890 —— 玩家单位回合：可达域洪泛(sub_4E390) → battle_move_select(4)
 * 选移动目标 → sub_4E4F6 路径 → sub_13488 走路动画 → battle_act_menu。 */
int  battle_unit_turn(int unit);

/* IDA 0x18D8C —— 行动菜单（径向 2x2，def={0,1,2,3}@0x51ED5）：
 * 装备武器 wdata[11]=最小射程(排除圈) [12]=最大射程(洪泛预算) → 无武器
 * 或射程内无目标 flags[0]=1；无法术或 ent[39] 状态 flags[1]=1；
 * 无道具 flags[2]=1。g_menu_choice: 0=攻击 1=魔法(spell_cast_flow
 * 0x1CFF0) 2=道具(battle_item_target_flow 0x1BBDC) 3=调查/待机
 * (treasure_interact)。2026-09-08 表述校正（旧注释 1/2 互换）。 */
int  battle_act_menu(int unit, int flags[4], int after_move);

/* 0x14818 shape_targets_collect(out, x, y, shape, radius, ally)：
 * shape>=16 → 十字形（臂长 shape-16 的行列标候选）；<16 → 可达域洪泛
 * （预算=shape）。radius>0 → 曼哈顿 <radius 的格排除（最小射程）。
 * 收集候选格上未阵亡单位：ally 0=敌(ent[6]==0) 1=我方全部(!=0)
 * 2=友军NPC(==1) 3=玩家(==2)；out 为 NULL 时只计数。调用方事后须
 * field_reset_candidates()。 */
int  shape_targets_collect(uint8_t *out, int x, int y, int shape,
                           int radius, int ally_mode);

/* 0x115B6 battle_move_select(mode, n_targets, targets)：
 *   mode 0..3 = 阵营目标选择（候选格 marker!=255 且半径内有该阵营
 *               单位才可确认，返回光标处实体）——半径 = g_text_busy(>1
 *               时 -1)，由调用方写入 w[18]+2 / ep[4]+2；
 *   mode 4 = 候选格（移动/地面目标），4/5/6 成功返回 0；
 *   mode 5 = 任意格；mode 6 = 传送落点（class_table[cls][tile 代价]
 *               ==20 且格上无其他单位；cls 来自 rec[32]，
 *               rec[7]==28→1，terrain_immune→19）。
 * 键 44/0x2C、76/0x4C 循环聚焦 targets[]（ent[7]==121 跳过镜头）。
 * Esc → -1。 */
int  battle_move_select(int mode, int n_targets, const uint8_t *targets);

/* 0x1BBDC —— 战斗道具四项径向菜单（def={8,9,10,11}@0x51F05，帧 24..35；
 * 旧误读 {16,17,16,17} → 帧 48..53 为 blk2 异构资源）：
 * 0=使用 1=给予 2=状态页 3=丢弃；相邻无我方单位时禁"给予"。
 * 返回 -1=Esc，0=取消/未消耗（外层重开），1=已消耗（道具效果已分发）。 */
int  battle_item_target_flow(int unit);

/* 0x51CF9/0x51CFD：传送目的格（道具 w[13]==23 / 法术 23 链写入；
 * 消费方=传送执行函数，fd2re 挂号）。 */
extern int32_t g_warp_dest_x, g_warp_dest_y;

/* IDA 0x2F7B6 —— 单击结算（完整公式）：
 *   命中: rand%100 < HIT(+0x4C) - EVADE(+0x4E)
 *   暴击: rand%100 < g_class_crit_base[class-1] (+武器效果4加成) → DEF/2
 *   伤害: dmg = 9*(ATK(+0x48)-DEF(+0x4A))/10 + rand%(dmg/9)，HP(-0x40)-=dmg
 *   地形: g_tile_atk_pct/g_tile_def_pct[tile]% 修正；武器效果2 附加状态
 *   经验: 敌基表[9]*敌lvl/(我lvl+30若进阶) * dmg/敌MaxHP → g_exp_gained
 * 返回伤害，res[0..5] = {命中,暴击,状态,?,效果3,伤害}。 */
int  battle_strike_formula(int att, int def, int res[6]);

/* IDA 0x2EBE1 duel_exchange（2026-09-08 帧协议全量重写，实现移驻 duel.c）：
 * 决斗交换演出——回合 1（3% 两回合）、res[4] 反杀追加一轮；FIGANI 3i+1
 * 帧表驱动，命中帧显示 HP 比例插值 + 守方面板刷新，pitch-400 冲突台
 * 双精灵震动合成，末命中帧暴击/状态 DAC 闪；远程包带接敌段 + 换景。
 * 返回守方显示 HP。 */
int  duel_exchange(int att, int def, const uint8_t *anim,
                   const uint8_t *vpose, uint8_t *work, uint8_t *frame,
                   const uint8_t *tai, const void *sfx_pkg);

/* IDA 0x2E2B0 duel_scene_load（2026-09-08 全量落地，实现移驻 duel.c）：
 * 决斗演出加载。TAI.DAT=决斗动画背景数据（非 AI）；背景按
 * g_state_duel_bg[g_state]（mage 类门控）或地块 info[6]；FIGANI.DAT
 * 每图标 3 块（3i 姿态 / 3i+1 帧表动画）；胜利定妆帧 + 战场恢复尾。 */
void duel_scene_load(int attacker, int defender);

/* IDA 0x1F0DC：贴邻 + 守方装备最小射程 1 武器 → 1（决斗反击门）。 */
int  melee_adjacent_test(int att, int def);

/* IDA 0x1AA1D —— 战后结果展示：记录 {type u8, val u16}：
 * type 0=战果文本432@vram+40739 + 物品槽写入；满槽 433 → 是/否确认
 * （confirm_yes_no）→ "是"→ inventory_pick_slot 换物（成功直接下一条）/
 * 取消→434@+40739；"否"→434@+46819（下一行）；均 delay(200)+restore；
 * type 1=金钱文本435@vram+40739 + g_gold；type 2=延时200ms后事件[val]；
 * type 3=从 g_pkg_text_evt 渲染文本[val]@vram+0（串内令牌自开窗）。 */
void battle_show_results(int unit, int count, const uint8_t records[]);

/* IDA 0x1B6B7 / 0x1DB65：收尸掉落（HP==0 且 +49!=255）/ 死亡动画。 */
int  battle_collect_drops(uint8_t out[][3]);
void battle_death_anim(void);

/* IDA 0x1BB8C：+10..+25 的 8×2 字节物品/武器槽加一项；满返回 -1。 */
int  ent_inventory_add(int unit, int item_id);

/* IDA 0x1B8A6 —— 统计单位当前已占用的物品/武器槽数量（0..8）。 */
int  sub_1B8A6(int unit);
/* IDA 0x1B722 —— 读取单位物品槽 index 的物品号。 */
int  sub_1B722(int unit, int index);
/* IDA 0x1B8E7 —— 道具应用（battle_show_results/事件脚本[61] 复用）。 */
void sub_1B8E7(int unit, int choice);
/* IDA 0x2AEDB —— 商店界面（事件脚本[61]：参数 208=商店类型？）。 */
int sub_2AEDB(int unit, int item_id);

/* IDA 0x1A30B —— 回合结束/新回合开始（2026-09-08 逐指令全量重写）：
 * ① wait(1)；② 回复 pass A：type==2 且 !(flags&0x81) 且无毒(+0x25)/
 * 已动(+0x26) 且 HP!=Max 的单位白剪影重绘(0x1DA16 mode2 色 0xFD)+
 * 呈现，命中则 sfx(fdother_31,#4)；③ pass B：HP=min(HP+Max/5,Max)+
 * mode0 复原重绘+sub_13512 暂置已行动；④ scan(1)→phase_status_tick(1)
 * （0x1A866 NPC 中毒/状态计时）→友军 NPC 相；⑤ bgm!=alt →
 * music_play(-1,0)+幕帘关/开(帧82)+battle_load_map（清 bit7=敌方回合）
 * →scan(0)→phase_status_tick(0)→music_play(alt)→敌方相；⑥ turn_count++
 * →幕帘关/开(帧80)+第二次 battle_load_map（新玩家回合）→music_play
 * (bgm)；⑦ 回合横幅两段（帧 83..91 直贴 VRAM + 帧 91 入带 k∈{2,3,4,9}，
 * 数字色 42，均 pkg_frame_blit/free 保存-恢复）；⑧ scan(2)→
 * phase_status_tick(2)（玩家侧回合开始结算）→camera_focus(0)。
 * scan 相位：1=友军相前、0=敌方相前、2=收尾（旧注反了，已正）。 */
void battle_turn_end(void);

/* IDA 0x13565：全部可控单位已行动(+38) → battle_turn_end。 */
void enemy_phase_check(void);
/* IDA 0x1D80B / 0x1D8BA：友军 NPC 相 / 敌方相。 */
void battle_phase_ally_npc(void);
void battle_phase_enemy(void);
/* IDA 0x1598A / 0x1567E —— 敌方目标/移动评分（六参数 ABI）。 */
int ai_score_targets(int unit, int mode);         /* 0x1598A（2026-09-08 真评分器） */
void battle_face_target(int attacker, int defender); /* 0x1F04A 主轴朝向 */
int  ray_targets_collect(uint8_t *out, int from_x, int from_y,
                         int to_x, int to_y, int len, int ally); /* 0x149F8 */
int ai_pick_move(int unit, int mode);             /* 0x1567E（2026-09-08 真评分器） */

/* IDA 0x13A9F —— AI 行为分发（ent[+34]&0xF）：
 * 0/1/2=攻击型变体(ai_decide_attack→plan_a/b/c)；3=攻击指定座标；
 * 4=移向固定座标；5=增援刷新点(g_battle_ctx 定义, g_spawner_flags 标记)；
 * 7=接近目标点。ai_move_to@0x14B78 走位、ai_attack_now@0x13FD4 出手。 */
void ai_unit_act(int unit, int mode);

/* IDA 0x1E292 —— 升级：exp(+60) 累计 ≥100 → level(+33)++（icon30/31 上限 99，
 * 其余 40）、5 项 stat_growth_roll（+55/+57/+62/+66/+70）、按级习得法术。 */
void exp_levelup_check(int unit);

/* IDA 0x22AF6 —— 名单发经验（+37 参战标记清零，+4*(lv+30若进阶)）。 */
void battle_award_exp_list(const uint8_t *units, int count, int field_off);

/* ---- 原有（更名后语义不变） ----------------------------------------- */

/* IDA 0x233C6 battle_field_init(x表, y表, dir, first, last, leader, lx, ly,
 *                               ldir, cursor_x, cursor_y) —— 2026-09-13 全量
 * 重导定案（真 ABI：11 个栈参，调用点 add esp,2Ch 实证；旧 fd2re 签名多出
 * 虚假 type_or_list 位导致全部调用点左移错位）：
 * fade → battle_load_map(0x13536) → 实体 [first..last] 写
 *   rec[0]=x表[i], rec[1]=y表[i], rec[3]=dir字段；
 * dir 字段 >=4 时为逐单位朝向表指针（dir[i]），<4 时为全队常量朝向
 * （0=下 1=左 2=上 3=右；rec[+3] 非 team——battle.c `3*rec[3]+phase`
 * 选精灵行定案）；leader != 0 → ent[leader]=(lx,ly,ldir)；
 * g_text_busy=0；scroll/cursor=(cx,cy)；view=0；尾 anim_tick_update(1)
 * +fade_in+delay(200)。 */
void battle_field_init(const uint8_t *list_x, const uint8_t *list_y,
                       const uint8_t *list_dir, int first, int last,
                       int leader_idx, int lx, int ly, int ldir,
                       int cursor_x, int cursor_y);

/* IDA 0x13536：清全部实体 flags bit7（=回合已行动位）→ 新回合。 */
void battle_load_map(void);
/* 2026-09-05 ctx 布局定案（§6.9）：常量与访问器见 battle.c 顶部注释块 */
int  battle_ctx_load(int state);                 /* FDFIELD[3k+1] → g_battle_ctx */
const uint8_t *battle_ctx_sprite_record(int idx);/* 26B 记录 @ctx+126 */
int  battle_ctx_sprite_record_count(void);
/* 0x34E05/0x34E1B（evt30 内联写）：运行时武装回合事件槽——直接改写
 * ctx[3*slot+3] 的 turn 字段（资产 turn=255=禁用；evt30 击杀首领后把
 * slot0/1 写成 turn+1/turn+2，令 evt31 在随后两回合敌方相前触发）。 */
void battle_ctx_turn_event_arm(int slot, uint8_t turn);
int  battle_stage_load(int state);               /* 0x1088D 全解 */
void sprites_reload_slot(int key);               /* 0x10B4E：按分组键生成 */
/* 0x53AFA（2026-09-08 定案）：standing 落位方式开关——1=按布阵层坐标直接
 * 落位（事件增援语义，原版在 reload 调用点前后成对置 1/清 0），0=以布阵
 * 坐标为目标的最近可走格搜索（battle_stage_load 初始组）。 */
extern uint8_t g_spawn_direct;
int  treasure_interact(int actor);               /* 0x190AC：宝箱/村庄交互 */

/* IDA 0x2AF28 roster_select_menu —— 编队界面：
 * 4×malloc(64000)、VRAM 快照、FDOTHER blk5 面板、12 槽位（sub_18409 渲染）、
 * Enter 多选、75/77 翻页（wrap g_roster_count-2）、容量 15（g_state>26 时 19）。
 * 返回 -1=取消 / 1=确认。 */
int  roster_select_menu(void);

/* IDA 0x205B4 battle_end_check（原 battle_sync_pending）：
 * 默认 pending=2；有敌人(type==0 且 !(flags&1)) → 0（战斗继续）；
 * ent[0] flags&1 → 1（先淡出再切换）。 */
void battle_end_check(void);

/* IDA 0x11506 battle_sync_to_roster：战后按 +8 char-id 把实体写回 roster，
 * 状态槽清零、HP=MaxHP、坐标从备份恢复。 */
void battle_sync_to_roster(void);

/* IDA 0x2000A battle_map_overview：缩略全图 + 光标闪烁，等键返回。 */
void battle_map_overview(void);

/* IDA 0x16F55 battle_system_menu：空地 Enter 的系统菜单（含直接结束回合、
 * sub_19DF7 存/读档）。 */
int battle_system_menu(void);

/* IDA 0x10010 save_game_load：FD2.SAV(0x59CB) 校验并恢复全部运行态。
 * 布局偏移见 fd2.h FD2_SAVE_*_OFF。 */
int  save_game_load(void);

#endif
