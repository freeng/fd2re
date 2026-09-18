/* host.h — 宿主平台抽象层（DOS 目标替换为端口/中断实现） */
#ifndef FD2_HOST_H
#define FD2_HOST_H

#include "fd2.h"

/* 键盘（DOS: BIOS 数据区 0x41A/0x41C 环 + int 0x16） */
void host_kbd_ring_flush(void);
int  host_kbd_key_avail(void);
int  host_kbd_get_scan(void);   /* int 0x16 AH=0x10，返回原始扫描码 */

/* SDL2 宿主显示后端。输入与显示生命周期均由 vga_set_mode 驱动。
 * indices/dac_rgb 是 VGA 线性显存与 DAC 的只读映射；后端在主线程事件泵
 * 中执行扫描，因而调用方无需模拟原版的“写显存即所见”语义。 */
int  host_video_init(const uint8_t *indices, const uint8_t *dac_rgb);
void host_video_shutdown(void);
void host_event_pump(void);
void host_idle_pump(void);

#endif
