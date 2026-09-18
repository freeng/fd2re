/* text.c -- FDTXT tokens, 16x16 font blitter, and FDICON frame cache. */
#include "text.h"
#include "fd2.h"
#include "audio.h"
#include "input.h"
#include "timer.h"
#include "entities.h"
#include "resource.h"
#include "video.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_text_busy;   /* 0x51A83 */

#define ICON_SOURCE_COUNT 140
#define ICON_CACHE_MAX 40
#define ICON_FRAME_COUNT 12
#define ICON_CACHE_HEADER (ICON_CACHE_MAX * ICON_FRAME_COUNT * 4)
#define ICON_CACHE_BYTES  0x32A00

static int s_icon_ids[ICON_CACHE_MAX];
static int s_icon_count;
static size_t s_icon_next = ICON_CACHE_HEADER;

static uint16_t rd_u16(const uint8_t *p);
static uint32_t rd_u32(const uint8_t *p);

/* sub_165AC/sub_16B43 keep the screen beneath a DATO dialogue intact.  The
 * original uses five 310x86 compressed work buffers for its expansion; a
 * full VGA snapshot has the same visible restore invariant without silently
 * redrawing the field from an unrelated renderer.
 * 2026-09-06 双形态重建：mode 0 = token 窗（0x165AC 渐进展开，串内
 * -17..-20 开）；mode 1 = 场景窗（0x1956B 整框滑入，页函数显式开）。 */
struct dialog_context {
    uint8_t screen[FD2_VRAM_SIZE];   /* token 窗：开窗前快照 */
    uint8_t *backdrop_snap;          /* g_menu_snap_buf：场景窗开前快照 */
    uint8_t *backdrop_box;           /* g_screen_backup：框+肖像合成（滑入/出源）*/
    uint8_t *backdrop_work;          /* g_menu_work_buf：sub_1974C 步进暂存 */
    int active;
    int mode;                        /* 0=token / 1=scene */
    int top;
    int focus_wy;   /* 0x16BA2 a6：聚焦窗 y（2/112），0=非聚焦（无飞出） */
};

static struct dialog_context s_dialog;

/* dword_53C67 等价：肖像 stamp 基址（也是"DATO 窗激活"判定键——
 * 1832 顶窗 / 36887 底窗内位 / 店主大肖像位 / 0=无窗）。 */
static int s_portrait_origin;
/* g_dato_speaker_blk@0x53A85 等价：dat_load_block 句柄，跨窗存活，
 * 仅被下一次装载替换（场景收尾 0x196CB 不释放它）。 */
static uint8_t *s_speaker_blk;
static size_t   s_speaker_blk_size;

/* 0x52387 g_dialog_backdrop_ids：场景相位 → 说话人 DATO 块号。 */
const uint8_t g_dialog_backdrop_ids[6] = { 0x81, 0x80, 0x00, 0x82, 0x83, 0x84 };

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t package_size(const void *pkg)
{
    if (pkg == g_pkg_fdtxt0)
        return g_pkg_fdtxt0_size > 0 ? (size_t)g_pkg_fdtxt0_size : 0;
    if (pkg == g_pkg_text_evt)
        return g_pkg_text_evt_size > 0 ? (size_t)g_pkg_text_evt_size : 0;
    return 0;
}

static int raw_frame(const uint8_t *pkg, size_t pkg_size, int frame,
                     const uint8_t **pixels, int *width, int *height)
{
    uint32_t off, next;

    if (!pkg || !pixels || !width || !height || frame < 0
        || pkg_size < 14u + 4u * (size_t)frame)
        return -1;
    off = rd_u32(pkg + 6u + 4u * (size_t)frame);
    next = rd_u32(pkg + 10u + 4u * (size_t)frame);
    if (off > next || next > pkg_size || next - off < 4u)
        return -1;
    *width = rd_u16(pkg + off);
    *height = rd_u16(pkg + off + 2u);
    if (*width <= 0 || *height <= 0
        || (size_t)(*width) * (size_t)(*height) > next - off - 4u)
        return -1;
    *pixels = pkg + off + 4u;
    return 0;
}

static int dialog_raw_blit(int frame, uint8_t *dst, int pitch)
{
    const uint8_t *pixels;
    int width, height;

    if (!dst || raw_frame(g_pkg_fdother_5, (size_t)g_pkg_fdother_5_size,
                           frame, &pixels, &width, &height) != 0) {
        fprintf(stderr, "dialog: malformed FDOTHER frame %d\n", frame);
        return -1;
    }
    for (int row = 0; row < height; row++)
        memcpy(dst + (size_t)row * (size_t)pitch,
               pixels + (size_t)row * (size_t)width, (size_t)width);
    return 0;
}

/* Exact frame placement from sub_168B6.  cols=19/rows=5 produces the
 * 310x86 dialogue box; smaller dimensions are the original opening steps. */
static int dialog_compose(uint8_t *screen, int x, int y, int cols, int rows)
{
    uint8_t *base;

    if (!screen || x < 0 || y < 0 || cols < 2 || rows < 2
        || x + 16 * cols + 6 > FD2_SCREEN_W
        || y + 16 * rows + 6 > FD2_SCREEN_H)
        return -1;
    base = screen + (size_t)y * FD2_SCREEN_W + x;
    if (dialog_raw_blit(1, base, FD2_SCREEN_W) != 0
        || dialog_raw_blit(2, base + 16 * cols + 3, FD2_SCREEN_W) != 0
        || dialog_raw_blit(3, base + 3 * FD2_SCREEN_W + 16 * rows * FD2_SCREEN_W,
                           FD2_SCREEN_W) != 0
        || dialog_raw_blit(4, base + 3 * FD2_SCREEN_W + 16 * rows * FD2_SCREEN_W
                           + 16 * cols + 3, FD2_SCREEN_W) != 0
        || dialog_raw_blit(5, base + 3, FD2_SCREEN_W) != 0
        || dialog_raw_blit(6, base + 19 + 16 * (cols - 2), FD2_SCREEN_W) != 0
        || dialog_raw_blit(7, base + 3 * FD2_SCREEN_W + 16 * rows * FD2_SCREEN_W + 3,
                           FD2_SCREEN_W) != 0
        || dialog_raw_blit(8, base + 3 * FD2_SCREEN_W + 16 * rows * FD2_SCREEN_W
                           + 19 + 16 * (cols - 2), FD2_SCREEN_W) != 0)
        return -1;
    /* 右轨 x = 16*(cols-2)+35（0x169AE v15 = v11+19+16*(a9-2) 同族，
     * 35 = 3+16*2 角宽）——相对量，此前硬编码 307（仅 cols=19 正确，
     * 开窗动画中间尺寸右轨画到框外=框动画伪影根因之一）。 */
    {
        int rail_x = 16 * (cols - 2) + 35;
        if (dialog_raw_blit(15, base + 3 * FD2_SCREEN_W + rail_x,
                            FD2_SCREEN_W) != 0
            || dialog_raw_blit(17, base + (3 + 16 * (rows - 1)) * FD2_SCREEN_W
                               + rail_x, FD2_SCREEN_W) != 0)
            return -1;
    }
    if (dialog_raw_blit(14, base + 3 * FD2_SCREEN_W, FD2_SCREEN_W) != 0
        || dialog_raw_blit(16, base + (3 + 16 * (rows - 1)) * FD2_SCREEN_W,
                           FD2_SCREEN_W) != 0)
        return -1;

    for (int col = 0; col < cols - 2; col++) {
        if (dialog_raw_blit(9, base + 19 + 16 * col, FD2_SCREEN_W) != 0
            || dialog_raw_blit(12, base + (3 + 16 * rows) * FD2_SCREEN_W
                               + 19 + 16 * col, FD2_SCREEN_W) != 0)
            return -1;
    }
    for (int row = 0; row < rows - 1; row++) {
        if (dialog_raw_blit(10, base + (16 * (row + 1)) * FD2_SCREEN_W,
                            FD2_SCREEN_W) != 0
            || dialog_raw_blit(11, base + (16 * (row + 1)) * FD2_SCREEN_W
                               + 16 * cols + 3, FD2_SCREEN_W) != 0)
            return -1;
    }
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
            if (dialog_raw_blit(13, base + (3 + 16 * row) * FD2_SCREEN_W
                                + 3 + 16 * col, FD2_SCREEN_W) != 0)
                return -1;
    return 0;
}

static int dato_frame(const uint8_t *dato, size_t dato_size, int frame,
                      const uint8_t **stream, const uint8_t **stream_end,
                      int *width, int *height)
{
    uint32_t off, next, dir0;

    /* DATO blocks differ from FDOTHER: their frame directory starts at
     * byte 0, without the common six-byte package prefix.  All 137 blocks
     * carry exactly four 80x80 frames (dir[0]==16). */
    if (!dato || !stream || !stream_end || !width || !height
        || dato_size < 8u || frame < 0)
        return -1;
    dir0 = rd_u32(dato);
    if (dir0 < 8u || dir0 > dato_size
        || 4u * (size_t)frame + 4u > (size_t)dir0)
        return -1;
    off = rd_u32(dato + 4u * (size_t)frame);
    next = (4u * (size_t)frame + 8u <= (size_t)dir0)
         ? rd_u32(dato + 4u * (size_t)frame + 4u)
         : (uint32_t)dato_size;
    if (off > next || next > dato_size || next - off < 4u)
        return -1;
    *width = rd_u16(dato + off);
    *height = rd_u16(dato + off + 2u);
    if (*width <= 0 || *height <= 0 || *width > 80 || *height > 80)
        return -1;
    *stream = dato + off + 4u;
    *stream_end = dato + next;
    return 0;
}

/* 0x4EBFF/0x4EC31（2026-09-07 反汇编定案；2026-09-06 参数化 origin）：
 * DATO 帧（80x80 RLE）**不透明** blit。origin==36887（底窗内左位）走
 * 0x4EC31 镜像（行内自 origin 向左，面向右侧文本），其余含顶窗 1832
 * 与五店主大肖像位（4283/1707/3939/1398/3644）走 0x4EBFF 正向。 */
static int dato_stamp_ex(uint8_t *dst, const uint8_t *dato, size_t dato_size,
                         int origin, int frame, int mirror)
{
    const uint8_t *p, *pend;
    int width, height;
    uint8_t color = 0, repeat = 0;

    if (dato_frame(dato, dato_size, frame, &p, &pend, &width, &height) != 0) {
        fprintf(stderr, "dialog: malformed DATO frame %d\n", frame);
        return -1;
    }
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            ptrdiff_t at;
            if (repeat) {
                repeat--;
            } else {
                uint8_t code;
                if (p >= pend)
                    return -1;
                code = *p++;
                if (code <= 0xC0) {
                    color = code;
                } else {
                    repeat = (uint8_t)(code - 0xC1);
                    if (p >= pend)
                        return -1;
                    color = *p++;
                }
            }
            at = (ptrdiff_t)origin + row * FD2_SCREEN_W
                 + (mirror ? -col : col);
            if (at < 0 || at >= FD2_VRAM_SIZE)
                return -1;
            dst[at] = color;          /* 不透明：0 也写 */
        }
    }
    return 0;
}

static int dato_stamp(uint8_t *dst, const uint8_t *dato, size_t dato_size,
                      int origin, int frame)
{
    /* dato_frame_stamp 规则（0x1657E）：仅标准框内位 36887 镜像。 */
    return dato_stamp_ex(dst, dato, dato_size, origin, frame,
                         origin == 36887);
}

/* 0x12C60 find_speaker(char_id)：实体表扫 rec[8]==id——flag1 未置的首个
 * 命中为"场上活跃"（原版返回其下标，v41=2/112 聚焦）；否则记最后命中
 * （含 flag1 置位者，对应 dword_53C1B）；实体表全无命中才扫 roster 末
 * 命中。返回 1=场上活跃 / 0=仅档案或未命中。 */
static int dialog_find_speaker(int char_id, uint8_t **record)
{
    uint8_t *last = NULL;

    *record = NULL;
    if (g_ent_table) {
        for (int i = 0; i < g_ent_count; i++) {
            uint8_t *rec = g_ent_table[i];
            if (rec[8] == (uint8_t)char_id) {
                last = rec;
                if (!(rec[5] & 1u)) {
                    *record = rec;
                    return 1;
                }
            }
        }
    }
    if (!last && g_roster_table) {
        for (int i = 0; i < g_roster_count; i++)
            if (g_roster_table[i][8] == (uint8_t)char_id)
                last = g_roster_table[i];
    }
    *record = last;
    return 0;
}

/* 0x15E9E + 0x15E71（2026-09-07 定案）：blk5 帧0（24x24 **字面**帧，
 * 非 RLE——帧距 580=4+576 实证；0x4ED4F 逐字节读、非零写）标记精灵
 * 沿插值路径飞行动画。开窗（0x165F2）：(24vx+4,24vy+4)→(5,wy)；
 * 关窗（0x16BA2）：(5,wy)→(24vx+4,24vy+4)。原版每步：保存 24x24
 * 背景 → 画 → delay(10) → kbd_flush → 恢复。宿主等价实现。 */
static void dialog_marker_fly(int fx, int fy, int tx, int ty)
{
    const uint8_t *fr;
    uint8_t save[24 * 24];
    uint8_t *screen = vram_base();
    /* 0x1660A v8 = view_x + view_y（**格距**步数，非像素距离）——
     * 每步像素位移 = 全程/格数（大跳步）。此前误用像素曼哈顿距离
     * （慢 20-30 倍且位移摊薄成蠕动）。 */
    int n = (int)g_cursor_view_x + (int)g_cursor_view_y;
    int w, h;

    if (n <= 0)
        return;
    fr = package_frame_ptr(g_death_fx_pkg, 0);
    if (!fr)
        return;
    w = fr[0] | (fr[1] << 8);
    h = fr[2] | (fr[3] << 8);
    if (w != 24 || h != 24)
        return;
    for (int i = 0; i <= n; i++) {
        /* 0x1661E/0x16C16 整数插值：step = i*(from-to)/n */
        int mx = fx - i * (fx - tx) / n;
        int my = fy - i * (fy - ty) / n;

        if (mx < 0 || my < 0 || mx + w > FD2_SCREEN_W || my + h > FD2_SCREEN_H)
            continue;
        for (int r = 0; r < h; r++)
            memcpy(save + w * r,
                   screen + (size_t)(my + r) * FD2_SCREEN_W + mx, w);
        for (int r = 0; r < h; r++) {
            const uint8_t *src = fr + 4 + w * r;
            uint8_t *dst = screen + (size_t)(my + r) * FD2_SCREEN_W + mx;
            for (int c = 0; c < w; c++)
                if (src[c])
                    dst[c] = src[c];
        }
        delay_ms(10);
        kbd_flush();
        for (int r = 0; r < h; r++)
            memcpy(screen + (size_t)(my + r) * FD2_SCREEN_W + mx,
                   save + w * r, w);
    }
}

static int dialog_begin(int x, int y, int top, int focus)
{
    if (s_dialog.active) {
        fprintf(stderr, "dialog: attempted to open while another dialog is active\n");
        return -1;
    }
    if (focus) {
        g_text_busy = 0;
        cursor_scroll_to(x, y);
        /* 0x165AC：聚焦后原版场图已在新机位（开窗循环作用于新画面）。
         * 宿主需重合成一次；快照也必须在其后取，否则关窗恢复旧机位。 */
        anim_tick_update(0);
        dialog_marker_fly(24 * (int)g_cursor_view_x + 4,
                          24 * (int)g_cursor_view_y + 4, 5, top ? 2 : 112);
        /* IDA 0x165DE: dialog_open_anim always re-enables the normal
         * battle cursor-marker mode after focusing the speaker.  Restoring
         * the incoming value is wrong after a preceding caller deliberately
         * cleared it for a cutscene line; chapter 1 relies on this write to
         * make the selector visible when the final line closes. */
        g_text_busy = 1;
    }
    memcpy(s_dialog.screen, vram_base(), FD2_VRAM_SIZE);
    /* 0x165AC 开窗动画：panel9_grid 五步渐进 (4x2)(8x3)(12x4)(16x5)(19x5)
     * 每步 delay(10)。rle_decoder_init 的 310x86 背景条保存（dword_53A18
     * 五缓冲）由上面的全屏快照等价替代。0x165AC 不贴初始肖像——帧0
     * 由调用方（render_tokens 0x161F9/0x162DE 或场景页 dato_frame_stamp）
     * 在窗开后盖。 */
    {
        static const int steps[5][2] = {
            { 4, 2 }, { 8, 3 }, { 12, 4 }, { 16, 5 }, { 19, 5 }
        };
        int wy = top ? 2 : 112;

        for (int s = 0; s < 5; s++) {
            if (dialog_compose(vram_base(), 5, wy,
                               steps[s][0], steps[s][1]) != 0)
                return -1;
            if (s < 4)
                delay_ms(10);            /* 0x16738..0x16846：4 个步间 delay */
        }
        kbd_flush();                     /* 0x1684E：末帧收尾 */
    }
    s_dialog.active = 1;
    s_dialog.mode = 0;
    s_dialog.top = top;
    s_dialog.focus_wy = focus ? (top ? 2 : 112) : 0;
    return 0;
}

static void dialog_close(void)
{
    int wy, fly;

    if (!s_dialog.active)
        return;
    wy = s_dialog.top ? 2 : 112;
    fly = s_dialog.focus_wy;    /* 0x16BA2：a6!=0（聚焦窗）才有飞出 */
    /* 0x16B43 关窗动画：pkg_frame_free 逆序恢复五缓冲（每步 delay(10)，
     * buf[0] 无延时）——视觉 = 16x5→12x4→8x3→4x2→清场。宿主以
     * 快照恢复 + 逐级重画等价。 */
    {
        static const int steps[5][2] = {
            { 4, 2 }, { 8, 3 }, { 12, 4 }, { 16, 5 }, { 19, 5 }
        };

        for (int s = 3; s >= 0; s--) {
            memcpy(vram_base(), s_dialog.screen, FD2_VRAM_SIZE);
            if (dialog_compose(vram_base(), 5, wy,
                               steps[s][0], steps[s][1]) != 0)
                break;
            delay_ms(10);
        }
    }
    memcpy(vram_base(), s_dialog.screen, FD2_VRAM_SIZE);
    s_dialog.active = 0;
    s_dialog.focus_wy = 0;
    /* 0x16BA2..0x16C45：飞出 = (5,wy)→光标屏幕位，与飞入同插值/步序 */
    if (fly)
        dialog_marker_fly(5, fly, 24 * (int)g_cursor_view_x + 4,
                          24 * (int)g_cursor_view_y + 4);
}

/* 0x16E24 族：对白窗文本上滚一行（升级窗成长行 line==3 特调消费，
 * 0x1E529 stat_growth_roll）。2026-09-08 起导出供 battle.c 使用。 */
int dialog_scroll_text(void)
{
    ptrdiff_t base = (s_portrait_origin == 1832) ? 2895 : 38175;
    uint8_t *screen = vram_base();

    if (!s_dialog.active)
        return 0;
    for (int pass = 0; pass < 5; pass++) {
        for (int row = 0; row < 72; row++)
            memmove(screen + base + FD2_SCREEN_W * row - 1,
                    screen + base + FD2_SCREEN_W * (row + 3) - 1, 208);
        memset(screen + base + 23040, 74, 208);
    }
    for (int row = 0; row < 72; row++)
        memmove(screen + base + FD2_SCREEN_W * row - 1,
                screen + base + FD2_SCREEN_W * (row + 4) - 1, 208);
    memset(screen + base + 23040, 74, 208);
    return 0;
}

static int text_stream(const void *pkg, int str_id, const uint8_t **out,
                       const uint8_t **end)
{
    const uint8_t *base = pkg;
    size_t size = package_size(pkg);
    int16_t off;

    if (!base || str_id < 0 || size < 2
        || (size_t)str_id > (size - 2u) / 2u)
        return -1;
    off = (int16_t)rd_u16(base + 2u * (size_t)str_id);
    if (off < 0 || (size_t)off > size - 2u)
        return -1;
    *out = base + off;
    *end = base + size;
    return 0;
}

/* 0x4ED7A glyph renderer（2026-09-07 反汇编全解，纠正配色）：
 * 实参 (pkg, glyph, dst, pitch, 205, 76, 74) = fg=205、影子=76、底=74。
 * ① bg!=0：先整格 16×16 填 bg（rep stosd×4/行 ×16 行）——
 * **glyph 10（空格）也铺底**（填充先于 glyph!=10 判断）；
 * ② 每 set 位：fg @ (r,c)；影子 76 @ (r+1,c-1) 与 (r+1,c)。
 * 2026-09-07 参数化：fg/bg 由调用方穿引（列表族 (0,fg) / 对话族
 * (74,205)——0x28197/0x18587/0x1CF4A/0x181DA/0x29AB2 五处压栈实证
 * 列表族 bg=0）；影子 76 全部已证调用点一致，保持常量。
 * 2026-09-07 症状④（文字应透明底却带色块）：bg=0 分支此前漏实现
 * （0x4EDC2 条件填充写成无条件）→ 已补 if(bg)；影子两写原版无
 * 行/列界护栏（末行溢写格下一行 / col=0 回绕 (r,pitch-1)），按原样。 */
static int glyph_blit(int glyph, uint8_t *dst, int pitch, uint8_t fg,
                      uint8_t bg)
{
    const uint8_t *font = g_pkg_fdother_4;
    const uint8_t shadow = 76;
    size_t off;

    if (!font || !dst || glyph < 0)
        return -1;
    /* 0x4EDC2 if(a7)：bg!=0 才整格铺底。bg=0（列表族五处实证）→ 透明
     * 底，空格整格不写。此前无条件 memset = 列表文字带黑色底块
     * （2026-09-07 症状④根因）。 */
    if (bg) {
        for (int row = 0; row < 16; row++)
            memset(dst + (size_t)row * (size_t)pitch, bg, 16);
    }
    if (glyph == 10)
        return 0;                           /* 空格：bg!=0 时仅铺底 */
    off = 32u * (size_t)glyph;
    if (off > (size_t)g_pkg_fdother_4_size
        || (size_t)g_pkg_fdother_4_size - off < 32u)
        return -1;

    for (int row = 0; row < 16; row++) {
        uint8_t *line = dst + (size_t)row * (size_t)pitch;
        uint16_t bits = rd_u16(font + off + 2u * (size_t)row);
        bits = (uint16_t)((bits << 8) | (bits >> 8));  /* MSB 先行 */
        for (int col = 0; col < 16; col++) {
            if (!(bits & (uint16_t)(0x8000u >> col)))
                continue;
            line[col] = fg;
            line[pitch + col] = shadow;      /* (r+1,c)：0x4EE23 无行界检查，末行溢写格下一行 */
            line[pitch + col - 1] = shadow;  /* (r+1,c-1)：0x4EE20 无列界检查，col=0 落 (r,pitch-1) */
        }
    }
    return 0;
}

/* 0x164E8 char_reveal_tick（2026-09-07 全解；2026-09-06 撤销 active
 * 守卫——原版 0x164A2 无条件调用，场景窗/菜单期均由它驱动口型）：
 * 每 2 字推进一次 53A10（0→1→2→3 回绕，3 映射回 1 = 0,1,2,1 乒乓），
 * dato_frame_stamp(frame) 把说话人口型盖到 s_portrait_origin。
 * 计数器跨串/跨窗不复位。随后 sfx_play(blk31,2) + wait 1 tick。 */
static void char_reveal_tick(void)
{
    static unsigned char s_reveal_chars;    /* 0x53A14 */
    static unsigned char s_portrait_frame;  /* 0x53A10 */

    if (++s_reveal_chars == 2) {
        if (++s_portrait_frame == 4)
            s_portrait_frame = 0;
        {
            int frame = s_portrait_frame == 3 ? 1 : (int)s_portrait_frame;

            (void)dato_frame_stamp(frame);
        }
        s_reveal_chars = 0;
    }
    sfx_play(g_pkg_fdother_31, 2, 1);
    wait_bios_ticks(1);
}

/* 场景合成（0x265EC）目标 = 456 影带 +109764；影带是 malloc 的屏外
 * 工作面——原版每个 compose 仅尾部一次 blit_rows 312x192@band+32904
 * → VRAM+1284 呈现完整帧（文本在窗内：109764-32904 = 456*168+252，
 * 行 168<192、列 252<312）。字形先写入影带，显示后端的 VGA 扫描器
 * 只会观察最终复制到 320 像素面的内容，因此不会暴露影带内部中间态。 */

static int render_tokens(const void *pkg, int str_id, uint8_t *dst,
                         int pitch, int typewriter, int depth,
                         uint8_t fg, uint8_t bg, uint8_t **end_cursor)
{
    const uint8_t *p, *end;
    uint8_t *cursor = dst;
    uint8_t *line_start = dst;
    int line = 0;
    int dato_active = s_dialog.active;  /* 全局窗状态（原版 dword_53C67） */
    int opened_here = 0;                /* 原版 v42：本层开的窗，嵌套层恒 0 */

    if (depth > 8 || text_stream(pkg, str_id, &p, &end) != 0) {
        fprintf(stderr, "FDTXT: invalid package/string id %d\n", str_id);
        return -1;
    }

    while (p + 2 <= end) {
        int16_t token = (int16_t)rd_u16(p);
        p += 2;
        if (token == -1) {
            /* 0x164AC：仅当本层开过窗（v42）才等键+关窗，与打字机
             * 标志无关；嵌套层 -1 直接返回。0x164B3 dato_frame_stamp(0)：
             * 说话窗在场时先盖回中性帧 0（口型复位）再等键。 */
            if (opened_here) {
                dato_frame_stamp(0);
                wait_key_anim(0);
                dialog_close();
                s_portrait_origin = 0;  /* 0x164D7：串尾关窗后 53C67=0 */
                /* 0x15F84 不写 g_text_busy；战场标记模式由调用者保留。 */
            }
            if (end_cursor)
                *end_cursor = cursor;
            return 0;
        }
        if (token == -2 || token == -3) {
            /* 0x15FCD..0x15FE3：仅标准窗（53C67=1832/36887）滚行——
             * 店主大肖像位（1707 等）不滚，行继续下探。 */
            if ((s_portrait_origin == 1832 || s_portrait_origin == 36887)
                && line == 3) {
                if (dialog_scroll_text() != 0)
                    return -1;
                line--;
            }
            line++;
            cursor = line_start + (size_t)line * 19u * (size_t)pitch;
            if (token == -3) {
                if (s_portrait_origin == 1832 || s_portrait_origin == 36887)
                    dato_frame_stamp(0);   /* 0x16028：翻页前口型复位 */
                wait_key_anim(1);       /* 0x16034：无条件等键翻页 */
                typewriter = 1;         /* 0x1603C a14=1：翻页后重启打字机 */
            }
            continue;
        }
        if (token == -4 || token == -5) {
            /* -5 读 0x53ADD 第三寄存器（原版 0x1609D），非 g_disp_num_b；
             * 唯一写点 treasure 换装 0x1937B（旧道具名+181）。 */
            int nested = token == -4 ? g_disp_num_a : g_disp_num_c;
            /* 原版递归末参 a14=1：嵌套串打字机开（0x16073/0x160A3），
             * 但嵌套层 v42=0，其 -1 不关窗、不等待。颜色沿用本层。 */
            uint8_t *nested_end = cursor;

            if (render_tokens(g_pkg_fdtxt0, nested, cursor, pitch, 1,
                              depth + 1, fg, bg, &nested_end) != 0)
                return -1;
            /* The original recursive text_render_box leaves the caller's
             * cursor immediately after the expanded name (0x16073..0x16081).
             * Without this advance, the comma and following glyphs overwrite
             * the product/unit name supplied by g_disp_num_a/g_disp_num_c. */
            cursor = nested_end;
            continue;
        }
        if (token == -6) {
            char digits[16];
            int n = snprintf(digits, sizeof(digits), "%d", g_disp_num_b);
            if (n < 0 || n >= (int)sizeof(digits))
                return -1;
            for (int i = 0; i < n; i++, cursor += 16) {
                if (glyph_blit(digits[i] - '0', cursor, pitch, fg, bg) != 0)
                    return -1;
                if (kbd_key_avail())    /* 0x16121：有键待读 → 关打字机 */
                    typewriter = 0;
                if (typewriter)
                    char_reveal_tick();
            }
            continue;
        }
        if (token >= -20 && token <= -17) {
            uint16_t speaker;
            uint8_t *record = NULL;
            int active;
            int icon;
            int top = token == -17 || token == -19;
            if (p + 2 > end) {
                fprintf(stderr, "FDTXT: truncated DATO token in string %d\n",
                        str_id);
                return -1;
            }
            speaker = rd_u16(p);
            p += 2;
            if (opened_here) {
                /* 0x16152：等键后关上一层窗。0x16150 dato_frame_stamp(0)：
                 * 先盖回中性帧 0（当前说话人最后一字后的口型复位）。 */
                dato_frame_stamp(0);
                wait_key_anim(0);
                dialog_close();
                opened_here = 0;
            } else if (dato_active) {
                /* 场景底窗（dialog_backdrop_load）等价原版的句柄覆盖：
                 * 无声恢复后再开新窗。 */
                dialog_close();
            }
            if (token == -17 || token == -18) {
                active = dialog_find_speaker((int)speaker, &record);
                /* 0x1FDF4..0x1FE07 例外：char==39（旁白）直接以 39 为
                 * DATO 块号，不取记录图标；聚焦仍按 active 判定。 */
                icon = (speaker == 39) ? 39
                     : (record ? record[7] : -1);
            } else {
                if (!g_ent_table || speaker >= (uint16_t)g_ent_count) {
                    fprintf(stderr, "FDTXT: invalid entity speaker slot %u\n",
                            (unsigned)speaker);
                    return -1;
                }
                record = g_ent_table[speaker];
                active = 1;             /* -19/-20：场上实体槽，恒聚焦 */
                icon = record[7];
            }
            /* 0x16174/0x1625E/0x16397/0x16413：先设肖像位（顶 1832 /
             * 底 36887），装载说话人块，开窗后由 0x161F9/0x162DE 盖帧0
             * （顶正向 / 底镜像——dato_frame_stamp 的 origin 规则）。 */
            s_portrait_origin = top ? 1832 : 36887;
            s_speaker_blk = dat_load_block("DATO.DAT", s_speaker_blk, icon);
            s_speaker_blk_size = (size_t)g_last_block_size;
            /* 0x165AC：仅场上活跃（sub_12C60 != -1 → v41=2/112）才
             * cursor_scroll_to 聚焦说话人。 */
            if (icon < 0 || dialog_begin(record ? record[0] : 0,
                                         record ? record[1] : 0,
                                         top, active) != 0)
                return -1;
            dato_frame_stamp(0);        /* 0x161F9/0x162DE：开窗即闭口肖像 */
            dato_active = 1;
            opened_here = 1;            /* 原版 v42 = 开窗返回句柄 */
            typewriter = 1;             /* 0x16208 a14=1：开窗后重启打字机 */
            line = 0;
            /* 0x15F84 开窗后切换到固定 VGA 文本原点。 */
            line_start = vram_base() + (size_t)(top ? 2895u : 38175u);
            cursor = line_start;
            continue;
        }
        if (token < 0 || glyph_blit(token, cursor, pitch, fg, bg) != 0) {
            fprintf(stderr, "FDTXT: invalid glyph token %d in string %d\n",
                    token, str_id);
            return -1;
        }
        cursor += 16;
        /* 打字机开时每字后等 1 tick（sub_164E8），显示线程在这些原版
         * 延时期间自然观察到每个已写入显存的字形；列表族则在整串写完
         * 后才离开当前调用，和 VGA 直写的可见性契约一致。 */
        if (kbd_key_avail())            /* 0x16486：有键待读 → 关打字机 */
            typewriter = 0;
        if (typewriter)
            char_reveal_tick();         /* 0x1649C sub_164E8 */
    }
    fprintf(stderr, "FDTXT: unterminated string %d\n", str_id);
    return -1;
}

/* 0x15F84 真实压栈序（cdecl 右到左）：
 *   text_render_box(pkg, str_id, dst, pitch, fg, shadow=76, bg, a2, a14)
 * fd2re 形参映射（签名保持历史顺序，语义 2026-09-07 激活）：
 *   flag=a14（打字机初值） a2=行距原参（列表族 0——其串均单行，行距
 *   不可观测，实现保持 19 常量） y=bg h=fg w=pitch。
 * 两族实参（压栈五处实证）：对话框 (1,19,74,205)；列表/面板
 * (0,0,0,201/205)（0x29AB2 槽摘要 / 0x28197 名册网格 / 0x18587 道具格 /
 * 0x1CF4A 法术列表 / 0x181DA 单位面板）。 */
void text_render_box(int flag, int a2, int y, int h, int w,
                     uint8_t *dst, int str_id, const void *pkg)
{
    if (!dst || w <= 0) {
        fprintf(stderr, "FDTXT: invalid destination/pitch %d\n", w);
        return;
    }
    /* 0x15F84 has no write to 0x51A83.  That byte is the battle/state
     * animation gate and is set by its callers; list text (notably
     * save_slot_summary_draw@0x29AB2) must not leave it asserted while the
     * picker waits for the next key. */
    if (render_tokens(pkg, str_id, dst, w, flag, 0,
                      (uint8_t)h, (uint8_t)y, NULL) != 0)
        abort();
}

void icon_cache_reset(void)
{
    s_icon_count = 0;
    s_icon_next = ICON_CACHE_HEADER;
}

int icon_load_entry(int icon_idx, void *file)
{
    FILE *fp = file;
    uint8_t directory[6720];
    uint32_t offsets[ICON_FRAME_COUNT + 1];
    size_t stream_size;
    uint8_t *data;

    /* 6720-byte directory holds 12-frame offset runs for ids 0..138. */
    if (!fp || icon_idx < 0 || icon_idx >= ICON_SOURCE_COUNT) {
        fprintf(stderr, "FDICON: invalid icon request %d\n", icon_idx);
        return -1;
    }
    for (int i = 0; i < s_icon_count; i++)
        if (s_icon_ids[i] == icon_idx)
            return i;
    if (s_icon_count >= ICON_CACHE_MAX) {
        fprintf(stderr, "FDICON: cache capacity exceeded by icon %d\n", icon_idx);
        return -1;
    }
    if (fseek(fp, 6L, SEEK_SET) != 0
        || fread(directory, 1, sizeof(directory), fp) != sizeof(directory)) {
        fprintf(stderr, "FDICON: cannot read directory for icon %d\n", icon_idx);
        return -1;
    }
    for (int i = 0; i <= ICON_FRAME_COUNT; i++)
        offsets[i] = rd_u32(directory + 4u * (size_t)(ICON_FRAME_COUNT * icon_idx + i));
    if (offsets[0] >= offsets[ICON_FRAME_COUNT]) {
        fprintf(stderr, "FDICON: malformed offsets for icon %d\n", icon_idx);
        return -1;
    }
    for (int i = 1; i <= ICON_FRAME_COUNT; i++) {
        if (offsets[i - 1] > offsets[i]) {
            fprintf(stderr, "FDICON: non-monotonic offsets for icon %d\n", icon_idx);
            return -1;
        }
    }
    stream_size = (size_t)(offsets[ICON_FRAME_COUNT] - offsets[0]);
    if (!g_standing_sprites) {
        /* calloc：原版 DOS/4GW 新提交页清零，未写目录槽读到偏移 0；
         * CRT 堆是脏的——垃圾偏移会让 sprite24_stamp 越出精灵格写进
         * 相邻名字区（商店名册“部分字高亮/选中名不亮”症状）。 */
        g_standing_sprites = calloc(1u, ICON_CACHE_BYTES);
        if (!g_standing_sprites) {
            fprintf(stderr, "FDICON: cannot allocate standing-sprite cache\n");
            return -1;
        }
    }
    if (s_icon_next + stream_size > ICON_CACHE_BYTES) {
        fprintf(stderr, "FDICON: standing-sprite cache overflow for icon %d\n", icon_idx);
        return -1;
    }
    data = g_standing_sprites + s_icon_next;
    if (fseek(fp, (long)offsets[0], SEEK_SET) != 0
        || fread(data, 1, stream_size, fp) != stream_size) {
        fprintf(stderr, "FDICON: cannot load icon %d\n", icon_idx);
        return -1;
    }
    for (int i = 0; i <= ICON_FRAME_COUNT; i++)
        if (i < ICON_FRAME_COUNT)
            *(uint32_t *)(g_standing_sprites
                + 4u * (size_t)(ICON_FRAME_COUNT * s_icon_count + i))
                = (uint32_t)(data - g_standing_sprites + offsets[i] - offsets[0]);
    s_icon_next += stream_size;
    s_icon_ids[s_icon_count] = icon_idx;
    return s_icon_count++;
}

/* 名册网格/单列列表按【名册位置】读目录（0x2810B/0x2825B 的
 * 48*(i+scroll)+4*相位），而 icon_load_entry 沿用原版 0x11019 的
 * 按 id 去重（战斗路径 rec[2] 消费该语义）。转职后两角色 rec[7]
 *（职业）相同 → 场景入口重建时去重令槽位压缩 → position != slot。
 * 原版 DOS 下未写槽读到的是复用块内旧的有效偏移（free+同尺寸
 * remalloc 同块），fd2re 必须显式把已加载图标的目录项复制到名册
 * 位置槽，才能保住原版隐含的 position==slot 不变量。
 * 2026-09-08：商店名册“选中名不亮/上一角色部分字残留”根因修复。 */
int icon_directory_alias(int dst_slot, int icon_idx)
{
    int src = -1;

    if (!g_standing_sprites || dst_slot < 0 || dst_slot >= ICON_CACHE_MAX)
        return -1;
    for (int i = 0; i < s_icon_count; i++)
        if (s_icon_ids[i] == icon_idx) {
            src = i;
            break;
        }
    if (src < 0)
        return -1;
    for (int i = 0; i < ICON_FRAME_COUNT; i++)
        *(uint32_t *)(g_standing_sprites
            + 4u * (size_t)(ICON_FRAME_COUNT * dst_slot + i))
            = *(uint32_t *)(g_standing_sprites
                + 4u * (size_t)(ICON_FRAME_COUNT * src + i));
    return 0;
}

int icon_frame_get(int cache_idx, int frame, const uint8_t **stream,
                   size_t *stream_len)
{
    size_t start, end;
    if (!stream || !stream_len || cache_idx < 0 || cache_idx >= ICON_CACHE_MAX
        || frame < 0 || frame >= ICON_FRAME_COUNT || !g_standing_sprites)
        return -1;
    start = rd_u32(g_standing_sprites
        + 4u * (size_t)(ICON_FRAME_COUNT * cache_idx + frame));
    end = frame + 1 < ICON_FRAME_COUNT
        ? rd_u32(g_standing_sprites
            + 4u * (size_t)(ICON_FRAME_COUNT * cache_idx + frame + 1))
        : ICON_CACHE_BYTES;
    if (start < ICON_CACHE_HEADER || start >= end || end > ICON_CACHE_BYTES)
        return -1;
    *stream = g_standing_sprites + start;
    *stream_len = end - start;
    return 0;
}

/* 0x16559 dato_frame_stamp（2026-09-06 定名重建；旧名 text_str_get——
 * 名字与实现都误植成"取串指针"，全游戏口型/肖像复位因此空转，本批
 * 撤销）：ebx = 0xA0000 + dword_53C67；流 = 说话人块[dir[frame]]；
 * 53C67==0x9017 → 镜像贴，否则正向。返回 0=已贴 / -1=无块或坏帧。 */
int dato_frame_stamp(int frame)
{
    if (!s_speaker_blk)
        return -1;
    return dato_stamp(vram_base(), s_speaker_blk, s_speaker_blk_size,
                      s_portrait_origin, frame);
}

/* 0x16C57 wait_key_anim 全解（2026-09-07，mode 此前被忽略）：
 * mode==1：等待前在 窗基(顶 658255/其余 693535=53C67) + v15(有战场图
 * 18336/无 18288) + 1600 处画 blk5 帧18（14x10 RAW 指示符）；等待期
 * 每 2 BIOS tick 计一次，满 3 次（≈6 tick）帧 18↔19 交替闪烁；按键后
 * 在 窗基+v15 处盖帧 13（16x16 格底，覆盖行域含指示符）擦除。
 * mode==0 无指示符。两模式等键期均泵 DATO 肖像（0x16C8E..0x16D3E）：
 * 随机 2..31 tick 后 dato_frame_stamp(3) 一拍，再随机间隔帧 0 还原
 * （状态页人物眨眼、对话店主口型共用）。末尾复用键盘读取路径并返回
 * 规范化扫描码。 */
int wait_key_anim(int mode)
{
    /* 0x16C57：窗基 = 53C67==1832 ? 658255 : 693535（顶 2895 / 其余
     * 38175——含店主大肖像位，均按底窗文本区取基）。 */
    ptrdiff_t base = (s_portrait_origin == 1832) ? 2895 : 38175;
    ptrdiff_t off = g_field_map ? 18336 : 18288;
    uint8_t *ind = vram_base() + base + off + 1600;
    uint8_t *erase = vram_base() + base + off;
    int frame = 18, blink = 0;
    /* 0x16C8E：DATO 肖像泵初值——闭态，随机 2..31 tick 后打一拍帧 3
     * （0x16D1B 帧闭还原），mode 0/1 均运行（状态页 0x17B06 等键期
     * 的眨眼/口型即此）。 */
    int mouth_open = 0;
    int countdown = (int)(fd2_rand() % 30) + 2;
    uint32_t last = bios_tick();

    if (mode == 1) {
        dialog_raw_blit(18, ind, FD2_SCREEN_W);
    }
    while (!kbd_key_avail()) {
        idle_pump();
        uint32_t now = bios_tick();
        if ((int32_t)(now - last) >= 2) {
            last = now;
            if (mode == 1 && ++blink == 3) {
                blink = 0;
                if (++frame == 20)
                    frame = 18;
                dialog_raw_blit(frame, ind, FD2_SCREEN_W);
            }
            if (mouth_open) {
                if (dato_frame_stamp(0) == 0)
                countdown = (int)(fd2_rand() % 30) + 2;
                mouth_open = 0;
            } else if (countdown-- == 0) {
                /* 0x16D1B dec/test 判等（非 <=0）：倒计时归零才触发，
                 * 之后重新取零扩展 fd2_rand()%30+2。 */
                if (dato_frame_stamp(3) == 0)   /* 一拍眨眼/张口 */
                mouth_open = 1;
            }
        }
        /* 0x16C57 原版此处只有 idle_pump，没有 anim_tick_update。
         * wait_key_anim 直接改 VRAM 中的肖像；额外重合成战场层会把
         * 刚贴上的 DATO 帧覆盖掉，导致等待期口型/眨眼不可见。 */
        wait_bios_ticks(1);
    }
    int scan = input_wait_key();
    if (mode == 1) {
        dialog_raw_blit(13, erase, FD2_SCREEN_W);
    }
    return scan;
}

/* 0x1E5C0 wait_key_ticks(ticks)：等键或超时（逐指令定案 2026-09-08）。
 * do { idle_pump(); 键可用 → 出 }，tick 差 >= ticks 或午夜回绕亦出；
 * 出口 kbd_flush。 */
void wait_key_ticks(int ticks)
{
    uint32_t start = bios_tick();

    for (;;) {
        idle_pump();
        if (kbd_key_avail())
            break;
        uint32_t now = bios_tick();
        if (now - start >= (uint32_t)ticks || now < start)
            break;
    }
    kbd_flush();
}

/* 0x19602..0x19660 说话人→肖像位 switch：五店主（DATO 128..132）
 * 的大肖像浮于场景上部（非镜像）；缺省 36887 = 底窗内左位（镜像，
 * 仅此位在滑入带内随框可见）。 */
static int portrait_origin_for(int variant)
{
    switch (variant) {
    case 128: return 4283;   /* 商店1：row13 col123 */
    case 129: return 1707;   /* 酒馆：row5 col107 */
    case 130: return 3939;   /* 商店3：row12 col99 */
    case 131: return 1398;   /* 整备：row4 col118 */
    case 132: return 3644;   /* 隐藏店：row11 col124 */
    default:  return 36887;  /* 通用：底窗内左 row115 col87 */
    }
}

/* sub_1974C：work←snap 全屏；work 行 row.. 段并 box 行 112.. 的
 * 310 宽框带（col5 起，86 行或 200-row 截断）；VRAM←work。原版
 * 步间无延时（0x196A9..0x196C7 六连调；选人器 0x27DE9/0x269A8 同）。 */
void dialog_band_blit(int row, uint8_t *work, const uint8_t *box,
                      const uint8_t *snap)
{
    int rows = row + 86 >= 200 ? 200 - row : 86;

    memcpy(work, snap, FD2_VRAM_SIZE);
    for (int i = 0; i < rows; i++)
        memcpy(work + 5 + 320 * (row + i), box + 35845 + 320 * i, 310);
    memcpy(vram_base(), work, FD2_VRAM_SIZE);
}

uint8_t *dialog_backdrop_snapshot(void)
{
    return (s_dialog.active && s_dialog.mode) ? s_dialog.backdrop_snap : NULL;
}

/* 0x1956B dialog_backdrop_load（场景版对白窗，2026-09-06 全解重建）：
 * malloc 三缓冲 → snap=VRAM、box=snap → panel9_grid(5,112,19,5) 整框
 * 合成进 box → 肖像位表设 53C67 → dat_load_block("DATO.DAT",variant)
 * （句柄跨窗存活）→ 帧0 镜像盖进 box+origin（仅缺省位落在滑入带内
 * 可见；店主位在带外，其大肖像由后续 dato_frame_stamp 上屏）→
 * 6 步滑入（行 177,164,151,138,125,112 步距 13）。 */
void dialog_backdrop_load(int variant)
{
    uint8_t *snap, *box, *work;

    if (s_dialog.active) {
        /* 原版此时直接重复 malloc（三缓冲泄漏）；宿主收拢为收尾重开。
         * menu_buffers_teardown 双半边：顺带收选人器三缓冲。 */
        if (s_dialog.mode)
            menu_buffers_teardown();
        else
            dialog_close();
    } else if (g_menu_snap_buf) {
        /* 选人器/列表三缓冲占用中（原版同样直开=泄漏）：同收拢护栏。 */
        menu_buffers_teardown();
    }
    s_portrait_origin = portrait_origin_for(variant);
    s_speaker_blk = dat_load_block("DATO.DAT", s_speaker_blk, variant);
    s_speaker_blk_size = (size_t)g_last_block_size;

    snap = malloc(FD2_VRAM_SIZE);
    box = malloc(FD2_VRAM_SIZE);
    work = malloc(FD2_VRAM_SIZE);
    if (!snap || !box || !work) {
        free(snap);
        free(box);
        free(work);
        return;
    }
    memcpy(snap, vram_base(), FD2_VRAM_SIZE);
    memcpy(box, snap, FD2_VRAM_SIZE);
    if (dialog_compose(box, 5, 112, 19, 5) != 0
        || (s_speaker_blk
            && dato_stamp(box, s_speaker_blk, s_speaker_blk_size,
                          s_portrait_origin, 0) != 0)) {
        free(snap);
        free(box);
        free(work);
        return;
    }
    for (int i = 5; i >= 0; i--) {
        dialog_band_blit(13 * i + 112, work, box, snap);
    }
    s_dialog.backdrop_snap = snap;
    s_dialog.backdrop_box = box;
    s_dialog.backdrop_work = work;
    s_dialog.active = 1;
    s_dialog.mode = 1;
    s_dialog.top = 0;
    s_dialog.focus_wy = 0;
}

/* 0x196CB dialog_backdrop_restore：下滑收尾（行 125..177 五步）+
 * 快照恢复 + free 三缓冲 + anim_tick_update(0)（战场在场时重绘图）。
 * 53C67 与说话人块不清——跨窗存活（原版语义）。 */
void dialog_backdrop_restore(void)
{
    menu_buffers_teardown();
    anim_tick_update(0);
}

/* 0x26996 menu_buffers_teardown：同收尾无战场重绘——场景页循环每轮
 * （0x293DF/0x27B75 等）在菜单收板后调用，动作分发前框已隐去。
 * 2026-09-08 扩为双半边：原版同一函数操作全局三缓冲，对话框
 * （0x1956B）与选人器/列表（0x27D33/0x27F4A/0x27738 族）共用；宿主
 * 侧对话框缓冲在 s_dialog、选人器三缓冲在 g_menu_* 三件套，此处
 * 分别收，各自守卫，重复调用无害。 */
void menu_buffers_teardown(void)
{
    if (s_dialog.active && s_dialog.mode) {
        for (int i = 1; i < 6; i++) {
            dialog_band_blit(13 * i + 112, s_dialog.backdrop_work,
                             s_dialog.backdrop_box, s_dialog.backdrop_snap);
        }
        memcpy(vram_base(), s_dialog.backdrop_snap, FD2_VRAM_SIZE);
        free(s_dialog.backdrop_snap);
        free(s_dialog.backdrop_box);
        free(s_dialog.backdrop_work);
        s_dialog.backdrop_snap = NULL;
        s_dialog.backdrop_box = NULL;
        s_dialog.backdrop_work = NULL;
        s_dialog.active = 0;
    }
    /* 选人器/列表半边（0x269A8..0x26A08 同序）：面板带下滑收起 →
     * VRAM←开面板前快照 → free 三件套。 */
    if (g_menu_snap_buf) {
        for (int i = 1; i < 6; i++) {
            dialog_band_blit(13 * i + 112, g_menu_work_buf,
                             g_screen_backup, g_menu_snap_buf);
        }
        memcpy(vram_base(), g_menu_snap_buf, FD2_VRAM_SIZE);
        free(g_menu_work_buf);
        free(g_menu_snap_buf);
        free(g_screen_backup);
        g_menu_work_buf = NULL;
        g_menu_snap_buf = NULL;
        g_screen_backup = NULL;
    }
}

int dialog_portrait_origin(void)
{
    return s_portrait_origin;
}

void dialog_portrait_origin_set(int origin)
{
    s_portrait_origin = origin;
}

const uint8_t *dialog_speaker_reload(int variant)
{
    s_speaker_blk = dat_load_block("DATO.DAT", s_speaker_blk, variant);
    s_speaker_blk_size = (size_t)g_last_block_size;
    return (const uint8_t *)s_speaker_blk;
}

/* ====================================================================
 * 0x19953 confirm_yes_no / 0x197E5 confirm_close_anim（2026-09-06
 * 全解重建；旧 confirm_yn 静默取键近似撤销——是/否按钮视觉挂号关闭）
 *
 * 数据：g_confirm_btn_ids@0x51EED = {16,17}（unk_51EE5 同对）；FDOTHER
 * blk2 帧目录在块首 +0（u32×N，与 blk5 的 +6 头不同族），每钮三帧组
 * 帧=3*id：+0 常态/闪烁A、+1 闪烁B（+2 高亮，径向菜单族用），24×16
 * RAW 透明（0x4ED34）。按钮基位 = 影带+0x1A59C（带 row164 col244 =
 * 屏 168,248）；开场 4 步自 (−4,+4) 分开到 (−16,+16) 步距 4 无延时，
 * 收尾反向收敛到 (−4,+4)。带合成：框带 86 行（box+35845 → 带+32905+
 * 456*(i+108)）/场景全屏 200 行（box+320*i → 带+32900+456*(i−4)）；
 * 战场（field_map>1）另做 field_render_viewport(13x8)+field_render_
 * entities；blit_rows(带+32904 → VRAM+1284, 320/456, 312x192)。
 * 等待期每 ≥2 tick：blink 0..3 回绕，选中钮帧 += blink/2；店主口型
 * 帧0（闭，rand%30+10）/帧3（张单拍，rand%30+2 起算）盖进 box+
 * s_portrait_origin（镜像 iff field_map!=0——0x19BA4 与 dato_frame_
 * stamp 的 36887 规则在实景两域一致）。键：Enter(28)/Space(57)=返1、
 * Esc(1)/Del(83)=返-1、←/→=choice 0/1（回环），命中后 blink 清零。
 * ==================================================================== */
static const uint32_t s_confirm_btn_ids[2] = { 16, 17 };  /* 0x51EED */
static uint8_t *s_confirm_scratch;   /* 无场景窗时的 backup 代偿缓冲 */

static const uint8_t *blk2_frame_ptr(int idx)
{
    const uint8_t *pkg = (const uint8_t *)g_pkg_fdother_2;
    uint32_t off;

    if (!pkg || idx < 0 || idx >= 78)
        return NULL;
    off = rd_u32(pkg + 4u * (size_t)idx);
    return pkg + off;
}

/* confirm 专用 backup（g_screen_backup 等价：0x1999C 无条件以当前屏
 * 覆盖——含场景窗框缓冲，收尾 slide-out 因此带最后文本态）。 */
static uint8_t *confirm_backup(void)
{
    if (s_dialog.active && s_dialog.mode)
        return s_dialog.backdrop_box;
    free(s_confirm_scratch);
    s_confirm_scratch = malloc(FD2_VRAM_SIZE);
    return s_confirm_scratch;
}

static void confirm_band_refresh(void)
{
    uint8_t *band = battle_shadow_layer();

    if (g_field_map && (uintptr_t)g_field_map > 1) {
        field_phase_tick();
        field_render_viewport(band + 32904, 456);
        field_render_entities(band);
    }
}

int confirm_yes_no(void)
{
    uint8_t *box = confirm_backup();
    uint8_t *band;
    uint8_t *btn;
    int x[2] = { 0, 0 };
    int flap_open = 0;
    int countdown = (int)(fd2_rand() % 30) + 2;
    int blink = 0;                    /* 0x53A8D g_menu_blink */
    static uint32_t s_last_tick;      /* g_last_bios_tick 族锚 */

    if (!box)
        return -1;
    band = battle_shadow_layer();
    btn = band + 107932;              /* 0x1A59C：带(164,244)=屏(168,248) */
    memcpy(box, vram_base(), FD2_VRAM_SIZE);
    g_menu_choice = 0;
    /* 0x199C8：全屏 200 行进带（带行 i-4）+ 战场层。 */
    for (int i = 0; i < 200; i++)
        memcpy(band + 32900 + 456 * (i - 4), box + 320 * i, 320);
    confirm_band_refresh();
    /* 0x19A44 开场：先调偏移再合成，4 步分开（无延时）。 */
    for (int step = 0; step < 4; step++) {
        x[0] -= 4;
        x[1] += 4;
        for (int i = 0; i < 86; i++)
            memcpy(band + 32905 + 456 * (i + 108),
                   box + 35845 + 320 * i, 310);
        for (int k = 0; k < 2; k++) {
            const uint8_t *fr = blk2_frame_ptr(3 * (int)s_confirm_btn_ids[k]);

            if (fr)
                stamp_raw_transparent(fr, btn + x[k], 456);
        }
        blit_rows(vram_base() + 1284, 320, band + 32904, 456, 312, 192);
    }
    /* 0x19B07 键循环：先查键，无键才走 ≥2 tick 重合成节拍；←/→ 换选
     * 后回环（0x19DD0/0x19DE2 → 0x19B07），选中态在下一节拍重画。 */
    for (;;) {
        uint32_t now = bios_tick();
        int32_t delta;

        if (kbd_key_avail()) {
            int key = input_wait_key();

            if (key == SCAN_ENTER || key == 57)
                return 1;
            if (key == SCAN_ESC || key == 83)
                return -1;
            if (key == SCAN_LEFT)
                g_menu_choice = 0;
            else if (key == SCAN_RIGHT)
                g_menu_choice = 1;
            continue;
        }
        idle_pump();
        delta = (int32_t)(now - s_last_tick);
        if (delta < 2 && delta >= 0)
            continue;
        blink++;
        if (blink == 4)
            blink = 0;
        s_last_tick = now;
        confirm_band_refresh();
        /* 0x19B91 口型：张(3)单拍 → 闭(0) 重计 rand%30+10。 */
        if (flap_open) {
            if (s_speaker_blk)
                (void)dato_stamp_ex(box, s_speaker_blk, s_speaker_blk_size,
                                    s_portrait_origin, 0,
                                    g_field_map != NULL);
            countdown = (int)(fd2_rand() % 30) + 10;
            flap_open = 0;
        } else if (--countdown == 0) {
            if (s_speaker_blk)
                (void)dato_stamp_ex(box, s_speaker_blk, s_speaker_blk_size,
                                    s_portrait_origin, 3,
                                    g_field_map != NULL);
            flap_open = 1;
        }
        /* 0x19C4B 带重合成：场景=全屏 200 行；战场=框带 86 行。 */
        if (!(g_field_map && (uintptr_t)g_field_map > 1)) {
            for (int i = 0; i < 200; i++)
                memcpy(band + 32900 + 456 * (i - 4), box + 320 * i, 320);
        } else {
            for (int i = 0; i < 86; i++)
                memcpy(band + 32905 + 456 * (i + 108),
                       box + 35845 + 320 * i, 310);
        }
        for (int k = 0; k < 2; k++) {
            int frame = 3 * (int)s_confirm_btn_ids[k];
            const uint8_t *fr;

            if (k == g_menu_choice)
                frame += blink / 2;   /* 0x19D1E：选中钮闪烁帧 */
            fr = blk2_frame_ptr(frame);
            if (fr)
                stamp_raw_transparent(fr, btn + x[k], 456);
        }
        blit_rows(vram_base() + 1284, 320, band + 32904, 456, 312, 192);
    }
}

/* 0x197E5 confirm_close_anim：4 步收敛（−16/+16 → −4/+4，先合成后调
 * 偏移），每步框带回拷 + 常态帧按钮 + blit；末尾框区 86 行自 backup
 * 回拷 VRAM（box+5+320*(i+112) → VRAM+35845+320*i，310 宽）。 */
void confirm_close_anim(void)
{
    static const uint32_t ids[2] = { 16, 17 };   /* 0x51EE5（同对）*/
    uint8_t *box = (s_dialog.active && s_dialog.mode)
                     ? s_dialog.backdrop_box : s_confirm_scratch;
    uint8_t *band;
    uint8_t *btn;
    int x[2] = { -16, 16 };

    if (!box)
        return;
    band = battle_shadow_layer();
    btn = band + 107932;
    if (g_field_map && (uintptr_t)g_field_map > 1) {
        field_phase_tick();
        field_render_viewport(band + 32904, 456);
        field_render_entities(band);
    }
    for (int step = 0; step < 4; step++) {
        for (int i = 0; i < 86; i++)
            memcpy(band + 32905 + 456 * (i + 108),
                   box + 35845 + 320 * i, 310);
        for (int k = 0; k < 2; k++) {
            const uint8_t *fr = blk2_frame_ptr(3 * (int)ids[k]);

            if (fr)
                stamp_raw_transparent(fr, btn + x[k], 456);
        }
        blit_rows(vram_base() + 1284, 320, band + 32904, 456, 312, 192);
        x[0] += 4;
        x[1] -= 4;
    }
    for (int i = 0; i < 86; i++)
        memcpy(vram_base() + 35845 + 320 * i,
               box + 5 + 320 * (i + 112), 310);
    free(s_confirm_scratch);
    s_confirm_scratch = NULL;
}
