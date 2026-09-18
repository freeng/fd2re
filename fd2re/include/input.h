/* input.h — BIOS 键盘（int 0x16 / 0x41A 环） */
#ifndef FD2_INPUT_H
#define FD2_INPUT_H

#include "fd2.h"

/* IDA 0x4E381 —— 已验证：0x41A head → 0x41C tail，清空待处理按键。 */
void kbd_flush(void);

/* IDA 0x10620：键盘环非空判断（名称待最终核实）。 */
int  kbd_key_avail(void);

/* IDA 0x11AA8 input_wait_key —— 已验证控制流：
 *  自旋等键（每 0x46C tick 变化调 anim_tick_update(0)），
 *  int386(0x16, AH=0x10) 读键，返回重映射后扫描码：
 *    0xE0 / 0x52(小键盘Enter) -> 0x1C(Enter)
 *    0x53(Del)                -> 0x01(Esc)
 *  原版全库仅 anim_pump(0x117E7) 一个调用方——等待期跑 idle_pump
 *  （DAC 色循环）。菜单族（主菜单/存档槽/编队/槽面板）原版均为
 *  内联 int386 直读、无空闲回调，必须用 input_read_key_blocking。 */
int  input_wait_key(void);

/* 原版菜单族内联"阻塞读键"（0x1FE48/0x29CC9/0x2B6E9 等）的宿主等价：
 * 自旋泵宿主事件（无 DAC 色循环）+ E0/52→Enter、53→Esc 归一化。 */
int  input_read_key_blocking(void);

/* IDA 0x4E31C idle_pump —— 2026-09-06 定案（非音乐服务）：等键等待
 * 循环的空闲回调；每 ≥2 BIOS tick 推进 phase(0..15)，从 0x60003 起
 * 31 组梯度滑窗连读 16 组 RGB 写 DAC 色 224..239（待机水波/微光）。
 * 注意：会覆盖业务调色板 224..239，只准在原版 15 个调用点（等键/
 * 行走/anim_tick_update/径向菜单/确认框/战况板/过场）使用；宿主窗口
 * 响应之外的私加点一律用 host_idle_pump()（2026-09-07 回归教训）。 */
void idle_pump(void);

#endif
