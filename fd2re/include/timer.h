/* timer.h — BIOS 0x46C tick 计时 */
#ifndef FD2_TIMER_H
#define FD2_TIMER_H

#include "fd2.h"

/* IDA 0x17AA9 wait_bios_ticks(n)：自旋等待 n 个 BIOS tick（处理午夜回绕）。 */
void wait_bios_ticks(int n);

/* 宿主抽象：WATCOM j___delay 对应的毫秒延迟（单位对应关系待核）。 */
void delay_ms(int ms);

/* BIOS tick 当前值（DOS: far read 0x0040:006C）。 */
uint32_t bios_tick(void);

#endif
