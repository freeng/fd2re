/* statemachine.h — 30 状态双表状态机 */
#ifndef FD2_STATEMACHINE_H
#define FD2_STATEMACHINE_H

#include "fd2.h"

/* 主循环核心（IDA main 0x25DB5 内联 + 0x26152 state_run_scene）。
 * 返回非 0 = 要求退出内层循环（quit 或回标题）。 */
int  game_inner_loop(void);

/* IDA 0x26152 state_run_scene：
 *  常规态：fade -> music(10) -> 背景块（config[0]→LUT@0x52405→FDOTHER {11,61,62}）
 *          -> RLE 到影子缓冲+0x8088（456 宽）-> scene_render_compose 交互循环
 *  编队态（g_state_roster_menu[st]!=0）：清屏 -> str410 -> roster_select_menu
 *  返回 0 = 本状态结束（走 g_state_exit）。 */
int  state_run_scene(void);

/* IDA 0x4E809：等价于 &g_state_config[state]；数组起点 dseg03 0x6236E，
 * 原始 lea 基址 0x6238D 是跳过的首个 31B 记录。 */
const uint8_t *state_config_get(int state);

/* 0x53F52 dword_53F52：场景/菜单共享动画钟（城镇 compose 与 sub_26EDA
 * 菜单等待均读写；0..3 循环，消费方各自映射 3→1）。 */
extern uint8_t g_scene_anim_clock;


/* IDA 0x25EBB state_boot_driver（F-005）：
 *  startup_intro_menu() == 0 → 新游戏（g_state=0，g_state_exit[0]()）
 *  == 1 → 续档（FD2.SAV 恢复路径） */
int  state_boot_driver(void);

#endif
