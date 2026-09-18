/* duel.h — 决斗/特效/演出引擎（逆向自 0x2B000-0x32000，docs/architecture.md §13）
 *
 * 子系统组成（入口均来自战斗/剧情代码）：
 *   duel_scene_load     0x2E2B0  普通决斗（battle_act_menu / ai_attack_plan_c / evt55）
 *   unit_anim_play      0x2DFC8  单位特写动画（ai_attack_plan_a）
 *   fx_scene_play       0x2FF01  特效演出总播放器（ai_attack_plan_b / 道具）
 *   ending_sequence_play 0x31529 结局演出（st26/st29 enter）
 *   ani_play            0x20421  ANI.DAT 流式全屏动画
 */
#include <stdint.h>

/* 0x2E2B0：普通决斗装载（实现于 battle.c——战场侧拥有实体状态） */
void duel_scene_load(int attacker, int defender);

#ifndef FD2RE_DUEL_H
#define FD2RE_DUEL_H

#include "fd2.h"

/* 特效相位协议：g_fx_controllers[effect_id] @0x524C6 被调用的 phase 值 */
enum {
    FX_PHASE_INTRO_FRAMES = 0,  /* 返回入场帧数 */
    FX_PHASE_INTRO_STEP   = 1,  /* 入场更新 */
    FX_PHASE_INTRO_END    = 2,  /* 入场收步 */
    FX_PHASE_HIT_COUNT    = 3,  /* 返回受击次数 */
    FX_PHASE_HIT_STEP     = 4,  /* 受击更新 */
    FX_PHASE_HIT_FX       = 5,  /* 受击特效 */
    FX_PHASE_OUTRO_FRAMES = 6,  /* 返回出靶帧数 */
    FX_PHASE_OUTRO_STEP   = 7,
    FX_PHASE_OUTRO_END    = 8,
};

/* 每效果受击扣血级数（IDA 0x526BC，4+4+2 字节，共 effect 0..9）；
 * 0x526C6 起是独立的阵营覆盖表。 */
extern const uint8_t g_fx_hit_counts[10];

/* 内表控制器表 @0x524C6 恰 10 项（0x524EE 起即 ctrl_0 内表数据）；
 * 0..6 = WATCOM ICF 折叠区（0x2B996..0x2CAFC，共享尾声 0x2C93D 嵌于
 * ctrl_6 段内，Hex-Rays 不可反编译——2026-09-05 已 capstone 逐指令
 * 定案，见 docs §13.4/§13.9 与 tools/fx_ctrl_dump.py）；7/8/9
 * （0x2CAFC/0x2CCF4/0x2CE1A）可反编译、重构挂号。
 * 统一协议（调用方 fx_scene_play@0x30469 定案）：
 *   int ctrl(ent, pkg, dst, pitch, mode)
 *   pkg  = FDOTHER 'R'+effect 覆盖包（(w,h) 帧族，帧表 @+8）；
 *   dst/pitch = 决斗影子缓冲指针与行距（320/640，640 为震屏双宽）；
 *   mode 0=入场帧数 1=入场更新 2=入场收步 3=受击次数 4=受击更新
 *        5=受击步（返 1=一次命中落地）6=出靶帧数 7/8=出靶步。 */
int fx_ctrl_0_particles(int ent, const uint8_t *pkg, uint8_t *dst,
                        int pitch, int mode);               /* 0x2B996 7 槽线性粒子 */
int fx_ctrl_1_particles8(int ent, const uint8_t *pkg, uint8_t *dst,
                         int pitch, int mode);              /* 0x2BB33 8 槽两行镜像 */
int fx_ctrl_2_statemach(int ent, const uint8_t *pkg, uint8_t *dst,
                        int pitch, int mode);               /* 0x2BD6C 帧元状态机 */
int fx_ctrl_3_particles12(int ent, const uint8_t *pkg, uint8_t *dst,
                          int pitch, int mode);             /* 0x2BFD9 12 槽再生流 */
int fx_ctrl_4_particles10(int ent, const uint8_t *pkg, uint8_t *dst,
                          int pitch, int mode);             /* 0x2C217 6 槽随机行 */
int fx_ctrl_5_particles10(int ent, const uint8_t *pkg, uint8_t *dst,
                          int pitch, int mode);             /* 0x2C441 6 槽双通道 */
int fx_ctrl_6_sparks(int ent, const uint8_t *pkg, uint8_t *dst,
                     int pitch, int mode);                  /* 0x2C67D 五点轨道环 */
int fx_ctrl_7(int ent, const uint8_t *pkg, uint8_t *dst,
              int pitch, int mode);                         /* 0x2CAFC 3 槽池 */
int fx_ctrl_8(int ent, const uint8_t *pkg, uint8_t *dst,
              int pitch, int mode);                         /* 0x2CCF4 16 槽带 */
int fx_ctrl_9(int ent, const uint8_t *pkg, uint8_t *dst,
              int pitch, int mode);                         /* 0x2CE1A 计数帧 */
int fx_ctrl_dispatch(int id, int ent, const uint8_t *pkg, uint8_t *dst,
                     int pitch, int mode);                  /* g_fx_controllers[id] */

/* 0x54153：fx 演出音效包（fx_scene_play 载入 FDOTHER 'R'+effect，
 * 控制器双通道播放；fx2re 宿主 NULL 时静音防崩）。 */
extern uint8_t *g_fx_sfx_pkg;

/* duel_scene_load@0x2E2B0 声明于 battle.h（实现在 battle.c），此处不重复。 */

void    unit_anim_play(int ent, int start_frame);          /* 0x2DFC8 */
void    fx_scene_play(int actor, int effect_id,
                      int n_targets, const uint8_t *targets); /* 0x2FF01 */
void    fx_scene_special(int actor, int effect_id,
                         int n_targets, const uint8_t *targets); /* 0x2D80D */
void    fx_scene_multi_hit(int actor, int effect_id,
                           int n_targets, const uint8_t *targets); /* 0x2CF30 */
void    ending_sequence_play(void);                        /* 0x31529 */
/* 0x24618 cast_beam_fall 真 ABI（2026-09-13 逐指令定案，4 栈参
 * add esp,10h）：(格x, 格y, half0, dhalf)——内部 cx=arg0*24+12、
 * cy=arg4*24+16（0x24632/0x2463E，均固定），光柱 esi=arg_8 起每帧
 * +=arg_C（0x246FB），帧循环 9..1（blk3 帧 + delay(5)），尾
 * delay(500)+palette 0..62 步进 2。调用点实参：st21=(vx,vy+3,10,8)、
 * st26=(vx,vy-1,10,10)、st29=(vx,vy+1,10,8)——旧 3 参像素坐标签名
 * 把 ×24 预乘放调用点且缺第 4 参，已废。 */
void    cast_beam_fall(int tile_x, int tile_y, int half0, int dhalf);
void    cast_beam_rise(int cx, int cy, int r0, int dr);   /* 0x2189A 半径语义 */
void    cast_charge_anim(int r0, int dr);                 /* 0x21EB1 蓄力两阶段 */
void    ending_text_show(int text_id, int slot);           /* 0x31BDF */
void    ending_anim_part2(void);                           /* 0x31DE2 */
void    ani_play(int anim_id, int delay_ms, int skippable); /* 0x20421 */

/* 共享原语（§13.3） */
void    duel_stamp_unit(void *dst, int ent);               /* 0x2FACD */
/* 0x18C6D 单位血条面板（duel_stamp_unit / move_range_wait_pump 共用）；
 * pitch 320=决斗位，456=战场影带位。 */
void    unit_bars_stamp(uint8_t *dst, int pitch, int ent);
void    duel_intro_slide(int actor, int with_target, void *a_pkg,
                         void *b_pkg, void *work, void *backdrop,
                         void *tai_pkg);                   /* 0x2E9A8 */
int     unit_pose_step(void *pose_pkg, int x_off, void *dst, int pitch);
                                                            /* 0x311E5 */
int     fx_hit_immune_test(int target, int effect_id);     /* 0x1C75E */
int     fx_damage_adjust(int target, int raw);             /* 0x1C81F */
void    fx_target_transition(int actor, void *overlay, void *pose,
                             void *pkg_a, void *work, void *backdrop,
                             void *pkg_b, int effect_id);  /* 0x31266 */
/* palette_mix_range 实现与声明在 video.h/video.c（IDA 0x2DF01）。 */

/* 0x1BFFE 战斗/据点共享装备菜单（三缓冲+滑入动画+两列道具网格+
 * 适性判定+equip_slot；2026-09-07 定案，见 duel.c 注释块）。 */
int     item_equip_menu(int unit);
/* 0x17AED 单位状态页：面板布局 + 法术列表子面板（spells_collect
 * 非空时底面板收起→法术列表→展开）+ 12 帧退场。 */
void    unit_status_page(int unit);

/* 0x1B932 inventory_pick_slot(unit, mode)：物品格选择整页（menu 布局 →
 * item_grid_pick 循环 → 12 帧滑出 + VRAM 还原 + 释放）。返回 1=选中
 * （g_menu_choice=槽位），0=取消/失败。mode=1 时仅类型非 0 道具可确认。 */
int     inventory_pick_slot(int unit, int mode);

/* 0x1CFF0 spell_cast_flow(unit)：魔法施放全流程（法术页选择 →
 * effect_param 形状/射程分类目标选择（传送 23 特判）→ fx_scene_play /
 * exec 表执行 → 收尸/死亡/结算）。返回 -1=Esc，0=取消，1=已施放。 */
int     spell_cast_flow(int unit);

/* 0x1CEED（原 static 公开，spell_cast_flow 复用）。 */
void    spell_list_draw(int unit, int sel, uint8_t *buf);

/* 0x18409（原 static 公开，roster_select_menu 复用）：菜单三面板
 * 12 帧滑入/滑出单帧（左 5/右 7/底 16i+94，全局 work/backup/snap）。 */
void    menu_slide_frame(int i);
/* 0x17FC0（原 static 公开，roster_select_menu 复用）：单位 HP/MP 条
 * 与数值面板（blk5 面板坐标系，battle 立绘帧）。 */
void    unit_stats_stamp(int unit, uint8_t *buf);

#endif /* FD2RE_DUEL_H */
