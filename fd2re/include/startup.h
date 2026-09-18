/* startup.h — 开场门 + 主菜单（F-006，IDA 0x1F894） */
#ifndef FD2_STARTUP_H
#define FD2_STARTUP_H

#include "fd2.h"

/* 重构契约（evidence main-menu-static-20260904.md，动态已证 Down/确认）：
 *
 * int startup_intro_menu(void) {
 *     intro_and_gate();                       // 播放开场/过场帧
 *     pkg = dat_load_block("FDOTHER.DAT", old, 7);   // 7 子帧菜单包
 *     entry_count = save_check() ? (progress != 0xFF ? 3 : 2) : 1;
 *     selected = 0; confirmed = 0;
 *     while (!confirmed) {
 *         menu_draw_entries(pkg, selected, entry_count);   // 0x1FF79
 *         confirmed = update_selection(&selected, entry_count, int16_key());
 *     }
 *     fade_out_and_free(pkg);
 *     return selected;                        // 0 基；0=新游戏 1/2=续档
 * }
 */
int  startup_intro_menu(void);
void menu_draw_entries(const void *pkg, int selected, int entry_count);

#endif
