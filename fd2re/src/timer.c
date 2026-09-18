/* timer.c — 计时（0x17AA9 已重建；宿主用时钟近似） */
#include "timer.h"
#include "host.h"

#if defined(_WIN32)
#include <windows.h>
static void sys_sleep_ms(int ms) { Sleep((DWORD)ms); }
static uint64_t sys_monotonic_ms(void) { return (uint64_t)GetTickCount64(); }
#else
#include <time.h>
#include <unistd.h>
static void sys_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
static uint64_t sys_monotonic_ms(void)
{
    return (uint64_t)clock() * 1000u / CLOCKS_PER_SEC;
}
#endif

static uint32_t s_virtual_tick;   /* 宿主侧伪 tick：18.2Hz */

uint32_t bios_tick(void)
{
    /* DOS 目标：读取 0x0040:006C；宿主以 ~18.2Hz 递增近似 */
    static uint64_t last_ms;
    uint64_t now = sys_monotonic_ms();
    if (now - last_ms >= 55) {
        s_virtual_tick += (uint32_t)((now - last_ms) / 55u);
        last_ms = now;
    }
    return s_virtual_tick;
}

void wait_bios_ticks(int n)
{
    uint32_t start = bios_tick(), t;
    do {
        /* BIOS polling was a busy wait.  Keep that cadence, but dispatch
         * host window messages so a native window does not appear frozen. */
        host_event_pump();
        t = bios_tick();
        if (t < start) t += 0x1800B0;   /* 午夜回绕（0x1800B0 ticks/天） */
    } while ((int32_t)(t - start) < n);
}

/* j___delay@0x3790A = thunk -> WATCOM _delay(ms)：参数为毫秒
 * （2026-09-06 反汇编定案，"单位待核"挂号关闭）。 */
void delay_ms(int ms)
{
    /* The DOS player busy-waited.  A native host must dispatch its queue at
     * every original frame delay so fades and ANI playback remain responsive. */
    host_event_pump();
    sys_sleep_ms(ms);
}
