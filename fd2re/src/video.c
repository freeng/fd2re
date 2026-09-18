/* video.c — 渲染层（palette/RLE/blit 已按反编译重建；宿主 VGA 扫描器
 * 直接观察索引显存与 DAC，业务层不承担窗口提交）。 */
#include <stdlib.h>
#include <string.h>
#include "video.h"
#include "resource.h"
#include "audio.h"
#include "host.h"
#include "timer.h"

void *g_palette_ptr;

/* vram 按 DOS 完整 64KB VGA 段（0xA0000..0xAFFFF）建模而非可见区
 * 64000：ANI op7/8/9 以 u16 偏移直写屏（实测 anim1 写到 64000——
 * 0x1FD24 菜单前最后一段动画），原版这些写落在段内屏外区无害；
 * 若缓冲仅 64000 则越界写穿 .bss/.data 引用指针槽。可见逻辑仍按
 * FD2_VRAM_SIZE=64000，屏外尾部仅为承接原版合法越界的衬垫。 */
static uint8_t s_vram[0x10000];          /* 宿主侧影子显存（完整 VGA 段） */
static uint8_t s_dac[256][3];            /* 当前 6-bit VGA DAC 状态 */

void dac_write(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (idx < 0 || idx >= 256)
        return;
    s_dac[idx][0] = r;
    s_dac[idx][1] = g;
    s_dac[idx][2] = b;
}

uint8_t *vram_base(void) { return s_vram; }

/* DOS 目标应改回 int386(0x10, regs)；宿主以 SDL2 窗口对应 mode 13h。 */
void vga_set_mode(int mode)
{
    if (mode == 0x13) {
        (void)host_video_init(s_vram, &s_dac[0][0]);
    } else if (mode == 3) {
        host_video_shutdown();
    }
}

/* IDA 0x11D40 —— 已验证 */
void palette_apply_range(int first, int last, int dim)
{
    const uint8_t *pal = (const uint8_t *)g_palette_ptr;
    if (!pal)
        return;
    for (int i = first; i <= last; i++) {
        int r = pal[3 * i + 0] - dim; if (r < 0) r = 0;
        int g = pal[3 * i + 1] - dim; if (g < 0) g = 0;
        int b = pal[3 * i + 2] - dim; if (b < 0) b = 0;
        dac_write(i, (uint8_t)r, (uint8_t)g, (uint8_t)b);  /* 0x3C8/0x3C9 */
    }
}

/* IDA 0x11DF2 —— 对当前调色板区间逐通道加值，DAC 通道上限为 63。 */
void palette_add_range(int first, int last, int add)
{
    const uint8_t *pal = (const uint8_t *)g_palette_ptr;
    if (!pal)
        return;
    for (int i = first; i <= last; i++) {
        int r = pal[3 * i + 0] + add; if (r > 63) r = 63; if (r < 0) r = 0;
        int g = pal[3 * i + 1] + add; if (g > 63) g = 63; if (g < 0) g = 0;
        int b = pal[3 * i + 2] + add; if (b > 63) b = 63; if (b < 0) b = 0;
        dac_write(i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
}

/* IDA 0x2DF01 —— [start..end) 每通道向 (r,g,b) 按 scale/40 混合：
 * out = c + scale*(master_pal[i]-c)/40（读主调色板，非 DAC 回读；
 * scale=40 复原、0 全闪色。大招 32/35 闪白与战场恢复渐变用；
 * 原误名 save_check，2026-09-04 更正）。 */
void palette_mix_range(int start, int end, int scale, int r, int g, int b)
{
    const uint8_t *pal = (const uint8_t *)g_palette_ptr;

    if (!pal)
        return;
    for (int i = start; i < end; i++)
        dac_write(i, (uint8_t)(r + scale * (pal[3 * i + 0] - r) / 40),
                  (uint8_t)(g + scale * (pal[3 * i + 1] - g) / 40),
                  (uint8_t)(b + scale * (pal[3 * i + 2] - b) / 40));
}

/* IDA 0x1F882 —— 已验证（2026-09-07 步数对齐原版 64 步 dim 0..63；
 * 此前 0..256 为多出的 192 步空转 delay，双次调用的路径拖慢约 0.4s） */
void palette_fade_black(void)
{
    for (int i = 0; i < 64; i++) {
        palette_apply_range(0, 255, i);
        delay_ms(2);   /* 原版 j___delay(2) */
    }
}

/* IDA 0x4E98D —— 四种 2-bit 控制游程：00=填充、01=交错/字面量、
 * 10=字面量（或着色填充）、11=跳过。flag==-1 是普通帧；0..255 与
 * >255 分支分别对应平面填充和 8 色索引变换。 */
void rle_decode_frame(const uint8_t *src, int x, int y,
                      uint8_t *dst, int pitch, int flag)
{
    uint16_t w = (uint16_t)(src[0] | (src[1] << 8));
    uint16_t h = (uint16_t)(src[2] | (src[3] << 8));
    const uint8_t *p = src + 4;
    uint8_t *out = dst + (size_t)pitch * y + x;

    for (int row = 0; row < h; row++) {
        int col = 0;
        while (col < w) {
            uint8_t c = *p++;
            int n = (c & 0x3F) + 1;
            int kind = c >> 6;
            if (kind == 3) {                   /* 11: transparent skip */
                col += n;
            } else if (flag == -1) {
                if (kind == 2) {               /* 10: literal bytes */
                    if (col + n > w) n = w - col;
                    memcpy(out + col, p, (size_t)n);
                    p += (c & 0x3F) + 1;
                    col += n;
                } else if (kind == 0) {        /* 00: solid run */
                    uint8_t value = *p++;
                    if (col + n > w) n = w - col;
                    memset(out + col, value, (size_t)n);
                    col += n;
                } else {                       /* 01: interleaved run */
                    uint8_t value = *p++;
                    int limit = n * 2;
                    if (col + limit > w) limit = w - col;
                    for (int k = 0; k < limit; k += 2)
                        out[col + k] = value;
                    col += limit;
                }
            } else if (flag > 255) {
                uint8_t base = (uint8_t)flag;
                uint8_t phase = (uint8_t)(flag >> 8);
                if (kind == 0) {               /* skip source, fill base */
                    p++;
                    if (col + n > w) n = w - col;
                    memset(out + col, base, (size_t)n);
                    col += n;
                } else if (kind == 1) {        /* indexed literal */
                    if (col + n > w) n = w - col;
                    for (int k = 0; k < n; k++)
                        out[col + k] = (uint8_t)(base + ((phase + *p++) & 7));
                    col += n;
                } else {                       /* skip source, fill base */
                    p += (c & 0x3F) + 1;
                    if (col + n > w) n = w - col;
                    memset(out + col, base, (size_t)n);
                    col += n;
                }
            } else {                            /* 0..255: flat-color plane */
                if (kind == 1 || kind == 2) p += n; /* source discarded */
                else if (kind == 0) p++;            /* run value discarded */
                if (col + n > w) n = w - col;
                memset(out + col, (uint8_t)flag, (size_t)n);
                col += n;
            }
        }
        out += pitch;
    }
}

/* IDA 0x4E8D3 —— 2026-09-05 定案：四游程 LUT 重映射解码（sub_4E8D3，
 * fx_scene_play 结果窗格用；头 [u16 w][u16 h] + 游程序列，实心/字面量
 * 字节均经 lut 映射，11 跳过不变）。 */
void rle_decode_frame_lut(const uint8_t *src, int x, int y,
                          uint8_t *dst, int pitch, const uint8_t *lut)
{
    uint16_t w = (uint16_t)(src[0] | (src[1] << 8));
    uint16_t h = (uint16_t)(src[2] | (src[3] << 8));
    const uint8_t *p = src + 4;
    uint8_t *out = dst + (size_t)pitch * y + x;

    for (int row = 0; row < h; row++) {
        int col = 0;
        while (col < w) {
            uint8_t c = *p++;
            int n = (c & 0x3F) + 1;
            int kind = c >> 6;
            if (kind == 3) {                 /* 11: skip */
                col += n;
            } else if (kind == 0) {          /* 00: solid run via lut */
                uint8_t value = lut[*p++];
                if (col + n > w) n = w - col;
                memset(out + col, value, (size_t)n);
                col += n;
            } else if (kind == 2) {          /* 10: literal run via lut */
                if (col + n > w) n = w - col;
                for (int k = 0; k < n; k++)
                    out[col + k] = lut[*p++];
                col += n;
            } else {                         /* 01: interleaved run via lut */
                uint8_t value = lut[*p++];
                int limit = n * 2;
                if (col + limit > w) limit = w - col;
                for (int k = 0; k < limit; k += 2)
                    out[col + k] = value;
                col += limit;
            }
        }
        out += pitch;
    }
}

/* IDA 0x4E29C —— 2026-09-06 定案：24x24 满铺精灵解码（无头，
 * 11 号 op 为固定色 0x49 而非跳过；01 号隔列写、列消耗 2n）。 */
void sprite24_stamp(const uint8_t *src, uint8_t *dst, int pitch)
{
    for (int row = 0; row < 24; row++) {
        uint8_t *out = dst;
        int col = 0;
        while (col < 24) {
            uint8_t c = *src++;
            int n = (c & 0x3F) + 1;
            switch (c >> 6) {
            case 0: {
                uint8_t v = *src++;
                memset(out, v, (size_t)n);
                out += n; col += n;
                break;
            }
            case 1: {
                uint8_t v = *src++;
                for (int k = 0; k < n; k++) {
                    out[1] = v;
                    out += 2;
                }
                col += 2 * n;
                break;
            }
            case 2:
                memcpy(out, src, (size_t)n);
                src += n; out += n; col += n;
                break;
            default:
                memset(out, 0x49, (size_t)n);
                out += n; col += n;
                break;
            }
        }
        dst += pitch;
    }
}

/* IDA 0x4E22A —— 2026-09-08 定案（反汇编逐指令核对）：24x24 透明版，
 * op11 = add edi,ecx 跳过不写；op00 实心（rep stosb）/op10 字面量
 *（rep movsb）/op01 隔点写（写 +1、+3、… 奇数列，消耗 2n）；
 * 行尾 edi += pitch-24。 */
static void stamp24_transparent_rows(const uint8_t *src, uint8_t *dst,
                                    int pitch, int max_rows)
{
    for (int row = 0; row < max_rows; row++) {
        uint8_t *out = dst;
        int col = 0;
        while (col < 24) {
            uint8_t c = *src++;
            int n = (c & 0x3F) + 1;
            switch (c >> 6) {
            case 0: {
                uint8_t v = *src++;
                memset(out, v, (size_t)n);
                out += n; col += n;
                break;
            }
            case 1: {
                uint8_t v = *src++;
                for (int k = 0; k < n; k++) {
                    out[1] = v;
                    out += 2;
                }
                col += 2 * n;
                break;
            }
            case 2:
                memcpy(out, src, (size_t)n);
                src += n; out += n; col += n;
                break;
            default:                  /* 11：透明跳过（原版不写） */
                out += n; col += n;
                break;
            }
        }
        dst += pitch;
    }
}

/* IDA 0x4E1A6 —— 2026-09-13 定案（roster_select_menu 网格定稿）：
 * 24x24 暗色版——与 0x4E22A 同构 4-op RLE，但逐写出像素改
 * (v&7)+24 调入暗色带 24..31（编队网格里"未入选"单位的暗显）；
 * op11 跳过段仍透明。entities.c field_render_entity_offset 的
 * 负标志支为同函数的影子层内联版。 */
static void stamp24_dark_rows(const uint8_t *src, uint8_t *dst,
                             int pitch, int max_rows)
{
    for (int row = 0; row < max_rows; row++) {
        uint8_t *out = dst;
        int col = 0;
        while (col < 24) {
            uint8_t c = *src++;
            int n = (c & 0x3F) + 1;
            switch (c >> 6) {
            case 0: {
                uint8_t v = (uint8_t)((*src++ & 7u) + 24u);
                memset(out, v, (size_t)n);
                out += n; col += n;
                break;
            }
            case 1: {
                uint8_t v = (uint8_t)((*src++ & 7u) + 24u);
                for (int k = 0; k < n; k++) {
                    out[1] = v;
                    out += 2;
                }
                col += 2 * n;
                break;
            }
            case 2: {
                for (int k = 0; k < n; k++)
                    out[k] = (uint8_t)((src[k] & 7u) + 24u);
                src += n; out += n; col += n;
                break;
            }
            default:                  /* 11：透明跳过 */
                out += n; col += n;
                break;
            }
        }
        dst += pitch;
    }
}

void sprite24_stamp_transparent(const uint8_t *src, uint8_t *dst, int pitch)
{
    stamp24_transparent_rows(src, dst, pitch, 24);
}

void sprite24_stamp_transparent_clip(const uint8_t *src, uint8_t *dst,
                                     int pitch, int max_rows)
{
    if (max_rows > 24)
        max_rows = 24;
    stamp24_transparent_rows(src, dst, pitch, max_rows);
}

void sprite24_stamp_dark(const uint8_t *src, uint8_t *dst, int pitch)
{
    stamp24_dark_rows(src, dst, pitch, 24);
}

void sprite24_stamp_dark_clip(const uint8_t *src, uint8_t *dst,
                              int pitch, int max_rows)
{
    if (max_rows > 24)
        max_rows = 24;
    stamp24_dark_rows(src, dst, pitch, max_rows);
}

/* IDA 0x4E127 —— 2026-09-12 定案(fx_effect_apply 剪影闪烁)：24x24
 * 固定色剪影版。op00/op10 的流色字节被消费但以固定色写出(原版
 * 写 a3=pitch 低字节=200)；op01 隔列抖动；op11 跳过。 */

void sprite24_stamp_silhouette(const uint8_t *src, uint8_t *dst, int pitch,
                               uint8_t color)
{
    for (int row = 0; row < 24; row++) {
        uint8_t *out = dst;
        int col = 0;
        while (col < 24) {
            uint8_t c = *src++;
            int n = (c & 0x3F) + 1;
            switch (c >> 6) {
            case 0: {
                src++;
                memset(out, color, (size_t)n);
                out += n; col += n;
                break;
            }
            case 1: {
                src++;
                for (int k = 0; k < n; k++) {
                    out[1] = color;
                    out += 2;
                }
                col += 2 * n;
                break;
            }
            case 2:
                for (int k = 0; k < n; k++)
                    out[k] = color;
                src += n; out += n; col += n;
                break;
            default:
                out += n; col += n;
                break;
            }
        }
        dst += pitch;
    }
}

/* IDA 0x4DF84 —— 2026-09-12 定案(fx_unit_flash 八级闪烁)：24x24
 * 调色坡版，输出色 = base + ((ramp + 原色) & 7)。 */
void sprite24_stamp_flash(const uint8_t *src, uint8_t *dst, int pitch,
                          uint8_t base, int ramp)
{
    for (int row = 0; row < 24; row++) {
        uint8_t *out = dst;
        int col = 0;
        while (col < 24) {
            uint8_t c = *src++;
            int n = (c & 0x3F) + 1;
            switch (c >> 6) {
            case 0: {
                uint8_t v = (uint8_t)(base + ((ramp + *src++) & 7));
                memset(out, v, (size_t)n);
                out += n; col += n;
                break;
            }
            case 1: {
                uint8_t v = (uint8_t)(base + ((ramp + *src++) & 7));
                for (int k = 0; k < n; k++) {
                    out[1] = v;
                    out += 2;
                }
                col += 2 * n;
                break;
            }
            case 2:
                for (int k = 0; k < n; k++)
                    out[k] = (uint8_t)(base + ((ramp + *src++) & 7));
                out += n; col += n;
                break;
            default:
                out += n; col += n;
                break;
            }
        }
        dst += pitch;
    }
}

/* IDA 0x4ED34 —— 2026-09-08 定案（"持续闪烁"根因修复）：RAW 帧族
 * 【透明版】：帧 = [u16 w][u16 h] + 未压缩像素，0=跳过不写；行进 pitch。
 * 菜单图标（blk13 帧 3..10 经 sub_26EDA/sub_27079/sub_26CE4 链）即此族
 * ——此前误用 package_blit_frame（4-op RLE）解原始像素流=噪声。 */
void stamp_raw_transparent(const uint8_t *frame, uint8_t *dst, int pitch)
{
    int w = frame[0] | (frame[1] << 8);
    int h = frame[2] | (frame[3] << 8);
    const uint8_t *p = frame + 4;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++, p++) {
            if (*p)
                dst[col] = *p;
        }
        dst += pitch;
    }
}

/* IDA 0x4ED0B（sub_1685C→此函数）：RAW 帧族【不透明版】：0 也写
 *（rep movsb 整行拷）。道具/效果图标（blk5 帧 15/60/61/64..67 经
 * sub_1685C 于 0x272D0 道具格/价格图标）即此族。 */
void stamp_raw_opaque(const uint8_t *frame, uint8_t *dst, int pitch)
{
    int w = frame[0] | (frame[1] << 8);
    int h = frame[2] | (frame[3] << 8);
    const uint8_t *p = frame + 4;

    for (int row = 0; row < h; row++) {
        memcpy(dst, p, (size_t)w);
        p += w;
        dst += pitch;
    }
}

/* IDA 0x16886 —— 已验证 */
void package_blit_frame(const void *pkg, uint8_t *dst, int pitch, int frame)
{
    const uint8_t *p = (const uint8_t *)pkg;
    uint32_t off = p[6 + 4 * frame]
                 | ((uint32_t)p[6 + 4 * frame + 1] << 8)
                 | ((uint32_t)p[6 + 4 * frame + 2] << 16)
                 | ((uint32_t)p[6 + 4 * frame + 3] << 24);
    rle_decode_frame(p + off, 0, 0, dst, pitch, -1);
}

const uint8_t *package_frame_ptr(const void *pkg, int frame)
{
    const uint8_t *p = (const uint8_t *)pkg;
    uint32_t off = p[6 + 4 * frame]
                 | ((uint32_t)p[6 + 4 * frame + 1] << 8)
                 | ((uint32_t)p[6 + 4 * frame + 2] << 16)
                 | ((uint32_t)p[6 + 4 * frame + 3] << 24);
    return p + off;
}

/* IDA 0x4EBFF stamp_frame_opaque（2026-09-07 定案）：帧 = [u16 w][u16 h]
 * + 字节 RLE（<=0xC0 色号；>0xC0 后跟色号重复 b-0xC0 次），逐像素解码
 * 后【不透明】写入（0 也写）。DAT/DATO/DAT 包共用。 */
void rle_blit_opaque(const uint8_t *frame, uint8_t *dst, int pitch)
{
    int w = frame[0] | (frame[1] << 8);
    int h = frame[2] | (frame[3] << 8);
    const uint8_t *p = frame + 4;
    uint8_t color = 0, repeat = 0;

    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (repeat) {
                repeat--;
            } else {
                uint8_t b = *p++;
                if (b <= 0xC0) {
                    color = b;
                } else {
                    repeat = (uint8_t)(b - 0xC1);
                    color = *p++;
                }
            }
            dst[c] = color;
        }
        dst += pitch;
    }
}

/* IDA 0x4EBBA rle_frame_blit（battle_death_anim 消失特效直调，2026-09-08
 * 落地）：同一字节 RLE，但【透明】写——解码 0 跳过、非零才写目标，行步
 * pitch。与 rle_blit_opaque 同族；blk5 死亡特效帧 68..79 经此格式精确
 * 耗尽验证（帧 73 用量 2485B=帧长、帧 79 用量 333B=帧长；4-op 解码均
 * 越界＝错误格式）。 */
void rle_blit_transparent(const uint8_t *frame, uint8_t *dst, int pitch)
{
    int w = frame[0] | (frame[1] << 8);
    int h = frame[2] | (frame[3] << 8);
    const uint8_t *p = frame + 4;
    uint8_t color = 0, repeat = 0;

    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (repeat) {
                repeat--;
            } else {
                uint8_t b = *p++;
                if (b <= 0xC0) {
                    color = b;
                } else {
                    repeat = (uint8_t)(b - 0xC1);
                    color = *p++;
                }
            }
            if (color)
                dst[c] = color;
        }
        dst += pitch;
    }
}

/* IDA 0x11EB0 —— 已验证（跨距 memmove） */
void blit_rows(uint8_t *dst, int dst_pitch, const uint8_t *src,
               int src_pitch, int row_len, int rows)
{
    for (int i = 0; i < rows; i++) {
        memmove(dst, src, (size_t)row_len);
        dst += dst_pitch;
        src += src_pitch;
    }
}

/* IDA 0x2EB9F —— (w,h) 帧族块（FDOTHER 26 块，2026-09-05 定案）：
 * 块 = [u16 帧数N][u16 N][u32 0][u32 off[N]][帧]；帧 =
 * [u16 x][u16 y][u16 0][u8 mode 0..4（本函数未消费，挂号）][u8 0]
 * [u16 w][u16 h][4-op RLE @+13]。原版将帧头 x/y 作为贴图坐标传入
 * rle_decode_frame（此前实现误传 (0,0)，已修正）。
 * 资产验证：26 块 489 帧全部严格耗尽（tools/fdother_family_validate.py）。 */
void blit_frame_flat(const void *src, int plane, uint8_t *dst, int pitch, int flag)
{
    if (!src || !dst || plane < 0)
        return;
    const uint8_t *pkg = (const uint8_t *)src;
    uint32_t off = (uint32_t)pkg[8 + 4 * plane]
                 | ((uint32_t)pkg[9 + 4 * plane] << 8)
                 | ((uint32_t)pkg[10 + 4 * plane] << 16)
                 | ((uint32_t)pkg[11 + 4 * plane] << 24);
    const uint8_t *frame = pkg + off;
    int x = frame[0] | ((int)frame[1] << 8);
    int y = frame[2] | ((int)frame[3] << 8);
    rle_decode_frame(frame + 9, x, y, dst, pitch, flag);
}

/* IDA 0x1F525 —— 调色板 0→64 级渐显，每级 delay(2) */
void fade_in(void)
{
    for (int dim = 64; dim >= 0; dim--) {
        palette_apply_range(0, 255, dim);
        delay_ms(2);
    }
}

/* IDA 0x22E5C —— 全量对齐（2026-09-07 补 palette_fade_black@0x22E7D
 * 与 fade_in@0x22EC0，此前缺失）。 */
void screen_fade_transition(void)
{
    void *frame;
    music_play(-1, 1);                        /* 淡出 */
    wait_bios_ticks(1);
    palette_fade_black();
    frame = dat_load_block("FDOTHER.DAT", NULL, 79);
    memset(vram_base(), 0, FD2_VRAM_SIZE);    /* 0xA0000 */
    blit_frame_flat(frame, 0, vram_base(), FD2_SCREEN_W, -1);
    fade_in();
    wait_bios_ticks(9);
    blit_frame_flat(frame, 1, vram_base(), FD2_SCREEN_W, -1);
    wait_bios_ticks(36);
    free(frame);                         /* shared tail 0x15E94 */
}
