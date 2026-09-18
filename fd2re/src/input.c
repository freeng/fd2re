/* input.c — 键盘输入（kbd_flush/idle_pump 已重建） */
#include "input.h"
#include "host.h"
#include "entities.h"
#include "video.h"
#include "timer.h"

void kbd_flush(void)
{
    /* DOS: uint16_t head = *(uint16_t*)0x41A; *(uint16_t*)0x41C = head; */
    host_kbd_ring_flush();
}

int kbd_key_avail(void) { return host_kbd_key_avail(); }

int input_wait_key(void)
{
    static uint32_t seen_tick;
    int scan;

    while (!kbd_key_avail()) {
        idle_pump();
        if (bios_tick() != seen_tick) {
            anim_tick_update(0);
            seen_tick = bios_tick();
        }
    }
    scan = host_kbd_get_scan();             /* int 0x16 AH=0x10 equivalent */
    if (scan == SCAN_PREFIX_E0 || scan == SCAN_KP_ENTER)
        return SCAN_ENTER;
    if (scan == SCAN_DEL)
        return SCAN_ESC;
    return scan;
}

/* IDA 0x60003 g_idle_cycle_gradient —— 31 组 RGB（6-bit DAC 值），
 * dseg03 静态数据，全库仅 idle_pump 引用、运行期从不重写。
 * 蓝紫脉冲波：0E,15,26 ↔ 0B,12,23 往返。 */
static const uint8_t s_idle_cycle_gradient[31 * 3] = {
    0x0E, 0x15, 0x26,   0x0D, 0x14, 0x25,   0x0D, 0x14, 0x25,
    0x0D, 0x14, 0x25,   0x0C, 0x13, 0x24,   0x0C, 0x13, 0x24,
    0x0B, 0x12, 0x23,   0x0B, 0x12, 0x23,   0x0B, 0x12, 0x23,
    0x0B, 0x12, 0x23,   0x0C, 0x13, 0x24,   0x0C, 0x13, 0x24,
    0x0D, 0x14, 0x25,   0x0E, 0x15, 0x26,   0x0E, 0x15, 0x26,
    0x0E, 0x15, 0x26,   0x0E, 0x15, 0x26,   0x0D, 0x14, 0x25,
    0x0D, 0x14, 0x25,   0x0D, 0x14, 0x25,   0x0C, 0x13, 0x24,
    0x0C, 0x13, 0x24,   0x0B, 0x12, 0x23,   0x0B, 0x12, 0x23,
    0x0B, 0x12, 0x23,   0x0B, 0x12, 0x23,   0x0C, 0x13, 0x24,
    0x0C, 0x13, 0x24,   0x0D, 0x14, 0x25,   0x0E, 0x15, 0x26,
    0x0E, 0x15, 0x26,
};
static uint16_t s_idle_cycle_tick;    /* 0x60000：上次 tick 锁存 */
static uint8_t  s_idle_cycle_phase;   /* 0x60002：0..15 回绕 */

/* 原版各菜单/槽位选择器内联的"阻塞读键"族（0x1FE48 主菜单、
 * 0x29CC9 存档槽、0x2B6E9 编队等）：int386(0x16,AH=10h) 直读 +
 * E0/52→Enter、53→Esc 归一化，等待期【无任何空闲回调】。fd2re 的
 * input_wait_key(0x11AA8) 自带 idle_pump 循环（DAC 色循环）——原版
 * 该函数全库仅 anim_pump(0x117E7) 一个调用方，菜单族误用会让待机
 * 色循环覆盖业务调色板 224..239（2026-09-07 主菜单回归根因）。
 * 宿主等价：自旋 host_idle_pump 只泵窗口事件，不动画循环。 */
int input_read_key_blocking(void)
{
    int scan;

    while (!kbd_key_avail())
        host_idle_pump();
    scan = host_kbd_get_scan();
    if (scan == SCAN_PREFIX_E0 || scan == SCAN_KP_ENTER)
        return SCAN_ENTER;
    if (scan == SCAN_DEL)
        return SCAN_ESC;
    return scan;
}

void idle_pump(void)
{
    /* 宿主事件必须在等待输入期间持续处理，否则窗口会失去响应。 */
    host_idle_pump();
    /* IDA 0x4E31C（§13.55 定案，非音乐服务）：每 ≥2 BIOS tick 推进
     * phase，从梯度 + 3*phase 滑窗连读 16 组写 DAC 色 224..239 ——
     * 31 组梯度的 16 窗口滑动 = 待机水波/微光动画；锁存取写后的
     * 新 tick 读数（u16 减法自带午夜回绕）。 */
    if ((uint16_t)(bios_tick() - s_idle_cycle_tick) >= 2u) {
        const uint8_t *p;

        if (++s_idle_cycle_phase == 16)
            s_idle_cycle_phase = 0;
        p = s_idle_cycle_gradient + 3u * s_idle_cycle_phase;
        for (int i = 0; i < 16; i++)
            dac_write(224 + i, p[3 * i + 0], p[3 * i + 1], p[3 * i + 2]);
        s_idle_cycle_tick = (uint16_t)bios_tick();
    }
}
