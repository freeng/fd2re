/* main.c — 游戏入口（逆向自 IDA 0x25BF4 main，runtime 0x1C1BF4）
 *
 * 原版顺序：
 *   AIL_init → 8×dat_load_block → malloc×3 → VGA 0x13 →
 *   tick 种子 → rand 热身 → 主循环（music_play(18) + state_boot_driver +
 *   内层状态机循环）→ 退出时 music_shutdown + VGA 模式 3。
 */
#include <stdio.h>
#include <stdlib.h>
#include "fd2.h"
#include "resource.h"
#include "video.h"
#include "audio.h"
#include "timer.h"
#include "entities.h"
#include "statemachine.h"
#include "states.h"

void *g_pkg_fdother_31, *g_pkg_fdother_1, *g_pkg_fdother_2, *g_pkg_fdother_3;
void *g_anim_sfx_data;
uint8_t *g_standing_sprites;             /* 0x53A61 立绘缓存 */
int32_t g_roster_grid_col;                 /* 0x53F72 */
void *g_pkg_fdother_4, *g_pkg_fdother_5, *g_pkg_fdtxt0, *g_pkg_fdother_6;
int32_t g_pkg_fdother_3_size, g_pkg_fdother_4_size, g_pkg_fdother_5_size, g_pkg_fdtxt0_size, g_pkg_text_evt_size;
void *g_magic_sfx_pkg;                         /* 0x53B13，魔法音效包 */
void *g_pkg_text_evt;                       /* 0x53A79 事件对白文本包 */
void *g_pkg_fdother_13;
uint8_t g_save_flag_53af9;
uint8_t g_save_flag_51aab = 1;   /* 0x51AAB 二进制初值 1：信息区显示开关（选项菜单 0x173B4 异或切换） */
uint8_t (*g_roster_table)[FD2_ENT_REC_SIZE];
int32_t  g_roster_count;

static void resources_load(void)
{
    g_pkg_fdother_31 = dat_load_block("FDOTHER.DAT", g_pkg_fdother_31, 31);
    g_pkg_fdother_1  = dat_load_block("FDOTHER.DAT", g_pkg_fdother_1,   1);
    g_pkg_fdother_2  = dat_load_block("FDOTHER.DAT", g_pkg_fdother_2,   2);
    g_pkg_fdother_3  = dat_load_block("FDOTHER.DAT", g_pkg_fdother_3,   3);
    g_pkg_fdother_3_size = g_last_block_size;
    g_pkg_fdother_4  = dat_load_block("FDOTHER.DAT", g_pkg_fdother_4,   4);
    g_pkg_fdother_4_size = g_last_block_size;
    g_pkg_fdother_5  = dat_load_block("FDOTHER.DAT", g_pkg_fdother_5,   5);
    g_pkg_fdother_5_size = g_last_block_size;
    g_death_fx_pkg   = g_pkg_fdother_5;   /* 0x53A81 别名（运行时实测补：此前 NULL→duel_stamp_unit 崩） */
    g_pkg_fdtxt0     = dat_load_block("FDTXT.DAT",   g_pkg_fdtxt0,      0);
    g_pkg_fdtxt0_size = g_last_block_size;
    g_pkg_fdother_6  = dat_load_block("FDOTHER.DAT", g_pkg_fdother_6,   6);
    g_roster_table   = malloc(FD2_ROSTER_MAX * FD2_ENT_REC_SIZE);  /* 原版 malloc(2560) */
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    audio_init();
    /* 宿主层 SDL_QUIT 直接 exit(0)，该路径的 AIL_shutdown 等价收尾
     * 靠此 atexit；audio_shutdown 幂等，正常退出路径的显式调用
     * 之后此处在 return 0 时成为空操作。 */
    atexit(audio_shutdown);
    resources_load();

    vga_set_mode(0x13);                       /* int386(0x10) AX=0x0013 */

    /* 原版 0x25D90..0x25DAD：取一次 libc rand()%256，调用 fd2_rand 热身；
     * 这一步影响后续所有口型/眨眼随机倒计时的序列。 */
    srand((unsigned)bios_tick());
    {
        int warmup = rand() % 256;

        for (int i = 0; i < warmup; i++)
            (void)fd2_rand();
    }

    states_init();

    for (;;) {
        int r, done = 0;

        music_play(18, 0);                     /* 标题 BGM */
        r = state_boot_driver();               /* F-005 */
        if (r == 0)
            /* IDA 0x25DC8..0x25E91：内层循环退出值仅 -1（anim_pump -1 或
             * state_run_scene 非零）→ v20=1 退出游戏；其余非零（pending==1
             * 淡出切换等）→ v20=0 重播标题。game_inner_loop 按 -1=退出 /
             * 0=回标题编码。 */
            done = game_inner_loop() == -1;
        else if (r == -1)
            done = 0;                          /* 原版将 -1 折叠为 0 */
        else
            done = 1;
        if (done) {
            /* 原版：AIL_shutdown（0x37ED8）→ VGA 模式 3 → 退出 */
            audio_shutdown();
            vga_set_mode(3);
            return 0;
        }
    }
}
