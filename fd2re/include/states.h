/* states.h — 30 个游戏状态的 enter/exit 处理器
 *
 * 表原址：g_state_enter @0x51DE9、g_state_exit @0x51D71。
 * 部分表项指向同一函数内部的相位标签（见 evidence
 * main-dispatch-targets-20260904.md：enter 表 30 项/26 起点，
 * exit 表 30 项/25 起点）。
 * 各状态的游戏语义（标题/菜单/战斗/地图…）待逐个逆向。
 */
#ifndef FD2_STATES_H
#define FD2_STATES_H

#include "fd2.h"

void st00_enter(void); void st00_exit(void);
void st01_enter(void); void st01_exit(void);
/* 0x33F78：cursor_scroll_to(x,y)+cutscene_stand（states.c；evt82 复用） */
void scroll_and_cutscene(int ent, int x, int y);
/* 0x22253 from==to 独立入口(fx_targeted_strike 复用) */
void battle_cutscene_stand(int ent, int x, int y);
void st02_enter(void); void st02_exit(void);
void st03_enter(void); void st03_exit(void);
void st04_enter(void); void st04_exit(void);
void st05_enter(void); void st05_exit(void);
void st06_enter(void); void st06_exit(void);
void st07_enter(void); void st07_exit(void);
void st08_enter(void); void st08_exit(void);
void st09_enter(void); void st09_exit(void);
void st10_enter(void); void st10_exit(void);
void st11_enter(void); void st11_exit(void);
void st12_enter(void); void st12_exit(void);
void st13_enter(void); void st13_exit(void);
void st14_enter(void); void st14_exit(void);
void st15_enter(void); void st15_exit(void);
void st16_enter(void); void st16_exit(void);
void st17_enter(void); void st17_exit(void);
void st18_enter(void); void st18_exit(void);
void st19_enter(void); void st19_exit(void);
void st20_enter(void); void st20_exit(void);
void st21_enter(void); void st21_exit(void);
void st22_enter(void); void st22_exit(void);
void st23_enter(void); void st23_exit(void);
void st24_enter(void); void st24_exit(void);
void st25_enter(void); void st25_exit(void);
void st26_enter(void); void st26_exit(void);
void st27_enter(void); void st28_enter(void); void st29_enter(void);
void st27_exit(void);  void st28_exit(void);  void st29_exit(void);

/* 状态表初始化（绑定到 g_state_enter/g_state_exit）。 */
void states_init(void);

#endif
